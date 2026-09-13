#include "game/titles/ff13/title_adapter.h"

#include "game/shared/controllers/controller_hotplug.h"
#include "game/shared/controllers/controller_vibration.h"
#include "game/shared/timing/frame_pacing.h"
#include "game/shared/timing/facial_animation_timing.h"
#include "game/shared/ui/scissor_fix.h"
#include "game/titles/ff13/animation/motion_blend_correction.h"
#include "game/titles/ff13/timing/cutscene_terminal_frame.h"
#include "game/titles/ff13/timing/frame_pacing_runtime.h"
#include "game/titles/ff13/titan_route_correction.h"
#include "game/titles/ff13/ui/prompt_mode_manager.h"

#include <windows.h>

#include <array>

namespace novafix::game::adapters::ff13 {
namespace {

constexpr CapabilityMask kCapabilities =
    CapabilityBit(Capability::LauncherGraphics) |
    CapabilityBit(Capability::FramePacing) |
    CapabilityBit(Capability::HighFpsFacialTiming) |
    CapabilityBit(Capability::DynamicPrompts) |
    CapabilityBit(Capability::ControllerHotplug) |
    CapabilityBit(Capability::ControllerVibration) |
    CapabilityBit(Capability::HighResolutionScissor) |
    CapabilityBit(Capability::PresentationOverride) |
    CapabilityBit(Capability::GracefulShutdown) |
    CapabilityBit(Capability::UiVertexBufferOptimization) |
    CapabilityBit(Capability::ScreenSpaceAlignment) |
    CapabilityBit(Capability::TextureFiltering) |
    CapabilityBit(Capability::MotionBlendCorrection) |
    CapabilityBit(Capability::TitanRouteCorrection);

constexpr std::array<BuildDescriptor, 2> kBuilds{{
    {
        {IMAGE_FILE_MACHINE_I386, IMAGE_NT_OPTIONAL_HDR32_MAGIC,
         0x545C8A57u, 38313984u, 13185024u, 0x650CAF9A22A812E3ull},
        ReleaseChannel::Steam,
        "Steam 2014 (Nova/community replacement executable)",
        "Steam 2014 Nova/community layout (modified executable)",
        kCapabilities,
    },
    {
        {IMAGE_FILE_MACHINE_I386, IMAGE_NT_OPTIONAL_HDR32_MAGIC,
         0x545C8A57u, 38899712u, 13181582u, 0xD12001E1F49CAE5Aull},
        ReleaseChannel::Steam,
        "Steam 2014 (original CEG executable)",
        "Steam 2014 original CEG/LAA layout (modified executable)",
        kCapabilities,
    },
}};

constexpr std::array<FeatureDescriptor, 9> kFeatures{{
    {"frame-pacing", Capability::FramePacing,
     &ff13_frame_pacing_runtime::Initialize,
     &ff13_frame_pacing_runtime::Pump, nullptr},
    {"high-fps-facial-timing", Capability::HighFpsFacialTiming,
     &facial_animation_timing::Initialize, &facial_animation_timing::Pump,
     nullptr},
    {"dynamic-prompts", Capability::DynamicPrompts, &ff13_prompt_mode::Pump,
     nullptr, nullptr},
    {"controller-hotplug", Capability::ControllerHotplug,
     &controller_hotplug::Initialize,
     &controller_hotplug::Pump,
     &controller_hotplug::NotifyDeviceTopologyChanged,
     &controller_hotplug::Available},
    {"controller-vibration", Capability::ControllerVibration,
     &controller_vibration::Initialize,
     &controller_vibration::Pump,
     &controller_vibration::NotifyDeviceTopologyChanged},
    {"high-resolution-scissor", Capability::HighResolutionScissor,
     &scissor_fix::Initialize, &scissor_fix::Pump, nullptr},
    {"ff13-motion-blend", Capability::MotionBlendCorrection,
     &ff13_motion_blend_correction::Pump, nullptr, nullptr},
    {"ff13-titan-route", Capability::TitanRouteCorrection,
     &ff13_titan_route_correction::Initialize,
     &ff13_titan_route_correction::Pump, nullptr},
    {"cutscene-terminal-frame-fix", Capability::FramePacing,
     &ff13_cutscene_terminal_frame::Pump, nullptr, nullptr},
}};

} // namespace

std::span<const BuildDescriptor> Builds() {
    return kBuilds;
}

std::span<const FeatureDescriptor> Features() {
    return kFeatures;
}

} // namespace novafix::game::adapters::ff13
