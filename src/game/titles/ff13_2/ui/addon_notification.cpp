#include "game/titles/ff13_2/ui/addon_notification.h"

#include "diagnostics/log.h"
#include "game/core/game_profile.h"
#include "game/core/image_view.h"
#include "game/shared/ui/startup_module_notification.h"
#include "patch/detour32.h"
#include "patch/memory_access.h"

#include <windows.h>
#include <intrin.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace novafix::game::xiii2_addon_notification {
namespace {

// Restore the startup barrier before XIII-2 Steam's battle/AOP construction
constexpr std::uint32_t kProductUpdateCompleteRva = 0x00492840u;
constexpr std::uint32_t kOpenRawMessageRva = 0x003A5580u;
constexpr std::uint32_t kActionActiveRva = 0x00377A50u;
constexpr std::uint32_t kBattleLoadRva = 0x0001DB80u;
constexpr std::uint32_t kStartupBattleLoadReturnRva = 0x00007993u;
constexpr std::uint32_t kBattleLoadSecurityCookieRva = 0x01FA90DCu;

constexpr std::array<std::uint8_t, 16> kProductUpdateCompletePrefix{
    0x56, 0x8B, 0xF1, 0x8B, 0x4E, 0x34, 0xE8, 0x05,
    0x52, 0xEE, 0xFF, 0x85, 0xC0, 0x75, 0x1E, 0xF6,
};
constexpr std::array<std::byte, 8> kBattleLoadPrefix{
    std::byte{0x83}, std::byte{0xEC}, std::byte{0x2C}, std::byte{0xA1},
    std::byte{0xDC}, std::byte{0x90}, std::byte{0x3A}, std::byte{0x02},
};

using ProductUpdateCompleteFn = void (__thiscall*)(void*, int);
using BattleLoadFn = void (__thiscall*)(void*, int);

patch::Detour32 g_completionDetour;
patch::Detour32 g_startupBarrierDetour;
ProductUpdateCompleteFn g_originalProductUpdateComplete{};
BattleLoadFn g_originalBattleLoad{};
std::atomic_bool g_startupBarrierActive{};
std::atomic_bool g_dialogOpen{};
std::atomic_bool g_hookSetReady{};
std::byte* g_imageBase{};

template <typename Function>
Function At(std::uint32_t rva) {
    return reinterpret_cast<Function>(g_imageBase + rva);
}

void* OpenMessage(const char* message) {
    using CreateFn = void* (__cdecl*)(char*);
    using ShowFn = int (__thiscall*)(void*, int, int);

    if (!message || !*message) return nullptr;
    void* product = At<CreateFn>(kOpenRawMessageRva)(
        const_cast<char*>(message));
    if (!product || !patch::memory::IsReadable(product, sizeof(void*))) {
        return nullptr;
    }
    void** vtable = *static_cast<void***>(product);
    if (!vtable ||
        !patch::memory::IsReadable(vtable, 96u * sizeof(void*)) ||
        !vtable[95]) {
        return nullptr;
    }
    // The factory constructs and registers the modal GuiMessage; slot 95
    // performs its normal first show/update transition
    reinterpret_cast<ShowFn>(vtable[95])(product, 0, 1);
    return product;
}

void MarkDialogOpen() {
    g_startupBarrierActive.store(true, std::memory_order_release);
    g_dialogOpen.store(true, std::memory_order_release);
}

void __fastcall BattleLoadHook(void* battleLoader, void*, int mode) {
    const auto* const returnAddress =
        static_cast<const std::byte*>(_ReturnAddress());
    const bool startupBoundary = returnAddress ==
        g_imageBase + kStartupBattleLoadReturnRva;
    if (!g_hookSetReady.load(std::memory_order_acquire) ||
        !startupBoundary ||
        !startup_module_notification::Pending()) {
        if (g_originalBattleLoad) g_originalBattleLoad(battleLoader, mode);
        return;
    }

    std::wstring message;
    const bool opening =
        startup_module_notification::BeginOpening(message);
    std::vector<char> native;
    void* product = opening &&
            startup_module_notification::NativeMessageText(
                message, 292140u, native)
        ? OpenMessage(native.data()) : nullptr;
    if (opening) {
        startup_module_notification::FinishOpening(
            product, &MarkDialogOpen);
    }
    if (product) {
        Log("XIII-2 loaded-module report opened at the native Xbox userSelect startup boundary before battle/AOP construction");
        // Hold the startup worker until GuiMessage closes and Confirm is released
        // Cinema/AOP does not exist yet and cannot consume that closing edge
        while (g_dialogOpen.load(std::memory_order_acquire)) {
            Sleep(1);
        }
        while (startup_module_notification::ConfirmHeld()) {
            Sleep(1);
        }

        std::wstring signature =
            startup_module_notification::CompletePending();
        g_startupBarrierActive.store(false, std::memory_order_release);
        startup_module_notification::Persist(
            std::move(signature), "XIII-2");
        Log("XIII-2 loaded-module report closed and Confirm released: continuing the untouched pre-AOP startup path");
    } else {
        startup_module_notification::Clear();
        g_startupBarrierActive.store(false, std::memory_order_release);
        g_dialogOpen.store(false, std::memory_order_release);
        LogWarning("XIII-2 Xbox-equivalent startup message could not be opened: continuing without persisting the notification signature");
    }

    if (g_originalBattleLoad) g_originalBattleLoad(battleLoader, mode);
}

void __fastcall ProductUpdateCompleteHook(
    void* product, void*, int delta) {
    bool completesPendingMessage{};
    if (g_hookSetReady.load(std::memory_order_acquire) &&
        startup_module_notification::Pending() && product &&
        patch::memory::IsReadable(product, 56u) &&
        startup_module_notification::IsTrackedProduct(product)) {
        std::uintptr_t root{};
        std::memcpy(&root,
                    static_cast<const std::byte*>(product) + 52u,
                    sizeof(root));
        using ActionActiveFn = int (__thiscall*)(void*);
        completesPendingMessage = root != 0u &&
            At<ActionActiveFn>(kActionActiveRva)(
                reinterpret_cast<void*>(root)) == 0;
    }
    if (g_originalProductUpdateComplete) {
        g_originalProductUpdateComplete(product, delta);
    }
    if (!completesPendingMessage ||
        !startup_module_notification::MarkProductClosed(product) ||
        !g_startupBarrierActive.load(std::memory_order_acquire)) {
        return;
    }
    g_dialogOpen.store(false, std::memory_order_release);
    Log("XIII-2 loaded-module report reached the native closed state while the pre-AOP startup boundary remained blocked");
}

bool Install(const ImageView& image) {
    g_imageBase = const_cast<std::byte*>(image.Bytes().data());
    auto startupBattleLoadPrefix = kBattleLoadPrefix;
    const auto relocatedSecurityCookie = static_cast<std::uint32_t>(
        reinterpret_cast<std::uintptr_t>(image.Bytes().data()) +
        kBattleLoadSecurityCookieRva);
    std::memcpy(startupBattleLoadPrefix.data() + 4u,
                &relocatedSecurityCookie,
                sizeof(relocatedSecurityCookie));

    const auto startupBattleLoad = image.At(
        kBattleLoadRva, startupBattleLoadPrefix.size());
    const auto completion = image.At(
        kProductUpdateCompleteRva, kProductUpdateCompletePrefix.size());
    if (!startupBattleLoad || !completion) return false;

    // Wait for CEG to expose GameMain; install both hooks together on retry
    if (std::memcmp(startupBattleLoad->data(),
                    startupBattleLoadPrefix.data(),
                    startupBattleLoadPrefix.size()) != 0) {
        return false;
    }

    if (!g_completionDetour.Install(
            "ff13-2-loaded-modules-notification-close",
            const_cast<std::byte*>(completion->data()),
            reinterpret_cast<void*>(&ProductUpdateCompleteHook),
            std::as_bytes(std::span{kProductUpdateCompletePrefix}),
            reinterpret_cast<void**>(&g_originalProductUpdateComplete))) {
        return false;
    }
    if (!g_startupBarrierDetour.Install(
            "ff13-2-loaded-modules-notification-open",
            const_cast<std::byte*>(startupBattleLoad->data()),
            reinterpret_cast<void*>(&BattleLoadHook),
            startupBattleLoadPrefix,
            reinterpret_cast<void**>(&g_originalBattleLoad))) {
        g_completionDetour.RevertAndClear(
            g_originalProductUpdateComplete);
        return false;
    }

    g_hookSetReady.store(true, std::memory_order_release);
    return true;
}

const startup_module_notification::Backend kBackend{
    &Install,
    "XIII-2 startup add-on notice is waiting for GameMain",
};

} // namespace

void Queue(std::wstring message, std::wstring signature) {
    const GameProfile& profile = CurrentProfile();
    if (profile.title != Title::FinalFantasyXIII2 ||
        profile.support != BuildSupport::ExactBuild) {
        return;
    }
    startup_module_notification::Queue(
        std::move(message), std::move(signature), kBackend);
}

} // namespace novafix::game::xiii2_addon_notification
