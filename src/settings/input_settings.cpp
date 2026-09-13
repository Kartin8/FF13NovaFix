#include "settings/input_settings.h"

#include "diagnostics/log.h"
#include "settings/config_path.h"
#include "settings/ini_value.h"
#include "settings/named_values.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace novafix::settings {
namespace {

INIT_ONCE g_loadOnce = INIT_ONCE_STATIC_INIT;
SRWLOCK g_lock = SRWLOCK_INIT;
InputSettings g_active{};
InputSettings g_stored{};
std::wstring g_configPath;
std::atomic_uint64_t g_activeGeneration{1};

void Normalize(InputSettings& value) {
    value.promptMode = static_cast<PromptMode>(
        std::clamp(static_cast<unsigned>(value.promptMode), 0u, 2u));
    value.vibrationStrengthPercent =
        std::clamp(value.vibrationStrengthPercent, 10u, 200u);
    value.overlayHotkey = static_cast<OverlayHotkey>(
        std::clamp(static_cast<unsigned>(value.overlayHotkey), 0u,
                   static_cast<unsigned>(OverlayHotkey::Home)));
    value.alternateOverlayHotkey = static_cast<OverlayHotkey>(
        std::clamp(static_cast<unsigned>(value.alternateOverlayHotkey), 0u,
                   static_cast<unsigned>(OverlayHotkey::Home)));
    value.overlayGamepadHotkey = static_cast<OverlayGamepadHotkey>(
        std::clamp(static_cast<unsigned>(value.overlayGamepadHotkey), 0u,
                   static_cast<unsigned>(OverlayGamepadHotkey::ViewMenu)));
    if (value.overlayHotkey == OverlayHotkey::Disabled &&
        value.alternateOverlayHotkey == OverlayHotkey::Disabled) {
        value.overlayHotkey = OverlayHotkey::F10;
    }
}

BOOL CALLBACK Load(PINIT_ONCE, PVOID, PVOID*) {
    g_configPath = storage::ConfigFile();

    InputSettings loaded{};
    if (!g_configPath.empty()) {
        loaded.promptMode = static_cast<PromptMode>(
            ini::ReadChoice(
                g_configPath, L"Input", L"PromptMode",
                names::kPromptMode,
                static_cast<unsigned>(PromptMode::Auto)));
        loaded.controllerVibration = ini::ReadBoolean(
            g_configPath, L"Input", L"ControllerVibration", true);
        loaded.vibrationStrengthPercent = ini::ReadUnsigned(
            g_configPath, L"Input", L"VibrationStrengthPercent", 100);
        loaded.overlayHotkey = static_cast<OverlayHotkey>(
            ini::ReadChoice(
                g_configPath, L"Input", L"OverlayHotkey",
                names::kOverlayHotkey,
                static_cast<unsigned>(OverlayHotkey::F10)));
        loaded.alternateOverlayHotkey = static_cast<OverlayHotkey>(
            ini::ReadChoice(
                g_configPath, L"Input", L"AlternateOverlayHotkey",
                names::kOverlayHotkey,
                static_cast<unsigned>(OverlayHotkey::Grave)));
        loaded.overlayGamepadHotkey = static_cast<OverlayGamepadHotkey>(
            ini::ReadChoice(
                g_configPath, L"Input", L"OverlayGamepadHotkey",
                names::kOverlayGamepadHotkey,
                static_cast<unsigned>(
                    OverlayGamepadHotkey::StickClicks)));
    }
    Normalize(loaded);
    g_active = loaded;
    g_stored = loaded;
    Log("Input settings loaded: prompt-mode=%u vibration=%d strength=%u%% hotkeys=%u/%u gamepad-hotkey=%u",
        static_cast<unsigned>(loaded.promptMode), loaded.controllerVibration,
        loaded.vibrationStrengthPercent,
        static_cast<unsigned>(loaded.overlayHotkey),
        static_cast<unsigned>(loaded.alternateOverlayHotkey),
        static_cast<unsigned>(loaded.overlayGamepadHotkey));
    return TRUE;
}

void EnsureLoaded() {
    InitOnceExecuteOnce(&g_loadOnce, &Load, nullptr, nullptr);
}

} // namespace

InputSettings ActiveInput() {
    EnsureLoaded();
    thread_local std::uint64_t cachedGeneration{};
    thread_local InputSettings cached{};
    const std::uint64_t generation =
        g_activeGeneration.load(std::memory_order_acquire);
    if (cachedGeneration == generation) return cached;

    AcquireSRWLockShared(&g_lock);
    cached = g_active;
    cachedGeneration =
        g_activeGeneration.load(std::memory_order_relaxed);
    ReleaseSRWLockShared(&g_lock);
    return cached;
}

InputSettings StoredInput() {
    EnsureLoaded();
    AcquireSRWLockShared(&g_lock);
    const InputSettings value = g_stored;
    ReleaseSRWLockShared(&g_lock);
    return value;
}

void PreviewInput(const InputSettings& requested) {
    EnsureLoaded();
    InputSettings value = requested;
    Normalize(value);
    AcquireSRWLockExclusive(&g_lock);
    g_active = value;
    g_activeGeneration.fetch_add(1, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_lock);
}

bool SaveInput(const InputSettings& requested) {
    EnsureLoaded();
    InputSettings value = requested;
    Normalize(value);
    if (g_configPath.empty()) return false;
    const std::array<ini::Entry, 6> entries{{
        {L"PromptMode", ini::FormatChoice(
             static_cast<unsigned>(value.promptMode), names::kPromptMode)},
        {L"ControllerVibration",
         ini::FormatBoolean(value.controllerVibration)},
        {L"VibrationStrengthPercent",
         std::to_wstring(value.vibrationStrengthPercent)},
        {L"OverlayHotkey", ini::FormatChoice(
             static_cast<unsigned>(value.overlayHotkey),
             names::kOverlayHotkey)},
        {L"AlternateOverlayHotkey", ini::FormatChoice(
             static_cast<unsigned>(value.alternateOverlayHotkey),
             names::kOverlayHotkey)},
        {L"OverlayGamepadHotkey", ini::FormatChoice(
             static_cast<unsigned>(value.overlayGamepadHotkey),
             names::kOverlayGamepadHotkey)},
    }};
    const bool saved = ini::WriteValues(g_configPath, L"Input", entries);
    if (saved) {
        AcquireSRWLockExclusive(&g_lock);
        g_active = value;
        g_stored = value;
        g_activeGeneration.fetch_add(1, std::memory_order_release);
        ReleaseSRWLockExclusive(&g_lock);
    }
    Log("Input settings saved: config=%d prompt-mode=%u vibration=%d strength=%u%% hotkeys=%u/%u gamepad-hotkey=%u",
        saved, static_cast<unsigned>(value.promptMode), value.controllerVibration,
        value.vibrationStrengthPercent,
        static_cast<unsigned>(value.overlayHotkey),
        static_cast<unsigned>(value.alternateOverlayHotkey),
        static_cast<unsigned>(value.overlayGamepadHotkey));
    return saved;
}

} // namespace novafix::settings
