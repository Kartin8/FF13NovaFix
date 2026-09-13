#pragma once

struct IDirect3DDevice9;

namespace novafix {
namespace game {
class ImageView;

namespace lr_snapshot::capture_pipeline {

bool HasExpectedContract(const ImageView& image);
bool InstallHooks(const ImageView& image);
bool RevertHooks();
void OnPresent(IDirect3DDevice9* device);

} // namespace lr_snapshot::capture_pipeline
} // namespace game
} // namespace novafix
