#pragma once

#include <cstdint>

namespace novafix::game::xiii2_resource_unwrap {

enum class ResolutionKind : std::uint8_t {
    Wrapped,
    Native,
    Rejected,
};

struct ImageRange {
    const void* begin{};
    const void* end{};
};

struct Resolution {
    void* resource{};
    ResolutionKind kind{ResolutionKind::Rejected};
};

// This helper can receive either a Crystal Tools wrapper or a raw IDirect3D9 resource
// Resolve both without scanning past the incoming allocation
Resolution ResolveResource(void* candidate, ImageRange gameImage) noexcept;

} // namespace novafix::game::xiii2_resource_unwrap
