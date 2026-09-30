#include "KswordDebuggerBackend.h"

#include <cstring>

namespace ksword::debugger
{
    DWORD Backend::nativeRequest(KSWORD_ARK_DEBUGGER_REQUEST& request, KSWORD_ARK_DEBUGGER_RESPONSE& response)
    {
        request.version = KSWORD_ARK_DEBUGGER_VERSION;
        request.size = sizeof(request);
        const auto result = client_.deviceIoControl(IOCTL_KSWORD_ARK_DEBUGGER,
            &request, sizeof(request), &response, sizeof(response), &driver_);
        if (!result.ok) return result.win32Error;
        if (result.bytesReturned != sizeof(response) || response.version != KSWORD_ARK_DEBUGGER_VERSION ||
            response.size != sizeof(response)) return ERROR_INVALID_DATA;
        if (response.status >= 0) return ERROR_SUCCESS;
        using Convert = ULONG(WINAPI*)(LONG);
        const auto convert = reinterpret_cast<Convert>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError"));
        return convert == nullptr ? ERROR_GEN_FAILURE : convert(response.status);
    }

    BOOL Backend::nativeContext(HANDLE thread, CONTEXT* context, bool write)
    {
        if (context == nullptr || sizeof(*context) != KSWORD_ARK_DEBUGGER_CONTEXT_BYTES)
        { SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
        KSWORD_ARK_DEBUGGER_REQUEST request{};
        KSWORD_ARK_DEBUGGER_RESPONSE response{};
        request.operation = write ? KSWORD_ARK_DEBUGGER_SET_CONTEXT : KSWORD_ARK_DEBUGGER_GET_CONTEXT;
        request.flags = write ? KSWORD_ARK_DEBUGGER_FLAG_CONFIRMED : 0;
        request.processId = GetProcessIdOfThread(thread); request.threadId = GetThreadId(thread);
        request.contextFlags = context->ContextFlags;
        std::memcpy(request.context, context, sizeof(*context));
        const DWORD error = nativeRequest(request, response);
        if (error == ERROR_SUCCESS && !write) std::memcpy(context, response.context, sizeof(*context));
        SetLastError(error); return error == ERROR_SUCCESS ? TRUE : FALSE;
    }

    DWORD Backend::changeSuspendCount(HANDLE thread, bool suspend)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        KSWORD_ARK_DEBUGGER_REQUEST request{};
        KSWORD_ARK_DEBUGGER_RESPONSE response{};
        request.operation = suspend ? KSWORD_ARK_DEBUGGER_SUSPEND : KSWORD_ARK_DEBUGGER_RESUME;
        request.flags = KSWORD_ARK_DEBUGGER_FLAG_CONFIRMED;
        request.processId = GetProcessIdOfThread(thread); request.threadId = GetThreadId(thread);
        const DWORD error = nativeRequest(request, response);
        SetLastError(error); return error == ERROR_SUCCESS ? response.previousSuspendCount : MAXDWORD;
    }
}
