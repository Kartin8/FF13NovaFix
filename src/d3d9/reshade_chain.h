#pragma once

#include <string>

namespace novafix::d3d9::reshade_chain {

bool PrepareLocalDxvk(const std::wstring& reshadeDll,
                      const std::wstring& dxvkDll);

} // namespace novafix::d3d9::reshade_chain
