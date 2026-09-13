#pragma once

#include <cstdint>

namespace novafix::game {
enum class Title : std::uint8_t;
}

namespace novafix::settings {

enum class DisplayMode : unsigned {
    Windowed,
    Borderless,
    BorderlessFullscreen,
};

struct GraphicsSettings {
    bool overrideLauncherGraphics{true};
    DisplayMode displayMode{DisplayMode::BorderlessFullscreen};
    unsigned monitorIndex{0};
    unsigned width{1920};
    unsigned height{1080};
    unsigned shadowResolution{4096};
    unsigned msaaSamples{4};
    bool highResolutionScissorFix{true};
    bool optimizeUiVertexBuffer{true};
    bool correctScreenSpaceAlignment{true};
    unsigned anisotropicFiltering{16};
    float xiii2MipLodBias{0.0f};
    bool xiii2ShaderCorrections{true};
    bool xiii2OptimizeShadowPipeline{true};
};

void SetGraphicsTitle(game::Title title);
void SetLauncherIntegrationEnabled(bool enabled);
GraphicsSettings Active();
GraphicsSettings Stored();
void PreviewGraphics(const GraphicsSettings& value);
bool CorrectScreenSpaceAlignmentEnabled();
bool OverridesLauncherGraphics();
bool Save(const GraphicsSettings& value);

int ShadowLauncherIndex(unsigned resolution);
int MsaaLauncherIndex(unsigned samples);

} // namespace novafix::settings
