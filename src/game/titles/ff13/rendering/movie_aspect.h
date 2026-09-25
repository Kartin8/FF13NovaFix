#pragma once

#include "patch/transaction.h"

namespace novafix::game {
class ImageView;
}

namespace novafix::game::ff13_movie_aspect {

// Register the movie quad CALL in the existing all-or-nothing XIII
// presentation transaction; the movie detour has its own rollback.
bool RegisterPrimitiveCall(patch::Transaction& transaction,
                           const ImageView& image);
void Prepare(const ImageView& image);
bool InstallPresentDetour(const ImageView& image);
void Revert();

} // namespace novafix::game::ff13_movie_aspect
