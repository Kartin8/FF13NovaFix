#include "d3d9/swap_chain_proxy.h"

#include "diagnostics/hook_profiler.h"
#include "diagnostics/log.h"
#include "d3d9/mip_lod_bias_controller.h"
#include "d3d9/presentation_router.h"
#include "game/shared/timing/cutscene_terminal_frame.h"

#include <d3d9.h>

#include <algorithm>
#include <atomic>
#include <new>
#include <vector>

namespace novafix::d3d9::swap_chain {
namespace {

class Proxy;

struct CacheEntry {
    IDirect3DSwapChain9* real{};
    Proxy* proxy{};
};

std::vector<CacheEntry> g_cache;
SRWLOCK g_cacheLock = SRWLOCK_INIT;

class Proxy final : public IDirect3DSwapChain9Ex {
public:
    Proxy(IDirect3DSwapChain9* real, IDirect3DDevice9* device, HWND window,
          bool extended)
        : real_(real), device_(device), window_(window) {
        if (extended) {
            void* interfacePointer = nullptr;
            if (SUCCEEDED(real_->QueryInterface(
                    IID_IDirect3DSwapChain9Ex, &interfacePointer))) {
                realEx_ = static_cast<IDirect3DSwapChain9Ex*>(interfacePointer);
            }
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) noexcept override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IDirect3DSwapChain9)) {
            *output = static_cast<IDirect3DSwapChain9*>(this);
            AddRef();
            return S_OK;
        }
        if (IsEqualIID(iid, IID_IDirect3DSwapChain9Ex)) {
            if (!realEx_) return E_NOINTERFACE;
            *output = static_cast<IDirect3DSwapChain9Ex*>(this);
            AddRef();
            return S_OK;
        }
        return real_->QueryInterface(iid, output);
    }

    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++references_; }

    ULONG STDMETHODCALLTYPE Release() noexcept override {
        AcquireSRWLockExclusive(&g_cacheLock);
        const ULONG remaining = --references_;
        if (!remaining) {
            const auto entry = std::find_if(
                g_cache.begin(), g_cache.end(),
                [this](const CacheEntry& candidate) {
                    return candidate.proxy == this;
                });
            if (entry != g_cache.end()) {
                g_cache.erase(entry);
            }
        }
        ReleaseSRWLockExclusive(&g_cacheLock);
        if (!remaining) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE Present(const RECT* source, const RECT* destination, HWND overrideWindow,
                                      const RGNDATA* dirty, DWORD flags) noexcept override {
        static const auto totalPoint = hook_profiler::RegisterDynamicPoint(
            "hook.d3d9.swap-chain-present-total");
        static const auto originalPoint = hook_profiler::RegisterDynamicPoint(
            "hook.d3d9.swap-chain-present-original");
        hook_profiler::Scope totalTiming(totalPoint);
        const bool owner = presentation::Begin(device_, window_, presentation::Path::SwapChain);
        if (owner) mip_lod_bias::ApplyPendingState(device_);
        const bool held = owner &&
            game::cutscene_terminal_frame::ShouldSuppressPresent();
        HRESULT result = D3D_OK;
        if (!held) {
            hook_profiler::Scope originalTiming(originalPoint);
            result = real_->Present(
                source, destination, overrideWindow, dirty, flags);
        }
        if (owner) presentation::End(result);
        if (!hasPresentResult_ || result != lastPresentResult_) {
            Log("Swap-chain Present result changed: proxy=%p result=0x%08lX", this,
                static_cast<unsigned long>(result));
            lastPresentResult_ = result;
            hasPresentResult_ = true;
        }
        return result;
    }

    HRESULT STDMETHODCALLTYPE GetFrontBufferData(IDirect3DSurface9* destination) noexcept override {
        return real_->GetFrontBufferData(destination);
    }
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT index, D3DBACKBUFFER_TYPE type,
                                            IDirect3DSurface9** output) noexcept override {
        return real_->GetBackBuffer(index, type, output);
    }
    HRESULT STDMETHODCALLTYPE GetRasterStatus(D3DRASTER_STATUS* status) noexcept override {
        return real_->GetRasterStatus(status);
    }
    HRESULT STDMETHODCALLTYPE GetDisplayMode(D3DDISPLAYMODE* mode) noexcept override {
        return real_->GetDisplayMode(mode);
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** output) noexcept override {
        return real_->GetDevice(output);
    }
    HRESULT STDMETHODCALLTYPE GetPresentParameters(D3DPRESENT_PARAMETERS* parameters) noexcept override {
        return real_->GetPresentParameters(parameters);
    }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* count) noexcept override {
        return realEx_ ? realEx_->GetLastPresentCount(count) : D3DERR_INVALIDCALL;
    }
    HRESULT STDMETHODCALLTYPE GetPresentStats(D3DPRESENTSTATS* statistics) noexcept override {
        return realEx_ ? realEx_->GetPresentStats(statistics) : D3DERR_INVALIDCALL;
    }
    HRESULT STDMETHODCALLTYPE GetDisplayModeEx(
        D3DDISPLAYMODEEX* mode, D3DDISPLAYROTATION* rotation) noexcept override {
        return realEx_
            ? realEx_->GetDisplayModeEx(mode, rotation)
            : D3DERR_INVALIDCALL;
    }
private:
    ~Proxy() {
        if (realEx_) realEx_->Release();
        real_->Release();
    }

    std::atomic<ULONG> references_{1};
    IDirect3DSwapChain9* real_{};
    IDirect3DSwapChain9Ex* realEx_{};
    IDirect3DDevice9* device_{};
    HWND window_{};
    HRESULT lastPresentResult_{};
    bool hasPresentResult_{};
};

} // namespace

HRESULT Wrap(HRESULT result, IDirect3DSwapChain9** output,
             IDirect3DDevice9* device, HWND window, bool extended) {
    if (FAILED(result) || !output || !*output) return result;
    IDirect3DSwapChain9* real = *output;

    AcquireSRWLockExclusive(&g_cacheLock);
    for (auto& entry : g_cache) {
        if (entry.real == real && entry.proxy) {
            entry.proxy->AddRef();
            *output = entry.proxy;
            ReleaseSRWLockExclusive(&g_cacheLock);
            real->Release();
            return result;
        }
    }

    auto* proxy = new (std::nothrow) Proxy(real, device, window, extended);
    if (!proxy) {
        ReleaseSRWLockExclusive(&g_cacheLock);
        return result;
    }
    g_cache.push_back({real, proxy});
    *output = proxy;
    ReleaseSRWLockExclusive(&g_cacheLock);
    return result;
}

} // namespace novafix::d3d9::swap_chain
