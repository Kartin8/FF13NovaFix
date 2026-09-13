#include "diagnostics/log.h"

#include "settings/ini_value.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <iterator>

namespace novafix {
namespace {

constexpr std::size_t kMaximumLineBytes = 1200u;
constexpr std::size_t kQueueCapacity = 2048u;
constexpr std::size_t kDrainBatchSize = 16u;

struct QueuedLine {
    std::array<char, kMaximumLineBytes> bytes{};
    DWORD length{};
};

wchar_t g_logPath[32768] = L"NovaFix.log";
HANDLE g_logFile = INVALID_HANDLE_VALUE;
std::atomic<LogLevel> g_logLevel{LogLevel::Info};
std::atomic<LogLevel> g_storedLogLevel{LogLevel::Info};
INIT_ONCE g_writerOnce = INIT_ONCE_STATIC_INIT;
std::atomic_bool g_writerStartAllowed{false};
SRWLOCK g_queueLock = SRWLOCK_INIT;
std::array<QueuedLine, kQueueCapacity> g_queue{};
std::size_t g_queueRead{};
std::size_t g_queueWrite{};
std::size_t g_queueSize{};
HANDLE g_queueEvent{};
std::atomic_bool g_writerReady{false};
std::atomic_uint64_t g_droppedLines{};
thread_local unsigned g_failureMessageCount{};
thread_local unsigned g_errorMessageCount{};

const char* LevelName(LogLevel level) {
    switch (level) {
    case LogLevel::Error: return "ERROR";
    case LogLevel::Warning: return "WARNING";
    case LogLevel::Info: return "INFO";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Off: return "OFF";
    }
    return "UNKNOWN";
}

bool Enabled(LogLevel level) {
    const LogLevel configured = g_logLevel.load(std::memory_order_relaxed);
    return configured != LogLevel::Off &&
           static_cast<unsigned>(level) <= static_cast<unsigned>(configured);
}

LogLevel ParseLevel(const wchar_t* text) {
    if (!text) return LogLevel::Info;
    while (std::iswspace(*text)) ++text;

    wchar_t value[32]{};
    size_t length{};
    while (text[length] && !std::iswspace(text[length]) &&
           length + 1 < std::size(value)) {
        value[length] = text[length];
        ++length;
    }
    value[length] = L'\0';

    if (_wcsicmp(value, L"Off") == 0 || wcscmp(value, L"0") == 0) {
        return LogLevel::Off;
    }
    if (_wcsicmp(value, L"Error") == 0 || wcscmp(value, L"1") == 0) {
        return LogLevel::Error;
    }
    if (_wcsicmp(value, L"Warning") == 0 || wcscmp(value, L"2") == 0) {
        return LogLevel::Warning;
    }
    if (_wcsicmp(value, L"Info") == 0 || wcscmp(value, L"3") == 0) {
        return LogLevel::Info;
    }
    if (_wcsicmp(value, L"Debug") == 0 || wcscmp(value, L"4") == 0) {
        return LogLevel::Debug;
    }
    return LogLevel::Info;
}

const wchar_t* IniValue(LogLevel level) {
    switch (level) {
    case LogLevel::Off: return L"Off";
    case LogLevel::Error: return L"Error";
    case LogLevel::Warning: return L"Warning";
    case LogLevel::Info: return L"Info";
    case LogLevel::Debug: return L"Debug";
    }
    return L"Info";
}

bool WriteAll(const char* bytes, DWORD length) {
    while (length != 0) {
        DWORD written{};
        if (!WriteFile(g_logFile, bytes, length, &written, nullptr) || written == 0) return false;
        bytes += written;
        length -= written;
    }
    return true;
}

bool EnsureOpen() {
    if (g_logFile != INVALID_HANDLE_VALUE) return true;
    g_logFile = CreateFileW(g_logPath, FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    return g_logFile != INVALID_HANDLE_VALUE;
}

void WriteDroppedNotice(std::uint64_t count) {
    if (!count || !EnsureOpen()) return;
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char output[kMaximumLineBytes]{};
    const int length = _snprintf_s(
        output, sizeof(output), _TRUNCATE,
        "[%02u:%02u:%02u.%03u][%lu][WARNING] Async log queue dropped %llu lines.\r\n",
        time.wHour, time.wMinute, time.wSecond, time.wMilliseconds,
        GetCurrentThreadId(), static_cast<unsigned long long>(count));
    if (length > 0) WriteAll(output, static_cast<DWORD>(length));
}

DWORD WINAPI WriterThread(void*) {
    std::array<QueuedLine, kDrainBatchSize> batch{};
    while (true) {
        WaitForSingleObject(g_queueEvent, INFINITE);
        while (true) {
            std::size_t count{};
            AcquireSRWLockExclusive(&g_queueLock);
            while (count < batch.size() && g_queueSize != 0u) {
                batch[count++] = g_queue[g_queueRead];
                g_queueRead = (g_queueRead + 1u) % g_queue.size();
                --g_queueSize;
            }
            ReleaseSRWLockExclusive(&g_queueLock);
            if (count == 0u) break;
            if (!EnsureOpen()) continue;
            for (std::size_t index = 0u; index < count; ++index) {
                WriteAll(batch[index].bytes.data(), batch[index].length);
            }
        }
        WriteDroppedNotice(
            g_droppedLines.exchange(0u, std::memory_order_acq_rel));
    }
}

BOOL CALLBACK StartWriter(PINIT_ONCE, PVOID, PVOID*) {
    g_queueEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_queueEvent) return TRUE;
    HANDLE thread = CreateThread(
        nullptr, 0u, &WriterThread, nullptr, 0u, nullptr);
    if (!thread) {
        CloseHandle(g_queueEvent);
        g_queueEvent = nullptr;
        return TRUE;
    }
    CloseHandle(thread);
    g_writerReady.store(true, std::memory_order_release);
    return TRUE;
}

void QueueLine(const char* bytes, DWORD length) {
    if (g_writerStartAllowed.load(std::memory_order_acquire)) {
        InitOnceExecuteOnce(&g_writerOnce, &StartWriter, nullptr, nullptr);
    }
    if (!TryAcquireSRWLockExclusive(&g_queueLock)) {
        g_droppedLines.fetch_add(1u, std::memory_order_relaxed);
        return;
    }
    if (g_queueSize == g_queue.size()) {
        g_droppedLines.fetch_add(1u, std::memory_order_relaxed);
    } else {
        QueuedLine& line = g_queue[g_queueWrite];
        line.length = length;
        std::memcpy(line.bytes.data(), bytes, length);
        g_queueWrite = (g_queueWrite + 1u) % g_queue.size();
        ++g_queueSize;
    }
    ReleaseSRWLockExclusive(&g_queueLock);
    if (g_writerReady.load(std::memory_order_acquire) && g_queueEvent) {
        SetEvent(g_queueEvent);
    }
}

void WriteFormatted(LogLevel level, const char* format, va_list args) {
    if (!Enabled(level)) return;

    if (level == LogLevel::Error || level == LogLevel::Warning) {
        ++g_failureMessageCount;
    }
    if (level == LogLevel::Error) ++g_errorMessageCount;

    char line[1024]{};
    vsnprintf_s(line, sizeof(line), _TRUNCATE, format, args);

    SYSTEMTIME time{};
    GetLocalTime(&time);
    char output[kMaximumLineBytes]{};
    const int length = _snprintf_s(
        output, sizeof(output), _TRUNCATE,
        "[%02u:%02u:%02u.%03u][%lu][%s] %s\r\n",
        time.wHour, time.wMinute, time.wSecond, time.wMilliseconds,
        GetCurrentThreadId(), LevelName(level), line);
    if (length > 0) QueueLine(output, static_cast<DWORD>(length));
}

} // namespace

void SetLogModuleHandle(void* module) {
    wchar_t modulePath[32768]{};
    const DWORD length = GetModuleFileNameW(static_cast<HMODULE>(module), modulePath,
                                            static_cast<DWORD>(std::size(modulePath)));
    if (length == 0 || length >= std::size(modulePath)) return;

    wchar_t* separator = std::wcsrchr(modulePath, L'\\');
    wchar_t* alternate = std::wcsrchr(modulePath, L'/');
    if (!separator || (alternate && alternate > separator)) separator = alternate;
    if (!separator) return;
    separator[1] = L'\0';
    if (wcscat_s(modulePath, std::size(modulePath), L"NovaFix.log") != 0) return;
    wcscpy_s(g_logPath, std::size(g_logPath), modulePath);
}

void SetLogLevelFromIni(const wchar_t* configPath) {
    wchar_t value[32]{};
    if (configPath && *configPath) {
        GetPrivateProfileStringW(L"Diagnostics", L"LogLevel", L"Info", value,
                                 static_cast<DWORD>(std::size(value)), configPath);
    } else {
        wcscpy_s(value, L"Info");
    }
    const LogLevel level = ParseLevel(value);
    g_storedLogLevel.store(level, std::memory_order_release);
    g_logLevel.store(level, std::memory_order_release);
}

void StartLogWriter() {
    if (g_writerStartAllowed.exchange(true, std::memory_order_acq_rel)) return;
    if (!Enabled(LogLevel::Error)) {
        AcquireSRWLockExclusive(&g_queueLock);
        g_queueRead = 0u;
        g_queueWrite = 0u;
        g_queueSize = 0u;
        ReleaseSRWLockExclusive(&g_queueLock);
        g_droppedLines.store(0u, std::memory_order_release);
        return;
    }
    InitOnceExecuteOnce(&g_writerOnce, &StartWriter, nullptr, nullptr);
    if (g_writerReady.load(std::memory_order_acquire) && g_queueEvent) {
        SetEvent(g_queueEvent);
    }
}

LogLevel StoredLogLevel() {
    return g_storedLogLevel.load(std::memory_order_acquire);
}

bool SaveLogLevelToIni(const wchar_t* configPath, LogLevel level) {
    if (!configPath || !*configPath) return false;
    const std::array<settings::ini::Entry, 1> entries{{
        {L"LogLevel", IniValue(level)},
    }};
    if (!settings::ini::WriteValues(
            std::wstring(configPath), L"Diagnostics", entries)) {
        return false;
    }
    g_storedLogLevel.store(level, std::memory_order_release);
    g_logLevel.store(level, std::memory_order_release);
    return true;
}

bool IsLogEnabled(LogLevel level) {
    return Enabled(level);
}

unsigned FailureMessageCountForCurrentThread() {
    return g_failureMessageCount;
}

unsigned ErrorMessageCountForCurrentThread() {
    return g_errorMessageCount;
}

void LogMessage(LogLevel level, const char* format, ...) {
    va_list args;
    va_start(args, format);
    WriteFormatted(level, format, args);
    va_end(args);
}

} // namespace novafix
