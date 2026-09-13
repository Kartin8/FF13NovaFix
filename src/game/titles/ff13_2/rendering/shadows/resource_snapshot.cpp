#include "game/titles/ff13_2/rendering/shadows/resource_snapshot.h"

#include "diagnostics/log.h"
#include "game/titles/ff13_2/rendering/shadows/foreign_resource_access.h"

#include <atomic>
#include <cstddef>
#include <cstring>

namespace novafix::game::xiii2_shadow_resources {
namespace {

constexpr std::ptrdiff_t kRendererSplitCountOffset = 948;
constexpr std::ptrdiff_t kRendererAtlasOffset = 956;
constexpr std::ptrdiff_t kRendererCascade0Offset = 964;
constexpr std::ptrdiff_t kRendererCascade1Offset = 968;
constexpr std::uint32_t kExpectedSplitCount = 2u;

constexpr std::ptrdiff_t kScreenColorOffset = 4;
constexpr std::ptrdiff_t kScreenDepthOffset = 20;
constexpr std::ptrdiff_t kTextureWidthOffset = 16;
constexpr std::ptrdiff_t kTextureHeightOffset = 20;
constexpr std::ptrdiff_t kTextureNativeAliasOffset = 40;
constexpr std::ptrdiff_t kTextureNativeOwnedOffset = 44;
constexpr std::ptrdiff_t kTextureSurfaceOwnerOffset = 48;
constexpr std::ptrdiff_t kSurfaceFormatOffset = 24;
constexpr std::ptrdiff_t kSurfaceNativeOffset = 32;

constexpr std::uint32_t kInternalR32F = 9u;
constexpr std::uint32_t kInternalD24S8 = 15u;

std::atomic_bool g_validationFailureLogged{false};

using xiii2_shadow_foreign_resource::IsReadable;
using xiii2_shadow_foreign_resource::ReadField;
using xiii2_shadow_foreign_resource::ResourceBelongsToDevice;
using xiii2_shadow_foreign_resource::ValidateComObject;

bool ReadTexture(void* object, IDirect3DDevice9* device,
                 TextureResource& output, const char*& failure) {
    output = {};
    failure = nullptr;
    output.object = object;
    void* nativeAlias{};
    void* nativeOwned{};
    if (!object || !IsReadable(object, 52u) ||
        !ReadField(object, kTextureWidthOffset, output.logicalWidth) ||
        !ReadField(object, kTextureHeightOffset, output.logicalHeight) ||
        !ReadField(object, kTextureNativeAliasOffset, nativeAlias) ||
        !ReadField(object, kTextureNativeOwnedOffset, nativeOwned) ||
        !ReadField(object, kTextureSurfaceOwnerOffset, output.surfaceOwner)) {
        failure = "wrapper fields";
        return false;
    }
    if (nativeAlias == nullptr || nativeAlias != nativeOwned ||
        output.surfaceOwner == nullptr) {
        failure = "native ownership slots";
        return false;
    }
    if (
        !ReadField(output.surfaceOwner, kSurfaceFormatOffset,
                   output.internalFormat) ||
        !ReadField(output.surfaceOwner, kSurfaceNativeOffset,
                   output.nativeSurface)) {
        failure = "surface wrapper fields";
        return false;
    }
    if (output.logicalWidth == 0u || output.logicalHeight == 0u ||
        !output.nativeSurface) {
        failure = "logical dimensions or native surface";
        return false;
    }
    if (!ValidateComObject(nativeOwned) ||
        !ValidateComObject(output.nativeSurface)) {
        failure = "COM object contract";
        return false;
    }

    output.nativeTexture = static_cast<IDirect3DTexture9*>(nativeOwned);
    if (FAILED(output.nativeTexture->GetLevelDesc(0u, &output.textureDesc)) ||
        FAILED(output.nativeSurface->GetDesc(&output.surfaceDesc))) {
        failure = "D3D9 descriptor query";
        return false;
    }
    if (output.textureDesc.Width != output.surfaceDesc.Width ||
        output.textureDesc.Height != output.surfaceDesc.Height ||
        output.textureDesc.Format != output.surfaceDesc.Format) {
        failure = "texture/surface descriptor mismatch";
        return false;
    }
    if (!ResourceBelongsToDevice(output.nativeTexture, device) ||
        !ResourceBelongsToDevice(output.nativeSurface, device)) {
        failure = "D3D9 device ownership";
        return false;
    }
    return true;
}

void LogResourceValidationFailure(const char* role, const char* component,
                                  const char* reason) {
    if (!g_validationFailureLogged.exchange(true, std::memory_order_acq_rel)) {
        Log("XIII-2 direct shadow atlas validation rejected: %s %s %s; native pipeline retained",
            role, component, reason);
    }
}

bool ReadScreen(void* object, IDirect3DDevice9* device,
                ScreenResource& output, const char* role) {
    output = {};
    output.object = object;
    void* color{};
    void* depth{};
    if (!object || !ReadField(object, kScreenColorOffset, color) ||
        !ReadField(object, kScreenDepthOffset, depth)) {
        LogResourceValidationFailure(role, "screen", "wrapper fields");
        return false;
    }
    const char* failure{};
    if (!ReadTexture(color, device, output.color, failure)) {
        LogResourceValidationFailure(role, "color", failure);
        return false;
    }
    if (!ReadTexture(depth, device, output.depth, failure)) {
        LogResourceValidationFailure(role, "depth", failure);
        return false;
    }
    return true;
}

void LogValidationFailure(const char* reason) {
    if (!g_validationFailureLogged.exchange(true, std::memory_order_acq_rel)) {
        Log("XIII-2 direct shadow atlas validation rejected: %s; native pipeline retained",
            reason);
    }
}

bool UsesNativeCascadeStorage(const Snapshot& snapshot) {
    for (const auto& cascade : snapshot.cascades) {
        if (cascade.color.nativeTexture == snapshot.atlas.color.nativeTexture ||
            cascade.color.nativeSurface == snapshot.atlas.color.nativeSurface ||
            cascade.depth.nativeTexture == snapshot.atlas.depth.nativeTexture ||
            cascade.depth.nativeSurface == snapshot.atlas.depth.nativeSurface ||
            cascade.color.textureDesc.Width != snapshot.layout.cascadeWidth ||
            cascade.color.textureDesc.Height != snapshot.layout.cascadeHeight ||
            cascade.depth.textureDesc.Width != snapshot.layout.cascadeWidth ||
            cascade.depth.textureDesc.Height != snapshot.layout.cascadeHeight) {
            return false;
        }
    }
    return true;
}

bool UsesAtlasStorage(const Snapshot& snapshot) {
    for (const auto& cascade : snapshot.cascades) {
        if (cascade.color.nativeTexture != snapshot.atlas.color.nativeTexture ||
            cascade.color.nativeSurface != snapshot.atlas.color.nativeSurface ||
            cascade.depth.nativeTexture != snapshot.atlas.depth.nativeTexture ||
            cascade.depth.nativeSurface != snapshot.atlas.depth.nativeSurface) {
            return false;
        }
    }
    return true;
}

template <typename T>
T ReadTrustedField(const void* object, std::ptrdiff_t offset) {
    T value{};
    std::memcpy(
        &value, static_cast<const std::byte*>(object) + offset,
        sizeof(value));
    return value;
}

bool TextureStillNative(const TextureResource& resource) {
    return resource.object && resource.surfaceOwner &&
           ReadTrustedField<std::uint32_t>(
               resource.object, kTextureWidthOffset) ==
               resource.logicalWidth &&
           ReadTrustedField<std::uint32_t>(
               resource.object, kTextureHeightOffset) ==
               resource.logicalHeight &&
           ReadTrustedField<void*>(
               resource.object, kTextureNativeAliasOffset) ==
               resource.nativeTexture &&
           ReadTrustedField<void*>(
               resource.object, kTextureNativeOwnedOffset) ==
               resource.nativeTexture &&
           ReadTrustedField<IDirect3DSurface9*>(
               resource.surfaceOwner, kSurfaceNativeOffset) ==
               resource.nativeSurface;
}

} // namespace

bool ReadSnapshot(void* renderer, void** deviceSlot, Snapshot& output) {
    output = {};
    if (!renderer || !deviceSlot ||
        !IsReadable(deviceSlot, sizeof(void*)) || !*deviceSlot ||
        !ValidateComObject(*deviceSlot)) {
        LogValidationFailure("renderer or D3D9 device contract");
        return false;
    }

    std::uint32_t splitCount{};
    void* atlas{};
    void* cascade0{};
    void* cascade1{};
    const bool splitReadable =
        ReadField(renderer, kRendererSplitCountOffset, splitCount);
    const bool atlasReadable =
        ReadField(renderer, kRendererAtlasOffset, atlas);
    const bool cascade0Readable =
        ReadField(renderer, kRendererCascade0Offset, cascade0);
    const bool cascade1Readable =
        ReadField(renderer, kRendererCascade1Offset, cascade1);
    if (!splitReadable || splitCount != kExpectedSplitCount ||
        !atlasReadable || !atlas || !cascade0Readable || !cascade0 ||
        !cascade1Readable || !cascade1) {
        if (!g_validationFailureLogged.exchange(
                true, std::memory_order_acq_rel)) {
            Log("XIII-2 direct shadow atlas validation rejected: renderer=%p split=%u readable=%d atlas=%p/%d cascade0=%p/%d cascade1=%p/%d; native pipeline retained",
                renderer, splitCount, splitReadable, atlas, atlasReadable,
                cascade0, cascade0Readable, cascade1, cascade1Readable);
        }
        return false;
    }

    output.renderer = renderer;
    output.device = static_cast<IDirect3DDevice9*>(*deviceSlot);
    if (!ReadScreen(atlas, output.device, output.atlas, "packed-atlas")) {
        return false;
    }
    if (!ReadScreen(cascade0, output.device, output.cascades[0], "cascade-0")) {
        return false;
    }
    if (!ReadScreen(cascade1, output.device, output.cascades[1], "cascade-1")) {
        return false;
    }

    const auto& first = output.cascades[0];
    const auto& second = output.cascades[1];
    output.layout = {
        first.color.logicalWidth,
        first.color.logicalHeight,
        output.atlas.color.logicalWidth,
        output.atlas.color.logicalHeight,
    };
    const bool logicalDimensionsMatch =
        first.color.logicalWidth == second.color.logicalWidth &&
        first.color.logicalHeight == second.color.logicalHeight &&
        first.depth.logicalWidth == first.color.logicalWidth &&
        first.depth.logicalHeight == first.color.logicalHeight &&
        second.depth.logicalWidth == second.color.logicalWidth &&
        second.depth.logicalHeight == second.color.logicalHeight &&
        output.atlas.depth.logicalWidth == output.atlas.color.logicalWidth &&
        output.atlas.depth.logicalHeight == output.atlas.color.logicalHeight;
    const bool formatsMatch =
        output.atlas.color.internalFormat == kInternalR32F &&
        output.atlas.depth.internalFormat == kInternalD24S8 &&
        first.color.internalFormat == kInternalR32F &&
        second.color.internalFormat == kInternalR32F &&
        first.depth.internalFormat == kInternalD24S8 &&
        second.depth.internalFormat == kInternalD24S8 &&
        output.atlas.color.textureDesc.Format == D3DFMT_R32F &&
        output.atlas.depth.textureDesc.Format == D3DFMT_D24S8;
    const bool atlasPhysicalDimensionsMatch =
        output.atlas.color.textureDesc.Width == output.layout.atlasWidth &&
        output.atlas.color.textureDesc.Height == output.layout.atlasHeight &&
        output.atlas.depth.textureDesc.Width == output.layout.atlasWidth &&
        output.atlas.depth.textureDesc.Height == output.layout.atlasHeight;
    if (!logicalDimensionsMatch || !formatsMatch ||
        !atlasPhysicalDimensionsMatch ||
        !xiii2_shadow_atlas_policy::IsValid(output.layout)) {
        LogValidationFailure("format or dimension contract");
        return false;
    }

    output.fullyNative = UsesNativeCascadeStorage(output);
    output.fullyAliased = UsesAtlasStorage(output);
    if (!output.fullyNative && !output.fullyAliased) {
        LogValidationFailure("mixed or unexpected native storage ownership");
    }
    return output.fullyNative || output.fullyAliased;
}

bool CachedSnapshotMatches(void* renderer, void** deviceSlot,
                           const Snapshot& snapshot) {
    if (!renderer || renderer != snapshot.renderer || !deviceSlot ||
        *deviceSlot != snapshot.device) {
        return false;
    }
    if (ReadTrustedField<void*>(
            renderer, kRendererAtlasOffset) != snapshot.atlas.object ||
        ReadTrustedField<void*>(
            renderer, kRendererCascade0Offset) !=
            snapshot.cascades[0].object ||
        ReadTrustedField<void*>(
            renderer, kRendererCascade1Offset) !=
            snapshot.cascades[1].object) {
        return false;
    }
    if (!TextureStillNative(snapshot.atlas.color) ||
        !TextureStillNative(snapshot.atlas.depth)) {
        return false;
    }
    for (const auto& cascade : snapshot.cascades) {
        if (!TextureStillNative(cascade.color) ||
            !TextureStillNative(cascade.depth)) {
            return false;
        }
    }
    return true;
}

} // namespace novafix::game::xiii2_shadow_resources
