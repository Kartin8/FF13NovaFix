#include "common/module_path.h"

#include <cwctype>
#include <vector>

namespace novafix::path {
namespace {

constexpr DWORD kInitialPathCapacity = 512;
constexpr DWORD kMaximumPathCapacity = 32768;

std::wstring SystemDirectory() {
    DWORD capacity = kInitialPathCapacity;
    while (capacity <= kMaximumPathCapacity) {
        std::vector<wchar_t> buffer(capacity);
        const UINT length = GetSystemDirectoryW(buffer.data(), capacity);
        if (length == 0) return {};
        if (length < capacity) return std::wstring(buffer.data(), length);
        capacity = length < kMaximumPathCapacity ? length + 1 : kMaximumPathCapacity + 1;
    }
    return {};
}

wchar_t ComparableCharacter(wchar_t character) {
    if (character == L'/') return L'\\';
    return static_cast<wchar_t>(std::towlower(character));
}

} // namespace

std::wstring ModuleFile(HMODULE module) {
    for (DWORD capacity = kInitialPathCapacity; capacity <= kMaximumPathCapacity; capacity *= 2) {
        if (capacity > kMaximumPathCapacity / 2) capacity = kMaximumPathCapacity;
        std::vector<wchar_t> buffer(capacity);
        SetLastError(ERROR_SUCCESS);
        const DWORD length = GetModuleFileNameW(module, buffer.data(), capacity);
        if (length == 0) return {};
        if (length < capacity) return std::wstring(buffer.data(), length);
        if (capacity == kMaximumPathCapacity) break;
    }
    return {};
}

std::wstring Directory(std::wstring_view file) {
    while (!file.empty() && (file.back() == L'\\' || file.back() == L'/')) file.remove_suffix(1);
    const size_t separator = file.find_last_of(L"\\/");
    return separator == std::wstring_view::npos ? std::wstring{} : std::wstring(file.substr(0, separator));
}

std::wstring SystemFile(std::wstring_view fileName) {
    std::wstring directory = SystemDirectory();
    if (directory.empty() || fileName.empty()) return {};
    if (directory.back() != L'\\' && directory.back() != L'/') directory.push_back(L'\\');
    directory.append(fileName);
    return directory;
}

bool Equivalent(std::wstring_view left, std::wstring_view right) {
    while (!left.empty() && (left.back() == L'\\' || left.back() == L'/')) left.remove_suffix(1);
    while (!right.empty() && (right.back() == L'\\' || right.back() == L'/')) right.remove_suffix(1);
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index) {
        if (ComparableCharacter(left[index]) != ComparableCharacter(right[index])) return false;
    }
    return true;
}

} // namespace novafix::path
