#include "game/shared/language_override.h"

#include "diagnostics/log.h"
#include "game/core/image_view.h"
#include "patch/iat_hook.h"
#include "patch/memory_access.h"
#include "settings/game_settings.h"
#include "common/guarded_call.h"

#include <windows.h>

#if defined(_MSC_VER)
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#endif

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>

namespace novafix::game::language_override {
namespace {

struct Contract {
    std::uint32_t checkpointReturnRva{};
    std::uint32_t languageRva{};
    std::uint32_t areaRva{};
    std::uint32_t voiceRva{};
};

constexpr Contract kXiii{
    0x00A921DFu, 0x022A19B8u, 0x022A19BCu, 0x0243E361u};
constexpr Contract kXiii2{
    0x00807D20u, 0x01F46D98u, 0x01F46D9Cu, 0x04D6B365u};

constexpr std::uint32_t kLrLanguageRva = 0x01EB12ECu;
constexpr std::uint32_t kLrVoiceStateSlotRva = 0x01F510C8u;
// Native VoiceLanguage callbacks write the state DWORD: Japanese=0,
// English=1. The following byte belongs to DownloadContent.
constexpr std::uint32_t kLrEnglishVoiceSetterRva = 0x00086810u;
constexpr std::uint32_t kLrJapaneseVoiceSetterRva = 0x000867E0u;
constexpr std::uint32_t kLrEnvironmentSaveRva = 0x00086920u;
constexpr std::array<std::byte, 9> kLrVoiceSetterPrefix{
    std::byte{0x55}, std::byte{0x8B}, std::byte{0xEC},
    std::byte{0x80}, std::byte{0x7D}, std::byte{0x08},
    std::byte{0x00}, std::byte{0x74}, std::byte{0x0B}};
constexpr std::array<std::byte, 4> kLrEnvironmentSavePrefix{
    std::byte{0x56}, std::byte{0x57}, std::byte{0x8B}, std::byte{0xF1}};

using SleepFn = void (WINAPI*)(DWORD);
using TimeGetTimeFn = DWORD (WINAPI*)();

SleepFn g_originalSleep{};
TimeGetTimeFn g_originalTimeGetTime{};
Contract g_contract{};
Title g_title{Title::Unknown};
std::uintptr_t g_imageBase{};
std::atomic_bool g_installed{};
std::atomic_bool g_applied{};
std::atomic_int g_appliedLanguage{-1};
std::atomic_int g_appliedArea{-1};
std::atomic_int g_appliedVoice{-1};

#if defined(_MSC_VER)
__forceinline void* ReturnAddress() {
    return _ReturnAddress();
#else
__attribute__((always_inline)) inline void* ReturnAddress() {
    return __builtin_return_address(0);
#endif
}

bool ResolveStatePointers(int*& language, int*& area,
                          unsigned char*& voice) {
    const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
    if (!image) return false;
    const auto languageBytes = image->At(g_contract.languageRva, sizeof(int));
    const auto areaBytes = image->At(g_contract.areaRva, sizeof(int));
    const auto voiceBytes = image->At(g_contract.voiceRva, sizeof(char));
    if (!languageBytes || !areaBytes || !voiceBytes) return false;
    language = reinterpret_cast<int*>(const_cast<std::byte*>(
        languageBytes->data()));
    area = reinterpret_cast<int*>(const_cast<std::byte*>(
        areaBytes->data()));
    voice = reinterpret_cast<unsigned char*>(const_cast<std::byte*>(
        voiceBytes->data()));
    return true;
}

void Apply() {
    int* language{};
    int* area{};
    unsigned char* voice{};
    if (!ResolveStatePointers(language, area, voice)) return;
    if (*language < -1 || *language > 10 || *area < -1 || *area > 4 ||
        *voice > 1u) {
        return;
    }

    const settings::GameSettings settings = settings::ActiveGame();
    if (!settings.overrideLanguageSettings) {
        g_appliedLanguage.store(*language, std::memory_order_release);
        g_appliedArea.store(*area, std::memory_order_release);
        g_appliedVoice.store(*voice, std::memory_order_release);
        g_applied.store(true, std::memory_order_release);
        return;
    }
    constexpr std::array<int, 8> kLanguages{
        1, 0, 4, 5, 3, 6, 8, 10};
    constexpr std::array<int, 8> kAreas{
        1, 0, 3, 3, 3, 3, 2, 2};
    const unsigned selected = static_cast<unsigned>(
        settings.interfaceLanguage);
    if (selected < kLanguages.size() && kLanguages[selected] >= 0) {
        *language = kLanguages[selected];
        *area = kAreas[selected];
    }
    *voice = settings.voiceLanguage == settings::VoiceLanguage::Japanese
        ? 1u : 0u;

    g_appliedLanguage.store(*language, std::memory_order_release);
    g_appliedArea.store(*area, std::memory_order_release);
    g_appliedVoice.store(*voice, std::memory_order_release);
    g_applied.store(true, std::memory_order_release);
}

bool ApplyLightningReturnsInterface() {
    const settings::GameSettings configured = settings::ActiveGame();
    if (!configured.overrideLanguageSettings) return true;
    const settings::InterfaceLanguage selected = configured.interfaceLanguage;
    constexpr std::array<int, 8> kLanguages{
        1, 0, 4, 5, 3, 6, 8, 10};
    const unsigned index = static_cast<unsigned>(selected);
    if (index >= kLanguages.size() || kLanguages[index] < 0) return false;

    const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
    if (!image) return false;
    const auto bytes = image->At(kLrLanguageRva, sizeof(int));
    if (!bytes) return false;
    auto* language = reinterpret_cast<int*>(
        const_cast<std::byte*>(bytes->data()));
    if (*language < -1 || *language > 10) return false;
    *language = kLanguages[index];
    g_appliedLanguage.store(*language, std::memory_order_release);
    return true;
}

using LrVoiceSetter = int (__cdecl*)(char);
using LrEnvironmentSave = int (__thiscall*)(void*);

struct LrVoiceState {
    unsigned char* state{};
    LrVoiceSetter setter{};
    LrEnvironmentSave save{};
};

std::optional<LrVoiceState>
ResolveLightningReturnsVoice(settings::VoiceLanguage language) {
    const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
    if (!image) return std::nullopt;
    const auto slotBytes = image->At(
        kLrVoiceStateSlotRva, sizeof(unsigned char*));
    const std::uint32_t setterRva =
        language == settings::VoiceLanguage::Japanese
            ? kLrJapaneseVoiceSetterRva
            : kLrEnglishVoiceSetterRva;
    const auto setterBytes = image->At(setterRva, kLrVoiceSetterPrefix.size());
    const auto saveBytes = image->At(
        kLrEnvironmentSaveRva, kLrEnvironmentSavePrefix.size());
    if (!slotBytes || !setterBytes || !saveBytes ||
        std::memcmp(setterBytes->data(), kLrVoiceSetterPrefix.data(),
                    kLrVoiceSetterPrefix.size()) != 0 ||
        std::memcmp(saveBytes->data(), kLrEnvironmentSavePrefix.data(),
                    kLrEnvironmentSavePrefix.size()) != 0) {
        return std::nullopt;
    }
    const auto state = *reinterpret_cast<unsigned char* const*>(
        slotBytes->data());
    if (!patch::memory::IsReadable(state, sizeof(std::uint32_t))) {
        return std::nullopt;
    }
    const auto setter = reinterpret_cast<LrVoiceSetter>(
        const_cast<std::byte*>(setterBytes->data()));
    const auto save = reinterpret_cast<LrEnvironmentSave>(
        const_cast<std::byte*>(saveBytes->data()));
    return LrVoiceState{state, setter, save};
}

void WINAPI SleepHook(DWORD milliseconds) {
    const auto caller = reinterpret_cast<std::uintptr_t>(ReturnAddress());
    if (!g_applied.load(std::memory_order_acquire) && g_imageBase != 0u &&
        caller == g_imageBase + g_contract.checkpointReturnRva) {
        Apply();
    }
    if (g_originalSleep) g_originalSleep(milliseconds);
}

DWORD WINAPI TimeGetTimeHook() {
    // The second startup timer returns after Environment.ini has been read,
    // but before language-dependent file lists are selected.
    constexpr std::uint32_t kLrCheckpointReturnRva = 0x0003249Cu;
    const auto caller = reinterpret_cast<std::uintptr_t>(ReturnAddress());
    if (!g_applied.load(std::memory_order_acquire) && g_imageBase != 0u &&
        caller == g_imageBase + kLrCheckpointReturnRva) {
        ApplyLightningReturnsInterface();
        g_applied.store(true, std::memory_order_release);
    }
    return g_originalTimeGetTime ? g_originalTimeGetTime() : 0u;
}

} // namespace

void InstallEarly(Title title) {
    g_title = title;
    if (title == Title::FinalFantasyXIII) {
        g_contract = kXiii;
    } else if (title == Title::FinalFantasyXIII2) {
        g_contract = kXiii2;
    } else if (title == Title::LightningReturns) {
        g_imageBase =
            reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        void* original{};
        if (g_imageBase != 0u && PatchExecutableImport(
                "winmm.dll", "timeGetTime",
                reinterpret_cast<void*>(&TimeGetTimeHook), &original)) {
            g_originalTimeGetTime =
                reinterpret_cast<TimeGetTimeFn>(original);
            g_installed.store(true, std::memory_order_release);
        }
        return;
    } else {
        return;
    }
    g_imageBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    void* original{};
    if (g_imageBase != 0u && PatchExecutableImport(
            "KERNEL32.dll", "Sleep", reinterpret_cast<void*>(&SleepHook),
            &original)) {
        g_originalSleep = reinterpret_cast<SleepFn>(original);
        g_installed.store(true, std::memory_order_release);
    }
}

std::optional<settings::InterfaceLanguage> CurrentInterfaceLanguage(
    Title title) {
    std::uint32_t languageRva{};
    if (title == Title::FinalFantasyXIII) {
        languageRva = kXiii.languageRva;
    } else if (title == Title::FinalFantasyXIII2) {
        languageRva = kXiii2.languageRva;
    } else if (title == Title::LightningReturns) {
        languageRva = kLrLanguageRva;
    } else {
        return std::nullopt;
    }

    const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
    if (!image) return std::nullopt;
    const auto bytes = image->At(languageRva, sizeof(int));
    if (!bytes) return std::nullopt;
    const int current = *reinterpret_cast<const int*>(bytes->data());
    switch (current) {
    case 0: return settings::InterfaceLanguage::Japanese;
    case 1: return settings::InterfaceLanguage::English;
    case 3: return settings::InterfaceLanguage::Italian;
    case 4: return settings::InterfaceLanguage::German;
    case 5: return settings::InterfaceLanguage::French;
    case 6: return settings::InterfaceLanguage::Spanish;
    case 8: return settings::InterfaceLanguage::Korean;
    case 10: return settings::InterfaceLanguage::TraditionalChinese;
    default: return std::nullopt;
    }
}

std::optional<settings::VoiceLanguage> CurrentVoiceLanguage(Title title) {
    if (title == Title::FinalFantasyXIII ||
        title == Title::FinalFantasyXIII2) {
        const Contract& contract = title == Title::FinalFantasyXIII
            ? kXiii : kXiii2;
        const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
        if (!image) return std::nullopt;
        const auto bytes = image->At(contract.voiceRva, sizeof(char));
        if (!bytes) return std::nullopt;
        const unsigned char current =
            std::to_integer<unsigned char>((*bytes)[0]);
        if (current > 1u) return std::nullopt;
        return current != 0u ? settings::VoiceLanguage::Japanese
                             : settings::VoiceLanguage::English;
    }
    if (title != Title::LightningReturns) return std::nullopt;
    const auto resolved = ResolveLightningReturnsVoice(
        settings::VoiceLanguage::English);
    if (!resolved) return std::nullopt;
    const std::uint32_t current =
        *reinterpret_cast<const std::uint32_t*>(resolved->state);
    if (current > 1u) return std::nullopt;
    return current == 0u ? settings::VoiceLanguage::Japanese
                         : settings::VoiceLanguage::English;
}

bool ApplyVoiceLanguage(Title title, settings::VoiceLanguage language) {
    if (title != Title::LightningReturns ||
        !settings::OverridesGameLanguage()) {
        return false;
    }
    const auto resolved = ResolveLightningReturnsVoice(language);
    if (!resolved) return false;
    return guarded_call::Run([&] {
        resolved->setter(1);
        resolved->save(resolved->state);
    });
}

void Report() {
    if (!g_installed.load(std::memory_order_acquire)) {
        LogWarning("Game language override unavailable: startup checkpoint hook failed");
        return;
    }
    if (!g_applied.load(std::memory_order_acquire)) {
        if (g_title == Title::LightningReturns) return;
        LogWarning("Game language override unavailable: startup checkpoint was missed");
        return;
    }
    if (g_title == Title::LightningReturns) {
        const int language =
            g_appliedLanguage.load(std::memory_order_acquire);
        const int voice = g_appliedVoice.load(std::memory_order_acquire);
        LogInfo("Game language startup state: interface=%s voice=%s",
            language >= 0 ? std::to_string(language).c_str() : "Steam",
            voice < 0 ? "Game setting" :
            (voice != 0 ? "Japanese" : "English"));
        return;
    }
    LogInfo("Game language applied: language=%d area=%d voice=%s",
        g_appliedLanguage.load(std::memory_order_acquire),
        g_appliedArea.load(std::memory_order_acquire),
        g_appliedVoice.load(std::memory_order_acquire) != 0
            ? "Japanese" : "English");
}

} // namespace novafix::game::language_override
