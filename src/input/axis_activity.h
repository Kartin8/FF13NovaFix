#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace novafix::input {

class AxisActivityTracker {
public:
    static constexpr std::size_t kMaxAxes = 8;

    bool Observe(std::span<const std::int32_t> axes);
    void Reset();

private:
    void Calibrate(std::span<const std::int32_t> axes);

    std::array<std::int32_t, kMaxAxes> neutral_{};
    std::array<std::int32_t, kMaxAxes> lastReported_{};
    std::array<bool, kMaxAxes> engaged_{};
    std::size_t axisCount_{};
    std::int64_t activationDeadzone_{};
    std::int64_t releaseDeadzone_{};
    std::int64_t movementStep_{};
    bool ready_{};
};

} // namespace novafix::input
