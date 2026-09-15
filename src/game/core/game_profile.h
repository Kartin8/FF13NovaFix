#pragma once

#include "game/core/image_view.h"
#include "game/core/release_channel.h"

#include <windows.h>

#include <cstdint>
#include <string_view>

namespace novafix::game {

enum class Title : std::uint8_t {
    Unknown,
    FinalFantasyXIII,
    FinalFantasyXIII2,
    LightningReturns,
};

enum class BuildSupport : std::uint8_t {
    UnknownTitle,
    RecognizedTitle,
    CompatibleLayout,
    ExactBuild,
};

enum class Capability : std::uint64_t {
    None = 0,
    LauncherGraphics = 1ull << 0,
    FramePacing = 1ull << 1,
    HighFpsFacialTiming = 1ull << 2,
    DynamicPrompts = 1ull << 3,
    ControllerHotplug = 1ull << 4,
    HighResolutionScissor = 1ull << 5,
    PresentationOverride = 1ull << 6,
    ControllerVibration = 1ull << 7,
    GracefulShutdown = 1ull << 8,
    UiVertexBufferOptimization = 1ull << 9,
    ScreenSpaceAlignment = 1ull << 10,
    TextureFiltering = 1ull << 11,
    SteamCloudSafety = 1ull << 12,
    ShaderCorrection = 1ull << 13,
    MemorySafety = 1ull << 14,
    NativeMenuExtensions = 1ull << 15,
    SnapshotPhotoMode = 1ull << 16,
    MotionBlendCorrection = 1ull << 17,
    TitanRouteCorrection = 1ull << 18,
    HighResolutionHudSampling = 1ull << 19,
    SchemataRename = 1ull << 20,
    MapMarkerRename = 1ull << 21,
    ChocoboRename = 1ull << 22,
    ArbitraryAspectRatio = 1ull << 23,
};

using CapabilityMask = std::uint64_t;

constexpr CapabilityMask CapabilityBit(Capability capability) {
    return static_cast<CapabilityMask>(capability);
}

struct GameProfile {
    Title title{Title::Unknown};
    BuildSupport support{BuildSupport::UnknownTitle};
    const char* titleName{"Unknown application"};
    const char* buildName{"unrecognized"};
    ReleaseChannel releaseChannel{ReleaseChannel::Unknown};
    ImageFingerprint fingerprint{};
    CapabilityMask capabilities{};
    bool imageValid{};
};

// The filename only identifies the game. Patching still needs a matching PE
Title IdentifyTitle(std::wstring_view executablePath);
const char* TitleName(Title title);
const char* SupportName(BuildSupport support);
GameProfile ResolveProfile(
    Title title, const ImageFingerprint& fingerprint, bool imageValid,
    ReleaseChannel releaseChannel = ReleaseChannel::Unknown);

// Capture the executable before any patch changes .text. Each feature still
// checks the bytes at its own patch site
const GameProfile& CurrentProfile();
bool Supports(Capability capability);

} // namespace novafix::game
