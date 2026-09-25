#pragma once

#include "display/aspect_ratio.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace novafix::game::xiii2_ui {

inline constexpr float kMinimapCenterX = 1058.0f;
inline constexpr float kMinimapCenterY = 144.0f;
inline constexpr float kMinimapRadiusX = 127.0f;
inline constexpr float kMinimapRadiusY = 95.25f;
inline constexpr float kRoundMinimapYScale = kMinimapRadiusX / kMinimapRadiusY;

inline bool MinimapHidden(float scale) { return scale == 0.0f; }

inline float MinimapCameraInverseAspect(
    const display::AspectRatio& aspect, float nativeInverseAspect) {
    return aspect.valid && aspect.correctionRequired
        ? 1.0f / display::kAuthoredAspectRatio
        : nativeInverseAspect;
}

struct MinimapLayout {
    float scaleX{1.0f};
    float scaleY{1.0f};
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
    const display::AspectRatio& aspect, float scale, bool round,
    float horizontalOffset, float verticalOffset) {
    if (!aspect.valid || !std::isfinite(scale) || scale <= 0.0f ||
        !std::isfinite(horizontalOffset) ||
        !std::isfinite(verticalOffset)) return {};
    const float scaleX = scale;
    const float scaleY = scale * (round ? kRoundMinimapYScale : 1.0f);
    const float radiusX = kMinimapRadiusX * aspect.safeScaleX / 640.0f;
    const float radiusY = kMinimapRadiusY * aspect.safeScaleY / 360.0f;
    const float pivotX = aspect.safeScaleX *
        (kMinimapCenterX / 640.0f - 1.0f);
    const float pivotY = aspect.safeScaleY *
        (1.0f - kMinimapCenterY / 360.0f);
    const float shiftX = 1.0f - aspect.safeScaleX -
        (scaleX - 1.0f) * radiusX +
        2.0f * horizontalOffset / static_cast<float>(aspect.width);
    const float shiftY = 1.0f - aspect.safeScaleY -
        (scaleY - 1.0f) * radiusY -
        2.0f * verticalOffset / static_cast<float>(aspect.height);
    return {scaleX, scaleY, pivotX, pivotY, shiftX, shiftY,
        scaleX != 1.0f || scaleY != 1.0f ||
        shiftX != 0.0f || shiftY != 0.0f};
}

inline MinimapOffsetLimits ResolveMinimapOffsetLimits(
    const display::AspectRatio& aspect, float scale, bool round,
    float marginPx = 8.0f) {
    if (!aspect.valid || !std::isfinite(scale) || scale <= 0.0f ||
        !std::isfinite(marginPx) || marginPx < 0.0f) return {};

    const auto layout = ResolveMinimapLayout(aspect, scale, round, 0.0f, 0.0f);
    const float halfWidth = static_cast<float>(aspect.width) * 0.5f;
    const float halfHeight = static_cast<float>(aspect.height) * 0.5f;
    const float centerX = (1.0f + layout.pivotX + layout.shiftX) * halfWidth;
    const float centerY = (1.0f - layout.pivotY - layout.shiftY) * halfHeight;
    const float radiusX = layout.scaleX * kMinimapRadiusX *
        aspect.safeScaleX * halfWidth / 640.0f;
    const float radiusY = layout.scaleY * kMinimapRadiusY *
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
    const display::AspectRatio& aspect) {
    const auto smallOval = ResolveMinimapOffsetLimits(aspect, 0.5f, false);
    const auto largeOval = ResolveMinimapOffsetLimits(aspect, 1.5f, false);
    const auto smallRound = ResolveMinimapOffsetLimits(aspect, 0.5f, true);
    const auto largeRound = ResolveMinimapOffsetLimits(aspect, 1.5f, true);
    if (!smallOval.valid || !largeOval.valid ||
        !smallRound.valid || !largeRound.valid) return {};
    return {
        std::min({smallOval.minX, largeOval.minX, smallRound.minX, largeRound.minX}),
        std::max({smallOval.maxX, largeOval.maxX, smallRound.maxX, largeRound.maxX}),
        std::min({smallOval.minY, largeOval.minY, smallRound.minY, largeRound.minY}),
        std::max({smallOval.maxY, largeOval.maxY, smallRound.maxY, largeRound.maxY}),
        true};
}

inline MinimapLayout ResolveVisibleMinimapLayout(
    const display::AspectRatio& aspect, float scale, bool round,
    float horizontalOffset, float verticalOffset) {
    if (aspect.valid && !aspect.correctionRequired && !round &&
        scale == 1.0f && horizontalOffset == 0.0f &&
        verticalOffset == 0.0f) return {};
    const auto limits = ResolveMinimapOffsetLimits(aspect, scale, round);
    if (limits.valid) {
        horizontalOffset = std::clamp(horizontalOffset, limits.minX, limits.maxX);
        verticalOffset = std::clamp(verticalOffset, limits.minY, limits.maxY);
    }
    return ResolveMinimapLayout(aspect, scale, round,
                                horizontalOffset, verticalOffset);
}

inline constexpr std::array<float, 2> MinimapClipTranslation(
    const MinimapLayout& layout) {
    return {(1.0f - layout.scaleX) * layout.pivotX + layout.shiftX,
            (1.0f - layout.scaleY) * layout.pivotY + layout.shiftY};
}

inline void ApplyMinimapLayout(std::array<float, 16>& matrix,
                               const MinimapLayout& layout) {
    const auto [translateX, translateY] = MinimapClipTranslation(layout);
    for (std::size_t column = 0; column < 4; ++column) {
        const std::size_t index = column * 4;
        const float w = matrix[index + 3];
        matrix[index] = layout.scaleX * matrix[index] + translateX * w;
        matrix[index + 1] = layout.scaleY * matrix[index + 1] +
            translateY * w;
    }
}

// NaviMap's pixel shader clips TEXCOORD2 against a hard-coded circle.
// TEXCOORD2 carries homogeneous clip XY, so restore the pre-layout XY there while leaving POSITION transformed.
inline std::array<float, 4> MinimapClipCoordinateInverse(
    const display::AspectRatio& aspect, const MinimapLayout& layout) {
    const float scaleX = aspect.safeScaleX * layout.scaleX;
    const float scaleY = aspect.safeScaleY * layout.scaleY;
    const auto [translateX, translateY] = MinimapClipTranslation(layout);
    return {1.0f / scaleX, 1.0f / scaleY,
            -translateX / scaleX, -translateY / scaleY};
}

// The clip shader evaluates 0.75 * TEXCOORD2.y - 513.75, giving a native center of 685.
// Round mode stretches NaviMap visuals 4/3 vertically, so undo that only for the map fill and widen its shader clip by the same factor.
inline std::array<float, 4> MinimapFillAndClipY(
    const MinimapLayout& layout, bool round) {
    if (!round) return {1.0f, 0.0f, 1.0f, 0.0f};
    constexpr float nativeClipCenterY = 513.75f / 0.75f;
    constexpr float inverseRound = 1.0f / kRoundMinimapYScale;
    return {inverseRound,
            (1.0f - inverseRound) * (layout.pivotY + layout.shiftY),
            inverseRound,
            (1.0f - inverseRound) * nativeClipCenterY};
}

} // namespace novafix::game::xiii2_ui
