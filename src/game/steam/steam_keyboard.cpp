#include "game/steam/steam_keyboard.h"

#include "common/guarded_call.h"
#include "diagnostics/log.h"

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace novafix::game::steam::keyboard {
namespace {

constexpr int kGamepadTextInputDismissedCallback = 714;
constexpr std::uint32_t kMaximumReturnedBytes = 4096u;
static_assert(sizeof(void*) == 4u);

struct GamepadTextInputDismissed {
    bool submitted{};
    std::uint8_t padding[3]{};
    std::uint32_t submittedTextBytes{};
};
static_assert(sizeof(GamepadTextInputDismissed) == 8u);

class CallbackBase {
public:
    virtual void Run(void* parameter) = 0;
    virtual void Run(void* parameter, bool ioFailure,
                     std::uint64_t apiCall) = 0;
    virtual int ParameterBytes() = 0;

    std::uint8_t flags{};
    std::uint8_t padding[3]{};
    int callback{};
};
// steam_api.dll writes the registration flag directly into this SDK object
// Keep the old 32-bit Steamworks callback layout used by LR
static_assert(sizeof(CallbackBase) == 12u);

using SteamUtilsFn = void* (__cdecl*)();
using RegisterCallbackFn = void (__cdecl*)(CallbackBase*, int);
using ShowGamepadTextInputFn = bool (__cdecl*)(
    void*, int, int, const char*, std::uint32_t, const char*);
using GetEnteredGamepadTextLengthFn = std::uint32_t (__cdecl*)(void*);
using GetEnteredGamepadTextInputFn = bool (__cdecl*)(
    void*, char*, std::uint32_t);

struct Api {
    void* utils{};
    RegisterCallbackFn registerCallback{};
    ShowGamepadTextInputFn showGamepadTextInput{};
    GetEnteredGamepadTextLengthFn enteredTextLength{};
    GetEnteredGamepadTextInputFn enteredText{};
};

void* SafeSteamUtils(SteamUtilsFn function) noexcept {
    return guarded_call::ResultOr<void*>([&] { return function(); }, nullptr);
}

bool SafeRegisterCallback(RegisterCallbackFn function,
                          CallbackBase* callback) noexcept {
    return guarded_call::Run([&] {
        function(callback, kGamepadTextInputDismissedCallback);
    });
}

bool SafeShowKeyboard(ShowGamepadTextInputFn function, void* utils,
                      std::uint32_t maximumCharacters,
                      const char* existingText) noexcept {
    return guarded_call::ResultOr<bool>([&] {
        return function(utils, 0, 0, "", maximumCharacters,
                        existingText);
    }, false);
}

std::uint32_t SafeEnteredTextLength(
    GetEnteredGamepadTextLengthFn function, void* utils) noexcept {
    return guarded_call::ResultOr<std::uint32_t>([&] {
        return function(utils);
    }, 0u);
}

bool SafeEnteredText(GetEnteredGamepadTextInputFn function, void* utils,
                     char* text, std::uint32_t bytes) noexcept {
    return guarded_call::ResultOr<bool>([&] {
        return function(utils, text, bytes);
    }, false);
}

std::mutex g_stateMutex;
Api g_api{};
bool g_callbackRegistered{};
bool g_resultReady{};
Result g_result{Result::None};
std::string g_resultText;
std::atomic_bool g_open{};

void StoreResult(Result result, std::string text = {}) {
    std::scoped_lock lock(g_stateMutex);
    if (!g_open.exchange(false, std::memory_order_acq_rel)) return;
    g_result = result;
    g_resultText = std::move(text);
    g_resultReady = true;
}

class DismissedCallback final : public CallbackBase {
public:
    DismissedCallback() {
        callback = kGamepadTextInputDismissedCallback;
    }

    void Run(void* parameter) override {
        const auto* dismissed =
            static_cast<const GamepadTextInputDismissed*>(parameter);
        if (!dismissed) {
            StoreResult(Result::Failed);
            return;
        }
        if (!dismissed->submitted) {
            StoreResult(Result::Canceled);
            return;
        }

        Api api;
        {
            std::scoped_lock lock(g_stateMutex);
            api = g_api;
        }
        if (!api.utils || !api.enteredTextLength || !api.enteredText) {
            StoreResult(Result::Failed);
            return;
        }

        const std::uint32_t bytes = SafeEnteredTextLength(
            api.enteredTextLength, api.utils);
        if (!bytes || bytes > kMaximumReturnedBytes ||
            (dismissed->submittedTextBytes &&
             bytes != dismissed->submittedTextBytes)) {
            StoreResult(Result::Failed);
            return;
        }

        std::vector<char> buffer(bytes, '\0');
        const bool received = SafeEnteredText(
            api.enteredText, api.utils, buffer.data(), bytes);
        if (!received || buffer.back() != '\0') {
            StoreResult(Result::Failed);
            return;
        }
        StoreResult(Result::Submitted, std::string(buffer.data()));
    }

    void Run(void* parameter, bool ioFailure,
             std::uint64_t) override {
        if (ioFailure) {
            StoreResult(Result::Failed);
        } else {
            Run(parameter);
        }
    }

    int ParameterBytes() override {
        return static_cast<int>(sizeof(GamepadTextInputDismissed));
    }
};

DismissedCallback g_callback;

template <typename T>
T Export(HMODULE module, const char* name) {
    return reinterpret_cast<T>(GetProcAddress(module, name));
}

bool ResolveApiLocked() {
    if (g_api.utils && g_callbackRegistered) return true;

    const HMODULE module = GetModuleHandleW(L"steam_api.dll");
    if (!module) return false;
    const SteamUtilsFn steamUtils =
        Export<SteamUtilsFn>(module, "SteamUtils");
    Api api;
    api.registerCallback = Export<RegisterCallbackFn>(
        module, "SteamAPI_RegisterCallback");
    api.showGamepadTextInput = Export<ShowGamepadTextInputFn>(
        module, "SteamAPI_ISteamUtils_ShowGamepadTextInput");
    api.enteredTextLength = Export<GetEnteredGamepadTextLengthFn>(
        module, "SteamAPI_ISteamUtils_GetEnteredGamepadTextLength");
    api.enteredText = Export<GetEnteredGamepadTextInputFn>(
        module, "SteamAPI_ISteamUtils_GetEnteredGamepadTextInput");
    if (!steamUtils || !api.registerCallback ||
        !api.showGamepadTextInput || !api.enteredTextLength ||
        !api.enteredText) {
        return false;
    }

    api.utils = SafeSteamUtils(steamUtils);
    if (!api.utils) return false;

    if (!g_callbackRegistered) {
        g_callbackRegistered = SafeRegisterCallback(
            api.registerCallback, &g_callback);
    }
    if (!g_callbackRegistered) return false;
    g_api = api;
    return true;
}

} // namespace

bool Open(std::string_view initialText, std::size_t maximumCharacters) {
    if (!maximumCharacters ||
        maximumCharacters >
            std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }

    Api api;
    {
        std::scoped_lock lock(g_stateMutex);
        if (g_open.load(std::memory_order_acquire) ||
            !ResolveApiLocked()) {
            return false;
        }
        g_resultReady = false;
        g_result = Result::None;
        g_resultText.clear();
        g_open.store(true, std::memory_order_release);
        api = g_api;
    }

    const std::string existing(initialText);
    const bool opened = SafeShowKeyboard(
        api.showGamepadTextInput, api.utils,
        static_cast<std::uint32_t>(maximumCharacters),
        existing.c_str());
    if (!opened) {
        std::scoped_lock lock(g_stateMutex);
        g_open.store(false, std::memory_order_release);
        LogDebug("Steam gamepad text input unavailable");
        return false;
    }
    return true;
}

bool IsOpen() {
    return g_open.load(std::memory_order_acquire);
}

Result TakeResult(std::string& text) {
    text.clear();
    std::scoped_lock lock(g_stateMutex);
    if (!g_resultReady) return Result::None;
    const Result result = g_result;
    text = std::move(g_resultText);
    g_resultReady = false;
    g_result = Result::None;
    return result;
}

} // namespace novafix::game::steam::keyboard
