#include "d3d9/redundant_state_filter.h"

#include "diagnostics/log.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace novafix::d3d9::redundant_state_filter {
namespace {

constexpr std::size_t kSamplerCount = 16u;
constexpr std::size_t kSamplerStateCount = 14u;
constexpr std::size_t kRenderStateCount = 256u;
constexpr std::size_t kTextureStageCount = 16u;
constexpr std::size_t kTextureStageStateCount = 33u;
constexpr std::size_t kPixelConstantRegisterCount = 224u;
constexpr std::size_t kVertexConstantRegisterCount = 256u;
constexpr std::size_t kStreamCount = 16u;
constexpr unsigned kSummaryFrameCount = 1800u;

struct StreamState {
    IDirect3DVertexBuffer9* buffer{};
    UINT offset{};
    UINT stride{};
};

struct SkipCounts {
    std::uint64_t sampler{};
    std::uint64_t render{};
    std::uint64_t texture{};
    std::uint64_t textureStage{};
    std::uint64_t pixelConstants{};
    std::uint64_t pixelShader{};
    std::uint64_t vertexConstants{};
    std::uint64_t vertexShader{};
    std::uint64_t stream{};
};

struct StateCache {
    IDirect3DDevice9* device{};
    std::uint64_t generation{};
    bool recordingStateBlock{};
    std::array<std::array<DWORD, kSamplerStateCount>, kSamplerCount>
        samplers{};
    std::array<std::array<bool, kSamplerStateCount>, kSamplerCount>
        samplerValid{};
    std::array<DWORD, kRenderStateCount> renderStates{};
    std::array<bool, kRenderStateCount> renderStateValid{};
    std::array<IDirect3DBaseTexture9*, kTextureStageCount> textures{};
    std::array<bool, kTextureStageCount> textureValid{};
    std::array<std::array<DWORD, kTextureStageStateCount>,
               kTextureStageCount> textureStageStates{};
    std::array<std::array<bool, kTextureStageStateCount>,
               kTextureStageCount> textureStageStateValid{};
    std::array<float, kPixelConstantRegisterCount * 4u> pixelConstants{};
    std::array<bool, kPixelConstantRegisterCount> pixelConstantValid{};
    IDirect3DPixelShader9* pixelShader{};
    bool pixelShaderValid{};
    std::array<float, kVertexConstantRegisterCount * 4u> vertexConstants{};
    std::array<bool, kVertexConstantRegisterCount> vertexConstantValid{};
    IDirect3DVertexShader9* vertexShader{};
    bool vertexShaderValid{};
    std::array<StreamState, kStreamCount> streams{};
    std::array<bool, kStreamCount> streamValid{};
    SkipCounts skipped{};
    unsigned frames{};
};

std::atomic_bool g_enabled{false};
std::atomic<IDirect3DDevice9*> g_device{nullptr};
std::atomic<DWORD> g_renderThread{};
std::atomic_uint64_t g_generation{1u};
thread_local StateCache g_cache{};

void ResetTrackedState(StateCache& cache) {
    for (auto& valid : cache.samplerValid) valid.fill(false);
    cache.renderStateValid.fill(false);
    cache.textureValid.fill(false);
    for (auto& valid : cache.textureStageStateValid) valid.fill(false);
    cache.pixelConstantValid.fill(false);
    cache.pixelShaderValid = false;
    cache.vertexConstantValid.fill(false);
    cache.vertexShaderValid = false;
    cache.streamValid.fill(false);
    cache.recordingStateBlock = false;
}

StateCache* ActiveCache(IDirect3DDevice9* device) {
    if (!device || !g_enabled.load(std::memory_order_relaxed) ||
        g_device.load(std::memory_order_acquire) != device ||
        g_renderThread.load(std::memory_order_acquire) !=
            GetCurrentThreadId()) {
        return nullptr;
    }
    const std::uint64_t generation =
        g_generation.load(std::memory_order_acquire);
    if (g_cache.device != device || g_cache.generation != generation) {
        g_cache.device = device;
        g_cache.generation = generation;
        ResetTrackedState(g_cache);
    }
    return &g_cache;
}

void InvalidateForeignMutation(IDirect3DDevice9* device) {
    if (device && g_device.load(std::memory_order_acquire) == device &&
        g_renderThread.load(std::memory_order_acquire) != 0u) {
        g_generation.fetch_add(1u, std::memory_order_acq_rel);
    }
}

template <std::size_t RegisterCount>
bool ConstantsMatch(const std::array<float, RegisterCount * 4u>& stored,
                    const std::array<bool, RegisterCount>& valid,
                    UINT startRegister, const float* values,
                    UINT vectorCount) {
    if (!values || vectorCount == 0u || startRegister >= RegisterCount ||
        vectorCount > RegisterCount - startRegister) {
        return false;
    }
    for (UINT index = 0u; index < vectorCount; ++index) {
        if (!valid[startRegister + index]) return false;
    }
    return std::memcmp(
               stored.data() + static_cast<std::size_t>(startRegister) * 4u,
               values, static_cast<std::size_t>(vectorCount) * 4u *
                           sizeof(float)) == 0;
}

template <std::size_t RegisterCount>
void CommitConstants(std::array<float, RegisterCount * 4u>& stored,
                     std::array<bool, RegisterCount>& valid,
                     UINT startRegister, const float* values,
                     UINT vectorCount) {
    if (!values || vectorCount == 0u || startRegister >= RegisterCount ||
        vectorCount > RegisterCount - startRegister) {
        return;
    }
    std::memcpy(
        stored.data() + static_cast<std::size_t>(startRegister) * 4u, values,
        static_cast<std::size_t>(vectorCount) * 4u * sizeof(float));
    std::fill_n(valid.data() + startRegister, vectorCount, true);
}

void LogSummary(StateCache& cache) {
    if (cache.frames < kSummaryFrameCount) return;
    const SkipCounts& value = cache.skipped;
    Log("XIII-2 redundant D3D9 state filter: last %u frames skipped sampler=%llu render=%llu texture=%llu texture-stage=%llu pixel-constants=%llu pixel-shader=%llu vertex-constants=%llu vertex-shader=%llu stream=%llu",
        cache.frames, static_cast<unsigned long long>(value.sampler),
        static_cast<unsigned long long>(value.render),
        static_cast<unsigned long long>(value.texture),
        static_cast<unsigned long long>(value.textureStage),
        static_cast<unsigned long long>(value.pixelConstants),
        static_cast<unsigned long long>(value.pixelShader),
        static_cast<unsigned long long>(value.vertexConstants),
        static_cast<unsigned long long>(value.vertexShader),
        static_cast<unsigned long long>(value.stream));
    cache.skipped = {};
    cache.frames = 0u;
}

} // namespace

void Configure(bool enabled) {
    const bool previous =
        g_enabled.exchange(enabled, std::memory_order_acq_rel);
    if (!enabled) Invalidate();
    if (previous != enabled) {
        Log("XIII-2 redundant D3D9 state filter configured=%d", enabled);
    }
}

void BeginFrame(IDirect3DDevice9* device) {
    if (!device || !g_enabled.load(std::memory_order_relaxed)) return;
    const DWORD thread = GetCurrentThreadId();
    const bool changed =
        g_device.load(std::memory_order_acquire) != device ||
        g_renderThread.load(std::memory_order_acquire) != thread;
    if (changed) {
        g_device.store(device, std::memory_order_release);
        g_renderThread.store(thread, std::memory_order_release);
        g_generation.fetch_add(1u, std::memory_order_acq_rel);
    }
    StateCache* cache = ActiveCache(device);
    if (!cache) return;
    ++cache->frames;
    LogSummary(*cache);
}

void Invalidate(IDirect3DDevice9* device) {
    if (!device || g_device.load(std::memory_order_acquire) == device) {
        g_generation.fetch_add(1u, std::memory_order_acq_rel);
    }
}

void BeginStateBlock(IDirect3DDevice9* device, bool succeeded) {
    if (!succeeded) return;
    StateCache* cache = ActiveCache(device);
    if (cache) {
        cache->recordingStateBlock = true;
    } else {
        InvalidateForeignMutation(device);
    }
}

void EndStateBlock(IDirect3DDevice9* device, bool succeeded) {
    StateCache* cache = ActiveCache(device);
    if (cache) cache->recordingStateBlock = false;
    if (succeeded) Invalidate(device);
}

bool SamplerState(IDirect3DDevice9* device, DWORD sampler,
                  D3DSAMPLERSTATETYPE type, DWORD value) {
    StateCache* cache = ActiveCache(device);
    const std::size_t state = static_cast<std::size_t>(type);
    if (!cache || cache->recordingStateBlock || sampler >= kSamplerCount ||
        state >= kSamplerStateCount || !cache->samplerValid[sampler][state] ||
        cache->samplers[sampler][state] != value) {
        return false;
    }
    ++cache->skipped.sampler;
    return true;
}

void CommitSamplerState(IDirect3DDevice9* device, DWORD sampler,
                        D3DSAMPLERSTATETYPE type, DWORD value) {
    StateCache* cache = ActiveCache(device);
    const std::size_t state = static_cast<std::size_t>(type);
    if (!cache) return InvalidateForeignMutation(device);
    if (cache->recordingStateBlock || sampler >= kSamplerCount ||
        state >= kSamplerStateCount) return;
    cache->samplers[sampler][state] = value;
    cache->samplerValid[sampler][state] = true;
}

bool RenderState(IDirect3DDevice9* device, D3DRENDERSTATETYPE state,
                 DWORD value) {
    StateCache* cache = ActiveCache(device);
    const std::size_t index = static_cast<std::size_t>(state);
    if (!cache || cache->recordingStateBlock || index >= kRenderStateCount ||
        !cache->renderStateValid[index] ||
        cache->renderStates[index] != value) return false;
    ++cache->skipped.render;
    return true;
}

void CommitRenderState(IDirect3DDevice9* device, D3DRENDERSTATETYPE state,
                       DWORD value) {
    StateCache* cache = ActiveCache(device);
    const std::size_t index = static_cast<std::size_t>(state);
    if (!cache) return InvalidateForeignMutation(device);
    if (cache->recordingStateBlock || index >= kRenderStateCount) return;
    cache->renderStates[index] = value;
    cache->renderStateValid[index] = true;
}

bool Texture(IDirect3DDevice9* device, DWORD stage,
             IDirect3DBaseTexture9* texture) {
    StateCache* cache = ActiveCache(device);
    if (!cache || cache->recordingStateBlock || stage >= kTextureStageCount ||
        !cache->textureValid[stage] || cache->textures[stage] != texture) {
        return false;
    }
    ++cache->skipped.texture;
    return true;
}

void CommitTexture(IDirect3DDevice9* device, DWORD stage,
                   IDirect3DBaseTexture9* texture) {
    StateCache* cache = ActiveCache(device);
    if (!cache) return InvalidateForeignMutation(device);
    if (cache->recordingStateBlock || stage >= kTextureStageCount) return;
    cache->textures[stage] = texture;
    cache->textureValid[stage] = true;
}

bool TextureStageState(IDirect3DDevice9* device, DWORD stage,
                       D3DTEXTURESTAGESTATETYPE type, DWORD value) {
    StateCache* cache = ActiveCache(device);
    const std::size_t state = static_cast<std::size_t>(type);
    if (!cache || cache->recordingStateBlock || stage >= kTextureStageCount ||
        state >= kTextureStageStateCount ||
        !cache->textureStageStateValid[stage][state] ||
        cache->textureStageStates[stage][state] != value) return false;
    ++cache->skipped.textureStage;
    return true;
}

void CommitTextureStageState(IDirect3DDevice9* device, DWORD stage,
                             D3DTEXTURESTAGESTATETYPE type, DWORD value) {
    StateCache* cache = ActiveCache(device);
    const std::size_t state = static_cast<std::size_t>(type);
    if (!cache) return InvalidateForeignMutation(device);
    if (cache->recordingStateBlock || stage >= kTextureStageCount ||
        state >= kTextureStageStateCount) return;
    cache->textureStageStates[stage][state] = value;
    cache->textureStageStateValid[stage][state] = true;
}

bool PixelShaderConstantF(IDirect3DDevice9* device, UINT startRegister,
                          const float* values, UINT vectorCount) {
    StateCache* cache = ActiveCache(device);
    if (!cache || cache->recordingStateBlock ||
        !ConstantsMatch(cache->pixelConstants, cache->pixelConstantValid,
                        startRegister, values, vectorCount)) return false;
    ++cache->skipped.pixelConstants;
    return true;
}

void CommitPixelShaderConstantF(IDirect3DDevice9* device, UINT startRegister,
                                const float* values, UINT vectorCount) {
    StateCache* cache = ActiveCache(device);
    if (!cache) return InvalidateForeignMutation(device);
    if (cache->recordingStateBlock) return;
    CommitConstants(cache->pixelConstants, cache->pixelConstantValid,
                    startRegister, values, vectorCount);
}

bool PixelShader(IDirect3DDevice9* device, IDirect3DPixelShader9* shader) {
    StateCache* cache = ActiveCache(device);
    if (!cache || cache->recordingStateBlock || !cache->pixelShaderValid ||
        cache->pixelShader != shader) return false;
    ++cache->skipped.pixelShader;
    return true;
}

void CommitPixelShader(IDirect3DDevice9* device,
                       IDirect3DPixelShader9* shader) {
    StateCache* cache = ActiveCache(device);
    if (!cache) return InvalidateForeignMutation(device);
    if (cache->recordingStateBlock) return;
    cache->pixelShader = shader;
    cache->pixelShaderValid = true;
}

bool VertexShaderConstantF(IDirect3DDevice9* device, UINT startRegister,
                           const float* values, UINT vectorCount) {
    StateCache* cache = ActiveCache(device);
    if (!cache || cache->recordingStateBlock ||
        !ConstantsMatch(cache->vertexConstants, cache->vertexConstantValid,
                        startRegister, values, vectorCount)) return false;
    ++cache->skipped.vertexConstants;
    return true;
}

void CommitVertexShaderConstantF(IDirect3DDevice9* device,
                                 UINT startRegister, const float* values,
                                 UINT vectorCount) {
    StateCache* cache = ActiveCache(device);
    if (!cache) return InvalidateForeignMutation(device);
    if (cache->recordingStateBlock) return;
    CommitConstants(cache->vertexConstants, cache->vertexConstantValid,
                    startRegister, values, vectorCount);
}

bool VertexShader(IDirect3DDevice9* device, IDirect3DVertexShader9* shader) {
    StateCache* cache = ActiveCache(device);
    if (!cache || cache->recordingStateBlock || !cache->vertexShaderValid ||
        cache->vertexShader != shader) return false;
    ++cache->skipped.vertexShader;
    return true;
}

void CommitVertexShader(IDirect3DDevice9* device,
                        IDirect3DVertexShader9* shader) {
    StateCache* cache = ActiveCache(device);
    if (!cache) return InvalidateForeignMutation(device);
    if (cache->recordingStateBlock) return;
    cache->vertexShader = shader;
    cache->vertexShaderValid = true;
}

bool StreamSource(IDirect3DDevice9* device, UINT stream,
                  IDirect3DVertexBuffer9* buffer, UINT offset, UINT stride) {
    StateCache* cache = ActiveCache(device);
    if (!cache || cache->recordingStateBlock || stream == 0u ||
        stream >= kStreamCount || !cache->streamValid[stream]) return false;
    const StreamState& previous = cache->streams[stream];
    if (previous.buffer != buffer || previous.offset != offset ||
        previous.stride != stride) return false;
    ++cache->skipped.stream;
    return true;
}

void CommitStreamSource(IDirect3DDevice9* device, UINT stream,
                        IDirect3DVertexBuffer9* buffer, UINT offset,
                        UINT stride) {
    StateCache* cache = ActiveCache(device);
    if (!cache) return InvalidateForeignMutation(device);
    if (cache->recordingStateBlock || stream == 0u ||
        stream >= kStreamCount) return;
    cache->streams[stream] = {buffer, offset, stride};
    cache->streamValid[stream] = true;
}

} // namespace novafix::d3d9::redundant_state_filter
