#include "KswordCeBridge.h"
#include "../DebuggerBackend/KswordDebuggerBackend.h"

#include <mutex>
#include <string>
#include <vector>

namespace ksword::ce
{
    namespace
    {
        // The adapter only translates CE's SDK calls; all backend policy is reusable.
        struct Hook { void* slot; void* replacement; void* original; };
        std::mutex g_mutex;
        std::vector<Hook> g_hooks;
        ExportedFunctions* g_functions = nullptr;
        int g_pluginId = -1;
        int g_registrationId = -1;

        template<typename T, typename Action> T boundary(Action action, T failure) noexcept
        {
            try { return action(); }
            catch (...) { SetLastError(ERROR_GEN_FAILURE); return failure; }
        }

        HANDLE WINAPI openProcess(DWORD access, BOOL inherit, DWORD pid)
        { return boundary([&] { return debugger::backend().openProcess(access, inherit, pid); }, static_cast<HANDLE>(nullptr)); }
        BOOL WINAPI readMemory(HANDLE process, LPCVOID address, LPVOID data, SIZE_T bytes, SIZE_T* transferred)
        { return boundary([&] {
            // CE's Local Lua functions build our ABI packets in its own memory.
            // Driver process protections must not intercept these local buffers.
            if (GetProcessId(process) == GetCurrentProcessId())
                return ::ReadProcessMemory(process, address, data, bytes, transferred);
            return debugger::backend().readMemory(process, address, data, bytes, transferred);
        }, FALSE); }
        BOOL WINAPI writeMemory(HANDLE process, LPVOID address, LPCVOID data, SIZE_T bytes, SIZE_T* transferred)
        { return boundary([&] {
            if (GetProcessId(process) == GetCurrentProcessId())
                return ::WriteProcessMemory(process, address, data, bytes, transferred);
            return debugger::backend().writeMemory(process, address, data, bytes, transferred);
        }, FALSE); }
        SIZE_T WINAPI queryMemory(HANDLE process, LPCVOID address, PMEMORY_BASIC_INFORMATION info, SIZE_T bytes)
        { return boundary([&] { return debugger::backend().queryMemory(process, address, info, bytes); }, static_cast<SIZE_T>(0)); }
        BOOL WINAPI getContext(HANDLE thread, LPCONTEXT context)
        { return boundary([&] { return debugger::backend().getContext(thread, context); }, FALSE); }
        BOOL WINAPI setContext(HANDLE thread, const CONTEXT* context)
        { return boundary([&] { return debugger::backend().setContext(thread, context); }, FALSE); }
        DWORD WINAPI suspendThread(HANDLE thread)
        { return boundary([&] { return debugger::backend().suspendThread(thread); }, MAXDWORD); }
        DWORD WINAPI resumeThread(HANDLE thread)
        { return boundary([&] { return debugger::backend().resumeThread(thread); }, MAXDWORD); }
        BOOL WINAPI waitEvent(LPDEBUG_EVENT event, DWORD milliseconds)
        { return boundary([&] { return debugger::backend().waitEvent(event, milliseconds); }, FALSE); }
        BOOL WINAPI continueEvent(DWORD pid, DWORD tid, DWORD status)
        { return boundary([&] { return debugger::backend().continueEvent(pid, tid, status); }, FALSE); }
        BOOL WINAPI attach(DWORD pid)
        { return boundary([&] { return debugger::backend().attach(pid); }, FALSE); }
        BOOL WINAPI protectMemory(HANDLE process, LPVOID address, SIZE_T bytes, DWORD protection, PDWORD previous)
        { return boundary([&] { return debugger::backend().protectMemory(process, address, bytes, protection, previous); }, FALSE); }
        LPVOID WINAPI allocateMemory(HANDLE process, LPVOID address, SIZE_T bytes, DWORD type, DWORD protection)
        { return boundary([&] { return debugger::backend().allocateMemory(process, address, bytes, type, protection); }, static_cast<LPVOID>(nullptr)); }
        HANDLE WINAPI createThread(HANDLE process, LPSECURITY_ATTRIBUTES attributes, SIZE_T stackBytes,
            LPTHREAD_START_ROUTINE start, LPVOID parameter, DWORD flags, LPDWORD tid)
        { return boundary([&] { return debugger::backend().createThread(process, attributes, stackBytes, start, parameter, flags, tid); }, static_cast<HANDLE>(nullptr)); }
        HANDLE WINAPI openThread(DWORD access, BOOL inherit, DWORD tid)
        { return boundary([&] { return debugger::backend().openThread(access, inherit, tid); }, static_cast<HANDLE>(nullptr)); }

        template<typename Function> void addHook(void* slot, Function function)
        {
            g_hooks.push_back({slot, reinterpret_cast<void*>(function), nullptr});
        }

        bool installHooks()
        {
            if (g_functions == nullptr) return false;
            if (g_hooks.empty())
            {
                addHook(g_functions->readProcessMemory, &readMemory);
                addHook(g_functions->writeProcessMemory, &writeMemory);
                addHook(g_functions->openProcess, &openProcess);
                addHook(g_functions->virtualQueryEx, &queryMemory);
                addHook(g_functions->getThreadContext, &getContext);
                addHook(g_functions->setThreadContext, &setContext);
                addHook(g_functions->suspendThread, &suspendThread);
                addHook(g_functions->resumeThread, &resumeThread);
                addHook(g_functions->waitForDebugEvent, &waitEvent);
                addHook(g_functions->continueDebugEvent, &continueEvent);
                addHook(g_functions->debugActiveProcess, &attach);
                addHook(g_functions->virtualProtectEx, &protectMemory);
                addHook(g_functions->virtualAllocEx, &allocateMemory);
                addHook(g_functions->createRemoteThread, &createThread);
                addHook(g_functions->openThread, &openThread);
                for (const auto& hook : g_hooks) if (hook.slot == nullptr) { g_hooks.clear(); return false; }
            }
            for (auto& hook : g_hooks)
            {
                auto* slot = static_cast<void* volatile*>(hook.slot);
                if (*slot != hook.replacement)
                    hook.original = InterlockedExchangePointer(slot, hook.replacement);
            }
            return true;
        }
    }

    BOOL initializeBridge(ExportedFunctions* functions, int pluginId)
    {
        if (functions == nullptr || functions->sizeofExportedFunctions < static_cast<int>(kRequiredExportedFunctionsSize))
            return FALSE;
        // CE can disable a plugin while its debug-event thread is inside a hook.
        // Keep code mapped until CE exits; disabling still restores every slot and
        // releases the driver session, so an outstanding call cannot return into
        // an unloaded DLL. No loader work occurs from DllMain.
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&initializeBridge), &pinned)) return FALSE;
        const bool driverReady = debugger::backend().initialize();
        const DWORD driverError = driverReady ? ERROR_SUCCESS : GetLastError();
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_functions != nullptr) return g_functions == functions ? TRUE : FALSE;
            g_functions = functions; g_pluginId = pluginId;
            if (driverReady && !installHooks()) { g_functions = nullptr; g_pluginId = -1; return FALSE; }
        }
        if (functions->registerFunction != nullptr)
        {
            FunctionPointerChangeInitialization initialization{&notifyFunctionPointersChanged};
            const int id = functions->registerFunction(pluginId, PluginType::functionPointerChange, &initialization);
            std::lock_guard<std::mutex> lock(g_mutex);
            g_registrationId = id;
        }
        debugger::backend().log(driverReady ? "CE adapter installed 15 memory/thread/debugger function hooks; edits and freeze share write policy" :
            "CE bridge connected; explicit native route: R0 unavailable (error " + std::to_string(driverError) +
            "), CE retains its native function table; Shadow/HVM memory policy is not active");
        return TRUE;
    }

    BOOL disableBridge()
    {
        // Refuse unloading code if backend cleanup left a live EPT transaction.
        if (!debugger::backend().shutdown()) return FALSE;
        UnregisterFunction unregister = nullptr;
        int pluginId = -1, registrationId = -1;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_functions == nullptr) return TRUE;
            for (const auto& hook : g_hooks)
                (void)InterlockedCompareExchangePointer(static_cast<void* volatile*>(hook.slot), hook.original, hook.replacement);
            unregister = g_functions->unregisterFunction;
            pluginId = g_pluginId; registrationId = g_registrationId;
            g_functions = nullptr; g_pluginId = -1; g_registrationId = -1; g_hooks.clear();
        }
        if (unregister != nullptr && registrationId >= 0) (void)unregister(pluginId, registrationId);
        return TRUE;
    }

    void __stdcall notifyFunctionPointersChanged(int reserved)
    {
        UNREFERENCED_PARAMETER(reserved);
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_functions != nullptr && debugger::backend().initialize()) (void)installHooks();
    }
}
