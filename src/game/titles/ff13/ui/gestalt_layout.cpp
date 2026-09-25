#include "game/titles/ff13/ui/gestalt_layout.h"

#include "patch/memory_access.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace novafix::game::ff13_ui::gestalt_layout {
namespace {

constexpr std::uint32_t kGestaltProductVtableRva = 0x1142D30u - 0x400000u;
constexpr std::uint32_t kRootVtableRva = 0x119C470u - 0x400000u;

struct Context {
    void* root{};
    float x{};
    float y{};
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
      previousX_(g_context.x),
      previousY_(g_context.y) {
    g_context = {};
    if (!product || !aspect.valid || imageBase == 0u ||
        reinterpret_cast<std::uintptr_t>(Field<void*>(product, 0u)) !=
            imageBase + kGestaltProductVtableRva) return;
    void* root = Field<void*>(product, 0x34u);
    if (!root || !patch::memory::IsReadable(root, 0xD0u) ||
        reinterpret_cast<std::uintptr_t>(Field<void*>(root, 0u)) !=
            imageBase + kRootVtableRva) return;

    float x = 0.0f;
    float y = 0.0f;
    if (std::isfinite(aspect.safeScaleX) &&
        aspect.safeScaleX > 0.0f && aspect.safeScaleX < 1.0f) {
        const float gutter = 640.0f * (1.0f / aspect.safeScaleX - 1.0f);
        x = -gutter;
    } else if (std::isfinite(aspect.safeScaleY) &&
               aspect.safeScaleY > 0.0f && aspect.safeScaleY < 1.0f) {
        y = 360.0f * (1.0f / aspect.safeScaleY - 1.0f);
    }
    if (!std::isfinite(x) || !std::isfinite(y) ||
        (x == 0.0f && y == 0.0f)) return;
    g_context = {root, x, y};
}

ScopedProduct::~ScopedProduct() {
    g_context = {previousRoot_, previousX_, previousY_};
}

bool Affects(void* node) {
    return node && node == g_context.root;
}

void Apply(void* node) {
    Field<float>(node, 0x48u) += g_context.x;
    Field<float>(node, 0x4Cu) += g_context.y;
}

} // namespace novafix::game::ff13_ui::gestalt_layout
