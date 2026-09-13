#pragma once

#include <cstdint>

namespace novafix::game {
enum class Title : std::uint8_t;
}

namespace novafix::settings {

bool EnsureDefaultConfig(game::Title title);

} // namespace novafix::settings
