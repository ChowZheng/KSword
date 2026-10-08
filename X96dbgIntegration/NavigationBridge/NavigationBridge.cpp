#include "../NavigationProtocol.h"
#include <sddl.h>
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
#include <cstring>

// Minimal, dynamically resolved public x64dbg SDK ABI. No third-party code is
// loaded into KSword, and no engine API is used by this navigation plugin.
// Signatures: x64dbg src/bridge/bridgemain.h and src/dbg/_plugins.h.
namespace
{
    using namespace ksword::x64dbg_navigation;
    using Address = std::uintptr_t;
    struct PluginInit { int pluginHandle, sdkVersion, pluginVersion; char pluginName[256]; };
    struct Selection { Address start, end; };
    struct Api
    {
        bool (*debugging)() = nullptr;
        bool (*running)() = nullptr;
        HANDLE (*process)() = nullptr;
        Address (*value)(const char*) = nullptr;
        void (*gui)(void (*)(void*), void*) = nullptr;
        void (*disasm)(Address, Address) = nullptr;
        void (*dump)(Address) = nullptr;
        bool (*selectionGet)(int, Selection*) = nullptr;
        bool (*selectionSet)(int, const Selection*) = nullptr;
        void (*showCpu)() = nullptr;
        void (*focus)(int) = nullptr;
        HWND (*window)() = nullptr;
        bool (*setting)(const char*, const char*, Address) = nullptr;
        void (*registerCallback)(int, int, void (*)(int, void*)) = nullptr;
        bool (*unregisterCallback)(int, int) = nullptr;
    } api;
    HANDLE stopEvent = nullptr, worker = nullptr;
    std::atomic<unsigned long> pending{0};
    std::atomic<bool> stopping{false};
    std::atomic<bool> freshAttach{false}, systemReached{false}, firstPauseReached{false};
    int pluginHandle = 0;
    std::mutex jobsMutex;
    enum class JobStage { Pending, Committed, Cancelled, Finished };
    struct Job
    {
        Request request;
        Response response;
        HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::atomic<JobStage> stage{JobStage::Pending};
        ~Job() { if (done != nullptr) CloseHandle(done); }
    };
    std::vector<std::weak_ptr<Job>> jobs;

    template<class T> bool resolve(HMODULE module, const char* name, T& target)
    {
        const auto address = GetProcAddress(module, name);
        static_assert(sizeof(address) == sizeof(target), "Function pointer ABI");
        std::memcpy(&target, &address, sizeof(target));
        return target != nullptr;
    }
    bool loadApi()
    {
#ifdef _WIN64
        const auto bridge = GetModuleHandleW(L"x64bridge.dll");
        const auto debugger = GetModuleHandleW(L"x64dbg.dll");
#else
        const auto bridge = GetModuleHandleW(L"x32bridge.dll");
        const auto debugger = GetModuleHandleW(L"x32dbg.dll");
#endif
        return bridge != nullptr && resolve(bridge, "DbgIsDebugging", api.debugging)
            && resolve(bridge, "DbgIsRunning", api.running) && resolve(bridge, "DbgGetProcessHandle", api.process)
            && resolve(bridge, "DbgValFromString", api.value) && resolve(bridge, "GuiExecuteOnGuiThreadEx", api.gui)
            && resolve(bridge, "GuiDisasmAt", api.disasm) && resolve(bridge, "GuiDumpAt", api.dump)
            && resolve(bridge, "GuiSelectionGet", api.selectionGet) && resolve(bridge, "GuiSelectionSet", api.selectionSet)
            && resolve(bridge, "GuiShowCpu", api.showCpu) && resolve(bridge, "GuiFocusView", api.focus)
            && resolve(bridge, "GuiGetWindowHandle", api.window) && resolve(bridge, "BridgeSettingSetUint", api.setting)
            && debugger != nullptr && resolve(debugger, "_plugin_registercallback", api.registerCallback)
            && resolve(debugger, "_plugin_unregistercallback", api.unregisterCallback);
    }
    void snapshot(Response& response)
    {
        response.debuggerPid = GetCurrentProcessId();
        if (!api.debugging()) return;
        HANDLE identity = nullptr;
        const HANDLE borrowed = api.process();
        if (borrowed == nullptr || !DuplicateHandle(GetCurrentProcess(), borrowed, GetCurrentProcess(), &identity,
            PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, 0))
        { response.error = ERROR_INVALID_HANDLE; return; }
        response.targetPid = GetProcessId(identity);
        response.targetCreateTime = creationTime(identity);
        if (WaitForSingleObject(identity, 0) == WAIT_OBJECT_0 || response.targetPid == 0 || response.targetCreateTime == 0)
            response.error = ERROR_INVALID_STATE;
        else response.flags = Debugging | (api.running() ? Running : 0U)
            | (!freshAttach.load() || firstPauseReached.load() ? AttachReady : 0U);
        CloseHandle(identity);
    }
    void onGui(void* userdata)
    {
        std::unique_ptr<std::shared_ptr<Job>> holder(static_cast<std::shared_ptr<Job>*>(userdata));
        const auto job = *holder;
        auto& response = job->response;
        response.requestId = job->request.requestId;
        if (stopping.load() || job->stage.load() == JobStage::Cancelled) response.error = ERROR_CANCELLED;
        else
        {
            snapshot(response);
            if (job->request.operation == Operation::Navigate && response.error == ERROR_SUCCESS)
            {
                if (!(response.flags & Debugging) || response.targetPid != job->request.targetPid
                    || response.targetCreateTime != job->request.targetCreateTime)
                    response.error = ERROR_INVALID_STATE;
                else if (job->request.address > static_cast<std::uint64_t>(UINTPTR_MAX)) response.error = ERROR_INVALID_ADDRESS;
                else
                {
                    const Address cip = job->request.address == 0 || job->request.view == View::Disassembly
                        ? api.value("cip") : 0;
                    const Address address = job->request.address == 0 ? cip : static_cast<Address>(job->request.address);
                    if (address == 0) response.error = ERROR_INVALID_ADDRESS;
                    else
                    {
                        // Snapshot/CIP queries may block. Cancellation and the
                        // first GUI side effect must have a single atomic winner.
                        // Once committed, the operation cannot claim cancellation.
                        JobStage expected = JobStage::Pending;
                        if (!job->stage.compare_exchange_strong(expected, JobStage::Committed))
                        {
                            response.error = ERROR_CANCELLED;
                            job->stage = JobStage::Finished;
                            SetEvent(job->done);
                            --pending;
                            return;
                        }
                        // These GUI calls execute on the GUI thread. No execution
                        // command is dispatched, including for a running target.
                        const int view = job->request.view == View::Dump ? 1 : 0;
                        if (view == 0) api.disasm(address, cip);
                        else api.dump(address);
                        const Selection select{address, address};
                        api.showCpu(); api.focus(view);
                        Selection actual{};
                        if (!api.selectionSet(view, &select) || !api.selectionGet(view, &actual)) response.error = ERROR_NOT_READY;
                        else
                        {
                            response.actualAddress = actual.start;
                            if (actual.start != address) response.error = ERROR_INVALID_ADDRESS;
                        }
                        // Recheck after the GUI operation; never ACK a different
                        // session if a target was detached or exited meanwhile.
                        Response after{}; snapshot(after);
                        if (after.error != ERROR_SUCCESS || after.targetPid != response.targetPid
                            || after.targetCreateTime != response.targetCreateTime) response.error = ERROR_INVALID_STATE;
                        response.flags = after.flags;
                        if (response.error == ERROR_SUCCESS)
                        { const HWND window = api.window(); ShowWindow(window, SW_RESTORE); SetForegroundWindow(window); }
                    }
                }
            }
        }
        if (job->stage.load() == JobStage::Cancelled) response.error = ERROR_CANCELLED;
        job->stage = JobStage::Finished;
        SetEvent(job->done);
        --pending;
    }
    Response dispatch(const Request& request)
    {
        Response failure{}; failure.requestId = request.requestId; failure.debuggerPid = GetCurrentProcessId();
        if (!valid(request)) { failure.error = ERROR_INVALID_DATA; return failure; }
        const auto job = std::make_shared<Job>(); job->request = request;
        if (job->done == nullptr) { failure.error = ERROR_NOT_ENOUGH_MEMORY; return failure; }
        {
            std::lock_guard<std::mutex> lock(jobsMutex);
            if (stopping.load()) { failure.error = ERROR_CANCELLED; return failure; }
            jobs.erase(std::remove_if(jobs.begin(), jobs.end(), [](auto& item) { return item.expired(); }), jobs.end());
            jobs.push_back(job); ++pending;
        }
        api.gui(onGui, new std::shared_ptr<Job>(job));
        const HANDLE events[]{stopEvent, job->done};
        if (WaitForMultipleObjects(2, events, FALSE, 4000) == WAIT_OBJECT_0 + 1) return job->response;
        JobStage expected = JobStage::Pending;
        if (job->stage.compare_exchange_strong(expected, JobStage::Cancelled)) failure.error = ERROR_TIMEOUT;
        else if (expected == JobStage::Finished) return job->response;
        // A committed GUI operation may still finish. Do not represent that
        // uncertain result as successful cancellation or retry it automatically.
        else failure.error = expected == JobStage::Committed ? ERROR_IO_PENDING : ERROR_TIMEOUT;
        return failure;
    }
    bool io(HANDLE pipe, void* data, DWORD bytes, bool write)
    {
        OVERLAPPED overlapped{}; overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (overlapped.hEvent == nullptr) return false;
        DWORD done = 0;
        BOOL ok = write ? WriteFile(pipe, data, bytes, &done, &overlapped) : ReadFile(pipe, data, bytes, &done, &overlapped);
        if (!ok && GetLastError() == ERROR_IO_PENDING)
        {
            const HANDLE events[]{stopEvent, overlapped.hEvent};
            if (WaitForMultipleObjects(2, events, FALSE, 5000) != WAIT_OBJECT_0 + 1)
            { CancelIoEx(pipe, &overlapped); GetOverlappedResult(pipe, &overlapped, &done, TRUE); }
            else ok = GetOverlappedResult(pipe, &overlapped, &done, FALSE);
        }
        CloseHandle(overlapped.hEvent);
        return ok && done == bytes;
    }
    bool respond(HANDLE pipe, const Request& request, Response& response)
    {
        if (!io(pipe, &response, sizeof(response), true)) return false;
        Receipt receipt{};
        // DisconnectNamedPipe discards unread output. Wait for the client's
        // bounded, typed read receipt, never an unbounded FlushFileBuffers.
        return io(pipe, &receipt, sizeof(receipt), false) && valid(receipt, request.requestId);
    }
    PSECURITY_DESCRIPTOR security()
    {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return nullptr;
        DWORD bytes = 0; GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
        std::vector<unsigned char> data(bytes);
        bool ok = bytes != 0 && GetTokenInformation(token, TokenUser, data.data(), bytes, &bytes);
        CloseHandle(token);
        LPWSTR sid = nullptr;
        if (!ok || !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, &sid)) return nullptr;
        const std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")";
        LocalFree(sid);
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return nullptr;
        return descriptor;
    }
    DWORD WINAPI serve(void*)
    {
        PSECURITY_DESCRIPTOR descriptor = security();
        if (descriptor == nullptr) return 1;
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
        const auto name = pipeName(GetCurrentProcessId(), creationTime(GetCurrentProcess()));
        while (!stopping.load())
        {
            const HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                sizeof(Response), sizeof(Request), 1000, &attributes);
            if (pipe == INVALID_HANDLE_VALUE) break;
            OVERLAPPED connection{}; connection.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            bool connected = connection.hEvent != nullptr && ConnectNamedPipe(pipe, &connection) != FALSE;
            if (!connected && connection.hEvent != nullptr)
            {
                const DWORD error = GetLastError();
                if (error == ERROR_PIPE_CONNECTED) connected = true;
                else if (error == ERROR_IO_PENDING)
                {
                    const HANDLE events[]{stopEvent, connection.hEvent};
                    DWORD transferred = 0;
                    if (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0 + 1)
                        connected = GetOverlappedResult(pipe, &connection, &transferred, FALSE) != FALSE;
                    else { CancelIoEx(pipe, &connection); GetOverlappedResult(pipe, &connection, &transferred, TRUE); }
                }
            }
            if (connection.hEvent != nullptr) CloseHandle(connection.hEvent);
            Request request{};
            if (connected && io(pipe, &request, sizeof(request), false))
            { auto response = dispatch(request); (void)respond(pipe, request, response); }
            DisconnectNamedPipe(pipe); CloseHandle(pipe);
        }
        LocalFree(descriptor);
        return 0;
    }
    void debugCallback(int type, void*)
    {
        if (type == 6) systemReached = true; // CB_SYSTEMBREAKPOINT
        else if (type == 12 && systemReached.load()) firstPauseReached = true; // CB_PAUSEDEBUG
        else if (type == 1) { systemReached = false; firstPauseReached = false; freshAttach = false; } // CB_STOPDEBUG
    }
}

extern "C" __declspec(dllexport) bool pluginit(PluginInit* init)
{
    if (init == nullptr || !loadApi()) return false;
    init->sdkVersion = 1; init->pluginVersion = 2;
    pluginHandle = init->pluginHandle;
    stopping = false;
    strcpy_s(init->pluginName, "KSword safe address navigation");
    stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stopEvent == nullptr) return false;
    // A launch requested by KSword always uses a new user directory. Set this
    // only for that fresh session; externally opened debugger settings remain.
    wchar_t initial[2]{};
    if (GetEnvironmentVariableW(L"KSWORD_NAVIGATION_FRESH_ATTACH", initial, 2) == 1 && initial[0] == L'1')
    { freshAttach = true; (void)api.setting("Events", "SystemBreakpoint", 1); }
    api.registerCallback(pluginHandle, 6, debugCallback);
    api.registerCallback(pluginHandle, 12, debugCallback);
    api.registerCallback(pluginHandle, 1, debugCallback);
    worker = CreateThread(nullptr, 0, serve, nullptr, 0, nullptr);
    if (worker == nullptr)
    {
        api.unregisterCallback(pluginHandle, 6); api.unregisterCallback(pluginHandle, 12); api.unregisterCallback(pluginHandle, 1);
        CloseHandle(stopEvent); stopEvent = nullptr; return false;
    }
    return true;
}

extern "C" __declspec(dllexport) bool plugstop()
{
    // Never unload DLL callbacks that the GUI has not consumed. The user can
    // retry unloading after the pending bounded request has been processed.
    { std::lock_guard<std::mutex> lock(jobsMutex); if (pending.load() != 0) return false; stopping = true; }
    SetEvent(stopEvent);
    if (worker != nullptr) { WaitForSingleObject(worker, INFINITE); CloseHandle(worker); worker = nullptr; }
    CloseHandle(stopEvent); stopEvent = nullptr;
    api.unregisterCallback(pluginHandle, 6); api.unregisterCallback(pluginHandle, 12); api.unregisterCallback(pluginHandle, 1);
    return true;
}
