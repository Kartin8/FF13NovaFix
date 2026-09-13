#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace novafix::game::steam_cloud::save_identity {

struct ContentIdentity {
    std::uint64_t size{};
    std::array<unsigned char, 32> sha256{};

    bool operator==(const ContentIdentity&) const = default;
};

bool Compute(std::span<const unsigned char> bytes,
             ContentIdentity& identity);

} // namespace novafix::game::steam_cloud::save_identity
