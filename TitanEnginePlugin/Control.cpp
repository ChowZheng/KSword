#include "Proxy.h"
#include "../DebuggerBackend/KswordDebuggerFileProtocol.h"
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

extern "C" unsigned long __stdcall KSwordBackendCall(KSWORD_DEBUGGER_CALL* call);

namespace ksword::titan
{
    namespace
    {
        std::mutex controlMutex;
        std::wstring environment(const wchar_t* name)
        {
            wchar_t buffer[32768]{};
            const DWORD count = GetEnvironmentVariableW(name, buffer, _countof(buffer));
            return count == 0 || count >= _countof(buffer) ? std::wstring{} : std::wstring(buffer, count);
        }

        DWORD selectHvm(DWORD enabled, KSWORD_DEBUGGER_BACKEND_STATUS& state)
        {
            std::lock_guard<std::mutex> lock(controlMutex);
            std::lock_guard<std::recursive_mutex> policy(policyMutex);
            if (enabled > 1) { state = debugger::backend().status(); return ERROR_INVALID_PARAMETER; }
            if ((enabled != 0) != hvmSelected.load() && nativeProcessId() != 0 && !nativeEventHeld())
            { state = debugger::backend().status(); log("HVM selection refused: pause the native debugger first"); return ERROR_BUSY; }
            if (enabled == 0 && hvmSelected.load() && (hasHardwareBindings() || hasPendingDebug()))
            { state = debugger::backend().status(); log("HVM selection refused: remove EPT hardware breakpoints first"); return ERROR_BUSY; }
            if (enabled != 0 && !hvmSelected.load() && hasHardwareBindings())
            {
                state = debugger::backend().status();
                log("HVM selection refused: remove the existing native hardware breakpoints first");
                return ERROR_BUSY;
            }
            if (enabled != 0 && !debugger::backend().initialize())
            { const DWORD error = GetLastError(); state = debugger::backend().status(); return error; }
            DWORD error = debugger::backend().setUseHvm(enabled != 0);
            state = debugger::backend().status();
            hvmSelected = state.useHvm != 0;
            if (error == ERROR_SUCCESS && hvmSelected.load() && nativeEventHeld())
            {
                error = adoptCurrentNativeSession();
                if (error != ERROR_SUCCESS)
                {
                    (void)debugger::backend().setUseHvm(false);
                    (void)releaseSession();
                    state = debugger::backend().status(); hvmSelected = state.useHvm != 0;
                }
            }
            // Adoption changes the attached PID; publish the state after it.
            state = debugger::backend().status();
            return error;
        }
        void writeAck(const std::filesystem::path& path, const std::string& session,
            std::uint64_t revision, DWORD error, const KSWORD_DEBUGGER_BACKEND_STATUS& state)
        {
            const auto temporary = std::filesystem::path(path.wstring() + L".tmp");
            {
                std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
                if (!stream) return;
                stream << session << ' ' << revision << ' ' << error << ' ' << state.useHvm << ' '
                    << state.driverReady << ' ' << state.residentActive << ' ' << state.eptBreakpointProtocol << '\n';
                stream.flush(); if (!stream) return;
            }
            (void)MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        }
    }

    void startControlWorker()
    {
        const auto control = environment(L"KSWORD_DEBUGGER_CONTROL_FILE");
        const auto state = environment(L"KSWORD_DEBUGGER_STATE_FILE");
        const auto wideSession = environment(L"KSWORD_DEBUGGER_SESSION_ID");
        if (control.empty() || state.empty() || wideSession.empty()) return;
        std::string session;
        for (const wchar_t c : wideSession)
        {
            if (c < 0x21 || c > 0x7E) return;
            session.push_back(static_cast<char>(c));
        }
        if (session.find_first_of(" \r\n\t") != std::string::npos || session.size() > 128) return;
        // Both proxy and native module were pinned before this worker starts.
        std::thread([control, state, session] {
            std::uint64_t previous = 0;
            writeAck(state, session, previous, ERROR_SUCCESS, debugger::backend().status());
            for (;;)
            {
                std::string packet;
                (void)debugger::readControlPacket(control, packet);
                std::istringstream stream(packet);
                std::string candidate; std::uint64_t revision = 0; DWORD requested = 0;
                if (stream >> candidate >> revision >> requested && candidate == session && revision > previous)
                {
                    KSWORD_DEBUGGER_BACKEND_STATUS actual{};
                    const DWORD error = selectHvm(requested, actual);
                    previous = revision; writeAck(state, session, revision, error, actual);
                    log("Log Tab HVM request " + std::to_string(revision) + ": " + std::to_string(error));
                }
                Sleep(100);
            }
        }).detach();
    }
}

static bool safeStatusCopy(KSWORD_DEBUGGER_BACKEND_STATUS* destination,
    const KSWORD_DEBUGGER_BACKEND_STATUS* source) noexcept
{
    __try { if (destination != nullptr) *destination = *source; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool safeCallCopy(KSWORD_DEBUGGER_CALL* destination, const KSWORD_DEBUGGER_CALL* source) noexcept
{
    __try { *destination = *source; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool safeDwordCopy(DWORD* destination, const DWORD* source) noexcept
{
    __try { *destination = *source; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

extern "C" DWORD __stdcall KSwordTitanControl(DWORD enabled, KSWORD_DEBUGGER_BACKEND_STATUS* status)
{
    using namespace ksword::titan;
    DWORD error = ensureNative();
    if (error != ERROR_SUCCESS) return error;
    KSWORD_DEBUGGER_BACKEND_STATUS actual{};
    error = selectHvm(enabled, actual);
    if (!safeStatusCopy(status, &actual)) return ERROR_NOACCESS;
    return error;
}

extern "C" unsigned long __stdcall KSwordDebuggerCall(KSWORD_DEBUGGER_CALL* call)
{
    const DWORD error = ksword::titan::ensureNative();
    if (error != ERROR_SUCCESS) return error;
    KSWORD_DEBUGGER_CALL snapshot{};
    if (call == nullptr || !safeCallCopy(&snapshot, call)) return ERROR_NOACCESS;
    if (snapshot.command == KSWORD_DEBUGGER_USE_HVM)
    {
        snapshot.bytesReturned = 0;
        if (snapshot.version != KSWORD_DEBUGGER_API_VERSION || snapshot.size != sizeof(snapshot) || snapshot.reserved != 0)
            snapshot.error = ERROR_REVISION_MISMATCH;
        else if (snapshot.inputBytes != sizeof(DWORD)) snapshot.error = ERROR_INVALID_PARAMETER;
        else if (snapshot.outputBytes < sizeof(KSWORD_DEBUGGER_BACKEND_STATUS)) snapshot.error = ERROR_INSUFFICIENT_BUFFER;
        else
        {
            DWORD enabled = 0;
            KSWORD_DEBUGGER_BACKEND_STATUS actual{};
            if (!safeDwordCopy(&enabled, reinterpret_cast<const DWORD*>(static_cast<std::uintptr_t>(snapshot.input))))
                snapshot.error = ERROR_NOACCESS;
            else
            {
                snapshot.error = ksword::titan::selectHvm(enabled, actual);
                if (snapshot.output == 0 || !safeStatusCopy(
                    reinterpret_cast<KSWORD_DEBUGGER_BACKEND_STATUS*>(static_cast<std::uintptr_t>(snapshot.output)), &actual))
                    snapshot.error = ERROR_NOACCESS;
                else snapshot.bytesReturned = sizeof(actual);
            }
        }
        return safeCallCopy(call, &snapshot) ? snapshot.error : ERROR_NOACCESS;
    }
    if (!ksword::debugger::backend().initialize()) return GetLastError();
    DWORD result = KSwordBackendCall(call);
    ksword::titan::hvmSelected = ksword::debugger::backend().status().useHvm != 0;
    if (result == ERROR_SUCCESS && snapshot.command == KSWORD_DEBUGGER_USE_HVM && ksword::titan::hvmSelected.load())
    {
        result = ksword::titan::adoptCurrentNativeSession();
        if (result != ERROR_SUCCESS)
        {
            (void)ksword::debugger::backend().setUseHvm(false);
            ksword::titan::hvmSelected = ksword::debugger::backend().status().useHvm != 0;
            if (!safeCallCopy(&snapshot, call)) return ERROR_NOACCESS;
            snapshot.error = result;
            if (!safeCallCopy(call, &snapshot)) return ERROR_NOACCESS;
        }
    }
    return result;
}
