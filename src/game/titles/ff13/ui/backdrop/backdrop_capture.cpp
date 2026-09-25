#include "game/titles/ff13/ui/backdrop/backdrop_capture.h"

#include "display/aspect_runtime.h"
#include "game/core/image_view.h"
#include "game/titles/ff13/camera/aspect_ratio_internal.h"
#include "game/titles/ff13/ui/backdrop/backdrop_geometry.h"
#include "patch/relative_branch.h"
#include "patch/transaction.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace novafix::game::ff13_ui::backdrop_capture {
namespace {

constexpr std::uint32_t kCaptureAllocateCallRva = 0x9B5F96u - 0x400000u;
constexpr std::uint32_t kCaptureFlushCallRva = 0x9B647Bu - 0x400000u;
constexpr std::uint32_t kGuiAllocateRva = 0x93CE40u - 0x400000u;
constexpr std::uint32_t kGuiFlushRva = 0x8E3BE0u - 0x400000u;

struct BackdropAllocationSite {
    std::uint32_t rva;
    bool captured;
};
constexpr std::array<BackdropAllocationSite, 4> kBackdropAllocationSites{{
    {0xA3FD59u - 0x400000u, false},
    {0xA401E5u - 0x400000u, false},
    {0xA40969u - 0x400000u, true},
    {0xA410D0u - 0x400000u, false},
}};
constexpr std::array<std::uint32_t, 4> kBackdropFlushSites{{
    0xA401C3u - 0x400000u,
    0xA406D9u - 0x400000u,
    0xA40DA3u - 0x400000u,
    0xA4152Eu - 0x400000u,
}};
constexpr std::array<std::byte, 5> kCaptureAllocateCall{
    std::byte{0xE8}, std::byte{0xA5}, std::byte{0x6E}, std::byte{0xF8},
    std::byte{0xFF}};
constexpr std::array<std::byte, 5> kCaptureFlushCall{
    std::byte{0xE8}, std::byte{0x60}, std::byte{0xD7}, std::byte{0xF2},
    std::byte{0xFF}};

using GuiAllocateFn = void* (__thiscall*)(void*, int, int);
GuiAllocateFn g_guiAllocate{};
void* g_guiFlush{};

struct CaptureVertex {
    float x;
    float y;
    std::array<std::byte, 24> remaining;
};
static_assert(sizeof(CaptureVertex) == 32u);
thread_local CaptureVertex* g_captureVertices{};
struct PendingBackdropQuad {
    BackdropVertex* vertices{};
    void* renderer{};
    BackdropMapping mapping{};
};
thread_local PendingBackdropQuad g_backdropQuad{};

void* Target(const ImageView& image, std::uint32_t rva) {
    const auto range = image.At(rva, 1u);
    return range ? const_cast<std::byte*>(range->data()) : nullptr;
}

bool AddCall(patch::Transaction& calls, const ImageView& image,
             std::uint32_t rva, std::span<const std::byte> expected,
             std::uintptr_t hook) {
    const auto replacement = patch::EncodeRelativeCall32(
        reinterpret_cast<std::uintptr_t>(Target(image, rva)), hook);
    return replacement && calls.Add(rva, expected, *replacement);
}

void* __fastcall CaptureAllocateHook(void* renderer, void*, int primitive,
                                     int count) {
    void* vertices = g_guiAllocate(renderer, primitive, count);
    g_captureVertices = primitive == 6 && count == 4 &&
                            ff13_aspect_ratio::g_guiDrawDepth != 0u
        ? static_cast<CaptureVertex*>(vertices) : nullptr;
    return vertices;
}

void __cdecl CorrectCaptureQuad() {
    CaptureVertex* vertices = g_captureVertices;
    g_captureVertices = nullptr;
    const auto aspect = display::CurrentAspectRatio();
    if (!vertices || !aspect.valid || !aspect.correctionRequired) return;
    for (std::size_t index = 0u; index != 4u; ++index) {
        vertices[index].x = 640.0f +
            (vertices[index].x - 640.0f) / aspect.safeScaleX;
        vertices[index].y = 360.0f +
            (vertices[index].y - 360.0f) / aspect.safeScaleY;
    }
}

void __declspec(naked) CaptureFlushHook() {
    __asm {
        pushfd
        pushad
        call CorrectCaptureQuad
        popad
        popfd
        jmp dword ptr [g_guiFlush]
    }
}

void* AllocateBackdropQuad(void* renderer, int primitive, int count,
                           BackdropMapping mapping) {
    g_backdropQuad = {};
    void* vertices = g_guiAllocate(renderer, primitive, count);
    if (ff13_aspect_ratio::g_guiDrawDepth != 0u &&
        primitive == 6 && count == 4 && vertices) {
        g_backdropQuad = {
            static_cast<BackdropVertex*>(vertices), renderer, mapping};
    }
    return vertices;
}

void* __fastcall ArtBackdropAllocateHook(void* renderer, void*, int primitive,
                                         int count) {
    return AllocateBackdropQuad(renderer, primitive, count,
        BackdropMapping::CoverArt);
}

void* __fastcall SnapshotBackdropAllocateHook(void* renderer, void*,
                                              int primitive, int count) {
    return AllocateBackdropQuad(renderer, primitive, count,
        BackdropMapping::CapturedViewport);
}

void __cdecl CorrectBackdropQuad(void* renderer) {
    const auto pending = g_backdropQuad;
    g_backdropQuad = {};
    if (!pending.vertices || pending.renderer != renderer ||
        ff13_aspect_ratio::g_guiDrawDepth == 0u) return;
    FitBackdropQuad(
        std::span<BackdropVertex, 4>(pending.vertices, 4),
        display::CurrentAspectRatio(), pending.mapping);
}

void __declspec(naked) BackdropFlushHook() {
    __asm {
        pushfd
        pushad
        push ecx
        call CorrectBackdropQuad
        add esp, 4
        popad
        popfd
        jmp dword ptr [g_guiFlush]
    }
}

} // namespace

bool RegisterAllocationCalls(patch::Transaction& calls,
                             const ImageView& image) {
    for (const auto& site : kBackdropAllocationSites) {
        const auto expected = patch::EncodeRelativeCall32(
            reinterpret_cast<std::uintptr_t>(Target(image, site.rva)),
            reinterpret_cast<std::uintptr_t>(Target(image, kGuiAllocateRva)));
        const auto hook = site.captured
            ? reinterpret_cast<std::uintptr_t>(&SnapshotBackdropAllocateHook)
            : reinterpret_cast<std::uintptr_t>(&ArtBackdropAllocateHook);
        if (!expected || !AddCall(calls, image, site.rva, *expected, hook))
            return false;
    }
    return true;
}

bool RegisterFlushCalls(patch::Transaction& calls, const ImageView& image) {
    for (const auto rva : kBackdropFlushSites) {
        const auto expected = patch::EncodeRelativeCall32(
            reinterpret_cast<std::uintptr_t>(Target(image, rva)),
            reinterpret_cast<std::uintptr_t>(Target(image, kGuiFlushRva)));
        if (!expected || !AddCall(calls, image, rva, *expected,
                reinterpret_cast<std::uintptr_t>(&BackdropFlushHook)))
            return false;
    }
    return true;
}

bool RegisterCaptureCalls(patch::Transaction& calls, const ImageView& image) {
    return AddCall(calls, image, kCaptureAllocateCallRva,
                   kCaptureAllocateCall,
                   reinterpret_cast<std::uintptr_t>(&CaptureAllocateHook)) &&
           AddCall(calls, image, kCaptureFlushCallRva,
                   kCaptureFlushCall,
                   reinterpret_cast<std::uintptr_t>(&CaptureFlushHook));
}

void Prepare(const ImageView& image) {
    g_guiAllocate = reinterpret_cast<GuiAllocateFn>(Target(image, kGuiAllocateRva));
    g_guiFlush = Target(image, kGuiFlushRva);
}

} // namespace novafix::game::ff13_ui::backdrop_capture
