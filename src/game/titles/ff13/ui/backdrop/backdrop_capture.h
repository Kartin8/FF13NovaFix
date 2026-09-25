#pragma once

namespace novafix::game {
class ImageView;
}

namespace novafix::patch {
class Transaction;
}

namespace novafix::game::ff13_ui::backdrop_capture {

bool RegisterAllocationCalls(patch::Transaction& calls, const ImageView& image);
bool RegisterFlushCalls(patch::Transaction& calls, const ImageView& image);
bool RegisterCaptureCalls(patch::Transaction& calls, const ImageView& image);
void Prepare(const ImageView& image);

}
