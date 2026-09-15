#include "display/aspect_runtime.h"

#include <atomic>

namespace novafix::display {
namespace {

std::atomic_uint64_t g_backBufferExtent{};

} // namespace

void PublishBackBufferExtent(std::uint32_t width, std::uint32_t height) {
    if (width == 0u || height == 0u) return;
    const std::uint64_t packed =
        static_cast<std::uint64_t>(width) << 32u | height;
    g_backBufferExtent.store(packed, std::memory_order_release);
}

AspectRatio CurrentAspectRatio() {
    const std::uint64_t packed =
        g_backBufferExtent.load(std::memory_order_acquire);
    return ResolveAspectRatio(
        static_cast<std::uint32_t>(packed >> 32u),
        static_cast<std::uint32_t>(packed));
}

} // namespace novafix::display
