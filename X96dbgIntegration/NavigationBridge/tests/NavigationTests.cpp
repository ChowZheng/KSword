#include "../../NavigationClient.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <thread>

#undef assert
#define assert(condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL line %d: %s (Win32=%lu)\n", __LINE__, #condition, GetLastError()); std::fflush(stderr); std::_Exit(1); } } while (0)

namespace
{
    struct PluginInit { int pluginHandle, sdkVersion, pluginVersion; char pluginName[256]; };
    template<class T> T function(HMODULE module, const char* name)
    {
        const auto symbol = GetProcAddress(module, name);
        T result = nullptr; std::memcpy(&result, &symbol, sizeof(result)); assert(result != nullptr); return result;
    }
}
int main()
{
    using namespace ksword::x64dbg_navigation;
    const auto bridge = LoadLibraryW(L"x64bridge.dll"); assert(bridge != nullptr);
    const auto debugger = LoadLibraryW(L"x64dbg.dll"); assert(debugger != nullptr);
    const auto plugin = LoadLibraryW(L"KSwordNavigation.dp64"); assert(plugin != nullptr);
    const auto initialize = function<bool (*)(PluginInit*)>(plugin, "pluginit");
    const auto stop = function<bool (*)()>(plugin, "plugstop");
    const auto running = function<void (*)(bool)>(bridge, "FixtureSetRunning");
    const auto delay = function<void (*)(bool)>(bridge, "FixtureDelay");
    const auto flush = function<void (*)()>(bridge, "FixtureFlush");
    const auto address = function<std::uintptr_t (*)()>(bridge, "FixtureAddress");
    const auto fire = function<void (*)(int)>(debugger, "FixtureEvent");
    const auto snapshotDelay = function<void (*)(DWORD)>(bridge, "FixtureSnapshotDelay");
    const auto cipDelay = function<void (*)(DWORD)>(bridge, "FixtureCipDelay");
    const auto navigationDelay = function<void (*)(DWORD)>(bridge, "FixtureNavigationDelay");
    const auto queuedCount = function<unsigned (*)()>(bridge, "FixtureQueuedCount");
    SetEnvironmentVariableW(L"KSWORD_NAVIGATION_FRESH_ATTACH", L"1");
    PluginInit init{}; assert(initialize(&init)); assert(init.sdkVersion == 1);
    Request request{}; request.requestId = 1;
    Response response{};
    DWORD error = ERROR_FILE_NOT_FOUND;
    for (unsigned attempt = 0; attempt < 30 && error != ERROR_SUCCESS; ++attempt)
    { error = exchange(GetCurrentProcessId(), GetCurrentProcess(), request, response, 100); if (error != ERROR_SUCCESS) Sleep(20); }
    if (error != ERROR_SUCCESS) std::fprintf(stderr, "Query exchange error=%lu target=%u\n", error, response.targetPid);
    assert(error == ERROR_SUCCESS && response.targetPid == GetCurrentProcessId());
    assert(response.targetCreateTime == creationTime(GetCurrentProcess()));
    assert((response.flags & Debugging) && !(response.flags & Running));
    assert(!(response.flags & AttachReady));
    fire(6); ++request.requestId;
    assert(exchange(GetCurrentProcessId(), GetCurrentProcess(), request, response) == ERROR_SUCCESS && !(response.flags & AttachReady));
    fire(12); ++request.requestId;
    assert(exchange(GetCurrentProcessId(), GetCurrentProcess(), request, response) == ERROR_SUCCESS && (response.flags & AttachReady));
    SetEnvironmentVariableW(L"KSWORD_NAVIGATION_FRESH_ATTACH", nullptr);
    request.operation = Operation::Navigate; request.targetPid = response.targetPid;
    request.targetCreateTime = response.targetCreateTime; request.address = 0x1234;
    auto invoke = [&](Request candidate, DWORD timeout = 5000) {
        candidate.requestId = ++request.requestId;
        // The one-instance server closes and recreates its endpoint after each
        // bounded transaction; retry connection availability, never navigation.
        DWORD result = ERROR_FILE_NOT_FOUND;
        for (unsigned attempt = 0; attempt < 100; ++attempt)
        {
            result = exchange(GetCurrentProcessId(), GetCurrentProcess(), candidate, response, timeout);
            if (result != ERROR_FILE_NOT_FOUND && result != ERROR_PIPE_BUSY) break;
            Sleep(10);
        }
        return result;
    };
    assert(invoke(request) == ERROR_SUCCESS && response.actualAddress == 0x1234);
    assert(!((response.flags) & Running));
    running(true); request.view = View::Dump; request.address = 0x5678;
    assert(invoke(request) == ERROR_SUCCESS && response.actualAddress == 0x5678 && (response.flags & Running));
    auto changed = request; ++changed.targetCreateTime;
    assert(invoke(changed) == ERROR_INVALID_STATE && address() == 0x5678);
    changed = request; ++changed.targetPid;
    assert(invoke(changed) == ERROR_INVALID_STATE && address() == 0x5678);
    changed = request; changed.version = 100;
    assert(invoke(changed) == ERROR_INVALID_DATA && address() == 0x5678);
    Receipt receipt{}; receipt.requestId = request.requestId;
    assert(valid(receipt, request.requestId));
    ++receipt.requestId; assert(!valid(receipt, request.requestId));
    receipt.requestId = request.requestId; receipt.reserved = 1; assert(!valid(receipt, request.requestId));
    // Intentionally delay reading the already-written response. Without the
    // receipt handshake, DisconnectNamedPipe discards that unread response.
    Request query = request; query.operation = Operation::Query; query.requestId = ++request.requestId;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    const auto endpoint = pipeName(GetCurrentProcessId(), creationTime(GetCurrentProcess()));
    for (unsigned attempt = 0; attempt < 200 && pipe == INVALID_HANDLE_VALUE; ++attempt)
    {
        pipe = CreateFileW(endpoint.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) Sleep(10);
    }
    assert(pipe != INVALID_HANDLE_VALUE);
    ULONG serverPid = 0; assert(GetNamedPipeServerProcessId(pipe, &serverPid) && serverPid == GetCurrentProcessId());
    DWORD mode = PIPE_READMODE_MESSAGE; assert(SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr));
    assert(transfer(pipe, &query, sizeof(query), true, 5000));
    Sleep(100);
    assert(transfer(pipe, &response, sizeof(response), false, 5000));
    assert(response.error == ERROR_SUCCESS && response.requestId == query.requestId);
    receipt = Receipt{}; receipt.requestId = query.requestId;
    assert(transfer(pipe, &receipt, sizeof(receipt), true, 5000)); CloseHandle(pipe);
    for (unsigned attempt = 0; attempt < 1000; ++attempt) assert(invoke(query) == ERROR_SUCCESS);
    changed = request; changed.operation = static_cast<Operation>(3);
    assert(invoke(changed) == ERROR_INVALID_DATA && address() == 0x5678);
    changed = request; changed.reserved = 1;
    assert(invoke(changed) == ERROR_INVALID_DATA && address() == 0x5678);
    delay(true); request.address = 0x9000;
    assert(invoke(request, 4500) == ERROR_TIMEOUT);
    assert(!stop()); // Queued DLL code cannot be unloaded.
    delay(false); flush();
    assert(address() == 0x5678); // Cancelled GUI request cannot navigate later.
    auto waitQueued = [&]() {
        for (unsigned attempt = 0; attempt < 200 && queuedCount() == 0; ++attempt) Sleep(10);
        assert(queuedCount() == 1);
    };
    // The callback has started, but the target snapshot is still blocked when
    // the pipe worker cancels it. No later GUI operation may escape that timeout.
    delay(true); snapshotDelay(5200); request.address = 0xa000;
    DWORD slowResult = ERROR_SUCCESS;
    std::thread slowRequest([&]() { slowResult = invoke(request); });
    waitQueued(); delay(false); flush(); slowRequest.join();
    assert(slowResult == ERROR_TIMEOUT && address() == 0x5678);
    assert((response.flags & Running) == 0); // Failure response contains no success claim.
    // CPU navigation needs CIP even with an explicit destination. Freeze that
    // value before committing; a slow CIP query remains safely cancellable.
    delay(true); cipDelay(5200); request.view = View::Disassembly; request.address = 0xa800;
    std::thread cipRequest([&]() { slowResult = invoke(request); });
    waitQueued(); delay(false); flush(); cipRequest.join();
    assert(slowResult == ERROR_TIMEOUT && address() == 0x5678);
    // Once the first GUI call has started, cancellation is no longer possible.
    // Report an explicitly uncertain in-progress result instead of pretending
    // that timeout prevented all side effects.
    delay(true); navigationDelay(5200); request.address = 0xb000;
    std::thread committedRequest([&]() { slowResult = invoke(request); });
    waitQueued(); delay(false); flush(); committedRequest.join();
    assert(slowResult == ERROR_IO_PENDING && address() == 0xb000);
    assert(stop());
    FreeLibrary(plugin); FreeLibrary(debugger); FreeLibrary(bridge);
    std::puts("PASS: navigation identity, paused/running preservation, typed protocol, delayed response receipt and 1000 exchanges, confirmed address, queued/in-progress snapshot/CIP cancellation, committed result uncertainty and unload lifetime. No debug attachment was performed.");
}
