#include "diagnostics/crash_dumps.h"

#include "common/module_path.h"

#include <dbghelp.h>

#include <atomic>
#include <cstdio>
#include <cwchar>
#include <iterator>
#include <string>

namespace novafix::diagnostics::crash_dumps {
namespace {

using ExceptionFilter = LPTOP_LEVEL_EXCEPTION_FILTER;
using MiniDumpWriteDumpFn = BOOL(WINAPI*)(
    HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
    PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

std::atomic<HMODULE> g_module{};
std::atomic<ExceptionFilter> g_previousFilter{};
volatile LONG g_dumpInProgress{};

std::wstring DumpDirectory() {
    const std::wstring moduleFile = path::ModuleFile(g_module.load(std::memory_order_acquire));
    std::wstring directory = path::Directory(moduleFile);
    if (directory.empty()) return {};
    directory.append(L"\\NovaFixCrashDumps");
    return directory;
}

std::wstring CrashFileStem(const std::wstring& directory) {
    SYSTEMTIME time{};
    GetLocalTime(&time);

    wchar_t name[160]{};
    _snwprintf_s(name, std::size(name), _TRUNCATE,
                 L"NovaFixCrash-%04u%02u%02u-%02u%02u%02u-%lu-%lu",
                 time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
                 time.wSecond, GetCurrentProcessId(), GetCurrentThreadId());

    std::wstring result = directory;
    result.push_back(L'\\');
    result.append(name);
    return result;
}

const wchar_t* AccessOperation(const EXCEPTION_RECORD* record) {
    if (!record || record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        record->NumberParameters < 1) {
        return L"n/a";
    }
    switch (record->ExceptionInformation[0]) {
    case 0:
        return L"read";
    case 1:
        return L"write";
    case 8:
        return L"execute";
    default:
        return L"unknown";
    }
}

void WriteCrashReport(const std::wstring& file, EXCEPTION_POINTERS* pointers,
                      bool dumpWritten, DWORD dumpError) {
    HANDLE output = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) return;

    const EXCEPTION_RECORD* record = pointers ? pointers->ExceptionRecord : nullptr;
    const unsigned long code = record ? record->ExceptionCode : 0;
    const void* address = record ? record->ExceptionAddress : nullptr;
    const ULONG_PTR target =
        record && record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
                record->NumberParameters >= 2
            ? record->ExceptionInformation[1]
            : 0;

    wchar_t text[2048]{};
#if defined(_M_IX86) || defined(__i386__)
    const CONTEXT* context = pointers ? pointers->ContextRecord : nullptr;
    _snwprintf_s(
        text, std::size(text), _TRUNCATE,
        L"NovaFix fatal exception\r\n"
        L"Process: %lu\r\nThread: %lu\r\n"
        L"Code: 0x%08lX\r\nAddress: %p\r\n"
        L"Access: %ls\r\nAccess target: 0x%08lX\r\n"
        L"EIP: 0x%08lX\r\nESP: 0x%08lX\r\nEBP: 0x%08lX\r\n"
        L"EAX: 0x%08lX\r\nEBX: 0x%08lX\r\nECX: 0x%08lX\r\n"
        L"EDX: 0x%08lX\r\nESI: 0x%08lX\r\nEDI: 0x%08lX\r\n"
        L"Dump written: %d\r\nDump error: %lu\r\n",
        GetCurrentProcessId(), GetCurrentThreadId(), code, address,
        AccessOperation(record), static_cast<unsigned long>(target),
        context ? context->Eip : 0, context ? context->Esp : 0,
        context ? context->Ebp : 0, context ? context->Eax : 0,
        context ? context->Ebx : 0, context ? context->Ecx : 0,
        context ? context->Edx : 0, context ? context->Esi : 0,
        context ? context->Edi : 0, dumpWritten ? 1 : 0, dumpError);
#else
    _snwprintf_s(
        text, std::size(text), _TRUNCATE,
        L"NovaFix fatal exception\r\n"
        L"Process: %lu\r\nThread: %lu\r\n"
        L"Code: 0x%08lX\r\nAddress: %p\r\n"
        L"Access: %ls\r\nAccess target: 0x%llX\r\n"
        L"Dump written: %d\r\nDump error: %lu\r\n",
        GetCurrentProcessId(), GetCurrentThreadId(), code, address,
        AccessOperation(record), static_cast<unsigned long long>(target),
        dumpWritten ? 1 : 0, dumpError);
#endif

    DWORD bytesWritten{};
    WriteFile(output, text,
              static_cast<DWORD>(std::wcslen(text) * sizeof(wchar_t)),
              &bytesWritten, nullptr);
    FlushFileBuffers(output);
    CloseHandle(output);
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* pointers) {
    if (InterlockedCompareExchange(&g_dumpInProgress, 1, 0) != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    bool dumpWritten = false;
    DWORD dumpError = ERROR_SUCCESS;
    const std::wstring directory = DumpDirectory();
    if (!directory.empty() &&
        (CreateDirectoryW(directory.c_str(), nullptr) ||
         GetLastError() == ERROR_ALREADY_EXISTS)) {
        const std::wstring stem = CrashFileStem(directory);
        const std::wstring dumpFile = stem + L".dmp";
        const std::wstring reportFile = stem + L".txt";

        HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
        auto writeDump = dbghelp
                             ? reinterpret_cast<MiniDumpWriteDumpFn>(
                                   GetProcAddress(dbghelp, "MiniDumpWriteDump"))
                             : nullptr;
        if (!writeDump) {
            dumpError = GetLastError();
        } else {
            HANDLE output =
                CreateFileW(dumpFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (output == INVALID_HANDLE_VALUE) {
                dumpError = GetLastError();
            } else {
                MINIDUMP_EXCEPTION_INFORMATION exceptionInfo{
                    GetCurrentThreadId(), pointers, FALSE};
                constexpr MINIDUMP_TYPE flags = static_cast<MINIDUMP_TYPE>(
                    MiniDumpWithFullMemory | MiniDumpWithHandleData |
                    MiniDumpWithUnloadedModules | MiniDumpWithFullMemoryInfo |
                    MiniDumpWithThreadInfo | MiniDumpWithProcessThreadData);
                dumpWritten = writeDump(
                                  GetCurrentProcess(), GetCurrentProcessId(), output,
                                  flags, &exceptionInfo, nullptr, nullptr) != FALSE;
                if (!dumpWritten) dumpError = GetLastError();
                FlushFileBuffers(output);
                CloseHandle(output);
            }
        }
        WriteCrashReport(reportFile, pointers, dumpWritten, dumpError);
    }

    InterlockedExchange(&g_dumpInProgress, 0);
    const ExceptionFilter previous =
        g_previousFilter.load(std::memory_order_acquire);
    if (previous && previous != &CrashFilter) return previous(pointers);
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void Install(HMODULE module) {
    if (module) g_module.store(module, std::memory_order_release);

    const ExceptionFilter previous = SetUnhandledExceptionFilter(&CrashFilter);
    if (previous != &CrashFilter) {
        g_previousFilter.store(previous, std::memory_order_release);
    }

}

} // namespace novafix::diagnostics::crash_dumps
