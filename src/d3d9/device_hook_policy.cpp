#include "d3d9/device_hook_policy.h"

namespace novafix::d3d9 {

DeviceHookPolicy ResolveDeviceHookPolicy(const DeviceHookPolicyInput& input) {
    const bool uiBuffers = input.uiVertexBufferOptimizationSupported &&
                           input.optimizeUiVertexBuffer;
    const bool shaders = input.shaderCorrectionSupported &&
                         input.shaderCorrections;
    const bool redundantStates =
        input.redundantStateOptimizationSupported &&
        input.optimizeStateSubmission;
    DeviceHookPolicy result{};
    result.createVertexBuffer = uiBuffers;
    // Keep these hooks installed so their settings can change without touching
    // a live device vtable
    result.drawPrimitiveUp = input.screenSpaceAlignmentSupported;
    result.setScissorRect = input.scissorCorrectionSupported;
    result.setSamplerState = input.textureFilteringSupported ||
                             redundantStates;
    result.createPixelShader = shaders ||
                               input.runtimeShaderObservationSupported;
    result.setPixelShader = input.runtimeShaderObservationSupported ||
                            redundantStates;
    result.setPixelShaderConstantF =
        input.runtimeShaderObservationSupported || redundantStates;
    result.setRenderState = redundantStates;
    result.setTexture = redundantStates;
    result.setTextureStageState = redundantStates;
    result.setVertexShaderConstantF = redundantStates;
    result.setVertexShader = redundantStates;
    result.setStreamSource = redundantStates;
    result.stateBlocks = redundantStates;
    result.redundantStateFilter = redundantStates;
    return result;
}

} // namespace novafix::d3d9
