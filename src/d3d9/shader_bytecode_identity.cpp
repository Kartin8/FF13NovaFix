#include "d3d9/shader_bytecode_identity.h"

#include <d3d9.h>

namespace novafix::d3d9 {
namespace {

std::uint64_t HashBytes(const void* data, std::size_t size) {
    constexpr std::uint64_t offset = 14695981039346656037ull;
    constexpr std::uint64_t prime = 1099511628211ull;
    std::uint64_t result = offset;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        result ^= bytes[index];
        result *= prime;
    }
    return result;
}

} // namespace

ShaderBytecodeIdentity InspectShaderBytecode(
    const std::uint32_t* bytecode, std::size_t maximumDwords) {
    if (!bytecode || maximumDwords < 2u) return {};

    const std::uint32_t version = bytecode[0];
    const bool pixelShader = (version & 0xFFFF0000u) == 0xFFFF0000u;
    const unsigned major = D3DSHADER_VERSION_MAJOR(version);
    if (!pixelShader || major < 2u || major > 3u) return {};

    std::size_t cursor = 1u;
    while (cursor < maximumDwords) {
        const std::uint32_t token = bytecode[cursor];
        const auto opcode = static_cast<D3DSHADER_INSTRUCTION_OPCODE_TYPE>(
            token & D3DSI_OPCODE_MASK);
        if (opcode == D3DSIO_END) {
            const std::size_t bytes = (cursor + 1u) * sizeof(std::uint32_t);
            return {bytes, HashBytes(bytecode, bytes)};
        }

        std::size_t operands = 0u;
        if (opcode == D3DSIO_COMMENT) {
            operands = (token & D3DSI_COMMENTSIZE_MASK) >>
                       D3DSI_COMMENTSIZE_SHIFT;
        } else {
            operands = (token & D3DSI_INSTLENGTH_MASK) >>
                       D3DSI_INSTLENGTH_SHIFT;
        }
        if (operands > maximumDwords - cursor - 1u) return {};
        cursor += operands + 1u;
    }
    return {};
}

} // namespace novafix::d3d9
