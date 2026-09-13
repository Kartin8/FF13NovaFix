#pragma once

#include <d3d9.h>

#include <filesystem>

namespace novafix::game::lr_snapshot::capture {

bool SaveNative(void* listener, std::filesystem::path& savedPath);
bool SaveBackBuffer(IDirect3DDevice9* device,
                    std::filesystem::path& savedPath);

} // namespace novafix::game::lr_snapshot::capture
