#include "game/titles/lightning_returns/ui/window_menu_win32.h"

#include "diagnostics/log.h"
#include "compat/addons/plugin_controls.h"
#include "game/core/game_profile.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace novafix::game::lr_native_menu {
namespace {

constexpr UINT kFirstAddonCommand = 0xD000u;
constexpr UINT kLastAddonCommand = 0xDFFDu;
constexpr UINT kAddonParentCommand = 0xDFFEu;

HMENU g_rootMenu{};
HMENU g_addonMenu{};
HWND g_hookedWindow{};
WNDPROC g_originalWindowProc{};
HWND g_lastHookFailureWindow{};
DWORD g_lastHookFailureError{};

struct AddonCommand {
    UINT id{};
    HMENU ownerMenu{};
    size_t pluginIndex{};
    size_t menuIndex{};
    size_t itemIndex{};
};

std::vector<AddonCommand> g_addonCommands;

struct ConfirmLayoutLabels {
    std::wstring_view parent;
    std::wstring_view opposite;
    std::wstring_view normal;
    std::wstring_view oppositeReplacement;
    std::wstring_view normalReplacement;
};

constexpr std::array<ConfirmLayoutLabels, 7> kConfirmLayoutLabels{{
    {L"Confirm Button Assignment", L"Type 1", L"Type 2",
     L"A: Confirm / B: Cancel",
     L"B: Confirm / A: Cancel"},
    {L"Tastenbelegung bestätigen", L"Layout 1", L"Layout 2",
     L"A: Bestätigen / B: Abbrechen",
     L"B: Bestätigen / A: Abbrechen"},
    {L"Confirmer l'attribution des touches", L"Type 1", L"Type 2",
     L"A : Confirmer / B : Annuler",
     L"B : Confirmer / A : Annuler"},
    {L"Conferma la configurazione dei tasti", L"Tipo 1", L"Tipo 2",
     L"A: Conferma / B: Annulla",
     L"B: Conferma / A: Annulla"},
    {L"결정 버튼 배치", L"TYPE 1", L"TYPE 2", L"A: 결정 / B: 취소",
     L"B: 결정 / A: 취소"},
    {L"確定按鈕配置", L"第1型", L"第2型", L"A：確定 / B：取消",
     L"B：確定 / A：取消"},
    {L"決定ボタンの配置", L"タイプ１", L"タイプ２",
     L"A：決定 / B：キャンセル",
     L"B：決定 / A：キャンセル"},
}};

struct TopLevelMenuLabel {
    std::wstring_view original;
    std::wstring_view replacement;
};

constexpr std::array<TopLevelMenuLabel, 7> kGameMenuLabels{{
    {L"File", L"Game"},
    {L"Datei", L"Spiel"},
    {L"Fichier", L"Jeu"},
    {L"Cartella", L"Gioco"},
    {L"Archivo", L"Juego"},
    {L"파일", L"게임"},
    {L"檔案", L"遊戲"},
}};

bool HasNativeAddonMenus() {
    return compat::plugin_controls::HasNativeMenus();
}

std::wstring Wide(std::string_view value) {
    if (value.empty()) return {};
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) return std::wstring(value.begin(), value.end());
    std::wstring result(static_cast<size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), required);
    return result;
}

HMENU FindSubmenuByCommand(HMENU menu, UINT command) {
    if (!menu) return nullptr;
    const int count = GetMenuItemCount(menu);
    for (int position = 0; position < count; ++position) {
        MENUITEMINFOW item{};
        item.cbSize = sizeof(item);
        item.fMask = MIIM_ID | MIIM_SUBMENU;
        if (GetMenuItemInfoW(menu, static_cast<UINT>(position), TRUE, &item) &&
            item.wID == command) {
            return item.hSubMenu;
        }
    }
    return nullptr;
}

std::wstring MenuItemText(HMENU menu, UINT position) {
    const int length = GetMenuStringW(menu, position, nullptr, 0, MF_BYPOSITION);
    if (length <= 0) return {};
    std::wstring text(static_cast<size_t>(length) + 1u, L'\0');
    const int written = GetMenuStringW(
        menu, position, text.data(), static_cast<int>(text.size()),
        MF_BYPOSITION);
    text.resize(written > 0 ? static_cast<size_t>(written) : 0u);
    return text;
}

bool ReplaceConfirmLayoutLabels(HMENU menu, std::wstring_view parentLabel = {}) {
    if (!menu) return false;
    const int count = GetMenuItemCount(menu);
    if (count == 2) {
        const std::wstring first = MenuItemText(menu, 0);
        const std::wstring second = MenuItemText(menu, 1);
        for (const auto& labels : kConfirmLayoutLabels) {
            if (parentLabel != labels.parent || first != labels.opposite ||
                second != labels.normal) {
                continue;
            }
            MENUITEMINFOW item{};
            item.cbSize = sizeof(item);
            item.fMask = MIIM_STRING;
            item.dwTypeData = const_cast<wchar_t*>(
                labels.oppositeReplacement.data());
            const bool firstChanged =
                SetMenuItemInfoW(menu, 0, TRUE, &item) != FALSE;
            item.dwTypeData = const_cast<wchar_t*>(
                labels.normalReplacement.data());
            const bool secondChanged =
                SetMenuItemInfoW(menu, 1, TRUE, &item) != FALSE;
            if (firstChanged && secondChanged) {
                return true;
            }
            return false;
        }
    }
    for (int position = 0; position < count; ++position) {
        const HMENU child = GetSubMenu(menu, position);
        if (child && ReplaceConfirmLayoutLabels(
                         child, MenuItemText(menu, position))) {
            return true;
        }
    }
    return false;
}

bool ReplaceGameMenuLabel(HMENU root) {
    if (!root || GetMenuItemCount(root) <= 0) return false;
    const std::wstring current = MenuItemText(root, 0);
    std::wstring_view replacement;
    if (current == L"ファイル") {
        replacement = L"ゲーム";
    } else {
        const auto found = std::find_if(
            kGameMenuLabels.begin(), kGameMenuLabels.end(),
            [&current](const TopLevelMenuLabel& label) {
                return current == label.original;
            });
        if (found == kGameMenuLabels.end()) return false;
        replacement = found->replacement;
    }

    MENUITEMINFOW item{};
    item.cbSize = sizeof(item);
    item.fMask = MIIM_STRING;
    item.dwTypeData = const_cast<wchar_t*>(replacement.data());
    if (!SetMenuItemInfoW(root, 0, TRUE, &item)) return false;
    return true;
}

bool InitialAddonState(const compat::plugin_controls::Plugin& plugin,
                       const compat::plugin_controls::NativeMenuItem& source,
                       UINT* state) {
    if (!state) return false;
    uint32_t value = 0;
    const bool readable =
        compat::plugin_controls::ReadMenuState(plugin, source, &value);
    *state = readable && (value & NOVAFIX_PLUGIN_NATIVE_MENU_ENABLED)
                 ? MFS_ENABLED
                 : MFS_DISABLED | MFS_GRAYED;
    if (readable && (value & NOVAFIX_PLUGIN_NATIVE_MENU_CHECKED)) {
        *state |= MFS_CHECKED;
    }
    return readable;
}

bool InsertAddonItem(HMENU owner, UINT position, const std::wstring& label,
                     UINT state, UINT type, UINT command, HMENU submenu) {
    MENUITEMINFOW item{};
    item.cbSize = sizeof(item);
    item.fMask = MIIM_FTYPE | MIIM_STATE;
    item.fType = type;
    item.fState = state;
    if (type != MFT_SEPARATOR) {
        item.fMask |= MIIM_STRING;
        item.dwTypeData = const_cast<wchar_t*>(label.c_str());
    }
    if (submenu) {
        item.fMask |= MIIM_SUBMENU;
        item.hSubMenu = submenu;
    } else if (type != MFT_SEPARATOR) {
        item.fMask |= MIIM_ID;
        item.wID = command;
    }
    return InsertMenuItemW(owner, position, TRUE, &item) != FALSE;
}

void AddAddonMenuItems(
    HMENU pluginMenu, const compat::plugin_controls::Plugin& plugin,
    size_t pluginIndex, const compat::plugin_controls::NativeMenu& menu,
    size_t menuIndex, UINT& nextCommand) {
    std::vector<HMENU> submenuByItem(menu.items.size(), nullptr);
    for (size_t itemIndex = 0; itemIndex < menu.items.size(); ++itemIndex) {
        const auto& source = menu.items[itemIndex];
        HMENU owner = pluginMenu;
        if (source.parentIndex >= 0) {
            owner = submenuByItem[static_cast<size_t>(source.parentIndex)];
        }
        if (!owner) owner = pluginMenu;

        const UINT position = static_cast<UINT>(GetMenuItemCount(owner));
        UINT state = MFS_DISABLED | MFS_GRAYED;
        InitialAddonState(plugin, source, &state);
        const std::wstring label = Wide(source.label);

        if (source.type == NOVAFIX_PLUGIN_NATIVE_MENU_SEPARATOR) {
            InsertAddonItem(owner, position, {}, MFS_ENABLED,
                            MFT_SEPARATOR, 0u, nullptr);
            continue;
        }
        if (source.type == NOVAFIX_PLUGIN_NATIVE_MENU_SUBMENU) {
            HMENU child = CreatePopupMenu();
            if (!child || !InsertAddonItem(owner, position, label, state,
                                           MFT_STRING, 0u, child)) {
                if (child) DestroyMenu(child);
                continue;
            }
            submenuByItem[itemIndex] = child;
            continue;
        }
        if (nextCommand > kLastAddonCommand) {
            InsertAddonItem(owner, position, label,
                            MFS_DISABLED | MFS_GRAYED, MFT_STRING,
                            0u, nullptr);
            continue;
        }

        const UINT type =
            source.type == NOVAFIX_PLUGIN_NATIVE_MENU_RADIO
                ? MFT_STRING | MFT_RADIOCHECK
                : MFT_STRING;
        if (InsertAddonItem(owner, position, label, state, type,
                            nextCommand, nullptr)) {
            g_addonCommands.push_back({nextCommand, owner, pluginIndex,
                                       menuIndex, itemIndex});
            ++nextCommand;
        }
    }
}

bool AddAddonMenus(HMENU root) {
    if (!root || !HasNativeAddonMenus()) return true;
    HMENU master = CreatePopupMenu();
    if (!master) return false;

    g_addonCommands.clear();
    UINT nextCommand = kFirstAddonCommand;
    const auto& plugins = compat::plugin_controls::CurrentPlugins();
    for (size_t pluginIndex = 0; pluginIndex < plugins.size(); ++pluginIndex) {
        const auto& plugin = plugins[pluginIndex];
        for (size_t menuIndex = 0; menuIndex < plugin.nativeMenus.size();
             ++menuIndex) {
            const auto& menu = plugin.nativeMenus[menuIndex];
            HMENU pluginMenu = CreatePopupMenu();
            if (!pluginMenu) continue;
            AddAddonMenuItems(pluginMenu, plugin, pluginIndex, menu,
                              menuIndex, nextCommand);

            MENUITEMINFOW parent{};
            parent.cbSize = sizeof(parent);
            parent.fMask = MIIM_STRING | MIIM_SUBMENU;
            parent.hSubMenu = pluginMenu;
            std::wstring label = Wide(menu.label);
            parent.dwTypeData = label.data();
            const UINT position = static_cast<UINT>(
                GetMenuItemCount(master));
            if (!InsertMenuItemW(master, position, TRUE, &parent)) {
                DestroyMenu(pluginMenu);
            }
        }
    }

    MENUITEMINFOW parent{};
    parent.cbSize = sizeof(parent);
    parent.fMask = MIIM_ID | MIIM_STRING | MIIM_SUBMENU;
    parent.wID = kAddonParentCommand;
    parent.hSubMenu = master;
    parent.dwTypeData = const_cast<wchar_t*>(L"Add-ons");
    const UINT position = static_cast<UINT>(GetMenuItemCount(root));
    if (!InsertMenuItemW(root, position, TRUE, &parent)) {
        DestroyMenu(master);
        g_addonCommands.clear();
        return false;
    }
    g_addonMenu = master;
    Log("Add-on menu: menus=%d items=%zu",
        GetMenuItemCount(master), g_addonCommands.size());
    return true;
}

void RefreshAddonChecks() {
    const auto& plugins = compat::plugin_controls::CurrentPlugins();
    for (const AddonCommand& command : g_addonCommands) {
        if (command.pluginIndex >= plugins.size()) continue;
        const auto& plugin = plugins[command.pluginIndex];
        if (command.menuIndex >= plugin.nativeMenus.size()) continue;
        const auto& menu = plugin.nativeMenus[command.menuIndex];
        if (command.itemIndex >= menu.items.size()) continue;
        const auto& item = menu.items[command.itemIndex];
        UINT state = MFS_DISABLED | MFS_GRAYED;
        InitialAddonState(plugin, item, &state);
        EnableMenuItem(command.ownerMenu, command.id, MF_BYCOMMAND |
            ((state & (MFS_DISABLED | MFS_GRAYED)) ? MF_GRAYED : MF_ENABLED));
        CheckMenuItem(command.ownerMenu, command.id, MF_BYCOMMAND |
            ((state & MFS_CHECKED) ? MF_CHECKED : MF_UNCHECKED));
    }
}

bool ActivateAddonCommand(UINT id) {
    const auto found = std::find_if(
        g_addonCommands.begin(), g_addonCommands.end(),
        [id](const AddonCommand& item) { return item.id == id; });
    if (found == g_addonCommands.end()) return false;
    const auto& plugins = compat::plugin_controls::CurrentPlugins();
    if (found->pluginIndex >= plugins.size()) return true;
    const auto& plugin = plugins[found->pluginIndex];
    if (found->menuIndex >= plugin.nativeMenus.size()) return true;
    const auto& menu = plugin.nativeMenus[found->menuIndex];
    if (found->itemIndex >= menu.items.size()) return true;
    const auto& item = menu.items[found->itemIndex];
    uint32_t state = 0;
    if (compat::plugin_controls::ReadMenuState(plugin, item, &state) &&
        (state & NOVAFIX_PLUGIN_NATIVE_MENU_ENABLED)) {
        compat::plugin_controls::ActivateMenuItem(plugin, item);
    }
    RefreshAddonChecks();
    return true;
}

LRESULT CALLBACK AddonWindowProc(HWND window, UINT message, WPARAM wParam,
                                 LPARAM lParam) {
    if (message == WM_COMMAND && HIWORD(wParam) == 0 &&
        ActivateAddonCommand(LOWORD(wParam))) {
        return 0;
    }
    if (message == WM_INITMENUPOPUP && g_addonMenu) RefreshAddonChecks();
    const WNDPROC original = g_originalWindowProc;
    if (message == WM_NCDESTROY && window == g_hookedWindow) {
        // Clear handles before forwarding; re-entrant Pump must see teardown
        g_hookedWindow = nullptr;
        g_originalWindowProc = nullptr;
        g_rootMenu = nullptr;
        g_addonMenu = nullptr;
        g_addonCommands.clear();
    }
    const bool unicode = IsWindowUnicode(window) != FALSE;
    const LRESULT result = original
        ? (unicode ? CallWindowProcW(original, window, message, wParam, lParam)
                   : CallWindowProcA(original, window, message, wParam, lParam))
        : (unicode ? DefWindowProcW(window, message, wParam, lParam)
                   : DefWindowProcA(window, message, wParam, lParam));
    return result;
}

bool InstallWindowBridge(HWND window) {
    if (window == g_hookedWindow) return true;
    if (g_hookedWindow) {
        // Keep the old WNDPROC until WM_NCDESTROY; one trampoline cannot own two HWNDs
        if (IsWindow(g_hookedWindow)) return false;
        g_hookedWindow = nullptr;
        g_originalWindowProc = nullptr;
        g_rootMenu = nullptr;
        g_addonMenu = nullptr;
        g_addonCommands.clear();
    }
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR previous = SetWindowLongPtrW(
        window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&AddonWindowProc));
    if (!previous && GetLastError() != ERROR_SUCCESS) {
        const DWORD error = GetLastError();
        if (g_lastHookFailureWindow != window ||
            g_lastHookFailureError != error) {
            LogWarning("Add-on menu disabled: WndProc hook failed error=%lu; repeats suppressed until state changes",
                error);
            g_lastHookFailureWindow = window;
            g_lastHookFailureError = error;
        }
        return false;
    }
    g_lastHookFailureWindow = nullptr;
    g_lastHookFailureError = ERROR_SUCCESS;
    g_hookedWindow = window;
    g_originalWindowProc = reinterpret_cast<WNDPROC>(previous);
    return true;
}

bool PatchMenus(HWND window) {
    const HMENU root = window ? GetMenu(window) : nullptr;
    if (!root) return false;
    if (!InstallWindowBridge(window)) return false;
    ReplaceGameMenuLabel(root);
    ReplaceConfirmLayoutLabels(root);
    if (root != g_rootMenu ||
        FindSubmenuByCommand(root, kAddonParentCommand) != g_addonMenu) {
        g_addonMenu = nullptr;
        g_addonCommands.clear();
        if (!AddAddonMenus(root)) return false;
    }

    g_rootMenu = root;
    RefreshAddonChecks();
    DrawMenuBar(window);
    return true;
}

} // namespace
void Pump() {
    static const bool supported =
        CurrentProfile().title == Title::LightningReturns &&
        Supports(Capability::NativeMenuExtensions);
    if (!supported) return;

    static ULONGLONG nextProbe{};
    const ULONGLONG now = GetTickCount64();
    if (now < nextProbe) return;
    nextProbe = now + 250u;

    const HWND window = g_hookedWindow && IsWindow(g_hookedWindow)
        ? g_hookedWindow
        : FindWindowW(nullptr, L"LIGHTNING RETURNS: FINAL FANTASY XIII");
    if (!window) return;
    const HMENU root = GetMenu(window);
    const bool needsPatch =
        root != g_rootMenu ||
        (HasNativeAddonMenus() &&
         FindSubmenuByCommand(root, kAddonParentCommand) != g_addonMenu) ||
        g_hookedWindow != window;
    if (needsPatch) PatchMenus(window);
}

} // namespace novafix::game::lr_native_menu
