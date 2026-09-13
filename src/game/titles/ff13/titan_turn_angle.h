#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace novafix::game::ff13_titan_route_correction {

inline float ComputeYawDeadZone(float nativeThreshold,
                                float effectiveDelta,
                                bool highFps) {
    constexpr float kNativeFrameSeconds = 1.0f / 30.0f;
    if (!highFps || !std::isfinite(nativeThreshold) ||
        !std::isfinite(effectiveDelta) || nativeThreshold <= 0.0f ||
        effectiveDelta <= 0.0f) {
        return nativeThreshold;
    }
    const float ratio = (std::min)(1.0f,
                                   effectiveDelta / kNativeFrameSeconds);
    return nativeThreshold * ratio;
}

// The native acos(dot / lengths) loses small turns when its float quotient
// rounds to 1. Keep the same signed, planar angle without forming that quotient
inline bool TryComputeTurnAngle(const std::array<float, 3>& desired,
                                const std::array<float, 3>& current,
                                const std::array<float, 3>& up,
                                float& angle) {
    for (std::size_t i = 0; i < 3; ++i) {
        if (!std::isfinite(desired[i]) || !std::isfinite(current[i]) ||
            !std::isfinite(up[i])) {
            return false;
        }
    }

    using Vector = std::array<double, 3>;
    const auto cross = [](const Vector& a, const Vector& b) -> Vector {
        return {a[1] * b[2] - a[2] * b[1],
                a[2] * b[0] - a[0] * b[2],
                a[0] * b[1] - a[1] * b[0]};
    };
    const auto dot = [](const Vector& a, const Vector& b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };

    const Vector axis{up[0], up[1], up[2]};
    // Crossing both vectors with up rotates their planar projections by the
    // same 90 degrees. This also avoids cancellation in v - up * dot(v, up)
    const Vector from = cross(axis, {current[0], current[1], current[2]});
    const Vector to = cross(axis, {desired[0], desired[1], desired[2]});
    const double axisSquared = dot(axis, axis);
    if (axisSquared <= 0.0 || dot(from, from) <= 0.0 || dot(to, to) <= 0.0) {
        return false; // Leave degenerate inputs to the original helper.
    }

    const double sine = dot(cross(from, to), axis) / std::sqrt(axisSquared);
    const double cosine = dot(from, to);
    const double magnitude = std::atan2(std::fabs(sine), cosine);
    // Native uses crossDot < 0: an exact half-turn, including -0, chooses +pi
    const float result = static_cast<float>(sine < 0.0 ? -magnitude : magnitude);
    if (!std::isfinite(result)) return false;
    angle = result;
    return true;
}

} // namespace novafix::game::ff13_titan_route_correction
