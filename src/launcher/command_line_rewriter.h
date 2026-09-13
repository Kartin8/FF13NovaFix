#pragma once

#include "settings/graphics_settings.h"

#include <string>
#include <string_view>

namespace novafix::launcher {

std::string RewriteGraphicsOptions(std::string_view commandLine,
                                   const settings::GraphicsSettings& graphics,
                                   unsigned fullscreenWidth,
                                   unsigned fullscreenHeight);
std::wstring RewriteGraphicsOptions(std::wstring_view commandLine,
                                    const settings::GraphicsSettings& graphics,
                                    unsigned fullscreenWidth,
                                    unsigned fullscreenHeight);

} // namespace novafix::launcher
