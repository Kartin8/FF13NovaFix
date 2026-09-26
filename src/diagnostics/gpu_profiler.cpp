#include "diagnostics/gpu_profiler.h"

#include "diagnostics/log.h"

#include <d3d9.h>

#include <array>
#include <cstdint>
#include <mutex>

namespace novafix::gpu_profiler {
namespace {

constexpr std::size_t kSlotCount = 4u;
constexpr ULONGLONG kReportIntervalMs = 10'000u;

struct Slot {
    IDirect3DQuery9* begin{};
    IDirect3DQuery9* end{};
    IDirect3DQuery9* frequency{};
    IDirect3DQuery9* disjoint{};
    bool pending{};
};

struct Pass {
    const char* name{};
    std::uint32_t sampleInterval{};
    std::array<Slot, kSlotCount> slots{};
    Slot* active{};
    std::uint32_t calls{};
    std::uint64_t samples{};
    double totalMs{};
    double maxMs{};
    DWORD ownerThread{};
};

Pass g_overlay{"overlay", 32u};
Pass g_shadowClear{"shadow-clear", 256u};
IDirect3DDevice9* g_device{};
ULONGLONG g_lastReport{};
bool g_unsupportedLogged{};
std::recursive_mutex g_lock;

void Release(Slot& slot) {
    if (slot.begin) slot.begin->Release();
    if (slot.end) slot.end->Release();
    if (slot.frequency) slot.frequency->Release();
    if (slot.disjoint) slot.disjoint->Release();
    slot = {};
}

bool Create(IDirect3DDevice9* device, Slot& slot) {
    if (SUCCEEDED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &slot.begin)) &&
        SUCCEEDED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &slot.end)) &&
        SUCCEEDED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ,
                                      &slot.frequency)) &&
        SUCCEEDED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT,
                                      &slot.disjoint))) {
        return true;
    }
    Release(slot);
    return false;
}

template <typename T>
bool Ready(IDirect3DQuery9* query, T& value) {
    return query && query->GetData(&value, sizeof(value), 0u) == S_OK;
}

void ReportIfDue() {
    const ULONGLONG now = GetTickCount64();
    if (!g_lastReport) g_lastReport = now;
    if (now - g_lastReport < kReportIntervalMs) return;
    for (Pass* pass : {&g_overlay, &g_shadowClear}) {
        if (pass->samples) {
            LogDebug("NovaFix GPU: pass=%s samples=%llu avg=%.3fms max=%.3fms measure=submitted-commands-not-net-delta",
                     pass->name,
                     static_cast<unsigned long long>(pass->samples),
                     pass->totalMs / static_cast<double>(pass->samples),
                     pass->maxMs);
        }
        pass->samples = 0u;
        pass->totalMs = 0.0;
        pass->maxMs = 0.0;
    }
    g_lastReport = now;
}

void Begin(IDirect3DDevice9* device, Pass& pass) {
    if (!IsLogEnabled(LogLevel::Debug) || !device || pass.active) return;
    if (g_device && g_device != device) Reset(g_device);
    g_device = device;
    if ((pass.calls++ & (pass.sampleInterval - 1u)) != 0u) return;
    for (Slot& slot : pass.slots) {
        if (slot.pending) continue;
        if (!slot.begin && !Create(device, slot)) {
            if (!g_unsupportedLogged) {
                LogDebug("NovaFix GPU: D3D9 timestamp queries unavailable; GPU cost not measured");
                g_unsupportedLogged = true;
            }
            return;
        }
        if (FAILED(slot.disjoint->Issue(D3DISSUE_BEGIN)) ||
            FAILED(slot.begin->Issue(D3DISSUE_END))) return;
        pass.active = &slot;
        pass.ownerThread = GetCurrentThreadId();
        return;
    }
}

void End(IDirect3DDevice9* device, Pass& pass) {
    if (pass.ownerThread != GetCurrentThreadId()) return;
    Slot* slot = pass.active;
    pass.active = nullptr;
    pass.ownerThread = 0u;
    if (!slot || device != g_device) return;
    if (SUCCEEDED(slot->end->Issue(D3DISSUE_END)) &&
        SUCCEEDED(slot->disjoint->Issue(D3DISSUE_END)) &&
        SUCCEEDED(slot->frequency->Issue(D3DISSUE_END))) {
        slot->pending = true;
    }
}

} // namespace

void Reset(IDirect3DDevice9* device) {
    std::scoped_lock lock(g_lock);
    if (device && device != g_device) return;
    for (Pass* pass : {&g_overlay, &g_shadowClear}) {
        pass->active = nullptr;
        pass->ownerThread = 0u;
        for (Slot& slot : pass->slots) Release(slot);
        pass->calls = 0u;
        pass->samples = 0u;
        pass->totalMs = 0.0;
        pass->maxMs = 0.0;
    }
    g_device = nullptr;
    g_lastReport = 0u;
}

void Poll(IDirect3DDevice9* device) {
    if (!IsLogEnabled(LogLevel::Debug) || !device) return;
    std::scoped_lock lock(g_lock);
    if (g_device && g_device != device) Reset(g_device);
    for (Pass* pass : {&g_overlay, &g_shadowClear}) {
        for (Slot& slot : pass->slots) {
            if (!slot.pending) continue;
            UINT64 begin{}, end{}, frequency{};
            BOOL disjoint{};
            if (!Ready(slot.begin, begin) || !Ready(slot.end, end) ||
                !Ready(slot.frequency, frequency) ||
                !Ready(slot.disjoint, disjoint)) continue;
            slot.pending = false;
            if (disjoint || !frequency || end < begin) continue;
            const double milliseconds = static_cast<double>(end - begin) *
                1000.0 / static_cast<double>(frequency);
            ++pass->samples;
            pass->totalMs += milliseconds;
            if (milliseconds > pass->maxMs) pass->maxMs = milliseconds;
        }
    }
    ReportIfDue();
}

void BeginOverlay(IDirect3DDevice9* device) {
    if (!IsLogEnabled(LogLevel::Debug)) return;
    std::scoped_lock lock(g_lock);
    Begin(device, g_overlay);
}

void EndOverlay(IDirect3DDevice9* device) {
    if (!IsLogEnabled(LogLevel::Debug)) return;
    std::scoped_lock lock(g_lock);
    End(device, g_overlay);
}

void BeginShadowClear(IDirect3DDevice9* device) {
    if (!IsLogEnabled(LogLevel::Debug)) return;
    std::scoped_lock lock(g_lock);
    Begin(device, g_shadowClear);
}

void EndShadowClear(IDirect3DDevice9* device) {
    if (!IsLogEnabled(LogLevel::Debug)) return;
    std::scoped_lock lock(g_lock);
    End(device, g_shadowClear);
}

} // namespace novafix::gpu_profiler
