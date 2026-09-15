#pragma once

#include <cstdint>

namespace novafix::display {

inline constexpr float kAuthoredAspectRatio = 16.0f / 9.0f;

struct AspectRatio {
    std::uint32_t width{};
    std::uint32_t height{};
    float value{kAuthoredAspectRatio};
    float safeScaleX{1.0f};
    float safeScaleY{1.0f};
    bool valid{};
    bool correctionRequired{};
};

struct AspectFitScale {
    float x{1.0f};
    float y{1.0f};
    bool valid{};
    bool correctionRequired{};
};

AspectRatio ResolveAspectRatio(std::uint32_t width, std::uint32_t height);
AspectFitScale ResolveAspectFitScale(
    std::uint32_t contentWidth, std::uint32_t contentHeight,
    std::uint32_t targetWidth, std::uint32_t targetHeight);

} // namespace novafix::display
