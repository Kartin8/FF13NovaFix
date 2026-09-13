#pragma once

#include <windows.h>

#include <string>
#include <string_view>

namespace novafix::path {

std::wstring ModuleFile(HMODULE module);
std::wstring Directory(std::wstring_view file);
std::wstring SystemFile(std::wstring_view fileName);
bool Equivalent(std::wstring_view left, std::wstring_view right);

} // namespace novafix::path
