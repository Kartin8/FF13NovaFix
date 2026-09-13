#pragma once

#include <windows.h>

struct ImGuiIO;

namespace novafix::overlay::style {

float ResolveUiScale(HWND window);
void ConfigureFonts(ImGuiIO& io, float scale);
void ConfigureStyle(float scale);

} // namespace novafix::overlay::style
