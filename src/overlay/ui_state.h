#pragma once

#include "imgui.h"

#include <optional>

namespace novafix::ui::state {

// Returns a stable UTF-8 path in the user's local application data directory.
// A null result leaves Dear ImGui's disk persistence disabled.
const char* ImGuiIniPath();

struct WindowGeometry {
    ImVec2 position{};
    ImVec2 size{};
};

std::optional<WindowGeometry> SavedWindowGeometry(float uiScale);
void ObserveWindowGeometry(ImVec2 position, ImVec2 size, float uiScale);
// Uses only our cached geometry; safe from the window-message shutdown path.
void SaveObservedWindowGeometry();

unsigned SelectedTab();
void SaveSelectedTab(unsigned tab);
bool CollapsingHeader(const char* label, const wchar_t* key,
                      ImGuiTreeNodeFlags flags = 0);

} // namespace novafix::ui::state
