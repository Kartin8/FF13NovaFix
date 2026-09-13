#pragma once

namespace novafix {
namespace patch {
class Transaction;
}

namespace game {
class ImageView;

namespace lr_snapshot_message_editor {

bool HasExpectedContract(const ImageView& image);
bool AddRedirects(patch::Transaction& transaction,
                  const ImageView& image);
void ClearBindings();

} // namespace lr_snapshot_message_editor
} // namespace game
} // namespace novafix
