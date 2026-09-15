#include "display/scissor_transform.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace novafix::display {
namespace {

constexpr std::int32_t kReferenceWidth = 1280;
constexpr std::int32_t kReferenceHeight = 720;

std::int32_t ScaleLeading(std::int32_t value, std::uint32_t target,
                          std::int32_t reference) {
    value = std::clamp(value, 0, reference);
    return static_cast<std::int32_t>(
        static_cast<std::int64_t>(value) * target / reference);
}

std::int32_t ScaleTrailing(std::int32_t value, std::uint32_t target,
                           std::int32_t reference) {
    value = std::clamp(value, 0, reference);
    const std::int64_t product = static_cast<std::int64_t>(value) * target;
    return static_cast<std::int32_t>((product + reference - 1) / reference);
}

std::int32_t QuantizeSafeAreaEdge(double value, bool trailing) {
    constexpr double kIntegerSnapEpsilon = 0.0000001;
    const double rounded = trailing
        ? std::ceil(value - kIntegerSnapEpsilon)
        : std::floor(value + kIntegerSnapEpsilon);
    return static_cast<std::int32_t>(std::clamp(
        rounded,
        static_cast<double>(std::numeric_limits<std::int32_t>::min()),
        static_cast<double>(std::numeric_limits<std::int32_t>::max())));
}

} // namespace

ScissorRect ScaleScissorRect(ScissorRect source, std::uint32_t targetWidth,
                             std::uint32_t targetHeight) {
    if (targetWidth == 0 || targetHeight == 0) return source;
    ScissorRect result{
        ScaleLeading(source.left, targetWidth, kReferenceWidth),
        ScaleLeading(source.top, targetHeight, kReferenceHeight),
        ScaleTrailing(source.right, targetWidth, kReferenceWidth),
        ScaleTrailing(source.bottom, targetHeight, kReferenceHeight),
    };
    result.right = std::max(result.left, result.right);
    result.bottom = std::max(result.top, result.bottom);
    return result;
}

ScissorRect FitScissorRectToSafeArea(
    ScissorRect source, const AspectRatio& aspect) {
    if (!aspect.valid || !aspect.correctionRequired ||
        source.right < source.left || source.bottom < source.top) {
        return source;
    }

    const std::uint64_t widthProduct =
        static_cast<std::uint64_t>(aspect.width) * 9u;
    const std::uint64_t heightProduct =
        static_cast<std::uint64_t>(aspect.height) * 16u;
    const double scaleX = widthProduct > heightProduct
        ? static_cast<double>(heightProduct) /
              static_cast<double>(widthProduct)
        : 1.0;
    const double scaleY = widthProduct < heightProduct
        ? static_cast<double>(widthProduct) /
              static_cast<double>(heightProduct)
        : 1.0;
    const double offsetX = static_cast<double>(aspect.width) *
        (1.0 - scaleX) * 0.5;
    const double offsetY = static_cast<double>(aspect.height) *
        (1.0 - scaleY) * 0.5;
    return {
        QuantizeSafeAreaEdge(source.left * scaleX + offsetX, false),
        QuantizeSafeAreaEdge(source.top * scaleY + offsetY, false),
        QuantizeSafeAreaEdge(source.right * scaleX + offsetX, true),
        QuantizeSafeAreaEdge(source.bottom * scaleY + offsetY, true),
    };
}

} // namespace novafix::display
