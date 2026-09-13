#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace novafix::compat::loaded_modules {

struct Entry {
    std::wstring path;
    std::wstring displayName;
};

struct Report {
    std::vector<Entry> entries;
};

// Takes one snapshot of modules beside NovaFix and in add-on directories
const Report& CurrentReport();

bool IsInNotificationScope(const std::wstring& modulePath,
                           const std::wstring& novaDirectory,
                           const std::wstring& novaModulePath);
std::wstring DisplayName(const std::wstring& modulePath);
std::wstring NotificationMessage(const Report& report);
// Stable, order-independent identity for detecting add-on list changes
std::wstring Signature(const Report& report);

} // namespace novafix::compat::loaded_modules
