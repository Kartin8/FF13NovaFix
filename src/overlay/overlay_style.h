#pragma once

#include <windows.h>

struct ImGuiIO;
struct ImFont;

namespace novafix::overlay::style {

float ResolveUiScale(HWND window);
void ConfigureFonts(ImGuiIO& io, float scale);
void ConfigureStyle(float scale);
ImFont* PluginTitleFont(const char* utf8Text);

} // namespace novafix::overlay::style
