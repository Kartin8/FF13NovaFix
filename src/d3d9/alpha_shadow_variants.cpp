#include "d3d9/alpha_shadow_variants.h"

#include "d3d9/shader_bytecode/alpha_shadow_fixed_depth.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>

namespace novafix::d3d9::alpha_shadow_variants {
namespace {

struct AlphaShadowVariants {
    std::atomic<IDirect3DPixelShader9*> original{nullptr};
    std::atomic<IDirect3DPixelShader9*> fixedDepth{nullptr};
};

constexpr std::size_t kVariantCapacity = 16u;
std::array<AlphaShadowVariants, kVariantCapacity> g_variants{};
std::atomic_uint g_variantCount{};
SRWLOCK g_variantLock = SRWLOCK_INIT;
std::atomic_bool g_fixedDepthEnabled{false};

} // namespace

bool NeedsVariant(const shader_corrections::Resolution& resolution) {
    return resolution.kind == ShaderKind::AlphaShadow &&
           resolution.replaced;
}

const DWORD* FixedDepthBytecode() {
    return reinterpret_cast<const DWORD*>(g_alpha_shadow_fixed_depth);
}

void RecordVariant(
    IDirect3DPixelShader9* original, IDirect3DPixelShader9* fixedDepth,
    HRESULT result) {
    if (!original || FAILED(result) || !fixedDepth) return;
    AcquireSRWLockExclusive(&g_variantLock);
    AlphaShadowVariants* target{};
    for (auto& record : g_variants) {
        IDirect3DPixelShader9* const recorded =
            record.original.load(std::memory_order_relaxed);
        if (recorded == original) {
            target = &record;
            break;
        }
        if (!recorded && !target) target = &record;
    }
    if (target && !target->original.load(std::memory_order_relaxed)) {
        target->fixedDepth.store(fixedDepth, std::memory_order_relaxed);
        target->original.store(original, std::memory_order_release);
        g_variantCount.fetch_add(1u, std::memory_order_relaxed);
    } else if (target) {
        target->fixedDepth.store(fixedDepth, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&g_variantLock);
    if (!target) fixedDepth->Release();
}

IDirect3DPixelShader9* ResolveFixedDepth(
    IDirect3DPixelShader9* original, bool enabled) {
    if (!enabled || !original) return original;
    const unsigned published = (std::min)(
        g_variantCount.load(std::memory_order_acquire),
        static_cast<unsigned>(g_variants.size()));
    for (unsigned index = 0u; index < published; ++index) {
        const auto& record = g_variants[index];
        if (record.original.load(std::memory_order_acquire) == original) {
            IDirect3DPixelShader9* const fixed =
                record.fixedDepth.load(std::memory_order_acquire);
            return fixed ? fixed : original;
        }
    }
    return original;
}

void SetFixedDepthEnabled(bool enabled) {
    g_fixedDepthEnabled.store(enabled, std::memory_order_release);
}

bool FixedDepthEnabled() {
    return g_fixedDepthEnabled.load(std::memory_order_acquire);
}

} // namespace novafix::d3d9::alpha_shadow_variants
