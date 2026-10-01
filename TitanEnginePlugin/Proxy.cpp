#include "Proxy.h"
#include "MemoryLifetime.h"
#include <mutex>
#include <unordered_map>
#include <vector>

namespace ksword::titan
{
    namespace
    {
        struct HardwareBinding
        {
            ULONG_PTR address;
            DWORD index;
            TitanHardwareBreakpointType type;
            TitanHardwareBreakpointSize size;
            TITANCBHWBP callback;
            DWORD processId;
            bool hvm;
            ULONG_PTR allocationBase;
            bool shadow = false;
        };
        std::recursive_mutex bindingMutex;
        std::unordered_map<DWORD, HardwareBinding> hardwareBindings;
        struct DeferredStep { TITANCBSTEP callback; DWORD pid; std::uint64_t event; bool over; };
        std::unordered_map<DWORD, DeferredStep> deferredSteps;
        void shadowHit(DWORD index)
        {
            TITANCBHWBP callback = nullptr; ULONG_PTR address = 0;
            {
                std::lock_guard<std::recursive_mutex> lock(bindingMutex);
                const auto found = hardwareBindings.find(index);
                if (found == hardwareBindings.end() || !found->second.shadow) return;
                callback = found->second.callback; address = found->second.address;
            }
            LogicalReadScope frontend(false);
            if (callback != nullptr) callback(reinterpret_cast<void*>(address));
        }
        void shadow0() { shadowHit(UE_DR0); }
        void shadow1() { shadowHit(UE_DR1); }
        void shadow2() { shadowHit(UE_DR2); }
        void shadow3() { shadowHit(UE_DR3); }
        TITANCBSOFTBP shadowCallbacks[] = {&shadow0, &shadow1, &shadow2, &shadow3};

        void completeStep()
        {
            TITANCBSTEP callback = nullptr;
            {
                std::lock_guard<std::recursive_mutex> policy(policyMutex);
                const auto* event = nativeApi.GetDebugData();
                if (event == nullptr) return;
                const auto found = deferredSteps.find(event->dwThreadId);
                if (found == deferredSteps.end() || found->second.pid != event->dwProcessId) return;
                const auto pending = found->second;
                // Only a subsequent real event may complete the frontend step.
                if (nativeEventSequence() <= pending.event)
                {
                    if (pending.over) nativeApi.StepOver(&completeStep);
                    else nativeApi.StepInto(&completeStep);
                    return;
                }
                deferredSteps.erase(found);
                callback = pending.callback;
            }
            // A real frontend callback can wait for Run while its UI reads
            // context. Never retain the adapter policy lock across that wait.
            LogicalReadScope frontend(false);
            if (callback != nullptr) callback();
        }

        void armStep(TITANCBSTEP callback, bool over)
        {
            if (ensureNative() != ERROR_SUCCESS) return;
            std::lock_guard<std::recursive_mutex> policy(policyMutex);
            const auto* event = nativeApi.GetDebugData();
            if (event == nullptr || event->dwProcessId == 0 || event->dwThreadId == 0)
            { SetLastError(ERROR_INVALID_STATE); return; }
            bool hardwareStop = false;
            if (event->dwDebugEventCode == EXCEPTION_DEBUG_EVENT)
            {
                const auto address = reinterpret_cast<ULONG_PTR>(event->u.Exception.ExceptionRecord.ExceptionAddress);
                std::lock_guard<std::recursive_mutex> lock(bindingMutex);
                for (const auto& pair : hardwareBindings)
                    if (pair.second.processId == event->dwProcessId && pair.second.address == address &&
                        event->u.Exception.ExceptionRecord.ExceptionCode ==
                            (pair.second.shadow ? EXCEPTION_BREAKPOINT : EXCEPTION_SINGLE_STEP)) hardwareStop = true;
            }
            if (!hardwareStop)
            {
                // Ordinary steps already have the native engine's complete
                // contract. Only its callback-in-hardware-event quirk needs
                // deferral to a later Windows event.
                if (over) nativeApi.StepOver(callback);
                else nativeApi.StepInto(callback);
                return;
            }
            deferredSteps[event->dwThreadId] = {callback, event->dwProcessId, nativeEventSequence(), over};
            if (over) nativeApi.StepOver(&completeStep);
            else nativeApi.StepInto(&completeStep);
        }
    }

    bool hasHardwareBindings() { std::lock_guard<std::recursive_mutex> lock(bindingMutex); return !hardwareBindings.empty(); }
    void clearHardwareBindings() { std::lock_guard<std::recursive_mutex> lock(bindingMutex); hardwareBindings.clear(); }
    void clearDeferredSteps() { deferredSteps.clear(); }

    bool setHardware(ULONG_PTR address, DWORD index, TitanHardwareBreakpointType type,
        TitanHardwareBreakpointSize size, TITANCBHWBP callback)
    {
        if (ensureNative() != ERROR_SUCCESS) return false;
        std::lock_guard<std::recursive_mutex> policy(policyMutex);
        if (hvmSelected.load() && type != UE_HARDWARE_EXECUTE)
        {
            log("HVM hardware data breakpoints rejected: EPT data coverage is 4 KiB; use native mode or the explicit Watch API");
            SetLastError(ERROR_NOT_SUPPORTED); return false;
        }
        std::lock_guard<std::recursive_mutex> lock(bindingMutex);
        ULONG_PTR allocationBase = 0;
        if (hvmSelected.load())
        {
            const DWORD pid = nativeProcessId();
            HANDLE process = pid == 0 ? nullptr : nativeApi.TitanOpenProcess(PROCESS_QUERY_INFORMATION, false, pid);
            if (process == nullptr) { SetLastError(pid == 0 ? ERROR_INVALID_STATE : GetLastError()); return false; }
            MEMORY_BASIC_INFORMATION info{};
            const SIZE_T queried = nativeApi.MemoryQuerySafe(process, reinterpret_cast<void*>(address), &info, sizeof(info));
            const DWORD queryError = GetLastError();
            (void)nativeApi.TitanCloseHandle(process);
            if (queried != sizeof(info) || info.State != MEM_COMMIT || info.AllocationBase == nullptr)
            { SetLastError(queried == sizeof(info) || queryError == ERROR_SUCCESS ? ERROR_INVALID_ADDRESS : queryError); return false; }
            allocationBase = reinterpret_cast<ULONG_PTR>(info.AllocationBase);
        }
        DWORD selected = index;
        const bool shadow = hvmSelected.load() && debugger::backend().status().eptBreakpointProtocol == 0;
        if (selected == 0)
        {
            if (shadow)
            {
                for (selected = UE_DR0; selected <= UE_DR3; ++selected)
                    if (hardwareBindings.find(selected) == hardwareBindings.end()) break;
                if (selected > UE_DR3) { SetLastError(ERROR_NO_SYSTEM_RESOURCES); return false; }
            }
            else if (!nativeApi.GetUnusedHardwareBreakPointRegister(&selected)) return false;
        }
        if (selected < UE_DR0 || selected > UE_DR3) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
        const auto found = hardwareBindings.find(selected);
        const bool hadPrevious = found != hardwareBindings.end();
        const HardwareBinding previous = hadPrevious ? found->second : HardwareBinding{};
        if (shadow)
        {
            if (size != UE_HARDWARE_SIZE_1)
            { SetLastError(ERROR_NOT_SUPPORTED); return false; }
            if (hadPrevious && !previous.shadow) { SetLastError(ERROR_BUSY); return false; }
            for (const auto& pair : hardwareBindings)
                if (pair.first != selected && pair.second.address == address) { SetLastError(ERROR_BUSY); return false; }
            LogicalReadScope native;
            if (hadPrevious && previous.address == address)
            {
                hardwareBindings[selected].callback = callback;
                return true;
            }
            // Do not take over an independent software breakpoint at this VA.
            if (nativeApi.IsBPXEnabled(address)) { SetLastError(ERROR_BUSY); return false; }
            if (hadPrevious && !nativeApi.DeleteBPX(previous.address)) return false;
            const bool result = nativeApi.SetBPX(address, UE_BREAKPOINT | UE_BREAKPOINT_TYPE_INT3,
                shadowCallbacks[selected - UE_DR0]);
            const DWORD installError = GetLastError();
            if (result)
            {
                hardwareBindings[selected] = {address, selected, type, size, callback,
                    nativeProcessId(), true, allocationBase, true};
                log("Execute breakpoint uses ShadowPage hidden INT3 fallback");
            }
            else if (hadPrevious && !nativeApi.SetBPX(previous.address, UE_BREAKPOINT | UE_BREAKPOINT_TYPE_INT3,
                shadowCallbacks[selected - UE_DR0]))
            {
                log("ShadowPage hardware replacement rollback failed");
                hardwareBindings.erase(selected);
            }
            SetLastError(installError);
            return result;
        }
        if (hvmSelected.load() && getSeamError() != ERROR_SUCCESS)
        { SetLastError(getSeamError()); return false; }
        const bool result = nativeApi.SetHardwareBreakPoint(address, selected, type, size, callback);
        const DWORD error = GetLastError();
        const DWORD hookError = hvmSelected.load() ? getSeamError() : ERROR_SUCCESS;
        if (hookError != ERROR_SUCCESS || (hvmSelected.load() && hasPendingDebug()))
        {
            // Native Titan ignores low-level SetThreadContext failures. Undo its
            // callback table before exposing a failed installation to x64dbg.
            clearSeamError();
            bool restored = nativeApi.DeleteHardwareBreakPoint(selected) && getSeamError() == ERROR_SUCCESS && !hasPendingDebug();
            if (restored && hadPrevious)
            {
                clearSeamError();
                restored = nativeApi.SetHardwareBreakPoint(previous.address, previous.index,
                    previous.type, previous.size, previous.callback) && getSeamError() == ERROR_SUCCESS && !hasPendingDebug();
            }
            if (restored) clearSeamError();
            log("EPT hardware installation failed; native callback rollback " + std::string(restored ? "completed" : "failed"));
            SetLastError(hookError == ERROR_SUCCESS ? ERROR_INVALID_STATE : hookError); return false;
        }
        if (result) hardwareBindings[selected] = {address, selected, type, size, callback,
            nativeProcessId(), hvmSelected.load(), allocationBase};
        SetLastError(error); return result;
    }

    bool deleteHardware(DWORD index)
    {
        if (ensureNative() != ERROR_SUCCESS) return false;
        std::lock_guard<std::recursive_mutex> policy(policyMutex);
        std::lock_guard<std::recursive_mutex> lock(bindingMutex);
        const auto found = hardwareBindings.find(index);
        LogicalReadScope native;
        const bool result = found != hardwareBindings.end() && found->second.shadow
            ? nativeApi.DeleteBPX(found->second.address) : nativeApi.DeleteHardwareBreakPoint(index);
        const DWORD nativeError = GetLastError();
        if (result && (!hvmSelected.load() || (getSeamError() == ERROR_SUCCESS && !hasPendingDebug())))
        { hardwareBindings.erase(index); SetLastError(nativeError); return true; }
        if (hvmSelected.load() && getSeamError() != ERROR_SUCCESS) SetLastError(getSeamError());
        return false;
    }

    DWORD retireNativeModule(DWORD pid, ULONG_PTR allocationBase)
    {
        std::lock_guard<std::recursive_mutex> policy(policyMutex);
        std::lock_guard<std::recursive_mutex> lock(bindingMutex);
        std::vector<DWORD> affected;
        for (const auto& pair : hardwareBindings)
            if (moduleOwnsBinding(pair.second.hvm, pair.second.processId,
                pair.second.allocationBase, pid, allocationBase)) affected.push_back(pair.first);
        for (const DWORD index : affected)
            if (!deleteHardware(index))
            {
                const DWORD error = GetLastError();
                log("Unloaded module EPT breakpoint retirement failed: " + std::to_string(error));
                return error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error;
            }
        return ERROR_SUCCESS;
    }

    bool freeMemory(HANDLE process, LPVOID address, SIZE_T bytes, DWORD type)
    {
        if (ensureNative() != ERROR_SUCCESS) return false;
        std::lock_guard<std::recursive_mutex> policy(policyMutex);
        std::lock_guard<std::recursive_mutex> lock(bindingMutex);
        const DWORD pid = GetProcessId(process);
        std::vector<DWORD> affected;
        const auto start = reinterpret_cast<ULONG_PTR>(address);
        SYSTEM_INFO system{}; GetSystemInfo(&system);
        for (const auto& pair : hardwareBindings)
        {
            const auto& binding = pair.second;
            if (!binding.hvm || binding.processId != pid) continue;
            bool overlap = false;
            if ((type & (MEM_RELEASE | MEM_DECOMMIT)) != 0 && bytes == 0)
                overlap = binding.allocationBase == start;
            else if ((type & MEM_DECOMMIT) != 0)
                overlap = decommitCovers(start, bytes, binding.address, system.dwPageSize);
            if (overlap) affected.push_back(pair.first);
        }
        const bool result = nativeApi.MemoryFreeSafe(process, address, bytes, type);
        const DWORD error = GetLastError();
        if (result)
            for (const DWORD index : affected)
                if (!deleteHardware(index)) log("Freed allocation EPT breakpoint retirement failed; continuation remains blocked");
        SetLastError(error); return result;
    }
}

extern "C"
{
    void StepInto(TITANCBSTEP callback) { ksword::titan::armStep(callback, false); }
    void StepOver(TITANCBSTEP callback) { ksword::titan::armStep(callback, true); }
    bool MemoryFreeSafe(HANDLE process, LPVOID address, SIZE_T bytes, DWORD type)
    { return ksword::titan::freeMemory(process, address, bytes, type); }
    PROCESS_INFORMATION* InitDebugW(const wchar_t* file, const wchar_t* command, const wchar_t* folder)
    {
        if (ksword::titan::ensureNative() != ERROR_SUCCESS) return nullptr;
        return ksword::titan::nativeApi.InitDebugW(file, command, folder);
    }

    bool StopDebug()
    {
        using namespace ksword::titan;
        if (ensureNative() != ERROR_SUCCESS) return false;
        std::lock_guard<std::recursive_mutex> policy(policyMutex);
        const DWORD error = releaseSession();
        if (error != ERROR_SUCCESS) { SetLastError(error); log("Stop refused: owned EPT retirement failed"); return false; }
        return nativeApi.StopDebug();
    }

    bool DetachDebuggerEx(DWORD pid)
    {
        using namespace ksword::titan;
        if (ensureNative() != ERROR_SUCCESS) return false;
        std::lock_guard<std::recursive_mutex> policy(policyMutex);
        const DWORD error = releaseSession();
        if (error != ERROR_SUCCESS) { SetLastError(error); log("Detach refused: owned EPT retirement failed"); return false; }
        return nativeApi.DetachDebuggerEx(pid);
    }

    bool TitanTerminateProcess(HANDLE process, DWORD exitCode)
    {
        using namespace ksword::titan;
        if (ensureNative() != ERROR_SUCCESS) return false;
        std::lock_guard<std::recursive_mutex> policy(policyMutex);
        const auto state = ksword::debugger::backend().status();
        if (state.attachedProcessId != 0 && GetProcessId(process) == state.attachedProcessId)
        {
            const DWORD error = releaseSession();
            if (error != ERROR_SUCCESS) { SetLastError(error); return false; }
        }
        return nativeApi.TitanTerminateProcess(process, exitCode);
    }

    bool TitanTerminateThread(HANDLE thread, DWORD exitCode)
    {
        using namespace ksword::titan;
        if (ensureNative() != ERROR_SUCCESS) return false;
        std::lock_guard<std::recursive_mutex> policy(policyMutex);
        if (hvmSelected.load())
        {
            const DWORD error = ksword::debugger::backend().retireNativeThread(GetThreadId(thread));
            if (error != ERROR_SUCCESS) { SetLastError(error); return false; }
        }
        return nativeApi.TitanTerminateThread(thread, exitCode);
    }

    bool SetHardwareBreakPoint(ULONG_PTR address, DWORD index, TitanHardwareBreakpointType type,
        TitanHardwareBreakpointSize size, TITANCBHWBP callback)
    { return ksword::titan::setHardware(address, index, type, size, callback); }

    bool DeleteHardwareBreakPoint(DWORD index) { return ksword::titan::deleteHardware(index); }

    bool RemoveAllBreakPoints(TitanBreakpointRemoveOption option)
    {
        using namespace ksword::titan;
        if (ensureNative() != ERROR_SUCCESS) return false;
        std::lock_guard<std::recursive_mutex> policy(policyMutex);
        LogicalReadScope native;
        const bool result = nativeApi.RemoveAllBreakPoints(option);
        if (hvmSelected.load() && (getSeamError() != ERROR_SUCCESS || hasPendingDebug()))
        { SetLastError(getSeamError() == ERROR_SUCCESS ? ERROR_INVALID_STATE : getSeamError()); return false; }
        if (result && option == UE_OPTION_REMOVEALL) clearHardwareBindings();
        return result;
    }

    void DebugLoop()
    {
        using namespace ksword::titan;
        if (ensureNative() != ERROR_SUCCESS) return;
        LogicalReadScope native;
        nativeApi.DebugLoop();
        const DWORD nativeError = GetLastError();
        const DWORD cleanup = releaseSession();
        if (cleanup != ERROR_SUCCESS) log("Native debug loop exited with owned EPT cleanup failure: " + std::to_string(cleanup));
        SetLastError(cleanup == ERROR_SUCCESS ? nativeError : cleanup);
    }

    bool AttachDebugger(DWORD pid, bool kill, LPVOID info, TITANCALLBACK callback)
    {
        using namespace ksword::titan;
        if (ensureNative() != ERROR_SUCCESS) return false;
        // Native AttachDebugger owns its entire DebugLoop internally.
        LogicalReadScope native;
        const bool result = nativeApi.AttachDebugger(pid, kill, info, callback);
        const DWORD nativeError = GetLastError();
        const DWORD cleanup = releaseSession();
        if (cleanup != ERROR_SUCCESS) log("Native attach loop exited with owned EPT cleanup failure: " + std::to_string(cleanup));
        SetLastError(cleanup == ERROR_SUCCESS ? nativeError : cleanup);
        return result;
    }

    bool MemoryReadSafe(HANDLE process, LPVOID address, LPVOID data, SIZE_T bytes, SIZE_T* done)
    {
        using namespace ksword::titan;
        if (ensureNative() != ERROR_SUCCESS) return false;
        LogicalReadScope frontend(false); return nativeApi.MemoryReadSafe(process, address, data, bytes, done);
    }
    bool MemoryReadUnsafe(HANDLE process, LPCVOID address, LPVOID data, SIZE_T bytes, SIZE_T* done)
    {
        using namespace ksword::titan;
        if (ensureNative() != ERROR_SUCCESS) return false;
        LogicalReadScope frontend(false); return nativeApi.MemoryReadUnsafe(process, address, data, bytes, done);
    }
    bool SetBPX(ULONG_PTR address, DWORD type, TITANCBSOFTBP callback)
    {
        using namespace ksword::titan;
        if (ensureNative() != ERROR_SUCCESS) return false;
        LogicalReadScope native; return nativeApi.SetBPX(address, type, callback);
    }
    bool DeleteBPX(ULONG_PTR address)
    {
        using namespace ksword::titan;
        if (ensureNative() != ERROR_SUCCESS) return false;
        LogicalReadScope native; return nativeApi.DeleteBPX(address);
    }
    bool IsBPXEnabled(ULONG_PTR address)
    {
        using namespace ksword::titan;
        if (ensureNative() != ERROR_SUCCESS) return false;
        LogicalReadScope native; return nativeApi.IsBPXEnabled(address);
    }
    bool GetUnusedHardwareBreakPointRegister(LPDWORD index)
    {
        using namespace ksword::titan;
        if (ensureNative() != ERROR_SUCCESS || index == nullptr) return false;
        if (!hvmSelected.load() || ksword::debugger::backend().status().eptBreakpointProtocol != 0)
            return nativeApi.GetUnusedHardwareBreakPointRegister(index);
        std::lock_guard<std::recursive_mutex> lock(bindingMutex);
        for (DWORD slot = UE_DR0; slot <= UE_DR3; ++slot)
            if (hardwareBindings.find(slot) == hardwareBindings.end()) { *index = slot; return true; }
        SetLastError(ERROR_NO_SYSTEM_RESOURCES); return false;
    }

    bool EngineCheckStructAlignment(TitanStructureType type, ULONG_PTR bytes)
    {
        using namespace ksword::titan;
        if (ensureNative() != ERROR_SUCCESS) return false;
        if (type == UE_STRUCT_TITAN_ENGINE_CONTEXT && bytes != sizeof(TITAN_ENGINE_CONTEXT_t))
        { SetLastError(ERROR_BAD_LENGTH); return false; }
        return nativeApi.EngineCheckStructAlignment(type, bytes);
    }
}

static_assert(sizeof(TITAN_SESSION_INFO) == 32, "Canonical live session ABI");
static_assert(sizeof(TITAN_REPLAY_POSITION) == 16, "Canonical replay ABI");
