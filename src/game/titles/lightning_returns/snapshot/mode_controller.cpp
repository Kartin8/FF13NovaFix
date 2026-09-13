#include "game/titles/lightning_returns/snapshot/mode_controller.h"

#include "diagnostics/hook_profiler.h"
#include "diagnostics/log.h"
#include "compat/addons/native_notifications.h"
#include "game/core/game_profile.h"
#include "game/core/image_view.h"
#include "game/titles/lightning_returns/snapshot/capture_pipeline.h"
#include "game/titles/lightning_returns/snapshot/message_editor_redirect.h"
#include "game/titles/lightning_returns/snapshot/outerworld_menu.h"
#include "game/titles/lightning_returns/ui/message_window.h"
#include "game/titles/lightning_returns/ui/addon_notification.h"
#include "game/titles/lightning_returns/ui/text_editor/editor.h"
#include "game/titles/lightning_returns/ui/text_editor/native_api.h"
#include "patch/relative_branch.h"
#include "patch/transaction.h"

#include <windows.h>
#include <d3d9.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace novafix::game::lr_snapshot {
namespace {

// PC retains profile-less Snapshot (platform feature 11 is clear), but two
// SNS-wide gates hide it. Bypass them only at Snapshot-owned call sites;
// leave network services and other Outerworld callers untouched
constexpr std::array<patch::RelativeCallSite, 6> kAvailabilityCalls{{
    {0x338D9Du, {0xE8, 0xEE, 0x47, 0x06, 0x00}},
    {0x3B0877u, {0xE8, 0x14, 0xCD, 0xFE, 0xFF}},
    {0x3F40B0u, {0xE8, 0xDB, 0x94, 0xFA, 0xFF}},
    {0x3F4333u, {0xE8, 0x58, 0x92, 0xFA, 0xFF}},
    {0x3F4460u, {0xE8, 0x2B, 0x91, 0xFA, 0xFF}},
    {0x3F4508u, {0xE8, 0x83, 0x90, 0xFA, 0xFF}},
}};

constexpr std::uint32_t kModalStateRva = 0x395C50u;
constexpr std::uint32_t kFieldConditionRva = 0x276B00u;
constexpr std::uint32_t kGameStateSlotRva = 0x1F50974u;
constexpr std::uint32_t kSnapshotContextSlotRva = 0x1F4F130u;
constexpr std::uint32_t kGameTextToWideRva = 0x003BE260u;
constexpr std::uint32_t kCreateSystemMessageRva = 0x003C1500u;
constexpr std::uint32_t kSetGuiLayerRva = 0x0044D540u;
constexpr std::uint32_t kGameLanguageRva = 0x00353E00u;
constexpr std::uint32_t kGameWideToTextRva = 0x003BE2C0u;
constexpr std::uint32_t kGameStringAssignRva = 0x003BAF00u;
constexpr std::uint32_t kTextLabelWrapperRva = 0x004849C0u;

using PredicateFn = int (__cdecl*)();
using FieldConditionFn = int (__cdecl*)(int);

INIT_ONCE g_initializeOnce = INIT_ONCE_STATIC_INIT;
PredicateFn g_modalState{};
FieldConditionFn g_fieldCondition{};
std::uintptr_t* g_gameStateSlot{};
std::uintptr_t* g_snapshotContextSlot{};
std::atomic<int> g_lastAvailabilityState{-1};

bool CanOpenPluginNativeMessage() {
    if (!lr_message_window::Available() || !g_modalState ||
        g_modalState() != 0) {
        return false;
    }
    return !lr_addon_notification::HasPending();
}

void* OpenPluginNativeMessage(
    std::wstring_view message, bool reportFailure) {
    std::vector<char> native;
    if (!lr_message_window::ConvertText(message, native)) {
        if (reportFailure) {
            Log("LR add-on API native status conversion failed");
        }
        return nullptr;
    }
    return lr_message_window::Open(
        native.data(), "add-on API", reportFailure);
}

int __cdecl SnapshotAvailabilityHook() {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.lr.snapshot-availability-total");
    const std::uintptr_t gameState =
        g_gameStateSlot ? *g_gameStateSlot : 0u;
    const bool appReady =
        gameState != 0u &&
        *reinterpret_cast<const std::uintptr_t*>(
            gameState + 736u * 4u) != 0u;
    const bool snsDisabledByPlatform =
        gameState != 0u &&
        (*reinterpret_cast<const std::uint32_t*>(
             gameState + 417u * 4u) & 4u) != 0u;
    const bool contextReady =
        g_snapshotContextSlot && *g_snapshotContextSlot != 0u;
    const std::uintptr_t context =
        contextReady ? *g_snapshotContextSlot : 0u;
    const bool fieldQueryReady =
        contextReady &&
        *reinterpret_cast<void* const*>(context + 518u * 4u) != nullptr &&
        *reinterpret_cast<const std::uint32_t*>(
            context + 520u * 4u) == 0u;
    const bool fieldAllowsSnapshot =
        fieldQueryReady && g_fieldCondition && g_fieldCondition(3) != 0;
    const bool modal = !g_modalState || g_modalState() != 0;
    const bool available =
        appReady && contextReady && fieldAllowsSnapshot && !modal;
    const int state = (appReady ? 1 : 0) |
                      (contextReady ? 2 : 0) |
                      (fieldQueryReady ? 4 : 0) |
                      (fieldAllowsSnapshot ? 8 : 0) |
                      (modal ? 16 : 0) |
                      (snsDisabledByPlatform ? 32 : 0);
    const int previous = g_lastAvailabilityState.exchange(state);
    if (previous != state) {
        Log("LR Snapshot availability: app=%d context=%d query=%d field=%d modal=%d platform_sns_disabled=%d result=%d",
            appReady, contextReady, fieldQueryReady, fieldAllowsSnapshot,
            modal, snsDisabledByPlatform, available);
    }
    return available;
}

void ClearBindings() {
    g_modalState = nullptr;
    g_fieldCondition = nullptr;
    g_gameStateSlot = nullptr;
    g_snapshotContextSlot = nullptr;
    outerworld_menu::ClearBindings();
    lr_snapshot_message_editor::ClearBindings();
    lr_text_editor::Configure({});
}

void RollBackInitialization(patch::Transaction& redirects) {
    bool clean = true;
    clean = capture_pipeline::RevertHooks() && clean;
    clean = outerworld_menu::RevertHooks() && clean;

    const bool redirectsClean =
        !redirects.IsApplied() || redirects.Revert();
    if (clean && redirectsClean) {
        ClearBindings();
    } else {
        clean = false;
    }
    if (!clean) {
        LogError("LR Snapshot initialization rollback incomplete: live hook bindings retained");
    }
}

BOOL CALLBACK Initialize(PINIT_ONCE, PVOID, PVOID*) {
    if (CurrentProfile().title != Title::LightningReturns ||
        !Supports(Capability::SnapshotPhotoMode)) {
        return TRUE;
    }

    const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
    if (!image) {
        LogWarning("LR Snapshot disabled: invalid executable image");
        return TRUE;
    }

    const auto modalState = image->At(kModalStateRva, 1u);
    const auto fieldCondition = image->At(kFieldConditionRva, 1u);
    const auto gameStateSlot =
        image->At(kGameStateSlotRva, sizeof(std::uintptr_t));
    const auto snapshotContextSlot =
        image->At(kSnapshotContextSlotRva, sizeof(std::uintptr_t));
    const auto gameTextToWide = image->At(kGameTextToWideRva, 1u);
    const auto gameLanguage = image->At(kGameLanguageRva, 1u);
    const auto gameWideToText = image->At(kGameWideToTextRva, 1u);
    const auto gameStringAssign = image->At(kGameStringAssignRva, 1u);
    const auto textLabelWrapper = image->At(kTextLabelWrapperRva, 1u);
    const auto createSystemMessage =
        image->At(kCreateSystemMessageRva, 1u);
    const auto setGuiLayer = image->At(kSetGuiLayerRva, 1u);
    if (!modalState || !fieldCondition || !gameStateSlot ||
        !snapshotContextSlot || !gameTextToWide || !gameLanguage ||
        !gameWideToText || !gameStringAssign || !textLabelWrapper ||
        !createSystemMessage || !setGuiLayer ||
        !outerworld_menu::HasExpectedContract(*image) ||
        !capture_pipeline::HasExpectedContract(*image) ||
        !lr_snapshot_message_editor::HasExpectedContract(*image) ||
        !lr_message_window::Bind(*image)) {
        LogWarning("LR Snapshot disabled: native contracts unavailable");
        return TRUE;
    }

    const auto bytes = image->Bytes();
    patch::Transaction callPatch(
        "lr-native-offline-snapshot",
        std::span<std::byte>(const_cast<std::byte*>(bytes.data()),
                            bytes.size()),
        patch::registry::Kind::Detour);

    g_modalState = reinterpret_cast<PredicateFn>(
        const_cast<std::byte*>(modalState->data()));
    g_fieldCondition = reinterpret_cast<FieldConditionFn>(
        const_cast<std::byte*>(fieldCondition->data()));
    g_gameStateSlot = reinterpret_cast<std::uintptr_t*>(
        const_cast<std::byte*>(gameStateSlot->data()));
    g_snapshotContextSlot = reinterpret_cast<std::uintptr_t*>(
        const_cast<std::byte*>(snapshotContextSlot->data()));
    if (!outerworld_menu::Bind(*image)) {
        ClearBindings();
        LogWarning("LR Snapshot disabled: Outerworld menu contracts unavailable");
        return TRUE;
    }

    lr_text_editor::Configure({
        reinterpret_cast<lr_text_editor::GameTextToWideFn>(
            const_cast<std::byte*>(gameTextToWide->data())),
        reinterpret_cast<lr_text_editor::CreateSystemMessageFn>(
            const_cast<std::byte*>(createSystemMessage->data())),
        reinterpret_cast<lr_text_editor::SetGuiLayerFn>(
            const_cast<std::byte*>(setGuiLayer->data())),
        reinterpret_cast<lr_text_editor::GameLanguageFn>(
            const_cast<std::byte*>(gameLanguage->data())),
        reinterpret_cast<lr_text_editor::GameWideToTextFn>(
            const_cast<std::byte*>(gameWideToText->data())),
        reinterpret_cast<lr_text_editor::GameStringAssignFn>(
            const_cast<std::byte*>(gameStringAssign->data())),
        reinterpret_cast<lr_text_editor::TextLabelWrapperFn>(
            const_cast<std::byte*>(textLabelWrapper->data())),
    });

    // Add-on messages need the close callback even when the startup report is off
    // The startup hook forwards immediately when no report is pending
    lr_addon_notification::Install();
    compat::native_notifications::RegisterBackend({
        &CanOpenPluginNativeMessage, &OpenPluginNativeMessage,
        &lr_message_window::Close});

    const bool availabilityDefined = patch::AddRelativeCalls(
        callPatch, *image, kAvailabilityCalls,
        reinterpret_cast<const void*>(&SnapshotAvailabilityHook));
    const bool outerworldDefined =
        outerworld_menu::AddRedirects(callPatch, *image);
    const bool composeMessageDefined =
        lr_snapshot_message_editor::AddRedirects(callPatch, *image);
    if (!availabilityDefined || !outerworldDefined ||
        !composeMessageDefined ||
        callPatch.Apply() != patch::ApplyStatus::Applied) {
        ClearBindings();
        LogWarning("LR Snapshot disabled: native offline route redirect failed");
        return TRUE;
    }

    if (!outerworld_menu::InstallHooks(*image) ||
        !capture_pipeline::InstallHooks(*image)) {
        RollBackInitialization(callPatch);
        return TRUE;
    }

    return TRUE;
}

} // namespace

void Pump() {
    InitOnceExecuteOnce(&g_initializeOnce, &Initialize, nullptr, nullptr);
}

void OnPresent(IDirect3DDevice9* device) {
    capture_pipeline::OnPresent(device);
}

} // namespace novafix::game::lr_snapshot
