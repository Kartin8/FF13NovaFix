#pragma once

#include "game/core/image_view.h"

#include <optional>
#include <string_view>

namespace novafix::game {

// Build a stable PE fingerprint from the file on disk.
// The .text hash includes raw bytes plus virtual zero padding, so ASLR/base address don't affect it.
std::optional<ImageFingerprint> FingerprintExecutableFile(
    std::wstring_view filePath);

} // namespace novafix::game
