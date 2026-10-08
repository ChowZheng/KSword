#pragma once
#include "NavigationProtocol.h"
#include <TlHelp32.h>
#include <vector>

namespace ksword::x64dbg_navigation
{
    inline bool transfer(HANDLE pipe, void* data, DWORD bytes, bool write, DWORD timeout)
    {
        OVERLAPPED overlapped{}; overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (overlapped.hEvent == nullptr) return false;
        DWORD done = 0;
        BOOL ok = write ? WriteFile(pipe, data, bytes, &done, &overlapped) : ReadFile(pipe, data, bytes, &done, &overlapped);
        if (!ok && GetLastError() == ERROR_IO_PENDING)
        {
            if (WaitForSingleObject(overlapped.hEvent, timeout) == WAIT_OBJECT_0)
                ok = GetOverlappedResult(pipe, &overlapped, &done, FALSE);
            else { CancelIoEx(pipe, &overlapped); GetOverlappedResult(pipe, &overlapped, &done, TRUE); SetLastError(ERROR_TIMEOUT); }
        }
        const DWORD error = ok ? (done == bytes ? ERROR_SUCCESS : ERROR_BAD_LENGTH) : GetLastError();
        CloseHandle(overlapped.hEvent); SetLastError(error);
        return ok && done == bytes;
    }
    inline DWORD exchange(DWORD debuggerPid, HANDLE debuggerIdentity, Request request, Response& response, DWORD timeout = 5000)
    {
        const auto created = creationTime(debuggerIdentity);
        if (created == 0 || GetProcessId(debuggerIdentity) != debuggerPid || WaitForSingleObject(debuggerIdentity, 0) != WAIT_TIMEOUT)
            return ERROR_INVALID_STATE;
        const auto name = pipeName(debuggerPid, created);
        HANDLE pipe = INVALID_HANDLE_VALUE;
        const auto deadline = GetTickCount64() + timeout;
        DWORD connectionError = ERROR_FILE_NOT_FOUND;
        do
        {
            pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (pipe != INVALID_HANDLE_VALUE) break;
            connectionError = GetLastError();
            if (connectionError != ERROR_PIPE_BUSY && connectionError != ERROR_FILE_NOT_FOUND) break;
            // Only retry connection establishment, never a request that may
            // already have been accepted or applied by the debugger.
            if (connectionError == ERROR_PIPE_BUSY) (void)WaitNamedPipeW(name.c_str(), 20);
            else Sleep(10);
        } while (GetTickCount64() < deadline);
        if (pipe == INVALID_HANDLE_VALUE) return connectionError;
        ULONG serverPid = 0;
        DWORD error = ERROR_SUCCESS;
        if (!GetNamedPipeServerProcessId(pipe, &serverPid) || serverPid != debuggerPid) error = ERROR_INVALID_OWNER;
        else
        {
            DWORD mode = PIPE_READMODE_MESSAGE;
            if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)
                || !transfer(pipe, &request, sizeof(request), true, timeout)
                || !transfer(pipe, &response, sizeof(response), false, timeout)) error = GetLastError();
            else if (response.magic != kMagic || response.version != kVersion || response.bytes != sizeof(response)
                || response.requestId != request.requestId || response.debuggerPid != debuggerPid || response.reserved != 0)
                error = ERROR_INVALID_DATA;
            else
            {
                Receipt receipt{}; receipt.requestId = request.requestId;
                // The server must not disconnect while its response is still
                // unread in the pipe buffer. This receipt follows a complete,
                // identity-checked read; a failed receipt does not invalidate
                // the already verified navigation result or retry navigation.
                (void)transfer(pipe, &receipt, sizeof(receipt), true, timeout);
                error = response.error;
            }
        }
        CloseHandle(pipe);
        return error;
    }
    // Return a matching connected debugger only. A busy debugger for another
    // target is never given any command, including an attach command.
    inline DWORD findSession(const Request& navigation, DWORD& debuggerPid, HANDLE& debuggerIdentity)
    {
        debuggerPid = 0; debuggerIdentity = nullptr;
        const HANDLE processes = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (processes == INVALID_HANDLE_VALUE) return GetLastError();
        PROCESSENTRY32W process{}; process.dwSize = sizeof(process);
        DWORD result = ERROR_NOT_FOUND;
        if (Process32FirstW(processes, &process)) do
        {
            if (_wcsicmp(process.szExeFile, L"x64dbg.exe") != 0 && _wcsicmp(process.szExeFile, L"x32dbg.exe") != 0) continue;
            const HANDLE identity = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, process.th32ProcessID);
            if (identity == nullptr) continue;
            Request query = navigation; query.operation = Operation::Query;
            Response response{};
            const DWORD error = exchange(process.th32ProcessID, identity, query, response, 300);
            if (error == ERROR_SUCCESS && (response.flags & Debugging) && response.targetPid == navigation.targetPid
                && response.targetCreateTime == navigation.targetCreateTime)
            { debuggerPid = process.th32ProcessID; debuggerIdentity = identity; result = ERROR_SUCCESS; break; }
            CloseHandle(identity);
            // An endpoint that is busy/unresponsive could be the existing owner.
            // Do not infer that no debugger owns this target; caller independently
            // checks CheckRemoteDebuggerPresent before any fresh launch.
        } while (Process32NextW(processes, &process));
        CloseHandle(processes);
        return result;
    }
}
