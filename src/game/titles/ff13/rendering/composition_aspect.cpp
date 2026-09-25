#include "game/titles/ff13/rendering/composition_aspect.h"

#include "display/aspect_runtime.h"
#include "game/core/image_view.h"
#include "patch/relative_branch.h"
#include "patch/transaction.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace novafix::game::ff13_composition_aspect {
namespace {

constexpr std::uint32_t kCompositeRva = 0xEAE920u - 0x400000u;
constexpr std::uint32_t kFinalCompositeCallRva = 0xE9884Fu - 0x400000u;
constexpr std::uint32_t kBackBufferCompositeCallRva = 0xE49FB6u - 0x400000u;
constexpr std::uint32_t kSnapshotCopyCallRva = 0x8FC3D9u - 0x400000u;
constexpr std::uint32_t kSurfaceCopyRva = 0xEB3ED0u - 0x400000u;
constexpr std::uint32_t kSurfaceStretchRva = 0xEB4090u - 0x400000u;
constexpr std::array<std::byte, 5> kFinalCompositeCall{
    std::byte{0xE8}, std::byte{0xCC}, std::byte{0x60}, std::byte{0x01},
    std::byte{0x00}};
constexpr std::array<std::byte, 5> kBackBufferCompositeCall{
    std::byte{0xE8}, std::byte{0x65}, std::byte{0x49}, std::byte{0x06},
    std::byte{0x00}};
constexpr std::array<std::byte, 5> kSnapshotCopyCall{
    std::byte{0xE8}, std::byte{0xB2}, std::byte{0x7C},
    std::byte{0x5B}, std::byte{0x00}};

using CompositeFn = int (__cdecl*)(int, void*, void*, float, float, int);
using SurfaceCopyFn = int (__cdecl*)(void*, void*);
using SurfaceStretchFn = int (__cdecl*)(void*, const void*, void*, const void*, int);

CompositeFn g_composite{};
SurfaceCopyFn g_surfaceCopy{};
SurfaceStretchFn g_surfaceStretch{};

void* Target(const ImageView& image, std::uint32_t rva) {
    const auto range = image.At(rva, 1u);
    return range ? const_cast<std::byte*>(range->data()) : nullptr;
}

int __cdecl CompositeHook(int mode, void* source, void* destination,
                          float sourceAspect, float targetAspect, int flags) {
    const auto aspect = display::CurrentAspectRatio();
    if (source && destination && aspect.valid && aspect.correctionRequired) {
        return g_surfaceCopy(source, destination);
    }
    return g_composite(mode, source, destination, sourceAspect, targetAspect,
                       flags);
}

int __cdecl SnapshotCopyHook(void* source, const void* sourceRect,
                             void* destination, const void* destinationRect,
                             int filter) {
    const auto aspect = display::CurrentAspectRatio();
    if (source && destination && !sourceRect && !destinationRect &&
        aspect.valid && aspect.correctionRequired) {
        return g_surfaceCopy(source, destination);
    }
    return g_surfaceStretch(source, sourceRect, destination, destinationRect,
                            filter);
}

} // namespace

bool RegisterCompositeCalls(patch::Transaction& calls,
                            const ImageView& image) {
    for (const auto site : std::array{
             std::pair{kFinalCompositeCallRva,
                       std::span<const std::byte>(kFinalCompositeCall)},
             std::pair{kBackBufferCompositeCallRva,
                       std::span<const std::byte>(kBackBufferCompositeCall)}}) {
        const auto replacement = patch::EncodeRelativeCall32(
            reinterpret_cast<std::uintptr_t>(Target(image, site.first)),
            reinterpret_cast<std::uintptr_t>(&CompositeHook));
        if (!replacement ||
            !calls.Add(site.first, site.second, *replacement)) return false;
    }
    return true;
}

bool RegisterSnapshotCall(patch::Transaction& calls,
                          const ImageView& image) {
    const auto replacement = patch::EncodeRelativeCall32(
        reinterpret_cast<std::uintptr_t>(
            Target(image, kSnapshotCopyCallRva)),
        reinterpret_cast<std::uintptr_t>(&SnapshotCopyHook));
    return replacement &&
        calls.Add(kSnapshotCopyCallRva, kSnapshotCopyCall, *replacement);
}

void Prepare(const ImageView& image) {
    g_composite = reinterpret_cast<CompositeFn>(Target(image, kCompositeRva));
    g_surfaceCopy =
        reinterpret_cast<SurfaceCopyFn>(Target(image, kSurfaceCopyRva));
    g_surfaceStretch =
        reinterpret_cast<SurfaceStretchFn>(Target(image, kSurfaceStretchRva));
}

} // namespace novafix::game::ff13_composition_aspect
