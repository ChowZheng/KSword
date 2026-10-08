#include <Windows.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <cstdint>
#include <array>

// A deliberately minimal frontend fixture. It never opens, attaches, suspends,
// resumes, or changes another process. No execution-control API is exported.
namespace
{
    using Address = std::uintptr_t;
    struct Selection { Address start, end; };
    std::atomic<bool> running{false}, delayed{false};
    std::atomic<DWORD> snapshotDelay{0}, cipDelay{0}, navigationDelay{0};
    std::atomic<Address> selected{0};
    std::mutex mutex;
    using Callback = void (*)(void*);
    std::vector<std::pair<Callback, void*>> callbacks;
    using DebugCallback = void (*)(int, void*);
    std::array<std::atomic<DebugCallback>, 13> debugCallbacks{};
}
extern "C"
{
    __declspec(dllexport) bool DbgIsDebugging() { return true; }
    __declspec(dllexport) bool DbgIsRunning() { return running.load(); }
    __declspec(dllexport) HANDLE DbgGetProcessHandle()
    { if (const DWORD delay = snapshotDelay.exchange(0)) Sleep(delay); return GetCurrentProcess(); }
    __declspec(dllexport) Address DbgValFromString(const char*)
    { if (const DWORD delay = cipDelay.exchange(0)) Sleep(delay); return 0x4000; }
    __declspec(dllexport) void GuiExecuteOnGuiThreadEx(Callback callback, void* data)
    { if (!delayed.load()) callback(data); else { std::lock_guard<std::mutex> guard(mutex); callbacks.emplace_back(callback, data); } }
    __declspec(dllexport) void GuiDisasmAt(Address address, Address)
    { if (const DWORD delay = navigationDelay.exchange(0)) Sleep(delay); selected = address; }
    __declspec(dllexport) void GuiDumpAt(Address address)
    { if (const DWORD delay = navigationDelay.exchange(0)) Sleep(delay); selected = address; }
    __declspec(dllexport) bool GuiSelectionGet(int, Selection* data) { const auto address = selected.load(); *data = {address, address}; return true; }
    __declspec(dllexport) bool GuiSelectionSet(int, const Selection* data) { selected = data->start; return true; }
    __declspec(dllexport) void GuiShowCpu() {}
    __declspec(dllexport) void GuiFocusView(int) {}
    __declspec(dllexport) HWND GuiGetWindowHandle() { return nullptr; }
    __declspec(dllexport) bool BridgeSettingSetUint(const char*, const char*, Address) { return true; }
    __declspec(dllexport) void _plugin_registercallback(int, int type, DebugCallback callback)
    { if (type >= 0 && type < static_cast<int>(debugCallbacks.size())) debugCallbacks[static_cast<std::size_t>(type)] = callback; }
    __declspec(dllexport) bool _plugin_unregistercallback(int, int type)
    { if (type >= 0 && type < static_cast<int>(debugCallbacks.size())) debugCallbacks[static_cast<std::size_t>(type)] = nullptr; return true; }
    __declspec(dllexport) void FixtureEvent(int type)
    { if (type >= 0 && type < static_cast<int>(debugCallbacks.size())) if (auto callback = debugCallbacks[static_cast<std::size_t>(type)].load()) callback(type, nullptr); }
    __declspec(dllexport) void FixtureSetRunning(bool value) { running = value; }
    __declspec(dllexport) void FixtureDelay(bool value) { delayed = value; }
    __declspec(dllexport) void FixtureSnapshotDelay(DWORD value) { snapshotDelay = value; }
    __declspec(dllexport) void FixtureCipDelay(DWORD value) { cipDelay = value; }
    __declspec(dllexport) void FixtureNavigationDelay(DWORD value) { navigationDelay = value; }
    __declspec(dllexport) unsigned FixtureQueuedCount()
    { std::lock_guard<std::mutex> guard(mutex); return static_cast<unsigned>(callbacks.size()); }
    __declspec(dllexport) void FixtureFlush()
    {
        std::vector<std::pair<Callback, void*>> pending;
        { std::lock_guard<std::mutex> guard(mutex); pending.swap(callbacks); }
        for (const auto& item : pending) item.first(item.second);
    }
    __declspec(dllexport) Address FixtureAddress() { return selected.load(); }
}
