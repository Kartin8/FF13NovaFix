#include "game/titles/lightning_returns/title_adapter.h"

#include "game/shared/controllers/controller_hotplug.h"
#include "game/shared/controllers/controller_vibration.h"
#include "game/shared/timing/facial_animation_timing.h"
#include "game/titles/lightning_returns/naming/chocobo_name.h"
#include "game/titles/lightning_returns/naming/map_marker_name.h"
#include "game/titles/lightning_returns/naming/schemata_name.h"
#include "game/titles/lightning_returns/snapshot/mode_controller.h"
#include "game/titles/lightning_returns/ui/prompt_mode_manager.h"
#include "game/titles/lightning_returns/ui/text_editor/editor.h"
#include "game/titles/lightning_returns/ui/window_menu_win32.h"

#include <windows.h>

#include <array>

namespace novafix::game::adapters::lightning_returns {
namespace {

constexpr CapabilityMask kCapabilities =
    CapabilityBit(Capability::HighFpsFacialTiming) |
    CapabilityBit(Capability::DynamicPrompts) |
    CapabilityBit(Capability::ControllerHotplug) |
    CapabilityBit(Capability::ControllerVibration) |
    CapabilityBit(Capability::NativeMenuExtensions) |
    CapabilityBit(Capability::SnapshotPhotoMode) |
    CapabilityBit(Capability::SchemataRename) |
    CapabilityBit(Capability::MapMarkerRename) |
    CapabilityBit(Capability::ChocoboRename);

constexpr std::array<BuildDescriptor, 1> kBuilds{{
    {
        {IMAGE_FILE_MACHINE_I386, IMAGE_NT_OPTIONAL_HDR32_MAGIC,
         0x60D2A2C4u, 82083840u, 10383108u, 0x5987AA6B385A20FAull},
        ReleaseChannel::Steam,
        "Steam 2021 executable",
        "Steam 2021 layout (modified executable)",
        kCapabilities,
    },
}};

constexpr std::array<FeatureDescriptor, 10> kFeatures{{
    {"native-addon-menu", Capability::NativeMenuExtensions,
     nullptr, &lr_native_menu::Pump, nullptr},
    {"high-fps-facial-timing", Capability::HighFpsFacialTiming,
     &facial_animation_timing::Initialize, &facial_animation_timing::Pump,
     nullptr},
    {"dynamic-prompts", Capability::DynamicPrompts,
     &lr_prompts::Pump, nullptr, nullptr},
    {"controller-hotplug", Capability::ControllerHotplug,
     &controller_hotplug::Initialize,
     &controller_hotplug::Pump,
     &controller_hotplug::NotifyDeviceTopologyChanged,
     &controller_hotplug::Available},
    {"controller-vibration", Capability::ControllerVibration,
     &controller_vibration::Initialize,
     &controller_vibration::Pump,
     &controller_vibration::NotifyDeviceTopologyChanged},
    {"snapshot-photo-mode", Capability::SnapshotPhotoMode,
     &lr_snapshot::Pump, nullptr, nullptr},
    {"native-text-editor", Capability::SnapshotPhotoMode,
     nullptr, &lr_text_editor::Pump, nullptr},
    {"native-chocobo-rename", Capability::ChocoboRename,
     &lr_chocobo_rename::Initialize, &lr_chocobo_rename::Pump, nullptr},
    {"native-schemata-rename", Capability::SchemataRename,
     &lr_schemata_rename::Initialize, nullptr, nullptr},
    {"native-map-marker-rename", Capability::MapMarkerRename,
     &lr_map_marker_rename::Initialize, &lr_map_marker_rename::Pump, nullptr},
}};

} // namespace

std::span<const BuildDescriptor> Builds() {
    return kBuilds;
}

std::span<const FeatureDescriptor> Features() {
    return kFeatures;
}

} // namespace novafix::game::adapters::lightning_returns
