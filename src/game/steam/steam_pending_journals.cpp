#include "game/steam/steam_pending_journals.h"

#include "game/steam/steam_save_files.h"

#include <algorithm>
#include <cstdint>

namespace novafix::game::steam_cloud::pending_journals {
namespace {

constexpr wchar_t kAchievementFile[] = L"NovaFix.pending-achievements";
constexpr wchar_t kDeletionFile[] = L"NovaFix.pending-cloud-deletions";
constexpr wchar_t kUploadFile[] = L"NovaFix.pending-cloud-uploads";
constexpr std::int32_t kMaximumMetadataBytes = 64 * 1024;

bool LoadLines(const wchar_t* filename, std::vector<std::string>& names) {
    names.clear();
    std::vector<char> bytes;
    const std::wstring root = save_files::Root();
    if (root.empty()) return false;
    if (!save_files::ReadWholeFile(
            root + L'\\' + filename, bytes, kMaximumMetadataBytes)) {
        return true;
    }
    std::string current;
    for (const char c : bytes) {
        if (c == '\r') continue;
        if (c == '\n') {
            if (!current.empty() && current.size() <= 127) {
                names.push_back(current);
            }
            current.clear();
        } else if (static_cast<unsigned char>(c) >= 0x20 && c != '\\') {
            current.push_back(c);
        }
    }
    if (!current.empty() && current.size() <= 127) names.push_back(current);
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return true;
}

bool StoreLines(const wchar_t* filename,
                const std::vector<std::string>& names) {
    const std::wstring root = save_files::Root();
    if (!save_files::EnsureDirectory(root)) return false;
    std::string contents;
    for (const std::string& name : names) {
        if (name.empty() || name.size() > 127 ||
            name.find_first_of("\r\n\\") != std::string::npos) {
            continue;
        }
        contents.append(name);
        contents.push_back('\n');
    }
    if (contents.size() > static_cast<std::size_t>(kMaximumMetadataBytes)) {
        return false;
    }
    return save_files::AtomicWritePath(
        root + L'\\' + filename, contents.data(),
        static_cast<std::int32_t>(contents.size()), false);
}

} // namespace

bool LoadAchievements(std::vector<std::string>& names) {
    return LoadLines(kAchievementFile, names);
}

bool StoreAchievements(const std::vector<std::string>& names) {
    return StoreLines(kAchievementFile, names);
}

bool LoadDeletions(std::vector<std::string>& names) {
    return LoadLines(kDeletionFile, names);
}

bool StoreDeletions(const std::vector<std::string>& names) {
    return StoreLines(kDeletionFile, names);
}

bool LoadUploads(std::vector<std::string>& names) {
    return LoadLines(kUploadFile, names);
}

bool StoreUploads(const std::vector<std::string>& names) {
    return StoreLines(kUploadFile, names);
}

} // namespace novafix::game::steam_cloud::pending_journals
