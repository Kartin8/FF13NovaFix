#include "runtime/restart.h"

#include "diagnostics/log.h"
#include "common/module_path.h"
#include "game/shared/graceful_shutdown.h"

#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <string>
#include <vector>

namespace novafix::restart {
namespace {

constexpr wchar_t kHelperVariable[] = L"NOVAFIX_RESTART_HELPER";
constexpr wchar_t kParentVariable[] = L"NOVAFIX_RESTART_PARENT";
constexpr wchar_t kCommandVariable[] = L"NOVAFIX_RESTART_COMMAND";
constexpr wchar_t kDirectoryVariable[] = L"NOVAFIX_RESTART_DIRECTORY";
constexpr wchar_t kTimeoutVariable[] = L"NOVAFIX_RESTART_TIMEOUT_MS";
constexpr DWORD kDefaultTimeoutMs = 8000;
constexpr DWORD kMaximumTimeoutMs = 60000;

HMODULE g_module = nullptr;

std::wstring EnvironmentValue(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (!required) return {};
    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), required);
    value.resize(written);
    return value;
}

bool SetRestartEnvironment(const std::wstring& command, const std::wstring& directory) {
    const std::wstring parent = std::to_wstring(GetCurrentProcessId());
    return SetEnvironmentVariableW(kHelperVariable, L"1") &&
           SetEnvironmentVariableW(kParentVariable, parent.c_str()) &&
           SetEnvironmentVariableW(kCommandVariable, command.c_str()) &&
           SetEnvironmentVariableW(kDirectoryVariable, directory.c_str());
}

void ClearRestartEnvironment() {
    SetEnvironmentVariableW(kHelperVariable, nullptr);
    SetEnvironmentVariableW(kParentVariable, nullptr);
    SetEnvironmentVariableW(kCommandVariable, nullptr);
    SetEnvironmentVariableW(kDirectoryVariable, nullptr);
    SetEnvironmentVariableW(kTimeoutVariable, nullptr);
}

} // namespace

void SetModuleHandle(HMODULE module) {
    g_module = module;
}

bool IsHelperProcess() {
    wchar_t value[2]{};
    return GetEnvironmentVariableW(kHelperVariable, value, static_cast<DWORD>(std::size(value))) == 1 &&
           value[0] == L'1';
}

bool ScheduleCurrentProcessRestart(HWND gameWindow) {
    const std::wstring dll = path::ModuleFile(g_module);
    const std::wstring executable = path::ModuleFile(nullptr);
    const wchar_t* currentCommand = GetCommandLineW();
    if (dll.empty() || executable.empty() || !currentCommand) return false;
    if (!SetRestartEnvironment(currentCommand, path::Directory(executable))) {
        ClearRestartEnvironment();
        return false;
    }

    const std::wstring rundll = path::SystemFile(L"rundll32.exe");
    if (rundll.empty()) {
        ClearRestartEnvironment();
        return false;
    }
    std::wstring helperCommand = L"\"" + rundll + L"\" \"" + dll +
                                 L"\",NovaFix_RestartHelper";
    std::vector<wchar_t> mutableCommand(helperCommand.begin(), helperCommand.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    const bool started = CreateProcessW(rundll.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE;
    ClearRestartEnvironment();
    if (!started) {
        LogError("Could not start restart helper: error=%lu", GetLastError());
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    Log("Restart helper scheduled: process=%lu", GetCurrentProcessId());
    const game::shutdown::RequestResult shutdown =
        game::shutdown::RequestApplicationShutdown();
    if (shutdown != game::shutdown::RequestResult::Requested) {
        LogWarning("Native shutdown unavailable: reason=%s fallback=WM_CLOSE",
            game::shutdown::RequestResultName(shutdown));
        if (gameWindow && IsWindow(gameWindow)) PostMessageW(gameWindow, WM_CLOSE, 0, 0);
    }
    return true;
}

void RunHelper() {
    const std::wstring parentText = EnvironmentValue(kParentVariable);
    const std::wstring command = EnvironmentValue(kCommandVariable);
    const std::wstring directory = EnvironmentValue(kDirectoryVariable);
    const std::wstring timeoutText = EnvironmentValue(kTimeoutVariable);
    const DWORD parentId = static_cast<DWORD>(std::wcstoul(parentText.c_str(), nullptr, 10));
    DWORD timeoutMs = kDefaultTimeoutMs;
    if (!timeoutText.empty()) {
        const unsigned long parsed = std::wcstoul(timeoutText.c_str(), nullptr, 10);
        if (parsed > 0) timeoutMs = static_cast<DWORD>(std::min<unsigned long>(
            parsed, kMaximumTimeoutMs));
    }
    HANDLE parent = parentId
        ? OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, parentId) : nullptr;
    if (!parent || command.empty() || directory.empty()) {
        if (parent) CloseHandle(parent);
        ClearRestartEnvironment();
        return;
    }

    DWORD waitResult = WaitForSingleObject(parent, timeoutMs);
    if (waitResult == WAIT_TIMEOUT) {
        LogWarning("Restart timeout: timeout=%lu process=%lu action=terminate",
            timeoutMs, parentId);
        if (TerminateProcess(parent, kForcedRestartExitCode)) {
            waitResult = WaitForSingleObject(parent, 2000);
        } else {
            LogError("Could not terminate restart parent: process=%lu error=%lu",
                parentId, GetLastError());
            waitResult = WAIT_FAILED;
        }
    }
    CloseHandle(parent);
    ClearRestartEnvironment();
    if (waitResult != WAIT_OBJECT_0) {
        LogError("Restart cancelled: previous process still active");
        return;
    }

    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, 0,
                       nullptr, directory.c_str(), &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

} // namespace novafix::restart
