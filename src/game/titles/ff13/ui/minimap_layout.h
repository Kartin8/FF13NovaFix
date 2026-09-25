#pragma once

#include "display/aspect_ratio.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace novafix::game::ff13_ui {

inline constexpr float kMinimapCanvasRadius = 152.0f;
inline constexpr float kMinimapDefaultCenterX = 1058.0f;
inline constexpr float kMinimapDefaultCenterY = 184.0f;

// A05290 subtracts this amount from the map-camera Y at the native 45-degree
// tilt. Round mode uses 0 degrees, where that subtraction becomes zero.
inline float NativeMinimapTiltCenterLift() {
    constexpr float nativeTiltRadians = 45.0f * 3.1415927f / 180.0f;
    const float cosine = std::fabs(std::cos(nativeTiltRadians));
    const int doubledLift = static_cast<int>(
        (1.0f - cosine) * (2.0f * kMinimapCanvasRadius));
    return static_cast<float>(doubledLift >> 1);
}

inline bool MinimapHidden(float scale) { return scale == 0.0f; }

struct MinimapLayout {
    float scale{1.0f};
    float pivotX{};
    float pivotY{};
    float shiftX{};
    float shiftY{};
    bool changed{};
};

struct MinimapOffsetLimits {
    float minX{};
    float maxX{};
    float minY{};
    float maxY{};
    bool valid{};
};

inline MinimapLayout ResolveMinimapLayout(
    const display::AspectRatio& aspect, float scale,
    float horizontalOffset, float verticalOffset,
    float authoredCenterX, float authoredCenterY) {
    if (!aspect.valid || !std::isfinite(scale) || scale <= 0.0f ||
        !std::isfinite(horizontalOffset) || !std::isfinite(verticalOffset) ||
        !std::isfinite(authoredCenterX) ||
        !std::isfinite(authoredCenterY)) return {};

    const float radiusX = kMinimapCanvasRadius * aspect.safeScaleX / 640.0f;
    const float radiusY = kMinimapCanvasRadius * aspect.safeScaleY / 360.0f;
    const float pivotX = aspect.safeScaleX *
        (authoredCenterX / 640.0f - 1.0f);
    const float pivotY = aspect.safeScaleY *
        (1.0f - authoredCenterY / 360.0f);
    const float shiftX = 1.0f - aspect.safeScaleX -
        (scale - 1.0f) * radiusX +
        2.0f * horizontalOffset / static_cast<float>(aspect.width);
    const float shiftY = 1.0f - aspect.safeScaleY -
        (scale - 1.0f) * radiusY -
        2.0f * verticalOffset / static_cast<float>(aspect.height);
    return {scale, pivotX, pivotY, shiftX, shiftY,
        scale != 1.0f || shiftX != 0.0f || shiftY != 0.0f};
}

inline MinimapOffsetLimits ResolveMinimapOffsetLimits(
    const display::AspectRatio& aspect, float scale,
    float authoredCenterX, float authoredCenterY,
    float marginPx = 8.0f) {
    if (!aspect.valid || !std::isfinite(scale) || scale <= 0.0f ||
        !std::isfinite(marginPx) || marginPx < 0.0f) return {};

    const auto layout = ResolveMinimapLayout(
        aspect, scale, 0.0f, 0.0f, authoredCenterX, authoredCenterY);
    const float halfWidth = static_cast<float>(aspect.width) * 0.5f;
    const float halfHeight = static_cast<float>(aspect.height) * 0.5f;
    const float centerX = (1.0f + layout.pivotX + layout.shiftX) * halfWidth;
    const float centerY = (1.0f - layout.pivotY - layout.shiftY) * halfHeight;
    const float radiusX = scale * kMinimapCanvasRadius *
        aspect.safeScaleX * halfWidth / 640.0f;
    const float radiusY = scale * kMinimapCanvasRadius *
        aspect.safeScaleY * halfHeight / 360.0f;
    const float minX = std::ceil(marginPx + radiusX - centerX);
    const float maxX = std::floor(
        static_cast<float>(aspect.width) - marginPx - radiusX - centerX);
    const float minY = std::ceil(marginPx + radiusY - centerY);
    const float maxY = std::floor(
        static_cast<float>(aspect.height) - marginPx - radiusY - centerY);
    if (!std::isfinite(minX) || !std::isfinite(maxX) ||
        !std::isfinite(minY) || !std::isfinite(maxY) ||
        minX > maxX || minY > maxY) return {};
    return {minX, maxX, minY, maxY, true};
}

inline MinimapOffsetLimits ResolveMinimapSliderLimits(
    const display::AspectRatio& aspect,
    float authoredCenterX, float authoredCenterY) {
    const auto smallest = ResolveMinimapOffsetLimits(
        aspect, 0.5f, authoredCenterX, authoredCenterY);
    const auto largest = ResolveMinimapOffsetLimits(
        aspect, 1.5f, authoredCenterX, authoredCenterY);
    if (!smallest.valid || !largest.valid) return {};
    return {
        std::min(smallest.minX, largest.minX),
        std::max(smallest.maxX, largest.maxX),
        std::min(smallest.minY, largest.minY),
        std::max(smallest.maxY, largest.maxY),
        true};
}

inline MinimapLayout ResolveVisibleMinimapLayout(
    const display::AspectRatio& aspect, float scale,
    float horizontalOffset, float verticalOffset,
    float authoredCenterX, float authoredCenterY) {
    if (aspect.valid && !aspect.correctionRequired && scale == 1.0f &&
        horizontalOffset == 0.0f && verticalOffset == 0.0f) return {};
    const auto limits = ResolveMinimapOffsetLimits(
        aspect, scale, authoredCenterX, authoredCenterY);
    if (limits.valid) {
        horizontalOffset = std::clamp(
            horizontalOffset, limits.minX, limits.maxX);
        verticalOffset = std::clamp(
            verticalOffset, limits.minY, limits.maxY);
    }
    return ResolveMinimapLayout(aspect, scale, horizontalOffset,
                                verticalOffset, authoredCenterX,
                                authoredCenterY);
}

inline constexpr std::array<float, 2> MinimapClipTranslation(
    const MinimapLayout& layout) {
    return {(1.0f - layout.scale) * layout.pivotX + layout.shiftX,
            (1.0f - layout.scale) * layout.pivotY + layout.shiftY};
}

inline void ApplyMinimapLayout(std::array<float, 16>& matrix,
                               const MinimapLayout& layout) {
    const auto [translateX, translateY] = MinimapClipTranslation(layout);
    for (std::size_t column = 0; column < 4; ++column) {
        const std::size_t index = column * 4;
        const float w = matrix[index + 3];
        matrix[index] = layout.scale * matrix[index] + translateX * w;
        matrix[index + 1] = layout.scale * matrix[index + 1] +
            translateY * w;
    }
}

} // namespace novafix::game::ff13_ui
