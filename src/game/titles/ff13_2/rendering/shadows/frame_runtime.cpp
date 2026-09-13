#include "game/titles/ff13_2/rendering/shadows/frame_runtime.h"

#include "diagnostics/log.h"
#include "game/titles/ff13_2/rendering/shadows/atlas_policy.h"
#include "game/titles/ff13_2/rendering/shadows/foreign_resource_access.h"

#include <d3d9.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>

namespace novafix::game::xiii2_shadow_frame_runtime {
namespace {

constexpr std::ptrdiff_t kRendererActiveTargetOffset = 1832;
constexpr std::ptrdiff_t kRendererCascadeIndexOffset = 2092;
constexpr std::ptrdiff_t kRendererFarReceiverMatrixOffset = 1060;
constexpr std::ptrdiff_t kSurfaceNativeOffset = 32;
constexpr float kReducedFarClipScale = 0.5f;

void** g_deviceSlot{};
void** g_currentColorOwnerSlot{};
void** g_currentDepthOwnerSlot{};

std::atomic_bool g_hooksReady{false};
std::atomic_bool g_enabled{false};
std::atomic_bool g_failClosedLogged{false};
std::atomic_bool g_failedClosed{false};
thread_local FrameToken g_frame{};
thread_local xiii2_shadow_resources::Snapshot g_cachedSnapshot{};
thread_local bool g_cacheValid{};

using xiii2_shadow_alias::AliasFieldsWritable;
using xiii2_shadow_alias::BeginCascadeAlias;
using xiii2_shadow_foreign_resource::IsWritable;
using xiii2_shadow_resources::CachedSnapshotMatches;
using xiii2_shadow_resources::ReadSnapshot;
using xiii2_shadow_resources::Snapshot;

template <typename T>
T ReadTrustedField(const void* object, std::ptrdiff_t offset) {
    T value{};
    std::memcpy(
        &value, static_cast<const std::byte*>(object) + offset,
        sizeof(value));
    return value;
}

bool RefreshCachedSnapshot(void* renderer) {
    Snapshot refreshed{};
    if (!ReadSnapshot(renderer, g_deviceSlot, refreshed) ||
        !refreshed.fullyNative ||
        !AliasFieldsWritable(refreshed)) {
        g_cacheValid = false;
        return false;
    }
    g_cachedSnapshot = refreshed;
    g_cacheValid = true;
    return true;
}

bool PrepareFrame(
    void* renderer, Snapshot& snapshot,
    xiii2_shadow_alias::AliasTransaction& transaction) {
    if (!g_hooksReady.load(std::memory_order_acquire) ||
        !g_enabled.load(std::memory_order_acquire)) {
        return false;
    }
    if (!g_cacheValid ||
        !CachedSnapshotMatches(renderer, g_deviceSlot, g_cachedSnapshot)) {
        if (!RefreshCachedSnapshot(renderer)) {
            FailClosed("shadow owner/device validation failed");
            return false;
        }
    }
    snapshot = g_cachedSnapshot;
    if (xiii2_shadow_atlas_policy::UsesReducedFarCascade(snapshot.layout) &&
        !IsWritable(
            static_cast<std::byte*>(renderer) +
                kRendererFarReceiverMatrixOffset,
            sizeof(float) * 16u)) {
        FailClosed("far receiver matrix is not writable");
        return false;
    }
    if (!BeginCascadeAlias(snapshot, transaction)) {
        FailClosed("cached alias transaction could not begin");
        return false;
    }
    return true;
}

} // namespace

void BindResourceSlots(void** deviceSlot, void** currentColorOwnerSlot,
                       void** currentDepthOwnerSlot) {
    g_deviceSlot = deviceSlot;
    g_currentColorOwnerSlot = currentColorOwnerSlot;
    g_currentDepthOwnerSlot = currentDepthOwnerSlot;
}

void SetHooksReady(bool ready) {
    g_hooksReady.store(ready, std::memory_order_release);
}

bool HooksReady() {
    return g_hooksReady.load(std::memory_order_acquire);
}

void FailClosed(const char* reason) {
    g_enabled.store(false, std::memory_order_release);
    g_failedClosed.store(true, std::memory_order_release);
    if (!g_failClosedLogged.exchange(true, std::memory_order_acq_rel)) {
        LogWarning("XIII-2 cached direct atlas failed closed: %s; native shadow resources forced until restart",
            reason);
    }
}

void Activate() {
    (void)g_enabled.exchange(true, std::memory_order_acq_rel);
}

bool Enabled() {
    return g_enabled.load(std::memory_order_acquire);
}

bool FailedClosed() {
    return g_failedClosed.load(std::memory_order_acquire);
}

FrameToken BeginFrame(
    void* renderer, Snapshot& snapshot,
    xiii2_shadow_alias::AliasTransaction& transaction) {
    const FrameToken previous = g_frame;
    g_frame.active = PrepareFrame(renderer, snapshot, transaction);
    g_frame.renderer = renderer;
    g_frame.snapshot = &snapshot;
    return previous;
}

void ApplyReducedFarReceiverMatrix(
    void* renderer, const Snapshot& snapshot) {
    if (g_frame.active &&
        xiii2_shadow_atlas_policy::UsesReducedFarCascade(snapshot.layout)) {
        auto* matrix = reinterpret_cast<float*>(
            static_cast<std::byte*>(renderer) +
            kRendererFarReceiverMatrixOffset);
        constexpr std::array<std::size_t, 8> kClipXYElements{
            0u, 1u, 4u, 5u, 8u, 9u, 12u, 13u};
        for (const std::size_t index : kClipXYElements) {
            matrix[index] *= kReducedFarClipScale;
        }
    }
}

void RestoreFrame(const FrameToken& previous) {
    g_frame = previous;
}

const Snapshot* CurrentSnapshot() {
    return g_frame.snapshot;
}

std::optional<std::uint32_t> ActiveCascade() {
    const Snapshot* snapshot = g_frame.snapshot;
    if (!g_frame.active || !g_frame.renderer || !snapshot ||
        !g_currentColorOwnerSlot || !g_currentDepthOwnerSlot) {
        return std::nullopt;
    }
    const std::uint32_t index = ReadTrustedField<std::uint32_t>(
        g_frame.renderer, kRendererCascadeIndexOffset);
    const void* activeTarget = ReadTrustedField<void*>(
        g_frame.renderer, kRendererActiveTargetOffset);
    if (index >= snapshot->cascades.size() ||
        activeTarget != snapshot->cascades[index].object ||
        *g_currentColorOwnerSlot !=
            snapshot->cascades[index].color.surfaceOwner ||
        *g_currentDepthOwnerSlot !=
            snapshot->cascades[index].depth.surfaceOwner) {
        return std::nullopt;
    }
    return index;
}

bool SuppressNativePacking(void* source, void* destination) {
    if (!g_frame.active || !source || !destination ||
        !g_frame.snapshot ||
        !g_enabled.load(std::memory_order_acquire)) {
        return false;
    }
    const auto* sourceWrapper = static_cast<const std::byte*>(source);
    const auto* destinationWrapper =
        static_cast<const std::byte*>(destination);
    IDirect3DSurface9* const sourceSurface =
        ReadTrustedField<IDirect3DSurface9*>(
            sourceWrapper, kSurfaceNativeOffset);
    IDirect3DSurface9* const destinationSurface =
        ReadTrustedField<IDirect3DSurface9*>(
            destinationWrapper, kSurfaceNativeOffset);
    const bool selfCopy =
        sourceSurface && sourceSurface == destinationSurface &&
        destinationSurface ==
            g_frame.snapshot->atlas.color.nativeSurface;
    return selfCopy;
}

} // namespace novafix::game::xiii2_shadow_frame_runtime
