#include "compat/addons/native_notifications.h"
#include "compat/addons/native_notification_text.h"

#include "common/guarded_call.h"
#include "diagnostics/log.h"
#include "input/xinput_backend.h"

#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>

namespace novafix::compat::native_notifications {
namespace {

constexpr std::size_t kMaximumMessageBytes =
    NOVAFIX_PLUGIN_MAX_NATIVE_MESSAGE_BYTES;
constexpr std::size_t kMaximumQueuedMessages =
    NOVAFIX_PLUGIN_MAX_NATIVE_MESSAGE_QUEUE;
constexpr std::uint32_t kMaximumTimeMs =
    NOVAFIX_PLUGIN_MAX_NATIVE_MESSAGE_TIME_MS;
constexpr ULONGLONG kCloseSettleMs = 250u;
constexpr std::size_t kNativeMessageOptionsV1Size =
    offsetof(NovaFixPluginNativeMessageOptions, durationMs) +
    sizeof(std::uint32_t);
constexpr std::size_t kNotificationOptionsV1Size =
    offsetof(NovaFixPluginNativeNotificationOptions, durationMs) +
    sizeof(std::uint32_t);
constexpr std::uint32_t kNotificationCapabilities =
    NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_CAP_TIMED |
    NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_CAP_ACKNOWLEDGE |
    NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_CAP_ACKNOWLEDGE_BLOCKS_GAME_INPUT |
    NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_CAP_POSITION_GRID;

struct QueuedMessage {
    std::uint64_t id{};
    std::wstring text;
    std::uint32_t mode{};
    std::uint32_t delayMs{};
    std::uint32_t durationMs{};
    std::uint32_t position{};
    bool exclusiveInput{};
    ULONGLONG queuedAt{};
};

struct ActiveMessage {
    void* product{};
    std::uint32_t mode{};
    std::uint32_t durationMs{};
    ULONGLONG openedAt{};
    bool closeRequested{};
    bool acknowledgementArmed{};
    bool choiceFocused{};
};

std::mutex g_mutex;
bool g_supported{};
Backend g_backend;
std::deque<QueuedMessage> g_queue;
ActiveMessage g_active;
ULONGLONG g_lastClosedAt{};
std::uint64_t g_nextId{1u};
bool g_acknowledgementReleasePending{};
std::atomic_flag g_pumping = ATOMIC_FLAG_INIT;
std::atomic_bool g_pumpNeeded{};
std::atomic_bool g_movementBlocked{};
std::atomic_bool g_gameplayInputBlocked{};
std::atomic_bool g_inlineGlyphsSupported{};
NovaFixPluginNativeNotificationService g_service{
    {sizeof(NovaFixPluginNativeNotificationService),
     NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_SERVICE_VERSION},
    nullptr, 0u, nullptr};

bool AcknowledgementPressed() {
    if ((GetAsyncKeyState(VK_RETURN) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0) {
        return true;
    }
    for (unsigned index = 0u; index != 4u; ++index) {
        std::uint16_t buttons{};
        if (input::xinput::GetButtons(index, buttons) &&
            (buttons & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B)) != 0u) {
            return true;
        }
    }
    return false;
}

bool Utf8ToWide(const char* source, std::wstring& result,
                bool& containsGlyph) {
    result.clear();
    containsGlyph = false;
    if (!source) return false;
    std::array<char, kMaximumMessageBytes + 1u> local{};
    DWORD exceptionCode{};
    const std::size_t bytes = guarded_call::ResultOr<std::size_t>([&] {
        for (std::size_t index = 0u; index < local.size(); ++index) {
            local[index] = source[index];
            if (local[index] == '\0') return index;
        }
        return local.size();
    }, 0u, &exceptionCode);
    if (bytes == 0u || bytes > kMaximumMessageBytes) return false;
    const int characters = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, local.data(), static_cast<int>(bytes),
        nullptr, 0);
    if (characters <= 0) return false;
    result.resize(static_cast<std::size_t>(characters));
    if (MultiByteToWideChar(
               CP_UTF8, MB_ERR_INVALID_CHARS, local.data(),
               static_cast<int>(bytes), result.data(), characters) ==
           characters) {
        if (!native_notification_text::ExpandMarkup(result)) return false;
        containsGlyph = std::any_of(
            result.begin(), result.end(),
            &native_notification_text::IsGlyph);
        return true;
    }
    return false;
}

bool CopyOptions(const NovaFixPluginNativeMessageOptions* source,
                 NovaFixPluginNativeMessageOptions* destination) {
    if (!source || !destination) return false;
    DWORD exceptionCode{};
    return guarded_call::ResultOr<bool>([&] {
        if (source->structSize < kNativeMessageOptionsV1Size) return false;
        std::memset(destination, 0, sizeof(*destination));
        std::memcpy(destination, source,
                    std::min<std::size_t>(source->structSize,
                                          sizeof(*destination)));
        return true;
    }, false, &exceptionCode);
}

bool CopyOptions(const NovaFixPluginNativeNotificationOptions* source,
                 NovaFixPluginNativeNotificationOptions* destination) {
    if (!source || !destination) return false;
    DWORD exceptionCode{};
    return guarded_call::ResultOr<bool>([&] {
        if (source->structSize < kNotificationOptionsV1Size) return false;
        std::memset(destination, 0, sizeof(*destination));
        std::memcpy(destination, source,
                    std::min<std::size_t>(source->structSize,
                                          sizeof(*destination)));
        return true;
    }, false, &exceptionCode);
}

std::uint32_t EnqueueCopied(
    const char* utf8Text,
    const NovaFixPluginNativeNotificationOptions& options,
    bool exclusiveInput) {
    QueuedMessage message;
    bool containsGlyph{};
    if (!Utf8ToWide(utf8Text, message.text, containsGlyph)) {
        return NOVAFIX_PLUGIN_RESULT_INVALID_ARGUMENT;
    }
    if (containsGlyph &&
        !g_inlineGlyphsSupported.load(std::memory_order_acquire)) {
        return NOVAFIX_PLUGIN_RESULT_UNSUPPORTED;
    }
    message.mode = options.mode;
    message.delayMs = options.delayMs;
    message.durationMs = options.durationMs;
    message.position = options.position;
    message.exclusiveInput = exclusiveInput;
    message.queuedAt = GetTickCount64();

    std::scoped_lock lock(g_mutex);
    if (!g_supported) return NOVAFIX_PLUGIN_RESULT_UNSUPPORTED;
    if (g_queue.size() >= kMaximumQueuedMessages) {
        return NOVAFIX_PLUGIN_RESULT_BUSY;
    }
    message.id = g_nextId++;
    g_queue.push_back(std::move(message));
    g_pumpNeeded.store(true, std::memory_order_release);
    return NOVAFIX_PLUGIN_RESULT_ACCEPTED;
}

std::uint32_t NOVAFIX_PLUGIN_CALL EnqueueFromService(
    void*, const char* utf8Text,
    const NovaFixPluginNativeNotificationOptions* options) {
    return Enqueue(utf8Text, options);
}

} // namespace

void Configure(bool supported, bool inlineGlyphsSupported) {
    std::scoped_lock lock(g_mutex);
    g_supported = supported;
    g_inlineGlyphsSupported.store(
        supported && inlineGlyphsSupported, std::memory_order_release);
    g_service.capabilities = supported
        ? kNotificationCapabilities |
            (inlineGlyphsSupported
                 ? NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_CAP_INLINE_GLYPHS
                 : 0u)
        : 0u;
    g_service.enqueue = &EnqueueFromService;
    if (!supported) {
        g_backend = {};
        g_queue.clear();
        g_active = {};
        g_acknowledgementReleasePending = false;
        g_pumpNeeded.store(false, std::memory_order_release);
        g_movementBlocked.store(false, std::memory_order_release);
        g_gameplayInputBlocked.store(false, std::memory_order_release);
    }
}

void RegisterBackend(Backend backend) {
    std::scoped_lock lock(g_mutex);
    g_backend = backend;
}

uint32_t Queue(const char* utf8Text) {
    const NovaFixPluginNativeMessageOptions options =
        NovaFixPlugin_MakeNativeMessageOptions();
    return QueueEx(utf8Text, &options);
}

uint32_t QueueEx(const char* utf8Text,
                 const NovaFixPluginNativeMessageOptions* options) {
    NovaFixPluginNativeMessageOptions copiedOptions{};
    if (!CopyOptions(options, &copiedOptions) ||
        (copiedOptions.flags &
         ~NOVAFIX_PLUGIN_NATIVE_MESSAGE_EXCLUSIVE_INPUT) != 0u ||
        (copiedOptions.flags &
         NOVAFIX_PLUGIN_NATIVE_MESSAGE_EXCLUSIVE_INPUT) == 0u ||
        copiedOptions.delayMs > kMaximumTimeMs ||
        copiedOptions.durationMs > kMaximumTimeMs) {
        return NOVAFIX_PLUGIN_RESULT_INVALID_ARGUMENT;
    }

    NovaFixPluginNativeNotificationOptions translated =
        copiedOptions.durationMs == 0u
            ? NovaFixPlugin_MakeAcknowledgementNotificationOptions()
            : NovaFixPlugin_MakeTimedNotificationOptions(
                  copiedOptions.durationMs);
    translated.delayMs = copiedOptions.delayMs;
    // Legacy timed messages may still request exclusive input.
    return EnqueueCopied(utf8Text, translated, true);
}

uint32_t Enqueue(
    const char* utf8Text,
    const NovaFixPluginNativeNotificationOptions* options) {
    NovaFixPluginNativeNotificationOptions copiedOptions{};
    if (!CopyOptions(options, &copiedOptions) ||
        copiedOptions.flags != 0u ||
        copiedOptions.delayMs > kMaximumTimeMs ||
        copiedOptions.durationMs > kMaximumTimeMs ||
        copiedOptions.position >
            NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_POSITION_BOTTOM_RIGHT) {
        return NOVAFIX_PLUGIN_RESULT_INVALID_ARGUMENT;
    }
    const bool timed =
        copiedOptions.mode == NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_TIMED;
    const bool acknowledge =
        copiedOptions.mode == NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_ACKNOWLEDGE;
    if ((!timed && !acknowledge) ||
        (timed && copiedOptions.durationMs == 0u) ||
        (acknowledge && copiedOptions.durationMs != 0u)) {
        return NOVAFIX_PLUGIN_RESULT_INVALID_ARGUMENT;
    }

    return EnqueueCopied(utf8Text, copiedOptions, acknowledge);
}

const NovaFixPluginNativeNotificationService* Service() {
    return &g_service;
}

void Pump() {
    // Hot LR UI path; avoid clock and synchronization work when idle
    if (!g_pumpNeeded.load(std::memory_order_acquire)) return;
    if (g_pumping.test_and_set(std::memory_order_acquire)) return;
    struct Guard {
        ~Guard() { g_pumping.clear(std::memory_order_release); }
    } guard;

    const ULONGLONG now = GetTickCount64();
    const bool acknowledgementPressed = AcknowledgementPressed();
    Backend backend;
    QueuedMessage message;
    void* productToClose{};
    void* productToFocus{};
    {
        std::scoped_lock lock(g_mutex);
        backend = g_backend;
        if (!g_supported || !backend.canOpen || !backend.open ||
            !backend.close) {
            return;
        }
        if (!g_active.product && g_acknowledgementReleasePending) {
            if (acknowledgementPressed) return;
            g_acknowledgementReleasePending = false;
            g_gameplayInputBlocked.store(false, std::memory_order_release);
        }
        if (g_active.product && !g_active.closeRequested &&
            g_active.durationMs != 0u &&
            now - g_active.openedAt >= g_active.durationMs) {
            productToClose = g_active.product;
            g_active.closeRequested = true;
        }
        if (g_active.product && !g_active.closeRequested &&
            g_active.mode ==
                NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_ACKNOWLEDGE) {
            if (!g_active.choiceFocused && backend.focusChoice) {
                productToFocus = g_active.product;
            }
            if (!acknowledgementPressed) {
                g_active.acknowledgementArmed = true;
            } else if (g_active.acknowledgementArmed) {
                productToClose = g_active.product;
                g_active.closeRequested = true;
                g_acknowledgementReleasePending = true;
            }
        }
        if (!productToClose && !productToFocus) {
            if (g_active.product) {
                return;
            }
            if (g_queue.empty()) {
                g_pumpNeeded.store(false, std::memory_order_release);
                return;
            }
            if (now - g_lastClosedAt < kCloseSettleMs) {
                return;
            }
            if (now - g_queue.front().queuedAt < g_queue.front().delayMs) {
                return;
            }
            message = g_queue.front();
        }
    }

    if (productToClose) {
        if (!backend.close(productToClose)) {
            std::scoped_lock lock(g_mutex);
            if (g_active.product == productToClose) {
                g_active.closeRequested = false;
                g_acknowledgementReleasePending = false;
            }
            LogWarning("Add-on notification close failed");
        }
        return;
    }
    if (productToFocus) {
        const bool focused = backend.focusChoice(productToFocus);
        std::scoped_lock lock(g_mutex);
        if (focused && g_active.product == productToFocus) {
            g_active.choiceFocused = true;
        }
        return;
    }
    if (!backend.canOpen()) return;
    static std::uint64_t lastFailedMessage{};
    const bool reportFailure = lastFailedMessage != message.id;
    int horizontalAlignment = 1;
    int verticalAlignment = 1;
    if (message.position !=
        NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_POSITION_DEFAULT) {
        const std::uint32_t index = message.position - 1u;
        horizontalAlignment = static_cast<int>(index % 3u) + 1;
        verticalAlignment = static_cast<int>(index / 3u) + 1;
        // The games use 1=center, 2=leading/top, 3=trailing/bottom.
        constexpr int kGridToNative[]{2, 1, 3};
        horizontalAlignment = kGridToNative[horizontalAlignment - 1];
        verticalAlignment = kGridToNative[verticalAlignment - 1];
    }
    const OpenRequest request{
        message.mode, message.durationMs, horizontalAlignment,
        verticalAlignment, message.exclusiveInput};
    void* const product = backend.open(
        message.text, request, reportFailure);
    if (!product) {
        lastFailedMessage = message.id;
        return;
    }

    {
        std::scoped_lock lock(g_mutex);
        if (!g_queue.empty() && g_queue.front().id == message.id) {
            g_queue.pop_front();
        }
        g_active = {product, message.mode, message.durationMs,
                    GetTickCount64(), false, false, false};
        g_movementBlocked.store(
            message.exclusiveInput, std::memory_order_release);
        g_gameplayInputBlocked.store(
            message.mode == NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_ACKNOWLEDGE,
            std::memory_order_release);
    }
}

void NotifyClosed(void* product) {
    if (!product) return;
    std::scoped_lock lock(g_mutex);
    if (g_active.product != product) return;
    const bool keepInputBlocked =
        g_active.mode == NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_ACKNOWLEDGE &&
        g_acknowledgementReleasePending;
    g_active = {};
    g_movementBlocked.store(false, std::memory_order_release);
    if (!keepInputBlocked) {
        g_gameplayInputBlocked.store(false, std::memory_order_release);
    }
    g_lastClosedAt = GetTickCount64();
    g_pumpNeeded.store(
        keepInputBlocked || !g_queue.empty(), std::memory_order_release);
}

bool ShouldSuppressClose(void* product) {
    if (!product) return false;
    std::scoped_lock lock(g_mutex);
    return g_active.product == product && !g_active.closeRequested;
}

bool BlocksGameplayMovement() {
    return g_movementBlocked.load(std::memory_order_acquire);
}

bool BlocksGameplayInput() {
    return g_gameplayInputBlocked.load(std::memory_order_acquire);
}

} // namespace novafix::compat::native_notifications
