#include "game/titles/lightning_returns/snapshot/outerworld_menu.h"

#include "diagnostics/hook_profiler.h"
#include "diagnostics/log.h"
#include "game/core/image_view.h"
#include "game/titles/lightning_returns/ui/message_window.h"
#include "patch/detour32.h"
#include "patch/memory_access.h"
#include "patch/relative_branch.h"
#include "patch/transaction.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace novafix::game::lr_snapshot::outerworld_menu {
namespace {

// SnsMenu's submit command normally opens the dead Outerworld confirmation
// and then starts SnsPost. Replace only that command boundary
constexpr std::array<patch::RelativeCallSite, 1> kOuterworldSubmitCalls{{
    {0x000587EBu, {0xE8, 0xD0, 0xA8, 0xFF, 0xFF}},
}};

// Keep profile-less Snapshot; block all rows of the online message menu
constexpr std::array<patch::RelativeCallSite, 1> kOuterworldCommandCalls{{
    {0x000587C1u, {0xE8, 0x2A, 0xFB, 0xFF, 0xFF}},
}};

// Block teardown's stale/white #backup copy into the pause background
constexpr std::array<patch::RelativeCallSite, 1> kPauseBackupRefreshCalls{{
    {0x0004F746u, {0xE8, 0x55, 0xA9, 0x36, 0x00}},
}};

constexpr std::uint32_t kSnsMenuUpdateRva = 0x00050480u;
constexpr std::uint32_t kSnsMenuCloseRva = 0x00052F10u;
constexpr std::uint32_t kSnsMenuCommandRva = 0x000582F0u;
constexpr std::uint32_t kPauseBackupRefreshRva = 0x003BA0A0u;

constexpr std::array<std::uint8_t, 16> kSnsMenuUpdatePrefix{
    0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x0C, 0x56, 0x57,
    0x8B, 0x7D, 0x08, 0x50, 0x57, 0x8B, 0xF1, 0xE8};
constexpr std::array<std::uint8_t, 16> kSnsMenuClosePrefix{
    0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1, 0x83, 0xBE,
    0xEC, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x85, 0x8B};

// SnsMenu keeps the native Snapshot route and the removed Outerworld post
// workflow in the same object. A negative profile selects the former
constexpr std::size_t kSnsMenuPostFinishedOffset = 244u;
constexpr std::size_t kSnsMenuClosingOffset = 236u;
constexpr std::size_t kSnsMenuProfileOffset = 276u;
constexpr std::size_t kSnsMenuNicknameReadyOffset = 284u;
constexpr std::size_t kSnsMenuMainListOffset = 140u;
constexpr std::size_t kSnsMenuActiveListOffset = 152u;
constexpr std::size_t kSnsListCommandTableOffset = 388u;
constexpr std::size_t kSnsListSelectionOffset = 520u;

using SnsMenuUpdateFn = int (__thiscall*)(void*, int, int);
using SnsMenuCloseFn = void (__thiscall*)(void*, int);
using SnsMenuCommandFn = void (__thiscall*)(void*);
using PauseBackupRefreshFn = void (__cdecl*)();

patch::Detour32 g_snsMenuUpdateDetour;
patch::Detour32 g_snsMenuCloseDetour;
SnsMenuUpdateFn g_originalSnsMenuUpdate{};
SnsMenuCloseFn g_originalSnsMenuClose{};
SnsMenuCommandFn g_originalSnsMenuCommand{};
PauseBackupRefreshFn g_originalPauseBackupRefresh{};
std::atomic<void*> g_offlineSnapshotMenu{};
std::atomic_bool g_snsMenuClosing{false};

constexpr std::array<unsigned char, 2> kXboxXGlyph{{0xF1u, 0x42u}};

struct LocalizedOuterworldText {
    int language;
    const wchar_t* shortMessage;
    const wchar_t* snapshotPrefix;
    const wchar_t* snapshotSuffix;
};

// Use LR language IDs, not Windows locales; keep X as a native inline glyph
constexpr std::array<LocalizedOuterworldText, 8> kOuterworldTexts{{
    {0, L"\x3044\x3064\x304B\x306F\x3002\x4ECA\x65E5\x3067\x306F\x3042\x308A\x307E\x305B\x3093\x3002",
     L"\x30DD\x30FC\x30BA\x30E1\x30CB\x30E5\x30FC\x3067", L"\x3092\x62BC\x3059\x3068\x3001\x30B9\x30CA\x30C3\x30D7\x30B7\x30E7\x30C3\x30C8\x30E2\x30FC\x30C9\x3092\x958B\x3051\x307E\x3059\x3002\x3053\x306E\x65E7Outerworld\x30AA\x30D7\x30B7\x30E7\x30F3\x306F\x73FE\x5728\x5229\x7528\x3067\x304D\x307E\x305B\x3093\x304C\x3001\x5C06\x6765\x8AB0\x304B\x304C\x5FA9\x5143\x3067\x304D\x308B\x3088\x3046\x306B\x6B8B\x3055\x308C\x3066\x3044\x307E\x3059\x3002"},
    {1, L"Maybe someday. Not today.",
     L"Press ", L" in the pause menu to open Snapshot mode. This original Outerworld option is currently unavailable, but has been preserved in case someone restores it in the future."},
    {3, L"Forse un giorno. Non oggi.",
     L"Premi ", L" nel menu di pausa per aprire la modalita Istantanea. Questa vecchia opzione Outerworld non e disponibile, ma e stata conservata nel caso qualcuno la ripristini in futuro."},
    {4, L"Vielleicht irgendwann. Heute nicht.",
     L"Drucke im Pausenmenu ", L", um den Schnappschussmodus zu offnen. Diese alte Outerworld-Option ist derzeit nicht verfugbar, wurde aber fur eine mogliche spatere Wiederherstellung beibehalten."},
    {5, L"Peut-etre un jour. Pas aujourd'hui.",
     L"Appuyez sur ", L" dans le menu de pause pour ouvrir le mode Photo. Cette ancienne option Outerworld est indisponible, mais elle a ete conservee au cas ou quelqu'un la restaurerait un jour."},
    {6, L"Quizas algun dia. Hoy no.",
     L"Pulsa ", L" en el menu de pausa para abrir el modo Instantanea. Esta antigua opcion de Outerworld no esta disponible, pero se ha conservado por si alguien la restaura en el futuro."},
    {8, L"\xC5B8\xC820\xAC00\xB294 \xB420\xACA0\xC9C0\xB9CC\xB2E4. \xC624\xB298\xC740 \xC544\xB2D9\xB2E4.",
     L"\xC77C\xC2DC \xC815\xC9C0 \xBA54\xB274\xC5D0\xC11C ", L"\xB97C \xB204\xB974\xBA74 \xC2A4\xB0C5\xC0C \xBAA8\xB4DC\xB97C \xC5F4 \xC218 \xC788\xC2B5\xB2C8\xB2E4. \xC774 \xC608\xC804 Outerworld \xC635\xC158\xC740 \xD604\xC7AC \xC0AC\xC6A9\xD560 \xC218 \xC5C6\xC9C0\xB9CC, \xBBF8\xB0C0\xC5D0 \xB204\xAD70\xAC00 \xBCF5\xC6D0\xD560 \xC218 \xC788\xB3C4\xB85D \xB0A8\xACA8 \xB450\xC5C8\xC2B5\xB2C8\xB2E4."},
    {10, L"\x4E5F\x8A31\x6709\x4E00\x5929\x3002\x4F46\x4E0D\x662F\x4ECA\x5929\x3002",
     L"\x5728\x66AB\x505C\x9078\x55AE\x4E2D\x6309\x4E0B", L"\x5373\x53EF\x958B\x555F\x5FEB\x7167\x6A21\x5F0F\x3002\x6B64\x820A Outerworld \x9078\x9805\x76EE\x524D\x7121\x6CD5\x4F7F\x7528\xFF0C\x4F46\x4ECD\x4FDD\x7559\x4EE5\x4FBF\x672A\x4F86\x6709\x4EBA\x80FD\x5920\x5FA9\x539F\x6B64\x529F\x80FD\x3002"},
}};

std::int32_t& SnsMenuField(void* menu, std::size_t offset) {
    return *reinterpret_cast<std::int32_t*>(
        static_cast<std::byte*>(menu) + offset);
}

bool IsOfflineSnapshotMenu(void* menu) {
    return menu && SnsMenuField(menu, kSnsMenuProfileOffset) < 0;
}

int SelectedSnsMenuCommand(void* menu) {
    if (!menu || !patch::memory::IsReadable(
            menu, kSnsMenuActiveListOffset + sizeof(void*))) {
        return -1;
    }

    auto* const bytes = static_cast<std::byte*>(menu);
    void* const mainList = *reinterpret_cast<void**>(
        bytes + kSnsMenuMainListOffset);
    void* const activeList = *reinterpret_cast<void**>(
        bytes + kSnsMenuActiveListOffset);
    if (!mainList || activeList != mainList ||
        !patch::memory::IsReadable(
            mainList, kSnsListSelectionOffset + sizeof(std::int32_t))) {
        return -1;
    }

    const auto* const listBytes = static_cast<const std::byte*>(mainList);
    const std::int32_t selection =
        *reinterpret_cast<const std::int32_t*>(
            listBytes + kSnsListSelectionOffset);
    if (selection < 0) return 0;
    if (selection > 3) return -1;

    return *reinterpret_cast<const std::int32_t*>(
        listBytes + kSnsListCommandTableOffset +
        static_cast<std::size_t>(selection) * sizeof(std::int32_t));
}

int __fastcall SnsMenuUpdateHook(void* menu, void*, int a2, int a3) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.lr.snapshot-menu-update-total");
    const SnsMenuUpdateFn original = g_originalSnsMenuUpdate;
    if (!original) return 0;

    if (menu) {
        // Satisfy the offline nickname prerequisite before native UI builds a prompt
        SnsMenuField(menu, kSnsMenuNicknameReadyOffset) = 1;
        if (SnsMenuField(menu, kSnsMenuClosingOffset) == 0) {
            g_snsMenuClosing.store(false, std::memory_order_release);
        }
    }
    if (IsOfflineSnapshotMenu(menu)) {
        g_offlineSnapshotMenu.store(menu, std::memory_order_release);
    }
    return original(menu, a2, a3);
}

void __fastcall SnsMenuCloseHook(void* menu, void*, int playSound) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.lr.snapshot-menu-close-total");
    const SnsMenuCloseFn original = g_originalSnsMenuClose;
    if (!original) return;

    if (menu) {
        // Both Snapshot and Submit Lightning message own SnsModelSnap. Do not
        // let their final teardown promote a stale/white #backup resource
        g_snsMenuClosing.store(true, std::memory_order_release);
    }
    if (IsOfflineSnapshotMenu(menu)) {
        // Profile-less Snapshot has no post. The successful-post branch is
        // the game's own direct cleanup path
        SnsMenuField(menu, kSnsMenuPostFinishedOffset) = 1;
    }
    original(menu, playSound);
}

const LocalizedOuterworldText& CurrentOuterworldText() {
    const int language = lr_message_window::Language();
    for (const auto& candidate : kOuterworldTexts) {
        if (candidate.language == language) return candidate;
    }
    return kOuterworldTexts[1];
}

bool AppendGameText(std::vector<char>& destination, const wchar_t* text) {
    return text && lr_message_window::AppendText(destination, text);
}

bool BuildOuterworldMessage(bool explainSnapshots,
                            std::vector<char>& message) {
    message.clear();
    const auto& localized = CurrentOuterworldText();
    if (!explainSnapshots) {
        if (!AppendGameText(message, localized.shortMessage)) return false;
    } else {
        if (!AppendGameText(message, localized.snapshotPrefix)) return false;
        message.push_back(static_cast<char>(kXboxXGlyph[0]));
        message.push_back(static_cast<char>(kXboxXGlyph[1]));
        if (!AppendGameText(message, localized.snapshotSuffix)) return false;
    }
    message.push_back('\0');
    return true;
}

void ShowOuterworldUnavailableMessage(bool explainSnapshots = false) {
    if (!lr_message_window::Available()) {
        Log("LR Outerworld submit suppressed: native status window unavailable");
        return;
    }

    std::vector<char> message;
    if (!BuildOuterworldMessage(explainSnapshots, message)) {
        Log("LR Outerworld submit suppressed: localized status conversion failed");
        return;
    }
    lr_message_window::Open(message.data(), "Outerworld");
}

void __fastcall OfflineOuterworldSubmitHook(void*, void*, int) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.lr.snapshot-offline-submit-total");
    ShowOuterworldUnavailableMessage();
}

void __fastcall OuterworldMenuCommandHook(void* menu, void*) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.lr.snapshot-menu-command-total");
    if (IsOfflineSnapshotMenu(menu)) {
        if (g_originalSnsMenuCommand) g_originalSnsMenuCommand(menu);
        return;
    }
    const int command = SelectedSnsMenuCommand(menu);
    // Native commands: 0 category, 1 message, 2 item, 3 snapshot
    ShowOuterworldUnavailableMessage(command == 0 || command == 3);
}

void __cdecl SuppressPauseBackupRefreshHook() {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.lr.snapshot-backup-refresh-total");
    if (g_snsMenuClosing.load(std::memory_order_acquire)) {
        return;
    }
    if (g_originalPauseBackupRefresh) g_originalPauseBackupRefresh();
}

template <std::size_t Count>
bool HasCallSites(
    const ImageView& image,
    const std::array<patch::RelativeCallSite, Count>& sites) {
    for (const patch::RelativeCallSite& site : sites) {
        if (!image.At(site.rva, site.expected.size())) return false;
    }
    return true;
}

} // namespace

bool HasExpectedContract(const ImageView& image) {
    return image.At(kSnsMenuUpdateRva, kSnsMenuUpdatePrefix.size()) &&
        image.At(kSnsMenuCloseRva, kSnsMenuClosePrefix.size()) &&
        image.At(kSnsMenuCommandRva, 1u) &&
        image.At(kPauseBackupRefreshRva, 1u) &&
        HasCallSites(image, kOuterworldSubmitCalls) &&
        HasCallSites(image, kOuterworldCommandCalls) &&
        HasCallSites(image, kPauseBackupRefreshCalls);
}

bool Bind(const ImageView& image) {
    const auto snsMenuCommand = image.At(kSnsMenuCommandRva, 1u);
    const auto pauseBackupRefresh = image.At(kPauseBackupRefreshRva, 1u);
    if (!snsMenuCommand || !pauseBackupRefresh) return false;
    g_originalSnsMenuCommand = reinterpret_cast<SnsMenuCommandFn>(
        const_cast<std::byte*>(snsMenuCommand->data()));
    g_originalPauseBackupRefresh =
        reinterpret_cast<PauseBackupRefreshFn>(
            const_cast<std::byte*>(pauseBackupRefresh->data()));
    return true;
}

bool AddRedirects(patch::Transaction& transaction,
                  const ImageView& image) {
    return patch::AddRelativeCalls(
               transaction, image, kOuterworldSubmitCalls,
               reinterpret_cast<const void*>(
                   &OfflineOuterworldSubmitHook)) &&
        patch::AddRelativeCalls(
               transaction, image, kOuterworldCommandCalls,
               reinterpret_cast<const void*>(&OuterworldMenuCommandHook)) &&
        patch::AddRelativeCalls(
               transaction, image, kPauseBackupRefreshCalls,
               reinterpret_cast<const void*>(
                   &SuppressPauseBackupRefreshHook));
}

bool InstallHooks(const ImageView& image) {
    const auto snsMenuUpdate =
        image.At(kSnsMenuUpdateRva, kSnsMenuUpdatePrefix.size());
    const auto snsMenuClose =
        image.At(kSnsMenuCloseRva, kSnsMenuClosePrefix.size());
    if (!snsMenuUpdate || !snsMenuClose) return false;

    if (!g_snsMenuUpdateDetour.Install(
            "lr-offline-snapshot-nickname-bypass",
            const_cast<std::byte*>(snsMenuUpdate->data()),
            reinterpret_cast<void*>(&SnsMenuUpdateHook),
            std::as_bytes(std::span{kSnsMenuUpdatePrefix}),
            reinterpret_cast<void**>(&g_originalSnsMenuUpdate))) {
        LogWarning("LR Snapshot disabled: offline menu update detour failed");
        return false;
    }
    if (!g_snsMenuCloseDetour.Install(
            "lr-offline-snapshot-direct-close",
            const_cast<std::byte*>(snsMenuClose->data()),
            reinterpret_cast<void*>(&SnsMenuCloseHook),
            std::as_bytes(std::span{kSnsMenuClosePrefix}),
            reinterpret_cast<void**>(&g_originalSnsMenuClose))) {
        LogWarning("LR Snapshot disabled: offline direct-close detour failed");
        return false;
    }
    return true;
}

bool RevertHooks() {
    bool clean = true;
    clean = g_snsMenuCloseDetour.RevertAndClear(
                g_originalSnsMenuClose) && clean;
    clean = g_snsMenuUpdateDetour.RevertAndClear(
                g_originalSnsMenuUpdate) && clean;
    return clean;
}

void ClearBindings() {
    g_originalSnsMenuCommand = nullptr;
    g_originalPauseBackupRefresh = nullptr;
    g_offlineSnapshotMenu.store(nullptr, std::memory_order_release);
    g_snsMenuClosing.store(false, std::memory_order_release);
}

bool HasOfflineSnapshotMenu() {
    return g_offlineSnapshotMenu.load(std::memory_order_acquire) != nullptr;
}

} // namespace novafix::game::lr_snapshot::outerworld_menu
