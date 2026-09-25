#pragma once

namespace novafix::game {
class ImageView;
}

namespace novafix::patch {
class Transaction;
}

namespace novafix::game::ff13_composition_aspect {

bool RegisterCompositeCalls(patch::Transaction& calls,
                            const ImageView& image);
bool RegisterSnapshotCall(patch::Transaction& calls,
                          const ImageView& image);
void Prepare(const ImageView& image);

} // namespace novafix::game::ff13_composition_aspect
