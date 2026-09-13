#include "d3d9/device_state_hooks.h"

#include "diagnostics/log.h"
#include "d3d9/device_hook_policy.h"
#include "d3d9/redundant_state_filter.h"
#include "patch/vtable_hook.h"

#include <windows.h>
#include <d3d9.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>

namespace novafix::d3d9::device_state_hooks {
namespace {

using SetRenderStateFn = HRESULT (STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
using SetTextureFn = HRESULT (STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
using SetTextureStageStateFn = HRESULT (STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, DWORD, D3DTEXTURESTAGESTATETYPE, DWORD);
using SetVertexShaderConstantFFn = HRESULT (STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, UINT, const float*, UINT);
using SetVertexShaderFn = HRESULT (STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, IDirect3DVertexShader9*);
using SetStreamSourceFn = HRESULT (STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, UINT, IDirect3DVertexBuffer9*, UINT, UINT);
using CreateStateBlockFn = HRESULT (STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, D3DSTATEBLOCKTYPE, IDirect3DStateBlock9**);
using BeginStateBlockFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*);
using EndStateBlockFn = HRESULT (STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, IDirect3DStateBlock9**);
using StateBlockApplyFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DStateBlock9*);

enum class Claim : std::uint8_t {
    RenderState,
    Texture,
    TextureStageState,
    VertexShaderConstantF,
    VertexShader,
    StreamSource,
    CreateStateBlock,
    BeginStateBlock,
    EndStateBlock,
    Count,
};

constexpr std::size_t kClaimCount = static_cast<std::size_t>(Claim::Count);

struct VtableRecord {
    void** vtable{};
    SetRenderStateFn setRenderState{};
    SetTextureFn setTexture{};
    SetTextureStageStateFn setTextureStageState{};
    SetVertexShaderConstantFFn setVertexShaderConstantF{};
    SetVertexShaderFn setVertexShader{};
    SetStreamSourceFn setStreamSource{};
    CreateStateBlockFn createStateBlock{};
    BeginStateBlockFn beginStateBlock{};
    EndStateBlockFn endStateBlock{};
    std::array<patch::registry::ClaimId, kClaimCount> claims{};
};

struct StateBlockVtableRecord {
    void** vtable{};
    StateBlockApplyFn apply{};
    patch::registry::ClaimId claim{};
};

std::deque<VtableRecord> g_vtables;
SRWLOCK g_vtableLock = SRWLOCK_INIT;
std::deque<StateBlockVtableRecord> g_stateBlockVtables;
SRWLOCK g_stateBlockLock = SRWLOCK_INIT;
std::atomic_bool g_stateBlockHookFailureLogged{false};

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
    AcquireSRWLockShared(&g_vtableLock);
    for (const auto& record : g_vtables) {
        if (record.vtable == table) {
            result = &record;
            break;
        }
    }
    ReleaseSRWLockShared(&g_vtableLock);
    if (result) {
        cachedTable = table;
        cachedRecord = result;
    }
    return result;
}

const StateBlockVtableRecord* FindStateBlockVtable(
    IDirect3DStateBlock9* stateBlock) {
    void** table = stateBlock
        ? *reinterpret_cast<void***>(stateBlock) : nullptr;
    if (!table) return nullptr;
    thread_local void** cachedTable{};
    thread_local const StateBlockVtableRecord* cachedRecord{};
    if (table == cachedTable && cachedRecord) return cachedRecord;
    const StateBlockVtableRecord* result{};
    AcquireSRWLockShared(&g_stateBlockLock);
    for (const auto& record : g_stateBlockVtables) {
        if (record.vtable == table) {
            result = &record;
            break;
        }
    }
    ReleaseSRWLockShared(&g_stateBlockLock);
    if (result) {
        cachedTable = table;
        cachedRecord = result;
    }
    return result;
}

HRESULT STDMETHODCALLTYPE StateBlockApplyHook(IDirect3DStateBlock9* self) {
    const StateBlockVtableRecord* record = FindStateBlockVtable(self);
    if (!record || !record->apply) return D3DERR_INVALIDCALL;
    const HRESULT result = record->apply(self);
    if (SUCCEEDED(result)) redundant_state_filter::Invalidate();
    return result;
}

void TrackStateBlock(IDirect3DStateBlock9* stateBlock) {
    if (!stateBlock) return;
    void** table = *reinterpret_cast<void***>(stateBlock);
    if (!table) return;
    AcquireSRWLockExclusive(&g_stateBlockLock);
    for (const auto& record : g_stateBlockVtables) {
        if (record.vtable == table) {
            ReleaseSRWLockExclusive(&g_stateBlockLock);
            return;
        }
    }
    g_stateBlockVtables.emplace_back();
    StateBlockVtableRecord& record = g_stateBlockVtables.back();
    record.vtable = table;
    void* original{};
    const bool installed = ReplaceVtableSlot(
        "d3d9-state-block:apply", table, 5u,
        reinterpret_cast<void*>(&StateBlockApplyHook), &original,
        &record.claim);
    if (installed) {
        record.apply = reinterpret_cast<StateBlockApplyFn>(original);
    } else {
        g_stateBlockVtables.pop_back();
    }
    ReleaseSRWLockExclusive(&g_stateBlockLock);
    if (!installed && !g_stateBlockHookFailureLogged.exchange(true)) {
        LogWarning("XIII-2 could not hook a D3D9 state block: redundant-state filtering will fail open");
        redundant_state_filter::Configure(false);
    }
}

HRESULT STDMETHODCALLTYPE SetRenderStateHook(
    IDirect3DDevice9* self, D3DRENDERSTATETYPE state, DWORD value) {
    if (redundant_state_filter::RenderState(self, state, value)) {
        return D3D_OK;
    }
    const VtableRecord* functions = FindVtable(self);
    if (!functions || !functions->setRenderState) return D3DERR_INVALIDCALL;
    const HRESULT result = functions->setRenderState(self, state, value);
    if (SUCCEEDED(result)) {
        redundant_state_filter::CommitRenderState(self, state, value);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE SetTextureHook(
    IDirect3DDevice9* self, DWORD stage, IDirect3DBaseTexture9* texture) {
    if (redundant_state_filter::Texture(self, stage, texture)) return D3D_OK;
    const VtableRecord* functions = FindVtable(self);
    if (!functions || !functions->setTexture) return D3DERR_INVALIDCALL;
    const HRESULT result = functions->setTexture(self, stage, texture);
    if (SUCCEEDED(result)) {
        redundant_state_filter::CommitTexture(self, stage, texture);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE SetTextureStageStateHook(
    IDirect3DDevice9* self, DWORD stage, D3DTEXTURESTAGESTATETYPE type,
    DWORD value) {
    if (redundant_state_filter::TextureStageState(
            self, stage, type, value)) return D3D_OK;
    const VtableRecord* functions = FindVtable(self);
    if (!functions || !functions->setTextureStageState) {
        return D3DERR_INVALIDCALL;
    }
    const HRESULT result =
        functions->setTextureStageState(self, stage, type, value);
    if (SUCCEEDED(result)) {
        redundant_state_filter::CommitTextureStageState(
            self, stage, type, value);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE SetVertexShaderConstantFHook(
    IDirect3DDevice9* self, UINT startRegister, const float* values,
    UINT vectorCount) {
    if (redundant_state_filter::VertexShaderConstantF(
            self, startRegister, values, vectorCount)) return D3D_OK;
    const VtableRecord* functions = FindVtable(self);
    if (!functions || !functions->setVertexShaderConstantF) {
        return D3DERR_INVALIDCALL;
    }
    const HRESULT result = functions->setVertexShaderConstantF(
        self, startRegister, values, vectorCount);
    if (SUCCEEDED(result)) {
        redundant_state_filter::CommitVertexShaderConstantF(
            self, startRegister, values, vectorCount);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE SetVertexShaderHook(
    IDirect3DDevice9* self, IDirect3DVertexShader9* shader) {
    if (redundant_state_filter::VertexShader(self, shader)) return D3D_OK;
    const VtableRecord* functions = FindVtable(self);
    if (!functions || !functions->setVertexShader) return D3DERR_INVALIDCALL;
    const HRESULT result = functions->setVertexShader(self, shader);
    if (SUCCEEDED(result)) {
        redundant_state_filter::CommitVertexShader(self, shader);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE SetStreamSourceHook(
    IDirect3DDevice9* self, UINT stream, IDirect3DVertexBuffer9* buffer,
    UINT offset, UINT stride) {
    if (redundant_state_filter::StreamSource(
            self, stream, buffer, offset, stride)) return D3D_OK;
    const VtableRecord* functions = FindVtable(self);
    if (!functions || !functions->setStreamSource) return D3DERR_INVALIDCALL;
    const HRESULT result = functions->setStreamSource(
        self, stream, buffer, offset, stride);
    if (SUCCEEDED(result)) {
        redundant_state_filter::CommitStreamSource(
            self, stream, buffer, offset, stride);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE CreateStateBlockHook(
    IDirect3DDevice9* self, D3DSTATEBLOCKTYPE type,
    IDirect3DStateBlock9** output) {
    const VtableRecord* functions = FindVtable(self);
    if (!functions || !functions->createStateBlock) return D3DERR_INVALIDCALL;
    const HRESULT result = functions->createStateBlock(self, type, output);
    if (SUCCEEDED(result) && output) TrackStateBlock(*output);
    return result;
}

HRESULT STDMETHODCALLTYPE BeginStateBlockHook(IDirect3DDevice9* self) {
    const VtableRecord* functions = FindVtable(self);
    if (!functions || !functions->beginStateBlock) return D3DERR_INVALIDCALL;
    const HRESULT result = functions->beginStateBlock(self);
    redundant_state_filter::BeginStateBlock(self, SUCCEEDED(result));
    return result;
}

HRESULT STDMETHODCALLTYPE EndStateBlockHook(
    IDirect3DDevice9* self, IDirect3DStateBlock9** output) {
    const VtableRecord* functions = FindVtable(self);
    if (!functions || !functions->endStateBlock) return D3DERR_INVALIDCALL;
    const HRESULT result = functions->endStateBlock(self, output);
    redundant_state_filter::EndStateBlock(self, SUCCEEDED(result));
    if (SUCCEEDED(result) && output) TrackStateBlock(*output);
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
    AcquireSRWLockExclusive(&g_vtableLock);
    VtableRecord* record{};
    for (auto& candidate : g_vtables) {
        if (candidate.vtable == table) {
            record = &candidate;
            break;
        }
    }
    if (record) {
        ReleaseSRWLockExclusive(&g_vtableLock);
        return;
    }
    g_vtables.emplace_back();
    record = &g_vtables.back();
    record->vtable = table;
    (void)InstallSlot(
        policy.setRenderState,
        "d3d9-device:set-render-state", table, 57u,
        reinterpret_cast<void*>(&SetRenderStateHook), record->setRenderState,
        ClaimSlot(*record, Claim::RenderState));
    (void)InstallSlot(
        policy.setTexture, "d3d9-device:set-texture", table,
        65u, reinterpret_cast<void*>(&SetTextureHook), record->setTexture,
        ClaimSlot(*record, Claim::Texture));
    (void)InstallSlot(
        policy.setTextureStageState,
        "d3d9-device:set-texture-stage-state", table, 67u,
        reinterpret_cast<void*>(&SetTextureStageStateHook),
        record->setTextureStageState,
        ClaimSlot(*record, Claim::TextureStageState));
    (void)InstallSlot(
        policy.setVertexShaderConstantF,
        "d3d9-device:set-vertex-constants", table, 94u,
        reinterpret_cast<void*>(&SetVertexShaderConstantFHook),
        record->setVertexShaderConstantF,
        ClaimSlot(*record, Claim::VertexShaderConstantF));
    (void)InstallSlot(
        policy.setVertexShader,
        "d3d9-device:set-vertex-shader", table, 92u,
        reinterpret_cast<void*>(&SetVertexShaderHook), record->setVertexShader,
        ClaimSlot(*record, Claim::VertexShader));
    (void)InstallSlot(
        policy.setStreamSource,
        "d3d9-device:set-stream-source", table, 100u,
        reinterpret_cast<void*>(&SetStreamSourceHook), record->setStreamSource,
        ClaimSlot(*record, Claim::StreamSource));
    const bool createStateBlock = InstallSlot(
        policy.stateBlocks, "d3d9-device:create-state-block",
        table, 59u, reinterpret_cast<void*>(&CreateStateBlockHook),
        record->createStateBlock, ClaimSlot(*record, Claim::CreateStateBlock));
    const bool beginStateBlock = InstallSlot(
        policy.stateBlocks, "d3d9-device:begin-state-block",
        table, 60u, reinterpret_cast<void*>(&BeginStateBlockHook),
        record->beginStateBlock, ClaimSlot(*record, Claim::BeginStateBlock));
    const bool endStateBlock = InstallSlot(
        policy.stateBlocks, "d3d9-device:end-state-block",
        table, 61u, reinterpret_cast<void*>(&EndStateBlockHook),
        record->endStateBlock, ClaimSlot(*record, Claim::EndStateBlock));
    ReleaseSRWLockExclusive(&g_vtableLock);

    if (policy.stateBlocks &&
        (!createStateBlock || !beginStateBlock || !endStateBlock)) {
        redundant_state_filter::Configure(false);
    }
}

void Reactivate(void** table) {
    if (!table) return;
    std::array<patch::registry::ClaimId, kClaimCount> claims{};
    AcquireSRWLockShared(&g_vtableLock);
    for (const auto& record : g_vtables) {
        if (record.vtable == table) {
            claims = record.claims;
            break;
        }
    }
    ReleaseSRWLockShared(&g_vtableLock);
    bool reactivated = true;
    for (const patch::registry::ClaimId claim : claims) {
        if (claim && !patch::registry::ReactivateVtableClaim(claim)) {
            reactivated = false;
        }
    }
    if (!reactivated) {
        redundant_state_filter::Configure(false);
        LogError("XIII-2 D3D9 state-hook reactivation failed: table=%p redundant-state-filter=disabled",
                 table);
    }
}

void MarkDormant(void** table) {
    if (!table) return;
    std::array<patch::registry::ClaimId, kClaimCount> claims{};
    AcquireSRWLockShared(&g_vtableLock);
    for (const auto& record : g_vtables) {
        if (record.vtable == table) {
            claims = record.claims;
            break;
        }
    }
    ReleaseSRWLockShared(&g_vtableLock);
    for (const patch::registry::ClaimId claim : claims) {
        if (claim) patch::registry::MarkDormant(claim);
    }
}

} // namespace novafix::d3d9::device_state_hooks
