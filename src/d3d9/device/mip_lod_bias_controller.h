#pragma once

#include <cstdint>

struct IDirect3DDevice9;

namespace novafix::d3d9::mip_lod_bias {

void Configure(bool enabled, bool clampXiii2MipBias,
               unsigned anisotropy, float minimumBias);
void Preview(unsigned anisotropy, float minimumBias);
std::uint32_t AdjustSamplerState(std::uint32_t sampler, std::uint32_t type,
                                 std::uint32_t value);
void ApplyPendingState(IDirect3DDevice9* device);
void ResetSamplerCache();

} // namespace novafix::d3d9::mip_lod_bias
