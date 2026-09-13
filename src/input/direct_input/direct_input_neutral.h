#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace novafix::input {

constexpr std::size_t kDirectInputAxisCount = 8;
using DirectInputNeutralAxes = std::array<std::int32_t, kDirectInputAxisCount>;

bool NeutralizeControllerState(void* data, std::size_t size,
                               const DirectInputNeutralAxes& calibratedAxes,
                               bool calibrated);

} // namespace novafix::input
