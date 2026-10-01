#pragma once

#include "KswordDebuggerApi.h"
#include "../Ksword5.1/Ksword5.1/ArkDriverClient/ArkDriverClient.h"
#include "../shared/driver/KswordArkHvmDebugIoctl.h"
#include "../shared/driver/KswordArkDebuggerIoctl.h"

#include <Windows.h>
#include <array>
#include <atomic>
#include <mutex>
#include <memory>
#include <unordered_map>
#include <unordered_set>

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
        // Borrow a transport already owned by the frontend. These helpers never
        // attach, detach, wait for, or continue a Windows debug event.
        DWORD observeNativeSession(DWORD pid, std::uint64_t* generation = nullptr);
        DWORD observeNativeEvent(const DEBUG_EVENT& event, std::uint64_t generation = 0);
        DWORD validateNativeContinue(DWORD pid, DWORD tid, std::uint64_t generation = 0);
        DWORD releaseNativeSession(std::uint64_t generation = 0);
        DWORD retireNativeThread(DWORD tid, std::uint64_t generation = 0);
        // The native adapter keeps complete Windows/XSTATE context ownership.
        // Only debug registers cross this seam; Dr6 always comes from Windows.
        using NativeDebugWriter = BOOL(WINAPI*)(HANDLE, const CONTEXT*, BOOL rollback, void*);
        // Success also includes a valid session with no shadow yet (unchanged).
        BOOL overlayNativeDebugContext(HANDLE thread, CONTEXT* context, std::uint64_t generation = 0);
        BOOL applyNativeDebugContext(HANDLE thread, const CONTEXT* context,
            NativeDebugWriter writer, void* opaque, std::uint64_t generation = 0);
        void log(const std::string& message);
        void overlayShadowBytes(DWORD pid, std::uint64_t address, void* data, SIZE_T bytes);
    private:
#ifdef KSWORD_DEBUGGER_TESTING
        friend struct BackendTestPeer;
#endif
        struct ThreadBreakpoints
        {
            CONTEXT requested{};
            std::array<DWORD, 4> ids{};
            std::shared_ptr<ark::DriverHandle> identity;
            std::uint64_t generation = 0;
            bool shadow = false;
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
        bool nativeContextFallback_ = false;
        std::unordered_set<std::uint64_t> shadowInt3_;
        bool shadowPrepared_ = false;
        std::unordered_map<std::uint64_t, DWORD> shadowReferences_;
        struct ShadowStop { std::uint64_t address; DWORD mask; bool userStep; bool stepping; };
        std::unordered_map<DWORD, ShadowStop> shadowStops_;
        DWORD ensureShadowHvm();
        DWORD removeShadowInt3(std::uint64_t address);
        DWORD addShadowInt3(std::uint64_t address);
        DWORD removeShadowRecord(DWORD tid);
        BOOL applyShadowContext(HANDLE thread, const CONTEXT* context);
        DWORD shadowEvent(DEBUG_EVENT& event, bool& consume);
        DWORD beginShadowStep(DWORD tid, bool automatic);
        bool shadowWrite(DWORD pid, std::uint64_t address, const void* data, SIZE_T bytes,
            SIZE_T& transferred, bool& handled);
        DWORD attachedPid_ = 0;
        enum class AttachmentOwner { none, backend, native };
        AttachmentOwner attachmentOwner_ = AttachmentOwner::none;
        ark::DriverHandle attachedIdentity_;
        std::uint64_t sessionGeneration_ = 0;
        DWORD lastError_ = ERROR_SUCCESS;
        DEBUG_EVENT lastEvent_{};
        DWORD ensureDebugHvm();
        DWORD pauseOwnedHvm(bool& restart);
        DWORD startOwnedHvm();
        DWORD releaseOwnedHvm();
        DWORD retireAttachment();
        void clearAttachment();
        bool validTargetThread(HANDLE thread) const;
        bool matchesThreadRecord(HANDLE thread, const ThreadBreakpoints& record) const;
        BOOL applyDebugContext(HANDLE thread, const CONTEXT* context,
            NativeDebugWriter writer, void* opaque);
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
