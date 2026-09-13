#include "display/scissor_transform.h"

#include <algorithm>
#include <cstdint>

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

} // namespace novafix::display
