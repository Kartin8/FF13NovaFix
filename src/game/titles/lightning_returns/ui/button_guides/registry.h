#pragma once

#include <cstddef>
#include <cstdint>

namespace novafix::game {
class ImageView;
}

namespace novafix::game::lr_prompt_guides {

bool Install(const ImageView& image);
bool Revert();
void BeginRefresh(std::uint64_t generation);
std::size_t RebuildMissing(std::uint64_t generation);

} // namespace novafix::game::lr_prompt_guides
