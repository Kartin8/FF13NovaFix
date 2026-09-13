#pragma once

#include <cstdint>

namespace novafix::game::xiii2_delta_guard {

// The native XIII-2 delta reader returns the output argument it was given
// Keep all reads and writes behind this guard because the timing object can be
// replaced while the game is suspended, minimized, or shutting down
bool TryRead(const std::uint32_t* result,
             const std::uint32_t* expectedOutput,
             std::uint32_t& value) noexcept;

bool TryWrite(std::uint32_t* result,
              const std::uint32_t* expectedOutput,
              std::uint32_t value) noexcept;

} // namespace novafix::game::xiii2_delta_guard
