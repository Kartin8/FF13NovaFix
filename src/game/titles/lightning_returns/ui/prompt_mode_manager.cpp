#include "game/titles/lightning_returns/ui/prompt_mode_manager.h"

#include "patch/iat_hook.h"
#include "diagnostics/hook_profiler.h"
#include "diagnostics/log.h"
#include "compat/addons/native_notifications.h"
#include "game/core/game_profile.h"
#include "game/core/image_view.h"
#include "game/titles/lightning_returns/ui/button_guides/registry.h"
#include "game/titles/lightning_returns/ui/text_editor/editor.h"
#include "input/activity.h"
#include "input/prompt_policy.h"
#include "overlay/overlay.h"
#include "patch/detour32.h"
#include "patch/memory_access.h"
#include "settings/input_settings.h"

#include <windows.h>
#include <xinput.h>

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace novafix::game::lr_prompts {
namespace {

constexpr std::uint32_t kPromptRefreshRva = 0x00086AD0u;
constexpr std::uint32_t kPlatformQueryRva = 0x005243A0u;
constexpr std::uint32_t kNativeConfirmLayoutRva = 0x020D9519u;
constexpr std::uint32_t kPlatformStateGlobalRva = 0x04CF69B0u;
constexpr int kPcPromptCapability = 26;
constexpr WORD kXInputGetStateOrdinal = 2;
constexpr std::size_t kRefreshInitializedOffset = 0x68u;

constexpr std::array<std::byte, 9> kPromptRefreshPrefixTemplate{
    std::byte{0x80}, std::byte{0x79}, std::byte{0x68}, std::byte{0x00},
    std::byte{0xA0}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x00},
};
constexpr std::array<std::byte, 12> kPlatformQueryPrefixTemplate{
    std::byte{0x55}, std::byte{0x8B}, std::byte{0xEC}, std::byte{0x8B},
    std::byte{0x45}, std::byte{0x08}, std::byte{0x8B}, std::byte{0x0D},
    std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
};

using NativeMode = input::NativePromptMode;
using PromptRefreshFn = char (__thiscall*)(void*);
using PlatformQueryFn = int (__cdecl*)(int);
using XInputGetStateFn = DWORD (WINAPI*)(DWORD, XINPUT_STATE*);

struct XInputSample {
    XINPUT_STATE state{};
    bool initialized{};
};

INIT_ONCE g_initializeOnce = INIT_ONCE_STATIC_INIT;
patch::Detour32 g_promptRefreshDetour;
patch::Detour32 g_platformQueryDetour;
PromptRefreshFn g_originalPromptRefresh{};
PlatformQueryFn g_originalPlatformQuery{};
XInputGetStateFn g_originalXInputGetState{};
const std::byte* g_nativeConfirmLayout{};
std::atomic_bool g_overrideActive{false};
std::atomic<NativeMode> g_selectedMode{NativeMode::Unknown};
std::uint64_t g_refreshGeneration{};
std::uint64_t g_pendingGuideRefreshGeneration{};
SRWLOCK g_xinputLock = SRWLOCK_INIT;
std::array<XInputSample, XUSER_MAX_COUNT> g_xinputSamples{};

const char* ModeName(NativeMode mode) {
    if (mode == NativeMode::Keyboard) return "keyboard";
    if (mode == NativeMode::Controller) return "controller";
    return "unknown";
}

int __cdecl PlatformQueryHook(int capability) {
    thread_local std::uint32_t totalSampleCounter{};
    hook_profiler::Scope totalTiming(
        hook_profiler::Point::LrPlatformQueryOwn,
        (totalSampleCounter++ & 63u) == 0u, 64u);
    if (capability == kPcPromptCapability &&
        g_overrideActive.load(std::memory_order_acquire)) {
        return g_selectedMode.load(std::memory_order_acquire) ==
                       NativeMode::Keyboard
                   ? 1
                   : 0;
    }

    PlatformQueryFn original = g_originalPlatformQuery;
    if (!original) return 0;
    thread_local std::uint32_t originalSampleCounter{};
    hook_profiler::Scope originalTiming(
        hook_profiler::Point::LrPlatformQueryOriginal,
        (originalSampleCounter++ & 63u) == 0u, 64u);
    return original(capability);
}

bool HasXInputActivity(const XINPUT_GAMEPAD& current,
                       const XINPUT_GAMEPAD& previous) {
    if ((current.wButtons & ~previous.wButtons) != 0) return true;

    constexpr int kStickStep = 1024;
    const auto stickMoved = [](SHORT now, SHORT before, SHORT deadzone) {
        return std::abs(static_cast<int>(now) - static_cast<int>(before)) >=
                   kStickStep &&
               (std::abs(static_cast<int>(now)) > deadzone ||
                std::abs(static_cast<int>(before)) > deadzone);
    };
    if (stickMoved(current.sThumbLX, previous.sThumbLX,
                   XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) ||
        stickMoved(current.sThumbLY, previous.sThumbLY,
                   XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) ||
        stickMoved(current.sThumbRX, previous.sThumbRX,
                   XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE) ||
        stickMoved(current.sThumbRY, previous.sThumbRY,
                   XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE)) {
        return true;
    }

    constexpr int kTriggerStep = 4;
    const auto triggerMoved = [](BYTE now, BYTE before) {
        return std::abs(static_cast<int>(now) - static_cast<int>(before)) >=
                   kTriggerStep &&
               (now > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ||
                before > XINPUT_GAMEPAD_TRIGGER_THRESHOLD);
    };
    return triggerMoved(current.bLeftTrigger, previous.bLeftTrigger) ||
           triggerMoved(current.bRightTrigger, previous.bRightTrigger);
}

DWORD WINAPI XInputGetStateHook(DWORD userIndex, XINPUT_STATE* state) {
    hook_profiler::Scope totalTiming(
        hook_profiler::Point::LrXInputOwn);
    XInputGetStateFn original = g_originalXInputGetState;
    if (!original) return ERROR_DEVICE_NOT_CONNECTED;

    DWORD result = ERROR_DEVICE_NOT_CONNECTED;
    {
        hook_profiler::Scope originalTiming(
            hook_profiler::Point::LrXInputOriginal);
        result = original(userIndex, state);
    }
    if (userIndex >= g_xinputSamples.size()) return result;

    bool active = false;
    bool stateChanged = false;
    AcquireSRWLockExclusive(&g_xinputLock);
    XInputSample& sample = g_xinputSamples[userIndex];
    if (result != ERROR_SUCCESS || !state) {
        stateChanged = sample.initialized;
        sample = {};
    } else if (!sample.initialized) {
        sample.state = *state;
        sample.initialized = true;
        stateChanged = true;
    } else if (sample.state.dwPacketNumber != state->dwPacketNumber) {
        active = HasXInputActivity(state->Gamepad, sample.state.Gamepad);
        sample.state = *state;
        stateChanged = true;
    }
    ReleaseSRWLockExclusive(&g_xinputLock);

    if (stateChanged) {
        lr_text_editor::ObserveControllerState(
            userIndex, result == ERROR_SUCCESS && state,
            result == ERROR_SUCCESS && state
                ? state->Gamepad.wButtons : 0u);
    }

    if (active && !overlay::CapturesControllerInput()) {
        input::NotifyActivity(input::InputSource::Controller);
    }
    if (result == ERROR_SUCCESS && state &&
        (overlay::CapturesControllerInput() ||
         lr_text_editor::CapturesInput())) {
        state->Gamepad = {};
    }
    return result;
}

void InstallXInputActivityHook() {
    if (!PatchExecutableImportOrdinal(
            "XINPUT1_3.dll", kXInputGetStateOrdinal,
            reinterpret_cast<void*>(&XInputGetStateHook),
            reinterpret_cast<void**>(&g_originalXInputGetState))) {
        Log("LR XInput activity observer unavailable: forced prompt modes still work");
    }
}

char __fastcall PromptRefreshHook(void* state, void*) {
    hook_profiler::Scope totalTiming(
        hook_profiler::Point::LrPromptRefreshOwn);
    PromptRefreshFn original = g_originalPromptRefresh;
    if (!original) return 0;

    const settings::PromptMode preference = settings::ActiveInput().promptMode;
    const input::InputSource activity = input::LatestActivity();
    const NativeMode desired =
        input::ResolvePromptMode(preference, activity);
    const NativeMode previous =
        g_selectedMode.load(std::memory_order_acquire);
    const bool switchRequested = desired != NativeMode::Unknown &&
        desired != previous && state &&
        patch::memory::IsReadable(
            static_cast<std::byte*>(state) + kRefreshInitializedOffset, 1u);
    bool switched = false;
    std::uint64_t refreshGeneration = 0u;

    // Give the rest of the native UI frame one chance to consume the atlas
    // change and perform its own component/layout refresh. Rebuilding a missed
    // complex dialog label immediately can run before its parent layout pass
    if (g_pendingGuideRefreshGeneration != 0u) {
        const std::uint64_t pendingGeneration =
            g_pendingGuideRefreshGeneration;
        g_pendingGuideRefreshGeneration = 0u;
        if (!switchRequested) {
            const std::size_t rebuilt =
                lr_prompt_guides::RebuildMissing(pendingGeneration);
            if (rebuilt != 0u) {
                Log("LR rebuilt %zu glyph label(s) missed after one native UI tick",
                    rebuilt);
            }
        }
    }

    if (switchRequested) {
        g_selectedMode.store(desired, std::memory_order_release);
        g_overrideActive.store(true, std::memory_order_release);
        *(static_cast<std::byte*>(state) + kRefreshInitializedOffset) =
            std::byte{0};
        refreshGeneration = ++g_refreshGeneration;
        lr_prompt_guides::BeginRefresh(refreshGeneration);
        switched = true;
        Log("LR button prompts switched immediately: mode=%s source=%s preference=%u refresh-generation=%llu",
            ModeName(desired), input::InputSourceName(activity),
            static_cast<unsigned>(preference),
            static_cast<unsigned long long>(refreshGeneration));
    }

    char result = 0;
    {
        hook_profiler::Scope originalTiming(
            hook_profiler::Point::LrPromptRefreshOriginal);
        result = original(state);
    }
    compat::native_notifications::Pump();
    if (switched) {
        lr_text_editor::RefreshPromptGlyphs();
        g_pendingGuideRefreshGeneration = refreshGeneration;
    }
    return result;
}

BOOL CALLBACK Initialize(PINIT_ONCE, PVOID, PVOID*) {
    if (CurrentProfile().title != Title::LightningReturns ||
        !Supports(Capability::DynamicPrompts)) {
        return TRUE;
    }

    const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
    if (!image) {
        LogWarning("LR dynamic prompts disabled: main image unavailable");
        return TRUE;
    }

    const auto promptRefreshRange = image->At(
        kPromptRefreshRva, kPromptRefreshPrefixTemplate.size());
    const auto platformQueryRange = image->At(
        kPlatformQueryRva, kPlatformQueryPrefixTemplate.size());
    const auto nativeConfirmLayout = image->Address32(
        kNativeConfirmLayoutRva, sizeof(std::byte));
    const auto platformStateGlobal = image->Address32(
        kPlatformStateGlobalRva, sizeof(void*));
    if (!promptRefreshRange || !platformQueryRange ||
        !nativeConfirmLayout || !platformStateGlobal) {
        LogWarning("LR dynamic prompts disabled: RVA contract is outside the image");
        return TRUE;
    }

    std::byte* const promptRefresh =
        const_cast<std::byte*>(promptRefreshRange->data());
    std::byte* const platformQuery =
        const_cast<std::byte*>(platformQueryRange->data());
    auto promptRefreshPrefix = kPromptRefreshPrefixTemplate;
    auto platformQueryPrefix = kPlatformQueryPrefixTemplate;
    std::memcpy(promptRefreshPrefix.data() + 5u, &*nativeConfirmLayout,
                sizeof(*nativeConfirmLayout));
    std::memcpy(platformQueryPrefix.data() + 8u, &*platformStateGlobal,
                sizeof(*platformStateGlobal));
    if (std::memcmp(promptRefresh, promptRefreshPrefix.data(),
                    promptRefreshPrefix.size()) != 0 ||
        std::memcmp(platformQuery, platformQueryPrefix.data(),
                    platformQueryPrefix.size()) != 0) {
        LogWarning("LR dynamic prompts disabled: executable contract mismatch");
        return TRUE;
    }
    g_nativeConfirmLayout = reinterpret_cast<const std::byte*>(
        static_cast<std::uintptr_t>(*nativeConfirmLayout));

    if (!lr_prompt_guides::Install(*image)) return TRUE;

    if (!g_platformQueryDetour.Install(
            "lr-prompt-platform-selector", platformQuery,
            reinterpret_cast<void*>(&PlatformQueryHook), platformQueryPrefix,
            reinterpret_cast<void**>(&g_originalPlatformQuery))) {
        lr_prompt_guides::Revert();
        LogWarning("LR dynamic prompts disabled: platform selector hook failed");
        return TRUE;
    }

    if (!g_promptRefreshDetour.Install(
            "lr-prompt-native-refresh", promptRefresh,
            reinterpret_cast<void*>(&PromptRefreshHook), promptRefreshPrefix,
            reinterpret_cast<void**>(&g_originalPromptRefresh))) {
        if (!g_platformQueryDetour.RevertAndClear(
                g_originalPlatformQuery)) {
            LogError("LR platform selector rollback refused: live binding retained");
        }
        lr_prompt_guides::Revert();
        LogWarning("LR dynamic prompts disabled: native refresh hook failed");
        return TRUE;
    }

    InstallXInputActivityHook();
    return TRUE;
}

} // namespace

void Pump() {
    InitOnceExecuteOnce(&g_initializeOnce, &Initialize, nullptr, nullptr);
}

input::NativePromptMode CurrentMode() {
    const NativeMode selected =
        g_selectedMode.load(std::memory_order_acquire);
    if (selected != NativeMode::Unknown) return selected;
    const input::InputSource activity = input::LatestActivity();
    return input::ResolvePromptMode(
        settings::ActiveInput().promptMode, activity);
}

bool UsesOppositeConfirmLayout() {
    return g_nativeConfirmLayout && patch::memory::IsReadable(
        g_nativeConfirmLayout, sizeof(std::byte)) &&
        *g_nativeConfirmLayout != std::byte{0};
}

} // namespace novafix::game::lr_prompts
