#include "game/titles/ff13_2/rendering/shadows/fixed_depth.h"

#include "diagnostics/hook_profiler.h"
#include "diagnostics/log.h"
#include "d3d9/shader_corrections.h"
#include "game/core/game_profile.h"
#include "game/core/image_view.h"
#include "game/titles/ff13_2/rendering/shadows/direct_atlas.h"
#include "patch/detour32.h"

#include <windows.h>
#include <d3d9.h>
#include <intrin.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace novafix::game::xiii2_shadow_fixed_depth {
namespace {

constexpr std::uint32_t kCasterDrawCallRva = 0x008442F0u;
constexpr std::uint32_t kCasterDrawContinueRva = 0x008442FAu;
constexpr std::uint32_t kIndexedDrawWrapperRva = 0x008215B0u;
constexpr std::uint32_t kShadowIndexedDrawReturnRva = 0x0086FEC8u;
constexpr std::uint32_t kStretchRectRva = 0x008219E0u;
constexpr std::uint32_t kShadowPackingCallRva = 0x008451D3u;
constexpr std::uint32_t kShadowPackingReturnRva = 0x008451D8u;
constexpr std::uint32_t kDeviceSlotRva = 0x04D6BA88u;
constexpr std::uint32_t kSecurityCookieRva = 0x01FA90DCu;
constexpr std::uint32_t kBlocNodeTag = 0x636F6C62u;

constexpr std::array<std::byte, 10> kCasterDrawCallPrefix{
    std::byte{0x8B}, std::byte{0x06},
    std::byte{0x8B}, std::byte{0x50}, std::byte{0x20},
    std::byte{0x55},
    std::byte{0x8B}, std::byte{0xCE},
    std::byte{0xFF}, std::byte{0xD2},
};
constexpr std::array<std::byte, 16> kIndexedDrawWrapperPrefix{
    std::byte{0x81}, std::byte{0xEC}, std::byte{0x08}, std::byte{0x04},
    std::byte{0x00}, std::byte{0x00},
    std::byte{0xA1}, std::byte{0xDC}, std::byte{0x90}, std::byte{0x3A},
    std::byte{0x02},
    std::byte{0x33}, std::byte{0xC4},
    std::byte{0x89}, std::byte{0x84}, std::byte{0x24},
};
constexpr std::array<std::byte, 9> kStretchRectPrefix{
    std::byte{0x8B}, std::byte{0x44}, std::byte{0x24}, std::byte{0x04},
    std::byte{0x33}, std::byte{0xD2},
    std::byte{0x83}, std::byte{0xEC}, std::byte{0x38},
};
constexpr std::array<std::byte, 5> kShadowPackingCall{
    std::byte{0xE8}, std::byte{0x08}, std::byte{0xC8},
    std::byte{0xFD}, std::byte{0xFF},
};

using StretchRect = int (__cdecl*)(
    void*, const RECT*, void*, const RECT*, int);

struct DepthBiasScope {
    IDirect3DDevice9* device{};
    DWORD savedBias{};
    bool active{};
};

INIT_ONCE g_initializeOnce = INIT_ONCE_STATIC_INIT;
patch::Detour32 g_casterDrawDetour;
patch::Detour32 g_indexedDrawDetour;
patch::Detour32 g_stretchRectDetour;
void* g_casterDrawTrampoline{};
void* g_casterDrawContinue{};
void* g_indexedDrawTrampoline{};
StretchRect g_originalStretchRect{};
std::byte* g_imageBase{};
void** g_deviceSource{};
std::atomic_bool g_available{false};
volatile LONG g_enabled{};
thread_local DepthBiasScope g_scope{};

template <std::size_t Size>
bool ApplyRelocatedAddress(std::array<std::byte, Size>& prefix,
                           std::size_t operandOffset,
                           std::uint32_t rva,
                           const ImageView& image) {
    if (operandOffset > prefix.size() ||
        sizeof(std::uint32_t) > prefix.size() - operandOffset) {
        return false;
    }
    const auto runtimeAddress = image.Address32(rva, sizeof(std::uint32_t));
    if (!runtimeAddress) return false;
    std::memcpy(prefix.data() + operandOffset, &*runtimeAddress,
                sizeof(*runtimeAddress));
    return true;
}

int __cdecl BeginDepthBias(void* caster) {
    if (!caster || !g_deviceSource ||
        InterlockedCompareExchange(&g_enabled, 0, 0) == 0) {
        return 0;
    }
    std::uint32_t nodeTag{};
    std::memcpy(&nodeTag, static_cast<const std::byte*>(caster) + 0x14C,
                sizeof(nodeTag));
    if (nodeTag != kBlocNodeTag) return 0;

    auto* device = static_cast<IDirect3DDevice9*>(*g_deviceSource);
    DWORD depthBias{};
    if (!device || FAILED(device->GetRenderState(D3DRS_DEPTHBIAS,
                                                  &depthBias))) {
        return 0;
    }
    g_scope = {device, depthBias, true};
    return 1;
}

void __cdecl EndDepthBias() {
    if (g_scope.active && g_scope.device) {
        g_scope.device->SetRenderState(D3DRS_DEPTHBIAS, g_scope.savedBias);
    }
    g_scope = {};
}

void __cdecl ApplyDepthBias(const std::uintptr_t* stack) {
    if (!stack || !g_scope.active || !g_scope.device || !g_imageBase ||
        stack[0] != reinterpret_cast<std::uintptr_t>(
                        g_imageBase + kShadowIndexedDrawReturnRva)) {
        return;
    }
    g_scope.device->SetRenderState(
        D3DRS_DEPTHBIAS,
        d3d9::shader_corrections::OpaqueShadowBiasBits());
}

#if defined(_M_IX86) || defined(__i386__)
__declspec(naked) void CasterDrawHook() {
    __asm {
        cmp dword ptr [g_enabled], 0
        je nativeDraw
        push esi
        call BeginDepthBias
        add esp, 4
        test eax, eax
        jz nativeDraw
        mov eax, dword ptr [esi]
        mov edx, dword ptr [eax + 20h]
        push ebp
        mov ecx, esi
        call edx
        call EndDepthBias
        jmp dword ptr [g_casterDrawContinue]
    nativeDraw:
        jmp dword ptr [g_casterDrawTrampoline]
    }
}

__declspec(naked) void IndexedDrawHook() {
    __asm {
        cmp dword ptr [g_enabled], 0
        je nativeDraw
        pushfd
        pushad
        lea eax, [esp + 36]
        push eax
        call ApplyDepthBias
        add esp, 4
        popad
        popfd
    nativeDraw:
        jmp dword ptr [g_indexedDrawTrampoline]
    }
}
#else
void CasterDrawHook() {}
void IndexedDrawHook() {}
#endif

int __cdecl StretchRectHook(
    void* source, const RECT* sourceRect, void* destination,
    const RECT* destinationRect, int filter) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.ff13-2.shadow-stretch-rect-total");
    StretchRect original = g_originalStretchRect;
    if (!original) return 0;
    const auto* returnAddress =
        reinterpret_cast<const std::byte*>(_ReturnAddress());
    const bool nativePackingCall = g_imageBase &&
        returnAddress == g_imageBase + kShadowPackingReturnRva;
    if (nativePackingCall &&
        xiii2_shadow_direct_atlas::SuppressNativePacking(source,
                                                          destination)) {
        return 0;
    }
    return original(source, sourceRect, destination, destinationRect, filter);
}

void RevertHooks() {
    g_stretchRectDetour.RevertAndClear(g_originalStretchRect);
    g_indexedDrawDetour.RevertAndClear(g_indexedDrawTrampoline);
    if (g_casterDrawDetour.RevertAndClear(g_casterDrawTrampoline)) {
        g_casterDrawContinue = nullptr;
    }
}

BOOL CALLBACK Initialize(PINIT_ONCE, PVOID, PVOID*) {
    if (CurrentProfile().title != Title::FinalFantasyXIII2) return TRUE;
    const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
    if (!image) {
        LogWarning("XIII-2 fixed-depth shadow unavailable: image contract missing");
        return TRUE;
    }

    auto indexedPrefix = kIndexedDrawWrapperPrefix;
    const auto casterDraw = image->At(
        kCasterDrawCallRva, kCasterDrawCallPrefix.size());
    const auto casterContinue = image->At(kCasterDrawContinueRva, 1u);
    const auto indexedDraw = image->At(
        kIndexedDrawWrapperRva, indexedPrefix.size());
    const auto indexedReturn = image->At(kShadowIndexedDrawReturnRva, 1u);
    const auto stretchRect = image->At(
        kStretchRectRva, kStretchRectPrefix.size());
    const auto packingCall = image->At(
        kShadowPackingCallRva, kShadowPackingCall.size());
    const auto packingReturn = image->At(kShadowPackingReturnRva, 1u);
    const auto deviceSlot = image->At(kDeviceSlotRva, sizeof(void*));
    if (!casterDraw || !casterContinue || !indexedDraw || !indexedReturn ||
        !stretchRect || !packingCall || !packingReturn || !deviceSlot ||
        !ApplyRelocatedAddress(indexedPrefix, 7u,
                               kSecurityCookieRva, *image) ||
        std::memcmp(packingCall->data(), kShadowPackingCall.data(),
                    kShadowPackingCall.size()) != 0) {
        LogWarning("XIII-2 fixed-depth shadow unavailable: executable contract mismatch");
        return TRUE;
    }

    g_imageBase = const_cast<std::byte*>(image->Bytes().data());
    g_deviceSource = reinterpret_cast<void**>(
        const_cast<std::byte*>(deviceSlot->data()));
    g_casterDrawContinue = const_cast<std::byte*>(casterContinue->data());
    const bool casterInstalled = g_casterDrawDetour.Install(
        "xiii2-shadow-fixed-depth-caster", 
        const_cast<std::byte*>(casterDraw->data()),
        reinterpret_cast<void*>(&CasterDrawHook), kCasterDrawCallPrefix,
        &g_casterDrawTrampoline);
    const bool indexedInstalled = casterInstalled &&
        g_indexedDrawDetour.Install(
            "xiii2-shadow-fixed-depth-draw",
            const_cast<std::byte*>(indexedDraw->data()),
            reinterpret_cast<void*>(&IndexedDrawHook), indexedPrefix,
            &g_indexedDrawTrampoline);
    const bool packingInstalled = indexedInstalled &&
        g_stretchRectDetour.Install(
            "xiii2-shadow-direct-atlas-pack",
            const_cast<std::byte*>(stretchRect->data()),
            reinterpret_cast<void*>(&StretchRectHook), kStretchRectPrefix,
            reinterpret_cast<void**>(&g_originalStretchRect));
    if (!packingInstalled) {
        RevertHooks();
        LogWarning("XIII-2 fixed-depth shadow unavailable: atomic hook ownership rejected; native producer retained");
        return TRUE;
    }

    g_available.store(true, std::memory_order_release);
    return TRUE;
}

} // namespace

void Pump() {
    InitOnceExecuteOnce(&g_initializeOnce, &Initialize, nullptr, nullptr);
}

bool Available() {
    return g_available.load(std::memory_order_acquire);
}

bool Enable() {
    if (!Available()) return false;
    InterlockedExchange(&g_enabled, 1);
    return true;
}

bool Enabled() {
    return InterlockedCompareExchange(&g_enabled, 0, 0) != 0;
}

} // namespace novafix::game::xiii2_shadow_fixed_depth
