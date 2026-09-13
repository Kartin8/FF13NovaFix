#include "game/titles/lightning_returns/naming/chocobo_name.h"

#include "diagnostics/hook_profiler.h"
#include "diagnostics/log.h"
#include "game/core/game_profile.h"
#include "game/core/image_view.h"
#include "game/titles/lightning_returns/ui/text_editor/editor.h"
#include "game/titles/lightning_returns/ui/text_editor/native_api.h"
#include "patch/detour32.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace novafix::game::lr_chocobo_rename {
namespace {

// Restore the missing interaction row/branch in memory. PC retains the rename
// menu, save storage and live refresh; game archives stay untouched
constexpr std::uint32_t kShowChoiceWindowVmRva = 0x005DA4C0u;
constexpr std::uint32_t kGetSelectedItemVmRva = 0x005DAD20u;
constexpr std::uint32_t kIsWindowClosedVmRva = 0x005DB940u;
constexpr std::uint32_t kOpenChocoboRenameRva = 0x00398CD0u;
constexpr std::uint32_t kGetChocoboRenameResultRva = 0x00398D00u;
constexpr std::uint32_t kSetUpGameNameRva = 0x0018EA00u;

constexpr std::array<std::uint8_t, 6> kShowChoiceWindowVmPrefix{
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x48};
constexpr std::array<std::uint8_t, 6> kGetSelectedItemVmPrefix{
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14};
constexpr std::array<std::uint8_t, 6> kIsWindowClosedVmPrefix{
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14};
constexpr std::array<std::uint8_t, 2> kOpenChocoboRenameStablePrefix{
    0x6A, 0x01};
constexpr std::array<std::uint8_t, 3> kGetChocoboRenameResultStablePrefix{
    0x56, 0x33, 0xF6};
constexpr std::array<std::uint8_t, 6> kSetUpGameNamePrefix{
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x48};

constexpr std::size_t kVmResumeStateOffset = 48u;
constexpr std::size_t kVmStackOffset = 2392u;
constexpr unsigned kVmArrayLengthShift = 18u;
constexpr std::uint32_t kVmArrayTypeMask =
    (std::uint32_t{1} << kVmArrayLengthShift) - 1u;
constexpr ULONGLONG kChoiceLifetimeMs = 300000u;

constexpr std::string_view kChoiceWindowName = "$tw020qad030b8c";
constexpr char kRenameChoice[] = "$tw020qad330b8c";

struct VmCell {
    std::uint32_t tag{};
    std::uint32_t value{};
};
static_assert(sizeof(VmCell) == 8u);

struct InlineGameString {
    char value[16]{};
};
static_assert(sizeof(InlineGameString) == 16u);

enum class RenameSession : unsigned {
    Idle,
    WaitingForChoiceClose,
    NativeMenuOpen,
};

using VmHandlerFn = int (__cdecl*)(void*);
using OpenChocoboRenameFn = void (__cdecl*)();
using GetChocoboRenameResultFn = int (__cdecl*)();
using SetUpGameNameFn = int (__cdecl*)(InlineGameString);

INIT_ONCE g_initializeOnce = INIT_ONCE_STATIC_INIT;
patch::Detour32 g_showChoiceWindowDetour;
patch::Detour32 g_getSelectedItemDetour;
patch::Detour32 g_isWindowClosedDetour;
VmHandlerFn g_originalShowChoiceWindow{};
VmHandlerFn g_originalGetSelectedItem{};
VmHandlerFn g_originalIsWindowClosed{};
OpenChocoboRenameFn g_openChocoboRename{};
GetChocoboRenameResultFn g_getChocoboRenameResult{};
SetUpGameNameFn g_setUpGameName{};

std::atomic_bool g_choiceActive{};
std::atomic_int g_renameChoiceIndex{-1};
std::atomic_int g_originalCancelIndex{-1};
std::atomic_int g_injectedCancelIndex{-1};
std::atomic_int g_choiceWindowHandle{-1};
std::atomic<ULONGLONG> g_choiceDeadline{};
std::atomic<RenameSession> g_session{RenameSession::Idle};
std::atomic<std::uintptr_t> g_waitingVm{};

template <std::size_t Size>
bool MatchesAt(const ImageView& image, std::uint32_t rva,
               const std::array<std::uint8_t, Size>& expected) {
    const auto bytes = image.At(rva, expected.size());
    return bytes && std::memcmp(bytes->data(), expected.data(),
                                expected.size()) == 0;
}

// VM pointers are validated by the handler hooks; avoid VirtualQuery here
VmCell* VmStack(void* vm) {
    if (!vm) return nullptr;
    VmCell* stack{};
    std::memcpy(&stack, static_cast<std::byte*>(vm) + kVmStackOffset,
                sizeof(stack));
    return stack;
}

bool TextEquals(const char* text, std::string_view expected) {
    return text && std::memcmp(
                       text, expected.data(), expected.size()) == 0 &&
        text[expected.size()] == '\0';
}

const char* CellText(const VmCell& cell) {
    return reinterpret_cast<const char*>(
        static_cast<std::uintptr_t>(cell.value));
}

const char* const* CellTextArray(const VmCell& cell) {
    return reinterpret_cast<const char* const*>(
        static_cast<std::uintptr_t>(cell.value));
}

bool IsInitialVmCall(void* vm) {
    if (!vm) return false;
    std::uint32_t state{};
    std::memcpy(&state, static_cast<std::byte*>(vm) +
                            kVmResumeStateOffset,
                sizeof(state));
    return state == 0u;
}

InlineGameString InlineName(std::string_view value) {
    InlineGameString result{};
    const std::size_t length =
        (std::min)(value.size(), sizeof(result.value) - 1u);
    std::memcpy(result.value, value.data(), length);
    return result;
}

void SetVmResult(void* vm, int value) {
    VmCell* const output = VmStack(vm);
    if (output) output[0].value = static_cast<std::uint32_t>(value);
}

void RefreshFieldNames() {
    if (!g_setUpGameName) return;
    g_setUpGameName(InlineName("en_npc_1283"));
    g_setUpGameName(InlineName("en_npc_1998"));
    g_setUpGameName(InlineName("en_npc_1999"));
}

int __cdecl ShowChoiceWindowHook(void* vm) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        timing, "hook.lr.chocobo-choice-create-total");
    const VmHandlerFn original = g_originalShowChoiceWindow;
    if (!original) return 0;

    VmCell* const stack = VmStack(vm);
    if (!IsInitialVmCall(vm) || !stack ||
        !TextEquals(CellText(stack[10]), kChoiceWindowName) ||
        g_session.load(std::memory_order_acquire) != RenameSession::Idle) {
        return original(vm);
    }

    const int count = static_cast<int>(stack[1].value);
    const int cancelIndex = static_cast<int>(stack[2].value);
    const int encodedCount = static_cast<int>(
        stack[6].tag >> kVmArrayLengthShift);
    const char* const* const items = CellTextArray(stack[6]);
    if (!items || (count != 2 && count != 3) ||
        count != encodedCount || cancelIndex != count - 1) {
        return original(vm);
    }

    std::array<const char*, 4> restoredItems{};
    const int renameIndex = count - 1;
    for (int index = 0; index < renameIndex; ++index) {
        restoredItems[static_cast<std::size_t>(index)] = items[index];
    }
    restoredItems[static_cast<std::size_t>(renameIndex)] = kRenameChoice;
    restoredItems[static_cast<std::size_t>(renameIndex + 1)] =
        items[count - 1];
    const int restoredCount = count + 1;

    const VmCell savedCount = stack[1];
    const VmCell savedCancel = stack[2];
    const VmCell savedArray = stack[6];
    stack[1].value = static_cast<std::uint32_t>(restoredCount);
    stack[2].value = static_cast<std::uint32_t>(restoredCount - 1);
    stack[6].tag = (stack[6].tag & kVmArrayTypeMask) |
        (static_cast<std::uint32_t>(restoredCount) <<
         kVmArrayLengthShift);
    stack[6].value = static_cast<std::uint32_t>(
        reinterpret_cast<std::uintptr_t>(restoredItems.data()));

    int result{};
    {
        NOVAFIX_PROFILE_NAMED_SCOPE(
            originalTiming,
            "hook.lr.chocobo-choice-create-original");
        result = original(vm);
    }

    stack[1] = savedCount;
    stack[2] = savedCancel;
    stack[6] = savedArray;

    VmCell* const output = VmStack(vm);
    const int windowHandle = output
        ? static_cast<int>(output[0].value) : -1;
    g_renameChoiceIndex.store(renameIndex, std::memory_order_relaxed);
    g_originalCancelIndex.store(cancelIndex, std::memory_order_relaxed);
    g_injectedCancelIndex.store(
        restoredCount - 1, std::memory_order_relaxed);
    g_choiceWindowHandle.store(windowHandle, std::memory_order_relaxed);
    g_choiceDeadline.store(
        GetTickCount64() + kChoiceLifetimeMs,
        std::memory_order_relaxed);
    g_choiceActive.store(true, std::memory_order_release);
    return result;
}

int __cdecl GetSelectedItemHook(void* vm) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        timing, "hook.lr.chocobo-choice-result-total");
    const VmHandlerFn original = g_originalGetSelectedItem;
    if (!original) return -1;

    VmCell* const input = VmStack(vm);
    const bool chocoboChoice = input &&
        g_choiceActive.load(std::memory_order_acquire) &&
        TextEquals(CellText(input[0]), kChoiceWindowName);

    int result{};
    {
        NOVAFIX_PROFILE_NAMED_SCOPE(
            originalTiming,
            "hook.lr.chocobo-choice-result-original");
        result = original(vm);
    }
    if (!chocoboChoice) return result;

    g_choiceActive.store(false, std::memory_order_release);
    VmCell* const output = VmStack(vm);
    if (!output) return result;

    const int selected = static_cast<int>(output[0].value);
    const int renameIndex =
        g_renameChoiceIndex.load(std::memory_order_relaxed);
    const int originalCancel =
        g_originalCancelIndex.load(std::memory_order_relaxed);
    const int injectedCancel =
        g_injectedCancelIndex.load(std::memory_order_relaxed);
    int mapped = selected;
    if (selected == renameIndex) {
        mapped = originalCancel;
        g_session.store(RenameSession::WaitingForChoiceClose,
                        std::memory_order_release);
    } else if (selected == injectedCancel) {
        mapped = originalCancel;
    }
    output[0].value = static_cast<std::uint32_t>(mapped);
    return mapped;
}

int __cdecl IsWindowClosedHook(void* vm) {
    const VmHandlerFn original = g_originalIsWindowClosed;
    if (!original) return 1;

    VmCell* const input = VmStack(vm);
    const int handle = input ? static_cast<int>(input[1].value) : -1;
    const bool byName = input && input[2].value != 0u;
    int result = original(vm);

    RenameSession session = g_session.load(std::memory_order_acquire);
    if (session == RenameSession::WaitingForChoiceClose) {
        if (byName ||
            handle !=
                g_choiceWindowHandle.load(std::memory_order_relaxed) ||
            result == 0) {
            return result;
        }
        if (!g_openChocoboRename || !g_getChocoboRenameResult ||
            !lr_text_editor::Available() ||
            lr_text_editor::IsOpen() ||
            lr_text_editor::ResultReady()) {
            g_session.store(RenameSession::Idle,
                            std::memory_order_release);
            LogWarning("LR Chocobo rename canceled: native editor lifecycle unavailable when Rename closed");
            return result;
        }

        {
            // Redirect only the OSK backend; keep ChocoboRenameMenu's native lifecycle
            lr_text_editor::BackendRedirectScope redirect;
            g_openChocoboRename();
        }
        if (!lr_text_editor::IsOpen() ||
            g_getChocoboRenameResult() >= 0) {
            g_session.store(RenameSession::Idle,
                            std::memory_order_release);
            LogWarning("LR Chocobo rename canceled: native ChocoboRenameMenu did not enter its pending state");
            return result;
        }
        g_waitingVm.store(reinterpret_cast<std::uintptr_t>(vm),
                          std::memory_order_release);
        g_session.store(RenameSession::NativeMenuOpen,
                        std::memory_order_release);
        SetVmResult(vm, 0);
        return 0;
    }

    if (session != RenameSession::NativeMenuOpen ||
        reinterpret_cast<std::uintptr_t>(vm) !=
            g_waitingVm.load(std::memory_order_acquire)) {
        return result;
    }

    const int renameResult = g_getChocoboRenameResult
        ? g_getChocoboRenameResult() : 0;
    if (renameResult < 0) {
        SetVmResult(vm, 0);
        return 0;
    }

    if (renameResult > 0) RefreshFieldNames();
    g_waitingVm.store(0u, std::memory_order_release);
    g_session.store(RenameSession::Idle, std::memory_order_release);
    SetVmResult(vm, 1);
    return 1;
}

BOOL CALLBACK InitializeFeature(PINIT_ONCE, PVOID, PVOID*) {
    if (CurrentProfile().title != Title::LightningReturns ||
        !Supports(Capability::ChocoboRename)) {
        return TRUE;
    }
#if !defined(_M_IX86) && !defined(__i386__)
    LogWarning("LR Chocobo rename disabled: x86 ABI required");
    return TRUE;
#else
    const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
    if (!image || !lr_text_editor::Available()) {
        LogWarning("LR Chocobo rename disabled: native text editor unavailable");
        return TRUE;
    }
    const bool contractMatches =
        MatchesAt(*image, kShowChoiceWindowVmRva,
                  kShowChoiceWindowVmPrefix) &&
        MatchesAt(*image, kGetSelectedItemVmRva,
                  kGetSelectedItemVmPrefix) &&
        MatchesAt(*image, kIsWindowClosedVmRva,
                  kIsWindowClosedVmPrefix) &&
        // Skip the relocated GuiManager address and validate the stable bytes after it
        MatchesAt(*image, kOpenChocoboRenameRva + 6u,
                  kOpenChocoboRenameStablePrefix) &&
        MatchesAt(*image, kGetChocoboRenameResultRva + 6u,
                  kGetChocoboRenameResultStablePrefix) &&
        MatchesAt(*image, kSetUpGameNameRva,
                  kSetUpGameNamePrefix);
    if (!contractMatches) {
        LogWarning("LR Chocobo rename disabled: native contract mismatch");
        return TRUE;
    }

    const auto showChoice = image->At(kShowChoiceWindowVmRva, 1u);
    const auto getSelected = image->At(kGetSelectedItemVmRva, 1u);
    const auto isWindowClosed = image->At(kIsWindowClosedVmRva, 1u);
    const auto openRename = image->At(kOpenChocoboRenameRva, 1u);
    const auto getRenameResult = image->At(
        kGetChocoboRenameResultRva, 1u);
    const auto setUpName = image->At(kSetUpGameNameRva, 1u);
    if (!showChoice || !getSelected || !isWindowClosed ||
        !openRename || !getRenameResult || !setUpName) {
        LogWarning("LR Chocobo rename disabled: native address resolution failed");
        return TRUE;
    }

    g_openChocoboRename = reinterpret_cast<OpenChocoboRenameFn>(
        const_cast<std::byte*>(openRename->data()));
    g_getChocoboRenameResult =
        reinterpret_cast<GetChocoboRenameResultFn>(
            const_cast<std::byte*>(getRenameResult->data()));
    g_setUpGameName = reinterpret_cast<SetUpGameNameFn>(
        const_cast<std::byte*>(setUpName->data()));

    const bool showInstalled = g_showChoiceWindowDetour.Install(
        "lr-chocobo-choice-create",
        const_cast<std::byte*>(showChoice->data()),
        reinterpret_cast<void*>(&ShowChoiceWindowHook),
        std::as_bytes(std::span{kShowChoiceWindowVmPrefix}),
        reinterpret_cast<void**>(&g_originalShowChoiceWindow));
    const bool selectedInstalled = showInstalled &&
        g_getSelectedItemDetour.Install(
            "lr-chocobo-choice-result",
            const_cast<std::byte*>(getSelected->data()),
            reinterpret_cast<void*>(&GetSelectedItemHook),
            std::as_bytes(std::span{kGetSelectedItemVmPrefix}),
            reinterpret_cast<void**>(&g_originalGetSelectedItem));
    const bool closedInstalled = selectedInstalled &&
        g_isWindowClosedDetour.Install(
            "lr-chocobo-conversation-wait",
            const_cast<std::byte*>(isWindowClosed->data()),
            reinterpret_cast<void*>(&IsWindowClosedHook),
            std::as_bytes(std::span{kIsWindowClosedVmPrefix}),
            reinterpret_cast<void**>(&g_originalIsWindowClosed));
    if (!closedInstalled) {
        if (selectedInstalled) {
            g_getSelectedItemDetour.RevertAndClear(
                g_originalGetSelectedItem);
        }
        if (showInstalled) {
            g_showChoiceWindowDetour.RevertAndClear(
                g_originalShowChoiceWindow);
        }
        g_openChocoboRename = nullptr;
        g_getChocoboRenameResult = nullptr;
        g_setUpGameName = nullptr;
        LogWarning("LR Chocobo rename disabled: detour installation failed");
        return TRUE;
    }

    return TRUE;
#endif
}

} // namespace

void Initialize() {
    InitOnceExecuteOnce(
        &g_initializeOnce, &InitializeFeature, nullptr, nullptr);
}

void Pump() {
    if (g_choiceActive.load(std::memory_order_acquire) &&
        g_session.load(std::memory_order_acquire) == RenameSession::Idle &&
        GetTickCount64() >=
            g_choiceDeadline.load(std::memory_order_relaxed)) {
        g_choiceActive.store(false, std::memory_order_release);
    }
}

} // namespace novafix::game::lr_chocobo_rename
