#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace novafix::patch {

struct SignatureMatch {
    const std::byte* address{};
    std::size_t count{};
};

class Signature {
public:
    static std::optional<Signature> Parse(std::string_view text);

    SignatureMatch Find(std::span<const std::byte> region) const;

private:
    std::vector<int> bytes_;
};

} // namespace novafix::patch
