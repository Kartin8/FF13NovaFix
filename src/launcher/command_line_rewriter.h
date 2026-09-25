#pragma once

#include "settings/game_settings.h"
#include "settings/graphics_settings.h"

#include <string>
#include <string_view>

namespace novafix::launcher {

std::string RewriteLaunchOptions(
    std::string_view commandLine,
    const settings::GraphicsSettings& graphics,
    const settings::GameSettings& gameSettings,
    unsigned fullscreenWidth, unsigned fullscreenHeight,
    bool rewriteGraphics, bool rewriteLanguages);
std::wstring RewriteLaunchOptions(
    std::wstring_view commandLine,
    const settings::GraphicsSettings& graphics,
    const settings::GameSettings& gameSettings,
    unsigned fullscreenWidth, unsigned fullscreenHeight,
    bool rewriteGraphics, bool rewriteLanguages);

std::string RewriteGraphicsOptions(std::string_view commandLine,
                                   const settings::GraphicsSettings& graphics,
                                   unsigned fullscreenWidth,
                                   unsigned fullscreenHeight);
std::wstring RewriteGraphicsOptions(std::wstring_view commandLine,
                                    const settings::GraphicsSettings& graphics,
                                    unsigned fullscreenWidth,
                                    unsigned fullscreenHeight);

} // namespace novafix::launcher
