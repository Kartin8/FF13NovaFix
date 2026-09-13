#include "compat/addons/native_notifications.h"

#include "diagnostics/log.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

namespace novafix::compat::native_notifications {
namespace {

constexpr std::size_t kMaximumMessageBytes = 2048u;
constexpr std::size_t kMaximumQueuedMessages = 8u;
constexpr std::uint32_t kMaximumTimeMs = 86'400'000u;
constexpr ULONGLONG kCloseSettleMs = 250u;

struct QueuedMessage {
    std::uint64_t id{};
    std::wstring text;
    std::uint32_t flags{};
    std::uint32_t delayMs{};
    std::uint32_t durationMs{};
    ULONGLONG queuedAt{};
};

struct ActiveMessage {
    void* product{};
    std::uint32_t durationMs{};
    ULONGLONG openedAt{};
};

std::mutex g_mutex;
bool g_supported{};
Backend g_backend;
std::deque<QueuedMessage> g_queue;
ActiveMessage g_active;
ULONGLONG g_lastClosedAt{};
std::uint64_t g_nextId{1u};
std::atomic_flag g_pumping = ATOMIC_FLAG_INIT;
std::atomic_bool g_pumpNeeded{};

bool Utf8ToWide(const char* source, std::wstring& result) {
    result.clear();
    if (!source) return false;
    const std::size_t bytes = strnlen_s(source, kMaximumMessageBytes + 1u);
    if (bytes == 0u || bytes > kMaximumMessageBytes) return false;
    const int characters = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, source, static_cast<int>(bytes),
        nullptr, 0);
    if (characters <= 0) return false;
    result.resize(static_cast<std::size_t>(characters));
    return MultiByteToWideChar(
               CP_UTF8, MB_ERR_INVALID_CHARS, source,
               static_cast<int>(bytes), result.data(), characters) ==
           characters;
}

} // namespace

void Configure(bool supported) {
    std::scoped_lock lock(g_mutex);
    g_supported = supported;
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
    if (!options ||
        options->structSize < sizeof(NovaFixPluginNativeMessageOptions) ||
        options->flags != NOVAFIX_PLUGIN_NATIVE_MESSAGE_EXCLUSIVE_INPUT ||
        options->delayMs > kMaximumTimeMs ||
        options->durationMs > kMaximumTimeMs) {
        return NOVAFIX_PLUGIN_RESULT_INVALID_ARGUMENT;
    }

    QueuedMessage message;
    if (!Utf8ToWide(utf8Text, message.text)) {
        return NOVAFIX_PLUGIN_RESULT_INVALID_ARGUMENT;
    }
    message.flags = options->flags;
    message.delayMs = options->delayMs;
    message.durationMs = options->durationMs;
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

void Pump() {
    // Hot LR UI path; avoid clock and synchronization work when idle
    if (!g_pumpNeeded.load(std::memory_order_acquire)) return;
    if (g_pumping.test_and_set(std::memory_order_acquire)) return;
    struct Guard {
        ~Guard() { g_pumping.clear(std::memory_order_release); }
    } guard;

    const ULONGLONG now = GetTickCount64();
    Backend backend;
    QueuedMessage message;
    void* productToClose{};
    {
        std::scoped_lock lock(g_mutex);
        backend = g_backend;
        if (!g_supported || !backend.canOpen || !backend.open ||
            !backend.close) {
            return;
        }
        if (g_active.product && g_active.durationMs != 0u &&
            now - g_active.openedAt >= g_active.durationMs) {
            productToClose = g_active.product;
            g_active = {};
            g_lastClosedAt = now;
        }
        if (!productToClose) {
            if (g_active.product) {
                if (g_active.durationMs == 0u) {
                    g_pumpNeeded.store(false, std::memory_order_release);
                }
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
            LogWarning("Add-on notification close failed");
        }
        return;
    }
    if (!backend.canOpen()) return;
    static std::uint64_t lastFailedMessage{};
    const bool reportFailure = lastFailedMessage != message.id;
    void* const product = backend.open(message.text, reportFailure);
    if (!product) {
        lastFailedMessage = message.id;
        return;
    }

    {
        std::scoped_lock lock(g_mutex);
        if (!g_queue.empty() && g_queue.front().id == message.id) {
            g_queue.pop_front();
        }
        g_active = {product, message.durationMs, GetTickCount64()};
        if (g_active.durationMs == 0u && g_queue.empty()) {
            g_pumpNeeded.store(false, std::memory_order_release);
        }
    }
}

void NotifyClosed(void* product) {
    if (!product) return;
    std::scoped_lock lock(g_mutex);
    if (g_active.product != product) return;
    g_active = {};
    g_lastClosedAt = GetTickCount64();
    g_pumpNeeded.store(!g_queue.empty(), std::memory_order_release);
}

} // namespace novafix::compat::native_notifications
