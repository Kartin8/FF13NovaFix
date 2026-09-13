#include "game/titles/lightning_returns/ui/addon_notification.h"

#include "diagnostics/log.h"
#include "compat/addons/native_notifications.h"
#include "game/core/game_profile.h"
#include "game/core/image_view.h"
#include "game/titles/lightning_returns/ui/message_window.h"
#include "input/win32_input.h"
#include "input/xinput_backend.h"
#include "patch/detour32.h"
#include "settings/performance_settings.h"

#include <windows.h>
#include <xinput.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace novafix::game::lr_addon_notification {
namespace {

constexpr std::uint32_t kModalStateRva = 0x395C50u;
constexpr std::uint32_t kCompanyLogoIsRunningRva = 0x004B6690u;
constexpr std::uint32_t kGuiMessageCloseRva = 0x0043E560u;

constexpr std::array<std::uint8_t, 13> kCompanyLogoIsRunningPrefix{
    0x33, 0xC0, 0x83, 0xB9, 0x1C, 0x01, 0x00,
    0x00, 0x06, 0x0F, 0x95, 0xC0, 0xC3};
constexpr std::array<std::uint8_t, 16> kGuiMessageClosePrefix{
    0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1, 0x8B, 0x4E,
    0x34, 0x6A, 0x02, 0xE8, 0x70, 0x51, 0xF5, 0xFF};

using PredicateFn = int (__cdecl*)();
using CompanyLogoIsRunningFn = int (__thiscall*)(void*);
using GuiMessageCloseFn = int (__thiscall*)(void*, int);

struct PendingNotification {
    std::wstring message;
    std::wstring signature;
    bool opening{};
    bool opened{};
    bool closed{};
    bool openFailureLogged{};
    void* product{};
};

struct LocalizedHeading {
    int language;
    const wchar_t* text;
};

constexpr std::array<LocalizedHeading, 8> kHeadings{{
    {0, L"\x6B63\x5E38\x306B\x8AAD\x307F\x8FBC\x307E\x308C\x305F\x30A2\x30C9\x30AA\x30F3:"},
    {1, L"Successfully loaded add-ons:"},
    {3, L"Componenti aggiuntivi caricati correttamente:"},
    {4, L"Erfolgreich geladene Add-ons:"},
    {5, L"Extensions chargees avec succes :"},
    {6, L"Complementos cargados correctamente:"},
    {8, L"\xC131\xACF5\xC801\xC73C\xB85C \xBD88\xB7EC\xC628 \xC560\xB4DC\xC628:"},
    {10, L"\x6210\x529F\x8F09\x5165\x7684\x9644\x52A0\x5143\x4EF6\xFF1A"},
}};

INIT_ONCE g_installOnce = INIT_ONCE_STATIC_INIT;
patch::Detour32 g_companyLogoDetour;
patch::Detour32 g_messageCloseDetour;
CompanyLogoIsRunningFn g_originalCompanyLogo{};
GuiMessageCloseFn g_originalMessageClose{};
std::atomic_bool g_hooksReady{};
PredicateFn g_modalState{};
std::mutex g_mutex;
PendingNotification g_pending;

std::wstring Localize(std::wstring_view message) {
    const int language = lr_message_window::Language();
    const wchar_t* heading = kHeadings[1].text;
    for (const LocalizedHeading& candidate : kHeadings) {
        if (candidate.language == language) {
            heading = candidate.text;
            break;
        }
    }

    const std::size_t details = message.find(L'\n');
    std::wstring localized(heading);
    if (details != std::wstring_view::npos) {
        localized.append(message.substr(details));
    }
    return localized;
}

bool ConfirmInputReleased() {
    if ((GetAsyncKeyState(VK_RETURN) & 0x8000) != 0) return false;
    std::uint16_t buttons{};
    return !input::xinput::GetButtons(0u, buttons) ||
           (buttons & XINPUT_GAMEPAD_A) == 0;
}

int __fastcall CompanyLogoHook(void* companyLogo, void*) {
    const int running = g_originalCompanyLogo
        ? g_originalCompanyLogo(companyLogo) : 1;
    if (!g_hooksReady.load(std::memory_order_acquire)) return running;
    if (running) return running;

    std::wstring messageToOpen;
    std::wstring signature;
    bool shouldCheckModal{};
    bool logOpenDetails{};
    const bool focused = input::GameInputHasFocus();
    {
        std::scoped_lock lock(g_mutex);
        if (g_pending.message.empty()) return running;
        if (!focused) return 1;

        if (!g_pending.opening && !g_pending.opened) {
            shouldCheckModal = true;
        }

        if (!shouldCheckModal) {
            if (!g_pending.opened || !g_pending.closed ||
                !ConfirmInputReleased()) {
                return 1;
            }
            signature = std::move(g_pending.signature);
            g_pending = {};
        }
    }

    if (shouldCheckModal) {
        if (g_modalState && g_modalState() != 0) return 1;
        std::wstring pendingMessage;
        {
            std::scoped_lock lock(g_mutex);
            if (g_pending.message.empty()) return running;
            if (g_pending.opening || g_pending.opened) return 1;
            g_pending.opening = true;
            logOpenDetails = !g_pending.openFailureLogged;
            pendingMessage = g_pending.message;
        }
        messageToOpen = Localize(pendingMessage);
    }

    if (!messageToOpen.empty()) {
        std::vector<char> native;
        void* product = lr_message_window::ConvertText(
                            messageToOpen, native)
            ? lr_message_window::Open(
                  native.data(), "loaded-module report", logOpenDetails)
            : nullptr;
        bool logOpenFailure{};
        {
            std::scoped_lock lock(g_mutex);
            g_pending.opening = false;
            if (product) {
                g_pending.product = product;
                g_pending.opened = true;
            } else if (!g_pending.openFailureLogged) {
                g_pending.openFailureLogged = true;
                logOpenFailure = true;
            }
        }
        if (logOpenFailure) {
            Log("LR startup notification open failed; intro transition pending retry");
        }
        return 1;
    }

    if (!signature.empty() &&
        !settings::SaveLoadedModulesNotificationSignature(signature)) {
        Log("LR loaded-module signature could not be persisted");
    }
    Log("LR loaded-module report acknowledged: releasing the intro movie transition");
    return running;
}

int __fastcall MessageCloseHook(void* product, void*, int reason) {
    {
        std::scoped_lock lock(g_mutex);
        if (g_pending.opened && g_pending.product == product) {
            g_pending.closed = true;
            g_pending.product = nullptr;
        }
    }
    compat::native_notifications::NotifyClosed(product);
    return g_originalMessageClose
        ? g_originalMessageClose(product, reason) : 0;
}

BOOL CALLBACK Initialize(PINIT_ONCE, PVOID, PVOID*) {
    if (CurrentProfile().title != Title::LightningReturns ||
        !Supports(Capability::SnapshotPhotoMode)) {
        return TRUE;
    }

    const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
    if (!image) {
        LogWarning("LR post-logo module notification disabled: invalid executable image");
        return TRUE;
    }

    const auto companyLogo = image->At(
        kCompanyLogoIsRunningRva, kCompanyLogoIsRunningPrefix.size());
    const auto messageClose = image->At(
        kGuiMessageCloseRva, kGuiMessageClosePrefix.size());
    const auto modalState = image->At(kModalStateRva, 1u);
    if (!companyLogo || !messageClose || !modalState ||
        !lr_message_window::Bind(*image)) {
        LogWarning("LR post-logo module notification disabled: native contracts unavailable");
        return TRUE;
    }

    g_modalState = reinterpret_cast<PredicateFn>(
        const_cast<std::byte*>(modalState->data()));
    if (!g_companyLogoDetour.Install(
            "lr-post-logo-loaded-modules",
            const_cast<std::byte*>(companyLogo->data()),
            reinterpret_cast<void*>(&CompanyLogoHook),
            std::as_bytes(std::span{kCompanyLogoIsRunningPrefix}),
            reinterpret_cast<void**>(&g_originalCompanyLogo))) {
        g_modalState = nullptr;
        LogWarning("LR module notification disabled: company-logo completion detour failed");
        return TRUE;
    }
    if (!g_messageCloseDetour.Install(
            "lr-post-logo-loaded-modules-close",
            const_cast<std::byte*>(messageClose->data()),
            reinterpret_cast<void*>(&MessageCloseHook),
            std::as_bytes(std::span{kGuiMessageClosePrefix}),
            reinterpret_cast<void**>(&g_originalMessageClose))) {
        g_companyLogoDetour.RevertAndClear(g_originalCompanyLogo);
        g_modalState = nullptr;
        LogWarning("LR module notification disabled: message-close detour failed");
        return TRUE;
    }

    g_hooksReady.store(true, std::memory_order_release);
    return TRUE;
}

} // namespace

void Install() {
    InitOnceExecuteOnce(&g_installOnce, &Initialize, nullptr, nullptr);
}

void Queue(std::wstring message, std::wstring signature) {
    if (message.empty()) return;
    {
        std::scoped_lock lock(g_mutex);
        if (!g_pending.message.empty()) return;
        g_pending.message = std::move(message);
        g_pending.signature = std::move(signature);
    }
    Install();
}

bool HasPending() {
    std::scoped_lock lock(g_mutex);
    return !g_pending.message.empty() || g_pending.opening ||
           g_pending.opened;
}

} // namespace novafix::game::lr_addon_notification
