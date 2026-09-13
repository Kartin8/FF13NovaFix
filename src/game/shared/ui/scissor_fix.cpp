#include "game/shared/ui/scissor_fix.h"

#include "diagnostics/log.h"
#include "game/core/game_profile.h"
#include "game/core/image_view.h"
#include "patch/transaction.h"
#include "settings/graphics_settings.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace novafix::game::scissor_fix {
namespace {

struct PatchSite {
    std::uint32_t rva;
    std::span<const std::uint8_t> expected;
};

constexpr std::array<std::uint8_t, 3> kFmulStackF4{0xD8, 0x4D, 0xF4};
constexpr std::array<std::uint8_t, 3> kFmulStackF0{0xD8, 0x4D, 0xF0};
constexpr std::array<std::uint8_t, 5> kSettingsScaleCall{0xE8, 0x45, 0x60, 0x0F, 0x00};
constexpr std::array<std::uint8_t, 4> kFmulEsp7C{0xD8, 0x4C, 0x24, 0x7C};
constexpr std::array<std::uint8_t, 7> kFmulEsp90{0xD8, 0x8C, 0x24, 0x90, 0x00, 0x00, 0x00};
constexpr std::array<std::uint8_t, 7> kFmulEsp84{0xD8, 0x8C, 0x24, 0x84, 0x00, 0x00, 0x00};
constexpr std::array<std::uint8_t, 7> kFmulEsp98{0xD8, 0x8C, 0x24, 0x98, 0x00, 0x00, 0x00};

constexpr std::array<PatchSite, 9> kPatchSites{{
    {0x61654Cu, kFmulStackF4},
    {0x616571u, kFmulStackF0},
    {0x616596u, kFmulStackF4},
    {0x6165BBu, kFmulStackF0},
    {0x572B26u, kSettingsScaleCall},
    {0x668DE9u, kFmulEsp7C},
    {0x668E1Eu, kFmulEsp90},
    {0x668E56u, kFmulEsp84},
    {0x668E91u, kFmulEsp98},
}};

INIT_ONCE g_initializeOnce = INIT_ONCE_STATIC_INIT;
std::unique_ptr<patch::Transaction> g_transaction;
bool g_supported = false;
std::atomic_bool g_active{false};
thread_local unsigned g_bypassDepth = 0;

BOOL CALLBACK Initialize(PINIT_ONCE, PVOID, PVOID*) {
    const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
    if (!image || !Supports(Capability::HighResolutionScissor)) {
        LogWarning("High-resolution UI clipping fix disabled: unsupported executable image");
        return TRUE;
    }

    auto bytes = image->Bytes();
    auto writable = std::span<std::byte>(const_cast<std::byte*>(bytes.data()), bytes.size());
    g_transaction = std::make_unique<patch::Transaction>(
        "normalize-game-scissor-coordinates", writable);
    for (const PatchSite& site : kPatchSites) {
        const auto siteRange = image->At(site.rva, site.expected.size());
        if (!siteRange) {
            g_transaction.reset();
            LogWarning("High-resolution UI clipping fix disabled: patch RVA 0x%X is out of range",
                site.rva);
            return TRUE;
        }
        std::array<std::byte, 7> nops{};
        nops.fill(std::byte{0x90});
        if (!g_transaction->Add(site.rva, std::as_bytes(site.expected),
                                std::span<const std::byte>(nops.data(), site.expected.size()))) {
            g_transaction.reset();
            LogWarning("High-resolution UI clipping fix disabled: invalid patch at RVA 0x%X",
                site.rva);
            return TRUE;
        }
    }
    g_supported = true;
    return TRUE;
}

} // namespace

ScopedBypass::ScopedBypass() {
    ++g_bypassDepth;
}

ScopedBypass::~ScopedBypass() {
    if (g_bypassDepth > 0) --g_bypassDepth;
}

void Initialize() {
    InitOnceExecuteOnce(&g_initializeOnce, &Initialize, nullptr, nullptr);
}

void Pump() {
    if (!g_supported || !g_transaction) return;

    const bool desired = settings::Active().highResolutionScissorFix;
    if (desired && !g_transaction->IsApplied()) {
        const patch::ApplyStatus status = g_transaction->Apply();
        if (status == patch::ApplyStatus::Applied ||
            status == patch::ApplyStatus::AlreadyApplied) {
            g_active.store(true, std::memory_order_release);
        } else {
            g_supported = false;
            LogWarning("High-resolution UI clipping fix disabled: patch status=%s",
                patch::ApplyStatusName(status));
        }
    } else if (!desired && g_transaction->IsApplied()) {
        if (g_transaction->Revert()) {
            g_active.store(false, std::memory_order_release);
            Log("High-resolution UI clipping fix disabled: game code restored");
        }
    }
}

bool IsActive() {
    return g_active.load(std::memory_order_acquire);
}

bool ShouldTransformCurrentThread() {
    return IsActive() && g_bypassDepth == 0;
}

} // namespace novafix::game::scissor_fix
