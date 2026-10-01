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
        std::recursive_mutex softwareOperationMutex;
        DWORD pendingBindingChanges = 0;
        struct BindingTransition
        {
            BindingTransition()
            {
                std::lock_guard<std::recursive_mutex> policy(policyMutex);
                std::lock_guard<std::recursive_mutex> lock(bindingMutex);
                ++pendingBindingChanges;
            }
            ~BindingTransition()
            {
                std::lock_guard<std::recursive_mutex> policy(policyMutex);
                std::lock_guard<std::recursive_mutex> lock(bindingMutex);
                --pendingBindingChanges;
            }
            BindingTransition(const BindingTransition&) = delete;
            BindingTransition& operator=(const BindingTransition&) = delete;
        };
        std::unordered_map<DWORD, HardwareBinding> hardwareBindings;
        DWORD defaultSoftwareType = UE_BREAKPOINT_TYPE_INT3;
        struct SoftwareBinding { bool singleShot; DWORD type; TITANCBSOFTBP callback; DWORD installedPath; };
        std::unordered_map<ULONG_PTR, SoftwareBinding> softwareBindings;
        void softwareHit()
        {
            TITANCBSOFTBP callback = nullptr;
            {
                std::lock_guard<std::recursive_mutex> policy(policyMutex);
                std::lock_guard<std::recursive_mutex> lock(bindingMutex);
                const auto* event = nativeApi.GetDebugData();
                if (event == nullptr || event->dwDebugEventCode != EXCEPTION_DEBUG_EVENT) return;
                const auto address = reinterpret_cast<ULONG_PTR>(event->u.Exception.ExceptionRecord.ExceptionAddress);
                auto found = softwareBindings.end();
                for (auto it = softwareBindings.begin(); it != softwareBindings.end(); ++it)
                    if (address >= it->first && address - it->first == (it->second.type == UE_BREAKPOINT_TYPE_LONG_INT3 ? 1U : 0U))
                    { found = it; break; }
                if (found == softwareBindings.end()) return;
                callback = found->second.callback;
                if (found->second.singleShot) softwareBindings.erase(found);
            }
            LogicalReadScope frontend(false);
            if (callback != nullptr) callback();
        }
        struct MemoryBinding { SIZE_T bytes; bool restore; TITANCBMEMBP callback; };
        std::unordered_map<ULONG_PTR, MemoryBinding> memoryBindings;
        void memoryHit(const void* accessed)
        {
            TITANCBMEMBP callback = nullptr;
            {
                std::lock_guard<std::recursive_mutex> policy(policyMutex);
                std::lock_guard<std::recursive_mutex> lock(bindingMutex);
                const auto address = reinterpret_cast<ULONG_PTR>(accessed);
                for (auto it = memoryBindings.begin(); it != memoryBindings.end(); ++it)
                    if (address >= it->first && address - it->first < it->second.bytes)
                    {
                        callback = it->second.callback;
                        if (!it->second.restore) memoryBindings.erase(it);
                        break;
                    }
            }
            LogicalReadScope frontend(false);
            if (callback != nullptr) callback(accessed);
        }
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
    bool hasPendingBindingChanges() { std::lock_guard<std::recursive_mutex> lock(bindingMutex); return pendingBindingChanges != 0; }
    DWORD adapterBreakpointCount()
    {
        std::lock_guard<std::recursive_mutex> policy(policyMutex);
        std::lock_guard<std::recursive_mutex> lock(bindingMutex);
        // No native breakpoint-table query here: Titan can hold its table lock
        // while entering the write seam. The callbacks retire one-shot records.
        return static_cast<DWORD>(softwareBindings.size() + memoryBindings.size() + hardwareBindings.size());
    }
    DWORD adapterBreakpointPath()
    {
        std::lock_guard<std::recursive_mutex> lock(bindingMutex);
        DWORD path = KSWORD_DEBUGGER_PATH_NATIVE;
        // The native event loop temporarily removes physical INT3 before a
        // frontend pause callback. Its retained logical binding still owns the
        // installed route until explicit deletion/session retirement.
        for (const auto& pair : hardwareBindings)
        {
            if (pair.second.shadow) return KSWORD_DEBUGGER_PATH_SHADOW;
            if (pair.second.hvm) path = KSWORD_DEBUGGER_PATH_EPT;
        }
        for (const auto& pair : softwareBindings)
            if (pair.second.installedPath == KSWORD_DEBUGGER_PATH_SHADOW) return KSWORD_DEBUGGER_PATH_SHADOW;
        return path;
    }
    void clearSoftwareBindings() { std::lock_guard<std::recursive_mutex> lock(bindingMutex); softwareBindings.clear(); memoryBindings.clear(); }
    void clearHardwareBindings() { std::lock_guard<std::recursive_mutex> lock(bindingMutex); hardwareBindings.clear(); }
    void clearDeferredSteps() { deferredSteps.clear(); }

    bool setHardware(ULONG_PTR address, DWORD index, TitanHardwareBreakpointType type,
        TitanHardwareBreakpointSize size, TITANCBHWBP callback)
    {
        if (ensureNative() != ERROR_SUCCESS) return false;
        std::lock_guard<std::recursive_mutex> policy(policyMutex);
        const auto options = debugger::backend().options();
        if (options.mode == KSWORD_DEBUGGER_MODE_STEALTH && !hvmSelected.load())
        {
            debugger::backend().recordFallback("Titan hardware breakpoint", "rejected", ERROR_NOT_READY,
                "stealth requires HVM; visible hardware debug registers are forbidden; Windows debug-event transport remains active");
            SetLastError(ERROR_NOT_READY); return false;
        }
        if (hvmSelected.load() && type != UE_HARDWARE_EXECUTE)
        {
            debugger::backend().recordFallback("Titan hardware data breakpoint", "rejected", ERROR_NOT_SUPPORTED,
                "EPT data coverage is 4 KiB; byte-range native DR fallback is not selected automatically");
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
        const bool shadow = hvmSelected.load() && debugger::backend().preferShadowExecution();
        if (shadow && options.mode == KSWORD_DEBUGGER_MODE_NORMAL && !options.allowFallback)
        {
            debugger::backend().recordFallback("EPT execute breakpoint", "rejected", ERROR_NOT_SUPPORTED,
                "ShadowPage fallback is disabled by policy");
            SetLastError(ERROR_NOT_SUPPORTED); return false;
        }
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
                if (options.mode == KSWORD_DEBUGGER_MODE_NORMAL)
                    debugger::backend().recordFallback("EPT execute breakpoint", "ShadowPage hidden INT3", ERROR_NOT_SUPPORTED,
                        "native-event EPT breakpoint protocol is unavailable; hidden execution view installed successfully");
                else log("Execute breakpoint: source=stealth policy error=0 -> actual=ShadowPage hidden INT3; Windows debug-event transport remains active");
            }
            else if (hadPrevious && !nativeApi.SetBPX(previous.address, UE_BREAKPOINT | UE_BREAKPOINT_TYPE_INT3,
                shadowCallbacks[selected - UE_DR0]))
            {
                log("ShadowPage hardware replacement rollback failed");
                hardwareBindings.erase(selected);
            }
            if (!result) debugger::backend().recordFallback("ShadowPage execute breakpoint", "rejected", installError,
                "hidden breakpoint installation failed; no original-page INT3 or hardware DR fallback performed");
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
            debugger::backend().recordFallback("EPT execute breakpoint", "rejected", hookError == ERROR_SUCCESS ? ERROR_INVALID_STATE : hookError,
                "installation failed; native callback rollback " + std::string(restored ? "completed" : "failed"));
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
        BindingTransition transition;
        std::lock_guard<std::recursive_mutex> operation(softwareOperationMutex);
        LogicalReadScope native;
        const bool result = nativeApi.RemoveAllBreakPoints(option);
        if (hvmSelected.load() && (getSeamError() != ERROR_SUCCESS || hasPendingDebug()))
        { SetLastError(getSeamError() == ERROR_SUCCESS ? ERROR_INVALID_STATE : getSeamError()); return false; }
        if (result && option == UE_OPTION_REMOVEALL) { clearHardwareBindings(); clearSoftwareBindings(); }
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
        BindingTransition transition;
        std::lock_guard<std::recursive_mutex> operation(softwareOperationMutex);
        if (ksword::debugger::backend().options().mode == KSWORD_DEBUGGER_MODE_STEALTH && !hvmSelected.load())
        {
            ksword::debugger::backend().recordFallback("Titan software breakpoint", "rejected", ERROR_NOT_READY,
                "stealth requires HVM; visible original-page INT3 is forbidden; Windows debug-event transport remains active");
            SetLastError(ERROR_NOT_READY); return false;
        }
        LogicalReadScope native; const bool result = nativeApi.SetBPX(address, type, callback == nullptr ? nullptr : &softwareHit);
        const DWORD error = GetLastError();
        if (result)
        {
            unsigned char hidden = 0;
            const auto options = ksword::debugger::backend().options();
            if (hvmSelected.load()) ksword::debugger::backend().overlayShadowBytes(nativeProcessId(), address, &hidden, 1);
            const DWORD installedPath = hvmSelected.load() && (hidden == 0xcc || options.shadowMemoryWrites != 0 ||
                options.mode == KSWORD_DEBUGGER_MODE_STEALTH) ? KSWORD_DEBUGGER_PATH_SHADOW : KSWORD_DEBUGGER_PATH_NATIVE;
            std::lock_guard<std::recursive_mutex> lock(bindingMutex);
            // Re-enabling an inactive native entry retains its original
            // callback/type. Preserve the matching adapter record as well.
            softwareBindings.try_emplace(address, SoftwareBinding{(type & 0xff) == UE_SINGLESHOOT,
                (type & 0xf0000000) == 0 ? defaultSoftwareType : (type & 0xf0000000), callback, installedPath});
        }
        SetLastError(error); return result;
    }
    bool DeleteBPX(ULONG_PTR address)
    {
        using namespace ksword::titan;
        if (ensureNative() != ERROR_SUCCESS) return false;
        BindingTransition transition;
        std::lock_guard<std::recursive_mutex> operation(softwareOperationMutex);
        LogicalReadScope native; const bool result = nativeApi.DeleteBPX(address); const DWORD error = GetLastError();
        if (result) { std::lock_guard<std::recursive_mutex> lock(bindingMutex); softwareBindings.erase(address); }
        SetLastError(error); return result;
    }
    void SetBPXOptions(TitanBreakpointType type)
    {
        using namespace ksword::titan;
        if (ensureNative() != ERROR_SUCCESS) return;
        std::lock_guard<std::recursive_mutex> operation(softwareOperationMutex);
        std::lock_guard<std::recursive_mutex> policy(policyMutex);
        nativeApi.SetBPXOptions(type);
        const DWORD error = GetLastError();
        if (type == UE_BREAKPOINT_INT3 || static_cast<DWORD>(type) == UE_BREAKPOINT_TYPE_INT3) defaultSoftwareType = UE_BREAKPOINT_TYPE_INT3;
        else if (type == UE_BREAKPOINT_LONG_INT3 || static_cast<DWORD>(type) == UE_BREAKPOINT_TYPE_LONG_INT3) defaultSoftwareType = UE_BREAKPOINT_TYPE_LONG_INT3;
        else if (type == UE_BREAKPOINT_UD2 || static_cast<DWORD>(type) == UE_BREAKPOINT_TYPE_UD2) defaultSoftwareType = UE_BREAKPOINT_TYPE_UD2;
        SetLastError(error);
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
        std::lock_guard<std::recursive_mutex> policy(policyMutex);
        if (!hvmSelected.load() || !ksword::debugger::backend().preferShadowExecution())
            return nativeApi.GetUnusedHardwareBreakPointRegister(index);
        std::lock_guard<std::recursive_mutex> lock(bindingMutex);
        for (DWORD slot = UE_DR0; slot <= UE_DR3; ++slot)
            if (hardwareBindings.find(slot) == hardwareBindings.end()) { *index = slot; return true; }
        SetLastError(ERROR_NO_SYSTEM_RESOURCES); return false;
    }

    bool SetMemoryBPXEx(ULONG_PTR address, SIZE_T bytes, TitanMemoryBreakpointType type, bool restore, TITANCBMEMBP callback)
    {
        using namespace ksword::titan;
        if (ensureNative() != ERROR_SUCCESS) return false;
        BindingTransition transition;
        std::lock_guard<std::recursive_mutex> operation(softwareOperationMutex);
        const auto options = ksword::debugger::backend().options();
        if (options.mode == KSWORD_DEBUGGER_MODE_STEALTH || (hvmSelected.load() && !options.allowFallback))
        {
            ksword::debugger::backend().recordFallback("Titan memory guard breakpoint", "rejected", ERROR_NOT_SUPPORTED,
                "visible PAGE_GUARD is forbidden by the selected policy; Windows debug-event transport remains active");
            SetLastError(ERROR_NOT_SUPPORTED); return false;
        }
        const bool result = nativeApi.SetMemoryBPXEx(address, bytes, type, restore, callback == nullptr ? nullptr : &memoryHit); const DWORD error = GetLastError();
        if (result)
        {
            { std::lock_guard<std::recursive_mutex> lock(bindingMutex); memoryBindings[address] = {bytes, restore, callback}; }
            if (hvmSelected.load()) ksword::debugger::backend().recordFallback("HVM byte-range memory breakpoint", "native PAGE_GUARD", ERROR_NOT_SUPPORTED,
                "native guard breakpoint installed successfully; explicit data Watch uses separate 4 KiB EPT coverage");
        }
        SetLastError(error); return result;
    }
    bool RemoveMemoryBPX(ULONG_PTR address, SIZE_T bytes)
    {
        using namespace ksword::titan;
        if (ensureNative() != ERROR_SUCCESS) return false;
        BindingTransition transition;
        std::lock_guard<std::recursive_mutex> operation(softwareOperationMutex);
        const bool result = nativeApi.RemoveMemoryBPX(address, bytes); const DWORD error = GetLastError();
        if (result) { std::lock_guard<std::recursive_mutex> lock(bindingMutex); memoryBindings.erase(address); }
        SetLastError(error); return result;
    }
    bool MemoryWriteSafe(HANDLE process, LPVOID address, LPCVOID data, SIZE_T bytes, SIZE_T* done)
    {
        using namespace ksword::titan;
        if (ensureNative() != ERROR_SUCCESS) return false;
        std::lock_guard<std::recursive_mutex> policy(policyMutex);
        const auto options = ksword::debugger::backend().options();
        if (options.shadowMemoryWrites == 0 && options.mode != KSWORD_DEBUGGER_MODE_STEALTH)
            return nativeApi.MemoryWriteSafe(process, address, data, bytes, done);
        // Classify the original protection before native Titan temporarily
        // promotes the allocation to RWX. No visible write precedes policy.
        return ksword::debugger::backend().writeMemory(process, address, data, bytes, done) != FALSE;
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
