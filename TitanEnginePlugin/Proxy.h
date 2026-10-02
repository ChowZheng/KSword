#pragma once
#include "NativeApi.h"
#include "../DebuggerBackend/KswordDebuggerBackend.h"
#include <atomic>

namespace ksword::titan
{
    std::uint64_t nativeEventSequence();
    extern std::atomic<bool> hvmSelected;
    extern std::recursive_mutex policyMutex;
    // The provider stays selected through frontend handle/table teardown.
    // Replay identities must never reach the live driver or Win32 handle APIs.
    NativeApi& activeApi();
    bool replayProvider();
    bool useHvm();
    DWORD selectLiveProvider();
    PROCESS_INFORMATION* startReplay(const wchar_t* artifact, TitanSessionKind kind);
    void rememberBreakpointOptions(TitanBreakpointType type);
    DWORD queryEngineInfo(KSWORD_DEBUGGER_ENGINE_INFO& info);
    DWORD queryBreakpoint(const KSWORD_DEBUGGER_BREAKPOINT_QUERY& query, KSWORD_DEBUGGER_BREAKPOINT_INFO& info);
    void log(const std::string& message);
    DWORD releaseSession();
    DWORD adoptCurrentNativeSession();
    void clearSeamError();
    DWORD getSeamError();
    bool hasPendingDebug();
    bool hasHardwareBindings();
    bool isShadowBreakpointAddress(DWORD processId, ULONG_PTR address);
    DWORD adapterBreakpointCount();
    DWORD adapterBreakpointPath();
    bool hasPendingBindingChanges();
    void clearSoftwareBindings();
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
