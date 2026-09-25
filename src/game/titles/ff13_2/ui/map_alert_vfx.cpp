#include "game/titles/ff13_2/ui/map_alert_vfx.h"

#include "diagnostics/log.h"
#include "display/aspect_runtime.h"
#include "game/core/image_view.h"
#include "game/shared/ui/map_alert_transform.h"
#include "game/titles/ff13_2/ui/minimap_layout.h"
#include "patch/relative_branch.h"
#include "patch/transaction.h"
#include "settings/graphics_settings.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>

namespace novafix::game::xiii2_ui::map_alert_vfx {
namespace {

constexpr std::uint32_t kCreateCallRva = 0x008897FEu - 0x00400000u;
constexpr std::uint32_t kCreateRva = 0x0099C9C0u - 0x00400000u;
constexpr std::uint32_t kPositionRva = 0x0099A900u - 0x00400000u;
constexpr std::uint32_t kViewportWidthRva = 0x007A4B70u - 0x00400000u;
constexpr std::uint32_t kViewportHeightRva = 0x007A4B80u - 0x00400000u;
// The absolute dword operand after these opcodes is relocated by CEG/ASLR.
constexpr std::array<std::byte, 2> kPositionPrefix{
    std::byte{0x8B}, std::byte{0x0D}};
constexpr std::array<std::byte, 1> kViewportPrefix{std::byte{0xA1}};

using CreateFn = int* (__cdecl*)(
    int*, const char*, const float*, int, void*, int);
using PositionFn = int (__cdecl*)(int*, const float*, int);
using ViewportExtentFn = int (__cdecl*)();

std::unique_ptr<patch::Transaction> g_callPatch;
CreateFn g_create{};
PositionFn g_position{};
ViewportExtentFn g_width{};
ViewportExtentFn g_height{};
int* __cdecl CreateHook(
    int* output, const char* name, const float* matrix,
    int layer, void* owner, int flags) {
    if (!g_create || !matrix) {
        if (output) *output = -1;
        return output;
    }
    const auto graphics = settings::Active();
    if (MinimapHidden(graphics.minimapScale)) {
        if (output) *output = -1;
        return output;
    }
    const auto aspect = display::CurrentAspectRatio();
    if (!aspect.valid) {
        return g_create(output, name, matrix, layer, owner, flags);
    }
    const auto layout = ResolveVisibleMinimapLayout(
        aspect, graphics.minimapScale, graphics.roundMinimap,
        graphics.minimapHorizontalOffset,
        graphics.minimapVerticalOffset);

    std::array<float, 16> scaled{};
    std::memcpy(scaled.data(), matrix, sizeof(scaled));
    map_alert_transform::Apply(scaled, aspect,
                               layout.scaleX, layout.scaleY);
    int* const result = g_create(
        output, name, scaled.data(), layer, owner, flags);
    if (!result || *result < 0 || !g_position || !g_width || !g_height)
        return result;

    // 99A900/7A4CD0 convert absolute viewport pixels to centred VFX space.
    const auto [translateX, translateY] = MinimapClipTranslation(layout);
    const int viewportWidth = g_width();
    const int viewportHeight = g_height();
    if (viewportWidth <= 0 || viewportHeight <= 0) return result;
    // Only the round map needs the fixed screen-space beam-centre correction.
    constexpr float kRoundAlertOffsetPixels = 20.0f;
    const float roundAlertOffset = graphics.roundMinimap
        ? kRoundAlertOffsetPixels * static_cast<float>(viewportHeight) /
              static_cast<float>(aspect.height)
        : 0.0f;
    const std::array<float, 4> position{
        (1.0f + translateX) * static_cast<float>(viewportWidth) * 0.5f,
        (1.0f - translateY) * static_cast<float>(viewportHeight) * 0.5f +
            roundAlertOffset,
        0.0f, 1.0f};
    g_position(result, position.data(), 0);
    return result;
}

} // namespace

void Install(const ImageView& image) {
    const auto target = [&image](std::uint32_t rva) -> void* {
        const auto bytes = image.At(rva, 1u);
        return bytes ? const_cast<std::byte*>(bytes->data()) : nullptr;
    };
    auto patch = std::make_unique<patch::Transaction>(
        "ff13-2-map-alert-vfx-layout",
        std::span<std::byte>(const_cast<std::byte*>(image.Bytes().data()),
                             image.Bytes().size()),
        patch::registry::Kind::Detour);
    void* const site = target(kCreateCallRva);
    void* const creator = target(kCreateRva);
    void* const positionFn = target(kPositionRva);
    void* const widthFn = target(kViewportWidthRva);
    void* const heightFn = target(kViewportHeightRva);
    const auto positionBytes = image.At(kPositionRva, kPositionPrefix.size());
    const auto widthBytes = image.At(kViewportWidthRva, kViewportPrefix.size());
    const auto heightBytes = image.At(kViewportHeightRva, kViewportPrefix.size());
    const auto expected = site && creator ? patch::EncodeRelativeCall32(
        reinterpret_cast<std::uintptr_t>(site),
        reinterpret_cast<std::uintptr_t>(creator)) : std::nullopt;
    const auto replacement = site ? patch::EncodeRelativeCall32(
        reinterpret_cast<std::uintptr_t>(site),
        reinterpret_cast<std::uintptr_t>(&CreateHook)) : std::nullopt;
    if (!positionFn || !positionBytes || !widthFn || !heightFn ||
        !widthBytes || !heightBytes ||
        std::memcmp(positionBytes->data(), kPositionPrefix.data(),
                    kPositionPrefix.size()) != 0 ||
        std::memcmp(widthBytes->data(), kViewportPrefix.data(),
                    kViewportPrefix.size()) != 0 ||
        std::memcmp(heightBytes->data(), kViewportPrefix.data(),
                    kViewportPrefix.size()) != 0 ||
        !expected || !replacement ||
        !patch->Add(kCreateCallRva, *expected, *replacement)) {
        LogWarning("XIII-2 map-alert VFX layout unavailable: call contract mismatch");
        return;
    }
    g_create = reinterpret_cast<CreateFn>(creator);
    g_position = reinterpret_cast<PositionFn>(positionFn);
    g_width = reinterpret_cast<ViewportExtentFn>(widthFn);
    g_height = reinterpret_cast<ViewportExtentFn>(heightFn);
    const auto status = patch->Apply();
    if (status == patch::ApplyStatus::Applied ||
        status == patch::ApplyStatus::AlreadyApplied) {
        g_callPatch = std::move(patch);
    } else {
        LogWarning("XIII-2 map-alert VFX layout unavailable: status=%s",
                   patch::ApplyStatusName(status));
    }
}

} // namespace novafix::game::xiii2_ui::map_alert_vfx
