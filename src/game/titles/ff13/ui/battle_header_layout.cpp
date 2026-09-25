#include "game/titles/ff13/ui/battle_header_layout.h"

#include "patch/memory_access.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace novafix::game::ff13_ui::battle_header_layout {
namespace {

constexpr std::uint32_t kLeftHeaderProductVtableRva = 0x11446D8u - 0x400000u;
constexpr std::uint32_t kRightHeaderProductVtableRva = 0x1142628u - 0x400000u;
constexpr std::uint32_t kHeaderRootVtableRva = 0x119C470u - 0x400000u;
constexpr float kLeftEdgeOverlap = 100.0f;
constexpr float kRightEdgeOverlap = 84.0f;

struct Context {
    void* root{};
    float offset{};
};
thread_local Context g_context{};

template <typename T>
T& Field(void* object, std::size_t offset) {
    return *reinterpret_cast<T*>(static_cast<std::byte*>(object) + offset);
}

} // namespace

ScopedProduct::ScopedProduct(
    void* product, const display::AspectRatio& aspect,
    std::uintptr_t imageBase)
    : previousRoot_(g_context.root),
      previousOffset_(g_context.offset) {
    g_context = {};
    if (!product || !aspect.valid || !std::isfinite(aspect.safeScaleX) ||
        aspect.safeScaleX <= 0.0f || aspect.safeScaleX >= 1.0f ||
        imageBase == 0u) return;
    const auto vtable = reinterpret_cast<std::uintptr_t>(
        Field<void*>(product, 0u));
    if (vtable < imageBase) return;
    const auto vtableRva = vtable - imageBase;
    const int side = vtableRva == kLeftHeaderProductVtableRva ? -1
        : vtableRva == kRightHeaderProductVtableRva ? 1 : 0;
    if (side == 0) return;
    void* root = Field<void*>(product, 0x34u);
    if (!root || !patch::memory::IsReadable(root, 0xD0u) ||
        reinterpret_cast<std::uintptr_t>(Field<void*>(root, 0u)) !=
            imageBase + kHeaderRootVtableRva ||
        Field<int>(root, 0xC0u) != (side < 0 ? 794 : 1041)) return;
    const float gutter = 640.0f * (1.0f / aspect.safeScaleX - 1.0f);
    if (!std::isfinite(gutter)) return;
    const float shift = std::max(0.0f, gutter -
        (side < 0 ? kLeftEdgeOverlap : kRightEdgeOverlap));
    if (shift == 0.0f) return;
    g_context = {root, side * shift};
}

ScopedProduct::~ScopedProduct() {
    g_context = {previousRoot_, previousOffset_};
}

bool Affects(void* node) {
    return node && node == g_context.root;
}

void Apply(void* node) {
    Field<float>(node, 0x48u) += g_context.offset;
}

} // namespace novafix::game::ff13_ui::battle_header_layout
