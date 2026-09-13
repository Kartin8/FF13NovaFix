#include "game/shared/graceful_shutdown.h"

#include "diagnostics/log.h"
#include "game/core/game_profile.h"
#include "game/core/image_view.h"
#include "patch/detour32.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <span>

namespace novafix::game::shutdown {
namespace {

constexpr std::array<std::uint8_t, 14> kFf13RequestExitPrefix{
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x20, 0x89,
    0x4D, 0xE8, 0xB9, 0x13, 0x00, 0x00, 0x00};
constexpr std::array<std::uint8_t, 8> kFf13_2RequestExitPrefix{
    0x8B, 0x01, 0xFF, 0xA0, 0xA0, 0x00, 0x00, 0x00};

template <std::size_t Size>
bool Matches(std::span<const std::byte> code,
             const std::array<std::uint8_t, Size>& expected) {
    const std::span<const std::byte> bytes = std::as_bytes(std::span{expected});
    return code.size() >= bytes.size() &&
           std::equal(bytes.begin(), bytes.end(), code.begin());
}

// XIII Steam 2014 White.GameMain global and virtual RequestExit slot. These
// values are shared by the known original and Nova/community layouts. Every
// pointer and the function bytes are checked at runtime before the call
constexpr std::uintptr_t kFf13GameMainSlotRva = 0x0243E35Cu;
constexpr std::size_t kFf13RequestExitVtableIndex = 35;

// XIII-2's WM_CLOSE path confirms first, then calls RequestExit. Cache the
// application object there so Restart can request the same orderly shutdown
// without reopening the confirmation dialog.
constexpr std::uintptr_t kFf13_2MessageHandlerRva = 0x008046B0u;
constexpr std::uintptr_t kFf13_2RequestExitRva = 0x00803B00u;
constexpr std::array<std::uint8_t, 8> kFf13_2MessageHandlerPrefix{
    0x83, 0xEC, 0x10, 0x53, 0x8B, 0x5C, 0x24, 0x18};

using Ff13_2MessageHandler = int(__thiscall*)(void*, int);
using Ff13_2RequestExit = int(__thiscall*)(void*);

std::atomic<void*> g_ff13_2Application{};

INIT_ONCE g_ff13_2InitializeOnce = INIT_ONCE_STATIC_INIT;
patch::Detour32 g_ff13_2MessageDetour;
Ff13_2MessageHandler g_ff13_2OriginalMessageHandler = nullptr;

int __fastcall Ff13_2MessageHandlerHook(void* application, void*, int message) {
    g_ff13_2Application.store(application, std::memory_order_release);
    Ff13_2MessageHandler original = g_ff13_2OriginalMessageHandler;
    return original ? original(application, message) : 0;
}

BOOL CALLBACK InitializeFf13_2Shutdown(PINIT_ONCE, PVOID, PVOID*) {
    if (CurrentProfile().title != Title::FinalFantasyXIII2 ||
        !Supports(Capability::GracefulShutdown)) return TRUE;
    const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
    if (!image) return TRUE;
    const auto handlerRange = image->At(
        kFf13_2MessageHandlerRva, kFf13_2MessageHandlerPrefix.size());
    if (!handlerRange ||
        std::memcmp(handlerRange->data(), kFf13_2MessageHandlerPrefix.data(),
                    kFf13_2MessageHandlerPrefix.size()) != 0) {
        LogWarning("XIII-2 graceful shutdown disabled: message-handler contract mismatch");
        return TRUE;
    }
    const std::byte* handler = handlerRange->data();
    if (!g_ff13_2MessageDetour.Install(
            "ff13-2-graceful-shutdown", const_cast<std::byte*>(handler),
            reinterpret_cast<void*>(&Ff13_2MessageHandlerHook),
            std::as_bytes(std::span{kFf13_2MessageHandlerPrefix}),
            reinterpret_cast<void**>(&g_ff13_2OriginalMessageHandler))) {
        LogWarning("XIII-2 graceful shutdown disabled: message-handler detour failed");
        return TRUE;
    }
    return TRUE;
}

bool IsReadable(const void* address, std::size_t size) {
    if (!address || size == 0) return false;

    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQuery(address, &memory, sizeof(memory)) || memory.State != MEM_COMMIT) {
        return false;
    }
    constexpr DWORD kUnreadable = PAGE_NOACCESS | PAGE_GUARD;
    if ((memory.Protect & kUnreadable) != 0) return false;

    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    const auto regionBegin = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    const auto regionEnd = regionBegin + memory.RegionSize;
    return begin >= regionBegin && begin <= regionEnd && size <= regionEnd - begin;
}

} // namespace

void Pump() {
    InitOnceExecuteOnce(&g_ff13_2InitializeOnce, &InitializeFf13_2Shutdown,
                        nullptr, nullptr);
}

RequestResult RequestApplicationShutdown() {
    const GameProfile& profile = CurrentProfile();
    if (!Supports(Capability::GracefulShutdown)) {
        return RequestResult::Unsupported;
    }

    if (profile.title == Title::FinalFantasyXIII2) {
        Pump();
        void* application = g_ff13_2Application.load(std::memory_order_acquire);
        if (!application || !IsReadable(application, sizeof(void*))) {
            return RequestResult::NotReady;
        }
        const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
        if (!image) return RequestResult::NotReady;
        const auto requestRange = image->At(
            kFf13_2RequestExitRva,
            kFf13_2RequestExitPrefix.size());
        if (!requestRange ||
            !Matches(*requestRange, kFf13_2RequestExitPrefix)) {
            return RequestResult::ContractMismatch;
        }
        const std::byte* request = requestRange->data();
        reinterpret_cast<Ff13_2RequestExit>(const_cast<std::byte*>(request))(application);
        Log("XIII-2 graceful shutdown requested through the native RequestExit path");
        return RequestResult::Requested;
    }

    if (profile.title != Title::FinalFantasyXIII) return RequestResult::Unsupported;

    HMODULE executable = GetModuleHandleW(nullptr);
    const auto image = ImageView::FromModule(executable);
    if (!executable || !image) return RequestResult::NotReady;

    const auto appSlotRange = image->At(kFf13GameMainSlotRva, sizeof(void*));
    if (!appSlotRange) return RequestResult::ContractMismatch;
    const auto* appSlot = reinterpret_cast<void* const*>(appSlotRange->data());
    if (!IsReadable(appSlot, sizeof(*appSlot))) {
        return RequestResult::ContractMismatch;
    }

    void* app = *appSlot;
    if (!IsReadable(app, sizeof(void*))) return RequestResult::NotReady;

    void** vtable = *reinterpret_cast<void***>(app);
    const std::size_t vtableBytes = (kFf13RequestExitVtableIndex + 1) * sizeof(void*);
    if (!image->Contains(vtable, vtableBytes) || !IsReadable(vtable, vtableBytes)) {
        return RequestResult::ContractMismatch;
    }

    void* requestExit = vtable[kFf13RequestExitVtableIndex];
    if (!image->Contains(
            requestExit, kFf13RequestExitPrefix.size()) ||
        !IsReadable(requestExit, kFf13RequestExitPrefix.size())) {
        return RequestResult::ContractMismatch;
    }
    const auto code = std::span(
        reinterpret_cast<const std::byte*>(requestExit),
        kFf13RequestExitPrefix.size());
    if (!Matches(code, kFf13RequestExitPrefix)) {
        return RequestResult::ContractMismatch;
    }

    using RequestExit = int(__thiscall*)(void*);
    reinterpret_cast<RequestExit>(requestExit)(app);
    Log("XIII graceful shutdown requested through White.GameMain::RequestExit");
    return RequestResult::Requested;
}

const char* RequestResultName(RequestResult result) {
    switch (result) {
    case RequestResult::Requested: return "requested";
    case RequestResult::Unsupported: return "unsupported";
    case RequestResult::NotReady: return "not-ready";
    case RequestResult::ContractMismatch: return "contract-mismatch";
    }
    return "unknown";
}

} // namespace novafix::game::shutdown
