#include "d3d9/shader_corrections.h"

#include "diagnostics/log.h"
#include "compat/compatibility.h"
#include "d3d9/shader_bytecode/alpha_shadow.h"
#include "d3d9/shader_bytecode/opaque_shadow_fixed_depth.h"
#include "d3d9/shader_bytecode_identity.h"
#include "game/core/game_profile.h"
#include "settings/graphics_settings.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace novafix::d3d9::shader_corrections {
namespace {

struct OpaqueShadowVariants {
    std::atomic<IDirect3DPixelShader9*> original{nullptr};
    std::atomic<IDirect3DPixelShader9*> fixedDepth{nullptr};
};

constexpr std::uint64_t kOpaqueShadowProducerHash =
    0x060B9A63A30B8F34ull;
constexpr std::size_t kOpaqueShadowVariantCapacity = 16u;
std::array<OpaqueShadowVariants, kOpaqueShadowVariantCapacity>
    g_opaqueShadowVariants{};
SRWLOCK g_opaqueShadowVariantLock = SRWLOCK_INIT;
std::atomic_uint g_opaqueShadowVariantCount{0};
std::atomic_bool g_opaqueShadowFixedDepthRejected{false};
std::atomic_bool g_shaderReplacementFailureLogged{false};
std::atomic_bool g_opaqueShadowTableFullLogged{false};
std::atomic<OpaqueShadowMode> g_opaqueShadowMode{OpaqueShadowMode::Native};
std::atomic_bool g_opaqueShadowProducerBound{false};
std::atomic_uint32_t g_opaqueShadowBiasBits{0};

ShaderKind IdentifyOriginalShader(std::uint64_t hash) {
    return hash == 0xE3FFDEC520D824B2ull
        ? ShaderKind::AlphaShadow
        : ShaderKind::Unknown;
}

bool ShouldReplaceShader(ShaderKind kind, bool correctionsEnabled) {
    return correctionsEnabled && kind == ShaderKind::AlphaShadow;
}

const DWORD* ReplacementBytecode(ShaderKind kind) {
    switch (kind) {
    case ShaderKind::AlphaShadow:
        return reinterpret_cast<const DWORD*>(g_alpha_shadow);
    case ShaderKind::Unknown:
    case ShaderKind::Count:
        return nullptr;
    }
    return nullptr;
}

const char* ShaderName(ShaderKind kind) {
    switch (kind) {
    case ShaderKind::AlphaShadow: return "alpha-tested shadow";
    case ShaderKind::Unknown:
    case ShaderKind::Count:
        return "unknown";
    }
    return "unknown";
}

void PublishOpaqueShadowProducerBound(bool bound) {
    if (g_opaqueShadowProducerBound.load(std::memory_order_relaxed) != bound) {
        g_opaqueShadowProducerBound.store(bound, std::memory_order_release);
    }
}

} // namespace

Resolution ResolveBytecode(const DWORD* original) {
    Resolution result{};
    result.bytecode = original;
    if (!original || !game::Supports(game::Capability::ShaderCorrection)) {
        return result;
    }

    const ShaderBytecodeIdentity identity = InspectShaderBytecode(
        reinterpret_cast<const std::uint32_t*>(original));
    if (!identity) return result;

    result.originalHash = identity.hash;
    result.originalBytes = static_cast<unsigned>(identity.bytes);
    result.kind = IdentifyOriginalShader(identity.hash);
    result.name = ShaderName(result.kind);
    const settings::GraphicsSettings graphics = settings::Active();
    const bool correctionsEnabled =
        compat::compatibility::AllowsNovaFixChanges() &&
        graphics.xiii2ShaderCorrections;
    if (!ShouldReplaceShader(result.kind, correctionsEnabled)) {
        return result;
    }

    const DWORD* replacement = ReplacementBytecode(result.kind);
    if (replacement) {
        result.bytecode = replacement;
        result.replaced = true;
    }
    return result;
}

void RecordCreation(const Resolution& resolution,
                    IDirect3DPixelShader9* created, HRESULT result) {
    if (!resolution.replaced) return;

    if (FAILED(result) || !created) {
        if (!g_shaderReplacementFailureLogged.exchange(
                true, std::memory_order_acq_rel)) {
            LogWarning("XIII-2 shader replacement creation failed: %s hash=%016llX result=0x%08lX retry=native-bytecode",
                resolution.name, resolution.originalHash,
                static_cast<unsigned long>(result));
        }
        return;
    }
    g_shaderReplacementFailureLogged.store(
        false, std::memory_order_release);
}

bool NeedsOpaqueShadowVariant(const Resolution& resolution) {
    return resolution.originalHash == kOpaqueShadowProducerHash &&
           resolution.originalBytes == 448u;
}

const DWORD* OpaqueShadowBytecode(OpaqueShadowMode mode) {
    switch (mode) {
    case OpaqueShadowMode::FixedFunctionDepth:
        return reinterpret_cast<const DWORD*>(g_opaque_shadow_fixed_depth);
    case OpaqueShadowMode::Native:
        return nullptr;
    }
    return nullptr;
}

void RecordOpaqueShadowVariant(
    IDirect3DPixelShader9* original, OpaqueShadowMode mode,
    IDirect3DPixelShader9* variant, HRESULT result) {
    if (!original || mode == OpaqueShadowMode::Native) return;
    if (FAILED(result) || !variant) {
        if (mode == OpaqueShadowMode::FixedFunctionDepth) {
            if (!g_opaqueShadowFixedDepthRejected.exchange(
                    true, std::memory_order_acq_rel)) {
                LogWarning("XIII-2 fixed-depth shadow shader creation failed: mode=%u result=0x%08lX",
                    static_cast<unsigned>(mode),
                    static_cast<unsigned long>(result));
            }
        }
        return;
    }

    AcquireSRWLockExclusive(&g_opaqueShadowVariantLock);
    OpaqueShadowVariants* target = nullptr;
    for (auto& record : g_opaqueShadowVariants) {
        IDirect3DPixelShader9* const recorded =
            record.original.load(std::memory_order_relaxed);
        if (recorded == original) {
            target = &record;
            break;
        }
        if (!recorded && !target) target = &record;
    }
    if (target && !target->original.load(std::memory_order_relaxed)) {
        if (mode == OpaqueShadowMode::FixedFunctionDepth) {
            target->fixedDepth.store(variant, std::memory_order_relaxed);
        }
        // Publishing original last makes every matching lock-free reader see
        // the corresponding variant initialized in the same release sequence
        target->original.store(original, std::memory_order_release);
        g_opaqueShadowVariantCount.fetch_add(1u, std::memory_order_relaxed);
    } else if (target && mode == OpaqueShadowMode::FixedFunctionDepth) {
        target->fixedDepth.store(variant, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&g_opaqueShadowVariantLock);
    if (!target) {
        variant->Release();
        if (!g_opaqueShadowTableFullLogged.exchange(
                true, std::memory_order_acq_rel)) {
            Log("XIII-2 fixed-depth shadow shader table full: variant discarded; repeats suppressed");
        }
        return;
    }
}

IDirect3DPixelShader9* ResolveOpaqueShadowMode(
    IDirect3DPixelShader9* original) {
    const OpaqueShadowMode mode =
        g_opaqueShadowMode.load(std::memory_order_acquire);
    if (!original || mode == OpaqueShadowMode::Native) {
        PublishOpaqueShadowProducerBound(false);
        return original;
    }

    IDirect3DPixelShader9* resolved{};
    bool matched = false;
    const unsigned published = (std::min)(
        g_opaqueShadowVariantCount.load(std::memory_order_acquire),
        static_cast<unsigned>(g_opaqueShadowVariants.size()));
    for (unsigned index = 0; index < published; ++index) {
        const auto& record = g_opaqueShadowVariants[index];
        if (record.original.load(std::memory_order_acquire) != original) {
            continue;
        }
        matched = true;
        if (mode == OpaqueShadowMode::FixedFunctionDepth) {
            resolved = record.fixedDepth.load(std::memory_order_acquire);
        }
        break;
    }
    PublishOpaqueShadowProducerBound(matched);
    if (!resolved) return original;
    return resolved;
}

void SetOpaqueShadowMode(OpaqueShadowMode mode) {
    g_opaqueShadowMode.store(mode, std::memory_order_release);
    Log("XIII-2 opaque shadow mode=%u available=%u",
        static_cast<unsigned>(mode),
        g_opaqueShadowVariantCount.load(std::memory_order_relaxed));
}

OpaqueShadowMode GetOpaqueShadowMode() {
    return g_opaqueShadowMode.load(std::memory_order_acquire);
}

bool OpaqueShadowModeAvailable(OpaqueShadowMode mode) {
    if (mode == OpaqueShadowMode::Native) return true;
    bool available = false;
    const unsigned published = (std::min)(
        g_opaqueShadowVariantCount.load(std::memory_order_acquire),
        static_cast<unsigned>(g_opaqueShadowVariants.size()));
    for (unsigned index = 0; index < published; ++index) {
        const auto& record = g_opaqueShadowVariants[index];
        if (!record.original.load(std::memory_order_acquire)) continue;
        available = mode == OpaqueShadowMode::FixedFunctionDepth &&
                    record.fixedDepth.load(std::memory_order_acquire) != nullptr;
        if (available) break;
    }
    return available;
}

bool OpaqueShadowModeRejected(OpaqueShadowMode mode) {
    return mode == OpaqueShadowMode::FixedFunctionDepth &&
           g_opaqueShadowFixedDepthRejected.load(std::memory_order_acquire);
}

void ObserveOpaqueShadowConstants(std::uint32_t startRegister,
                                  const float* values,
                                  std::uint32_t vectorCount) {
    if (!values || vectorCount == 0u ||
        !g_opaqueShadowProducerBound.load(std::memory_order_acquire)) {
        return;
    }
    constexpr std::uint32_t biasRegister = 4u;
    const std::uint64_t endRegister =
        static_cast<std::uint64_t>(startRegister) + vectorCount;
    if (startRegister > biasRegister || endRegister <= biasRegister) return;

    const std::size_t floatOffset =
        static_cast<std::size_t>(biasRegister - startRegister) * 4u;
    std::uint32_t bits{};
    std::memcpy(&bits, values + floatOffset, sizeof(bits));
    if (g_opaqueShadowBiasBits.load(std::memory_order_relaxed) != bits) {
        g_opaqueShadowBiasBits.store(bits, std::memory_order_release);
    }
}

std::uint32_t OpaqueShadowBiasBits() {
    return g_opaqueShadowBiasBits.load(std::memory_order_acquire);
}

} // namespace novafix::d3d9::shader_corrections
