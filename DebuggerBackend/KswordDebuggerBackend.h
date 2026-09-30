#pragma once

#include "KswordDebuggerApi.h"
#include "../Ksword5.1/Ksword5.1/ArkDriverClient/ArkDriverClient.h"
#include "../shared/driver/KswordArkHvmDebugIoctl.h"
#include "../shared/driver/KswordArkDebuggerIoctl.h"

#include <Windows.h>
#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace ksword::debugger
{
    // One backend instance per debugger process; CE-specific ABI stays outside.
    class Backend
    {
    public:
        bool initialize();
        bool shutdown();
        DWORD dispatch(KSWORD_DEBUGGER_CALL& call);
        DWORD setUseHvm(bool enabled);
        KSWORD_DEBUGGER_BACKEND_STATUS status();
        HANDLE openProcess(DWORD access, BOOL inherit, DWORD pid);
        DWORD processId(HANDLE process);
        BOOL readMemory(HANDLE process, LPCVOID address, LPVOID data, SIZE_T bytes, SIZE_T* transferred);
        BOOL writeMemory(HANDLE process, LPVOID address, LPCVOID data, SIZE_T bytes, SIZE_T* transferred);
        SIZE_T queryMemory(HANDLE process, LPCVOID address, PMEMORY_BASIC_INFORMATION info, SIZE_T bytes);
        BOOL protectMemory(HANDLE process, LPVOID address, SIZE_T bytes, DWORD protection, PDWORD previous);
        LPVOID allocateMemory(HANDLE process, LPVOID address, SIZE_T bytes, DWORD type, DWORD protection);
        HANDLE createThread(HANDLE process, LPSECURITY_ATTRIBUTES attributes, SIZE_T stackBytes,
            LPTHREAD_START_ROUTINE start, LPVOID parameter, DWORD flags, LPDWORD tid);
        HANDLE openThread(DWORD access, BOOL inherit, DWORD tid);
        BOOL getContext(HANDLE thread, LPCONTEXT context);
        BOOL setContext(HANDLE thread, const CONTEXT* context);
        DWORD suspendThread(HANDLE thread);
        DWORD resumeThread(HANDLE thread);
        BOOL attach(DWORD pid);
        BOOL waitEvent(LPDEBUG_EVENT event, DWORD milliseconds);
        BOOL continueEvent(DWORD pid, DWORD tid, DWORD status);
        void log(const std::string& message);
    private:
#ifdef KSWORD_DEBUGGER_TESTING
        friend struct BackendTestPeer;
#endif
        struct ThreadBreakpoints
        {
            CONTEXT requested{};
            std::array<DWORD, 4> ids{};
        };
        // One lock serializes policy changes with high-frequency memory access.
        std::recursive_mutex mutex_;
        ark::DriverClient client_;
        ark::DriverHandle driver_;
        struct ProcessProxy { DWORD pid = 0; ark::DriverHandle identity; };
        std::unordered_map<HANDLE, ProcessProxy> processIds_;
        std::unordered_map<DWORD, ThreadBreakpoints> breakpoints_;
        bool useHvm_ = false;
        bool ownsResident_ = false;
        bool monitorAttachment_ = false;
        DWORD attachedPid_ = 0;
        DWORD lastError_ = ERROR_SUCCESS;
        DEBUG_EVENT lastEvent_{};
        DWORD ensureDebugHvm();
        DWORD pauseOwnedHvm(bool& restart);
        DWORD startOwnedHvm();
        DWORD releaseOwnedHvm();
        void retireDetachedTarget();
        DWORD removeBreakpoints(DWORD tid);
        DWORD installBreakpoints(DWORD tid, ThreadBreakpoints& record);
        DWORD nativeRequest(KSWORD_ARK_DEBUGGER_REQUEST& request, KSWORD_ARK_DEBUGGER_RESPONSE& response);
        BOOL nativeContext(HANDLE thread, CONTEXT* context, bool write);
        DWORD changeSuspendCount(HANDLE thread, bool suspend);
        DWORD breakpointRequest(KSWORD_ARK_HVM_DEBUG_REQUEST& request,
            KSWORD_ARK_HVM_DEBUG_RESPONSE& response);
        bool transfer(bool write, DWORD pid, std::uint64_t address, void* data,
            SIZE_T bytes, SIZE_T& transferred);
    };
    Backend& backend();
}
