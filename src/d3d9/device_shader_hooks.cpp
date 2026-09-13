#include "d3d9/device_shader_hooks.h"

#include "diagnostics/hook_profiler.h"
#include "diagnostics/log.h"
#include "d3d9/alpha_shadow_variants.h"
#include "d3d9/device_hook_policy.h"
#include "d3d9/mip_lod_bias_controller.h"
#include "d3d9/redundant_state_filter.h"
#include "d3d9/shader_corrections.h"
#include "patch/vtable_hook.h"

#include <windows.h>
#include <d3d9.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>

namespace novafix::d3d9::device_shader_hooks {
namespace {

using SetSamplerStateFn = HRESULT (STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);
using CreatePixelShaderFn = HRESULT (STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);
using SetPixelShaderFn = HRESULT (STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, IDirect3DPixelShader9*);
using SetPixelShaderConstantFFn = HRESULT (STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, UINT, const float*, UINT);

enum class Claim : std::uint8_t {
    SamplerState,
    CreatePixelShader,
    PixelShader,
    PixelShaderConstantF,
    Count,
};

constexpr std::size_t kClaimCount = static_cast<std::size_t>(Claim::Count);

struct VtableRecord {
    void** vtable{};
    SetSamplerStateFn setSamplerState{};
    CreatePixelShaderFn createPixelShader{};
    SetPixelShaderFn setPixelShader{};
    SetPixelShaderConstantFFn setPixelShaderConstantF{};
    std::array<patch::registry::ClaimId, kClaimCount> claims{};
};

std::deque<VtableRecord> g_vtables;
SRWLOCK g_lock = SRWLOCK_INIT;

patch::registry::ClaimId* ClaimSlot(VtableRecord& record, Claim claim) {
    return &record.claims[static_cast<std::size_t>(claim)];
}

const VtableRecord* FindVtable(IDirect3DDevice9* device) {
    void** table = device ? *reinterpret_cast<void***>(device) : nullptr;
    if (!table) return nullptr;
    thread_local void** cachedTable{};
    thread_local const VtableRecord* cachedRecord{};
    if (table == cachedTable && cachedRecord) return cachedRecord;
    const VtableRecord* result{};
    AcquireSRWLockShared(&g_lock);
    for (const auto& record : g_vtables) {
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
    return result;
}

HRESULT STDMETHODCALLTYPE SetSamplerStateHook(
    IDirect3DDevice9* self, DWORD sampler, D3DSAMPLERSTATETYPE type,
    DWORD value) {
    NOVAFIX_PROFILE_SAMPLED_NAMED_SCOPE(
        hookTiming, "hook.d3d9.set-sampler-state-total", 128u);
    const VtableRecord* functions = FindVtable(self);
    if (!functions || !functions->setSamplerState) return D3DERR_INVALIDCALL;
    value = mip_lod_bias::AdjustSamplerState(
        sampler, static_cast<std::uint32_t>(type), value);
    if (redundant_state_filter::SamplerState(
            self, sampler, type, value)) return D3D_OK;
    const HRESULT result =
        functions->setSamplerState(self, sampler, type, value);
    if (SUCCEEDED(result)) {
        redundant_state_filter::CommitSamplerState(
            self, sampler, type, value);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE CreatePixelShaderHook(
    IDirect3DDevice9* self, const DWORD* function,
    IDirect3DPixelShader9** output) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.d3d9.create-pixel-shader-total");
    const VtableRecord* functions = FindVtable(self);
    if (!functions || !functions->createPixelShader) {
        return D3DERR_INVALIDCALL;
    }
    const shader_corrections::Resolution resolution =
        shader_corrections::ResolveBytecode(function);
    if (output) *output = nullptr;
    HRESULT result = functions->createPixelShader(
        self, resolution.bytecode, output);
    IDirect3DPixelShader9* created =
        SUCCEEDED(result) && output ? *output : nullptr;
    shader_corrections::RecordCreation(resolution, created, result);
    if (created && alpha_shadow_variants::NeedsVariant(resolution)) {
        IDirect3DPixelShader9* variant{};
        const HRESULT variantResult = functions->createPixelShader(
            self, alpha_shadow_variants::FixedDepthBytecode(),
            &variant);
        alpha_shadow_variants::RecordVariant(
            created, variant, variantResult);
    }
    if (resolution.replaced && FAILED(result)) {
        if (output && *output) {
            (*output)->Release();
            *output = nullptr;
        }
        result = functions->createPixelShader(self, function, output);
        created = SUCCEEDED(result) && output ? *output : nullptr;
    }
    if (created && shader_corrections::NeedsOpaqueShadowVariant(resolution)) {
        constexpr auto mode =
            shader_corrections::OpaqueShadowMode::FixedFunctionDepth;
        IDirect3DPixelShader9* variant{};
        const HRESULT variantResult = functions->createPixelShader(
            self, shader_corrections::OpaqueShadowBytecode(mode), &variant);
        shader_corrections::RecordOpaqueShadowVariant(
            created, mode, variant, variantResult);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE SetPixelShaderHook(
    IDirect3DDevice9* self, IDirect3DPixelShader9* shader) {
    NOVAFIX_PROFILE_SAMPLED_NAMED_SCOPE(
        hookTiming, "hook.d3d9.set-pixel-shader-total", 128u);
    const VtableRecord* functions = FindVtable(self);
    if (!functions || !functions->setPixelShader) return D3DERR_INVALIDCALL;
    IDirect3DPixelShader9* resolved =
        shader_corrections::ResolveOpaqueShadowMode(shader);
    resolved = alpha_shadow_variants::ResolveFixedDepth(
        resolved, alpha_shadow_variants::FixedDepthEnabled());
    if (redundant_state_filter::PixelShader(self, resolved)) return D3D_OK;
    const HRESULT result = functions->setPixelShader(self, resolved);
    if (SUCCEEDED(result)) {
        redundant_state_filter::CommitPixelShader(self, resolved);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE SetPixelShaderConstantFHook(
    IDirect3DDevice9* self, UINT startRegister, const float* values,
    UINT vectorCount) {
    NOVAFIX_PROFILE_SAMPLED_NAMED_SCOPE(
        hookTiming, "hook.d3d9.set-pixel-constants-total", 128u);
    if (redundant_state_filter::PixelShaderConstantF(
            self, startRegister, values, vectorCount)) {
        shader_corrections::ObserveOpaqueShadowConstants(
            startRegister, values, vectorCount);
        return D3D_OK;
    }
    const VtableRecord* functions = FindVtable(self);
    if (!functions || !functions->setPixelShaderConstantF) {
        return D3DERR_INVALIDCALL;
    }
    const HRESULT result = functions->setPixelShaderConstantF(
        self, startRegister, values, vectorCount);
    if (SUCCEEDED(result)) {
        redundant_state_filter::CommitPixelShaderConstantF(
            self, startRegister, values, vectorCount);
        shader_corrections::ObserveOpaqueShadowConstants(
            startRegister, values, vectorCount);
    }
    return result;
}

template <typename Function>
bool InstallSlot(bool enabled, const char* feature, void** table,
                 std::size_t index, void* replacement, Function& destination,
                 patch::registry::ClaimId* claim) {
    if (!enabled) return false;
    void* original{};
    const bool installed = ReplaceVtableSlot(
        feature, table, index, replacement, &original, claim);
    if (installed) destination = reinterpret_cast<Function>(original);
    return installed;
}

} // namespace

void Install(void** table, const DeviceHookPolicy& policy) {
    if (!table) return;
    AcquireSRWLockExclusive(&g_lock);
    for (const auto& record : g_vtables) {
        if (record.vtable == table) {
            ReleaseSRWLockExclusive(&g_lock);
            return;
        }
    }
    g_vtables.emplace_back();
    VtableRecord& record = g_vtables.back();
    record.vtable = table;
    (void)InstallSlot(
        policy.setSamplerState, "d3d9-device:set-sampler-state", table, 69u,
        reinterpret_cast<void*>(&SetSamplerStateHook), record.setSamplerState,
        ClaimSlot(record, Claim::SamplerState));
    (void)InstallSlot(
        policy.createPixelShader, "d3d9-device:create-pixel-shader", table,
        106u, reinterpret_cast<void*>(&CreatePixelShaderHook),
        record.createPixelShader, ClaimSlot(record, Claim::CreatePixelShader));
    (void)InstallSlot(
        policy.setPixelShader, "d3d9-device:set-pixel-shader", table, 107u,
        reinterpret_cast<void*>(&SetPixelShaderHook), record.setPixelShader,
        ClaimSlot(record, Claim::PixelShader));
    (void)InstallSlot(
        policy.setPixelShaderConstantF,
        "d3d9-device:set-pixel-shader-constant-f", table, 109u,
        reinterpret_cast<void*>(&SetPixelShaderConstantFHook),
        record.setPixelShaderConstantF,
        ClaimSlot(record, Claim::PixelShaderConstantF));
    ReleaseSRWLockExclusive(&g_lock);
}

void Reactivate(void** table) {
    if (!table) return;
    std::array<patch::registry::ClaimId, kClaimCount> claims{};
    AcquireSRWLockShared(&g_lock);
    for (const auto& record : g_vtables) {
        if (record.vtable == table) {
            claims = record.claims;
            break;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    bool reactivated = true;
    for (const patch::registry::ClaimId claim : claims) {
        if (claim && !patch::registry::ReactivateVtableClaim(claim)) {
            reactivated = false;
        }
    }
    if (!reactivated) {
        redundant_state_filter::Configure(false);
        LogError("IDirect3DDevice9 shader-hook reactivation failed: table=%p redundant-state-filter=disabled",
                 table);
    }
}

void MarkDormant(void** table) {
    if (!table) return;
    std::array<patch::registry::ClaimId, kClaimCount> claims{};
    AcquireSRWLockShared(&g_lock);
    for (const auto& record : g_vtables) {
        if (record.vtable == table) {
            claims = record.claims;
            break;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    for (const patch::registry::ClaimId claim : claims) {
        if (claim) patch::registry::MarkDormant(claim);
    }
}

} // namespace novafix::d3d9::device_shader_hooks
