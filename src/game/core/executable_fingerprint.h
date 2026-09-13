#pragma once

#include "game/core/image_view.h"

#include <optional>
#include <string_view>

namespace novafix::game {

 // Produces a stable on-disk PE identity. The .text hash covers raw section bytes
 // plus virtual zero padding, so ASLR and the image's mapped base cannot affect it
std::optional<ImageFingerprint> FingerprintExecutableFile(
    std::wstring_view filePath);

} // namespace novafix::game
