#include "KswordDebuggerBackend.h"

#include <algorithm>
#include <cstring>

namespace ksword::debugger
{
    namespace
    {
        DWORD ntError(LONG status)
        {
            using Convert = ULONG(WINAPI*)(LONG);
            const auto convert = reinterpret_cast<Convert>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError"));
            return status >= 0 ? ERROR_SUCCESS : convert == nullptr ? ERROR_GEN_FAILURE : convert(status);
        }
        DWORD rejectedOperation(LONG status) { return status >= 0 ? ERROR_GEN_FAILURE : ntError(status); }
        std::uint64_t instructionPointer(const CONTEXT& context)
        {
#ifdef _WIN64
            return context.Rip;
#else
            return context.Eip;
#endif
        }
        std::array<std::uint64_t, 4> debugAddresses(const CONTEXT& context)
        {
            return {context.Dr0, context.Dr1, context.Dr2, context.Dr3};
        }
    }

    DWORD Backend::breakpointRequest(KSWORD_ARK_HVM_DEBUG_REQUEST& request,
        KSWORD_ARK_HVM_DEBUG_RESPONSE& response)
    {
        const auto result = client_.deviceIoControl(IOCTL_KSWORD_ARK_HVM_DEBUG,
            &request, sizeof(request), &response, sizeof(response), &driver_);
        if (!result.ok) return result.win32Error;
        if (result.bytesReturned != sizeof(response) || response.version != KSWORD_ARK_HVM_DEBUG_VERSION ||
            response.size != sizeof(response)) return ERROR_INVALID_DATA;
        return ntError(response.status);
    }

    DWORD Backend::startOwnedHvm()
    {
        const auto state = client_.queryHvmStatus();
        if (!state.io.ok) return state.io.win32Error;
        const auto result = client_.controlHvm(KSWORD_ARK_HVM_CONTROL_START_RESIDENT,
            state.response.generation, true, false, true, true);
        if (!result.io.ok) return result.io.win32Error;
        if (result.response.status != KSWORD_ARK_HVM_CONTROL_STATUS_OK)
        { log(result.io.message); return rejectedOperation(result.response.lastStatus); }
        const auto verify = client_.queryHvmStatus();
        if (!verify.io.ok || (verify.response.stateFlags & KSWORD_ARK_HVM_STATE_RESIDENT_ACTIVE) == 0 ||
            verify.response.residentProcessorCount != verify.response.processorCount)
            return ERROR_INVALID_STATE;
        ownsResident_ = true;
        return ERROR_SUCCESS;
    }

    DWORD Backend::pauseOwnedHvm(bool& restart)
    {
        restart = false;
        const auto state = client_.queryHvmStatus();
        if (!state.io.ok) return state.io.win32Error;
        if (state.response.residentProcessorCount == 0) return ERROR_SUCCESS;
        if (!ownsResident_) return ERROR_BUSY;
        const auto result = client_.controlHvm(KSWORD_ARK_HVM_CONTROL_STOP_RESIDENT,
            state.response.generation, false, false, true);
        if (!result.io.ok) return result.io.win32Error;
        if (result.response.status != KSWORD_ARK_HVM_CONTROL_STATUS_OK) return rejectedOperation(result.response.lastStatus);
        restart = true;
        return ERROR_SUCCESS;
    }

    DWORD Backend::releaseOwnedHvm()
    {
        if (!ownsResident_) return ERROR_SUCCESS;
        bool restart = false;
        DWORD error = pauseOwnedHvm(restart);
        if (error != ERROR_SUCCESS) return error;
        const auto state = client_.queryHvmStatus();
        if (!state.io.ok) return state.io.win32Error;
        const auto result = client_.controlHvm(KSWORD_ARK_HVM_CONTROL_TEARDOWN,
            state.response.generation, false, false, true);
        if (!result.io.ok) return result.io.win32Error;
        if (result.response.status != KSWORD_ARK_HVM_CONTROL_STATUS_OK) return rejectedOperation(result.response.lastStatus);
        ownsResident_ = false;
        return ERROR_SUCCESS;
    }

    void Backend::retireDetachedTarget()
    {
        if (!monitorAttachment_ || attachedPid_ == 0) return;
        HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | SYNCHRONIZE, FALSE, attachedPid_);
        bool detached = process == nullptr && GetLastError() == ERROR_INVALID_PARAMETER;
        if (process != nullptr)
        {
            BOOL present = TRUE;
            detached = WaitForSingleObject(process, 0) == WAIT_OBJECT_0 ||
                (CheckRemoteDebuggerPresent(process, &present) && !present);
            CloseHandle(process);
        }
        if (!detached) return;
        // CE calls DebugActiveProcessStop directly, outside its SDK function table.
        // Observe that lifetime transition so stale EPT leases cannot strand the
        // HVM switch after the frontend has discarded its breakpoint list.
        DWORD error = ERROR_SUCCESS;
        if (!breakpoints_.empty() || ownsResident_)
        {
            bool restart = false;
            error = pauseOwnedHvm(restart);
            while (!breakpoints_.empty() && error == ERROR_SUCCESS)
                error = removeBreakpoints(breakpoints_.begin()->first);
            if (error == ERROR_SUCCESS) error = releaseOwnedHvm();
        }
        if (error == ERROR_SUCCESS)
        {
            attachedPid_ = 0; monitorAttachment_ = false; lastEvent_ = {};
            log("Debugger detached; owned EPT stops and HVM residency retired");
        }
        else { lastError_ = error; log("Detached target retirement failed: " + std::to_string(error)); }
    }

    DWORD Backend::ensureDebugHvm()
    {
        if (!useHvm_) return ERROR_INVALID_STATE;
        KSWORD_ARK_HVM_DEBUG_REQUEST query{};
        KSWORD_ARK_HVM_DEBUG_RESPONSE response{};
        query.version = KSWORD_ARK_HVM_DEBUG_VERSION; query.size = sizeof(query);
        DWORD error = breakpointRequest(query, response);
        if (error != ERROR_SUCCESS || response.supported == 0) return error == ERROR_SUCCESS ? ERROR_NOT_SUPPORTED : error;
        const auto state = client_.queryHvmStatus();
        if (!state.io.ok) return state.io.win32Error;
        if ((state.response.stateFlags & KSWORD_ARK_HVM_STATE_RESIDENT_ACTIVE) != 0)
            return ownsResident_ ? ERROR_SUCCESS : ERROR_BUSY;
        if (!ownsResident_)
        {
            // Existing KSword preparations belong to their caller; never rebuild them implicitly.
            if ((state.response.stateFlags & KSWORD_ARK_HVM_STATE_RESOURCES_READY) != 0) return ERROR_BUSY;
            const auto prepared = client_.controlHvm(KSWORD_ARK_HVM_CONTROL_PREPARE, state.response.generation,
                false, false, true, true, false, false, false, false, true);
            if (!prepared.io.ok) return prepared.io.win32Error;
            if (prepared.response.status != KSWORD_ARK_HVM_CONTROL_STATUS_OK) return rejectedOperation(prepared.response.lastStatus);
            ownsResident_ = true;
            const auto preparedState = client_.queryHvmStatus();
            if (!preparedState.io.ok) return preparedState.io.win32Error;
            const auto tested = client_.controlHvm(KSWORD_ARK_HVM_CONTROL_SELF_TEST, preparedState.response.generation,
                true, false, true);
            if (!tested.io.ok || tested.response.status != KSWORD_ARK_HVM_CONTROL_STATUS_OK)
            {
                (void)releaseOwnedHvm();
                return tested.io.ok ? rejectedOperation(tested.response.lastStatus) : tested.io.win32Error;
            }
            ownsResident_ = true;
        }
        return startOwnedHvm();
    }

    DWORD Backend::removeBreakpoints(DWORD tid)
    {
        const auto found = breakpoints_.find(tid);
        if (found == breakpoints_.end()) return ERROR_SUCCESS;
        for (DWORD& id : found->second.ids)
        {
            if (id == 0) continue;
            KSWORD_ARK_HVM_DEBUG_REQUEST request{};
            KSWORD_ARK_HVM_DEBUG_RESPONSE response{};
            request.version = KSWORD_ARK_HVM_DEBUG_VERSION; request.size = sizeof(request);
            request.operation = KSWORD_ARK_HVM_DEBUG_REMOVE;
            request.flags = KSWORD_ARK_HVM_DEBUG_FLAG_CONFIRMED;
            request.breakpointId = id;
            const DWORD error = breakpointRequest(request, response);
            if (error != ERROR_SUCCESS && error != ERROR_NOT_FOUND) return error;
            id = 0;
        }
        breakpoints_.erase(found);
        return ERROR_SUCCESS;
    }

    DWORD Backend::installBreakpoints(DWORD tid, ThreadBreakpoints& record)
    {
        const auto addresses = debugAddresses(record.requested);
        for (DWORD index = 0; index < 4; ++index)
        {
            if (record.ids[index] != 0 || (record.requested.Dr7 & (3ULL << (index * 2))) == 0) continue;
            const DWORD kind = static_cast<DWORD>((record.requested.Dr7 >> (16 + index * 4)) & 3);
            const DWORD lengthCode = static_cast<DWORD>((record.requested.Dr7 >> (18 + index * 4)) & 3);
            KSWORD_ARK_HVM_DEBUG_REQUEST request{};
            KSWORD_ARK_HVM_DEBUG_RESPONSE response{};
            request.version = KSWORD_ARK_HVM_DEBUG_VERSION; request.size = sizeof(request);
            request.operation = KSWORD_ARK_HVM_DEBUG_ADD; request.flags = KSWORD_ARK_HVM_DEBUG_FLAG_CONFIRMED;
            request.processId = attachedPid_; request.threadId = tid; request.debugRegister = index;
            request.access = kind == 0 ? KSWORD_ARK_HVM_EPT_ACCESS_EXECUTE : kind == 1 ? KSWORD_ARK_HVM_EPT_ACCESS_WRITE : 3U;
            static constexpr DWORD lengths[] = {1, 2, 8, 4};
            request.length = kind == 0 ? 1 : lengths[lengthCode]; request.address = addresses[index];
            const DWORD error = breakpointRequest(request, response);
            // A target-exit race may return a revoked ID that still owns a pin.
            if (response.breakpointId != 0) record.ids[index] = response.breakpointId;
            if (error != ERROR_SUCCESS) return error;
            if (response.breakpointId == 0) return ERROR_INVALID_DATA;
            record.ids[index] = response.breakpointId;
        }
        return ERROR_SUCCESS;
    }

    HANDLE Backend::openThread(DWORD access, BOOL inherit, DWORD tid)
    {
        HANDLE thread = OpenThread(access | THREAD_QUERY_LIMITED_INFORMATION, inherit, tid);
        if (thread == nullptr) thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, inherit, tid);
        return thread;
    }

    BOOL Backend::getContext(HANDLE thread, LPCONTEXT context)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (context == nullptr) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
        if (!nativeContext(thread, context, false)) return FALSE;
        const auto found = breakpoints_.find(GetThreadId(thread));
        if ((context->ContextFlags & CONTEXT_DEBUG_REGISTERS) == CONTEXT_DEBUG_REGISTERS && found != breakpoints_.end())
        {
            const CONTEXT& virtualContext = found->second.requested;
            context->Dr0 = virtualContext.Dr0; context->Dr1 = virtualContext.Dr1;
            context->Dr2 = virtualContext.Dr2; context->Dr3 = virtualContext.Dr3;
            context->Dr7 = virtualContext.Dr7;
        }
        return TRUE;
    }

    BOOL Backend::setContext(HANDLE thread, const CONTEXT* context)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (context == nullptr) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
        if (!useHvm_ || (context->ContextFlags & CONTEXT_DEBUG_REGISTERS) != CONTEXT_DEBUG_REGISTERS)
        { CONTEXT native = *context; return nativeContext(thread, &native, true); }
        const DWORD tid = GetThreadId(thread);
        if (tid == 0 || attachedPid_ == 0 || GetProcessIdOfThread(thread) != attachedPid_)
        { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
        const bool enabled = (context->Dr7 & 0xffU) != 0;
        const auto found = breakpoints_.find(tid);
        if (!enabled && found == breakpoints_.end())
        { CONTEXT native = *context; return nativeContext(thread, &native, true); }
        // Validate the complete replacement before any old rule is removed.
        const auto addresses = debugAddresses(*context);
        for (DWORD index = 0; index < 4; ++index)
            if ((context->Dr7 & (3ULL << (index * 2))) != 0 &&
                (((context->Dr7 >> (16 + index * 4)) & 3) == 2 || addresses[index] == 0))
            { SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
        const bool hadPrevious = found != breakpoints_.end();
        const ThreadBreakpoints previous = hadPrevious ? found->second : ThreadBreakpoints{};
        CONTEXT saved{}; saved.ContextFlags = context->ContextFlags;
        if (!nativeContext(thread, &saved, false)) return FALSE;
        DWORD error = enabled ? ensureDebugHvm() : ERROR_SUCCESS;
        bool restart = false;
        if (error == ERROR_SUCCESS) error = pauseOwnedHvm(restart);
        if (error != ERROR_SUCCESS) { log("EPT breakpoint preparation failed: " + std::to_string(error)); SetLastError(error); return FALSE; }
        error = removeBreakpoints(tid);
        if (error == ERROR_SUCCESS && enabled)
        {
            auto& replacement = breakpoints_[tid]; replacement.requested = *context;
            error = installBreakpoints(tid, replacement);
        }
        // DR7 stays enabled in the real thread. Windows clears it on debugger detach,
        // giving VM-exit a reliable lifetime guard. Actual DR addresses are neutral.
        CONTEXT native = *context;
        native.Dr0 = 0; native.Dr1 = 0; native.Dr2 = 0; native.Dr3 = 0;
        if (error == ERROR_SUCCESS && !nativeContext(thread, &native, true)) error = GetLastError();
        if (error == ERROR_SUCCESS && restart) error = startOwnedHvm();
        if (error != ERROR_SUCCESS)
        {
            bool ignored = false;
            DWORD rollback = pauseOwnedHvm(ignored);
            if (rollback == ERROR_SUCCESS) rollback = removeBreakpoints(tid);
            if (rollback == ERROR_SUCCESS && hadPrevious)
            {
                auto& restored = breakpoints_[tid]; restored.requested = previous.requested;
                rollback = installBreakpoints(tid, restored);
            }
            if (rollback == ERROR_SUCCESS && !nativeContext(thread, &saved, true)) rollback = GetLastError();
            if (rollback == ERROR_SUCCESS && restart) rollback = startOwnedHvm();
            log("EPT breakpoint transaction failed: " + std::to_string(error) +
                "; rollback " + std::to_string(rollback));
            if (rollback != ERROR_SUCCESS) lastError_ = rollback;
        }
        else lastError_ = ERROR_SUCCESS;
        SetLastError(error);
        return error == ERROR_SUCCESS ? TRUE : FALSE;
    }

    DWORD Backend::suspendThread(HANDLE thread) { return changeSuspendCount(thread, true); }
    DWORD Backend::resumeThread(HANDLE thread) { return changeSuspendCount(thread, false); }

    BOOL Backend::attach(DWORD pid)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (attachedPid_ != 0 && attachedPid_ != pid && !breakpoints_.empty())
        { SetLastError(ERROR_BUSY); return FALSE; }
        if (!DebugActiveProcess(pid)) return FALSE;
        // Detaching or losing the debugger must leave the target alive.
        if (!DebugSetProcessKillOnExit(FALSE))
        { const DWORD error = GetLastError(); (void)DebugActiveProcessStop(pid); SetLastError(error); return FALSE; }
        attachedPid_ = pid;
        monitorAttachment_ = true;
        log("Attached debugger event transport to PID " + std::to_string(pid));
        return TRUE;
    }

    BOOL Backend::waitEvent(LPDEBUG_EVENT event, DWORD milliseconds)
    {
        // Never hold the backend lock while the debugger event thread waits.
        if (!WaitForDebugEvent(event, milliseconds)) return FALSE;
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        lastEvent_ = *event;
        if (event->dwDebugEventCode == EXCEPTION_DEBUG_EVENT)
            log("Debug exception " + std::to_string(event->u.Exception.ExceptionRecord.ExceptionCode) +
                ", PID " + std::to_string(event->dwProcessId) + ", TID " + std::to_string(event->dwThreadId));
        if (event->dwDebugEventCode == EXIT_THREAD_DEBUG_EVENT || event->dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT)
        {
            bool restart = false;
            DWORD error = pauseOwnedHvm(restart);
            if (error == ERROR_SUCCESS)
            {
                if (event->dwDebugEventCode == EXIT_THREAD_DEBUG_EVENT) error = removeBreakpoints(event->dwThreadId);
                else while (!breakpoints_.empty() && error == ERROR_SUCCESS) error = removeBreakpoints(breakpoints_.begin()->first);
            }
            if (error == ERROR_SUCCESS && event->dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT)
            { error = releaseOwnedHvm(); attachedPid_ = 0; monitorAttachment_ = false; }
            else if (error == ERROR_SUCCESS && restart) error = startOwnedHvm();
            if (error != ERROR_SUCCESS) { lastError_ = error; log("Debug target retirement failed: " + std::to_string(error)); }
        }
        return TRUE;
    }

    BOOL Backend::continueEvent(DWORD pid, DWORD tid, DWORD continueStatus)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        const auto found = breakpoints_.find(tid);
        if (useHvm_ && !breakpoints_.empty())
        {
            const auto state = client_.queryHvmStatus();
            if (!state.io.ok || (state.response.stateFlags & KSWORD_ARK_HVM_STATE_RESIDENT_ACTIVE) == 0 ||
                state.response.residentProcessorCount != state.response.processorCount)
            {
                lastError_ = state.io.ok ? ERROR_INVALID_STATE : state.io.win32Error;
                log("Continue refused: EPT debugger residency is incomplete; remove or restore the breakpoints first");
                SetLastError(lastError_); return FALSE;
            }
        }
        if (found != breakpoints_.end() && lastEvent_.dwDebugEventCode == EXCEPTION_DEBUG_EVENT &&
            lastEvent_.dwThreadId == tid && lastEvent_.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_SINGLE_STEP)
        {
            HANDLE thread = openThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
            CONTEXT context{}; context.ContextFlags = CONTEXT_CONTROL | CONTEXT_DEBUG_REGISTERS;
            if (thread == nullptr) return FALSE;
            if (nativeContext(thread, &context, false))
            {
                const auto addresses = debugAddresses(found->second.requested);
                for (DWORD index = 0; index < 4; ++index)
                    if ((context.Dr6 & (1ULL << index)) != 0 && addresses[index] == instructionPointer(context) &&
                        ((found->second.requested.Dr7 >> (16 + index * 4)) & 3) == 0)
                        context.EFlags |= 0x10000UL; // RF skips exactly the resumed execution breakpoint.
                context.ContextFlags = CONTEXT_CONTROL;
                if (!nativeContext(thread, &context, true)) { const DWORD error = GetLastError(); CloseHandle(thread); SetLastError(error); return FALSE; }
            }
            else { const DWORD error = GetLastError(); CloseHandle(thread); SetLastError(error); return FALSE; }
            CloseHandle(thread);
        }
        return ContinueDebugEvent(pid, tid, continueStatus);
    }

    bool Backend::shutdown()
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        bool restart = false;
        if (!breakpoints_.empty() && pauseOwnedHvm(restart) != ERROR_SUCCESS) return false;
        while (!breakpoints_.empty())
        {
            const DWORD tid = breakpoints_.begin()->first;
            CONTEXT native = breakpoints_.begin()->second.requested;
            native.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            HANDLE thread = openThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
            if (thread != nullptr)
            {
                const BOOL restored = nativeContext(thread, &native, true);
                const DWORD error = GetLastError(); CloseHandle(thread);
                if (!restored && error != ERROR_INVALID_PARAMETER && error != ERROR_INVALID_HANDLE) return false;
            }
            if (removeBreakpoints(tid) != ERROR_SUCCESS) return false;
        }
        if (releaseOwnedHvm() != ERROR_SUCCESS) return false;
        ownsResident_ = false; useHvm_ = false; attachedPid_ = 0; monitorAttachment_ = false;
        processIds_.clear(); driver_.reset();
        return true;
    }
}
