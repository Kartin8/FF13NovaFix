#include "compat/modules/foreign_fix.h"

#include <cstring>
#include <string_view>

namespace novafix::compat::foreign_fix {
namespace {

bool ContainsAscii(std::span<const std::byte> image, std::string_view marker) {
    if (marker.empty() || image.size() < marker.size()) return false;
    const auto* cursor = reinterpret_cast<const unsigned char*>(image.data());
    const auto* const end = cursor + image.size();
    const unsigned char first = static_cast<unsigned char>(marker.front());
    while (static_cast<std::size_t>(end - cursor) >= marker.size()) {
        const void* match = std::memchr(
            cursor, first, static_cast<std::size_t>(end - cursor) - marker.size() + 1);
        if (!match) return false;
        const auto* candidate = static_cast<const unsigned char*>(match);
        if (std::memcmp(candidate, marker.data(), marker.size()) == 0) return true;
        cursor = candidate + 1;
    }
    return false;
}

bool ContainsWideAscii(std::span<const std::byte> image, std::string_view marker) {
    if (marker.empty() || image.size() < marker.size() * 2) return false;
    const auto* cursor = reinterpret_cast<const unsigned char*>(image.data());
    const auto* const end = cursor + image.size();
    const std::size_t bytesRequired = marker.size() * 2;
    const unsigned char first = static_cast<unsigned char>(marker.front());
    while (static_cast<std::size_t>(end - cursor) >= bytesRequired) {
        const void* match = std::memchr(
            cursor, first, static_cast<std::size_t>(end - cursor) - bytesRequired + 1);
        if (!match) return false;
        const auto* candidate = static_cast<const unsigned char*>(match);
        bool equal = true;
        for (std::size_t index = 0; index < marker.size(); ++index) {
            if (candidate[index * 2] !=
                    static_cast<unsigned char>(marker[index]) ||
                candidate[index * 2 + 1] != 0) {
                equal = false;
                break;
            }
        }
        if (equal) return true;
        cursor = candidate + 1;
    }
    return false;
}

bool Contains(std::span<const std::byte> image, std::string_view marker) {
    return ContainsAscii(image, marker) || ContainsWideAscii(image, marker);
}

} // namespace

Product Classify(std::span<const std::byte> image) {
    if (image.empty()) return Product::None;

    const bool configuration = Contains(image, "FF13Fix.ini");
    const bool log = Contains(image, "FF13Fix.log");
    const bool implementation = Contains(image, "OneTweakNG");
    const bool gameMarker =
        Contains(image, "Final Fantasy XIII") || Contains(image, "ffxiiiimg.exe");

    // A single marker is too easy to match accidentally, so require corroboration
    if ((configuration && (log || implementation || gameMarker)) ||
        (implementation && log && gameMarker)) {
        return Product::FF13FixFamily;
    }
    return Product::None;
}

const char* ProductName(Product product) {
    switch (product) {
    case Product::None: return "none";
    case Product::FF13FixFamily: return "FF13Fix or FF13Fix PLUS";
    }
    return "unknown";
}

} // namespace novafix::compat::foreign_fix
