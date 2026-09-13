#include "game/titles/ff13_2/rendering/shadows/native_hooks.h"

#include "diagnostics/hook_profiler.h"
#include "diagnostics/log.h"
#include "game/core/game_profile.h"
#include "game/core/image_view.h"
#include "game/titles/ff13_2/rendering/shadows/alias_transaction.h"
#include "game/titles/ff13_2/rendering/shadows/atlas_policy.h"
#include "game/titles/ff13_2/rendering/shadows/frame_runtime.h"
#include "game/titles/ff13_2/rendering/shadows/resource_snapshot.h"
#include "patch/detour32.h"

#include <windows.h>
#include <d3d9.h>
#include <intrin.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace novafix::game::xiii2_shadow_native_hooks {
namespace {

constexpr std::uint32_t kScreenFactoryRva = 0x0034BE60u;
// CALL 0x0074BE60 is at VA 0x00BE9B04 and is five bytes long
// _ReturnAddress therefore observes VA 0x00BE9B09 (RVA 0x007E9B09)
constexpr std::uint32_t kAtlasFactoryReturnRva = 0x007E9B09u;
constexpr std::uint32_t kViewportRva = 0x006418E0u;
constexpr std::uint32_t kClearRva = 0x00821330u;
constexpr std::uint32_t kMainShadowPassRva = 0x008447F0u;
constexpr std::uint32_t kDeviceSlotRva = 0x04D6BA88u;
constexpr std::uint32_t kCurrentColorOwnerSlotRva = 0x04D6B400u;
constexpr std::uint32_t kCurrentDepthOwnerSlotRva = 0x04D6B410u;
constexpr std::uint32_t kRendererFactoryGlobalRva = 0x01F4E424u;
constexpr std::uint32_t kSceneDrawingByteRva = 0x04D6B526u;

constexpr std::array<std::byte, 8> kViewportPrefix{
    std::byte{0xD9}, std::byte{0x44}, std::byte{0x24}, std::byte{0x18},
    std::byte{0x8B}, std::byte{0x54}, std::byte{0x24}, std::byte{0x0C},
};
constexpr std::array<std::byte, 12> kMainShadowPassPrefix{
    std::byte{0x55}, std::byte{0x8B}, std::byte{0xEC},
    std::byte{0x83}, std::byte{0xE4}, std::byte{0xF0},
    std::byte{0x81}, std::byte{0xEC}, std::byte{0xD4}, std::byte{0x05},
    std::byte{0x00}, std::byte{0x00},
};

constexpr std::uint32_t kInternalR32F = 9u;
constexpr std::uint32_t kInternalD24S8 = 15u;

using ScreenFactory = void** (__cdecl*)(
    void** output, int width, int height, int colorFormat, int depthFormat,
    int colorCount, int unknown, int flags, int trailing);
using SetViewport = int (__cdecl*)(
    int x, int y, int width, int height, float minZ, float maxZ);
using Clear = int (__cdecl*)(
    int flags, std::uint32_t color, float depth, int stencil);
using MainShadowPass = void* (__thiscall*)(void* renderer, void* view);
using xiii2_shadow_resources::Snapshot;

INIT_ONCE g_initializeOnce = INIT_ONCE_STATIC_INIT;
patch::Detour32 g_factoryDetour;
patch::Detour32 g_viewportDetour;
patch::Detour32 g_clearDetour;
patch::Detour32 g_shadowPassDetour;
ScreenFactory g_originalFactory{};
SetViewport g_originalSetViewport{};
Clear g_originalClear{};
MainShadowPass g_originalShadowPass{};
std::byte* g_imageBase{};

std::atomic_bool g_installed{false};
std::atomic_bool g_factoryObserved{false};
std::atomic_bool g_clearFailureLogged{false};

using xiii2_shadow_alias::AliasTransaction;
using xiii2_shadow_alias::RestoreCascadeStorage;
using xiii2_shadow_frame_runtime::ActiveCascade;
using xiii2_shadow_frame_runtime::CurrentSnapshot;
using xiii2_shadow_frame_runtime::FailClosed;

template <std::size_t Size>
bool WriteAddress(std::array<std::byte, Size>& bytes, std::size_t offset,
                  std::uint32_t address) {
    if (offset > bytes.size() || sizeof(address) > bytes.size() - offset) {
        return false;
    }
    std::memcpy(bytes.data() + offset, &address, sizeof(address));
    return true;
}

#if defined(_M_IX86) || defined(__i386__)
void** __cdecl ScreenFactoryHook(
    void** output, int width, int height, int colorFormat, int depthFormat,
    int colorCount, int unknown, int flags, int trailing) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.ff13-2.shadow-screen-factory-total");
    ScreenFactory original = g_originalFactory;
    if (!original) return output;

    const auto returnAddress =
        reinterpret_cast<std::byte*>(_ReturnAddress());
    const bool atlasCall =
        xiii2_shadow_frame_runtime::HooksReady() && g_imageBase &&
        returnAddress == g_imageBase + kAtlasFactoryReturnRva &&
        width > 0 && height == width * 2 &&
        colorFormat == static_cast<int>(kInternalR32F) &&
        depthFormat == 0 && colorCount == 1 && flags == 16;
    void** result = original(
        output, width, height, colorFormat,
        atlasCall ? static_cast<int>(kInternalD24S8) : depthFormat,
        colorCount, unknown, flags, trailing);
    if (atlasCall && (!result || !output || !*output)) {
        FailClosed("D24S8 atlas attachment was rejected");
        return original(
            output, width, height, colorFormat, depthFormat,
            colorCount, unknown, flags, trailing);
    }
    if (atlasCall && output && *output) {
        (void)g_factoryObserved.exchange(true, std::memory_order_acq_rel);
    }
    return result;
}

int __cdecl SetViewportHook(
    int x, int y, int width, int height, float minZ, float maxZ) {
    NOVAFIX_PROFILE_SAMPLED_NAMED_SCOPE(
        hookTiming, "hook.ff13-2.shadow-viewport-total", 16u);
    const auto cascade = ActiveCascade();
    const Snapshot* snapshot = CurrentSnapshot();
    if (cascade &&
        width == static_cast<int>(snapshot->layout.cascadeWidth) &&
        height == static_cast<int>(snapshot->layout.cascadeHeight)) {
        const auto region = xiii2_shadow_atlas_policy::CascadeRenderRegion(
            snapshot->layout, *cascade);
        if (region && *cascade == 1u &&
            xiii2_shadow_atlas_policy::UsesReducedFarCascade(
                snapshot->layout)) {
            x = static_cast<int>(region->left);
            y = static_cast<int>(region->top);
            width = static_cast<int>(region->right - region->left);
            height = static_cast<int>(region->bottom - region->top);
        } else if (region && y >= 0 &&
                   static_cast<std::uint32_t>(y) <=
                       snapshot->layout.cascadeHeight) {
            y += static_cast<int>(region->top);
        }
    }
    return g_originalSetViewport
        ? g_originalSetViewport(x, y, width, height, minZ, maxZ) : 0;
}

int __cdecl ClearHook(
    int flags, std::uint32_t color, float depth, int stencil) {
    NOVAFIX_PROFILE_SAMPLED_NAMED_SCOPE(
        hookTiming, "hook.ff13-2.shadow-clear-total", 16u);
    const auto cascade = ActiveCascade();
    const Snapshot* snapshot = CurrentSnapshot();
    if (!cascade || !snapshot || !snapshot->device) {
        return g_originalClear
            ? g_originalClear(flags, color, depth, stencil)
            : D3DERR_INVALIDCALL;
    }

    const auto region = xiii2_shadow_atlas_policy::CascadeRenderRegion(
        snapshot->layout, *cascade);
    if (!region) {
        return g_originalClear
            ? g_originalClear(flags, color, depth, stencil)
            : D3DERR_INVALIDCALL;
    }

    DWORD clearFlags = 0u;
    if ((flags & 1) != 0) clearFlags |= D3DCLEAR_TARGET;
    if ((flags & 2) != 0) clearFlags |= D3DCLEAR_ZBUFFER;
    if ((flags & 4) != 0) clearFlags |= D3DCLEAR_STENCIL;
    const D3DRECT rectangle{
        static_cast<LONG>(region->left),
        static_cast<LONG>(region->top),
        static_cast<LONG>(region->right),
        static_cast<LONG>(region->bottom),
    };
    const HRESULT result = snapshot->device->Clear(
        1u, &rectangle, clearFlags,
        xiii2_shadow_atlas_policy::ConvertClearColor(color),
        depth, static_cast<DWORD>(stencil));
    if (FAILED(result)) {
        FailClosed("per-half D3D9 clear failed");
        if (!g_clearFailureLogged.exchange(true, std::memory_order_acq_rel)) {
            Log("XIII-2 cached direct atlas partial clear result=0x%08lX",
                static_cast<unsigned long>(result));
        }
    }
    return result;
}

void* __fastcall MainShadowPassHook(void* renderer, void*, void* view) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.ff13-2.main-shadow-pass-total");
    MainShadowPass original = g_originalShadowPass;
    if (!original) return nullptr;

    Snapshot snapshot{};
    AliasTransaction transaction{};
    const auto previous = xiii2_shadow_frame_runtime::BeginFrame(
        renderer, snapshot, transaction);
    void* result = original(renderer, view);
    xiii2_shadow_frame_runtime::ApplyReducedFarReceiverMatrix(
        renderer, snapshot);
    RestoreCascadeStorage(transaction);
    xiii2_shadow_frame_runtime::RestoreFrame(previous);
    return result;
}
#else
void** ScreenFactoryHook(
    void** output, int width, int height, int colorFormat, int depthFormat,
    int colorCount, int unknown, int flags, int trailing) {
    return g_originalFactory
        ? g_originalFactory(output, width, height, colorFormat, depthFormat,
                            colorCount, unknown, flags, trailing)
        : output;
}
int SetViewportHook(
    int x, int y, int width, int height, float minZ, float maxZ) {
    return g_originalSetViewport
        ? g_originalSetViewport(x, y, width, height, minZ, maxZ) : 0;
}
int ClearHook(int flags, std::uint32_t color, float depth, int stencil) {
    return g_originalClear
        ? g_originalClear(flags, color, depth, stencil) : D3DERR_INVALIDCALL;
}
void* MainShadowPassHook(void* renderer, void*, void* view) {
    return g_originalShadowPass ? g_originalShadowPass(renderer, view) : nullptr;
}
#endif

void RevertHooks() {
    xiii2_shadow_frame_runtime::SetHooksReady(false);
    g_factoryDetour.RevertAndClear(g_originalFactory);
    g_shadowPassDetour.RevertAndClear(g_originalShadowPass);
    g_clearDetour.RevertAndClear(g_originalClear);
    g_viewportDetour.RevertAndClear(g_originalSetViewport);
}

BOOL CALLBACK Initialize(PINIT_ONCE, PVOID, PVOID*) {
    if (CurrentProfile().title != Title::FinalFantasyXIII2 ||
        !Supports(Capability::FramePacing)) {
        return TRUE;
    }

    const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
    if (!image) {
        LogWarning("XIII-2 direct shadow atlas unavailable: image contract missing");
        return TRUE;
    }
    const auto factoryRange = image->At(kScreenFactoryRva, 8u);
    const auto viewportRange = image->At(
        kViewportRva, kViewportPrefix.size());
    const auto clearRange = image->At(kClearRva, 8u);
    const auto shadowPassRange = image->At(
        kMainShadowPassRva, kMainShadowPassPrefix.size());
    const auto deviceSlotRange = image->At(kDeviceSlotRva, sizeof(void*));
    const auto colorOwnerRange = image->At(
        kCurrentColorOwnerSlotRva, sizeof(void*));
    const auto depthOwnerRange = image->At(
        kCurrentDepthOwnerSlotRva, sizeof(void*));
    const auto atlasReturnRange = image->At(kAtlasFactoryReturnRva, 1u);
    const auto rendererFactoryRange = image->At(
        kRendererFactoryGlobalRva, sizeof(void*));
    const auto sceneDrawingRange = image->At(kSceneDrawingByteRva, 1u);
    const auto rendererFactoryAddress = image->Address32(
        kRendererFactoryGlobalRva, sizeof(void*));
    const auto sceneDrawingAddress = image->Address32(
        kSceneDrawingByteRva, 1u);
    if (!factoryRange || !viewportRange || !clearRange || !shadowPassRange ||
        !deviceSlotRange || !colorOwnerRange || !depthOwnerRange ||
        !atlasReturnRange || !rendererFactoryRange || !sceneDrawingRange ||
        !rendererFactoryAddress || !sceneDrawingAddress) {
        LogWarning("XIII-2 direct shadow atlas unavailable: RVA contract outside image or x86 address space");
        return TRUE;
    }

    g_imageBase = const_cast<std::byte*>(image->Bytes().data());
    xiii2_shadow_frame_runtime::BindResourceSlots(
        reinterpret_cast<void**>(
            const_cast<std::byte*>(deviceSlotRange->data())),
        reinterpret_cast<void**>(
            const_cast<std::byte*>(colorOwnerRange->data())),
        reinterpret_cast<void**>(
            const_cast<std::byte*>(depthOwnerRange->data())));

    std::array<std::byte, 8> factoryPrefix{
        std::byte{0x83}, std::byte{0xEC}, std::byte{0x0C}, std::byte{0xA1},
        std::byte{}, std::byte{}, std::byte{}, std::byte{},
    };
    if (!WriteAddress(factoryPrefix, 4u, *rendererFactoryAddress)) {
        return TRUE;
    }
    std::array<std::byte, 8> clearPrefix{
        std::byte{0x51}, std::byte{0x80}, std::byte{0x3D},
        std::byte{}, std::byte{}, std::byte{}, std::byte{}, std::byte{0x00},
    };
    if (!WriteAddress(clearPrefix, 3u, *sceneDrawingAddress)) {
        return TRUE;
    }

    auto* factory = const_cast<std::byte*>(factoryRange->data());
    auto* viewport = const_cast<std::byte*>(viewportRange->data());
    auto* clear = const_cast<std::byte*>(clearRange->data());
    auto* shadowPass = const_cast<std::byte*>(shadowPassRange->data());
    if (std::memcmp(factory, factoryPrefix.data(), factoryPrefix.size()) != 0 ||
        std::memcmp(viewport, kViewportPrefix.data(), kViewportPrefix.size()) != 0 ||
        std::memcmp(clear, clearPrefix.data(), clearPrefix.size()) != 0 ||
        std::memcmp(shadowPass, kMainShadowPassPrefix.data(),
                    kMainShadowPassPrefix.size()) != 0) {
        LogWarning("XIII-2 direct shadow atlas unavailable: executable contract mismatch");
        return TRUE;
    }

    const bool viewportOk = g_viewportDetour.Install(
        "xiii2-shadow-atlas-viewport", viewport,
        reinterpret_cast<void*>(&SetViewportHook), kViewportPrefix,
        reinterpret_cast<void**>(&g_originalSetViewport));
    const bool clearOk = viewportOk && g_clearDetour.Install(
        "xiii2-shadow-atlas-clear", clear,
        reinterpret_cast<void*>(&ClearHook), clearPrefix,
        reinterpret_cast<void**>(&g_originalClear));
    const bool shadowOk = clearOk && g_shadowPassDetour.Install(
        "xiii2-shadow-atlas-main-pass", shadowPass,
        reinterpret_cast<void*>(&MainShadowPassHook), kMainShadowPassPrefix,
        reinterpret_cast<void**>(&g_originalShadowPass));
    const bool factoryOk = shadowOk && g_factoryDetour.Install(
        "xiii2-shadow-atlas-factory", factory,
        reinterpret_cast<void*>(&ScreenFactoryHook), factoryPrefix,
        reinterpret_cast<void**>(&g_originalFactory));
    if (!factoryOk) {
        RevertHooks();
        LogWarning("XIII-2 direct shadow atlas unavailable: atomic hook ownership rejected; native pipeline retained");
        return TRUE;
    }

    xiii2_shadow_frame_runtime::SetHooksReady(true);
    g_installed.store(true, std::memory_order_release);
    return TRUE;
}

} // namespace

void Pump() {
    InitOnceExecuteOnce(&g_initializeOnce, &Initialize, nullptr, nullptr);
}

bool Installed() {
    return g_installed.load(std::memory_order_acquire);
}

bool FactoryObserved() {
    return g_factoryObserved.load(std::memory_order_acquire);
}

} // namespace novafix::game::xiii2_shadow_native_hooks
