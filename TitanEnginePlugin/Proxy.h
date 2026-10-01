#pragma once
#include "NativeApi.h"
#include "../DebuggerBackend/KswordDebuggerBackend.h"
#include <atomic>

namespace ksword::titan
{
    std::uint64_t nativeEventSequence();
    extern std::atomic<bool> hvmSelected;
    extern std::recursive_mutex policyMutex;
    void log(const std::string& message);
    DWORD releaseSession();
    DWORD adoptCurrentNativeSession();
    void clearSeamError();
    DWORD getSeamError();
    bool hasPendingDebug();
    bool hasHardwareBindings();
    void clearHardwareBindings();
    void clearDeferredSteps();
    bool nativeEventHeld();
    DWORD nativeProcessId();
    DWORD retireNativeModule(DWORD processId, ULONG_PTR allocationBase);
    extern thread_local bool nativeLogicalRead;
    struct LogicalReadScope {
        bool previous;
        explicit LogicalReadScope(bool enabled = true) : previous(nativeLogicalRead) { nativeLogicalRead = enabled; }
        ~LogicalReadScope() { nativeLogicalRead = previous; }
    };
}

extern "C" __declspec(dllexport) DWORD __stdcall KSwordTitanInitialize();
extern "C" __declspec(dllexport) DWORD __stdcall KSwordTitanControl(
    DWORD enabled, KSWORD_DEBUGGER_BACKEND_STATUS* status);
