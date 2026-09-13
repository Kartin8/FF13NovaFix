#include "game/titles/lightning_returns/ui/text_editor/editor.h"

#include "diagnostics/log.h"
#include "game/steam/steam_keyboard.h"
#include "game/titles/lightning_returns/ui/prompt_mode_manager.h"
#include "game/titles/lightning_returns/ui/text_editor/native_api.h"
#include "game/titles/lightning_returns/ui/text_editor/game_text_converter.h"
#include "game/titles/lightning_returns/ui/text_editor/input_handler.h"
#include "game/titles/lightning_returns/ui/text_editor/session_context.h"
#include "game/titles/lightning_returns/ui/text_editor/message_window_adapter.h"
#include "input/win32_input.h"

#include <atomic>
#include <mutex>
#include <string>
#include <utility>

namespace novafix::game::lr_text_editor {
namespace {

SRWLOCK g_apiLock = SRWLOCK_INIT;
NativeApi g_api{};
std::mutex g_stateMutex;
detail::EditorState g_state;
bool g_opening{};
std::atomic_bool g_open{};
std::atomic_bool g_resultReady{};
thread_local unsigned g_backendRedirectDepth{};

NativeApi Api() {
    AcquireSRWLockShared(&g_apiLock);
    const NativeApi api = g_api;
    ReleaseSRWLockShared(&g_apiLock);
    return api;
}

void OpenControllerKeyboard(std::string_view initialText,
                            std::size_t maximumCharacters) {
    if (steam::keyboard::IsOpen() ||
        !steam::keyboard::Open(
            initialText, maximumCharacters)) {
        return;
    }

    std::scoped_lock lock(g_stateMutex);
    if (g_state.open &&
        !detail::SuppressProductVisuals(g_state, true)) {
        LogWarning("Could not suppress native text editor visuals: input=Steam keyboard");
    }
}

void* FinishLocked(bool submitted) {
    if (!g_state.open) return nullptr;
    g_state.submitted = submitted;
    g_state.resultUtf8.clear();
    if (submitted &&
        !detail::WideToUtf8(g_state.text, g_state.resultUtf8)) {
        g_state.submitted = false;
    }
    g_state.resultReady = true;
    g_state.open = false;
    g_open.store(false, std::memory_order_release);
    detail::BeginInputRelease(g_state);
    void* const product = g_state.product;
    g_state.product = nullptr;
    g_resultReady.store(true, std::memory_order_release);
    return product;
}

bool Open(
    const NativeApi& api, std::wstring_view initialText,
    std::size_t maximumCharacters) {
    if (!api.createSystemMessage || !api.setGuiLayer ||
        !api.gameStringAssign || !api.textLabelWrapper ||
        !maximumCharacters) {
        return false;
    }

    detail::EditorState editor;
    editor.text.assign(initialText);
    detail::TrimToMaximum(editor.text, maximumCharacters);
    editor.cursor = editor.text.size();
    editor.maximumCharacters = maximumCharacters;
    detail::InitializeInput(editor);

    {
        std::scoped_lock lock(g_stateMutex);
        if (g_state.open || g_state.resultReady || g_opening) return false;
        g_opening = true;
    }

    const auto releaseOpeningReservation = [] {
        std::scoped_lock lock(g_stateMutex);
        g_opening = false;
    };

    if (!detail::CreateProduct(api, editor)) {
        releaseOpeningReservation();
        return false;
    }

    editor.open = true;
    const bool requestControllerKeyboard =
        lr_prompts::CurrentMode() ==
        input::NativePromptMode::Controller;
    std::string controllerInitialText;
    const bool controllerInitialTextReady =
        requestControllerKeyboard &&
        detail::WideToUtf8(editor.text, controllerInitialText);
    const std::uint16_t previousButtons =
        detail::CurrentControllerButtons();
    {
        std::scoped_lock lock(g_stateMutex);
        g_state = std::move(editor);
        detail::SetPreviousControllerButtons(previousButtons);
        g_opening = false;
        g_open.store(true, std::memory_order_release);
    }
    if (controllerInitialTextReady) {
        OpenControllerKeyboard(controllerInitialText,
                               maximumCharacters);
    }
    return true;
}

} // namespace

void Configure(const NativeApi& api) {
    AcquireSRWLockExclusive(&g_apiLock);
    g_api = api;
    ReleaseSRWLockExclusive(&g_apiLock);
}

BackendRedirectScope::BackendRedirectScope() {
    ++g_backendRedirectDepth;
}

BackendRedirectScope::~BackendRedirectScope() {
    if (g_backendRedirectDepth) --g_backendRedirectDepth;
}

bool BackendRedirectRequested() {
    return g_backendRedirectDepth != 0u;
}

bool Available() {
    const NativeApi api = Api();
    return api.gameTextToWide && api.createSystemMessage &&
        api.setGuiLayer && api.gameWideToText &&
        api.gameStringAssign && api.textLabelWrapper;
}

bool OpenFromGameText(
    const char* initialText, std::size_t maximumCharacters) {
    const NativeApi api = Api();
    std::wstring initialWide;
    if (!detail::GameTextToWideString(api, initialText, initialWide)) {
        Log("Initial text conversion failed: opening empty editor");
        initialWide.clear();
    }
    return Open(api, initialWide, maximumCharacters);
}

bool TakeResult(std::string& text, bool& submitted) {
    if (!g_resultReady.load(std::memory_order_acquire)) return false;
    std::scoped_lock lock(g_stateMutex);
    if (!g_state.resultReady) return false;
    submitted = g_state.submitted;
    text = std::move(g_state.resultUtf8);
    g_state = {};
    g_resultReady.store(false, std::memory_order_release);
    return true;
}

bool ResultReady() {
    return g_resultReady.load(std::memory_order_acquire);
}

bool ConvertUtf8ToGameText(std::string_view source, std::string& text) {
    return detail::Utf8ToGameText(Api(), source, text);
}

void RefreshPromptGlyphs() {
    if (!g_open.load(std::memory_order_acquire)) return;
    std::string controllerInitialText;
    std::size_t maximumCharacters{};
    {
        std::scoped_lock lock(g_stateMutex);
        if (!g_state.open) return;
        g_state.refreshRequested = true;
        if (lr_prompts::CurrentMode() ==
                input::NativePromptMode::Controller &&
            detail::WideToUtf8(g_state.text,
                               controllerInitialText)) {
            maximumCharacters = g_state.maximumCharacters;
        }
        LogDebug(
            "Text editor prompt refresh queued: mode=%d",
            static_cast<int>(lr_prompts::CurrentMode()));
    }
    if (maximumCharacters) {
        OpenControllerKeyboard(controllerInitialText,
                               maximumCharacters);
    }
}

void Pump() {
    if (!g_open.load(std::memory_order_acquire)) {
        detail::UpdateInputReleaseLatch();
        return;
    }
    const NativeApi api = Api();
    const bool focused = input::GameInputHasFocus();

    std::string steamText;
    steam::keyboard::Result steamResult =
        steam::keyboard::TakeResult(steamText);
    std::wstring steamWide;
    if (steamResult ==
            steam::keyboard::Result::Submitted &&
        !detail::Utf8ToWide(steamText, steamWide)) {
        steamResult = steam::keyboard::Result::Failed;
    }

    void* productToClose{};
    detail::EditorState refreshState;
    bool refresh{};
    {
        std::scoped_lock lock(g_stateMutex);
        if (!g_state.open) return;

        if (steamResult !=
            steam::keyboard::Result::None) {
            g_state.releaseControllerButtons =
                detail::CurrentControllerButtons();
            if (steamResult ==
                steam::keyboard::Result::Submitted) {
                detail::TrimToMaximum(steamWide, g_state.maximumCharacters);
                g_state.text = std::move(steamWide);
                g_state.cursor = g_state.text.size();
                g_state.closeRequested = true;
                g_state.submitRequested = true;
            } else if (steamResult ==
                       steam::keyboard::Result::Canceled) {
                g_state.closeRequested = true;
                g_state.submitRequested = false;
            } else {
                detail::SuppressProductVisuals(g_state, false);
                detail::SetPreviousControllerButtons(
                    detail::CurrentControllerButtons());
                LogWarning("Steam keyboard result unavailable: native editor remains active");
            }
        }

        const ULONGLONG now = GetTickCount64();
        const bool atCharacterLimit =
            detail::UnicodeCharacterCount(g_state.text) >=
            g_state.maximumCharacters;
        if (!focused) {
            detail::ResetCursorBlink(g_state);
        } else if (!atCharacterLimit &&
                   now >= g_state.nextCursorBlink) {
            g_state.cursorVisible = !g_state.cursorVisible;
            g_state.nextCursorBlink =
                now + detail::kCursorBlinkIntervalMs;
            g_state.refreshRequested = true;
        }

        if (!focused) {
            detail::SetPreviousControllerButtons(
                detail::CurrentControllerButtons());
        }

        if (g_state.closeRequested) {
            productToClose = FinishLocked(g_state.submitRequested);
        } else if (g_state.refreshRequested) {
            g_state.refreshRequested = false;
            refreshState = g_state;
            refresh = true;
        }
    }

    if (productToClose) {
        detail::CloseProduct(productToClose);
    } else if (refresh) {
        detail::RefreshProduct(api, refreshState);
    }
}

bool IsOpen() {
    return g_open.load(std::memory_order_acquire);
}

bool CapturesInput() {
    return (IsOpen() || detail::InputReleasePending()) &&
        input::GameInputHasFocus();
}

void ObserveControllerState(
    unsigned userIndex, bool connected, std::uint16_t buttons) {
    if (!detail::RecordControllerState(userIndex, connected, buttons)) {
        return;
    }
    detail::UpdateInputReleaseLatch();
    if (!g_open.load(std::memory_order_acquire)) return;

    const bool focused = input::GameInputHasFocus();
    std::scoped_lock lock(g_stateMutex);
    if (!g_state.open) return;
    if (steam::keyboard::IsOpen()) {
        detail::SetPreviousControllerButtons(buttons);
        return;
    }
    detail::HandleControllerInput(
        g_state, focused, connected, buttons,
        lr_prompts::UsesOppositeConfirmLayout());
}

bool HandleInputMessage(const MSG& message, bool unicodeMessage) {
    if (!g_open.load(std::memory_order_acquire)) {
        return detail::ConsumeReleasedKeyboardMessage(message);
    }
    std::scoped_lock lock(g_stateMutex);
    if (!g_state.open) return false;
    if (steam::keyboard::IsOpen()) return true;
    return detail::HandleKeyboardMessage(
        g_state, message, unicodeMessage);
}

bool HandleInputLanguageMessage(const MSG& message) {
    if (!g_open.load(std::memory_order_acquire)) return false;

    HKL requested{};
    WPARAM requestFlags{};
    bool consume{};
    if (message.message == WM_INPUTLANGCHANGEREQUEST) {
        requested = reinterpret_cast<HKL>(message.lParam);
        requestFlags = message.wParam;
        consume = true;
    } else if (message.message == WM_ACTIVATEAPP &&
               message.wParam != FALSE && message.lParam != 0) {
        requested = GetKeyboardLayout(
            static_cast<DWORD>(message.lParam));
    } else {
        return false;
    }
    if (!requested || !message.hwnd) return false;

    const HKL previous = GetKeyboardLayout(0);
    if (requested != previous) {
        // LR ignores input-language requests; let USER32 activate the layout
        // and emit WM_INPUTLANGCHANGE
        DefWindowProcW(
            message.hwnd, WM_INPUTLANGCHANGEREQUEST, requestFlags,
            reinterpret_cast<LPARAM>(requested));

        const HKL active = GetKeyboardLayout(0);
        LogDebug(
            "Keyboard layout update: previous=%p requested=%p active=%p source=%s",
            static_cast<void*>(previous),
            static_cast<void*>(requested),
            static_cast<void*>(active),
            consume ? "request" : "activation");
    }

    {
        std::scoped_lock lock(g_stateMutex);
        if (g_state.open) {
            g_state.pendingHighSurrogate = 0;
            g_state.pendingAnsiLeadByte = 0;
        }
    }
    return consume;
}

} // namespace novafix::game::lr_text_editor
