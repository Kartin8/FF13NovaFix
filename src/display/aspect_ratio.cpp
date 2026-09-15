#include "display/aspect_ratio.h"

namespace novafix::display {

AspectRatio ResolveAspectRatio(std::uint32_t width, std::uint32_t height) {
    AspectRatio result{};
    result.width = width;
    result.height = height;
    if (width == 0u || height == 0u) return result;

    result.value = static_cast<float>(width) / static_cast<float>(height);
    result.valid = true;
    const std::uint64_t widthProduct =
        static_cast<std::uint64_t>(width) * 9u;
    const std::uint64_t heightProduct =
        static_cast<std::uint64_t>(height) * 16u;
    result.correctionRequired = widthProduct != heightProduct;
    if (widthProduct > heightProduct) {
        result.safeScaleX = kAuthoredAspectRatio / result.value;
    } else if (widthProduct < heightProduct) {
        result.safeScaleY = result.value / kAuthoredAspectRatio;
    }
    return result;
}

AspectFitScale ResolveAspectFitScale(
    std::uint32_t contentWidth, std::uint32_t contentHeight,
    std::uint32_t targetWidth, std::uint32_t targetHeight) {
    AspectFitScale result{};
    if (contentWidth == 0u || contentHeight == 0u ||
        targetWidth == 0u || targetHeight == 0u) {
        return result;
    }

    result.valid = true;
    const std::uint64_t targetProduct =
        static_cast<std::uint64_t>(targetWidth) * contentHeight;
    const std::uint64_t contentProduct =
        static_cast<std::uint64_t>(contentWidth) * targetHeight;
    result.correctionRequired = targetProduct != contentProduct;
    if (targetProduct > contentProduct) {
        result.x = static_cast<float>(contentProduct) /
                   static_cast<float>(targetProduct);
    } else if (targetProduct < contentProduct) {
        result.y = static_cast<float>(targetProduct) /
                   static_cast<float>(contentProduct);
    }
    return result;
}

} // namespace novafix::display
