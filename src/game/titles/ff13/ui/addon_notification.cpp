#include "game/titles/ff13/ui/addon_notification.h"

#include "compat/addons/native_notifications.h"
#include "diagnostics/log.h"
#include "game/core/game_profile.h"
#include "game/core/image_view.h"
#include "game/shared/ui/startup_module_notification.h"
#include "patch/detour32.h"
#include "patch/memory_access.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace novafix::game::ff13_addon_notification {
namespace {

// Original, LAA and Nova/community XIII builds share this runtime layout
// Fingerprint the executable and validate both live detour prefixes
constexpr std::uint32_t kStartupLogoCompleteRva = 0x007DDFB0u;
constexpr std::uint32_t kProductUpdateCompleteRva = 0x0060B7F0u;
constexpr std::uint32_t kAllocatorRva = 0x004F8F00u;
constexpr std::uint32_t kMessageConstructorRva = 0x005F4110u;
constexpr std::uint32_t kMessageInitializeRva = 0x005F4360u;
constexpr std::uint32_t kMessageCloseRva = 0x005F74C0u;
constexpr std::uint32_t kMessageAlignRva = 0x005F7DA0u;
constexpr std::uint32_t kChoiceFactoryRva = 0x004EF370u;
constexpr std::uint32_t kProductLookupRva = 0x004EE9C0u;
constexpr std::uint32_t kSelectChoiceRva = 0x005F79A0u;
constexpr std::uint32_t kSetLayerRva = 0x0060B4A0u;
constexpr std::uint32_t kGuiManagerSlotRva = 0x02426A4Cu;
constexpr unsigned kSteamAppId = 292120u;

constexpr std::array<std::uint8_t, 16> kStartupLogoCompletePrefix{
    0x55, 0x8B, 0xEC, 0x8B, 0x0D, 0x44, 0x12, 0x73,
    0x02, 0xE8, 0x02, 0xC4, 0x85, 0xFF, 0x5D, 0xC3};
constexpr std::array<std::uint8_t, 16> kProductUpdateCompletePrefix{
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0x89, 0x4D,
    0xF8, 0x8B, 0x45, 0xF8, 0x8B, 0x48, 0x34, 0xE8};
constexpr std::array<std::uint8_t, 16> kMessageClosePrefix{
    0x55, 0x8B, 0xEC, 0x51, 0x89, 0x4D, 0xFC, 0x6A,
    0x02, 0x8B, 0x45, 0xFC, 0x8B, 0x48, 0x34, 0xE8};

using StartupLogoCompleteFn = int (__cdecl*)();
using ProductUpdateCompleteFn = unsigned int (__thiscall*)(void*, int);
using GuiMessageCloseFn = int (__thiscall*)(void*, int);

patch::Detour32 g_boundaryDetour;
patch::Detour32 g_completionDetour;
patch::Detour32 g_messageCloseDetour;
StartupLogoCompleteFn g_originalStartupLogoComplete{};
ProductUpdateCompleteFn g_originalProductUpdateComplete{};
GuiMessageCloseFn g_originalMessageClose{};
std::atomic_bool g_dialogOpen{};
std::atomic_bool g_hookSetReady{};
std::byte* g_imageBase{};

template <typename Function>
Function At(std::uint32_t rva) {
    return reinterpret_cast<Function>(g_imageBase + rva);
}

bool CanOpenPluginNativeMessage() {
    if (!g_hookSetReady.load(std::memory_order_acquire) || !g_imageBase ||
        startup_module_notification::Pending()) {
        return false;
    }
    auto* const managerSlot = At<std::uintptr_t*>(kGuiManagerSlotRva);
    return patch::memory::IsReadable(managerSlot, sizeof(*managerSlot)) &&
           *managerSlot != 0u &&
           patch::memory::IsReadable(
               reinterpret_cast<const void*>(*managerSlot + 2156u),
               sizeof(std::uintptr_t)) &&
           *reinterpret_cast<const std::uintptr_t*>(
               *managerSlot + 2156u) != 0u;
}

void* OpenMessage(const char* message, std::uint32_t mode,
                  bool exclusiveInput, int horizontalAlignment = 1,
                  int verticalAlignment = 1,
                  bool showAcknowledgeButton = false) {
    using AllocateFn = void* (__cdecl*)(std::size_t);
    using ConstructFn = void* (__thiscall*)(void*);
    using InitializeFn = void (__thiscall*)(void*, int, int, int, int,
                                            const char*, int, int, int,
                                            int, int);
    using AlignFn = int (__thiscall*)(void*, int, int);
    using SetLayerFn = int (__thiscall*)(void*, int);
    using OpenFn = int (__thiscall*)(void*, int, int);
    using ChoiceFactoryFn = int (__thiscall*)(
        void*, int, int, int, int, const char*, int, float, int, int, int,
        int);
    using ProductLookupFn = void* (__thiscall*)(void*, int, int);
    using SelectChoiceFn = int (__thiscall*)(void*, int);

    auto* managerSlot = At<std::uintptr_t*>(kGuiManagerSlotRva);
    if (!message || !*message ||
        !patch::memory::IsReadable(managerSlot, sizeof(*managerSlot)) ||
        !*managerSlot ||
        !patch::memory::IsReadable(
            reinterpret_cast<const void*>(*managerSlot + 2156u),
            sizeof(std::uintptr_t))) {
        return nullptr;
    }
    void* product{};
    if (showAcknowledgeButton) {
        const int productId = At<ChoiceFactoryFn>(kChoiceFactoryRva)(
            reinterpret_cast<void*>(*managerSlot), 0, 400, 0, 0,
            message, 1, 0.0f, 0, 0, 1, 1);
        void* const wrapper = At<ProductLookupFn>(kProductLookupRva)(
            reinterpret_cast<void*>(*managerSlot), productId, 0);
        if (!wrapper || !patch::memory::IsReadable(wrapper, sizeof(void*))) {
            return nullptr;
        }
        void** const wrapperVtable = *static_cast<void***>(wrapper);
        if (!wrapperVtable || !wrapperVtable[1]) return nullptr;
        using GetProductFn = void* (__thiscall*)(void*);
        product = reinterpret_cast<GetProductFn>(wrapperVtable[1])(wrapper);
    } else {
        void* memory = At<AllocateFn>(kAllocatorRva)(816u);
        if (!memory) return nullptr;
        product = At<ConstructFn>(kMessageConstructorRva)(memory);
        if (!product) return nullptr;
        *reinterpret_cast<std::uintptr_t*>(
            static_cast<std::byte*>(product) + 76u) =
            *reinterpret_cast<std::uintptr_t*>(*managerSlot + 2156u);
        At<InitializeFn>(kMessageInitializeRva)(
            product, 0, 0, 64, 1, message, 0, 0, 2, 0, 0);
    }
    if (!product) return nullptr;
    const bool timed =
        mode == NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_TIMED;
    At<AlignFn>(kMessageAlignRva)(
        product, horizontalAlignment, verticalAlignment);
    // Layer 10 is used by transient notices; layer 6 is the native modal
    // message layer. Choice windows use the game's interactive layer 2.
    At<SetLayerFn>(kSetLayerRva)(
        product, showAcknowledgeButton ? 2
            : timed && !exclusiveInput ? 10 : 6);
    auto* flags = reinterpret_cast<std::uint32_t*>(
        static_cast<std::byte*>(product) + 56u);
    *flags |= 0x88u;
    void** vtable = *static_cast<void***>(product);
    if (!vtable ||
        !patch::memory::IsReadable(vtable, 67u * sizeof(void*))) {
        return nullptr;
    }
    reinterpret_cast<OpenFn>(vtable[66])(product, 0, 1);
    if (showAcknowledgeButton) {
        At<SelectChoiceFn>(kSelectChoiceRva)(product, 1);
    }
    return product;
}

void* OpenPluginNativeMessage(
    std::wstring_view message,
    const compat::native_notifications::OpenRequest& request,
    bool reportFailure) {
    std::vector<char> native;
    if (!startup_module_notification::NativeText(
            message, kSteamAppId, native)) {
        if (reportFailure) {
            LogWarning("XIII add-on native notification conversion failed");
        }
        return nullptr;
    }
    const bool acknowledge =
        request.mode == NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_ACKNOWLEDGE;
    if (acknowledge) {
        native.pop_back();
        native.insert(native.end(), {'|', 'O', 'K', '\0'});
    }
    void* const product = OpenMessage(
        native.data(), request.mode, request.exclusiveInput,
        request.horizontalAlignment, request.verticalAlignment,
        acknowledge);
    if (!product && reportFailure) {
        LogWarning("XIII add-on native notification could not be opened");
    }
    return product;
}

bool ClosePluginNativeMessage(void* product) {
    if (!product || !patch::memory::IsReadable(product, sizeof(void*))) {
        return false;
    }
    void** const vtable = *static_cast<void***>(product);
    if (!vtable ||
        !patch::memory::IsReadable(vtable, 68u * sizeof(void*)) ||
        !vtable[67]) {
        return false;
    }
    using CloseFn = int (__thiscall*)(void*, int);
    reinterpret_cast<CloseFn>(vtable[67])(product, 0);
    return true;
}

bool FocusPluginNativeMessage(void* product) {
    if (!product || !patch::memory::IsReadable(product, 136u)) {
        return false;
    }
    const auto* const selection = reinterpret_cast<const std::int32_t*>(
        static_cast<const std::byte*>(product) + 132u);
    if (*selection >= 0) return true;
    using SelectChoiceFn = int (__thiscall*)(void*, int);
    At<SelectChoiceFn>(kSelectChoiceRva)(product, 1);
    return *selection >= 0;
}

int __fastcall MessageCloseHook(void* product, void*, int reason) {
    if (compat::native_notifications::ShouldSuppressClose(product)) {
        return 0;
    }
    return g_originalMessageClose
        ? g_originalMessageClose(product, reason) : 0;
}

void MarkDialogOpen() {
    g_dialogOpen.store(true, std::memory_order_release);
}

unsigned int __fastcall ProductUpdateCompleteHook(
    void* product, void*, int delta) {
    const unsigned int result = g_originalProductUpdateComplete
        ? g_originalProductUpdateComplete(product, delta) : 0u;
    const auto* bytes = static_cast<const std::byte*>(product);
    const bool closed = product &&
        patch::memory::IsReadable(bytes + 58u, 1u) &&
        static_cast<unsigned char>(bytes[58]) == 3u;
    if (closed) {
        compat::native_notifications::NotifyClosed(product);
    }
    if (!g_hookSetReady.load(std::memory_order_acquire) ||
        !startup_module_notification::Pending()) {
        return result;
    }

    if (closed) {
        std::wstring signature;
        if (startup_module_notification::CompleteProduct(
                product, signature)) {
            g_dialogOpen.store(false, std::memory_order_release);
            startup_module_notification::Persist(
                std::move(signature), "XIII");
        }
    }
    return result;
}

int __cdecl StartupLogoCompleteHook() {
    const int result = g_originalStartupLogoComplete
        ? g_originalStartupLogoComplete() : 0;
    if (!g_hookSetReady.load(std::memory_order_acquire)) return result;

    std::wstring message;
    if (!startup_module_notification::BeginOpening(message)) return result;

    std::vector<char> native;
    void* product = startup_module_notification::NativeMessageText(
                        message, 292120u, native)
        ? OpenMessage(
              native.data(),
              NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_ACKNOWLEDGE, true)
        : nullptr;
    startup_module_notification::FinishOpening(product, &MarkDialogOpen);
    if (product) {
        Log("XIII loaded-module report opened at the native sqexlogo completion command");
        // Hold the startup VM command while Present services GuiMessage
        // Never pump frames here: re-entry corrupts the startup scheduler
        while (g_dialogOpen.load(std::memory_order_acquire)) {
            Sleep(1);
        }

        // The same confirm edge that closes GuiMessage must not immediately
        // skip the pre-render which starts when this VM command returns
        while (startup_module_notification::ConfirmHeld()) {
            Sleep(1);
        }
        Log("XIII loaded-module report closed: releasing the startup VM command");
    } else {
        startup_module_notification::Clear();
        g_dialogOpen.store(false, std::memory_order_release);
        LogWarning("XIII post-logo loaded-module window could not be opened");
    }
    return result;
}

bool Install(const ImageView& image) {
    if (g_hookSetReady.load(std::memory_order_acquire)) return true;
    g_imageBase = const_cast<std::byte*>(image.Bytes().data());
    const auto boundary = image.At(
        kStartupLogoCompleteRva, kStartupLogoCompletePrefix.size());
    const auto completion = image.At(
        kProductUpdateCompleteRva, kProductUpdateCompletePrefix.size());
    const auto messageClose = image.At(
        kMessageCloseRva, kMessageClosePrefix.size());
    if (!boundary || !completion || !messageClose) return false;
    if (!g_boundaryDetour.Install(
            "ff13-loaded-modules-notification-open",
            const_cast<std::byte*>(boundary->data()),
            reinterpret_cast<void*>(&StartupLogoCompleteHook),
            std::as_bytes(std::span{kStartupLogoCompletePrefix}),
            reinterpret_cast<void**>(&g_originalStartupLogoComplete))) {
        return false;
    }
    if (!g_completionDetour.Install(
            "ff13-loaded-modules-notification-close",
            const_cast<std::byte*>(completion->data()),
            reinterpret_cast<void*>(&ProductUpdateCompleteHook),
            std::as_bytes(std::span{kProductUpdateCompletePrefix}),
            reinterpret_cast<void**>(&g_originalProductUpdateComplete))) {
        g_boundaryDetour.RevertAndClear(g_originalStartupLogoComplete);
        return false;
    }
    if (!g_messageCloseDetour.Install(
            "ff13-native-notification-close-guard",
            const_cast<std::byte*>(messageClose->data()),
            reinterpret_cast<void*>(&MessageCloseHook),
            std::as_bytes(std::span{kMessageClosePrefix}),
            reinterpret_cast<void**>(&g_originalMessageClose))) {
        g_completionDetour.RevertAndClear(
            g_originalProductUpdateComplete);
        g_boundaryDetour.RevertAndClear(g_originalStartupLogoComplete);
        return false;
    }
    g_hookSetReady.store(true, std::memory_order_release);
    compat::native_notifications::RegisterBackend({
        &CanOpenPluginNativeMessage, &OpenPluginNativeMessage,
        &ClosePluginNativeMessage, &FocusPluginNativeMessage});
    return true;
}

const startup_module_notification::Backend kBackend{
    &Install,
    "XIII startup add-on notice is waiting for the CEG runtime",
};

} // namespace

void Initialize() {
    const GameProfile& profile = CurrentProfile();
    if (profile.title != Title::FinalFantasyXIII ||
        profile.support != BuildSupport::ExactBuild) {
        return;
    }
    startup_module_notification::EnsureInstalled(kBackend);
}

void Queue(std::wstring message, std::wstring signature) {
    const GameProfile& profile = CurrentProfile();
    if (profile.title != Title::FinalFantasyXIII ||
        profile.support != BuildSupport::ExactBuild) {
        return;
    }
    startup_module_notification::Queue(
        std::move(message), std::move(signature), kBackend);
}

} // namespace novafix::game::ff13_addon_notification
