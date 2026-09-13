#include "game/titles/lightning_returns/ui/text_editor/input_handler.h"

#include "game/titles/lightning_returns/ui/text_editor/game_text_converter.h"

#include <xinput.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <string>

namespace novafix::game::lr_text_editor::detail {
namespace {

std::atomic_bool g_controllerConnected{};
std::atomic<std::uint16_t> g_controllerButtons{};
std::atomic_bool g_inputReleasePending{};
std::atomic_uint g_releaseKeyboardKey{};
std::atomic<std::uint16_t> g_releaseControllerButtons{};
std::uint16_t g_previousControllerButtons{};

UINT ActiveKeyboardAnsiCodePage() {
    const HKL layout = GetKeyboardLayout(0);
    const LANGID language = LOWORD(reinterpret_cast<ULONG_PTR>(layout));
    const LCID locale = MAKELCID(language, SORT_DEFAULT);
    std::array<wchar_t, 16> value{};
    if (GetLocaleInfoW(
            locale, LOCALE_IDEFAULTANSICODEPAGE, value.data(),
            static_cast<int>(value.size())) > 1) {
        UINT codePage = 0u;
        for (const wchar_t character : value) {
            if (character < L'0' || character > L'9') break;
            codePage = codePage * 10u +
                       static_cast<UINT>(character - L'0');
        }
        if (codePage != 0u) return codePage;
    }
    return GetACP();
}

} // namespace

void InitializeInput(EditorState& state) {
    state.cursorVisible = true;
    state.nextCursorBlink = GetTickCount64() + kCursorBlinkIntervalMs;
    state.returnKeyHeldOnOpen =
        (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
    state.escapeKeyHeldOnOpen =
        (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
}

void ResetCursorBlink(EditorState& state) {
    if (!state.cursorVisible) state.refreshRequested = true;
    state.cursorVisible = true;
    state.nextCursorBlink = GetTickCount64() + kCursorBlinkIntervalMs;
}

bool RecordControllerState(
    unsigned userIndex, bool connected, std::uint16_t buttons) {
    if (userIndex != 0u) return false;
    g_controllerButtons.store(
        connected ? buttons : 0u, std::memory_order_relaxed);
    g_controllerConnected.store(connected, std::memory_order_release);
    return true;
}

std::uint16_t CurrentControllerButtons() {
    return g_controllerConnected.load(std::memory_order_acquire)
        ? g_controllerButtons.load(std::memory_order_relaxed)
        : 0u;
}

void SetPreviousControllerButtons(std::uint16_t buttons) {
    g_previousControllerButtons = buttons;
}

void HandleControllerInput(
    EditorState& state, bool focused, bool connected,
    std::uint16_t buttons, bool oppositeConfirm) {
    if (!focused || !connected) {
        g_previousControllerButtons = connected ? buttons : 0u;
        return;
    }

    const std::uint16_t pressed = static_cast<std::uint16_t>(
        buttons & ~g_previousControllerButtons);
    g_previousControllerButtons = buttons;
    if (!pressed) return;

    ResetCursorBlink(state);
    const std::uint16_t submitButton = oppositeConfirm
        ? XINPUT_GAMEPAD_B : XINPUT_GAMEPAD_A;
    const std::uint16_t cancelButton = oppositeConfirm
        ? XINPUT_GAMEPAD_A : XINPUT_GAMEPAD_B;
    if (pressed & submitButton) {
        state.closeRequested = true;
        state.submitRequested = true;
        state.releaseControllerButtons = submitButton;
    } else if (pressed & cancelButton) {
        state.closeRequested = true;
        state.submitRequested = false;
        state.releaseControllerButtons = cancelButton;
    } else if ((pressed & XINPUT_GAMEPAD_DPAD_LEFT) && state.cursor > 0u) {
        --state.cursor;
        if (state.cursor > 0u &&
            IS_LOW_SURROGATE(state.text[state.cursor])) {
            --state.cursor;
        }
        state.refreshRequested = true;
    } else if ((pressed & XINPUT_GAMEPAD_DPAD_RIGHT) &&
               state.cursor < state.text.size()) {
        ++state.cursor;
        if (state.cursor < state.text.size() &&
            IS_LOW_SURROGATE(state.text[state.cursor])) {
            ++state.cursor;
        }
        state.refreshRequested = true;
    }
}

void BeginInputRelease(const EditorState& state) {
    g_releaseKeyboardKey.store(
        state.releaseKeyboardKey, std::memory_order_relaxed);
    g_releaseControllerButtons.store(
        state.releaseControllerButtons, std::memory_order_relaxed);
    g_inputReleasePending.store(
        state.releaseKeyboardKey != 0u ||
            state.releaseControllerButtons != 0u,
        std::memory_order_release);
    g_previousControllerButtons = 0u;
}

void UpdateInputReleaseLatch() {
    if (!g_inputReleasePending.load(std::memory_order_acquire)) return;

    const unsigned keyboardKey =
        g_releaseKeyboardKey.load(std::memory_order_relaxed);
    if (keyboardKey != 0u &&
        (GetAsyncKeyState(static_cast<int>(keyboardKey)) & 0x8000) == 0) {
        g_releaseKeyboardKey.store(0u, std::memory_order_relaxed);
    }

    const std::uint16_t controllerButtons =
        g_releaseControllerButtons.load(std::memory_order_relaxed);
    if (controllerButtons != 0u &&
        (g_controllerButtons.load(std::memory_order_relaxed) &
         controllerButtons) == 0u) {
        g_releaseControllerButtons.store(0u, std::memory_order_relaxed);
    }

    if (g_releaseKeyboardKey.load(std::memory_order_relaxed) == 0u &&
        g_releaseControllerButtons.load(std::memory_order_relaxed) == 0u) {
        g_inputReleasePending.store(false, std::memory_order_release);
    }
}

bool InputReleasePending() {
    return g_inputReleasePending.load(std::memory_order_acquire);
}

bool ConsumeReleasedKeyboardMessage(const MSG& message) {
    if (!InputReleasePending() ||
        (message.message != WM_KEYUP &&
         message.message != WM_SYSKEYUP) ||
        message.wParam !=
            g_releaseKeyboardKey.load(std::memory_order_relaxed)) {
        return false;
    }
    g_releaseKeyboardKey.store(0u, std::memory_order_relaxed);
    UpdateInputReleaseLatch();
    return true;
}

bool HandleKeyboardMessage(
    EditorState& state, const MSG& message, bool unicodeMessage) {
    ResetCursorBlink(state);

    if (message.message == WM_KEYUP || message.message == WM_SYSKEYUP) {
        if (message.wParam == VK_RETURN) {
            state.returnKeyHeldOnOpen = false;
        } else if (message.wParam == VK_ESCAPE) {
            state.escapeKeyHeldOnOpen = false;
        }
        return true;
    }

    if (message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN) {
        switch (message.wParam) {
        case VK_RETURN:
            if (!state.returnKeyHeldOnOpen) {
                state.closeRequested = true;
                state.submitRequested = true;
                state.releaseKeyboardKey = VK_RETURN;
            }
            return true;
        case VK_ESCAPE:
            if (!state.escapeKeyHeldOnOpen) {
                state.closeRequested = true;
                state.submitRequested = false;
                state.releaseKeyboardKey = VK_ESCAPE;
            }
            return true;
        case VK_LEFT:
            if (state.cursor > 0u) {
                --state.cursor;
                if (state.cursor > 0u &&
                    IS_LOW_SURROGATE(state.text[state.cursor])) {
                    --state.cursor;
                }
                state.refreshRequested = true;
            }
            return true;
        case VK_RIGHT:
            if (state.cursor < state.text.size()) {
                ++state.cursor;
                if (state.cursor < state.text.size() &&
                    IS_LOW_SURROGATE(state.text[state.cursor])) {
                    ++state.cursor;
                }
                state.refreshRequested = true;
            }
            return true;
        case VK_HOME:
            state.cursor = 0u;
            state.refreshRequested = true;
            return true;
        case VK_END:
            state.cursor = state.text.size();
            state.refreshRequested = true;
            return true;
        case VK_BACK:
            if (state.cursor > 0u) {
                std::size_t begin = state.cursor - 1u;
                if (begin > 0u && IS_LOW_SURROGATE(state.text[begin]) &&
                    IS_HIGH_SURROGATE(state.text[begin - 1u])) {
                    --begin;
                }
                state.text.erase(begin, state.cursor - begin);
                state.cursor = begin;
                state.refreshRequested = true;
            }
            return true;
        case VK_DELETE:
            if (state.cursor < state.text.size()) {
                std::size_t length = 1u;
                if (IS_HIGH_SURROGATE(state.text[state.cursor]) &&
                    state.cursor + 1u < state.text.size() &&
                    IS_LOW_SURROGATE(state.text[state.cursor + 1u])) {
                    length = 2u;
                }
                state.text.erase(state.cursor, length);
                state.refreshRequested = true;
            }
            return true;
        default:
            return false;
        }
    }

    if (message.message != WM_CHAR && message.message != WM_SYSCHAR) {
        return true;
    }

    std::wstring insertion;
    if (unicodeMessage) {
        const wchar_t character = static_cast<wchar_t>(message.wParam);
        if (character < L' ' || character == 0x7Fu) return true;
        if (IS_HIGH_SURROGATE(character)) {
            state.pendingHighSurrogate = character;
            return true;
        }
        if (IS_LOW_SURROGATE(character) && state.pendingHighSurrogate) {
            insertion.push_back(state.pendingHighSurrogate);
            insertion.push_back(character);
        } else if (!IS_LOW_SURROGATE(character)) {
            insertion.push_back(character);
        }
        state.pendingHighSurrogate = 0;
        state.pendingAnsiLeadByte = 0;
    } else {
        const char byte = static_cast<char>(message.wParam & 0xFFu);
        const UINT codePage = ActiveKeyboardAnsiCodePage();
        std::array<char, 2> encoded{};
        int byteCount = 1;
        if (state.pendingAnsiLeadByte) {
            encoded[0] = state.pendingAnsiLeadByte;
            encoded[1] = byte;
            byteCount = 2;
            state.pendingAnsiLeadByte = 0;
        } else if (IsDBCSLeadByteEx(codePage, static_cast<BYTE>(byte))) {
            state.pendingAnsiLeadByte = byte;
            return true;
        } else {
            encoded[0] = byte;
        }

        std::array<wchar_t, 2> decoded{};
        int characters = MultiByteToWideChar(
            codePage, MB_ERR_INVALID_CHARS, encoded.data(), byteCount,
            decoded.data(), static_cast<int>(decoded.size()));
        if (characters <= 0) {
            characters = MultiByteToWideChar(
                codePage, 0, encoded.data(), byteCount, decoded.data(),
                static_cast<int>(decoded.size()));
        }
        if (characters <= 0) return true;
        insertion.assign(decoded.data(), static_cast<std::size_t>(characters));
        state.pendingHighSurrogate = 0;
    }

    if (insertion.size() == 1u &&
        (insertion[0] < L' ' || insertion[0] == 0x7Fu)) {
        return true;
    }
    if (insertion.empty() ||
        UnicodeCharacterCount(state.text) >= state.maximumCharacters) {
        return true;
    }
    state.text.insert(state.cursor, insertion);
    state.cursor += insertion.size();
    state.refreshRequested = true;
    return true;
}

} // namespace novafix::game::lr_text_editor::detail
