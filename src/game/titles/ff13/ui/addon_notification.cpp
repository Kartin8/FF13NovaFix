#include "game/titles/ff13/ui/addon_notification.h"

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
constexpr std::uint32_t kMessageAlignRva = 0x005F7DA0u;
constexpr std::uint32_t kSetLayerRva = 0x0060B4A0u;
constexpr std::uint32_t kGuiManagerSlotRva = 0x02426A4Cu;

constexpr std::array<std::uint8_t, 16> kStartupLogoCompletePrefix{
    0x55, 0x8B, 0xEC, 0x8B, 0x0D, 0x44, 0x12, 0x73,
    0x02, 0xE8, 0x02, 0xC4, 0x85, 0xFF, 0x5D, 0xC3};
constexpr std::array<std::uint8_t, 16> kProductUpdateCompletePrefix{
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0x89, 0x4D,
    0xF8, 0x8B, 0x45, 0xF8, 0x8B, 0x48, 0x34, 0xE8};

using StartupLogoCompleteFn = int (__cdecl*)();
using ProductUpdateCompleteFn = unsigned int (__thiscall*)(void*, int);

patch::Detour32 g_boundaryDetour;
patch::Detour32 g_completionDetour;
StartupLogoCompleteFn g_originalStartupLogoComplete{};
ProductUpdateCompleteFn g_originalProductUpdateComplete{};
std::atomic_bool g_dialogOpen{};
std::atomic_bool g_hookSetReady{};
std::byte* g_imageBase{};

template <typename Function>
Function At(std::uint32_t rva) {
    return reinterpret_cast<Function>(g_imageBase + rva);
}

void* OpenMessage(const char* message) {
    using AllocateFn = void* (__cdecl*)(std::size_t);
    using ConstructFn = void* (__thiscall*)(void*);
    using InitializeFn = void (__thiscall*)(void*, int, int, int, int,
                                            const char*, int, int, int,
                                            int, int);
    using AlignFn = int (__thiscall*)(void*, int, int);
    using SetLayerFn = int (__thiscall*)(void*, int);
    using OpenFn = int (__thiscall*)(void*, int, int);

    auto* managerSlot = At<std::uintptr_t*>(kGuiManagerSlotRva);
    if (!message || !*message ||
        !patch::memory::IsReadable(managerSlot, sizeof(*managerSlot)) ||
        !*managerSlot ||
        !patch::memory::IsReadable(
            reinterpret_cast<const void*>(*managerSlot + 2156u),
            sizeof(std::uintptr_t))) {
        return nullptr;
    }
    void* memory = At<AllocateFn>(kAllocatorRva)(816u);
    if (!memory) return nullptr;
    void* product = At<ConstructFn>(kMessageConstructorRva)(memory);
    if (!product) return nullptr;
    *reinterpret_cast<std::uintptr_t*>(
        static_cast<std::byte*>(product) + 76u) =
        *reinterpret_cast<std::uintptr_t*>(*managerSlot + 2156u);
    At<InitializeFn>(kMessageInitializeRva)(
        product, 0, 0, 64, 1, message, 0, 0, 2, 0, 0);
    At<AlignFn>(kMessageAlignRva)(product, 1, 1);
    At<SetLayerFn>(kSetLayerRva)(product, 6);
    auto* flags = reinterpret_cast<std::uint32_t*>(
        static_cast<std::byte*>(product) + 56u);
    *flags |= 0x88u;
    void** vtable = *static_cast<void***>(product);
    if (!vtable ||
        !patch::memory::IsReadable(vtable, 67u * sizeof(void*))) {
        return nullptr;
    }
    reinterpret_cast<OpenFn>(vtable[66])(product, 0, 1);
    return product;
}

void MarkDialogOpen() {
    g_dialogOpen.store(true, std::memory_order_release);
}

unsigned int __fastcall ProductUpdateCompleteHook(
    void* product, void*, int delta) {
    const unsigned int result = g_originalProductUpdateComplete
        ? g_originalProductUpdateComplete(product, delta) : 0u;
    if (!g_hookSetReady.load(std::memory_order_acquire) ||
        !startup_module_notification::Pending()) {
        return result;
    }

    const auto* bytes = static_cast<const std::byte*>(product);
    if (patch::memory::IsReadable(bytes + 58u, 1u) &&
        static_cast<unsigned char>(bytes[58]) == 3u) {
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
        ? OpenMessage(native.data()) : nullptr;
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
    g_imageBase = const_cast<std::byte*>(image.Bytes().data());
    const auto boundary = image.At(
        kStartupLogoCompleteRva, kStartupLogoCompletePrefix.size());
    const auto completion = image.At(
        kProductUpdateCompleteRva, kProductUpdateCompletePrefix.size());
    if (!boundary || !completion) return false;
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
    g_hookSetReady.store(true, std::memory_order_release);
    return true;
}

const startup_module_notification::Backend kBackend{
    &Install,
    "XIII startup add-on notice is waiting for the CEG runtime",
};

} // namespace

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
