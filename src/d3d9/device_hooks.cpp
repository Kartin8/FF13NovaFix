#include "d3d9/device_hooks.h"

#include "diagnostics/hook_profiler.h"
#include "diagnostics/log.h"
#include "patch/vtable_hook.h"
#include "compat/compatibility.h"
#include "compat/platform.h"
#include "d3d9/backend.h"
#include "d3d9/device_hook_policy.h"
#include "d3d9/device_recovery_policy.h"
#include "d3d9/device_shader_hooks.h"
#include "d3d9/device_state_hooks.h"
#include "d3d9/mip_lod_bias_controller.h"
#include "d3d9/presentation_router.h"
#include "d3d9/presentation_setup.h"
#include "d3d9/redundant_state_filter.h"
#include "d3d9/swap_chain_proxy.h"
#include "display/display_profile.h"
#include "display/scissor_transform.h"
#include "display/texture_policy.h"
#include "display/ui_render_policy.h"
#include "game/core/game_profile.h"
#include "game/shared/timing/cutscene_terminal_frame.h"
#include "game/titles/ff13/ui/prompt_mode_manager.h"
#include "game/shared/ui/scissor_fix.h"
#include "overlay/overlay.h"
#include "settings/graphics_settings.h"
#include "settings/performance_settings.h"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace novafix::d3d9 {
namespace {

using DeviceReleaseFn = ULONG (STDMETHODCALLTYPE*)(IDirect3DDevice9*);
using CreateAdditionalSwapChainFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*,
                                                                  IDirect3DSwapChain9**);
using GetSwapChainFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, IDirect3DSwapChain9**);
using DeviceResetFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using DevicePresentFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND,
                                                      const RGNDATA*);
using DeviceResetExFn = HRESULT (STDMETHODCALLTYPE*)(
    IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);
using DevicePresentExFn = HRESULT (STDMETHODCALLTYPE*)(
    IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
using SetScissorRectFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*);
using CreateVertexBufferFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, DWORD, DWORD,
                                                           D3DPOOL, IDirect3DVertexBuffer9**, HANDLE*);
using DrawPrimitiveUPFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE,
                                                        UINT, const void*, UINT);

struct DeviceRecord {
    IDirect3DDevice9* device{};
    void** vtable{};
    HWND window{};
    bool forcedProfile{};
    bool extended{};
    UINT backBufferWidth{};
    UINT backBufferHeight{};
};

enum class DeviceClaim : std::uint8_t {
    Release,
    CreateAdditionalSwapChain,
    GetSwapChain,
    Reset,
    Present,
    SetScissorRect,
    CreateVertexBuffer,
    DrawPrimitiveUp,
    PresentEx,
    ResetEx,
    Count,
};

constexpr std::size_t kDeviceClaimCount =
    static_cast<std::size_t>(DeviceClaim::Count);

struct DeviceVtableRecord {
    void** vtable{};
    DeviceReleaseFn release{};
    CreateAdditionalSwapChainFn createAdditionalSwapChain{};
    GetSwapChainFn getSwapChain{};
    DeviceResetFn reset{};
    DevicePresentFn present{};
    DeviceResetExFn resetEx{};
    DevicePresentExFn presentEx{};
    SetScissorRectFn setScissorRect{};
    CreateVertexBufferFn createVertexBuffer{};
    DrawPrimitiveUPFn drawPrimitiveUP{};
    std::array<patch::registry::ClaimId, kDeviceClaimCount> claims{};
};

patch::registry::ClaimId* Claim(
    DeviceVtableRecord& record, DeviceClaim claim) {
    return &record.claims[static_cast<std::size_t>(claim)];
}

std::vector<DeviceRecord> g_devices;
std::deque<DeviceVtableRecord> g_deviceVtables;
SRWLOCK g_lock = SRWLOCK_INIT;
std::atomic_uint64_t g_deviceGeneration{1};
std::atomic_uint g_loggedScissors{0};
std::atomic_uint g_loggedUiBuffers{0};
std::atomic_uint g_loggedScreenQuads{0};

bool ClaimLimitedLog(std::atomic_uint& counter, unsigned limit) {
    unsigned current = counter.load(std::memory_order_relaxed);
    while (current < limit) {
        if (counter.compare_exchange_weak(
                current, current + 1u, std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

DeviceRecord FindDevice(IDirect3DDevice9* device) {
    thread_local IDirect3DDevice9* cachedDevice = nullptr;
    thread_local std::uint64_t cachedGeneration{};
    thread_local DeviceRecord cachedRecord{};
    const std::uint64_t generation =
        g_deviceGeneration.load(std::memory_order_acquire);
    if (device == cachedDevice && cachedGeneration == generation) {
        return cachedRecord;
    }

    DeviceRecord result{};
    AcquireSRWLockShared(&g_lock);
    for (const auto& record : g_devices) {
        if (record.device == device) {
            result = record;
            break;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    cachedDevice = device;
    cachedGeneration = generation;
    cachedRecord = result;
    return result;
}

const DeviceVtableRecord& FindDeviceVtable(IDirect3DDevice9* device) {
    static const DeviceVtableRecord empty{};
    void** table = device ? *reinterpret_cast<void***>(device) : nullptr;
    if (!table) return empty;

    // Vtable records are process-lifetime stable; cache them per thread to
    // avoid locking and scanning on every draw/state call
    thread_local void** cachedTable = nullptr;
    thread_local const DeviceVtableRecord* cachedRecord = nullptr;
    if (table == cachedTable && cachedRecord) return *cachedRecord;

    const DeviceVtableRecord* result = nullptr;
    AcquireSRWLockShared(&g_lock);
    for (const auto& record : g_deviceVtables) {
        if (record.vtable == table) {
            result = &record;
            break;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    if (result) {
        cachedTable = table;
        cachedRecord = result;
    }
    return result ? *result : empty;
}

void DeactivateDevice(IDirect3DDevice9* device) {
    void** table = nullptr;
    std::array<patch::registry::ClaimId, kDeviceClaimCount> dormantClaims{};
    bool tableDormant = false;

    AcquireSRWLockExclusive(&g_lock);
    for (auto& record : g_devices) {
        if (record.device != device) continue;
        table = record.vtable;
        record = {};
        break;
    }
    if (table) {
        const bool stillActive = std::any_of(
            g_devices.begin(), g_devices.end(),
            [table](const DeviceRecord& record) {
                return record.device && record.vtable == table;
            });
        if (!stillActive) {
            for (const auto& record : g_deviceVtables) {
                if (record.vtable != table) continue;
                dormantClaims = record.claims;
                tableDormant = true;
                break;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    g_deviceGeneration.fetch_add(1u, std::memory_order_release);

    if (tableDormant) {
        for (const patch::registry::ClaimId claim : dormantClaims) {
            patch::registry::MarkDormant(claim);
        }
        device_shader_hooks::MarkDormant(table);
        device_state_hooks::MarkDormant(table);
    }
    // Keep saved originals so DXVK can reuse its discovery vtable. Integrity
    // is suspended only while no registered live device owns the table
}

void UpdateDevicePresentation(IDirect3DDevice9* device,
                              const D3DPRESENT_PARAMETERS* parameters) {
    if (!device || !parameters) return;
    AcquireSRWLockExclusive(&g_lock);
    for (auto& record : g_devices) {
        if (record.device == device) {
            record.window = parameters->hDeviceWindow ? parameters->hDeviceWindow : record.window;
            record.backBufferWidth = parameters->BackBufferWidth;
            record.backBufferHeight = parameters->BackBufferHeight;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    g_deviceGeneration.fetch_add(1u, std::memory_order_release);
}

HRESULT STDMETHODCALLTYPE DevicePresentHook(IDirect3DDevice9* self, const RECT* source, const RECT* destination,
                                             HWND overrideWindow, const RGNDATA* dirty) {
    static const auto totalPoint = hook_profiler::RegisterDynamicPoint(
        "hook.d3d9.device-present-total");
    static const auto originalPoint = hook_profiler::RegisterDynamicPoint(
        "hook.d3d9.device-present-original");
    hook_profiler::Scope totalTiming(totalPoint);
    const auto& functions = FindDeviceVtable(self);
    if (!functions.present) return D3DERR_INVALIDCALL;
    const DeviceRecord record = FindDevice(self);
    const bool owner = presentation::Begin(self, record.window, presentation::Path::Device);
    if (owner) mip_lod_bias::ApplyPendingState(self);
    const bool held = owner &&
        game::cutscene_terminal_frame::ShouldSuppressPresent();
    HRESULT result = D3D_OK;
    if (!held) {
        hook_profiler::Scope originalTiming(originalPoint);
        result = functions.present(
            self, source, destination, overrideWindow, dirty);
    }
    if (owner) presentation::End(result);
    return result;
}

HRESULT STDMETHODCALLTYPE DevicePresentExHook(
    IDirect3DDevice9Ex* self, const RECT* source, const RECT* destination,
    HWND overrideWindow, const RGNDATA* dirty, DWORD flags) {
    static const auto totalPoint = hook_profiler::RegisterDynamicPoint(
        "hook.d3d9.device-present-ex-total");
    static const auto originalPoint = hook_profiler::RegisterDynamicPoint(
        "hook.d3d9.device-present-ex-original");
    hook_profiler::Scope totalTiming(totalPoint);
    const auto& functions = FindDeviceVtable(self);
    if (!functions.presentEx) return D3DERR_INVALIDCALL;
    const DeviceRecord record = FindDevice(self);
    const bool owner =
        presentation::Begin(self, record.window, presentation::Path::Device);
    if (owner) mip_lod_bias::ApplyPendingState(self);
    const bool held = owner &&
        game::cutscene_terminal_frame::ShouldSuppressPresent();
    HRESULT result = D3D_OK;
    if (!held) {
        hook_profiler::Scope originalTiming(originalPoint);
        result = functions.presentEx(
            self, source, destination, overrideWindow, dirty, flags);
    }
    if (owner) presentation::End(result);
    return result;
}

ULONG STDMETHODCALLTYPE DeviceReleaseHook(IDirect3DDevice9* self) {
    const auto& functions = FindDeviceVtable(self);
    if (!functions.release) return 0;
    ULONG references = functions.release(self);
    if (references == 1 && overlay::OwnsDevice(self)) {
        // ImGui owns the sole remaining reference. Its shutdown performs the
        // final (nested) Release and the nested hook retires this device
        overlay::DetachDevice(self);
        return 0;
    } else if (references == 0) {
        overlay::DetachDevice(self);
    }
    if (references == 0) {
        redundant_state_filter::Invalidate(self);
        DeactivateDevice(self);
    }
    return references;
}

HRESULT STDMETHODCALLTYPE SetScissorRectHook(IDirect3DDevice9* self, const RECT* rectangle) {
    NOVAFIX_PROFILE_SAMPLED_NAMED_SCOPE(
        hookTiming, "hook.d3d9.set-scissor-rect-total", 64u);
    const auto& functions = FindDeviceVtable(self);
    if (!functions.setScissorRect) return D3DERR_INVALIDCALL;
    if (!rectangle || !game::scissor_fix::ShouldTransformCurrentThread()) {
        return functions.setScissorRect(self, rectangle);
    }
    const DeviceRecord record = FindDevice(self);
    if (record.backBufferWidth < 640 || record.backBufferHeight < 480) {
        return functions.setScissorRect(self, rectangle);
    }

    const display::ScissorRect source{
        rectangle->left, rectangle->top, rectangle->right, rectangle->bottom};
    const display::ScissorRect scaled = display::ScaleScissorRect(
        source, record.backBufferWidth, record.backBufferHeight);
    const RECT output{scaled.left, scaled.top, scaled.right, scaled.bottom};
    if (ClaimLimitedLog(g_loggedScissors, 6u)) {
        Log("Scissor scaled: %ld,%ld-%ld,%ld -> %ld,%ld-%ld,%ld target=%ux%u",
            rectangle->left, rectangle->top, rectangle->right, rectangle->bottom,
            output.left, output.top, output.right, output.bottom,
            record.backBufferWidth, record.backBufferHeight);
    }
    return functions.setScissorRect(self, &output);
}

HRESULT STDMETHODCALLTYPE CreateVertexBufferHook(
    IDirect3DDevice9* self, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
    IDirect3DVertexBuffer9** output, HANDLE* shared) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.d3d9.create-vertex-buffer-total");
    const auto& functions = FindDeviceVtable(self);
    if (!functions.createVertexBuffer) return D3DERR_INVALIDCALL;
    const bool optimize = compat::compatibility::AllowsNovaFixChanges() &&
        game::Supports(game::Capability::UiVertexBufferOptimization) &&
        settings::Active().optimizeUiVertexBuffer &&
        display::IsNativeUiVertexBuffer(
            {length, usage, fvf, static_cast<std::uint32_t>(pool),
             shared != nullptr});
    HRESULT result = D3DERR_INVALIDCALL;
    if (optimize) {
        result = functions.createVertexBuffer(
            self, length, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, fvf,
            D3DPOOL_SYSTEMMEM, output, shared);
        const bool logAllocation = ClaimLimitedLog(g_loggedUiBuffers, 4u);
        if (logAllocation) {
            Log("Crystal Tools UI vertex buffer: optimized=%d result=0x%08lX length=%u usage=0x%lX pool=%u",
                SUCCEEDED(result), static_cast<unsigned long>(result), length,
                static_cast<unsigned long>(usage), static_cast<unsigned>(pool));
        }
        if (FAILED(result)) {
            result = functions.createVertexBuffer(self, length, usage, fvf, pool, output, shared);
            if (logAllocation) {
                Log("UI vertex buffer optimization rejected by backend; original allocation fallback result=0x%08lX",
                    static_cast<unsigned long>(result));
            }
        }
    } else {
        result = functions.createVertexBuffer(self, length, usage, fvf, pool, output, shared);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE DrawPrimitiveUPHook(
    IDirect3DDevice9* self, D3DPRIMITIVETYPE primitiveType, UINT primitiveCount,
    const void* vertexData, UINT stride) {
    NOVAFIX_PROFILE_SAMPLED_NAMED_SCOPE(
        hookTiming, "hook.d3d9.draw-primitive-up-total", 64u);
    const auto& functions = FindDeviceVtable(self);
    if (!functions.drawPrimitiveUP) return D3DERR_INVALIDCALL;
    const auto submit = [&](const void* submittedVertices) {
        const HRESULT result = functions.drawPrimitiveUP(
            self, primitiveType, primitiveCount, submittedVertices, stride);
        return result;
    };
    if (primitiveType != D3DPT_TRIANGLEFAN || primitiveCount != 2 ||
        !settings::CorrectScreenSpaceAlignmentEnabled()) {
        return submit(vertexData);
    }
    if (!display::IsNativeScreenQuad(
            static_cast<const float*>(vertexData), stride)) {
        return submit(vertexData);
    }

    D3DVIEWPORT9 viewport{};
    display::ScreenQuad corrected{};
    if (SUCCEEDED(self->GetViewport(&viewport)) &&
        display::CorrectNativeScreenQuad(static_cast<const float*>(vertexData), stride,
                                         viewport.Width, viewport.Height, corrected)) {
        if (ClaimLimitedLog(g_loggedScreenQuads, 4u)) {
            Log("Crystal Tools screen quad aligned: viewport=%ux%u", viewport.Width, viewport.Height);
        }
        return submit(corrected.data());
    }
    return submit(vertexData);
}

HRESULT STDMETHODCALLTYPE DeviceResetHook(IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* parameters) {
    const auto& functions = FindDeviceVtable(self);
    if (!functions.reset) return D3DERR_INVALIDCALL;

    const DeviceRecord record = FindDevice(self);
    const bool conservativeRecovery =
        game::CurrentProfile().title == game::Title::FinalFantasyXIII;
    const HRESULT cooperativeResult = conservativeRecovery
        ? self->TestCooperativeLevel() : D3D_OK;
    const bool preserveRecoveryParameters =
        device_recovery::SuspendNovaFixWork(
            conservativeRecovery, cooperativeResult);
    if (!preserveRecoveryParameters) {
        if (record.forcedProfile && compat::compatibility::AllowsNovaFixChanges()) {
            display::ApplyProfile(record.window, parameters);
        }
        presentation_setup::ApplyDevicePolicy(self, parameters);
    } else {
        // GRC::OnSwap_ already supplies profiled swap-chain parameters
        // Do not rebuild them through IDirect3D9 during XIII device recovery
        Log("XIII recovery Reset preserving swap-chain parameters: cooperative=0x%08lX",
            static_cast<unsigned long>(cooperativeResult));
    }
    game::cutscene_terminal_frame::BeforeDeviceReset();
    if (game::CurrentProfile().title == game::Title::FinalFantasyXIII) {
        game::ff13_prompt_mode::BeforeDeviceReset();
    }
    overlay::BeforeDeviceReset(self);
    mip_lod_bias::ResetSamplerCache();
    redundant_state_filter::Invalidate(self);
    const HRESULT result = functions.reset(self, parameters);
    overlay::AfterDeviceReset(self, result);
    if (SUCCEEDED(result)) UpdateDevicePresentation(self, parameters);
    Log("IDirect3DDevice9::Reset device=%p result=0x%08lX size=%ux%u", self,
        static_cast<unsigned long>(result), parameters ? parameters->BackBufferWidth : 0,
        parameters ? parameters->BackBufferHeight : 0);
    return result;
}

HRESULT STDMETHODCALLTYPE DeviceResetExHook(
    IDirect3DDevice9Ex* self, D3DPRESENT_PARAMETERS* parameters,
    D3DDISPLAYMODEEX* fullscreenMode) {
    const auto& functions = FindDeviceVtable(self);
    if (!functions.resetEx) return D3DERR_INVALIDCALL;

    const DeviceRecord record = FindDevice(self);
    if (record.forcedProfile &&
        compat::compatibility::AllowsNovaFixChanges()) {
        display::ApplyProfile(record.window, parameters);
    }
    presentation_setup::ApplyDevicePolicy(self, parameters);

    IDirect3D9* d3d = nullptr;
    IDirect3D9Ex* d3dEx = nullptr;
    if (SUCCEEDED(self->GetDirect3D(&d3d)) && d3d) {
        d3d->QueryInterface(
            IID_IDirect3D9Ex, reinterpret_cast<void**>(&d3dEx));
        d3d->Release();
    }
    D3DDEVICE_CREATION_PARAMETERS creation{};
    self->GetCreationParameters(&creation);
    D3DDISPLAYMODEEX adjustedMode{};
    D3DDISPLAYMODEEX* effectiveMode =
        presentation_setup::ResolveFullscreenMode(
            d3dEx, creation.AdapterOrdinal, parameters, fullscreenMode,
            adjustedMode);

    game::cutscene_terminal_frame::BeforeDeviceReset();
    if (game::CurrentProfile().title == game::Title::FinalFantasyXIII) {
        game::ff13_prompt_mode::BeforeDeviceReset();
    }
    overlay::BeforeDeviceReset(self);
    mip_lod_bias::ResetSamplerCache();
    redundant_state_filter::Invalidate(self);
    const HRESULT result =
        functions.resetEx(self, parameters, effectiveMode);
    overlay::AfterDeviceReset(self, result);
    if (SUCCEEDED(result)) {
        UpdateDevicePresentation(self, parameters);
        if (fullscreenMode && effectiveMode) {
            *fullscreenMode = *effectiveMode;
        }
    }
    if (d3dEx) d3dEx->Release();
    Log("IDirect3DDevice9Ex::ResetEx device=%p result=0x%08lX size=%ux%u",
        self, static_cast<unsigned long>(result),
        parameters ? parameters->BackBufferWidth : 0,
        parameters ? parameters->BackBufferHeight : 0);
    return result;
}

HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChainHook(IDirect3DDevice9* self,
                                                         D3DPRESENT_PARAMETERS* parameters,
                                                         IDirect3DSwapChain9** output) {
    const auto& functions = FindDeviceVtable(self);
    if (!functions.createAdditionalSwapChain) return D3DERR_INVALIDCALL;
    const DeviceRecord record = FindDevice(self);
    return swap_chain::Wrap(functions.createAdditionalSwapChain(self, parameters, output),
                            output, self, record.window, record.extended);
}

HRESULT STDMETHODCALLTYPE GetSwapChainHook(IDirect3DDevice9* self, UINT index,
                                            IDirect3DSwapChain9** output) {
    const auto& functions = FindDeviceVtable(self);
    if (!functions.getSwapChain) return D3DERR_INVALIDCALL;
    const DeviceRecord record = FindDevice(self);
    return swap_chain::Wrap(functions.getSwapChain(self, index, output),
                            output, self, record.window, record.extended);
}

DeviceHookPolicy ConfigureDeviceHookPolicy() {
    const bool compatibilityAllowsChanges =
        compat::compatibility::AllowsNovaFixChanges();
    const settings::GraphicsSettings graphics = settings::Active();
    const settings::PerformanceSettings performance =
        settings::ActivePerformance();
    DeviceHookPolicy hookPolicy = ResolveDeviceHookPolicy({
        compatibilityAllowsChanges &&
            game::Supports(game::Capability::TextureFiltering),
        compatibilityAllowsChanges &&
            game::Supports(game::Capability::UiVertexBufferOptimization),
        compatibilityAllowsChanges &&
            game::Supports(game::Capability::ScreenSpaceAlignment),
        compatibilityAllowsChanges &&
            game::Supports(game::Capability::HighResolutionScissor),
        compatibilityAllowsChanges &&
            game::Supports(game::Capability::ShaderCorrection),
        graphics.optimizeUiVertexBuffer,
        graphics.xiii2ShaderCorrections,
        game::CurrentProfile().title == game::Title::FinalFantasyXIII2,
        compatibilityAllowsChanges &&
            game::CurrentProfile().title == game::Title::FinalFantasyXIII2,
        performance.xiii2OptimizeStateSubmission,
    });
    redundant_state_filter::Configure(hookPolicy.redundantStateFilter);
    mip_lod_bias::Configure(
        hookPolicy.setSamplerState,
        game::CurrentProfile().title == game::Title::FinalFantasyXIII2,
        graphics.anisotropicFiltering,
        graphics.xiii2MipLodBias);
    return hookPolicy;
}

DeviceVtableRecord* RecordDevice(
    IDirect3DDevice9* device, void** table, HWND window, bool forcedProfile,
    const D3DPRESENT_PARAMETERS* parameters, bool extended) {
    bool deviceRecorded = false;
    for (auto& record : g_devices) {
        if (!record.device || record.device == device) {
            record.device = device;
            record.vtable = table;
            record.window = window;
            record.forcedProfile = forcedProfile;
            record.extended = extended;
            record.backBufferWidth = parameters ? parameters->BackBufferWidth : 0;
            record.backBufferHeight = parameters ? parameters->BackBufferHeight : 0;
            deviceRecorded = true;
            break;
        }
    }
    if (!deviceRecorded) {
        g_devices.push_back({
            device, table, window, forcedProfile, extended,
            parameters ? parameters->BackBufferWidth : 0,
            parameters ? parameters->BackBufferHeight : 0,
        });
    }

    DeviceVtableRecord* vtableRecord = nullptr;
    for (auto& record : g_deviceVtables) {
        if (record.vtable == table) {
            vtableRecord = &record;
            break;
        }
    }
    if (!vtableRecord) {
        g_deviceVtables.emplace_back();
        vtableRecord = &g_deviceVtables.back();
    }
    return vtableRecord;
}

void ReactivateDeviceHooks(
    IDirect3DDevice9* device, void** table, DeviceVtableRecord* vtableRecord) {
    if (vtableRecord->vtable) {
        bool reactivated = true;
        for (const patch::registry::ClaimId claim : vtableRecord->claims) {
            if (claim &&
                !patch::registry::ReactivateVtableClaim(claim)) {
                reactivated = false;
            }
        }
        if (!reactivated) {
            LogError("IDirect3DDevice9 hook reactivation failed: device=%p table=%p",
                device, table);
        }
    }
    device_shader_hooks::Reactivate(table);
    device_state_hooks::Reactivate(table);
}

void InstallDeviceHooks(
    void** table, DeviceVtableRecord* vtableRecord,
    const DeviceHookPolicy& hookPolicy) {
    device_state_hooks::Install(table, hookPolicy);
    device_shader_hooks::Install(table, hookPolicy);
    if (!vtableRecord->vtable) {
        vtableRecord->vtable = table;
        void* originalRelease = nullptr;
        void* originalCreateAdditional = nullptr;
        void* originalGetSwapChain = nullptr;
        void* originalReset = nullptr;
        void* originalPresent = nullptr;
        void* originalSetScissorRect = nullptr;
        void* originalDrawPrimitiveUP = nullptr;
        void* originalVertexBuffer = nullptr;
        const auto replaceOptional =
            [table](bool enabled, const char* feature, size_t index,
                    void* replacement, void** original,
                    patch::registry::ClaimId* claim) {
                return enabled && ReplaceVtableSlot(
                    feature, table, index, replacement, original, claim);
            };
        const bool releaseOk = ReplaceVtableSlot(
            "d3d9-device:release", table, 2,
            reinterpret_cast<void*>(&DeviceReleaseHook), &originalRelease,
            Claim(*vtableRecord, DeviceClaim::Release));
        const bool createAdditionalOk = ReplaceVtableSlot(
            "d3d9-device:create-additional-swap-chain", table, 13,
            reinterpret_cast<void*>(&CreateAdditionalSwapChainHook), &originalCreateAdditional,
            Claim(*vtableRecord, DeviceClaim::CreateAdditionalSwapChain));
        const bool getSwapChainOk = ReplaceVtableSlot(
            "d3d9-device:get-swap-chain", table, 14,
            reinterpret_cast<void*>(&GetSwapChainHook), &originalGetSwapChain,
            Claim(*vtableRecord, DeviceClaim::GetSwapChain));
        const bool resetOk = ReplaceVtableSlot(
            "d3d9-device:reset", table, 16,
            reinterpret_cast<void*>(&DeviceResetHook),
            &originalReset,
            Claim(*vtableRecord, DeviceClaim::Reset));
        const bool presentOk = ReplaceVtableSlot(
            "d3d9-device:present", table, 17,
            reinterpret_cast<void*>(&DevicePresentHook), &originalPresent,
            Claim(*vtableRecord, DeviceClaim::Present));
        const bool scissorOk = replaceOptional(
            hookPolicy.setScissorRect, "d3d9-device:set-scissor-rect", 75,
            reinterpret_cast<void*>(&SetScissorRectHook), &originalSetScissorRect,
            Claim(*vtableRecord, DeviceClaim::SetScissorRect));
        const bool vertexBufferOk = replaceOptional(
            hookPolicy.createVertexBuffer, "d3d9-device:create-vertex-buffer", 26,
            reinterpret_cast<void*>(&CreateVertexBufferHook),
            &originalVertexBuffer,
            Claim(*vtableRecord, DeviceClaim::CreateVertexBuffer));
        const bool drawPrimitiveUpOk = replaceOptional(
            hookPolicy.drawPrimitiveUp, "d3d9-device:draw-primitive-up", 83,
            reinterpret_cast<void*>(&DrawPrimitiveUPHook),
            &originalDrawPrimitiveUP,
            Claim(*vtableRecord, DeviceClaim::DrawPrimitiveUp));
        if (releaseOk) vtableRecord->release = reinterpret_cast<DeviceReleaseFn>(originalRelease);
        if (createAdditionalOk) {
            vtableRecord->createAdditionalSwapChain =
                reinterpret_cast<CreateAdditionalSwapChainFn>(originalCreateAdditional);
        }
        if (getSwapChainOk) vtableRecord->getSwapChain = reinterpret_cast<GetSwapChainFn>(originalGetSwapChain);
        if (resetOk) vtableRecord->reset = reinterpret_cast<DeviceResetFn>(originalReset);
        if (presentOk) vtableRecord->present = reinterpret_cast<DevicePresentFn>(originalPresent);
        if (scissorOk) {
            vtableRecord->setScissorRect = reinterpret_cast<SetScissorRectFn>(originalSetScissorRect);
        }
        if (vertexBufferOk) vtableRecord->createVertexBuffer = reinterpret_cast<CreateVertexBufferFn>(originalVertexBuffer);
        if (drawPrimitiveUpOk) vtableRecord->drawPrimitiveUP = reinterpret_cast<DrawPrimitiveUPFn>(originalDrawPrimitiveUP);
    }
}

void InstallExtendedDeviceHooks(
    void** table, DeviceVtableRecord* vtableRecord) {
    bool presentExOk = vtableRecord->presentEx != nullptr;
    bool resetExOk = vtableRecord->resetEx != nullptr;
    if (!presentExOk) {
        void* originalPresentEx = nullptr;
        presentExOk = ReplaceVtableSlot(
            "d3d9-device:present-ex", table, 121,
            reinterpret_cast<void*>(&DevicePresentExHook),
            &originalPresentEx,
            Claim(*vtableRecord, DeviceClaim::PresentEx));
        if (presentExOk) {
            vtableRecord->presentEx =
                reinterpret_cast<DevicePresentExFn>(originalPresentEx);
        }
    }
    if (!resetExOk) {
        void* originalResetEx = nullptr;
        resetExOk = ReplaceVtableSlot(
            "d3d9-device:reset-ex", table, 132,
            reinterpret_cast<void*>(&DeviceResetExHook),
            &originalResetEx,
            Claim(*vtableRecord, DeviceClaim::ResetEx));
        if (resetExOk) {
            vtableRecord->resetEx =
                reinterpret_cast<DeviceResetExFn>(originalResetEx);
        }
    }
}

void RegisterDeviceInternal(
    IDirect3DDevice9* device, HWND window, bool forcedProfile,
    const D3DPRESENT_PARAMETERS* parameters, bool extended) {
    if (!device) return;
    void** table = *reinterpret_cast<void***>(device);
    const DeviceHookPolicy hookPolicy = ConfigureDeviceHookPolicy();

    AcquireSRWLockExclusive(&g_lock);
    DeviceVtableRecord* record = RecordDevice(
        device, table, window, forcedProfile, parameters, extended);
    ReactivateDeviceHooks(device, table, record);
    InstallDeviceHooks(table, record, hookPolicy);
    if (extended) InstallExtendedDeviceHooks(table, record);
    ReleaseSRWLockExclusive(&g_lock);
    g_deviceGeneration.fetch_add(1u, std::memory_order_release);
}

} // namespace

namespace device_hooks {

void Register(
    IDirect3DDevice9* device, HWND window, bool forcedProfile,
    const D3DPRESENT_PARAMETERS* parameters, bool extended) {
    RegisterDeviceInternal(
        device, window, forcedProfile, parameters, extended);
}

void PreviewXiii2MipLodBias(float value) {
    mip_lod_bias::Preview(settings::Active().anisotropicFiltering, value);
}

void PreviewTextureFiltering(unsigned anisotropy, float mipLodBias) {
    mip_lod_bias::Preview(anisotropy, mipLodBias);
}

} // namespace device_hooks

} // namespace novafix::d3d9
