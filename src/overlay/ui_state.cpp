#include "overlay/ui_state.h"

#include "game/core/game_profile.h"
#include "settings/ini_value.h"
#include "settings/input_settings.h"

#include <windows.h>
#include <shlobj.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace novafix::ui::state {
namespace {

INIT_ONCE g_pathOnce = INIT_ONCE_STATIC_INIT;
std::wstring g_statePath;
std::string g_imguiPath;
std::unordered_map<std::wstring, bool> g_openHeaders;
std::mutex g_geometryMutex;
std::once_flag g_geometryOnce;

struct StoredGeometry {
    unsigned x{};
    unsigned y{};
    unsigned width{};
    unsigned height{};
    bool operator==(const StoredGeometry&) const = default;
};

std::optional<StoredGeometry> g_savedGeometry;
std::optional<StoredGeometry> g_observedGeometry;

const wchar_t* TitleDirectory(game::Title title) {
    switch (title) {
    case game::Title::FinalFantasyXIII: return L"FFXIII";
    case game::Title::FinalFantasyXIII2: return L"FFXIII2";
    case game::Title::LightningReturns: return L"LightningReturns";
    case game::Title::Unknown: return L"Unknown";
    }
    return L"Unknown";
}

std::wstring LocalAppData() {
    for (const KNOWNFOLDERID* folder :
         {&FOLDERID_LocalAppData, &FOLDERID_RoamingAppData}) {
        PWSTR knownPath{};
        if (SUCCEEDED(SHGetKnownFolderPath(
                *folder, 0, nullptr, &knownPath)) && knownPath) {
            std::wstring path(knownPath);
            CoTaskMemFree(knownPath);
            return path;
        }
        if (knownPath) CoTaskMemFree(knownPath);
    }

    std::array<wchar_t, 32768> fallback{};
    for (const wchar_t* variable : {L"LOCALAPPDATA", L"APPDATA"}) {
        const DWORD length = GetEnvironmentVariableW(
            variable, fallback.data(),
            static_cast<DWORD>(fallback.size()));
        if (length > 0 && length < fallback.size()) {
            return std::wstring(fallback.data(), length);
        }
    }
    return {};
}

bool EnsureDirectory(const std::wstring& path) {
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::string Utf8(const std::wstring& wide) {
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string utf8(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, 0, wide.c_str(), -1, utf8.data(), size,
            nullptr, nullptr) != size) {
        return {};
    }
    utf8.pop_back();
    return utf8;
}

BOOL CALLBACK ResolvePaths(PINIT_ONCE, PVOID, PVOID*) {
    const std::wstring base = LocalAppData();
    if (base.empty()) return TRUE;
    const std::wstring appDirectory = base + L"\\NovaFix";
    const std::wstring gameDirectory = appDirectory + L"\\" +
        TitleDirectory(game::CurrentProfile().title);
    if (!EnsureDirectory(appDirectory) ||
        !EnsureDirectory(gameDirectory)) {
        return TRUE;
    }
    g_imguiPath = Utf8(gameDirectory + L"\\imgui.ini");
    g_statePath = gameDirectory + L"\\state.ini";
    return TRUE;
}

void EnsurePaths() {
    InitOnceExecuteOnce(&g_pathOnce, &ResolvePaths, nullptr, nullptr);
}

unsigned LogicalPixels(float pixels, float scale) {
    return static_cast<unsigned>(std::lround(std::clamp(
        pixels / std::max(scale, 0.01f), 0.0f, 32767.0f)));
}

void LoadGeometry() {
    std::call_once(g_geometryOnce, [] {
        EnsurePaths();
        if (!settings::ini::ReadBoolean(
                g_statePath, L"Overlay", L"WindowSaved", false)) {
            return;
        }
        StoredGeometry geometry{
            settings::ini::ReadUnsigned(
                g_statePath, L"Overlay", L"WindowX", 0u),
            settings::ini::ReadUnsigned(
                g_statePath, L"Overlay", L"WindowY", 0u),
            settings::ini::ReadUnsigned(
                g_statePath, L"Overlay", L"WindowWidth", 0u),
            settings::ini::ReadUnsigned(
                g_statePath, L"Overlay", L"WindowHeight", 0u),
        };
        if (geometry.x <= 32767u && geometry.y <= 32767u &&
            geometry.width >= 64u && geometry.width <= 32767u &&
            geometry.height >= 64u && geometry.height <= 32767u) {
            std::scoped_lock lock(g_geometryMutex);
            g_savedGeometry = geometry;
        }
    });
}

} // namespace

const char* ImGuiIniPath() {
    if (!settings::ActiveInput().rememberOverlayState) return nullptr;
    EnsurePaths();
    return g_imguiPath.empty() ? nullptr : g_imguiPath.c_str();
}

std::optional<WindowGeometry> SavedWindowGeometry(float uiScale) {
    if (!settings::ActiveInput().rememberOverlayState) return std::nullopt;
    LoadGeometry();
    std::scoped_lock lock(g_geometryMutex);
    if (!g_savedGeometry) return std::nullopt;
    const float scale = std::max(uiScale, 0.01f);
    return WindowGeometry{
        {g_savedGeometry->x * scale, g_savedGeometry->y * scale},
        {g_savedGeometry->width * scale, g_savedGeometry->height * scale},
    };
}

void ObserveWindowGeometry(ImVec2 position, ImVec2 size, float uiScale) {
    if (!settings::ActiveInput().rememberOverlayState) return;
    LoadGeometry();
    const StoredGeometry geometry{
        LogicalPixels(position.x, uiScale),
        LogicalPixels(position.y, uiScale),
        LogicalPixels(size.x, uiScale),
        LogicalPixels(size.y, uiScale),
    };
    std::scoped_lock lock(g_geometryMutex);
    g_observedGeometry = geometry;
}

void SaveObservedWindowGeometry() {
    if (!settings::ActiveInput().rememberOverlayState) return;
    LoadGeometry();
    std::optional<StoredGeometry> geometry;
    {
        std::scoped_lock lock(g_geometryMutex);
        if (!g_observedGeometry || g_observedGeometry == g_savedGeometry) {
            return;
        }
        geometry = g_observedGeometry;
    }
    const std::array entries{
        settings::ini::Entry{L"WindowX", std::to_wstring(geometry->x)},
        settings::ini::Entry{L"WindowY", std::to_wstring(geometry->y)},
        settings::ini::Entry{L"WindowWidth", std::to_wstring(geometry->width)},
        settings::ini::Entry{L"WindowHeight", std::to_wstring(geometry->height)},
        settings::ini::Entry{L"WindowSaved", L"On"},
    };
    if (settings::ini::WriteValues(g_statePath, L"Overlay", entries)) {
        std::scoped_lock lock(g_geometryMutex);
        g_savedGeometry = geometry;
    }
}

unsigned SelectedTab() {
    if (!settings::ActiveInput().rememberOverlayState) return 0u;
    EnsurePaths();
    return settings::ini::ReadUnsigned(
        g_statePath, L"Overlay", L"SelectedTab", 0u);
}

void SaveSelectedTab(unsigned tab) {
    if (!settings::ActiveInput().rememberOverlayState) return;
    EnsurePaths();
    const std::array entries{
        settings::ini::Entry{L"SelectedTab", std::to_wstring(tab)},
    };
    settings::ini::WriteValues(g_statePath, L"Overlay", entries);
}

bool CollapsingHeader(const char* label, const wchar_t* key,
                      ImGuiTreeNodeFlags flags) {
    if (!settings::ActiveInput().rememberOverlayState) {
        return ImGui::CollapsingHeader(label, flags);
    }
    EnsurePaths();
    const std::wstring stateKey(key);
    auto [entry, inserted] = g_openHeaders.try_emplace(stateKey);
    if (inserted) {
        entry->second = settings::ini::ReadBoolean(
            g_statePath, L"Overlay", key,
            (flags & ImGuiTreeNodeFlags_DefaultOpen) != 0);
    }
    ImGui::SetNextItemOpen(entry->second, ImGuiCond_Once);
    const bool open = ImGui::CollapsingHeader(label, flags);
    if (ImGui::IsItemToggledOpen()) {
        entry->second = open;
        const std::array entries{
            settings::ini::Entry{key, settings::ini::FormatBoolean(open)},
        };
        settings::ini::WriteValues(g_statePath, L"Overlay", entries);
    }
    return open;
}

} // namespace novafix::ui::state
