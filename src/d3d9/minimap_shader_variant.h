#pragma once

#include <d3d9.h>

namespace novafix::d3d9::minimap_shader_variant {

bool MatchesNativeVertexShader(const DWORD* bytecode);
bool Available();
const DWORD* CorrectedBytecode();
void Record(IDirect3DVertexShader9* original,
            IDirect3DVertexShader9* corrected);
IDirect3DVertexShader9* Resolve(IDirect3DVertexShader9* original);

} // namespace novafix::d3d9::minimap_shader_variant
