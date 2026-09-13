#pragma once

#include "compat/addons/plugin_controls.h"

#include <windows.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace novafix::compat::plugin_descriptor {

bool CopyDescriptor(const NovaFixPluginDescriptor* source,
                    NovaFixPluginDescriptor* destination,
                    DWORD* exceptionCode) noexcept;
void ReadIdentity(const NovaFixPluginDescriptor& descriptor,
                  std::wstring_view source, std::size_t fallbackIndex,
                  plugin_controls::Plugin& plugin);
void ReadControls(const NovaFixPluginDescriptor& descriptor,
                  plugin_controls::Plugin& plugin);
void ReadNativeMenus(const NovaFixPluginDescriptor& descriptor,
                     plugin_controls::Plugin& plugin);
bool SameId(const std::string& left, const std::string& right);

} // namespace novafix::compat::plugin_descriptor
