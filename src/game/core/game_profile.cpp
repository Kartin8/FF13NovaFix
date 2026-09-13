#include "game/core/game_profile.h"

#include "common/module_path.h"
#include "game/core/catalog.h"
#include "game/core/executable_fingerprint.h"

#include <windows.h>

#include <array>
#include <cwctype>

namespace novafix::game {
namespace {

INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
GameProfile g_profile{};

bool Equal(const ImageFingerprint& left, const ImageFingerprint& right) {
    return left.machine == right.machine &&
           left.optionalHeaderMagic == right.optionalHeaderMagic &&
           left.timeDateStamp == right.timeDateStamp &&
           left.imageSize == right.imageSize &&
           left.textSize == right.textSize &&
           left.textHash == right.textHash;
}

bool SameLayout(const ImageFingerprint& left, const ImageFingerprint& right) {
    return left.machine == right.machine &&
           left.optionalHeaderMagic == right.optionalHeaderMagic &&
           left.imageSize == right.imageSize &&
           left.textSize == right.textSize;
}

std::wstring_view Filename(std::wstring_view pathValue) {
    const std::size_t separator = pathValue.find_last_of(L"\\/");
    return pathValue.substr(separator == std::wstring_view::npos ? 0 : separator + 1);
}

bool EqualInsensitive(std::wstring_view left, std::wstring_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (std::towlower(left[index]) != std::towlower(right[index])) return false;
    }
    return true;
}

bool MatchesExecutableName(std::wstring_view filename,
                           std::wstring_view canonical) {
    if (EqualInsensitive(filename, canonical)) return true;
    constexpr std::wstring_view kPatchedSuffix = L".patch";
    return filename.size() == canonical.size() + kPatchedSuffix.size() &&
           EqualInsensitive(filename.substr(0, canonical.size()), canonical) &&
           EqualInsensitive(filename.substr(canonical.size()), kPatchedSuffix);
}

Title InferTitleFromLayout(const ImageFingerprint& fingerprint) {
    constexpr std::array kTitles{
        Title::FinalFantasyXIII,
        Title::FinalFantasyXIII2,
        Title::LightningReturns,
    };

    Title inferred = Title::Unknown;
    for (const Title candidate : kTitles) {
        bool matches = false;
        for (const adapters::BuildDescriptor& build :
             adapters::BuildsFor(candidate)) {
            if (Equal(build.fingerprint, fingerprint) ||
                SameLayout(build.fingerprint, fingerprint)) {
                matches = true;
                break;
            }
        }
        if (!matches) continue;
        if (inferred != Title::Unknown && inferred != candidate) {
            // Never guess if two title catalogs happen to describe the same
            // PE layout in a future release
            return Title::Unknown;
        }
        inferred = candidate;
    }
    return inferred;
}

BOOL CALLBACK Capture(PINIT_ONCE, PVOID, PVOID*) {
    const std::wstring executable = path::ModuleFile(nullptr);
    const Title title = IdentifyTitle(executable);
    const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
    const auto fingerprint = FingerprintExecutableFile(executable);
    g_profile = ResolveProfile(
        title, fingerprint.value_or(ImageFingerprint{}),
        image.has_value() && fingerprint.has_value(),
        DetectReleaseChannel());
    return TRUE;
}

} // namespace

Title IdentifyTitle(std::wstring_view executablePath) {
    if (executablePath.empty()) return Title::Unknown;
    const std::wstring_view filename = Filename(executablePath);
    if (MatchesExecutableName(filename, L"ffxiiiimg.exe")) return Title::FinalFantasyXIII;
    if (MatchesExecutableName(filename, L"ffxiii2img.exe")) return Title::FinalFantasyXIII2;
    if (MatchesExecutableName(filename, L"LRFF13.exe")) return Title::LightningReturns;
    return Title::Unknown;
}

const char* TitleName(Title title) {
    switch (title) {
    case Title::FinalFantasyXIII: return "Final Fantasy XIII";
    case Title::FinalFantasyXIII2: return "Final Fantasy XIII-2";
    case Title::LightningReturns: return "Lightning Returns: Final Fantasy XIII";
    case Title::Unknown: return "Unknown application";
    }
    return "Unknown application";
}

const char* SupportName(BuildSupport support) {
    switch (support) {
    case BuildSupport::UnknownTitle: return "transparent proxy only";
    case BuildSupport::RecognizedTitle: return "recognized title, unvalidated build";
    case BuildSupport::CompatibleLayout: return "compatible modified layout";
    case BuildSupport::ExactBuild: return "exact build contract";
    }
    return "unknown";
}

GameProfile ResolveProfile(
    Title title, const ImageFingerprint& fingerprint, bool imageValid,
    ReleaseChannel releaseChannel) {
    if (title == Title::Unknown && imageValid) {
        title = InferTitleFromLayout(fingerprint);
    }
    GameProfile profile{};
    profile.title = title;
    profile.titleName = TitleName(title);
    profile.releaseChannel = releaseChannel;
    profile.support = title == Title::Unknown
        ? BuildSupport::UnknownTitle : BuildSupport::RecognizedTitle;
    profile.fingerprint = fingerprint;
    profile.imageValid = imageValid;
    if (!imageValid) return profile;

    const auto builds = adapters::BuildsFor(title);
    for (const adapters::BuildDescriptor& build : builds) {
        if (!Equal(build.fingerprint, fingerprint)) continue;
        profile.support = BuildSupport::ExactBuild;
        profile.releaseChannel = build.releaseChannel;
        profile.buildName = build.exactName;
        profile.capabilities = build.capabilities;
        break;
    }
    if (profile.support == BuildSupport::ExactBuild) return profile;

    for (const adapters::BuildDescriptor& build : builds) {
        if (!SameLayout(build.fingerprint, fingerprint)) continue;
        profile.support = BuildSupport::CompatibleLayout;
        profile.releaseChannel = build.releaseChannel;
        profile.buildName = build.compatibleName;
        profile.capabilities = build.capabilities;
        break;
    }
    return profile;
}

const GameProfile& CurrentProfile() {
    InitOnceExecuteOnce(&g_once, &Capture, nullptr, nullptr);
    return g_profile;
}

bool Supports(Capability capability) {
    return (CurrentProfile().capabilities & CapabilityBit(capability)) != 0;
}

} // namespace novafix::game
