#include "d3d9/mip_lod_bias_controller.h"

#include "display/texture_policy.h"

#include <d3d9.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <limits>

namespace novafix::d3d9::mip_lod_bias {
namespace {

constexpr std::uint32_t kMipMapLodBias = 8u;
constexpr std::uint32_t kMinFilter = 6u;
constexpr std::uint32_t kMipFilter = 7u;
constexpr std::uint32_t kMaxAnisotropy = 10u;
constexpr std::size_t kSamplerCount = 16;
constexpr std::size_t kTrackedStateCount = 4;
constexpr std::uint32_t kEnabled = 1u << 0;
constexpr std::uint32_t kClampMipBias = 1u << 1;
constexpr unsigned kAnisotropyShift = 8;
constexpr std::uint32_t kUnseen = std::numeric_limits<std::uint32_t>::max();

// Pack the common sampler policy into one 32-bit atomic load on the hot path
std::atomic<std::uint32_t> g_policy{0};
std::atomic<std::uint32_t> g_minimumBiasBits{0};
std::atomic<bool> g_applyPending{false};
std::array<std::array<std::atomic<std::uint32_t>, kTrackedStateCount>,
           kSamplerCount> g_nativeState{};
thread_local bool g_applyingPendingState = false;

float Normalize(float value) {
    return std::isfinite(value) ? std::clamp(value, -1.0f, 1.0f) : 0.0f;
}

unsigned NormalizeAnisotropy(unsigned value) {
    constexpr unsigned values[]{0u, 2u, 4u, 8u, 16u};
    for (const unsigned candidate : values) {
        if (value == candidate) return value;
    }
    return 0u;
}

int TrackedStateIndex(std::uint32_t type) {
    switch (type) {
    case kMinFilter: return 0;
    case kMipFilter: return 1;
    case kMipMapLodBias: return 2;
    case kMaxAnisotropy: return 3;
    default: return -1;
    }
}

std::uint32_t MakePolicy(bool enabled, bool clampMipBias,
                         unsigned anisotropy) {
    return (enabled ? kEnabled : 0u) |
           (clampMipBias ? kClampMipBias : 0u) |
           (anisotropy << kAnisotropyShift);
}

unsigned PolicyAnisotropy(std::uint32_t policy) {
    return policy >> kAnisotropyShift;
}

std::uint32_t Adjust(std::uint32_t type, std::uint32_t value,
                     std::uint32_t policy) {
    const unsigned anisotropy = PolicyAnisotropy(policy);
    switch (type) {
    case kMaxAnisotropy:
        return anisotropy > 1u ? anisotropy : value;
    case kMinFilter:
        if (anisotropy > 1u && (value == D3DTEXF_LINEAR ||
                               value == D3DTEXF_ANISOTROPIC)) {
            return D3DTEXF_ANISOTROPIC;
        }
        return value;
    case kMipFilter:
        return anisotropy > 1u && value == D3DTEXF_POINT
            ? D3DTEXF_LINEAR : value;
    case kMipMapLodBias:
        if (anisotropy > 1u && (policy & kClampMipBias) != 0u) {
            return display::texture_policy::ClampNegativeMipLodBias(
                type, value, std::bit_cast<float>(
                    g_minimumBiasBits.load(std::memory_order_relaxed)));
        }
        return value;
    default:
        return value;
    }
}

} // namespace

void Configure(bool enabled, bool clampXiii2MipBias,
               unsigned anisotropy, float minimumBias) {
    const unsigned normalizedAnisotropy = NormalizeAnisotropy(anisotropy);
    g_minimumBiasBits.store(
        std::bit_cast<std::uint32_t>(Normalize(minimumBias)),
        std::memory_order_relaxed);
    g_policy.store(
        MakePolicy(enabled, clampXiii2MipBias, normalizedAnisotropy),
        std::memory_order_release);
    ResetSamplerCache();
}

void Preview(unsigned anisotropy, float minimumBias) {
    const std::uint32_t currentPolicy =
        g_policy.load(std::memory_order_acquire);
    if ((currentPolicy & kEnabled) == 0u) return;
    const unsigned normalizedAnisotropy = NormalizeAnisotropy(anisotropy);
    const float normalized = Normalize(minimumBias);
    const std::uint32_t newBiasBits = std::bit_cast<std::uint32_t>(normalized);
    const std::uint32_t newPolicy =
        MakePolicy(true, (currentPolicy & kClampMipBias) != 0u,
                   normalizedAnisotropy);
    const std::uint32_t oldBiasBits =
        g_minimumBiasBits.exchange(newBiasBits, std::memory_order_relaxed);
    const std::uint32_t oldPolicy =
        g_policy.exchange(newPolicy, std::memory_order_release);
    if (oldPolicy != newPolicy || oldBiasBits != newBiasBits) {
        g_applyPending.store(true, std::memory_order_release);
    }
}

std::uint32_t AdjustSamplerState(std::uint32_t sampler, std::uint32_t type,
                                 std::uint32_t value) {
    if (g_applyingPendingState) {
        return value;
    }
    const std::uint32_t policy = g_policy.load(std::memory_order_relaxed);
    if ((policy & kEnabled) == 0u) return value;

    const int stateIndex = TrackedStateIndex(type);
    if (sampler < kSamplerCount && stateIndex >= 0) {
        auto& native =
            g_nativeState[sampler][static_cast<std::size_t>(stateIndex)];
        if (native.load(std::memory_order_relaxed) != value) {
            native.store(value, std::memory_order_relaxed);
        }
    }
    return Adjust(type, value, policy);
}

void ApplyPendingState(IDirect3DDevice9* device) {
    // Avoid an atomic exchange on every frame while no preview is pending
    if (!device || !g_applyPending.load(std::memory_order_acquire) ||
        !g_applyPending.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    const std::uint32_t policy = g_policy.load(std::memory_order_acquire);
    if ((policy & kEnabled) == 0u) return;

    constexpr D3DSAMPLERSTATETYPE states[]{
        D3DSAMP_MINFILTER,
        D3DSAMP_MIPFILTER,
        D3DSAMP_MIPMAPLODBIAS,
        D3DSAMP_MAXANISOTROPY,
    };
    g_applyingPendingState = true;
    for (std::uint32_t sampler = 0; sampler < kSamplerCount; ++sampler) {
        for (std::size_t index = 0; index < kTrackedStateCount; ++index) {
            const std::uint32_t native =
                g_nativeState[sampler][index].load(
                    std::memory_order_relaxed);
            if (native == kUnseen) continue;
            device->SetSamplerState(
                sampler, states[index],
                Adjust(static_cast<std::uint32_t>(states[index]), native,
                       policy));
        }
    }
    g_applyingPendingState = false;
}

void ResetSamplerCache() {
    for (auto& sampler : g_nativeState) {
        for (auto& state : sampler) {
            state.store(kUnseen, std::memory_order_relaxed);
        }
    }
    g_applyPending.store(false, std::memory_order_release);
}

} // namespace novafix::d3d9::mip_lod_bias
