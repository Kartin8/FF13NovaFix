#include "game/titles/ff13_2/ui/prompt_mode_manager.h"

#include "diagnostics/hook_profiler.h"
#include "diagnostics/log.h"
#include "game/core/game_profile.h"
#include "game/core/image_view.h"
#include "game/titles/ff13_2/ui/prompt_layout_registry.h"
#include "game/titles/ff13_2/ui/qte/qte_prompt_lifecycle.h"
#include "input/activity.h"
#include "input/prompt_policy.h"
#include "patch/detour32.h"
#include "patch/memory_access.h"
#include "settings/input_settings.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace novafix::game::xiii2_prompts {
namespace {

// button_set01 already supports native keyboard replacement and pristine restore
// Switch after input on the game thread, before UI drawing, without saving setting 17
constexpr std::uint32_t kInputUpdateRva = 0x002A5D00u;
constexpr std::uint32_t kFindTextureRva = 0x0039F8E0u;
constexpr std::uint32_t kReplaceKeyboardAtlasRva = 0x003A1440u;
constexpr std::uint32_t kRestoreControllerAtlasRva = 0x003A1630u;
constexpr std::uint32_t kPromptWrapperRva = 0x003A19B0u;
constexpr std::uint32_t kSettingGetterRva = 0x00554830u;
constexpr std::uint32_t kTextureManagerGlobalRva = 0x04D4E2B0u;
constexpr std::uint32_t kNativePromptFlagRva = 0x04D4E2B8u;
constexpr std::uint32_t kSecurityCookieRva = 0x01FA90DCu;
constexpr std::uint32_t kInputObserverGlobalRva = 0x02129F2Cu;
constexpr std::uint32_t kLoadedTextureStateOffset = 104u;
constexpr std::uint32_t kLoadedTextureState = 3u;
constexpr char kButtonAtlasName[] = "button_set01";

constexpr std::array<std::uint8_t, 13> kReplaceKeyboardPrefix{
    0x8B, 0x44, 0x24, 0x04, 0x83, 0xEC, 0x34,
    0x50, 0xE8, 0x63, 0xE1, 0xFF, 0xFF,
};
constexpr std::array<std::uint8_t, 8> kPromptWrapperPrefix{
    0x56, 0x6A, 0x11, 0xE8, 0x78, 0x2E, 0x1B, 0x00,
};
using NativeMode = input::NativePromptMode;
using InputUpdateFn = int (__thiscall*)(void* manager, int ticks);
using FindTextureFn = void* (__thiscall*)(void* manager, const char* name,
                                          const void* fallback, int loadIfMissing);
using ReplaceKeyboardAtlasFn = void* (__cdecl*)(int force);
using RestoreControllerAtlasFn = int (__cdecl*)();
using PromptWrapperFn = char* (__cdecl*)(int forceKeyboard);
using SettingGetterFn = int (__cdecl*)(int settingIndex);

INIT_ONCE g_initializeOnce = INIT_ONCE_STATIC_INIT;
patch::Detour32 g_inputUpdateDetour;
patch::Detour32 g_promptWrapperDetour;
InputUpdateFn g_originalInputUpdate{};
PromptWrapperFn g_originalPromptWrapper{};
SettingGetterFn g_settingGetter{};
FindTextureFn g_findTexture{};
ReplaceKeyboardAtlasFn g_replaceKeyboardAtlas{};
RestoreControllerAtlasFn g_restoreControllerAtlas{};
void* volatile* g_textureManagerGlobal{};
volatile LONG* g_nativePromptFlag{};
std::atomic_bool g_available{false};
NativeMode g_automaticPromptCandidate{NativeMode::Unknown};
ULONGLONG g_automaticPromptCandidateSince{};

std::array<std::byte, 8> AbsoluteLoadPrefix(
    const std::array<std::byte, 4>& opcode,
    std::uint32_t absoluteAddress) {
    std::array<std::byte, 8> result{};
    std::memcpy(result.data(), opcode.data(), opcode.size());
    std::memcpy(result.data() + opcode.size(), &absoluteAddress,
                sizeof(absoluteAddress));
    return result;
}

NativeMode ReadActualMode() {
    if (!g_nativePromptFlag) return NativeMode::Unknown;
    return InterlockedCompareExchange(g_nativePromptFlag, 0, 0) == 0
        ? NativeMode::Controller : NativeMode::Keyboard;
}

NativeMode RequestedModeNow() {
    return input::ResolvePromptMode(
        settings::ActiveInput().promptMode,
        input::LatestActivity());
}

NativeMode DesiredModeNow() {
    const NativeMode requested = RequestedModeNow();
    return xiii2_qte_prompts::ResolveMode(requested);
}

const char* ModeName(NativeMode mode) {
    if (mode == NativeMode::Keyboard) return "keyboard";
    if (mode == NativeMode::Controller) return "controller";
    return "unknown";
}

struct AtlasStatus {
    void* object{};
    std::uint32_t state{};

    bool ready() const { return object && state == kLoadedTextureState; }
};

AtlasStatus QueryAtlas(const char* name) {
    if (!g_textureManagerGlobal || !g_findTexture) return {};
    void* manager = *g_textureManagerGlobal;
    if (!manager) return {};

    void* atlas = g_findTexture(manager, name, nullptr, 0);
    if (!atlas) return {};
    const auto* bytes = static_cast<const std::byte*>(atlas);
    std::uint32_t state = 0;
    std::memcpy(&state, bytes + kLoadedTextureStateOffset, sizeof(state));
    return {atlas, state};
}

void ApplyQteMode(NativeMode mode) {
    if (mode == NativeMode::Unknown || !g_replaceKeyboardAtlas ||
        !g_restoreControllerAtlas || !g_nativePromptFlag) {
        return;
    }

    const NativeMode previous = ReadActualMode();
    if (mode == NativeMode::Keyboard) {
        g_replaceKeyboardAtlas(1);
        InterlockedExchange(g_nativePromptFlag, 1);
    } else {
        g_restoreControllerAtlas();
        InterlockedExchange(g_nativePromptFlag, 0);
    }

    if (previous != mode) {
        xiii2_prompt_layout_registry::QueueVisibleReflow();
    }
}

char* __cdecl PromptWrapperHook(int forceKeyboard) {
    static const auto totalPoint = hook_profiler::RegisterDynamicPoint(
        "hook.ff13-2.prompt-native-wrapper-total");
    static const auto originalPoint = hook_profiler::RegisterDynamicPoint(
        "hook.ff13-2.prompt-native-wrapper-original");
    hook_profiler::Scope totalTiming(totalPoint);

    PromptWrapperFn original = g_originalPromptWrapper;
    if (!original) return nullptr;

    const NativeMode desired = DesiredModeNow();
    if (g_available.load(std::memory_order_acquire) &&
        desired != NativeMode::Unknown && desired == ReadActualMode() &&
        g_settingGetter) {
        return reinterpret_cast<char*>(
            static_cast<std::uintptr_t>(g_settingGetter(17)));
    }

    hook_profiler::Scope originalTiming(originalPoint);
    return original(forceKeyboard);
}

void ApplyPendingMode() {
    if (!g_available.load(std::memory_order_acquire)) return;

    const settings::PromptMode preference = settings::ActiveInput().promptMode;
    const input::InputSource activity = input::LatestActivity();
    NativeMode desired = input::ResolvePromptMode(preference, activity);

    if (preference == settings::PromptMode::Auto) {
        const ULONGLONG now = GetTickCount64();
        if (desired != g_automaticPromptCandidate) {
            g_automaticPromptCandidate = desired;
            g_automaticPromptCandidateSince = now;
            return;
        }
        if (now - g_automaticPromptCandidateSince <
            input::kAutomaticPromptSwitchDebounceMs) {
            return;
        }
    } else {
        g_automaticPromptCandidate = NativeMode::Unknown;
        g_automaticPromptCandidateSince = 0u;
    }

    if (desired == NativeMode::Unknown) return;

    const NativeMode actual = ReadActualMode();
    const NativeMode frozen = xiii2_qte_prompts::ResolveMode(desired);
    AtlasStatus button{};
    if (desired != actual || frozen != actual) {
        button = QueryAtlas(kButtonAtlasName);
        if (!button.ready()) return;
    }

    desired = xiii2_qte_prompts::ApplyRequestedMode(desired);
    if (desired == NativeMode::Unknown || desired == actual) return;

    static const auto keyboardPoint = hook_profiler::RegisterDynamicPoint(
        "hook.ff13-2.prompt-atlas-keyboard");
    static const auto controllerPoint = hook_profiler::RegisterDynamicPoint(
        "hook.ff13-2.prompt-atlas-controller");
    if (desired == NativeMode::Keyboard) {
        hook_profiler::Scope atlasTiming(keyboardPoint);
        g_replaceKeyboardAtlas(1);
        InterlockedExchange(g_nativePromptFlag, 1);
    } else {
        hook_profiler::Scope atlasTiming(controllerPoint);
        g_restoreControllerAtlas();
        InterlockedExchange(g_nativePromptFlag, 0);
    }

    xiii2_prompt_layout_registry::QueueVisibleReflow();

    Log("XIII-2 button prompts switched: mode=%s source=%s preference=%u atlas-path=native visible-reflow=queued",
        ModeName(desired), input::InputSourceName(activity),
        static_cast<unsigned>(preference));
}

int __fastcall InputUpdateHook(void* manager, void*, int ticks) {
    static const auto totalPoint = hook_profiler::RegisterDynamicPoint(
        "hook.ff13-2.prompt-input-update-total");
    static const auto originalPoint = hook_profiler::RegisterDynamicPoint(
        "hook.ff13-2.prompt-input-update-original");
    hook_profiler::Scope totalTiming(totalPoint);
    InputUpdateFn original = g_originalInputUpdate;
    if (!original) return 0;
    int result = 0;
    {
        hook_profiler::Scope originalTiming(originalPoint);
        result = original(manager, ticks);
    }
    xiii2_prompt_layout_registry::CompleteVisibleReflow();
    ApplyPendingMode();
    xiii2_qte_prompts::FlushPendingAtlas();
    return result;
}

BOOL CALLBACK Initialize(PINIT_ONCE, PVOID, PVOID*) {
    if (CurrentProfile().title != Title::FinalFantasyXIII2 ||
        !Supports(Capability::DynamicPrompts)) {
        return TRUE;
    }

    const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
    if (!image) {
        LogWarning("XIII-2 dynamic prompts disabled: main image unavailable");
        return TRUE;
    }

    const auto inputUpdateRange = image->At(kInputUpdateRva, 8u);
    const auto findTextureRange = image->At(kFindTextureRva, 8u);
    const auto replaceKeyboardRange = image->At(
        kReplaceKeyboardAtlasRva, kReplaceKeyboardPrefix.size());
    const auto restoreControllerRange = image->At(kRestoreControllerAtlasRva, 11u);
    const auto promptWrapperRange = image->At(
        kPromptWrapperRva, kPromptWrapperPrefix.size());
    const auto settingGetterRange = image->At(
        kSettingGetterRva, 1u);
    const auto textureManagerRange = image->At(
        kTextureManagerGlobalRva, sizeof(void*));
    const auto nativePromptFlagRange = image->At(
        kNativePromptFlagRva, sizeof(LONG));
    const auto inputObserverAddress = image->Address32(
        kInputObserverGlobalRva, sizeof(void*));
    const auto cookieAddress = image->Address32(
        kSecurityCookieRva, sizeof(std::uint32_t));
    if (!inputUpdateRange || !findTextureRange ||
        !replaceKeyboardRange || !restoreControllerRange ||
        !promptWrapperRange || !settingGetterRange ||
        !textureManagerRange ||
        !nativePromptFlagRange ||
        !xiii2_qte_prompts::HasExpectedContract(*image) ||
        !inputObserverAddress || !cookieAddress) {
        LogWarning("XIII-2 dynamic prompts disabled: RVA contract is outside the image or x86 address space");
        return TRUE;
    }
    std::byte* inputUpdate = const_cast<std::byte*>(inputUpdateRange->data());
    std::byte* findTexture = const_cast<std::byte*>(findTextureRange->data());
    std::byte* replaceKeyboard = const_cast<std::byte*>(replaceKeyboardRange->data());
    std::byte* restoreController = const_cast<std::byte*>(restoreControllerRange->data());
    std::byte* promptWrapper = const_cast<std::byte*>(
        promptWrapperRange->data());
    std::byte* settingGetter = const_cast<std::byte*>(
        settingGetterRange->data());
    std::byte* textureManagerGlobal = const_cast<std::byte*>(textureManagerRange->data());
    std::byte* nativePromptFlag = const_cast<std::byte*>(nativePromptFlagRange->data());
    const std::array<std::byte, 8> inputUpdatePrefix = AbsoluteLoadPrefix(
        {std::byte{0x83}, std::byte{0xEC}, std::byte{0x10}, std::byte{0xA1}},
        *inputObserverAddress);
    const std::array<std::byte, 8> findTexturePrefix = AbsoluteLoadPrefix(
        {std::byte{0x83}, std::byte{0xEC}, std::byte{0x20}, std::byte{0xA1}},
        *cookieAddress);
    std::array<std::byte, 11> restoreControllerPrefix{
        std::byte{0x81}, std::byte{0xEC}, std::byte{0xA8}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0xA1}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    };
    std::memcpy(restoreControllerPrefix.data() + 7, &*cookieAddress,
                sizeof(*cookieAddress));
    const bool matches =
        std::memcmp(inputUpdate, inputUpdatePrefix.data(), inputUpdatePrefix.size()) == 0 &&
        std::memcmp(findTexture, findTexturePrefix.data(), findTexturePrefix.size()) == 0 &&
        std::memcmp(replaceKeyboard, kReplaceKeyboardPrefix.data(),
                    kReplaceKeyboardPrefix.size()) == 0 &&
        std::memcmp(restoreController, restoreControllerPrefix.data(),
                    restoreControllerPrefix.size()) == 0 &&
        std::memcmp(promptWrapper, kPromptWrapperPrefix.data(),
                    kPromptWrapperPrefix.size()) == 0 &&
        patch::memory::IsExecutable(settingGetter, 1u);
    if (!matches) {
        LogWarning("XIII-2 dynamic prompts disabled: executable contract mismatch");
        return TRUE;
    }

    g_findTexture = reinterpret_cast<FindTextureFn>(findTexture);
    g_replaceKeyboardAtlas = reinterpret_cast<ReplaceKeyboardAtlasFn>(replaceKeyboard);
    g_restoreControllerAtlas = reinterpret_cast<RestoreControllerAtlasFn>(restoreController);
    g_settingGetter = reinterpret_cast<SettingGetterFn>(settingGetter);
    g_textureManagerGlobal = reinterpret_cast<void* volatile*>(textureManagerGlobal);
    g_nativePromptFlag = reinterpret_cast<volatile LONG*>(nativePromptFlag);
    if (!xiii2_prompt_layout_registry::Install(*image)) {
        g_findTexture = nullptr;
        g_replaceKeyboardAtlas = nullptr;
        g_restoreControllerAtlas = nullptr;
        g_settingGetter = nullptr;
        g_textureManagerGlobal = nullptr;
        g_nativePromptFlag = nullptr;
        LogWarning("XIII-2 dynamic prompts disabled: prompt layout registry unavailable");
        return TRUE;
    }

    const bool wrapperInstalled = g_promptWrapperDetour.Install(
        "ff13-2-prompt-wrapper-ownership", promptWrapper,
        reinterpret_cast<void*>(&PromptWrapperHook),
        std::as_bytes(std::span{kPromptWrapperPrefix}),
        reinterpret_cast<void**>(&g_originalPromptWrapper));
    if (!wrapperInstalled) {
        g_originalPromptWrapper = nullptr;
        LogWarning("XIII-2 native prompt-wrapper ownership unavailable: redundant saved-setting transactions may recur");
    }

    if (!g_inputUpdateDetour.Install(
            "ff13-2-dynamic-prompts", inputUpdate,
            reinterpret_cast<void*>(&InputUpdateHook), inputUpdatePrefix,
            reinterpret_cast<void**>(&g_originalInputUpdate))) {
        const bool registryReverted =
            xiii2_prompt_layout_registry::Revert();
        const bool wrapperReverted = !wrapperInstalled ||
            g_promptWrapperDetour.RevertAndClear(g_originalPromptWrapper);
        if (registryReverted && wrapperReverted) {
            g_findTexture = nullptr;
            g_replaceKeyboardAtlas = nullptr;
            g_restoreControllerAtlas = nullptr;
            g_settingGetter = nullptr;
            g_textureManagerGlobal = nullptr;
            g_nativePromptFlag = nullptr;
        }
        LogWarning("XIII-2 dynamic prompts disabled: input-update detour failed");
        return TRUE;
    }

    if (!xiii2_qte_prompts::InstallLifecycle(
            *image,
            {&RequestedModeNow, &ReadActualMode, &ApplyQteMode})) {
        const bool inputReverted =
            g_inputUpdateDetour.RevertAndClear(g_originalInputUpdate);
        const bool registryReverted =
            xiii2_prompt_layout_registry::Revert();
        const bool wrapperReverted = !wrapperInstalled ||
            g_promptWrapperDetour.RevertAndClear(g_originalPromptWrapper);
        if (inputReverted && registryReverted && wrapperReverted) {
            g_findTexture = nullptr;
            g_replaceKeyboardAtlas = nullptr;
            g_restoreControllerAtlas = nullptr;
            g_settingGetter = nullptr;
            g_textureManagerGlobal = nullptr;
            g_nativePromptFlag = nullptr;
        }
        LogWarning("XIII-2 dynamic prompts disabled: QTE owner lifecycle unavailable");
        return TRUE;
    }
    g_available.store(true, std::memory_order_release);
    return TRUE;
}

} // namespace

void Pump() {
    InitOnceExecuteOnce(&g_initializeOnce, &Initialize, nullptr, nullptr);
}

} // namespace novafix::game::xiii2_prompts
