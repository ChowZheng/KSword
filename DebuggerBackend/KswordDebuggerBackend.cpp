#include "KswordDebuggerBackend.h"

#include <cstring>
#include <fstream>
#include <limits>
#include <vector>

namespace ksword::debugger
{
    Backend& backend()
    {
        static Backend instance;
        return instance;
    }

    void Backend::log(const std::string& message)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        wchar_t path[4096]{};
        const DWORD count = GetEnvironmentVariableW(L"KSWORD_DEBUGGER_LOG_FILE", path, _countof(path));
        if (count == 0 || count >= _countof(path)) return;
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        const bool reset = GetFileAttributesExW(path, GetFileExInfoStandard, &attributes) &&
            (attributes.nFileSizeHigh != 0 || attributes.nFileSizeLow > 16U * 1024U * 1024U);
        std::ofstream stream(path, std::ios::binary | (reset ? std::ios::trunc : std::ios::app));
        if (reset) stream << "[backend] Older logs were retired after the 16 MiB limit.\n";
        SYSTEMTIME time{};
        GetLocalTime(&time);
        char timestamp[64]{};
        (void)sprintf_s(timestamp, "%04u-%02u-%02u %02u:%02u:%02u [backend] ",
            time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
        std::string line = message.substr(0, 4000);
        for (char& c : line) if (c == '\n' || c == '\r') c = ' ';
        stream << timestamp << line << '\n';
    }

    bool Backend::initialize()
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (driver_.isValid()) return true;
        driver_ = client_.open();
        if (!driver_.isValid()) return false;
        log("KSword debugger backend API v1 loaded; memory and debugger adapters ready");
        return true;
    }

    KSWORD_DEBUGGER_BACKEND_STATUS Backend::status()
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        retireDetachedTarget();
        KSWORD_DEBUGGER_BACKEND_STATUS result{};
        result.version = KSWORD_DEBUGGER_API_VERSION;
        result.size = sizeof(result);
        result.driverReady = driver_.isValid() ? 1U : 0U;
        result.useHvm = useHvm_ ? 1U : 0U;
        result.ownsResident = ownsResident_ ? 1U : 0U;
        result.attachedProcessId = attachedPid_;
        result.lastError = lastError_;
        if (!driver_.isValid()) return result;
        const auto window = client_.hvmMemory(KSWORD_ARK_HVM_MEMORY_OP_QUERY_WINDOW,
            0, 0, 0, nullptr, false, false, 0, &driver_);
        result.directMemoryWindow = window.io.ok && window.response.windowReady != 0 ? 1U : 0U;
        const auto hvm = client_.queryHvmStatus();
        result.residentActive = hvm.io.ok &&
            (hvm.response.stateFlags & KSWORD_ARK_HVM_STATE_RESIDENT_ACTIVE) != 0 ? 1U : 0U;
        KSWORD_ARK_HVM_DEBUG_REQUEST query{};
        KSWORD_ARK_HVM_DEBUG_RESPONSE response{};
        query.version = KSWORD_ARK_HVM_DEBUG_VERSION;
        query.size = sizeof(query);
        if (breakpointRequest(query, response) == ERROR_SUCCESS && response.supported != 0)
            result.eptBreakpointProtocol = response.version;
        return result;
    }

    DWORD Backend::setUseHvm(const bool enabled)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (enabled == useHvm_) return ERROR_SUCCESS;
        if (enabled)
        {
            const auto window = client_.hvmMemory(KSWORD_ARK_HVM_MEMORY_OP_QUERY_WINDOW,
                0, 0, 0, nullptr, false, false, 0, &driver_);
            if (!window.io.ok || window.response.windowReady == 0)
            {
                lastError_ = window.io.ok ? ERROR_NOT_SUPPORTED : window.io.win32Error;
                log("HVM unavailable: private memory window is not ready; selection rejected");
                return lastError_;
            }
        }
        else if (!breakpoints_.empty())
        {
            lastError_ = ERROR_BUSY;
            log("Remove the active EPT breakpoints before switching off HVM");
            return lastError_;
        }
        if (!enabled && ownsResident_)
        {
            const DWORD error = releaseOwnedHvm();
            if (error != ERROR_SUCCESS) { lastError_ = error; return error; }
        }
        useHvm_ = enabled;
        lastError_ = ERROR_SUCCESS;
        log(enabled ? "HVM selected: strict private-window reads/writes; EPT debugger enabled"
            : "R0 selected: KSword memory backend; HVM is disabled for this debugger");
        return ERROR_SUCCESS;
    }

    DWORD Backend::dispatch(KSWORD_DEBUGGER_CALL& call)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        call.bytesReturned = 0;
        if (call.version != KSWORD_DEBUGGER_API_VERSION || call.size != sizeof(call) || call.reserved != 0)
            return ERROR_REVISION_MISMATCH;
        if (!driver_.isValid()) return ERROR_DEVICE_NOT_CONNECTED;
        auto* output = reinterpret_cast<void*>(static_cast<std::uintptr_t>(call.output));
        const auto* input = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(call.input));
        if (call.command == KSWORD_DEBUGGER_QUERY_BACKEND || call.command == KSWORD_DEBUGGER_USE_HVM)
        {
            if (output == nullptr || call.outputBytes < sizeof(KSWORD_DEBUGGER_BACKEND_STATUS))
                return ERROR_INSUFFICIENT_BUFFER;
            if (call.command == KSWORD_DEBUGGER_USE_HVM)
            {
                if (input == nullptr || call.inputBytes != sizeof(DWORD)) return ERROR_INVALID_PARAMETER;
                DWORD enabled = 0;
                std::memcpy(&enabled, input, sizeof(enabled));
                if (enabled > 1) return ERROR_INVALID_PARAMETER;
                const DWORD error = setUseHvm(enabled != 0);
                const auto state = status();
                std::memcpy(output, &state, sizeof(state));
                call.bytesReturned = sizeof(state);
                return error;
            }
            if (call.inputBytes != 0) return ERROR_INVALID_PARAMETER;
            const auto state = status();
            std::memcpy(output, &state, sizeof(state));
            call.bytesReturned = sizeof(state);
            return ERROR_SUCCESS;
        }
        struct Operation { DWORD command, ioctl, inputBytes, outputBytes; };
#define KSW_OPERATION(command, ioctl, request, response) \
        {command, ioctl, static_cast<DWORD>(sizeof(request)), static_cast<DWORD>(sizeof(response))}
        static constexpr Operation operations[] = {
            KSW_OPERATION(KSWORD_DEBUGGER_HVM_STATUS, IOCTL_KSWORD_ARK_QUERY_HVM, KSWORD_ARK_QUERY_HVM_REQUEST, KSWORD_ARK_QUERY_HVM_RESPONSE),
            KSW_OPERATION(KSWORD_DEBUGGER_HVM_CONTROL, IOCTL_KSWORD_ARK_CONTROL_HVM, KSWORD_ARK_CONTROL_HVM_REQUEST, KSWORD_ARK_CONTROL_HVM_RESPONSE),
            KSW_OPERATION(KSWORD_DEBUGGER_HVM_MEMORY, IOCTL_KSWORD_ARK_HVM_MEMORY, KSWORD_ARK_HVM_MEMORY_REQUEST, KSWORD_ARK_HVM_MEMORY_RESPONSE),
            KSW_OPERATION(KSWORD_DEBUGGER_HVM_EPT_RULE, IOCTL_KSWORD_ARK_HVM_EPT_RULE, KSWORD_ARK_HVM_EPT_RULE_REQUEST, KSWORD_ARK_HVM_EPT_RULE_RESPONSE),
            KSW_OPERATION(KSWORD_DEBUGGER_HVM_EVENTS, IOCTL_KSWORD_ARK_HVM_EVENTS, KSWORD_ARK_HVM_EVENT_QUERY_REQUEST, KSWORD_ARK_HVM_EVENT_QUERY_RESPONSE),
            KSW_OPERATION(KSWORD_DEBUGGER_HVM_VIEW, IOCTL_KSWORD_ARK_HVM_VIEW, KSWORD_ARK_HVM_VIEW_REQUEST, KSWORD_ARK_HVM_VIEW_RESPONSE),
            KSW_OPERATION(KSWORD_DEBUGGER_HVM_CR_POLICY, IOCTL_KSWORD_ARK_HVM_CR_POLICY, KSWORD_ARK_HVM_CR_POLICY_REQUEST, KSWORD_ARK_HVM_CR_POLICY_RESPONSE),
            KSW_OPERATION(KSWORD_DEBUGGER_HVM_MSR_POLICY, IOCTL_KSWORD_ARK_HVM_MSR_POLICY, KSWORD_ARK_HVM_MSR_POLICY_REQUEST, KSWORD_ARK_HVM_MSR_POLICY_RESPONSE),
            KSW_OPERATION(KSWORD_DEBUGGER_HVM_DOMAIN, IOCTL_KSWORD_ARK_HVM_DOMAIN, KSWORD_ARK_HVM_DOMAIN_REQUEST, KSWORD_ARK_HVM_DOMAIN_RESPONSE),
            KSW_OPERATION(KSWORD_DEBUGGER_HVM_PROCESS, IOCTL_KSWORD_ARK_HVM_PROCESS, KSWORD_ARK_HVM_PROCESS_REQUEST, KSWORD_ARK_HVM_PROCESS_RESPONSE),
            KSW_OPERATION(KSWORD_DEBUGGER_HVM_INJECT, IOCTL_KSWORD_ARK_HVM_INJECT, KSWORD_ARK_HVM_INJECT_REQUEST, KSWORD_ARK_HVM_INJECT_RESPONSE),
            KSW_OPERATION(KSWORD_DEBUGGER_HVM_PLATFORM, IOCTL_KSWORD_ARK_HVM_PLATFORM, KSWORD_ARK_HVM_PLATFORM_REQUEST, KSWORD_ARK_HVM_PLATFORM_RESPONSE),
            KSW_OPERATION(KSWORD_DEBUGGER_HVM_METRICS, IOCTL_KSWORD_ARK_HVM_METRICS, KSWORD_ARK_HVM_METRICS_REQUEST, KSWORD_ARK_HVM_METRICS_RESPONSE),
            KSW_OPERATION(KSWORD_DEBUGGER_HVM_NESTED_PROBE, IOCTL_KSWORD_ARK_HVM_NESTED_PROBE, KSWORD_ARK_HVM_NESTED_PROBE_REQUEST, KSWORD_ARK_HVM_NESTED_PROBE_RESPONSE),
            KSW_OPERATION(KSWORD_DEBUGGER_HVM_NESTED_PAGE, IOCTL_KSWORD_ARK_HVM_NESTED_PAGE, KSWORD_ARK_HVM_NESTED_PAGE_REQUEST, KSWORD_ARK_HVM_NESTED_PAGE_RESPONSE),
            KSW_OPERATION(KSWORD_DEBUGGER_HVM_BREAKPOINT, IOCTL_KSWORD_ARK_HVM_DEBUG, KSWORD_ARK_HVM_DEBUG_REQUEST, KSWORD_ARK_HVM_DEBUG_RESPONSE),
            KSW_OPERATION(KSWORD_DEBUGGER_NATIVE, IOCTL_KSWORD_ARK_DEBUGGER, KSWORD_ARK_DEBUGGER_REQUEST, KSWORD_ARK_DEBUGGER_RESPONSE)
        };
#undef KSW_OPERATION
        if (call.command == KSWORD_DEBUGGER_OPERATION_LAYOUT)
        {
            if (input == nullptr || call.inputBytes != sizeof(DWORD)) return ERROR_INVALID_PARAMETER;
            if (output == nullptr || call.outputBytes < sizeof(KSWORD_DEBUGGER_OPERATION_INFO)) return ERROR_INSUFFICIENT_BUFFER;
            DWORD command = 0; std::memcpy(&command, input, sizeof(command));
            for (const auto& operation : operations)
            {
                if (operation.command != command) continue;
                DWORD protocol = KSWORD_ARK_HVM_PROTOCOL_VERSION;
                switch (command)
                {
                case KSWORD_DEBUGGER_HVM_MEMORY: protocol = KSWORD_ARK_HVM_MEMORY_PROTOCOL_VERSION; break;
                case KSWORD_DEBUGGER_HVM_VIEW: protocol = KSWORD_ARK_HVM_VIEW_PROTOCOL_VERSION; break;
                case KSWORD_DEBUGGER_HVM_CR_POLICY: protocol = KSWORD_ARK_HVM_CR_POLICY_PROTOCOL_VERSION; break;
                case KSWORD_DEBUGGER_HVM_MSR_POLICY: protocol = KSWORD_ARK_HVM_MSR_POLICY_PROTOCOL_VERSION; break;
                case KSWORD_DEBUGGER_HVM_DOMAIN: protocol = KSWORD_ARK_HVM_DOMAIN_PROTOCOL_VERSION; break;
                case KSWORD_DEBUGGER_HVM_PROCESS: protocol = KSWORD_ARK_HVM_PROCESS_PROTOCOL_VERSION; break;
                case KSWORD_DEBUGGER_HVM_INJECT: protocol = KSWORD_ARK_HVM_INJECT_PROTOCOL_VERSION; break;
                case KSWORD_DEBUGGER_HVM_PLATFORM: protocol = KSWORD_ARK_HVM_PLATFORM_PROTOCOL_VERSION; break;
                case KSWORD_DEBUGGER_HVM_METRICS: protocol = KSWORD_ARK_HVM_METRICS_VERSION; break;
                case KSWORD_DEBUGGER_HVM_NESTED_PROBE: protocol = KSWORD_ARK_HVM_NESTED_PROBE_PROTOCOL_VERSION; break;
                case KSWORD_DEBUGGER_HVM_NESTED_PAGE: protocol = KSWORD_ARK_HVM_NESTED_PAGE_VERSION; break;
                case KSWORD_DEBUGGER_HVM_BREAKPOINT: protocol = KSWORD_ARK_HVM_DEBUG_VERSION; break;
                case KSWORD_DEBUGGER_NATIVE: protocol = KSWORD_ARK_DEBUGGER_VERSION; break;
                default: break;
                }
                const KSWORD_DEBUGGER_OPERATION_INFO info{KSWORD_DEBUGGER_API_VERSION, command,
                    operation.inputBytes, operation.outputBytes, protocol, 0};
                std::memcpy(output, &info, sizeof(info)); call.bytesReturned = sizeof(info);
                return ERROR_SUCCESS;
            }
            return ERROR_NOT_SUPPORTED;
        }
        for (const auto& operation : operations)
        {
            if (operation.command != call.command) continue;
            if (input == nullptr || call.inputBytes != operation.inputBytes) return ERROR_INVALID_PARAMETER;
            if (output == nullptr || call.outputBytes < operation.outputBytes) return ERROR_INSUFFICIENT_BUFFER;
            // Preserve input even when the caller reuses one buffer for request/response.
            std::vector<unsigned char> snapshot(call.inputBytes);
            std::memcpy(snapshot.data(), input, snapshot.size());
            if (!breakpoints_.empty())
            {
                DWORD action = 0;
                if (snapshot.size() >= 12) std::memcpy(&action, snapshot.data() + 8, sizeof(action));
                bool conflicts = false;
                switch (call.command)
                {
                case KSWORD_DEBUGGER_HVM_CONTROL: conflicts = true; break;
                case KSWORD_DEBUGGER_HVM_EPT_RULE: conflicts = action != KSWORD_ARK_HVM_EPT_RULE_QUERY && action != KSWORD_ARK_HVM_EPT_RULE_WATCH_QUERY; break;
                case KSWORD_DEBUGGER_HVM_VIEW: conflicts = action != KSWORD_ARK_HVM_VIEW_OP_QUERY; break;
                case KSWORD_DEBUGGER_HVM_CR_POLICY: conflicts = action != KSWORD_ARK_HVM_CR_POLICY_OP_QUERY; break;
                case KSWORD_DEBUGGER_HVM_MSR_POLICY: conflicts = action != KSWORD_ARK_HVM_MSR_POLICY_OP_QUERY; break;
                case KSWORD_DEBUGGER_HVM_DOMAIN: conflicts = action != KSWORD_ARK_HVM_DOMAIN_OP_QUERY; break;
                case KSWORD_DEBUGGER_HVM_BREAKPOINT: conflicts = action != KSWORD_ARK_HVM_DEBUG_QUERY; break;
                case KSWORD_DEBUGGER_NATIVE:
                {
                    DWORD flags = 0;
                    std::memcpy(&flags, snapshot.data() + offsetof(KSWORD_ARK_DEBUGGER_REQUEST, contextFlags), sizeof(flags));
                    conflicts = action == KSWORD_ARK_DEBUGGER_SET_CONTEXT && (flags & CONTEXT_DEBUG_REGISTERS) == CONTEXT_DEBUG_REGISTERS;
                    break;
                }
                default: break;
                }
                if (conflicts) { log("Raw policy edit refused while this debugger owns EPT breakpoints"); return ERROR_BUSY; }
            }
            const auto result = client_.deviceIoControl(operation.ioctl, snapshot.data(), operation.inputBytes,
                output, operation.outputBytes, &driver_);
            call.bytesReturned = result.bytesReturned;
            if (!result.ok) { log(result.message); return result.win32Error; }
            if (result.bytesReturned != operation.outputBytes) return ERROR_INVALID_DATA;
            return ERROR_SUCCESS; // Protocol-level NTSTATUS remains in the typed response.
        }
        return ERROR_NOT_SUPPORTED;
    }
}

static bool debuggerCopy(void* output, const void* input, std::size_t bytes) noexcept
{
    __try { if (bytes != 0) std::memcpy(output, input, bytes); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

extern "C" unsigned long __stdcall KSwordDebuggerCall(KSWORD_DEBUGGER_CALL* call)
{
    if (call == nullptr) return ERROR_INVALID_PARAMETER;
    KSWORD_DEBUGGER_CALL snapshot{};
    if (!debuggerCopy(&snapshot, call, sizeof(snapshot))) return ERROR_NOACCESS;
    snapshot.bytesReturned = 0;
    snapshot.error = ERROR_SUCCESS;
    try
    {
        if (snapshot.version != KSWORD_DEBUGGER_API_VERSION || snapshot.size != sizeof(snapshot) || snapshot.reserved != 0)
            snapshot.error = ERROR_REVISION_MISMATCH;
        else if (snapshot.inputBytes > 4U * 1024U * 1024U || snapshot.outputBytes > 8U * 1024U * 1024U)
            snapshot.error = ERROR_BAD_LENGTH;
        else
        {
            // Foreign pointers are accessed only through SEH copy helpers outside
            // backend locks. An invalid adapter buffer cannot strand a C++ mutex.
            std::vector<unsigned char> input(snapshot.inputBytes), output(snapshot.outputBytes);
            if (!debuggerCopy(input.data(), reinterpret_cast<const void*>(static_cast<std::uintptr_t>(snapshot.input)), input.size()))
                snapshot.error = ERROR_NOACCESS;
            else
            {
                KSWORD_DEBUGGER_CALL owned = snapshot;
                owned.input = input.empty() ? 0 : reinterpret_cast<std::uintptr_t>(input.data());
                owned.output = output.empty() ? 0 : reinterpret_cast<std::uintptr_t>(output.data());
                snapshot.error = ksword::debugger::backend().dispatch(owned);
                snapshot.bytesReturned = owned.bytesReturned;
                if (snapshot.bytesReturned > output.size()) { snapshot.error = ERROR_INVALID_DATA; snapshot.bytesReturned = 0; }
                else if (!debuggerCopy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(snapshot.output)), output.data(), snapshot.bytesReturned))
                    snapshot.error = ERROR_NOACCESS;
            }
        }
    }
    catch (const std::bad_alloc&) { snapshot.error = ERROR_NOT_ENOUGH_MEMORY; }
    catch (...) { snapshot.error = ERROR_GEN_FAILURE; }
    return debuggerCopy(call, &snapshot, sizeof(snapshot)) ? snapshot.error : ERROR_NOACCESS;
}
