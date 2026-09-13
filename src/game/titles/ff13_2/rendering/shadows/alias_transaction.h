#pragma once

#include "game/titles/ff13_2/rendering/shadows/resource_snapshot.h"

#include <array>

namespace novafix::game::xiii2_shadow_alias {

struct AliasBinding {
    xiii2_shadow_resources::TextureResource* source{};
    const xiii2_shadow_resources::TextureResource* target{};
    IDirect3DTexture9* oldTexture{};
    IDirect3DSurface9* oldSurface{};
};

struct AliasTransaction {
    std::array<AliasBinding, 4> bindings{};
    bool active{};
};

void RestoreCascadeStorage(AliasTransaction& transaction);
bool AliasFieldsWritable(const xiii2_shadow_resources::Snapshot& snapshot);
bool BeginCascadeAlias(xiii2_shadow_resources::Snapshot& snapshot,
                       AliasTransaction& transaction);

} // namespace novafix::game::xiii2_shadow_alias
