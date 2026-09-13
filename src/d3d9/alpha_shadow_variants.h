#pragma once

#include "d3d9/shader_corrections.h"

#include <d3d9.h>

namespace novafix::d3d9::alpha_shadow_variants {

bool NeedsVariant(const shader_corrections::Resolution& resolution);
const DWORD* FixedDepthBytecode();
void RecordVariant(
    IDirect3DPixelShader9* original, IDirect3DPixelShader9* fixedDepth,
    HRESULT result);
IDirect3DPixelShader9* ResolveFixedDepth(
    IDirect3DPixelShader9* original, bool enabled);
void SetFixedDepthEnabled(bool enabled);
bool FixedDepthEnabled();

} // namespace novafix::d3d9::alpha_shadow_variants
