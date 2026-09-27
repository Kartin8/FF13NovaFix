#include "d3d9/shaders/minimap_shader_variant.h"

#include "d3d9/shaders/bytecode/ff13_2_minimap_clip_coordinates.h"
#include "d3d9/shaders/shader_bytecode_identity.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

namespace novafix::d3d9::minimap_shader_variant {
namespace {

constexpr std::uint64_t kNativeMapVertexHash = 0x3254E4161C0D6A13ull;
constexpr std::size_t kNativeMapVertexBytes = 356u;
constexpr std::size_t kCapacity = 16u;

struct Variant {
    std::atomic<IDirect3DVertexShader9*> original{nullptr};
    std::atomic<IDirect3DVertexShader9*> corrected{nullptr};
};

std::array<Variant, kCapacity> g_variants{};
std::atomic_uint g_count{};
SRWLOCK g_lock = SRWLOCK_INIT;

} // namespace

bool MatchesNativeVertexShader(const DWORD* bytecode) {
    const auto identity = InspectVertexShaderBytecode(
        reinterpret_cast<const std::uint32_t*>(bytecode));
    return identity.bytes == kNativeMapVertexBytes &&
           identity.hash == kNativeMapVertexHash;
}

bool Available() {
    return g_count.load(std::memory_order_acquire) != 0u;
}

const DWORD* CorrectedBytecode() {
    return reinterpret_cast<const DWORD*>(g_ff13_2_minimap_clip_coordinates);
}

void Record(IDirect3DVertexShader9* original,
            IDirect3DVertexShader9* corrected) {
    if (!original || !corrected) return;
    AcquireSRWLockExclusive(&g_lock);
    Variant* target{};
    for (auto& record : g_variants) {
        const auto* known = record.original.load(std::memory_order_relaxed);
        if (known == original) {
            target = &record;
            break;
        }
        if (!known && !target) target = &record;
    }
    if (target && !target->original.load(std::memory_order_relaxed)) {
        target->corrected.store(corrected, std::memory_order_relaxed);
        target->original.store(original, std::memory_order_release);
        g_count.fetch_add(1u, std::memory_order_relaxed);
    } else if (target) {
        auto* previous = target->corrected.exchange(
            corrected, std::memory_order_acq_rel);
        if (previous) previous->Release();
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!target) corrected->Release();
}

IDirect3DVertexShader9* Resolve(IDirect3DVertexShader9* original) {
    if (!original) return original;
    const unsigned count = (std::min)(
        g_count.load(std::memory_order_acquire),
        static_cast<unsigned>(g_variants.size()));
    for (unsigned index = 0; index < count; ++index) {
        const auto& record = g_variants[index];
        if (record.original.load(std::memory_order_acquire) == original) {
            auto* corrected = record.corrected.load(std::memory_order_acquire);
            return corrected ? corrected : original;
        }
    }
    return original;
}

} // namespace novafix::d3d9::minimap_shader_variant
