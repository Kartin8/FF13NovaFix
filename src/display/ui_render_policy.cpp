#include "display/ui_render_policy.h"

#include <algorithm>
#include <cmath>

namespace novafix::display {
namespace {

constexpr std::uint32_t kD3dPoolManaged = 1;

constexpr ScreenQuad kNativeScreenQuad{
    -1.0f - 1.0f / 1280.0f,  1.0f + 1.0f / 720.0f, 0.0f, 0.0f, 0.0f,
     1.0f - 1.0f / 1280.0f,  1.0f + 1.0f / 720.0f, 0.0f, 1.0f, 0.0f,
     1.0f - 1.0f / 1280.0f, -1.0f + 1.0f / 720.0f, 0.0f, 1.0f, 1.0f,
    -1.0f - 1.0f / 1280.0f, -1.0f + 1.0f / 720.0f, 0.0f, 0.0f, 1.0f,
};

bool NearlyEqual(float left, float right) {
    return std::isfinite(left) && std::abs(left - right) <= 0.00001f;
}

} // namespace

bool IsNativeUiVertexBuffer(const VertexBufferRequest& request) {
    return request.length == kNativeUiVertexBufferSize &&
           request.usage == 0 && request.fvf == 0 &&
           request.pool == kD3dPoolManaged && !request.sharedHandle;
}

bool IsNativeScreenQuad(const float* vertices, std::uint32_t stride) {
    if (!vertices || stride != 5u * sizeof(float)) return false;
    for (std::size_t index = 0; index < kNativeScreenQuad.size(); ++index) {
        if (!NearlyEqual(vertices[index], kNativeScreenQuad[index])) {
            return false;
        }
    }
    return true;
}

bool CorrectNativeScreenQuad(const float* vertices, std::uint32_t stride,
                             std::uint32_t viewportWidth,
                             std::uint32_t viewportHeight,
                             ScreenQuad& corrected) {
    if (!IsNativeScreenQuad(vertices, stride) ||
        viewportWidth < 1 || viewportHeight < 1) return false;

    corrected = kNativeScreenQuad;
    const float horizontal = 1.0f / static_cast<float>(viewportWidth);
    const float vertical = 1.0f / static_cast<float>(viewportHeight);
    for (std::size_t vertex = 0; vertex < 4; ++vertex) {
        const std::size_t base = vertex * 5;
        corrected[base] = (vertex == 0 || vertex == 3 ? -1.0f : 1.0f) - horizontal;
        corrected[base + 1] = (vertex < 2 ? 1.0f : -1.0f) + vertical;
    }
    return true;
}

} // namespace novafix::display
