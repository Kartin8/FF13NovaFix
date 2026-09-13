#pragma once

#include "game/titles/ff13_2/rendering/shadows/atlas_policy.h"

#include <d3d9.h>

#include <array>
#include <cstdint>

namespace novafix::game::xiii2_shadow_resources {

struct TextureResource {
    void* object{};
    std::uint32_t logicalWidth{};
    std::uint32_t logicalHeight{};
    std::uint32_t internalFormat{};
    IDirect3DTexture9* nativeTexture{};
    void* surfaceOwner{};
    IDirect3DSurface9* nativeSurface{};
    D3DSURFACE_DESC textureDesc{};
    D3DSURFACE_DESC surfaceDesc{};
};

struct ScreenResource {
    void* object{};
    TextureResource color{};
    TextureResource depth{};
};

struct Snapshot {
    void* renderer{};
    IDirect3DDevice9* device{};
    ScreenResource atlas{};
    std::array<ScreenResource, 2> cascades{};
    xiii2_shadow_atlas_policy::Layout layout{};
    bool fullyNative{};
    bool fullyAliased{};
};

bool ReadSnapshot(void* renderer, void** deviceSlot, Snapshot& output);
bool CachedSnapshotMatches(void* renderer, void** deviceSlot,
                           const Snapshot& snapshot);

} // namespace novafix::game::xiii2_shadow_resources
