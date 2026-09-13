#pragma once

#include <cstddef>
#include <cstdint>

namespace novafix::d3d9 {

struct ShaderBytecodeIdentity {
    std::size_t bytes{};
    std::uint64_t hash{};

    explicit operator bool() const {
        return bytes != 0;
    }
};

// Reads a Shader Model 2/3 token stream through its END token. The caller
// supplies a hard upper bound because CreatePixelShader does not carry a size
ShaderBytecodeIdentity InspectShaderBytecode(
    const std::uint32_t* bytecode, std::size_t maximumDwords = 16384u);

} // namespace novafix::d3d9
