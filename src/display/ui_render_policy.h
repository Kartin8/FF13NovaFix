#pragma once

#include <array>
#include <cstdint>

namespace novafix::display {

constexpr std::uint32_t kNativeUiVertexBufferSize = 358400;

struct VertexBufferRequest {
    std::uint32_t length{};
    std::uint32_t usage{};
    std::uint32_t fvf{};
    std::uint32_t pool{};
    bool sharedHandle{};
};

// Match the full XIII/XIII-2 GUI allocation contract, not just its byte size
bool IsNativeUiVertexBuffer(const VertexBufferRequest& request);

using ScreenQuad = std::array<float, 20>;

bool IsNativeScreenQuad(const float* vertices, std::uint32_t stride);

// Recognizes the engine's 1280x720 half-pixel fullscreen quad and rewrites it
// for the active viewport. Already-corrected or unrelated quads are ignored
bool CorrectNativeScreenQuad(const float* vertices, std::uint32_t stride,
                             std::uint32_t viewportWidth,
                             std::uint32_t viewportHeight,
                             ScreenQuad& corrected);

} // namespace novafix::display
