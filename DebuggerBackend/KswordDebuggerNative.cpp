#include "KswordDebuggerBackend.h"

#include <cstring>

namespace ksword::debugger
{
    void Backend::overlayShadowBytes(DWORD pid, std::uint64_t address, void* data, SIZE_T bytes)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (pid != attachedPid_ || data == nullptr) return;
        for (const auto point : shadowInt3_)
            if (point >= address && point - address < bytes)
                static_cast<unsigned char*>(data)[static_cast<SIZE_T>(point - address)] = 0xcc;
    }
    DWORD Backend::ensureShadowHvm()
    {
        if (!useHvm_) return ERROR_INVALID_STATE;
        for (const auto& pair : breakpoints_) if (!pair.second.shadow) return ERROR_BUSY;
        if (ownsResident_) return shadowPrepared_ ? ERROR_SUCCESS : ERROR_BUSY;
        KSWORD_ARK_DEBUGGER_REQUEST query{};
        KSWORD_ARK_DEBUGGER_RESPONSE response{};
        const DWORD capability = nativeRequest(query, response);
        if (capability != ERROR_SUCCESS) return capability;
        if ((response.capabilities & KSWORD_ARK_DEBUGGER_CAP_SHADOW_INT3) == 0) return ERROR_NOT_SUPPORTED;
        const auto state = client_.queryHvmStatus();
        if (!state.io.ok) return state.io.win32Error;
        if ((state.response.stateFlags & (KSWORD_ARK_HVM_STATE_RESOURCES_READY | KSWORD_ARK_HVM_STATE_RESIDENT_ACTIVE)) != 0)
            return ERROR_BUSY;
        const auto prepared = client_.controlHvm(KSWORD_ARK_HVM_CONTROL_PREPARE, state.response.generation,
            false, true, true, false, false, false, false, false, false, true);
        if (!prepared.io.ok) return prepared.io.win32Error;
        if (prepared.response.status != KSWORD_ARK_HVM_CONTROL_STATUS_OK)
            return ERROR_NOT_SUPPORTED;
        ownsResident_ = true; shadowPrepared_ = true;
        const auto tested = client_.controlHvm(KSWORD_ARK_HVM_CONTROL_SELF_TEST, prepared.response.newGeneration,
            true, true, true);
        if (!tested.io.ok || tested.response.status != KSWORD_ARK_HVM_CONTROL_STATUS_OK)
        {
            (void)releaseOwnedHvm();
            return tested.io.ok ? ERROR_NOT_SUPPORTED : tested.io.win32Error;
        }
        log("Execute breakpoint fallback: pinned ShadowPage INT3 using EPTP switching (MTF not required)");
        return ERROR_SUCCESS;
    }

    DWORD Backend::removeShadowInt3(std::uint64_t address)
    {
        KSWORD_ARK_DEBUGGER_REQUEST request{};
        KSWORD_ARK_DEBUGGER_RESPONSE response{};
        request.operation = KSWORD_ARK_DEBUGGER_SHADOW_REMOVE;
        request.flags = KSWORD_ARK_DEBUGGER_FLAG_CONFIRMED;
        request.processId = attachedPid_; request.address = address; request.bytes = 1;
        const DWORD error = nativeRequest(request, response);
        if (error == ERROR_SUCCESS || error == ERROR_NOT_FOUND) { shadowInt3_.erase(address); return ERROR_SUCCESS; }
        return error;
    }

    DWORD Backend::addShadowInt3(std::uint64_t address)
    {
        KSWORD_ARK_DEBUGGER_REQUEST request{};
        KSWORD_ARK_DEBUGGER_RESPONSE response{};
        request.operation = KSWORD_ARK_DEBUGGER_SHADOW_ADD; request.flags = KSWORD_ARK_DEBUGGER_FLAG_CONFIRMED;
        request.processId = attachedPid_; request.address = address; request.bytes = 1;
        const DWORD error = nativeRequest(request, response);
        if (error == ERROR_SUCCESS) shadowInt3_.insert(address);
        return error;
    }

    DWORD Backend::removeShadowRecord(DWORD tid)
    {
        const auto found = breakpoints_.find(tid);
        if (found == breakpoints_.end()) return ERROR_SUCCESS;
        const CONTEXT& context = found->second.requested;
        const std::uint64_t addresses[] = {context.Dr0, context.Dr1, context.Dr2, context.Dr3};
        for (DWORD slot = 0; slot < 4; ++slot)
        {
            if (found->second.ids[slot] == 0) continue;
            if ((context.Dr7 & (3ULL << (2 * slot))) == 0 || ((context.Dr7 >> (16 + 4 * slot)) & 3) != 0) continue;
            const auto reference = shadowReferences_.find(addresses[slot]);
            if (reference == shadowReferences_.end()) continue;
            if (reference->second > 1) --reference->second;
            else
            {
                if (shadowInt3_.count(addresses[slot]) != 0)
                {
                    const DWORD error = removeShadowInt3(addresses[slot]);
                    if (error != ERROR_SUCCESS) return error;
                }
                shadowReferences_.erase(reference);
            }
        }
        breakpoints_.erase(found); return ERROR_SUCCESS;
    }

    BOOL Backend::applyShadowContext(HANDLE thread, const CONTEXT* context)
    {
        if (!validTargetThread(thread)) { SetLastError(ERROR_INVALID_STATE); return FALSE; }
        const DWORD tid = GetThreadId(thread);
        CONTEXT saved{}; saved.ContextFlags = context->ContextFlags;
        if (!nativeContext(thread, &saved, false)) return FALSE;
        const std::uint64_t addresses[] = {context->Dr0, context->Dr1, context->Dr2, context->Dr3};
        bool execute = false;
        for (DWORD slot = 0; slot < 4; ++slot)
        {
            if ((context->Dr7 & (3ULL << (2 * slot))) == 0) continue;
            const DWORD type = static_cast<DWORD>((context->Dr7 >> (16 + 4 * slot)) & 3);
            if (addresses[slot] == 0 || type == 2 || (type == 0 && ((context->Dr7 >> (18 + 4 * slot)) & 3) != 0))
            { SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
            execute |= type == 0;
        }
        const auto previous = breakpoints_.find(tid);
        const bool hadPrevious = previous != breakpoints_.end();
        const ThreadBreakpoints old = hadPrevious ? previous->second : ThreadBreakpoints{};
        if (hadPrevious && (!old.shadow || !matchesThreadRecord(thread, old)))
        { SetLastError(ERROR_INVALID_STATE); return FALSE; }
        HANDLE retained = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), thread, GetCurrentProcess(), &retained, 0, FALSE, DUPLICATE_SAME_ACCESS)) return FALSE;
        ThreadBreakpoints replacement{}; replacement.requested = *context;
        replacement.identity = std::make_shared<ark::DriverHandle>(retained);
        replacement.generation = sessionGeneration_; replacement.shadow = true;
        DWORD error = execute ? ensureShadowHvm() : ERROR_SUCCESS;
        bool restart = false;
        if (error == ERROR_SUCCESS) error = pauseOwnedHvm(restart);
        if (error == ERROR_SUCCESS && hadPrevious) error = removeShadowRecord(tid);
        const auto install = [&](const ThreadBreakpoints& record) -> DWORD {
            breakpoints_[tid] = record;
            breakpoints_[tid].ids = {};
            const auto& value = record.requested;
            const std::uint64_t points[] = {value.Dr0,value.Dr1,value.Dr2,value.Dr3};
            for (DWORD slot = 0; slot < 4; ++slot)
            {
                if ((value.Dr7 & (3ULL << (2 * slot))) == 0 || ((value.Dr7 >> (16 + 4 * slot)) & 3) != 0) continue;
                if (shadowReferences_.count(points[slot]) == 0)
                {
                    const DWORD added = addShadowInt3(points[slot]);
                    if (added != ERROR_SUCCESS) return added;
                }
                ++shadowReferences_[points[slot]];
                breakpoints_[tid].ids[slot] = 1;
            }
            return ERROR_SUCCESS;
        };
        if (error == ERROR_SUCCESS && (context->Dr7 & 0xff) != 0) error = install(replacement);
        CONTEXT native = *context;
        std::uint64_t* nativeAddresses[] = {&native.Dr0,&native.Dr1,&native.Dr2,&native.Dr3};
        for (DWORD slot = 0; slot < 4; ++slot)
            if ((context->Dr7 & (3ULL << (2 * slot))) != 0 && ((context->Dr7 >> (16 + 4 * slot)) & 3) == 0)
            { *nativeAddresses[slot] = 0; native.Dr7 &= ~(3ULL << (2 * slot)); }
        if (error == ERROR_SUCCESS && !nativeContext(thread, &native, true)) error = GetLastError();
        if (error == ERROR_SUCCESS && !shadowInt3_.empty()) error = startOwnedHvm();
        if (error != ERROR_SUCCESS)
        {
            bool ignored = false;
            DWORD rollback = pauseOwnedHvm(ignored);
            if (rollback == ERROR_SUCCESS) rollback = removeShadowRecord(tid);
            if (rollback == ERROR_SUCCESS && hadPrevious) rollback = install(old);
            if (rollback == ERROR_SUCCESS && !nativeContext(thread, &saved, true)) rollback = GetLastError();
            if (rollback == ERROR_SUCCESS && !shadowInt3_.empty()) rollback = startOwnedHvm();
            log("ShadowPage context transaction failed: " + std::to_string(error) + "; rollback " + std::to_string(rollback));
            if (rollback != ERROR_SUCCESS) lastError_ = rollback;
        }
        if (shadowInt3_.empty()) (void)releaseOwnedHvm();
        SetLastError(error); return error == ERROR_SUCCESS;
    }

    DWORD Backend::beginShadowStep(DWORD tid, bool automatic)
    {
        const auto found = shadowStops_.find(tid);
        if (found == shadowStops_.end() || found->second.stepping) return ERROR_SUCCESS;
        ark::DriverHandle thread(openThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, tid));
        if (!thread.isValid()) return GetLastError();
        CONTEXT context{}; context.ContextFlags = CONTEXT_CONTROL | CONTEXT_DEBUG_REGISTERS;
        if (!nativeContext(thread.native(), &context, false)) return GetLastError();
        bool restart = false;
        DWORD error = pauseOwnedHvm(restart);
        if (error == ERROR_SUCCESS) error = removeShadowInt3(found->second.address);
        if (error != ERROR_SUCCESS) return error;
        found->second.userStep = !automatic && (context.EFlags & 0x100) != 0;
        context.Rip = found->second.address; context.EFlags |= 0x100;
        context.Dr6 &= ~static_cast<DWORD64>(found->second.mask);
        if (!nativeContext(thread.native(), &context, true)) return GetLastError();
        found->second.stepping = true;
        return shadowInt3_.empty() ? ERROR_SUCCESS : startOwnedHvm();
    }

    DWORD Backend::shadowEvent(DEBUG_EVENT& event, bool& consume)
    {
        consume = false;
        if (event.dwDebugEventCode != EXCEPTION_DEBUG_EVENT) return ERROR_SUCCESS;
        const auto pending = shadowStops_.find(event.dwThreadId);
        if (pending != shadowStops_.end() && pending->second.stepping)
        {
            const auto stop = pending->second;
            bool restart = false;
            DWORD error = pauseOwnedHvm(restart);
            if (error == ERROR_SUCCESS && shadowReferences_.count(stop.address) != 0) error = addShadowInt3(stop.address);
            if (error == ERROR_SUCCESS && !shadowInt3_.empty()) error = startOwnedHvm();
            if (error != ERROR_SUCCESS) return error;
            ark::DriverHandle thread(openThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, event.dwThreadId));
            CONTEXT context{}; context.ContextFlags = CONTEXT_CONTROL | CONTEXT_DEBUG_REGISTERS;
            if (!thread.isValid() || !nativeContext(thread.native(), &context, false)) return GetLastError();
            if (!stop.userStep) context.EFlags &= ~0x100UL;
            context.Dr6 &= ~static_cast<DWORD64>(stop.mask);
            if (!nativeContext(thread.native(), &context, true)) return GetLastError();
            consume = !stop.userStep && event.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_SINGLE_STEP;
            shadowStops_.erase(pending);
            if (shadowInt3_.empty()) (void)releaseOwnedHvm();
            return ERROR_SUCCESS;
        }
        if (event.u.Exception.ExceptionRecord.ExceptionCode != EXCEPTION_BREAKPOINT) return ERROR_SUCCESS;
        const auto address = reinterpret_cast<std::uintptr_t>(event.u.Exception.ExceptionRecord.ExceptionAddress);
        if (shadowReferences_.count(address) == 0) return ERROR_SUCCESS;
        DWORD mask = 0;
        const auto record = breakpoints_.find(event.dwThreadId);
        if (record != breakpoints_.end() && record->second.shadow)
        {
            const auto& value = record->second.requested;
            const std::uint64_t points[] = {value.Dr0,value.Dr1,value.Dr2,value.Dr3};
            for (DWORD slot = 0; slot < 4; ++slot)
                if (points[slot] == address && (value.Dr7 & (3ULL << (2 * slot))) != 0 && ((value.Dr7 >> (16 + slot * 4)) & 3) == 0)
                    mask |= 1UL << slot;
        }
        shadowStops_[event.dwThreadId] = {address,mask,false,false};
        log("ShadowPage INT3 stop: TID " + std::to_string(event.dwThreadId) +
            ", address " + std::to_string(address) + ", slot mask " + std::to_string(mask));
        if (mask == 0) { consume = true; return beginShadowStep(event.dwThreadId, true); }
        ark::DriverHandle thread(openThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, event.dwThreadId));
        CONTEXT context{}; context.ContextFlags = CONTEXT_CONTROL | CONTEXT_DEBUG_REGISTERS;
        if (!thread.isValid() || !nativeContext(thread.native(), &context, false)) return GetLastError();
        context.Rip = address; context.Dr6 |= mask;
        if (!nativeContext(thread.native(), &context, true)) return GetLastError();
        // Translate this held, owned INT3 stop into CE's hardware-stop contract.
        event.u.Exception.ExceptionRecord.ExceptionCode = EXCEPTION_SINGLE_STEP;
        return ERROR_SUCCESS;
    }

    bool Backend::shadowWrite(DWORD pid, std::uint64_t address, const void* data, SIZE_T bytes,
        SIZE_T& transferred, bool& handled)
    {
        handled = false;
        const bool removing = shadowInt3_.find(address) != shadowInt3_.end();
        const bool adding = pid == attachedPid_ && attachedIdentity_.isValid() &&
            bytes == 1 && data != nullptr && *static_cast<const unsigned char*>(data) == 0xcc;
        if (adding && !shadowPrepared_ && status().eptBreakpointProtocol != 0) return false;
        if (!removing && !adding)
        {
            for (const auto point : shadowInt3_)
                if (bytes != 0 && address < (point & ~0xfffULL) + 4096 && address + bytes > (point & ~0xfffULL))
                { handled = true; SetLastError(ERROR_BUSY); return false; }
            return false;
        }
        handled = true;
        if (pid != attachedPid_ || bytes != 1 || lastEvent_.dwProcessId != pid)
        { SetLastError(ERROR_INVALID_STATE); return false; }
        const bool set = *static_cast<const unsigned char*>(data) == 0xcc;
        if (removing && !set)
        {
            unsigned char original = 0; SIZE_T read = 0;
            if (!transfer(false, pid, address, &original, 1, read)) return false;
            if (original != *static_cast<const unsigned char*>(data))
            { SetLastError(ERROR_BUSY); return false; }
        }
        DWORD error = set ? ensureShadowHvm() : ERROR_SUCCESS;
        bool restart = false;
        if (error == ERROR_SUCCESS) error = pauseOwnedHvm(restart);
        if (error == ERROR_SUCCESS)
        {
            if (set)
            {
                KSWORD_ARK_DEBUGGER_REQUEST request{};
                KSWORD_ARK_DEBUGGER_RESPONSE response{};
                request.operation = KSWORD_ARK_DEBUGGER_SHADOW_ADD; request.flags = KSWORD_ARK_DEBUGGER_FLAG_CONFIRMED;
                request.processId = pid; request.address = address; request.bytes = 1;
                error = nativeRequest(request, response);
                if (error == ERROR_SUCCESS) shadowInt3_.insert(address);
            }
            else error = removeShadowInt3(address);
        }
        if (error == ERROR_SUCCESS)
            error = shadowInt3_.empty() ? releaseOwnedHvm() : startOwnedHvm();
        else if (restart && !shadowInt3_.empty()) (void)startOwnedHvm();
        else if (shadowInt3_.empty()) (void)releaseOwnedHvm();
        transferred = error == ERROR_SUCCESS ? 1 : 0;
        if (error != ERROR_SUCCESS) log("ShadowPage INT3 operation failed: " + std::to_string(error));
        SetLastError(error); return error == ERROR_SUCCESS;
    }

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
        if (nativeContextFallback_)
            return write ? ::SetThreadContext(thread, context) : ::GetThreadContext(thread, context);
        KSWORD_ARK_DEBUGGER_REQUEST request{};
        KSWORD_ARK_DEBUGGER_RESPONSE response{};
        request.operation = write ? KSWORD_ARK_DEBUGGER_SET_CONTEXT : KSWORD_ARK_DEBUGGER_GET_CONTEXT;
        request.flags = write ? KSWORD_ARK_DEBUGGER_FLAG_CONFIRMED : 0;
        request.processId = GetProcessIdOfThread(thread); request.threadId = GetThreadId(thread);
        request.contextFlags = context->ContextFlags;
        std::memcpy(request.context, context, sizeof(*context));
        const DWORD error = nativeRequest(request, response);
        // Some Windows builds export PsGet/SetContextThread but reject the
        // target's context APC. Preserve actual Windows debug-port semantics.
        // Identity, access and policy rejections must not trigger this fallback.
        if (error == ERROR_NOT_SUPPORTED || (!write && error == ERROR_GEN_FAILURE))
        {
            const BOOL completed = write ? ::SetThreadContext(thread, context) : ::GetThreadContext(thread, context);
            if (completed)
            {
                nativeContextFallback_ = true;
                log("R0 context unavailable (error " + std::to_string(error) +
                    "); using Windows thread context for this adapter session");
            }
            return completed;
        }
        if (error != ERROR_SUCCESS) log("R0 context " + std::string(write ? "write" : "read") +
            " failed, PID " + std::to_string(request.processId) + ", TID " + std::to_string(request.threadId) +
            ", error " + std::to_string(error));
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
        if (error == ERROR_NOT_SUPPORTED)
        {
            log("R0 suspend-count export unavailable; using Windows " + std::string(suspend ? "SuspendThread" : "ResumeThread"));
            return suspend ? ::SuspendThread(thread) : ::ResumeThread(thread);
        }
        SetLastError(error); return error == ERROR_SUCCESS ? response.previousSuspendCount : MAXDWORD;
    }
}
