#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace novafix::game::ff13_motion_blend_policy {

constexpr std::uint64_t kRampDurationMilliseconds = 200u;
constexpr float kMinimumRampWeight = 0.025f;

inline bool IsFieldLocomotionResource(std::string_view name) {
    const bool fieldFamily =
        name.find("z_idle") != std::string_view::npos ||
        name.find("z_walk") != std::string_view::npos ||
        name.find("z_run") != std::string_view::npos ||
        name.find("z_turn") != std::string_view::npos;
    if (fieldFamily) return true;

    // Some enemies switch from their field-prefixed resources to the generic
    // skeleton locomotion set while they are still visible in the field. The
    // native names start with a skeleton id (s1/s2), optionally followed by
    // the x_ field-enemy namespace. Keep action resources such as summon,
    // attack, appear and encounter outside this positive whitelist
    if (name.size() < 3u || name[0] != 's' ||
        name[1] < '0' || name[1] > '9') {
        return false;
    }
    std::string_view suffix = name.substr(2u);
    if (suffix.starts_with("x_")) suffix.remove_prefix(2u);
    return suffix.starts_with("idle") || suffix.starts_with("walk") ||
        suffix.starts_with("run") || suffix.starts_with("turn");
}

inline float ScaleActivatedWeight(float nativeWeight,
                                  std::uint64_t elapsedMilliseconds,
                                  bool hasActivation) {
    if (!hasActivation || !std::isfinite(nativeWeight) ||
        nativeWeight <= 0.0f ||
        elapsedMilliseconds >= kRampDurationMilliseconds) {
        return nativeWeight;
    }

    const float progress = static_cast<float>(elapsedMilliseconds) /
        static_cast<float>(kRampDurationMilliseconds);
    return nativeWeight * std::max(progress, kMinimumRampWeight);
}

} // namespace novafix::game::ff13_motion_blend_policy
