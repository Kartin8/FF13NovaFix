#include "game/titles/ff13_2/rendering/shadows/alias_transaction.h"

#include "diagnostics/log.h"
#include "game/titles/ff13_2/rendering/shadows/atlas_policy.h"
#include "game/titles/ff13_2/rendering/shadows/foreign_resource_access.h"

#include <atomic>
#include <cstddef>

namespace novafix::game::xiii2_shadow_alias {
namespace {

constexpr std::ptrdiff_t kTextureNativeAliasOffset = 40;
constexpr std::ptrdiff_t kTextureNativeOwnedOffset = 44;
constexpr std::ptrdiff_t kSurfaceNativeOffset = 32;

std::atomic_bool g_reducedFarLogged{false};

using xiii2_shadow_foreign_resource::FieldWritable;
using xiii2_shadow_foreign_resource::WriteField;

} // namespace

void RestoreCascadeStorage(AliasTransaction& transaction) {
    if (!transaction.active) return;

    for (const auto& binding : transaction.bindings) {
        WriteField(binding.source->object, kTextureNativeAliasOffset,
                   binding.oldTexture);
        WriteField(binding.source->object, kTextureNativeOwnedOffset,
                   binding.oldTexture);
        WriteField(binding.source->surfaceOwner, kSurfaceNativeOffset,
                   binding.oldSurface);
    }
    transaction.active = false;
}

bool AliasFieldsWritable(const xiii2_shadow_resources::Snapshot& snapshot) {
    for (const auto& cascade : snapshot.cascades) {
        const std::array<const xiii2_shadow_resources::TextureResource*, 2>
            resources{&cascade.color, &cascade.depth};
        for (const xiii2_shadow_resources::TextureResource* resource :
             resources) {
            if (!FieldWritable<void*>(
                    resource->object, kTextureNativeAliasOffset) ||
                !FieldWritable<void*>(
                    resource->object, kTextureNativeOwnedOffset) ||
                !FieldWritable<void*>(
                    resource->surfaceOwner, kSurfaceNativeOffset)) {
                return false;
            }
        }
    }
    return true;
}

bool BeginCascadeAlias(xiii2_shadow_resources::Snapshot& snapshot,
                       AliasTransaction& transaction) {
    if (!snapshot.fullyNative || transaction.active) return false;

    const bool reducedFar =
        xiii2_shadow_atlas_policy::UsesReducedFarCascade(snapshot.layout);
    transaction.bindings = {{
        {&snapshot.cascades[0].color, &snapshot.atlas.color},
        {&snapshot.cascades[0].depth, &snapshot.atlas.depth},
        {&snapshot.cascades[1].color, &snapshot.atlas.color},
        {&snapshot.cascades[1].depth, &snapshot.atlas.depth},
    }};
    for (auto& binding : transaction.bindings) {
        auto& source = *binding.source;
        binding.oldTexture = source.nativeTexture;
        binding.oldSurface = source.nativeSurface;
    }

    for (const auto& binding : transaction.bindings) {
        auto* nativeTexture = binding.target->nativeTexture;
        auto* nativeSurface = binding.target->nativeSurface;
        WriteField(binding.source->object, kTextureNativeAliasOffset,
                   nativeTexture);
        WriteField(binding.source->object, kTextureNativeOwnedOffset,
                   nativeTexture);
        WriteField(binding.source->surfaceOwner, kSurfaceNativeOffset,
                   nativeSurface);
    }
    if (reducedFar) {
        if (!g_reducedFarLogged.exchange(true, std::memory_order_acq_rel)) {
            Log("XIII-2 shadow layout: near=8192 far=4096");
        }
    }
    transaction.active = true;
    return true;
}

} // namespace novafix::game::xiii2_shadow_alias
