#include "compat/addons/plugin_services.h"

#include "compat/addons/native_notifications.h"
#include "common/guarded_call.h"
#include "diagnostics/log.h"

#include <windows.h>

#include <array>
#include <cstdint>
#include <string_view>

namespace novafix::compat::plugin_services {
namespace {

constexpr std::size_t kMaximumPluginIdBytes = NOVAFIX_PLUGIN_MAX_ID_BYTES;
constexpr std::size_t kMaximumMessageBytes =
    NOVAFIX_PLUGIN_MAX_LOG_MESSAGE_BYTES;

template <std::size_t Capacity>
bool CopyPluginText(const char* source, std::array<char, Capacity>* output,
                    bool allowTruncation) {
    if (!source || !output) return false;
    DWORD exceptionCode{};
    return guarded_call::ResultOr<bool>([&] {
        for (std::size_t index = 0u; index < Capacity; ++index) {
            const char value = source[index];
            (*output)[index] = value == '\r' || value == '\n' ? ' ' : value;
            if (value == '\0') return true;
        }
        (*output)[Capacity - 1u] = '\0';
        return allowTruncation;
    }, false, &exceptionCode);
}

LogLevel CoreLogLevel(std::uint32_t level) {
    switch (level) {
    case NOVAFIX_PLUGIN_LOG_DEBUG: return LogLevel::Debug;
    case NOVAFIX_PLUGIN_LOG_INFO: return LogLevel::Info;
    case NOVAFIX_PLUGIN_LOG_WARNING: return LogLevel::Warning;
    case NOVAFIX_PLUGIN_LOG_ERROR: return LogLevel::Error;
    default: return LogLevel::Off;
    }
}

std::uint32_t NOVAFIX_PLUGIN_CALL WriteLog(
    void*, std::uint32_t level, const char* pluginId,
    const char* utf8Message) {
    const LogLevel coreLevel = CoreLogLevel(level);
    if (coreLevel == LogLevel::Off || !pluginId || !utf8Message) {
        return NOVAFIX_PLUGIN_RESULT_INVALID_ARGUMENT;
    }

    std::array<char, kMaximumPluginIdBytes + 1u> id{};
    std::array<char, kMaximumMessageBytes + 1u> message{};
    if (!CopyPluginText(pluginId, &id, false) ||
        !CopyPluginText(utf8Message, &message, true) || id[0] == '\0' ||
        message[0] == '\0') {
        return NOVAFIX_PLUGIN_RESULT_INVALID_ARGUMENT;
    }

    LogMessage(coreLevel, "[Add-on:%s] %s", id.data(), message.data());
    return NOVAFIX_PLUGIN_RESULT_ACCEPTED;
}

const NovaFixPluginLogService kLogService{
    {sizeof(NovaFixPluginLogService), NOVAFIX_PLUGIN_LOG_SERVICE_VERSION},
    nullptr, &WriteLog};

bool ServiceIdEquals(const char* candidate, std::string_view expected) {
    if (!candidate) return false;
    DWORD exceptionCode{};
    return guarded_call::ResultOr<bool>([&] {
        for (std::size_t index = 0u; index <= expected.size(); ++index) {
            if (candidate[index] !=
                (index == expected.size() ? '\0' : expected[index])) {
                return false;
            }
        }
        return true;
    }, false, &exceptionCode);
}

} // namespace

const void* NOVAFIX_PLUGIN_CALL GetService(
    void*, const char* serviceId, std::uint32_t minimumVersion) {
    if (minimumVersion <= kLogService.header.version &&
        ServiceIdEquals(serviceId, NOVAFIX_PLUGIN_SERVICE_LOG)) {
        return &kLogService;
    }
    const NovaFixPluginNativeNotificationService* const notifications =
        native_notifications::Service();
    if (notifications &&
        minimumVersion <= notifications->header.version &&
        ServiceIdEquals(
            serviceId, NOVAFIX_PLUGIN_SERVICE_NATIVE_NOTIFICATIONS)) {
        return notifications;
    }
    return nullptr;
}

} // namespace novafix::compat::plugin_services
