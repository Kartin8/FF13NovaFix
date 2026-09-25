#include "game/titles/ff13/ui/map_alert_vfx.h"

#include "diagnostics/log.h"
#include "display/aspect_runtime.h"
#include "game/core/image_view.h"
#include "game/shared/ui/map_alert_transform.h"
#include "game/titles/ff13/ui/minimap_layout.h"
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

namespace novafix::game::ff13_ui::map_alert_vfx {
namespace {

constexpr std::uint32_t kCreateCallRva = 0xA02E98u - 0x400000u;
constexpr std::uint32_t kCreateRva = 0xB73E70u - 0x400000u;
constexpr std::uint32_t kPositionRva = 0xB74540u - 0x400000u;
constexpr std::uint32_t kViewportWidthRva = 0x905F70u - 0x400000u;
constexpr std::uint32_t kViewportHeightRva = 0x905F90u - 0x400000u;
constexpr std::size_t kCenterXOffset = 1992u * sizeof(int);
constexpr std::size_t kCenterYOffset = 1993u * sizeof(int);
constexpr std::array<std::byte, 6> kPositionPrefix{
    std::byte{0x55}, std::byte{0x8B}, std::byte{0xEC},
    std::byte{0x83}, std::byte{0xEC}, std::byte{0x0C}};
constexpr std::array<std::byte, 7> kViewportPrefix{
    std::byte{0x55}, std::byte{0x8B}, std::byte{0xEC},
    std::byte{0x83}, std::byte{0xEC}, std::byte{0x08},
    std::byte{0xA1}};

using CreateFn = int (__cdecl*)(const char*, const float*, int, void*, int);
using PositionFn = int (__cdecl*)(int, const float*, int);
using ViewportExtentFn = int (__cdecl*)();

std::unique_ptr<patch::Transaction> g_callPatch;
CreateFn g_create{};
PositionFn g_position{};
ViewportExtentFn g_width{};
ViewportExtentFn g_height{};
int __cdecl CreateHook(
    const char* name, const float* matrix, int layer, void* owner, int flags) {
    if (!g_create || !matrix) return -1;
    const auto graphics = settings::Active();
    if (MinimapHidden(graphics.minimapScale)) return -1;

    const auto aspect = display::CurrentAspectRatio();
    if (!owner || !aspect.valid) {
        return g_create(name, matrix, layer, owner, flags);
    }
    // The callsite passes GuiNaviMap product+112 as its VFX owner.
    auto* const product = static_cast<std::byte*>(owner) - 112u;
    const float centerX = static_cast<float>(
        *reinterpret_cast<int*>(product + kCenterXOffset));
    const float centerY = static_cast<float>(
        *reinterpret_cast<int*>(product + kCenterYOffset));
    const auto layout = ResolveVisibleMinimapLayout(
        aspect, graphics.minimapScale,
        graphics.minimapHorizontalOffset,
        graphics.minimapVerticalOffset,
        centerX, centerY);

    std::array<float, 16> scaled{};
    std::memcpy(scaled.data(), matrix, sizeof(scaled));
    map_alert_transform::Apply(scaled, aspect, layout.scale, layout.scale);
    const int id = g_create(name, scaled.data(), layer, owner, flags);
    if (id < 0 || !g_position || !g_width || !g_height) return id;

    // B74540/905680 convert absolute viewport pixels to centred VFX space.
    const auto [translateX, translateY] = MinimapClipTranslation(layout);
    const int viewportWidth = g_width();
    const int viewportHeight = g_height();
    if (viewportWidth <= 0 || viewportHeight <= 0) return id;
    const std::array<float, 4> position{
        (1.0f + translateX) * static_cast<float>(viewportWidth) * 0.5f,
        (1.0f - translateY) * static_cast<float>(viewportHeight) * 0.5f +
            (settings::RoundMinimapEnabled()
                ? NativeMinimapTiltCenterLift() * layout.scale *
                      aspect.safeScaleY
                : 0.0f),
        0.0f, 1.0f};
    g_position(id, position.data(), 0);
    return id;
}

} // namespace

void Install(const ImageView& image) {
    const auto target = [&image](std::uint32_t rva) -> void* {
        const auto bytes = image.At(rva, 1u);
        return bytes ? const_cast<std::byte*>(bytes->data()) : nullptr;
    };
    auto patch = std::make_unique<patch::Transaction>(
        "ff13-map-alert-vfx-layout",
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
        LogWarning("XIII map-alert VFX layout unavailable: call contract mismatch");
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
        LogWarning("XIII map-alert VFX layout unavailable: status=%s",
                   patch::ApplyStatusName(status));
    }
}

} // namespace novafix::game::ff13_ui::map_alert_vfx
