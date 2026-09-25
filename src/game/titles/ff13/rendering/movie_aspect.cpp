#include "game/titles/ff13/rendering/movie_aspect.h"

#include "display/aspect_ratio.h"
#include "display/aspect_runtime.h"
#include "game/core/image_view.h"
#include "patch/detour32.h"
#include "patch/relative_branch.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace novafix::game::ff13_movie_aspect {
namespace {

constexpr std::uint32_t kPresentRva = 0x1008CF0u - 0x400000u;
constexpr std::uint32_t kPrimitiveCallRva = 0x1009168u - 0x400000u;
constexpr std::uint32_t kPrimitiveRva = 0xEB72F0u - 0x400000u;
constexpr std::array<std::byte, 9> kPresentPrefix{
    std::byte{0x55}, std::byte{0x8B}, std::byte{0xEC},
    std::byte{0x83}, std::byte{0xEC}, std::byte{0x20},
    std::byte{0x89}, std::byte{0x4D}, std::byte{0xE4}};
constexpr std::array<std::byte, 5> kPrimitiveCall{
    std::byte{0xE8}, std::byte{0x83}, std::byte{0xE1},
    std::byte{0xEA}, std::byte{0xFF}};

using PresentFn = void* (__thiscall*)(void*, void*, int, int);
using PrimitiveFn = void (__cdecl*)(int, int, int, int, const void*);

patch::Detour32 g_presentDetour;
PresentFn g_present{};
PrimitiveFn g_primitive{};
thread_local display::AspectFitScale g_fit{};

void* __fastcall PresentHook(void* movie, void*, void* destination,
                             int a3, int a4) {
    const auto previous = g_fit;
    g_fit = {};
    if (movie && destination) {
        const auto* bytes = static_cast<const std::byte*>(destination);
        const auto width = *reinterpret_cast<const std::uint32_t*>(bytes + 0x0Cu);
        const auto height = *reinterpret_cast<const std::uint32_t*>(bytes + 0x10u);
        const auto aspect = display::CurrentAspectRatio();
        const bool referenceTarget = width == 1280u && height == 720u;
        const bool fillSixteenTen = aspect.valid &&
            display::IsSixteenTenAspect(aspect.width, aspect.height);
        g_fit = (referenceTarget || fillSixteenTen) && aspect.valid
            ? display::ResolveWideMovieScale(aspect.width, aspect.height)
            : display::ResolveWideMovieScale(width, height);
    }
    void* result = g_present(movie, destination, a3, a4);
    g_fit = previous;
    return result;
}

void __cdecl PrimitiveHook(int primitive, int count, int format,
                           int stride, const void* vertices) {
    struct Vertex {
        float x;
        float y;
        std::array<std::byte, 12> remaining;
    };
    static_assert(sizeof(Vertex) == 20u);
    if (!vertices || primitive != 6 || count != 4 || stride != 20 ||
        !g_fit.valid || !g_fit.correctionRequired) {
        g_primitive(primitive, count, format, stride, vertices);
        return;
    }
    std::array<Vertex, 4> corrected{};
    std::memcpy(corrected.data(), vertices, sizeof(corrected));
    for (auto& vertex : corrected) {
        vertex.x *= g_fit.x;
        vertex.y *= g_fit.y;
    }
    g_primitive(primitive, count, format, stride, corrected.data());
}

} // namespace

bool RegisterPrimitiveCall(patch::Transaction& transaction,
                           const ImageView& image) {
    const auto site = image.At(kPrimitiveCallRva, kPrimitiveCall.size());
    if (!site) return false;
    const auto replacement = patch::EncodeRelativeCall32(
        reinterpret_cast<std::uintptr_t>(site->data()),
        reinterpret_cast<std::uintptr_t>(&PrimitiveHook));
    return replacement && transaction.Add(
        kPrimitiveCallRva, kPrimitiveCall, *replacement);
}

void Prepare(const ImageView& image) {
    const auto bytes = image.At(kPrimitiveRva, 1u);
    g_primitive = bytes ? reinterpret_cast<PrimitiveFn>(
        const_cast<std::byte*>(bytes->data())) : nullptr;
}

bool InstallPresentDetour(const ImageView& image) {
    const auto bytes = image.At(kPresentRva, kPresentPrefix.size());
    return bytes && g_presentDetour.Install(
        "ff13-movie-aspect", const_cast<std::byte*>(bytes->data()),
        reinterpret_cast<void*>(&PresentHook), kPresentPrefix,
        reinterpret_cast<void**>(&g_present));
}

void Revert() {
    g_presentDetour.RevertAndClear(g_present);
    g_primitive = nullptr;
}

} // namespace novafix::game::ff13_movie_aspect
