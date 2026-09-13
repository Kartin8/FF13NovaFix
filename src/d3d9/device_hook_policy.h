#pragma once

namespace novafix::d3d9 {

struct DeviceHookPolicyInput {
    bool textureFilteringSupported{};
    bool uiVertexBufferOptimizationSupported{};
    bool screenSpaceAlignmentSupported{};
    bool scissorCorrectionSupported{};
    bool shaderCorrectionSupported{};
    bool optimizeUiVertexBuffer{};
    bool shaderCorrections{};
    bool runtimeShaderObservationSupported{};
    bool redundantStateOptimizationSupported{};
    bool optimizeStateSubmission{};
};

struct DeviceHookPolicy {
    bool createVertexBuffer{};
    bool drawPrimitiveUp{};
    bool setScissorRect{};
    bool setSamplerState{};
    bool createPixelShader{};
    bool setPixelShader{};
    bool setPixelShaderConstantF{};
    bool setRenderState{};
    bool setTexture{};
    bool setTextureStageState{};
    bool setVertexShaderConstantF{};
    bool setVertexShader{};
    bool setStreamSource{};
    bool stateBlocks{};
    bool redundantStateFilter{};
};

DeviceHookPolicy ResolveDeviceHookPolicy(const DeviceHookPolicyInput& input);

} // namespace novafix::d3d9
