#include "patch/detour32.h"

#include "diagnostics/log.h"
#include "patch/memory_access.h"

#include <windows.h>
#include <MinHook.h>

#include <cstring>
#include <vector>

namespace novafix::patch {
namespace {

INIT_ONCE g_minHookOnce = INIT_ONCE_STATIC_INIT;
MH_STATUS g_minHookStatus = MH_UNKNOWN;

BOOL CALLBACK InitializeMinHook(PINIT_ONCE, PVOID, PVOID*) {
    g_minHookStatus = MH_Initialize();
    if (g_minHookStatus == MH_ERROR_ALREADY_INITIALIZED) {
        g_minHookStatus = MH_OK;
    }
    return TRUE;
}

bool EnsureMinHook() {
    InitOnceExecuteOnce(&g_minHookOnce, &InitializeMinHook, nullptr, nullptr);
    return g_minHookStatus == MH_OK;
}

} // namespace

bool Detour32::Install(std::string_view featureName, void* target, void* hook,
                       std::span<const std::byte> expectedPrefix, void** originalOut) {
#if !defined(_M_IX86) && !defined(__i386__)
    (void)featureName;
    (void)target;
    (void)hook;
    (void)expectedPrefix;
    (void)originalOut;
    return false;
#else
    if (originalOut) *originalOut = nullptr;
    if (IsInstalled() || !target || !hook || expectedPrefix.size() < 5) return false;
    if (!memory::IsReadable(target, expectedPrefix.size()) ||
        !memory::IsExecutable(target, expectedPrefix.size()) ||
        !memory::IsExecutable(hook, 1u)) {
        Log("Detour validation failed: inaccessible range feature=%.*s target=%p hook=%p",
            static_cast<int>(featureName.size()), featureName.data(), target,
            hook);
        return false;
    }
    if (std::memcmp(target, expectedPrefix.data(), expectedPrefix.size()) != 0) {
        Log("Detour validation failed: prefix mismatch feature=%.*s target=%p",
            static_cast<int>(featureName.size()), featureName.data(), target);
        return false;
    }

    const registry::SiteDefinition site{
        static_cast<std::byte*>(target), expectedPrefix, expectedPrefix,
        registry::Kind::Detour};
    const registry::ReserveResult reservation =
        registry::Reserve(featureName, std::span(&site, 1));
    claim_ = reservation.claim;
    if (reservation.status != registry::ReserveStatus::Reserved) {
        Log("Detour reservation failed: feature=%.*s target=%p owner=%s",
            static_cast<int>(featureName.size()), featureName.data(), target,
            reservation.conflictOwner.c_str());
        return false;
    }

    if (!EnsureMinHook()) {
        registry::MarkFailure(claim_, registry::State::WriteFailed,
                              "MinHook initialization failed");
        Log("MinHook initialization failed: feature=%.*s status=%s",
            static_cast<int>(featureName.size()), featureName.data(),
            MH_StatusToString(g_minHookStatus));
        return false;
    }

    MH_STATUS status = MH_CreateHook(target, hook, &trampoline_);
    if (status != MH_OK) {
        registry::MarkFailure(
            claim_,
            status == MH_ERROR_UNSUPPORTED_FUNCTION
                ? registry::State::ContractMismatch
                : registry::State::WriteFailed,
            MH_StatusToString(status));
        Log("MinHook create failed: feature=%.*s target=%p status=%s",
            static_cast<int>(featureName.size()), featureName.data(), target,
            MH_StatusToString(status));
        trampoline_ = nullptr;
        return false;
    }

    if (originalOut) *originalOut = trampoline_;
    status = MH_EnableHook(target);
    if (status != MH_OK && status != MH_ERROR_ENABLED) {
        if (originalOut) *originalOut = nullptr;
        MH_RemoveHook(target);
        registry::MarkFailure(claim_, registry::State::WriteFailed,
                              MH_StatusToString(status));
        Log("MinHook enable failed: feature=%.*s target=%p status=%s",
            static_cast<int>(featureName.size()), featureName.data(), target,
            MH_StatusToString(status));
        trampoline_ = nullptr;
        return false;
    }

    expectedPrefix_.assign(expectedPrefix.begin(), expectedPrefix.end());
    installedPrefix_.resize(expectedPrefix.size());
    std::memcpy(installedPrefix_.data(), target, installedPrefix_.size());
    if (installedPrefix_ == expectedPrefix_ ||
        !registry::CommitExternalPatch(claim_, installedPrefix_)) {
        target_ = target;
        state_ = State::Enabled;
        bool cleaned = Revert();
        if (!cleaned && state_ == State::Disabled) {
            cleaned = Revert();
        }
        registry::MarkFailure(claim_, registry::State::WriteFailed,
                              "installed bytes could not be committed");
        if (!cleaned && state_ == State::Enabled) {
            LogError("MinHook rollback incomplete: feature=%.*s target=%p detour=active trampoline=retained",
                     static_cast<int>(featureName.size()),
                     featureName.data(), target);
            return true;
        }
        if (originalOut) *originalOut = nullptr;
        Log("MinHook commit failed: feature=%.*s target=%p",
            static_cast<int>(featureName.size()), featureName.data(), target);
        return false;
    }
    target_ = target;
    state_ = State::Enabled;
    return true;
#endif
}

bool Detour32::Revert() {
    if (state_ == State::Inactive) return true;
    if (!target_ || expectedPrefix_.empty() || installedPrefix_.empty() ||
        !memory::IsReadable(target_, expectedPrefix_.size())) {
        registry::MarkFailure(claim_, registry::State::IntegrityLost,
                              "revert refused because target is unavailable");
        return false;
    }

    if (state_ == State::Enabled) {
        const bool stillEnabled =
            std::memcmp(target_, installedPrefix_.data(),
                        installedPrefix_.size()) == 0;
        const bool alreadyRestored =
            std::memcmp(target_, expectedPrefix_.data(),
                        expectedPrefix_.size()) == 0;
        if (!stillEnabled && !alreadyRestored) {
            registry::MarkFailure(claim_, registry::State::IntegrityLost,
                                  "revert refused after external modification");
            return false;
        }

        if (stillEnabled) {
            const MH_STATUS disableStatus = MH_DisableHook(target_);
            if (disableStatus != MH_OK &&
                disableStatus != MH_ERROR_DISABLED) {
                registry::MarkFailure(claim_, registry::State::WriteFailed,
                                      MH_StatusToString(disableStatus));
                return false;
            }
        }
        state_ = State::Disabled;
    }

    if (std::memcmp(target_, expectedPrefix_.data(),
                    expectedPrefix_.size()) != 0) {
        registry::MarkFailure(claim_, registry::State::WriteFailed,
                              "MinHook did not restore the validated prefix");
        return false;
    }

    const MH_STATUS status = MH_RemoveHook(target_);
    if (status != MH_OK && status != MH_ERROR_NOT_CREATED) {
        registry::MarkFailure(claim_, registry::State::WriteFailed,
                              MH_StatusToString(status));
        return false;
    }
    registry::MarkReverted(claim_);
    state_ = State::Inactive;
    target_ = nullptr;
    trampoline_ = nullptr;
    expectedPrefix_.clear();
    installedPrefix_.clear();
    return true;
}

bool Detour32::IsInstalled() const {
    return state_ != State::Inactive && target_ && trampoline_;
}

} // namespace novafix::patch
