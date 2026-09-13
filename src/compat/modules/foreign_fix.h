#pragma once

#include <cstddef>
#include <span>

namespace novafix::compat::foreign_fix {

enum class Product {
    None,
    FF13FixFamily,
};

Product Classify(std::span<const std::byte> image);
const char* ProductName(Product product);

} // namespace novafix::compat::foreign_fix
