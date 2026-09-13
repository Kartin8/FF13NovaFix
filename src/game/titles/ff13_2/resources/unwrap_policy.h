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

// XIII-2 sometimes passes a Crystal Tools wrapper and sometimes a native
// IDirect3D9 resource to the same helper. Resolve either representation without
// reading beyond the allocation that owns the incoming pointer
Resolution ResolveResource(void* candidate, ImageRange gameImage) noexcept;

} // namespace novafix::game::xiii2_resource_unwrap
