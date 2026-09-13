#pragma once

#include <d3d9.h>

namespace novafix::d3d9::redundant_state_filter {

void Configure(bool enabled);
void BeginFrame(IDirect3DDevice9* device);
void Invalidate(IDirect3DDevice9* device = nullptr);
void BeginStateBlock(IDirect3DDevice9* device, bool succeeded);
void EndStateBlock(IDirect3DDevice9* device, bool succeeded);

bool SamplerState(IDirect3DDevice9* device, DWORD sampler,
                  D3DSAMPLERSTATETYPE type, DWORD value);
void CommitSamplerState(IDirect3DDevice9* device, DWORD sampler,
                        D3DSAMPLERSTATETYPE type, DWORD value);

bool RenderState(IDirect3DDevice9* device, D3DRENDERSTATETYPE state,
                 DWORD value);
void CommitRenderState(IDirect3DDevice9* device, D3DRENDERSTATETYPE state,
                       DWORD value);

bool Texture(IDirect3DDevice9* device, DWORD stage,
             IDirect3DBaseTexture9* texture);
void CommitTexture(IDirect3DDevice9* device, DWORD stage,
                   IDirect3DBaseTexture9* texture);

bool TextureStageState(IDirect3DDevice9* device, DWORD stage,
                       D3DTEXTURESTAGESTATETYPE type, DWORD value);
void CommitTextureStageState(IDirect3DDevice9* device, DWORD stage,
                             D3DTEXTURESTAGESTATETYPE type, DWORD value);

bool PixelShaderConstantF(IDirect3DDevice9* device, UINT startRegister,
                          const float* values, UINT vectorCount);
void CommitPixelShaderConstantF(IDirect3DDevice9* device, UINT startRegister,
                                const float* values, UINT vectorCount);

bool PixelShader(IDirect3DDevice9* device, IDirect3DPixelShader9* shader);
void CommitPixelShader(IDirect3DDevice9* device,
                       IDirect3DPixelShader9* shader);

bool VertexShaderConstantF(IDirect3DDevice9* device, UINT startRegister,
                           const float* values, UINT vectorCount);
void CommitVertexShaderConstantF(IDirect3DDevice9* device,
                                 UINT startRegister, const float* values,
                                 UINT vectorCount);

bool VertexShader(IDirect3DDevice9* device, IDirect3DVertexShader9* shader);
void CommitVertexShader(IDirect3DDevice9* device,
                        IDirect3DVertexShader9* shader);

bool StreamSource(IDirect3DDevice9* device, UINT stream,
                  IDirect3DVertexBuffer9* buffer, UINT offset, UINT stride);
void CommitStreamSource(IDirect3DDevice9* device, UINT stream,
                        IDirect3DVertexBuffer9* buffer, UINT offset,
                        UINT stride);

} // namespace novafix::d3d9::redundant_state_filter
