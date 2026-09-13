#include "settings/graphics_settings.h"

#include "diagnostics/log.h"
#include "common/module_path.h"
#include "game/core/game_profile.h"
#include "settings/config_path.h"
#include "settings/ini_value.h"
#include "settings/named_values.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace novafix::settings {
namespace {

INIT_ONCE g_loadOnce = INIT_ONCE_STATIC_INIT;
SRWLOCK g_settingsLock = SRWLOCK_INIT;
GraphicsSettings g_active{};
GraphicsSettings g_stored{};
std::wstring g_configPath;
std::wstring g_launcherPath;
std::atomic_bool g_launcherIntegration{true};
std::atomic<game::Title> g_title{game::Title::Unknown};
std::atomic_uint64_t g_activeGeneration{1};
std::atomic_bool g_correctScreenSpaceAlignment{true};

void ApplyLiveFields(GraphicsSettings& target,
                     const GraphicsSettings& source) {
    target.highResolutionScissorFix = source.highResolutionScissorFix;
    target.correctScreenSpaceAlignment = source.correctScreenSpaceAlignment;
    g_correctScreenSpaceAlignment.store(
        source.correctScreenSpaceAlignment, std::memory_order_release);
    target.anisotropicFiltering = source.anisotropicFiltering;
    target.xiii2MipLodBias = source.xiii2MipLodBias;
}

void BuildPaths() {
    const std::wstring& bin = storage::ModuleDirectory();
    if (bin.empty()) return;
    g_configPath = storage::ConfigFile();

    std::wstring root = bin;
    for (int i = 0; i < 4; ++i) root = path::Directory(root);
    if (!root.empty()) g_launcherPath = root + L"\\setup.xml";
}

unsigned ReadUnsigned(const wchar_t* key, unsigned fallback) {
    return ini::ReadUnsigned(g_configPath, L"Graphics", key, fallback);
}

std::string ReadFile(const std::wstring& path) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) return {};
    std::vector<char> bytes;
    char block[4096]{};
    while (const size_t count = std::fread(block, 1, sizeof(block), file)) {
        bytes.insert(bytes.end(), block, block + count);
    }
    std::fclose(file);
    return std::string(bytes.begin(), bytes.end());
}

bool ReplaceXmlInteger(std::string& xml, const char* tag, int value) {
    const std::string opening = std::string("<") + tag + ">";
    const std::string closing = std::string("</") + tag + ">";
    const size_t begin = xml.find(opening);
    if (begin == std::string::npos) return false;
    const size_t valueBegin = begin + opening.size();
    const size_t end = xml.find(closing, valueBegin);
    if (end == std::string::npos) return false;
    xml.replace(valueBegin, end - valueBegin, std::to_string(value));
    return true;
}

unsigned ShadowFromLauncherIndex(int index) {
    constexpr unsigned values[]{512, 1024, 2048, 4096, 8192};
    return values[std::clamp(index, 0, 4)];
}

unsigned MsaaFromLauncherIndex(int index) {
    constexpr unsigned values[]{2, 4, 8, 16};
    return values[std::clamp(index, 0, 3)];
}

void Normalize(GraphicsSettings& value) {
    value.displayMode = static_cast<DisplayMode>(
        std::clamp(static_cast<unsigned>(value.displayMode), 0u, 2u));
    value.monitorIndex = std::clamp(value.monitorIndex, 0u, 16u);
    value.width = std::clamp(value.width, 640u, 16384u);
    value.height = std::clamp(value.height, 480u, 16384u);
    value.shadowResolution = ShadowFromLauncherIndex(ShadowLauncherIndex(value.shadowResolution));
    value.msaaSamples = MsaaFromLauncherIndex(MsaaLauncherIndex(value.msaaSamples));
    constexpr unsigned anisotropyValues[]{0, 2, 4, 8, 16};
    unsigned nearest = anisotropyValues[0];
    for (const unsigned candidate : anisotropyValues) {
        const unsigned distance = candidate > value.anisotropicFiltering
            ? candidate - value.anisotropicFiltering
            : value.anisotropicFiltering - candidate;
        const unsigned bestDistance = nearest > value.anisotropicFiltering
            ? nearest - value.anisotropicFiltering
            : value.anisotropicFiltering - nearest;
        if (distance < bestDistance) nearest = candidate;
    }
    value.anisotropicFiltering = nearest;
    if (!std::isfinite(value.xiii2MipLodBias)) {
        value.xiii2MipLodBias = 0.0f;
    }
    value.xiii2MipLodBias = std::clamp(value.xiii2MipLodBias, -1.0f, 1.0f);
}

BOOL CALLBACK LoadSettings(PINIT_ONCE, PVOID, PVOID*) {
    BuildPaths();
    GraphicsSettings loaded{};
    const bool hasConfig = !g_configPath.empty() && GetFileAttributesW(g_configPath.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (hasConfig) {
        loaded.overrideLauncherGraphics = ini::ReadBoolean(
            g_configPath, L"Graphics", L"OverrideLauncherGraphics",
            loaded.overrideLauncherGraphics);
        loaded.displayMode = static_cast<DisplayMode>(
            ini::ReadChoice(
                g_configPath, L"Graphics", L"DisplayMode",
                names::kDisplayMode,
                static_cast<unsigned>(loaded.displayMode)));
        loaded.monitorIndex = ini::ReadChoice(
            g_configPath, L"Graphics", L"Monitor", names::kMonitor,
            loaded.monitorIndex);
        loaded.width = ReadUnsigned(L"Width", loaded.width);
        loaded.height = ReadUnsigned(L"Height", loaded.height);
        loaded.shadowResolution = ReadUnsigned(L"ShadowResolution", loaded.shadowResolution);
        loaded.msaaSamples = ReadUnsigned(L"MSAA", loaded.msaaSamples);
        loaded.highResolutionScissorFix = ini::ReadBoolean(
            g_configPath, L"Graphics", L"HighResolutionScissorFix",
            loaded.highResolutionScissorFix);
        loaded.optimizeUiVertexBuffer = ini::ReadBoolean(
            g_configPath, L"Graphics", L"OptimizeUiVertexBuffer",
            loaded.optimizeUiVertexBuffer);
        loaded.correctScreenSpaceAlignment = ini::ReadBoolean(
            g_configPath, L"Graphics", L"CorrectScreenSpaceAlignment",
            loaded.correctScreenSpaceAlignment);
        loaded.anisotropicFiltering = ReadUnsigned(
            L"AnisotropicFiltering", loaded.anisotropicFiltering);
        loaded.xiii2MipLodBias = ini::ReadFloat(
            g_configPath, L"Graphics", L"XIII2MipLodBias",
            loaded.xiii2MipLodBias);
        loaded.xiii2ShaderCorrections = ini::ReadBoolean(
            g_configPath, L"Graphics", L"XIII2ShaderCorrections",
            loaded.xiii2ShaderCorrections);
        loaded.xiii2OptimizeShadowPipeline = ini::ReadBoolean(
            g_configPath, L"Graphics", L"XIII2OptimizeShadowPipeline",
            loaded.xiii2OptimizeShadowPipeline);
    }
    Normalize(loaded);
    g_active = loaded;
    g_stored = loaded;
    g_correctScreenSpaceAlignment.store(
        loaded.correctScreenSpaceAlignment, std::memory_order_release);
    Log("Graphics settings loaded: override=%d mode=%u monitor=%u size=%ux%u shadow=%u MSAA=%u scissor-fix=%d ui-vb=%d screen-align=%d AF=%u xiii2-mip-lod=%+.2f xiii2-shaders=%d xiii2-shadow-pipeline=%d",
        loaded.overrideLauncherGraphics, static_cast<unsigned>(loaded.displayMode), loaded.monitorIndex,
        loaded.width, loaded.height,
        loaded.shadowResolution, loaded.msaaSamples, loaded.highResolutionScissorFix,
        loaded.optimizeUiVertexBuffer, loaded.correctScreenSpaceAlignment,
        loaded.anisotropicFiltering, loaded.xiii2MipLodBias,
        loaded.xiii2ShaderCorrections,
        loaded.xiii2OptimizeShadowPipeline);
    return TRUE;
}

void EnsureLoaded() {
    InitOnceExecuteOnce(&g_loadOnce, &LoadSettings, nullptr, nullptr);
}

void AddSetting(std::vector<ini::Entry>& entries, const wchar_t* key,
                unsigned value) {
    entries.push_back({key, std::to_wstring(value)});
}

void AddSetting(std::vector<ini::Entry>& entries, const wchar_t* key,
                bool value) {
    entries.push_back({key, ini::FormatBoolean(value)});
}

void AddSetting(std::vector<ini::Entry>& entries, const wchar_t* key,
                unsigned value, std::span<const wchar_t* const> names) {
    entries.push_back({key, ini::FormatChoice(value, names)});
}

void AddSetting(std::vector<ini::Entry>& entries, const wchar_t* key,
                float value) {
    wchar_t text[32]{};
    _snwprintf_s(text, _countof(text), _TRUNCATE, L"%.2f", value);
    entries.push_back({key, text});
}

bool SaveConfig(const GraphicsSettings& value) {
    if (g_configPath.empty()) return false;
    const game::Title title = g_title.load(std::memory_order_acquire);
    if (title == game::Title::LightningReturns) return true;
    const bool ff13 = title == game::Title::FinalFantasyXIII;
    const bool ff13_2 = title == game::Title::FinalFantasyXIII2;
    const bool unknown = title == game::Title::Unknown;

    std::vector<ini::Entry> entries;
    entries.reserve(14);
    if (ff13 || ff13_2 || unknown) {
        AddSetting(entries, L"OverrideLauncherGraphics",
                   value.overrideLauncherGraphics);
        AddSetting(entries, L"DisplayMode",
                   static_cast<unsigned>(value.displayMode),
                   names::kDisplayMode);
        AddSetting(entries, L"Monitor", value.monitorIndex, names::kMonitor);
        AddSetting(entries, L"Width", value.width);
        AddSetting(entries, L"Height", value.height);
        AddSetting(entries, L"ShadowResolution", value.shadowResolution);
        AddSetting(entries, L"MSAA", value.msaaSamples);
        AddSetting(entries, L"OptimizeUiVertexBuffer",
                   value.optimizeUiVertexBuffer);
        AddSetting(entries, L"CorrectScreenSpaceAlignment",
                   value.correctScreenSpaceAlignment);
        AddSetting(entries, L"AnisotropicFiltering",
                   value.anisotropicFiltering);
    }
    if (ff13 || unknown) {
        AddSetting(entries, L"HighResolutionScissorFix",
                   value.highResolutionScissorFix);
    }
    if (ff13_2 || unknown) {
        AddSetting(entries, L"XIII2MipLodBias", value.xiii2MipLodBias);
        AddSetting(entries, L"XIII2ShaderCorrections",
                   value.xiii2ShaderCorrections);
        AddSetting(entries, L"XIII2OptimizeShadowPipeline",
                   value.xiii2OptimizeShadowPipeline);
    }
    return ini::WriteValues(g_configPath, L"Graphics", entries);
}

bool SynchronizeLauncher(const GraphicsSettings& value) {
    if (g_launcherPath.empty()) return false;
    std::string xml = ReadFile(g_launcherPath);
    if (xml.empty()) return false;

    const int windowMode = 0;
    const int windowResolution = value.width >= 1920 && value.height >= 1080 ? 1 : 0;
    bool complete = ReplaceXmlInteger(xml, "window_mode", windowMode);
    complete = ReplaceXmlInteger(xml, "window_resolution", windowResolution) && complete;
    complete = ReplaceXmlInteger(xml, "shadow_resolution", ShadowLauncherIndex(value.shadowResolution)) && complete;
    complete = ReplaceXmlInteger(xml, "mass", MsaaLauncherIndex(value.msaaSamples)) && complete;
    if (!complete) return false;

    const std::wstring temporary = g_launcherPath + L".novafix.tmp";
    FILE* file = nullptr;
    if (_wfopen_s(&file, temporary.c_str(), L"wb") != 0 || !file) return false;
    const bool written = std::fwrite(xml.data(), 1, xml.size(), file) == xml.size();
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), g_launcherPath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

} // namespace

void SetGraphicsTitle(game::Title title) {
    g_title.store(title, std::memory_order_release);
}

void SetLauncherIntegrationEnabled(bool enabled) {
    g_launcherIntegration.store(enabled, std::memory_order_release);
}

GraphicsSettings Active() {
    EnsureLoaded();
    thread_local std::uint64_t cachedGeneration{};
    thread_local GraphicsSettings cached{};
    const std::uint64_t generation =
        g_activeGeneration.load(std::memory_order_acquire);
    if (cachedGeneration == generation) return cached;

    AcquireSRWLockShared(&g_settingsLock);
    cached = g_active;
    cachedGeneration =
        g_activeGeneration.load(std::memory_order_relaxed);
    ReleaseSRWLockShared(&g_settingsLock);
    return cached;
}

GraphicsSettings Stored() {
    EnsureLoaded();
    AcquireSRWLockShared(&g_settingsLock);
    const GraphicsSettings value = g_stored;
    ReleaseSRWLockShared(&g_settingsLock);
    return value;
}

bool OverridesLauncherGraphics() {
    return Active().overrideLauncherGraphics;
}

void PreviewGraphics(const GraphicsSettings& requested) {
    EnsureLoaded();
    GraphicsSettings value = requested;
    Normalize(value);
    AcquireSRWLockExclusive(&g_settingsLock);
    ApplyLiveFields(g_active, value);
    g_activeGeneration.fetch_add(1, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_settingsLock);
}

bool CorrectScreenSpaceAlignmentEnabled() {
    return g_correctScreenSpaceAlignment.load(std::memory_order_acquire);
}

bool Save(const GraphicsSettings& requested) {
    EnsureLoaded();
    GraphicsSettings value = requested;
    Normalize(value);

    AcquireSRWLockShared(&g_settingsLock);
    const GraphicsSettings previous = g_stored;
    ReleaseSRWLockShared(&g_settingsLock);

    const bool configSaved = SaveConfig(value);
    const bool launcherSettingsChanged =
        !previous.overrideLauncherGraphics ||
        value.width != previous.width || value.height != previous.height ||
        value.shadowResolution != previous.shadowResolution ||
        value.msaaSamples != previous.msaaSamples;
    const bool synchronizeLauncher = configSaved &&
        value.overrideLauncherGraphics && launcherSettingsChanged &&
        g_launcherIntegration.load(std::memory_order_acquire);
    const bool officialSetupSynchronized =
        !synchronizeLauncher || SynchronizeLauncher(value);
    if (configSaved) {
        AcquireSRWLockExclusive(&g_settingsLock);
        g_stored = value;
        ApplyLiveFields(g_active, value);
        g_activeGeneration.fetch_add(1, std::memory_order_release);
        ReleaseSRWLockExclusive(&g_settingsLock);
    }
    Log("Graphics settings saved: config=%d launcher=%d override=%d mode=%u monitor=%u size=%ux%u shadow=%u MSAA=%u scissor-fix=%d ui-vb=%d screen-align=%d AF=%u xiii2-mip-lod=%+.2f xiii2-shaders=%d xiii2-shadow-pipeline=%d",
        configSaved, officialSetupSynchronized, value.overrideLauncherGraphics, static_cast<unsigned>(value.displayMode),
        value.monitorIndex, value.width, value.height, value.shadowResolution, value.msaaSamples,
        value.highResolutionScissorFix, value.optimizeUiVertexBuffer,
        value.correctScreenSpaceAlignment, value.anisotropicFiltering,
        value.xiii2MipLodBias,
        value.xiii2ShaderCorrections,
        value.xiii2OptimizeShadowPipeline);
    return configSaved;
}

int ShadowLauncherIndex(unsigned resolution) {
    constexpr unsigned values[]{512, 1024, 2048, 4096, 8192};
    int best = 0;
    for (int i = 1; i < 5; ++i) {
        const unsigned currentDistance = values[i] > resolution ? values[i] - resolution : resolution - values[i];
        const unsigned bestDistance = values[best] > resolution ? values[best] - resolution : resolution - values[best];
        if (currentDistance < bestDistance) best = i;
    }
    return best;
}

int MsaaLauncherIndex(unsigned samples) {
    constexpr unsigned values[]{2, 4, 8, 16};
    int best = 0;
    for (int i = 1; i < 4; ++i) {
        const unsigned currentDistance = values[i] > samples ? values[i] - samples : samples - values[i];
        const unsigned bestDistance = values[best] > samples ? values[best] - samples : samples - values[best];
        if (currentDistance < bestDistance) best = i;
    }
    return best;
}

} // namespace novafix::settings
