#pragma once

#include "compat/modules/foreign_fix.h"

#include <string>
#include <vector>

namespace novafix::compat::compatibility {

struct Detection {
    foreign_fix::Product product{foreign_fix::Product::None};
    std::wstring path;
};

struct Report {
    std::vector<Detection> blocked;
    std::vector<Detection> active;
};

bool ShouldBlockBackend(const std::wstring& path);

bool ShouldBlockExtraLibrary(const std::wstring& path);

void Initialize();
bool AllowsNovaFixChanges();
Report CurrentReport();

} // namespace novafix::compat::compatibility
