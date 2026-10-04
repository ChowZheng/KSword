#include "process_run_as.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <TlHelp32.h>
#include <shellapi.h>
#include <userenv.h>
#include <vector>

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Userenv.lib")

namespace
{
    struct Handle
    {
        HANDLE value = nullptr;
        ~Handle() { if (value && value != INVALID_HANDLE_VALUE) ::CloseHandle(value); }
        Handle() = default;
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
    };

    struct ServiceHandle
    {
        SC_HANDLE value = nullptr;
        ~ServiceHandle() { if (value) ::CloseServiceHandle(value); }
    };

    // 所有调权仅作用于临时线程令牌；离开作用域恢复原线程上下文。
    struct ThreadContext
    {
        Handle previous;
        bool changed = false;
        bool capture()
        {
            if (::OpenThreadToken(::GetCurrentThread(), TOKEN_IMPERSONATE | TOKEN_QUERY,
                TRUE, &previous.value)) return true;
            return ::GetLastError() == ERROR_NO_TOKEN;
        }
        bool set(HANDLE token)
        {
            changed = ::SetThreadToken(nullptr, token) != FALSE;
            return changed;
        }
        ~ThreadContext() { if (changed) ::SetThreadToken(nullptr, previous.value); }
    };

    ks::process::RunAsResult failure(const wchar_t* step, DWORD error = ::GetLastError())
    {
        ks::process::RunAsResult result;
        result.error = error ? error : ERROR_GEN_FAILURE;
        result.detail = step;
        return result;
    }

    bool tokenData(HANDLE token, TOKEN_INFORMATION_CLASS type, std::vector<BYTE>& data)
    {
        DWORD length = 0;
        ::GetTokenInformation(token, type, nullptr, 0, &length);
        if (!length) return false;
        data.resize(length);
        return ::GetTokenInformation(token, type, data.data(), length, &length) != FALSE;
    }

    bool isSystem(HANDLE token)
    {
        std::vector<BYTE> data;
        return tokenData(token, TokenUser, data) &&
            ::IsWellKnownSid(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, WinLocalSystemSid);
    }

    bool isStandard(HANDLE token)
    {
        TOKEN_ELEVATION elevation{};
        DWORD length = 0;
        std::vector<BYTE> label;
        if (!::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &length)
            || elevation.TokenIsElevated || isSystem(token)
            || !tokenData(token, TokenIntegrityLevel, label)) return false;
        PSID sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(label.data())->Label.Sid;
        const DWORD rid = *::GetSidSubAuthority(sid, *::GetSidSubAuthorityCount(sid) - 1);
        return rid <= SECURITY_MANDATORY_MEDIUM_RID;
    }

    bool enablePrivilege(HANDLE token, const wchar_t* name)
    {
        TOKEN_PRIVILEGES privileges{};
        privileges.PrivilegeCount = 1;
        if (!::LookupPrivilegeValueW(nullptr, name, &privileges.Privileges[0].Luid)) return false;
        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        ::SetLastError(ERROR_SUCCESS);
        return ::AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr)
            && ::GetLastError() == ERROR_SUCCESS;
    }

    bool duplicateProcessToken(DWORD pid, Handle& token)
    {
        Handle process;
        process.value = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        Handle source;
        return process.value && ::OpenProcessToken(process.value, TOKEN_QUERY | TOKEN_DUPLICATE, &source.value)
            && ::DuplicateTokenEx(source.value, TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation,
                TokenPrimary, &token.value);
    }

    bool findSystemToken(DWORD session, Handle& token)
    {
        Handle snapshot;
        snapshot.value = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (snapshot.value == INVALID_HANDLE_VALUE || !::Process32FirstW(snapshot.value, &entry)) return false;
        DWORD lastError = ERROR_NOT_FOUND;
        do
        {
            DWORD candidateSession = 0;
            if (_wcsicmp(entry.szExeFile, L"winlogon.exe") ||
                !::ProcessIdToSessionId(entry.th32ProcessID, &candidateSession) || candidateSession != session) continue;
            Handle candidate;
            if (!duplicateProcessToken(entry.th32ProcessID, candidate))
            {
                lastError = ::GetLastError();
                continue;
            }
            if (!isSystem(candidate.value)) { lastError = ERROR_ACCESS_DENIED; continue; }
            token.value = candidate.value;
            candidate.value = nullptr;
            return true;
        } while (::Process32NextW(snapshot.value, &entry));
        ::SetLastError(lastError);
        return false;
    }

    bool hasTrustedInstallerSid(HANDLE token)
    {
        DWORD sidLength = 0, domainLength = 0;
        SID_NAME_USE use{};
        ::LookupAccountNameW(nullptr, L"NT SERVICE\\TrustedInstaller", nullptr, &sidLength,
            nullptr, &domainLength, &use);
        if (!sidLength) return false;
        std::vector<BYTE> sid(sidLength), groups;
        std::vector<wchar_t> domain(domainLength);
        if (!::LookupAccountNameW(nullptr, L"NT SERVICE\\TrustedInstaller", sid.data(), &sidLength,
            domain.data(), &domainLength, &use) || !tokenData(token, TokenGroups, groups)) return false;
        const auto* tokenGroups = reinterpret_cast<const TOKEN_GROUPS*>(groups.data());
        for (DWORD i = 0; i < tokenGroups->GroupCount; ++i)
        {
            const auto& group = tokenGroups->Groups[i];
            if ((group.Attributes & SE_GROUP_ENABLED) && !(group.Attributes & SE_GROUP_USE_FOR_DENY_ONLY)
                && ::EqualSid(group.Sid, sid.data())) return true;
        }
        return false;
    }

    bool trustedInstallerToken(Handle& token)
    {
        ServiceHandle manager;
        manager.value = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        ServiceHandle service;
        if (manager.value) service.value = ::OpenServiceW(manager.value, L"TrustedInstaller",
            SERVICE_QUERY_STATUS | SERVICE_START);
        if (!service.value) return false;
        const ULONGLONG deadline = ::GetTickCount64() + 15000;
        bool startRequested = false;
        do
        {
            SERVICE_STATUS_PROCESS status{};
            DWORD length = 0;
            if (!::QueryServiceStatusEx(service.value, SC_STATUS_PROCESS_INFO,
                reinterpret_cast<BYTE*>(&status), sizeof(status), &length)) return false;
            if (status.dwCurrentState == SERVICE_RUNNING && status.dwProcessId)
            {
                if (!duplicateProcessToken(status.dwProcessId, token)) return false;
                // SCM 提供 PID，实际令牌必须同时具备 SYSTEM 用户和服务 SID。
                if (isSystem(token.value) && hasTrustedInstallerSid(token.value)) return true;
                ::SetLastError(ERROR_ACCESS_DENIED);
                return false;
            }
            if (status.dwCurrentState == SERVICE_STOPPED)
            {
                if (startRequested)
                {
                    ::SetLastError(status.dwWin32ExitCode ? status.dwWin32ExitCode : ERROR_SERVICE_NOT_ACTIVE);
                    return false;
                }
                if (!::StartServiceW(service.value, 0, nullptr) && ::GetLastError() != ERROR_SERVICE_ALREADY_RUNNING)
                    return false;
                startRequested = true;
            }
            ::Sleep(100);
        } while (::GetTickCount64() < deadline);
        ::SetLastError(ERROR_SERVICE_REQUEST_TIMEOUT);
        return false;
    }

    bool shellStandardToken(Handle& token)
    {
        DWORD pid = 0;
        const HWND shell = ::GetShellWindow();
        if (shell) ::GetWindowThreadProcessId(shell, &pid);
        if (!pid || !duplicateProcessToken(pid, token)) return false;
        if (isStandard(token.value)) return true;
        ::SetLastError(ERROR_ACCESS_DENIED);
        return false;
    }

    bool standardToken(HANDLE current, Handle& token)
    {
        TOKEN_LINKED_TOKEN linked{};
        DWORD length = 0;
        if (::GetTokenInformation(current, TokenLinkedToken, &linked, sizeof(linked), &length))
        {
            Handle linkedHandle;
            linkedHandle.value = linked.LinkedToken;
            if (isStandard(linkedHandle.value) && ::DuplicateTokenEx(linkedHandle.value,
                TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenPrimary, &token.value)) return true;
        }
        return shellStandardToken(token);
    }
}

ks::process::RunAsAvailability ks::process::QueryRunAsAvailability()
{
    RunAsAvailability result;
    Handle current;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &current.value)) return result;
    TOKEN_ELEVATION elevation{};
    DWORD length = 0;
    result.system = ::GetTokenInformation(current.value, TokenElevation, &elevation, sizeof(elevation), &length)
        && (elevation.TokenIsElevated || isSystem(current.value));
    Handle standard;
    result.standardUser = isStandard(current.value) || standardToken(current.value, standard);
    result.administrator = !isSystem(current.value) || ::GetShellWindow() != nullptr;
    ServiceHandle manager;
    manager.value = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    ServiceHandle service;
    if (manager.value) service.value = ::OpenServiceW(manager.value, L"TrustedInstaller", SERVICE_QUERY_STATUS);
    result.trustedInstaller = result.system && service.value;
    return result;
}

ks::process::RunAsResult ks::process::RunExecutableAs(
    const std::wstring& imagePath, RunAsIdentity identity, const std::wstring& arguments)
{
    // 不通过 Shell 文件关联解释非程序文件，也不允许路径中的引号注入命令行。
    if (imagePath.empty() || imagePath.find(L'"') != std::wstring::npos ||
        (imagePath.size() < 4 || _wcsicmp(imagePath.c_str() + imagePath.size() - 4, L".exe")))
        return failure(L"Executable path required", ERROR_INVALID_PARAMETER);
    DWORD binaryType = 0;
    if (!::GetBinaryTypeW(imagePath.c_str(), &binaryType)) return failure(L"GetBinaryTypeW");
    if (binaryType != SCS_32BIT_BINARY && binaryType != SCS_64BIT_BINARY)
        return failure(L"Unsupported executable", ERROR_BAD_EXE_FORMAT);
    const auto slash = imagePath.find_last_of(L"\\/");
    const std::wstring directory = slash == std::wstring::npos ? L"" : imagePath.substr(0, slash + 1);
    Handle processToken;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &processToken.value))
        return failure(L"OpenProcessToken");
    if (identity == RunAsIdentity::Administrator && !isSystem(processToken.value))
    {
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        info.lpVerb = L"runas";
        info.lpFile = imagePath.c_str();
        info.lpParameters = arguments.c_str();
        info.lpDirectory = directory.c_str();
        info.nShow = SW_SHOWNORMAL;
        if (!::ShellExecuteExW(&info)) return failure(L"ShellExecuteExW(runas)");
        Handle process;
        process.value = info.hProcess;
        return { true, 0, process.value ? ::GetProcessId(process.value) : 0, L"UAC launch succeeded" };
    }
    if (identity != RunAsIdentity::System && identity != RunAsIdentity::TrustedInstaller && identity != RunAsIdentity::Administrator
        && identity != RunAsIdentity::StandardUser) return failure(L"Unknown identity", ERROR_INVALID_PARAMETER);

    TOKEN_ELEVATION currentElevation{};
    DWORD elevationLength = 0;
    if (identity != RunAsIdentity::StandardUser &&
        (!::GetTokenInformation(processToken.value, TokenElevation, &currentElevation,
            sizeof(currentElevation), &elevationLength) || !currentElevation.TokenIsElevated)
        && !isSystem(processToken.value))
        return failure(L"Obtain SYSTEM token (administrator required)", ERROR_ELEVATION_REQUIRED);

    Handle localToken;
    ThreadContext context;
    if (!context.capture() || !::DuplicateTokenEx(processToken.value,
        TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES | TOKEN_IMPERSONATE, nullptr,
        SecurityImpersonation, TokenImpersonation, &localToken.value) || !context.set(localToken.value))
        return failure(L"Set temporary thread token");
    enablePrivilege(localToken.value, SE_DEBUG_NAME);
    enablePrivilege(localToken.value, SE_IMPERSONATE_NAME);
    enablePrivilege(localToken.value, SE_ASSIGNPRIMARYTOKEN_NAME);
    enablePrivilege(localToken.value, SE_INCREASE_QUOTA_NAME);

    DWORD session = 0;
    if (!::ProcessIdToSessionId(::GetCurrentProcessId(), &session)) return failure(L"ProcessIdToSessionId");
    Handle token;
    Handle system;
    ThreadContext systemContext;
    const bool currentStandard = identity == RunAsIdentity::StandardUser && isStandard(processToken.value);
    if (identity == RunAsIdentity::StandardUser)
    {
        if (isStandard(processToken.value))
        {
            if (!::DuplicateTokenEx(processToken.value, TOKEN_ALL_ACCESS, nullptr,
                SecurityImpersonation, TokenPrimary, &token.value)) return failure(L"Duplicate standard token");
        }
        else if (!standardToken(processToken.value, token)) return failure(L"Obtain unelevated user token");
    }
    else
    {
        if (!findSystemToken(session, system)) return failure(L"Obtain SYSTEM token (administrator required)");
        Handle impersonation;
        if (!systemContext.capture() || !::DuplicateTokenEx(system.value,
            TOKEN_QUERY | TOKEN_IMPERSONATE | TOKEN_ADJUST_PRIVILEGES, nullptr,
            SecurityImpersonation, TokenImpersonation, &impersonation.value)) return failure(L"Duplicate SYSTEM context");
        enablePrivilege(impersonation.value, SE_TCB_NAME);
        enablePrivilege(impersonation.value, SE_IMPERSONATE_NAME);
        enablePrivilege(impersonation.value, SE_ASSIGNPRIMARYTOKEN_NAME);
        enablePrivilege(impersonation.value, SE_INCREASE_QUOTA_NAME);
        if (!systemContext.set(impersonation.value)) return failure(L"Impersonate SYSTEM");
        if (identity == RunAsIdentity::TrustedInstaller)
        {
            if (!trustedInstallerToken(token)) return failure(L"Obtain TrustedInstaller service token");
        }
        else if (identity == RunAsIdentity::Administrator)
        {
            // 当前 KSword 是 SYSTEM 时不能用 runas 继承 SYSTEM 身份；只取交互式用户的提升令牌。
            Handle shell;
            if (!shellStandardToken(shell)) return failure(L"Obtain interactive user token");
            TOKEN_LINKED_TOKEN linked{};
            DWORD length = 0;
            if (!::GetTokenInformation(shell.value, TokenLinkedToken, &linked, sizeof(linked), &length))
                return failure(L"Obtain interactive administrator token");
            Handle elevated;
            elevated.value = linked.LinkedToken;
            TOKEN_ELEVATION elevation{};
            if (isSystem(elevated.value) || !::GetTokenInformation(elevated.value, TokenElevation,
                &elevation, sizeof(elevation), &length) || !elevation.TokenIsElevated)
                return failure(L"Interactive administrator token unavailable", ERROR_ELEVATION_REQUIRED);
            if (!::DuplicateTokenEx(elevated.value, TOKEN_ALL_ACCESS, nullptr,
                SecurityImpersonation, TokenPrimary, &token.value)) return failure(L"Duplicate administrator token");
        }
        else
        {
            token.value = system.value;
            system.value = nullptr;
        }
        if (!::SetTokenInformation(token.value, TokenSessionId, &session, sizeof(session)))
            return failure(L"Set interactive session");
    }

    struct Environment
    {
        LPVOID value = nullptr;
        ~Environment() { if (value) ::DestroyEnvironmentBlock(value); }
    } environment;
    if (!::CreateEnvironmentBlock(&environment.value, token.value, FALSE)) return failure(L"CreateEnvironmentBlock");
    const std::wstring command = L"\"" + imagePath + L"\"" + (arguments.empty() ? L"" : L" " + arguments);
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    wchar_t desktop[] = L"winsta0\\default";
    startup.lpDesktop = desktop;
    PROCESS_INFORMATION process{};
    BOOL created = currentStandard
        ? ::CreateProcessW(imagePath.c_str(), buffer.data(), nullptr, nullptr,
            FALSE, CREATE_UNICODE_ENVIRONMENT, environment.value, directory.c_str(), &startup, &process)
        : ::CreateProcessAsUserW(token.value, imagePath.c_str(), buffer.data(), nullptr, nullptr,
            FALSE, CREATE_UNICODE_ENVIRONMENT, environment.value, directory.c_str(), &startup, &process);
    if (!created && !currentStandard)
    {
        // CreateProcess* 可以改写命令行；回退前必须恢复原始内容。
        buffer.assign(command.begin(), command.end());
        buffer.push_back(L'\0');
        created = ::CreateProcessWithTokenW(token.value, 0, imagePath.c_str(), buffer.data(),
            CREATE_UNICODE_ENVIRONMENT, environment.value, directory.c_str(), &startup, &process);
    }
    if (!created) return failure(L"CreateProcessAsUserW / CreateProcessWithTokenW");
    Handle processHandle, threadHandle;
    processHandle.value = process.hProcess;
    threadHandle.value = process.hThread;
    return { true, 0, process.dwProcessId, L"Token launch succeeded" };
}
