#pragma once

#include <cstdint>

namespace novafix::game::xiii2_delta_guard {

// XIII-2's delta reader returns the same output pointer it receives.
// Keep all reads/writes behind this guard because the timing object can change while suspended, minimized or shutting down.
bool TryRead(const std::uint32_t* result,
             const std::uint32_t* expectedOutput,
             std::uint32_t& value) noexcept;

bool TryWrite(std::uint32_t* result,
              const std::uint32_t* expectedOutput,
              std::uint32_t value) noexcept;

} // namespace novafix::game::xiii2_delta_guard
