#include "game/titles/ff13_2/title_adapter.h"

#include "game/shared/controllers/controller_hotplug.h"
#include "game/shared/controllers/controller_vibration.h"
#include "game/shared/graceful_shutdown.h"
#include "game/shared/timing/facial_animation_timing.h"
#include "game/steam/steam_cloud_safety.h"
#include "game/titles/ff13_2/animation/chain_timing.h"
#include "game/titles/ff13_2/animation/root_motion.h"
#include "game/titles/ff13_2/camera/aspect_ratio.h"
#include "game/titles/ff13_2/camera/chocobo_race.h"
#include "game/titles/ff13_2/camera/scene_a_bahamut.h"
#include "game/titles/ff13_2/rendering/hud_sampling.h"
#include "game/titles/ff13_2/rendering/shadows/fixed_depth.h"
#include "game/titles/ff13_2/rendering/shadows/optimization.h"
#include "game/titles/ff13_2/resources/budget.h"
#include "game/titles/ff13_2/resources/unwrap.h"
#include "game/titles/ff13_2/timing/cutscene_terminal_frame.h"
#include "game/titles/ff13_2/timing/frame_pacing_runtime.h"
#include "game/titles/ff13_2/ui/main_menu_frame.h"
#include "game/titles/ff13_2/ui/prompt_mode_manager.h"

#include <windows.h>

#include <array>

namespace novafix::game::adapters::ff13_2 {
namespace {

// Each feature checks its own XIII-2 bytes. An LAA-only edit leaves .text and
// this fingerprint unchanged
constexpr CapabilityMask kCapabilities =
    CapabilityBit(Capability::LauncherGraphics) |
    CapabilityBit(Capability::FramePacing) |
    CapabilityBit(Capability::HighFpsFacialTiming) |
    CapabilityBit(Capability::DynamicPrompts) |
    CapabilityBit(Capability::ControllerHotplug) |
    CapabilityBit(Capability::ControllerVibration) |
    CapabilityBit(Capability::PresentationOverride) |
    CapabilityBit(Capability::GracefulShutdown) |
    CapabilityBit(Capability::SteamCloudSafety) |
    CapabilityBit(Capability::UiVertexBufferOptimization) |
    CapabilityBit(Capability::ScreenSpaceAlignment) |
    CapabilityBit(Capability::TextureFiltering) |
    CapabilityBit(Capability::ShaderCorrection) |
    CapabilityBit(Capability::MemorySafety) |
    CapabilityBit(Capability::HighResolutionHudSampling) |
    CapabilityBit(Capability::ArbitraryAspectRatio);

constexpr std::array<BuildDescriptor, 2> kBuilds{{
    {
        {IMAGE_FILE_MACHINE_I386, IMAGE_NT_OPTIONAL_HDR32_MAGIC,
         0x548043BEu, 82325504u, 9754222u, 0xB997E471B28C040Eull},
        ReleaseChannel::Steam,
        "Steam 2014 (original CEG executable)",
        "Steam 2014 CEG layout (modified executable)",
        kCapabilities,
    },
    {
        {IMAGE_FILE_MACHINE_I386, IMAGE_NT_OPTIONAL_HDR32_MAGIC,
         0x548043BEu, 82911232u, 9754222u, 0xB997E471B28C040Eull},
        ReleaseChannel::Steam,
        "Steam 2014 Asia (original executable)",
        "Steam 2014 Asia layout (modified executable)",
        kCapabilities,
    },
}};

constexpr std::array<FeatureDescriptor, 19> kFeatures{{
    {"arbitrary-aspect-ratio", Capability::ArbitraryAspectRatio,
     &xiii2_aspect_ratio::Initialize, nullptr, nullptr},
    {"shared-main-menu-wide-frame", Capability::ArbitraryAspectRatio,
     &xiii2_main_menu_frame::Initialize, nullptr, nullptr},
    {"frame-pacing", Capability::FramePacing,
     &xiii2_frame_pacing_runtime::Initialize,
     &xiii2_frame_pacing_runtime::Pump, nullptr},
    {"high-fps-facial-timing", Capability::HighFpsFacialTiming,
     &facial_animation_timing::Initialize, &facial_animation_timing::Pump,
     nullptr},
    {"root-motion-delta", Capability::FramePacing,
     &xiii2_root_motion::Initialize, &xiii2_root_motion::Pump, nullptr},
    {"chain-fixed-step", Capability::FramePacing,
     &xiii2_chain_timing::Initialize, &xiii2_chain_timing::Pump, nullptr},
    {"chocobo-race-camera-transition", Capability::FramePacing,
     &xiii2_chocobo_race::Initialize, &xiii2_chocobo_race::Pump, nullptr},
    {"scene-a-bahamut-acos", Capability::FramePacing,
     &xiii2_scene_a_bahamut::Initialize,
     &xiii2_scene_a_bahamut::Pump, nullptr},
    {"resource-frame-budget", Capability::FramePacing,
     &xiii2_resource_budget::Initialize, &xiii2_resource_budget::Pump, nullptr},
    {"dynamic-prompts", Capability::DynamicPrompts,
     &xiii2_prompts::Pump, nullptr, nullptr},
    {"high-resolution-hud-sampling", Capability::HighResolutionHudSampling,
     &xiii2_hud_sampling::Install, nullptr, nullptr},
    {"shadow-fixed-depth", Capability::ShaderCorrection,
     &xiii2_shadow_fixed_depth::Pump, nullptr, nullptr,
     &xiii2_shadow_fixed_depth::Available},
    {"combined-shadow-optimization", Capability::ShaderCorrection,
     &xiii2_shadow_optimization::Pump,
     &xiii2_shadow_optimization::Pump, nullptr,
     &xiii2_shadow_optimization::InstallationViable},
    {"controller-hotplug", Capability::ControllerHotplug,
     &controller_hotplug::Initialize,
     &controller_hotplug::Pump,
     &controller_hotplug::NotifyDeviceTopologyChanged,
     &controller_hotplug::Available},
    {"controller-vibration", Capability::ControllerVibration,
     &controller_vibration::Initialize,
     &controller_vibration::Pump,
     &controller_vibration::NotifyDeviceTopologyChanged},
    {"graceful-shutdown", Capability::GracefulShutdown, &shutdown::Pump,
     nullptr, nullptr},
    {"steam-cloud-safety", Capability::SteamCloudSafety,
     &steam_cloud::InstallEarly, &steam_cloud::Pump, nullptr,
     &steam_cloud::InstallationViable},
    {"d3d9-resource-unwrap", Capability::MemorySafety,
     &xiii2_resource_unwrap::Pump, nullptr, nullptr},
    {"cutscene-terminal-frame-fix", Capability::FramePacing,
     &xiii2_cutscene_terminal_frame::Pump, nullptr, nullptr},
}};

} // namespace

std::span<const BuildDescriptor> Builds() {
    return kBuilds;
}

std::span<const FeatureDescriptor> Features() {
    return kFeatures;
}

} // namespace novafix::game::adapters::ff13_2
