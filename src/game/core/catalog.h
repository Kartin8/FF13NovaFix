#pragma once

#include "game/core/game_profile.h"

#include <span>

namespace novafix::game::adapters {

struct BuildDescriptor {
    ImageFingerprint fingerprint;
    ReleaseChannel releaseChannel;
    const char* exactName;
    const char* compatibleName;
    CapabilityMask capabilities;
};

struct FeatureDescriptor {
    const char* name;
    Capability capability;
    void (*initialize)();
    void (*update)();
    void (*notifyDeviceTopologyChanged)();
    bool (*ready)(){};
};

std::span<const BuildDescriptor> BuildsFor(Title title);
std::span<const FeatureDescriptor> FeaturesFor(Title title);

} // namespace novafix::game::adapters
