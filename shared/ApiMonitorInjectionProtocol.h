#pragma once
#include "ApiMonitorPlatform.h"
#include <algorithm>
#include <cwchar>

namespace ks::winapi_monitor
{
    inline constexpr DWORD kInjectionProtocolVersion = 1;
    struct InjectionEndpoint {
        std::wstring pipeName, token;
        DWORD hostPid = 0;
        std::uint64_t hostCreation = 0;
    };
    struct InjectionRequest {
        DWORD size = sizeof(InjectionRequest), version = kInjectionProtocolVersion, childPid = 0, reserved = 0;
        std::uint64_t clientCreation = 0, childCreation = 0;
        wchar_t token[128]{};
    };
    struct InjectionResponse {
        DWORD size = sizeof(InjectionResponse), version = kInjectionProtocolVersion, error = ERROR_INVALID_DATA, reserved = 0;
        wchar_t detail[256]{};
    };
    static_assert(sizeof(InjectionRequest) == 288 && sizeof(InjectionResponse) == 528);

    // Both pipe peers use overlapped I/O: no unbounded read on a stopped/missing host.
    inline bool injectionPipeTransfer(HANDLE pipe, void* buffer, DWORD size, bool writing,
        HANDLE stop = nullptr, DWORD timeout = 20000)
    {
        OVERLAPPED operation{}; operation.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!operation.hEvent) return false;
        DWORD transferred = 0;
        BOOL result = writing ? ::WriteFile(pipe, buffer, size, &transferred, &operation)
            : ::ReadFile(pipe, buffer, size, &transferred, &operation);
        DWORD error = result ? ERROR_SUCCESS : ::GetLastError();
        if (!result && error == ERROR_IO_PENDING) {
            HANDLE waits[] = {operation.hEvent, stop};
            const DWORD wait = stop ? ::WaitForMultipleObjects(2, waits, FALSE, timeout)
                : ::WaitForSingleObject(operation.hEvent, timeout);
            if (wait == WAIT_OBJECT_0) {
                result = ::GetOverlappedResult(pipe, &operation, &transferred, FALSE);
                error = result ? ERROR_SUCCESS : ::GetLastError();
            } else {
                error = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : wait == WAIT_OBJECT_0 + 1
                    ? ERROR_OPERATION_ABORTED : ::GetLastError();
                ::CancelIoEx(pipe, &operation);
                DWORD ignored = 0; ::GetOverlappedResult(pipe, &operation, &ignored, TRUE);
            }
        }
        if (!error && transferred != size) error = ERROR_INVALID_DATA;
        ::CloseHandle(operation.hEvent); ::SetLastError(error); return !error;
    }
    inline bool requestChildInjection(const InjectionEndpoint& endpoint, DWORD childPid,
        std::uint64_t childCreation, std::wstring* error)
    {
        if (endpoint.pipeName.empty() || endpoint.token.empty() || endpoint.token.size() >= 128
            || !endpoint.hostPid || !endpoint.hostCreation || !childPid || !childCreation)
            return platformFailure(error, L"main-program injection session unavailable", ERROR_INVALID_PARAMETER);
        HANDLE host = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, endpoint.hostPid);
        FILETIME created{}, exited{}, kernel{}, user{};
        const bool live = host && ::GetProcessTimes(host, &created, &exited, &kernel, &user)
            && (((static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime) == endpoint.hostCreation);
        if (host) ::CloseHandle(host);
        if (!live) return platformFailure(error, L"main-program injection host exited or changed", ERROR_INVALID_HANDLE);
        HANDLE pipe = INVALID_HANDLE_VALUE;
        const auto deadline = ::GetTickCount64() + 20000;
        do {
            pipe = ::CreateFileW(endpoint.pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED, nullptr);
            if (pipe != INVALID_HANDLE_VALUE) break;
            const DWORD code = ::GetLastError();
            if (code != ERROR_PIPE_BUSY || ::GetTickCount64() >= deadline)
                return platformFailure(error, L"connect main-program injection session", code);
            ::WaitNamedPipeW(endpoint.pipeName.c_str(), 250);
        } while (true);
        ULONG serverPid = 0; DWORD mode = PIPE_READMODE_MESSAGE;
        if (!::GetNamedPipeServerProcessId(pipe, &serverPid) || serverPid != endpoint.hostPid
            || !::SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
            ::CloseHandle(pipe); return platformFailure(error, L"injection pipe host identity mismatch", ERROR_ACCESS_DENIED);
        }
        InjectionRequest request{}; request.childPid = childPid; request.childCreation = childCreation;
        ::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user);
        request.clientCreation = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        wcscpy_s(request.token, endpoint.token.c_str());
        InjectionResponse response{};
        const bool received = injectionPipeTransfer(pipe, &request, sizeof(request), true)
            && injectionPipeTransfer(pipe, &response, sizeof(response), false);
        const DWORD code = ::GetLastError(); ::CloseHandle(pipe);
        if (!received) return platformFailure(error, L"main-program injection request failed", code);
        if (response.size != sizeof(response) || response.version != kInjectionProtocolVersion
            || response.reserved || !std::wmemchr(response.detail, 0, std::size(response.detail)))
            return platformFailure(error, L"invalid main-program injection reply", ERROR_INVALID_DATA);
        if (response.error) {
            if (error) *error = response.detail;
            ::SetLastError(response.error); return false;
        }
        return true;
    }
}
