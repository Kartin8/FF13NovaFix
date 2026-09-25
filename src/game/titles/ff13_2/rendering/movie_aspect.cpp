#include "game/titles/ff13_2/rendering/movie_aspect.h"

#include "diagnostics/log.h"
#include "display/aspect_ratio.h"
#include "display/aspect_runtime.h"
#include "game/core/image_view.h"
#include "patch/detour32.h"
#include "patch/relative_branch.h"
#include "patch/transaction.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>

namespace novafix::game::xiii2_movie_aspect {
namespace {

constexpr std::uint32_t kPresentRva = 0x0061F0C0u;
constexpr std::uint32_t kPrimitiveCallRva = 0x008250D9u;
constexpr std::uint32_t kPrimitiveSubmitRva = 0x00824A60u;
constexpr std::uint32_t kCompositeModeRva = 0x00809FDBu;
constexpr std::size_t kMovieWidthOffset = 268u;
constexpr std::size_t kMovieHeightOffset = 272u;
constexpr std::array<std::byte, 12> kPresentPrefix{
    std::byte{0x56}, std::byte{0x57}, std::byte{0x8B}, std::byte{0xF1},
    std::byte{0x8D}, std::byte{0xBE}, std::byte{0xB0}, std::byte{0x00},
    std::byte{0x00}, std::byte{0x00}, std::byte{0x57}, std::byte{0xFF}};
constexpr std::array<std::byte, 5> kPrimitiveCall{
    std::byte{0xE8}, std::byte{0x82}, std::byte{0xF9},
    std::byte{0xFF}, std::byte{0xFF}};
constexpr std::array<std::byte, 3> kCompositeModeLoad{
    std::byte{0x8B}, std::byte{0x78}, std::byte{0x30}};
constexpr std::array<std::byte, 3> kFullScreenCompositeMode{
    std::byte{0x33}, std::byte{0xFF}, std::byte{0x90}};

using PresentFn = void (__thiscall*)(void*, void*, int, int);
using PrimitiveSubmitFn = void (__cdecl*)(int, int, int, int, const void*);

struct MovieDimensions {
    std::uint32_t width{};
    std::uint32_t height{};
};
struct MovieVertex {
    float x{};
    float y{};
    std::array<std::byte, 12> remaining{};
};
static_assert(sizeof(MovieVertex) == 20u);

patch::Detour32 g_presentDetour;
std::unique_ptr<patch::Transaction> g_primitivePatch;
std::unique_ptr<patch::Transaction> g_compositePatch;
PresentFn g_present{};
PrimitiveSubmitFn g_primitive{};
thread_local MovieDimensions g_dimensions{};

void __fastcall PresentHook(
    void* manager, void*, void* destination, int a3, int a4) {
    const PresentFn original = g_present;
    if (!original) return;

    const MovieDimensions previous = g_dimensions;
    if (manager) {
        const auto* bytes = static_cast<const std::byte*>(manager);
        g_dimensions.width = *reinterpret_cast<const std::uint32_t*>(
            bytes + kMovieWidthOffset);
        g_dimensions.height = *reinterpret_cast<const std::uint32_t*>(
            bytes + kMovieHeightOffset);
    }
    original(manager, destination, a3, a4);
    g_dimensions = previous;
}

void __cdecl PrimitiveSubmitHook(
    int primitiveType, int enabled, int format, int stride,
    const void* vertices) {
    const PrimitiveSubmitFn native = g_primitive;
    if (!native) return;

    const display::AspectRatio target = display::CurrentAspectRatio();
    const display::AspectFitScale fit =
        g_dimensions.width != 0u && g_dimensions.height != 0u
        ? display::ResolveWideMovieScale(target.width, target.height)
        : display::AspectFitScale{};
    if (!vertices || primitiveType != 6 || enabled != 4 || stride != 20 ||
        !fit.valid || !fit.correctionRequired) {
        native(primitiveType, enabled, format, stride, vertices);
        return;
    }

    std::array<MovieVertex, 4> corrected{};
    std::memcpy(corrected.data(), vertices, sizeof(corrected));
    for (MovieVertex& vertex : corrected) {
        vertex.x *= fit.x;
        vertex.y *= fit.y;
    }
    native(primitiveType, enabled, format, stride, corrected.data());
}

bool Matches(std::span<const std::byte> actual,
             std::span<const std::byte> expected) {
    return actual.size() == expected.size() &&
        std::memcmp(actual.data(), expected.data(), expected.size()) == 0;
}

} // namespace

bool Prepare(const ImageView& image) {
    const auto present = image.At(kPresentRva, kPresentPrefix.size());
    const auto primitiveCall = image.At(kPrimitiveCallRva, kPrimitiveCall.size());
    const auto primitiveSubmit = image.At(kPrimitiveSubmitRva, 1u);
    const auto compositeMode = image.At(kCompositeModeRva, kCompositeModeLoad.size());
    if (!present || !primitiveCall || !primitiveSubmit || !compositeMode ||
        !Matches(*present, kPresentPrefix) ||
        !Matches(*primitiveCall, kPrimitiveCall) ||
        !Matches(*compositeMode, kCompositeModeLoad)) return false;
    g_primitive = reinterpret_cast<PrimitiveSubmitFn>(
        const_cast<std::byte*>(primitiveSubmit->data()));
    return true;
}

bool InstallPresentDetour(const ImageView& image) {
    const auto present = image.At(kPresentRva, kPresentPrefix.size());
    return present && g_presentDetour.Install(
        "ff13-2-prerender-aspect-scope",
        const_cast<std::byte*>(present->data()),
        reinterpret_cast<void*>(&PresentHook), kPresentPrefix,
        reinterpret_cast<void**>(&g_present));
}

bool ApplyPrimitivePatch(const ImageView& image) {
    const auto call = image.At(kPrimitiveCallRva, kPrimitiveCall.size());
    if (!call) return false;
    const auto replacement = patch::EncodeRelativeCall32(
        reinterpret_cast<std::uintptr_t>(call->data()),
        reinterpret_cast<std::uintptr_t>(&PrimitiveSubmitHook));
    auto patch = std::make_unique<patch::Transaction>(
        "ff13-2-prerender-aspect-fit",
        std::span<std::byte>(const_cast<std::byte*>(call->data()), call->size()),
        patch::registry::Kind::Detour);
    if (!replacement || !patch->Add(0u, kPrimitiveCall, *replacement)) return false;
    const auto status = patch->Apply();
    if (status != patch::ApplyStatus::Applied &&
        status != patch::ApplyStatus::AlreadyApplied) {
        LogWarning("XIII-2 aspect ratio correction disabled: prerender patch status=%s",
                   patch::ApplyStatusName(status));
        return false;
    }
    g_primitivePatch = std::move(patch);
    return true;
}

bool ApplyCompositePatch(const ImageView& image) {
    const auto mode = image.At(kCompositeModeRva, kCompositeModeLoad.size());
    if (!mode) return false;
    auto patch = std::make_unique<patch::Transaction>(
        "ff13-2-final-composite-aspect",
        std::span<std::byte>(const_cast<std::byte*>(mode->data()), mode->size()));
    if (!patch->Add(0u, kCompositeModeLoad, kFullScreenCompositeMode)) return false;
    const auto status = patch->Apply();
    if (status != patch::ApplyStatus::Applied &&
        status != patch::ApplyStatus::AlreadyApplied) {
        LogWarning("XIII-2 aspect ratio correction disabled: final composite patch status=%s",
                   patch::ApplyStatusName(status));
        return false;
    }
    g_compositePatch = std::move(patch);
    return true;
}

void Revert() {
    if (g_compositePatch) {
        g_compositePatch->Revert();
        g_compositePatch.reset();
    }
    if (g_primitivePatch) {
        g_primitivePatch->Revert();
        g_primitivePatch.reset();
    }
    g_presentDetour.RevertAndClear(g_present);
    g_primitive = nullptr;
}

} // namespace novafix::game::xiii2_movie_aspect
