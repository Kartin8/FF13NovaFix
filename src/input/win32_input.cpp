#include "input/win32_input.h"

#include "diagnostics/hook_profiler.h"
#include "diagnostics/log.h"
#include "patch/iat_hook.h"
#include "display/display_profile.h"
#include "game/core/features.h"
#include "game/titles/lightning_returns/ui/text_editor/editor.h"
#include "input/activity.h"
#include "input/direct_input/direct_input.h"
#include "overlay/input/overlay_hotkey.h"
#include "overlay/overlay.h"
#include "settings/input_settings.h"

#include <windows.h>
#include <dbt.h>

#include <atomic>

#include "imgui.h"

namespace novafix::input {
namespace {

using ClipCursorFn = BOOL (WINAPI*)(const RECT*);
using SetCursorPosFn = BOOL (WINAPI*)(int, int);
using GetCursorPosFn = BOOL (WINAPI*)(LPPOINT);
using SetCaptureFn = HWND (WINAPI*)(HWND);
using ShowCursorFn = int (WINAPI*)(BOOL);
using GetAsyncKeyStateFn = SHORT (WINAPI*)(int);
using PeekMessageAFn = BOOL (WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);
using PeekMessageWFn = BOOL (WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);

ClipCursorFn g_gameClipCursor = nullptr;
SetCursorPosFn g_gameSetCursorPos = nullptr;
GetCursorPosFn g_gameGetCursorPos = nullptr;
SetCaptureFn g_gameSetCapture = nullptr;
ShowCursorFn g_gameShowCursor = nullptr;
GetAsyncKeyStateFn g_gameGetAsyncKeyState = nullptr;
PeekMessageAFn g_gamePeekMessageA = nullptr;
PeekMessageWFn g_gamePeekMessageW = nullptr;

std::atomic<LONG> g_frozenCursorX{0};
std::atomic<LONG> g_frozenCursorY{0};
std::atomic<LONG> g_pendingWheel{0};
bool g_lastLeft = false;
bool g_lastRight = false;
bool g_lastMiddle = false;
bool g_overlayNativeCursorVisible = false;
int g_overlayShowCursorBalance = 0;

bool ShouldSuppressGameInput(bool windowKnown, bool foregroundOwnedByGame,
                             bool iconic, bool overlayCapturesInput) {
    if (overlayCapturesInput) return true;
    if (!windowKnown) return false;
    return iconic || !foregroundOwnedByGame;
}

bool MustReleaseMouse() {
    return GameInputSuppressed();
}

bool IsInputMessage(UINT message) {
    return (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
           (message >= WM_KEYFIRST && message <= WM_KEYLAST);
}

bool IsKeyboardMessage(UINT message) {
    return message >= WM_KEYFIRST && message <= WM_KEYLAST;
}

bool IsConfiguredOverlayHotkey(int key) {
    switch (key) {
    case VK_F8:
    case VK_F9:
    case VK_F10:
    case VK_F11:
    case VK_F12:
    case VK_OEM_3:
    case VK_INSERT:
    case VK_HOME:
        return IsOverlayHotkeyVirtualKey(key, settings::ActiveInput());
    default:
        return false;
    }
}

bool IsOverlayHotkeyMessage(const MSG& message) {
    return (message.message == WM_KEYDOWN || message.message == WM_KEYUP ||
            message.message == WM_SYSKEYDOWN || message.message == WM_SYSKEYUP) &&
           IsConfiguredOverlayHotkey(static_cast<int>(message.wParam));
}

bool IsApplicationCloseAttempt(const MSG& message) {
    if (message.message == WM_CLOSE || message.message == WM_QUERYENDSESSION ||
        message.message == WM_ENDSESSION || message.message == WM_QUIT) {
        return true;
    }
    if (message.message == WM_SYSCOMMAND &&
        (message.wParam & 0xFFF0u) == SC_CLOSE) {
        return true;
    }
    if (message.message == WM_NCLBUTTONDOWN && message.wParam == HTCLOSE) {
        return true;
    }
    return message.message == WM_SYSKEYDOWN && message.wParam == VK_F4 &&
           (message.lParam & (1ll << 29)) != 0;
}

bool IsControllerTopologyMessage(const MSG& message) {
    if (message.message == WM_INPUT_DEVICE_CHANGE) return true;
    if (message.message != WM_DEVICECHANGE) return false;

    return message.wParam == DBT_DEVICEARRIVAL ||
           message.wParam == DBT_DEVICEREMOVECOMPLETE ||
           message.wParam == DBT_DEVNODES_CHANGED;
}

bool IsInputLanguageTransition(const MSG& message) {
    return message.message == WM_INPUTLANGCHANGEREQUEST ||
           (message.message == WM_ACTIVATEAPP &&
            message.wParam != FALSE);
}

BOOL WINAPI GameClipCursor(const RECT* rectangle) {
    if (!g_gameClipCursor) return FALSE;
    return MustReleaseMouse() ? g_gameClipCursor(nullptr) : g_gameClipCursor(rectangle);
}

BOOL WINAPI GameSetCursorPos(int x, int y) {
    if (!g_gameSetCursorPos) return FALSE;
    return MustReleaseMouse() ? TRUE : g_gameSetCursorPos(x, y);
}

BOOL WINAPI GameGetCursorPos(LPPOINT point) {
    if (!g_gameGetCursorPos || !point) return FALSE;
    if (GameInputSuppressed()) {
        point->x = g_frozenCursorX.load(std::memory_order_relaxed);
        point->y = g_frozenCursorY.load(std::memory_order_relaxed);
        return TRUE;
    }

    const BOOL result = g_gameGetCursorPos(point);
    if (result) {
        g_frozenCursorX.store(point->x, std::memory_order_relaxed);
        g_frozenCursorY.store(point->y, std::memory_order_relaxed);
    }
    return result;
}

HWND WINAPI GameSetCapture(HWND window) {
    if (!g_gameSetCapture) return nullptr;
    return GameInputSuppressed() ? GetCapture() : g_gameSetCapture(window);
}

int WINAPI GameShowCursor(BOOL show) {
    if (!g_gameShowCursor) return 0;
    if (overlay::WantsInput()) {
        // End the game's normalization loop without touching USER32's cursor counter
        return show ? 0 : -1;
    }
    // Preserve USER32's counter; returning zero here would hang the game's
    // while (ShowCursor(FALSE) >= 0) loop when unfocused
    return g_gameShowCursor(show);
}

SHORT WINAPI GameGetAsyncKeyState(int key) {
    if (!g_gameGetAsyncKeyState) return 0;
    if (GameInputSuppressed() || IsConfiguredOverlayHotkey(key)) return 0;
    return g_gameGetAsyncKeyState(key);
}

template <typename PeekMessageFn>
BOOL ProcessPeekMessage(PeekMessageFn original, LPMSG message, HWND window,
                        UINT minimum, UINT maximum, UINT remove,
                        bool unicodeMessages) {
    NOVAFIX_PROFILE_SAMPLED_NAMED_SCOPE(
        totalTiming, "hook.win32.peek-message-total", 16u);
    if (!original || !message) return FALSE;
    BOOL result = FALSE;
    {
        NOVAFIX_PROFILE_SAMPLED_NAMED_SCOPE(
            originalTiming, "hook.win32.peek-message-original", 16u);
        result = original(message, window, minimum, maximum, remove);
    }
    if (!result) return FALSE;

    // Shut down ImGui before the game begins D3D teardown
    if (IsApplicationCloseAttempt(*message)) {
        overlay::PrepareForApplicationClose();
        return TRUE;
    }

    if (IsInputLanguageTransition(*message) &&
        game::lr_text_editor::HandleInputLanguageMessage(*message)) {
        if ((remove & PM_REMOVE) == 0) {
            MSG discarded{};
            original(&discarded, window, message->message,
                     message->message, PM_REMOVE);
        }
        return FALSE;
    }

    if (IsControllerTopologyMessage(*message)) {
        game::features::NotifyDeviceTopologyChanged();
        overlay::ForwardWin32InputMessage(
            message->hwnd, message->message,
            message->wParam, message->lParam);
    }
    if (message->message == WM_DISPLAYCHANGE || message->message == WM_DPICHANGED ||
        (message->message == WM_ACTIVATEAPP && message->wParam != FALSE)) {
        display::NotifyWindowEnvironmentChanged();
    }

    const bool deactivated =
        (message->message == WM_ACTIVATEAPP && message->wParam == FALSE) ||
        (message->message == WM_ACTIVATE && LOWORD(message->wParam) == WA_INACTIVE);
    const HWND gameWindow = overlay::PresentationWindow();
    if (deactivated || (gameWindow && GetForegroundWindow() != gameWindow)) {
        overlay::CloseForFocusLoss();
    }

    if (message->message == WM_ACTIVATEAPP) {
        NotifyDirectInputFocusChanged(message->wParam != FALSE);
    } else if (message->message == WM_ACTIVATE) {
        NotifyDirectInputFocusChanged(LOWORD(message->wParam) != WA_INACTIVE);
    }

    const bool nativeTextInput =
        game::lr_text_editor::CapturesInput() &&
        IsKeyboardMessage(message->message);
    const bool overlayInput = overlay::CapturesInput() && IsInputMessage(message->message);
    const bool backgroundInput = !GameInputHasFocus() && IsInputMessage(message->message);
    if (!nativeTextInput && !overlayInput && !backgroundInput &&
        !IsOverlayHotkeyMessage(*message)) return TRUE;

    if (nativeTextInput) {
        if ((message->message == WM_KEYDOWN ||
             message->message == WM_SYSKEYDOWN) &&
            !IsOverlayHotkeyMessage(*message)) {
            // Report keyboard activity before the text editor suppresses DirectInput
            NotifyActivity(InputSource::KeyboardMouse);
        }
        const bool handled =
            game::lr_text_editor::HandleInputMessage(
                *message, unicodeMessages);
        if (!handled &&
            (message->message == WM_KEYDOWN ||
             message->message == WM_SYSKEYDOWN)) {
            TranslateMessage(message);
        }
        if ((remove & PM_REMOVE) == 0) {
            MSG discarded{};
            original(&discarded, window, message->message, message->message,
                     PM_REMOVE);
        }
        return FALSE;
    }

    if (overlayInput && IsKeyboardMessage(message->message) &&
        !IsOverlayHotkeyMessage(*message)) {
        const HWND target = message->hwnd ? message->hwnd : gameWindow;
        overlay::ForwardWin32InputMessage(
            target, message->message, message->wParam, message->lParam);

        // Generate WM_CHAR here because suppressed keys never reach the game loop
        if (message->message == WM_KEYDOWN ||
            message->message == WM_SYSKEYDOWN) {
            TranslateMessage(message);
        }
    }
    if (overlayInput && message->message == WM_MOUSEWHEEL) {
        g_pendingWheel.fetch_add(static_cast<SHORT>(HIWORD(message->wParam)), std::memory_order_relaxed);
    }

    if ((remove & PM_REMOVE) == 0) {
        MSG discarded{};
        original(&discarded, window, message->message, message->message, PM_REMOVE);
    }
    return FALSE;
}

BOOL WINAPI GamePeekMessageA(LPMSG message, HWND window, UINT minimum, UINT maximum, UINT remove) {
    return ProcessPeekMessage(
        g_gamePeekMessageA, message, window, minimum, maximum, remove,
        false);
}

BOOL WINAPI GamePeekMessageW(LPMSG message, HWND window, UINT minimum, UINT maximum, UINT remove) {
    return ProcessPeekMessage(
        g_gamePeekMessageW, message, window, minimum, maximum, remove,
        true);
}

} // namespace

bool GameInputHasFocus() {
    const HWND window = overlay::PresentationWindow();
    if (!window || !IsWindow(window)) return true;
    if (IsIconic(window)) return false;
    const HWND foreground = GetForegroundWindow();
    if (!foreground) return false;
    const HWND gameOwner = GetAncestor(window, GA_ROOTOWNER);
    const HWND foregroundOwner = GetAncestor(foreground, GA_ROOTOWNER);
    return gameOwner && gameOwner == foregroundOwner;
}

bool GameInputSuppressed() {
    const HWND window = overlay::PresentationWindow();
    if (!window || !IsWindow(window)) {
        return ShouldSuppressGameInput(false, true, false, false);
    }

    // Use the same foreground state for Win32 and DirectInput
    const bool iconic = IsIconic(window) != FALSE;
    const HWND foreground = GetForegroundWindow();
    bool foregroundOwnedByGame = false;
    if (!iconic && foreground) {
        const HWND gameOwner = GetAncestor(window, GA_ROOTOWNER);
        const HWND foregroundOwner = GetAncestor(foreground, GA_ROOTOWNER);
        foregroundOwnedByGame = gameOwner && gameOwner == foregroundOwner;
    }
    const bool overlayCapturesInput =
        (overlay::WantsInput() ||
         game::lr_text_editor::CapturesInput()) &&
        !iconic && foreground == window;
    return ShouldSuppressGameInput(
        true, foregroundOwnedByGame, iconic, overlayCapturesInput);
}

void InstallWin32Hooks() {
    PatchExecutableImport("USER32.dll", "ClipCursor", reinterpret_cast<void*>(&GameClipCursor),
                          reinterpret_cast<void**>(&g_gameClipCursor));
    PatchExecutableImport("USER32.dll", "SetCursorPos", reinterpret_cast<void*>(&GameSetCursorPos),
                          reinterpret_cast<void**>(&g_gameSetCursorPos));
    PatchExecutableImport("USER32.dll", "GetCursorPos", reinterpret_cast<void*>(&GameGetCursorPos),
                          reinterpret_cast<void**>(&g_gameGetCursorPos));
    PatchExecutableImport("USER32.dll", "SetCapture", reinterpret_cast<void*>(&GameSetCapture),
                          reinterpret_cast<void**>(&g_gameSetCapture));
    PatchExecutableImport("USER32.dll", "ShowCursor", reinterpret_cast<void*>(&GameShowCursor),
                          reinterpret_cast<void**>(&g_gameShowCursor));
    PatchExecutableImport("USER32.dll", "GetAsyncKeyState", reinterpret_cast<void*>(&GameGetAsyncKeyState),
                          reinterpret_cast<void**>(&g_gameGetAsyncKeyState));
    PatchExecutableImport("USER32.dll", "PeekMessageA", reinterpret_cast<void*>(&GamePeekMessageA),
                          reinterpret_cast<void**>(&g_gamePeekMessageA));
    PatchExecutableImport("USER32.dll", "PeekMessageW", reinterpret_cast<void*>(&GamePeekMessageW),
                          reinterpret_cast<void**>(&g_gamePeekMessageW));
}

void FeedOverlayMouse() {
    ImGuiIO& io = ImGui::GetIO();
    POINT cursor{};
    const HWND gameWindow = overlay::PresentationWindow();
    if (GetCursorPos(&cursor) && gameWindow && ScreenToClient(gameWindow, &cursor)) {
        io.AddMousePosEvent(static_cast<float>(cursor.x), static_cast<float>(cursor.y));
    }

    const bool left = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool right = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    const bool middle = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
    if (left != g_lastLeft) io.AddMouseButtonEvent(0, left);
    if (right != g_lastRight) io.AddMouseButtonEvent(1, right);
    if (middle != g_lastMiddle) io.AddMouseButtonEvent(2, middle);
    const LONG wheel = g_pendingWheel.exchange(0, std::memory_order_relaxed);
    if (wheel) io.AddMouseWheelEvent(0.0f, static_cast<float>(wheel) / static_cast<float>(WHEEL_DELTA));
    g_lastLeft = left;
    g_lastRight = right;
    g_lastMiddle = middle;
}

void SetOverlayNativeCursorVisible(bool visible) {
    if (visible == g_overlayNativeCursorVisible) return;

    if (visible) {
        // Track ShowCursor increments so hiding the overlay restores the original counter
        constexpr int kMaximumCounterAdjustments = 64;
        g_overlayShowCursorBalance = 0;
        int counter = -1;
        do {
            counter = ::ShowCursor(TRUE);
            ++g_overlayShowCursorBalance;
        } while (counter < 0 &&
                 g_overlayShowCursorBalance < kMaximumCounterAdjustments);

        ::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
        g_overlayNativeCursorVisible = true;
        return;
    }

    for (int adjustment = 0; adjustment < g_overlayShowCursorBalance;
         ++adjustment) {
        ::ShowCursor(FALSE);
    }
    g_overlayShowCursorBalance = 0;
    g_overlayNativeCursorVisible = false;
}

} // namespace novafix::input
