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

bool IsSixteenTenAspect(std::uint32_t width, std::uint32_t height) {
    return width != 0u && height != 0u &&
        static_cast<std::uint64_t>(width) * 10u ==
            static_cast<std::uint64_t>(height) * 16u;
}

AspectFitScale ResolveWideMovieScale(
    std::uint32_t targetWidth, std::uint32_t targetHeight) {
    AspectFitScale result{};
    const AspectRatio target = ResolveAspectRatio(targetWidth, targetHeight);
    if (!target.valid) return result;

    result.valid = true;
    // At exactly 16:10, preserve the authored 16:9 proportions by cropping
    // the sides rather than stretching the movie across the taller target.
    if (IsSixteenTenAspect(targetWidth, targetHeight)) {
        result.x = kAuthoredAspectRatio / target.value;
        result.correctionRequired = true;
        return result;
    }

    // Keep the existing presentation at every other aspect ratio.
    if (target.safeScaleX < 1.0f) {
        result.x = target.safeScaleX;
        result.correctionRequired = true;
    }
    return result;
}

} // namespace novafix::display
