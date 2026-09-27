#pragma once

#include <d3d9.h>

#include <cstdint>

namespace novafix::d3d9 {

enum class ShaderKind : std::uint8_t {
    Unknown,
    AlphaShadow,
    Count,
};

} // namespace novafix::d3d9

namespace novafix::d3d9::shader_corrections {

struct Resolution {
    const DWORD* bytecode{};
    std::uint64_t originalHash{};
    unsigned originalBytes{};
    ShaderKind kind{ShaderKind::Unknown};
    const char* name{};
    bool replaced{};
};

enum class OpaqueShadowMode : std::uint32_t {
    Native,
    FixedFunctionDepth,
};

// Resolve Crystal Tools bytecode before shader creation.
// This avoids compiling both versions and keeps replacement work out of SetPixelShader.
Resolution ResolveBytecode(const DWORD* original);
void RecordCreation(const Resolution& resolution,
                    IDirect3DPixelShader9* created, HRESULT result);
bool NeedsOpaqueShadowVariant(const Resolution& resolution);
const DWORD* OpaqueShadowBytecode(OpaqueShadowMode mode);
void RecordOpaqueShadowVariant(
    IDirect3DPixelShader9* original, OpaqueShadowMode mode,
    IDirect3DPixelShader9* variant, HRESULT result);
IDirect3DPixelShader9* ResolveOpaqueShadowMode(
    IDirect3DPixelShader9* original);
void SetOpaqueShadowMode(OpaqueShadowMode mode);
OpaqueShadowMode GetOpaqueShadowMode();
bool OpaqueShadowModeAvailable(OpaqueShadowMode mode);
bool OpaqueShadowModeRejected(OpaqueShadowMode mode);
void ObserveOpaqueShadowConstants(std::uint32_t startRegister,
                                  const float* values,
                                  std::uint32_t vectorCount);
std::uint32_t OpaqueShadowBiasBits();

} // namespace novafix::d3d9::shader_corrections
