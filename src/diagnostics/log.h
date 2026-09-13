#pragma once

namespace novafix {

enum class LogLevel : unsigned char {
    Off = 0,
    Error = 1,
    Warning = 2,
    Info = 3,
    Debug = 4,
};

void SetLogModuleHandle(void* module);
void SetLogLevelFromIni(const wchar_t* configPath);
void StartLogWriter();
LogLevel StoredLogLevel();
bool SaveLogLevelToIni(const wchar_t* configPath, LogLevel level);
bool IsLogEnabled(LogLevel level);
unsigned FailureMessageCountForCurrentThread();
unsigned ErrorMessageCountForCurrentThread();
void LogMessage(LogLevel level, const char* format, ...);

} // namespace novafix

// Keep the guard at the call site so Off skips argument evaluation and log I/O
#define NOVAFIX_LOG_AT_LEVEL(level, ...)                                      \
    do {                                                                      \
        if (::novafix::IsLogEnabled(level)) {                                 \
            ::novafix::LogMessage(level, __VA_ARGS__);                        \
        }                                                                     \
    } while (false)

#define LogError(...)                                                         \
    NOVAFIX_LOG_AT_LEVEL(::novafix::LogLevel::Error, __VA_ARGS__)
#define LogWarning(...)                                                       \
    NOVAFIX_LOG_AT_LEVEL(::novafix::LogLevel::Warning, __VA_ARGS__)
#define LogInfo(...)                                                          \
    NOVAFIX_LOG_AT_LEVEL(::novafix::LogLevel::Info, __VA_ARGS__)
#define LogDebug(...)                                                         \
    NOVAFIX_LOG_AT_LEVEL(::novafix::LogLevel::Debug, __VA_ARGS__)

// Unclassified diagnostics are debug-only
#define Log(...) LogDebug(__VA_ARGS__)
