#include "Proxy.h"
#include <filesystem>
#include <mutex>
#include <vector>

namespace ksword::titan
{
    NativeApi nativeApi{};
    std::atomic<bool> hvmSelected{false};
    std::recursive_mutex policyMutex;
    HMODULE selfModule = nullptr;
    DWORD installNativeSeam(HMODULE module);
    void startControlWorker();

    void log(const std::string& message) { debugger::backend().log("[TitanEngine] " + message); }

    DWORD ensureNative()
    {
        const DWORD previousError = GetLastError();
        // Canonical entry points run after the host has left its loader lock.
        // Modules and import thunks remain pinned for the process lifetime.
        static std::once_flag once;
        static DWORD error = ERROR_DLL_INIT_FAILED;
        std::call_once(once, [] {
            wchar_t ownPath[32768]{};
            const DWORD count = GetModuleFileNameW(selfModule, ownPath, _countof(ownPath));
            if (count == 0 || count >= _countof(ownPath)) { error = ERROR_BAD_PATHNAME; return; }
            const auto proxyPath = std::filesystem::path(ownPath).lexically_normal();
            const auto nativePath = (proxyPath.parent_path().parent_path() / L"TitanEngine.dll").lexically_normal();
            if (CompareStringOrdinal(proxyPath.c_str(), -1, nativePath.c_str(), -1, TRUE) == CSTR_EQUAL)
            { error = ERROR_INVALID_NAME; log("Recursive native path rejected"); return; }
            using CheckedLoader = HMODULE (*)(const wchar_t*, bool);
            auto bridge = GetModuleHandleW(L"x64bridge.dll");
            if (bridge == nullptr) bridge = GetModuleHandleW(L"x64_bridge.dll");
            const auto checked = bridge == nullptr ? nullptr : reinterpret_cast<CheckedLoader>(
                GetProcAddress(bridge, "BridgeLoadLibraryCheckedW"));
            HMODULE native = checked != nullptr ? checked(nativePath.c_str(), true) :
                LoadLibraryExW(nativePath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (native == nullptr) { error = GetLastError(); log("Native TitanEngine could not be loaded: " + std::to_string(error)); return; }
            if (native == selfModule || GetProcAddress(native, "KSwordTitanInitialize") != nullptr)
            { error = ERROR_INVALID_NAME; log("Native loader returned a KSword proxy; recursive forwarding rejected"); return; }
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(native);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const BYTE*>(native) + dos->e_lfanew);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE ||
                nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
            { error = ERROR_BAD_EXE_FORMAT; log("Native TitanEngine is not AMD64"); return; }
#define KSW_RESOLVE(name) nativeApi.name = reinterpret_cast<decltype(nativeApi.name)>(GetProcAddress(native, #name)); \
            if (nativeApi.name == nullptr) { error = ERROR_PROC_NOT_FOUND; log("Missing canonical native export: " #name); return; }
#include "ResolveExports.inc"
#undef KSW_RESOLVE
            HMODULE pinned = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(nativeApi.GetSessionInfo), &pinned) ||
                !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                    reinterpret_cast<LPCWSTR>(&KSwordTitanInitialize), &pinned))
            { error = GetLastError(); log("Module lifetime pin failed"); return; }
            error = installNativeSeam(native);
            if (error != ERROR_SUCCESS) { log("Native event/context import seam failed: " + std::to_string(error)); return; }
            log("64 canonical exports validated; native forwarding enabled; HVM initially disabled");
            startControlWorker();
        });
        SetLastError(error == ERROR_SUCCESS ? previousError : error);
        return error;
    }
}

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) ksword::titan::selfModule = instance;
    return TRUE;
}

extern "C" DWORD __stdcall KSwordTitanInitialize() { return ksword::titan::ensureNative(); }
