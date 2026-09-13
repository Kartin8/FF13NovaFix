#pragma once

#include <cstdint>

namespace novafix::game {

enum class ReleaseChannel : std::uint8_t {
    Unknown,
    Steam,
    MicrosoftStore,
};

ReleaseChannel DetectReleaseChannel();
const char* ReleaseChannelName(ReleaseChannel channel);

} // namespace novafix::game
