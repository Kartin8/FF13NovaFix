#pragma once

namespace novafix::game {
class ImageView;
}

namespace novafix::game::xiii2_movie_aspect {

// The movie present scope, quad submission, and final composite form one
// presentation pipeline. Initialize in the same order as the main hooks.
bool Prepare(const ImageView& image);
bool InstallPresentDetour(const ImageView& image);
bool ApplyPrimitivePatch(const ImageView& image);
bool ApplyCompositePatch(const ImageView& image);
void Revert();

} // namespace novafix::game::xiii2_movie_aspect
