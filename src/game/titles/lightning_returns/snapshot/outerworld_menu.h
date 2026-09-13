#pragma once

namespace novafix {
namespace patch {
class Transaction;
}

namespace game {
class ImageView;

namespace lr_snapshot::outerworld_menu {

bool HasExpectedContract(const ImageView& image);
bool Bind(const ImageView& image);
bool AddRedirects(patch::Transaction& transaction,
                  const ImageView& image);
bool InstallHooks(const ImageView& image);
bool RevertHooks();
void ClearBindings();
bool HasOfflineSnapshotMenu();

} // namespace lr_snapshot::outerworld_menu
} // namespace game
} // namespace novafix
