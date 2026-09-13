#include "game/titles/ff13_2/rendering/hud_sampling.h"

#include "diagnostics/log.h"
#include "game/core/game_profile.h"
#include "game/core/image_view.h"
#include "patch/transaction.h"

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace novafix::game::xiii2_hud_sampling {
namespace {

constexpr std::uint32_t kSourceRectBranchRva = 0x004E0135u;
constexpr std::size_t kWidthDecrementOffset = 6u;
constexpr std::size_t kHeightDecrementOffset = 11u;

constexpr std::array<std::uint8_t, 12> kNativeBytes{
    0x74, 0x0A, 0x85, 0xC0, 0x7E, 0x01,
    0x48, 0x85, 0xFF, 0x7E, 0x01, 0x4F};
constexpr std::array<std::uint8_t, 1> kDecrementEax{0x48};
constexpr std::array<std::uint8_t, 1> kDecrementEdi{0x4F};
constexpr std::array<std::uint8_t, 1> kNop{0x90};

INIT_ONCE g_installOnce = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK InstallOnce(PINIT_ONCE, PVOID, PVOID*) {
    if (CurrentProfile().title != Title::FinalFantasyXIII2 ||
        !Supports(Capability::HighResolutionHudSampling)) {
        return TRUE;
    }

    const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
    const auto site = image
        ? image->At(kSourceRectBranchRva, kNativeBytes.size())
        : std::nullopt;
    if (!image || !site ||
        std::memcmp(site->data(), kNativeBytes.data(), kNativeBytes.size()) != 0) {
        LogWarning("XIII-2 high-resolution HUD sampling correction disabled: executable contract mismatch");
        return TRUE;
    }

    auto imageBytes = image->Bytes();
    auto writable = std::span<std::byte>(
        const_cast<std::byte*>(imageBytes.data()), imageBytes.size());
    patch::Transaction transaction(
        "xiii2-high-resolution-hud-sampling", writable);
    const bool valid =
        transaction.Add(kSourceRectBranchRva + kWidthDecrementOffset,
                        std::as_bytes(std::span{kDecrementEax}),
                        std::as_bytes(std::span{kNop})) &&
        transaction.Add(kSourceRectBranchRva + kHeightDecrementOffset,
                        std::as_bytes(std::span{kDecrementEdi}),
                        std::as_bytes(std::span{kNop}));
    if (!valid) {
        LogWarning("XIII-2 high-resolution HUD sampling correction disabled: invalid patch definition");
        return TRUE;
    }

    const patch::ApplyStatus status = transaction.Apply();
    if (status != patch::ApplyStatus::Applied &&
        status != patch::ApplyStatus::AlreadyApplied) {
        LogWarning("XIII-2 high-resolution HUD sampling correction disabled: patch status=%s",
                   patch::ApplyStatusName(status));
        return TRUE;
    }

    return TRUE;
}

} // namespace

void Install() {
    InitOnceExecuteOnce(&g_installOnce, &InstallOnce, nullptr, nullptr);
}

} // namespace novafix::game::xiii2_hud_sampling
