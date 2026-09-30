#include "../KswordDebuggerBackend.h"

#include <cstring>
#include <future>
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>

namespace
{
    struct Model
    {
        bool prepared = false, resident = false, failStart = false;
        int failAddAfter = -1;
        DWORD nextId = 0;
        CONTEXT native{};
        std::vector<SIZE_T> writeSizes;
        DWORD readCalls = 0, windowTransfers = 0;
        bool missingWindow = false;
        std::map<DWORD, KSWORD_ARK_HVM_DEBUG_REQUEST> stops;
    } model;
    void require(bool condition, const char* message)
    { if (!condition) throw std::runtime_error(message); }
    ksword::ark::IoResult success(DWORD bytes = 0)
    { ksword::ark::IoResult result{}; result.ok = true; result.bytesReturned = bytes; return result; }
}

// The test links no ArkDriverClient implementation. Every driver operation uses
// this deterministic model; no driver device, remote target, GUI, or VMX is used.
namespace ksword::ark
{
    DriverHandle::DriverHandle(HANDLE value) noexcept : m_handle(value) {}
    DriverHandle::~DriverHandle() { reset(); }
    DriverHandle::DriverHandle(DriverHandle&& other) noexcept : m_handle(other.release()) {}
    DriverHandle& DriverHandle::operator=(DriverHandle&& other) noexcept
    { if (this != &other) reset(other.release()); return *this; }
    bool DriverHandle::isValid() const noexcept { return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE; }
    HANDLE DriverHandle::native() const noexcept { return m_handle; }
    HANDLE DriverHandle::release() noexcept { HANDLE value = m_handle; m_handle = INVALID_HANDLE_VALUE; return value; }
    void DriverHandle::reset(HANDLE value) noexcept { if (isValid()) CloseHandle(m_handle); m_handle = value; }
    DriverHandle DriverClient::open(unsigned long) const { return DriverHandle(CreateEventW(nullptr, FALSE, FALSE, nullptr)); }
    HvmStatusResult DriverClient::queryHvmStatus() const
    {
        HvmStatusResult result{}; result.io = success(sizeof(result.response));
        result.response.processorCount = 2; result.response.residentProcessorCount = model.resident ? 2 : 0;
        result.response.stateFlags = (model.prepared ? KSWORD_ARK_HVM_STATE_RESOURCES_READY : 0) |
            (model.resident ? KSWORD_ARK_HVM_STATE_RESIDENT_ACTIVE : 0);
        return result;
    }
    HvmControlResult DriverClient::controlHvm(unsigned long command, unsigned long, bool, bool, bool,
        bool, bool, bool, bool, bool, bool, bool, unsigned long, bool) const
    {
        HvmControlResult result{}; result.io = success(sizeof(result.response));
        if (command == KSWORD_ARK_HVM_CONTROL_PREPARE) model.prepared = true;
        if (command == KSWORD_ARK_HVM_CONTROL_START_RESIDENT)
        {
            if (model.failStart)
            {
                model.failStart = false;
                result.response.status = KSWORD_ARK_HVM_CONTROL_STATUS_VERIFY_FAILED;
                // A protocol rejection with nonnegative lastStatus must still fail.
                result.response.lastStatus = 0;
            }
            else model.resident = true;
        }
        if (command == KSWORD_ARK_HVM_CONTROL_STOP_RESIDENT) model.resident = false;
        if (command == KSWORD_ARK_HVM_CONTROL_TEARDOWN) { model.prepared = false; model.stops.clear(); }
        return result;
    }
    HvmMemoryResult DriverClient::hvmMemory(unsigned long operation, std::uint64_t, std::uint64_t,
        unsigned long bytes, const unsigned char*, bool, bool requireWindow, unsigned long, DriverHandle*) const
    {
        HvmMemoryResult result{}; result.io = success(sizeof(result.response)); result.response.windowReady = 1;
        if (operation == KSWORD_ARK_HVM_MEMORY_OP_READ_VIRTUAL || operation == KSWORD_ARK_HVM_MEMORY_OP_WRITE_VIRTUAL)
        {
            require(requireWindow && bytes <= KSWORD_ARK_HVM_MEMORY_MAX_BYTES, "HVM access lost its strict window contract");
            ++model.windowTransfers;
            result.response.usedDirectWindow = model.missingWindow ? 0 : 1;
            result.response.bytesTransferred = bytes;
            std::memset(result.response.data, 0x3a, bytes);
        }
        return result;
    }
    VirtualMemoryQueryResult DriverClient::queryVirtualMemory(std::uint32_t, std::uint64_t, unsigned long, DriverHandle*) const
    { VirtualMemoryQueryResult result{}; result.io.win32Error = ERROR_NOT_SUPPORTED; return result; }
    VirtualMemoryReadResult DriverClient::readVirtualMemory(std::uint32_t, std::uint64_t, std::uint32_t bytes,
        unsigned long, DriverHandle*) const
    {
        require(bytes <= KSWORD_ARK_MEMORY_READ_MAX_BYTES, "R0 read exceeded its protocol limit");
        ++model.readCalls;
        VirtualMemoryReadResult result{}; result.io = success(); result.readStatus = KSWORD_ARK_MEMORY_READ_STATUS_OK;
        result.data.assign(bytes, 0x5a);
        return result;
    }
    VirtualMemoryWriteResult DriverClient::writeVirtualMemory(std::uint32_t, std::uint64_t,
        const std::vector<std::uint8_t>& bytes, unsigned long flags, DriverHandle*) const
    {
        require(bytes.size() <= KSWORD_ARK_MEMORY_WRITE_MAX_BYTES && flags == KSWORD_ARK_MEMORY_WRITE_FLAG_UI_CONFIRMED,
            "R0 write exceeded its limit, lost confirmation, or added FORCE");
        model.writeSizes.push_back(bytes.size());
        VirtualMemoryWriteResult result{}; result.io = success(); result.writeStatus = KSWORD_ARK_MEMORY_WRITE_STATUS_OK;
        result.bytesWritten = static_cast<DWORD>(bytes.size());
        return result;
    }
    IoResult DriverClient::deviceIoControl(unsigned long ioctl, void* input, unsigned long,
        void* output, unsigned long outputBytes, DriverHandle*) const
    {
        if (ioctl == IOCTL_KSWORD_ARK_HVM_DEBUG)
        {
            const auto& request = *static_cast<const KSWORD_ARK_HVM_DEBUG_REQUEST*>(input);
            auto& response = *static_cast<KSWORD_ARK_HVM_DEBUG_RESPONSE*>(output);
            response = {}; response.version = KSWORD_ARK_HVM_DEBUG_VERSION; response.size = sizeof(response); response.supported = 1;
            if (request.operation == KSWORD_ARK_HVM_DEBUG_ADD)
            {
                require(!model.resident, "Rule mutation occurred during residency");
                if (model.failAddAfter == 0) { model.failAddAfter = -1; response.status = static_cast<LONG>(0xC0000017UL); }
                else
                {
                    if (model.failAddAfter > 0) --model.failAddAfter;
                    response.breakpointId = ++model.nextId; model.stops[response.breakpointId] = request;
                }
            }
            if (request.operation == KSWORD_ARK_HVM_DEBUG_REMOVE)
            {
                require(!model.resident, "Rule retirement occurred during residency");
                model.stops.erase(request.breakpointId);
            }
            return success(outputBytes);
        }
        if (ioctl == IOCTL_KSWORD_ARK_DEBUGGER)
        {
            const auto& request = *static_cast<const KSWORD_ARK_DEBUGGER_REQUEST*>(input);
            auto& response = *static_cast<KSWORD_ARK_DEBUGGER_RESPONSE*>(output);
            response = {}; response.version = KSWORD_ARK_DEBUGGER_VERSION; response.size = sizeof(response);
            if (request.operation == KSWORD_ARK_DEBUGGER_GET_CONTEXT)
            {
                CONTEXT native = model.native; native.ContextFlags = request.contextFlags;
                std::memcpy(response.context, &native, sizeof(native));
            }
            if (request.operation == KSWORD_ARK_DEBUGGER_SET_CONTEXT) std::memcpy(&model.native, request.context, sizeof(model.native));
            return success(outputBytes);
        }
        IoResult result{}; result.win32Error = ERROR_NOT_SUPPORTED; return result;
    }
}

namespace ksword::debugger
{
    struct BackendTestPeer
    {
        static void attach(Backend& value) { value.attachedPid_ = GetCurrentProcessId(); }
        static void observeDetach(Backend& value) { value.monitorAttachment_ = true; }
        static bool hasStops(Backend& value) { return !value.breakpoints_.empty(); }
    };
}

int main()
{
    try
    {
        static_assert(sizeof(KSWORD_ARK_DEBUGGER_REQUEST) == 1296);
        static_assert(sizeof(KSWORD_ARK_DEBUGGER_RESPONSE) == 1296);
        static_assert(offsetof(KSWORD_ARK_DEBUGGER_REQUEST, context) == 64);
        static_assert(sizeof(KSWORD_DEBUGGER_BACKEND_STATUS) == 40);
        const auto match = [](DWORD access, DWORD requested, DWORD slot, ULONGLONG dr7,
            ULONGLONG rf, DWORD cpl, ULONGLONG teb, ULONGLONG rip)
        { return KswordArkHvmDebugMatch(access, requested, slot, dr7, rf, cpl, teb, 0x4000, rip, 0x9000) != 0; };
        require(match(4, 4, 0, 1, 0, 3, 0x4000, 0x9000), "Execution stop did not match");
        require(!match(4, 4, 0, 1, 0x10000, 3, 0x4000, 0x9000), "RF did not suppress execution retry");
        require(!match(4, 4, 0, 1, 0, 0, 0x4000, 0x9000), "Kernel execution matched a user stop");
        require(!match(4, 4, 0, 1, 0, 3, 0x5000, 0x9000), "Another TEB matched a stop");
        require(!match(4, 4, 0, 0, 0, 3, 0x4000, 0x9000), "Disabled DR7 matched a stop");
        require(!match(4, 4, 4, 0xff, 0, 3, 0x4000, 0x9000), "Invalid register matched");
        require(!match(4, 4, 0, 1, 0, 3, 0x4000, 0x9001), "Another instruction matched execution");
        require(!match(1, 2, 0, 1, 0, 3, 0x4000, 0x9000), "Read matched a write-only stop");
        require(match(2, 2, 0, 1, 0x10000, 3, 0x4000, 0x9001), "RF incorrectly suppressed a page-level data stop");

        auto& backend = ksword::debugger::backend(); require(backend.initialize(), "Model initialization failed");
        ksword::ark::DriverHandle process(backend.openProcess(PROCESS_VM_OPERATION, FALSE, GetCurrentProcessId()));
        require(process.isValid(), "Process handle initialization failed");
        void* nativeAllocation = VirtualAllocEx(process.native(), nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        require(nativeAllocation != nullptr && VirtualFreeEx(process.native(), nativeAllocation, 0, MEM_RELEASE),
            "Process identity handle discarded requested rights used outside the SDK hooks");
        std::vector<unsigned char> data(KSWORD_ARK_MEMORY_WRITE_MAX_BYTES + 17, 0x6b);
        SIZE_T done = 0;
        require(backend.writeMemory(process.native(), reinterpret_cast<void*>(0x10000), data.data(), data.size(), &done) && done == data.size(),
            "R0 chunked write failed");
        require(model.writeSizes.size() == 2 && model.writeSizes[0] == KSWORD_ARK_MEMORY_WRITE_MAX_BYTES && model.writeSizes[1] == 17,
            "R0 write did not split at 256 KiB");
        data.resize(KSWORD_ARK_MEMORY_READ_MAX_BYTES + 3);
        require(backend.readMemory(process.native(), reinterpret_cast<void*>(0x10000), data.data(), data.size(), &done) &&
            done == data.size() && model.readCalls == 2 && data.back() == 0x5a, "R0 read did not split at 1 MiB");
        ksword::debugger::BackendTestPeer::attach(backend);
        require(backend.setUseHvm(true) == ERROR_SUCCESS, "HVM selection failed");
        data.resize(KSWORD_ARK_HVM_MEMORY_MAX_BYTES + 1);
        require(backend.readMemory(process.native(), reinterpret_cast<void*>(0x10000), data.data(), data.size(), &done) &&
            done == data.size() && model.windowTransfers == 2 && data.back() == 0x3a, "HVM window chunking failed");
        model.missingWindow = true;
        require(!backend.readMemory(process.native(), reinterpret_cast<void*>(0x10000), data.data(), data.size(), &done) &&
            done == 0 && model.readCalls == 2, "Missing HVM window silently used R0");
        model.missingWindow = false;
        CONTEXT first{}; first.ContextFlags = CONTEXT_DEBUG_REGISTERS; first.Dr0 = 0x9000; first.Dr7 = 1;
        require(backend.setContext(GetCurrentThread(), &first) != FALSE, "Initial EPT stop failed");
        require(model.stops.size() == 1 && model.resident && model.native.Dr0 == 0 && model.native.Dr7 == 1,
            "EPT stop did not neutralize hardware addresses or start residency");
        CONTEXT replacement = first; replacement.Dr0 = 0xa000; replacement.Dr1 = 0xb000; replacement.Dr7 = 5;
        model.failAddAfter = 1;
        require(backend.setContext(GetCurrentThread(), &replacement) == FALSE, "Injected second-stop failure was accepted");
        require(model.stops.size() == 1 && model.stops.begin()->second.address == 0x9000 && model.resident,
            "Installation failure did not restore the original EPT stop");
        CONTEXT read{}; read.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        require(backend.getContext(GetCurrentThread(), &read) && read.Dr0 == 0x9000 && read.Dr7 == 1,
            "Frontend register shadow changed after rollback");
        model.failStart = true;
        require(backend.setContext(GetCurrentThread(), &replacement) == FALSE,
            "Nonnegative NTSTATUS masked a rejected start operation");
        require(model.stops.size() == 1 && model.stops.begin()->second.address == 0x9000 && model.resident,
            "Start failure did not restore the original transaction");
        require(backend.setUseHvm(false) == ERROR_BUSY, "Mode changed while EPT stops remained");
        CONTEXT cleared{}; cleared.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        require(backend.setContext(GetCurrentThread(), &cleared) && model.stops.empty(), "Stop removal failed");
        require(backend.setUseHvm(false) == ERROR_SUCCESS && !model.prepared && !model.resident,
            "HVM disable left a preparation owned by the debugger");
        require(backend.setUseHvm(true) == ERROR_SUCCESS, "HVM re-enable failed");
        model.prepared = true;
        require(backend.setContext(GetCurrentThread(), &first) == FALSE && GetLastError() == ERROR_BUSY,
            "Existing external preparation was seized");
        model.prepared = false;

        require(backend.setContext(GetCurrentThread(), &first), "Detach test stop installation failed");
        ksword::debugger::BackendTestPeer::observeDetach(backend);
        const auto detached = backend.status();
        require(detached.attachedProcessId == 0 && !ksword::debugger::BackendTestPeer::hasStops(backend) &&
            model.stops.empty() && !model.prepared && !model.resident,
            "Frontend detach left EPT stops or owned residency behind");

        KSWORD_DEBUGGER_BACKEND_STATUS status{};
        KSWORD_DEBUGGER_CALL call{}; call.version = 1; call.size = sizeof(call); call.command = 1;
        call.outputBytes = sizeof(status); call.output = 1;
        require(KSwordDebuggerCall(&call) == ERROR_NOACCESS, "Invalid output pointer was accepted");
        require(KSwordDebuggerCall(reinterpret_cast<KSWORD_DEBUGGER_CALL*>(1)) == ERROR_NOACCESS, "Invalid call pointer was accepted");
        std::promise<DWORD> promise; auto future = promise.get_future();
        std::thread check([&] { call.output = reinterpret_cast<std::uintptr_t>(&status); promise.set_value(KSwordDebuggerCall(&call)); });
        require(future.wait_for(std::chrono::seconds(1)) == std::future_status::ready, "Invalid pointer stranded the backend mutex");
        check.join(); require(future.get() == ERROR_SUCCESS, "Backend did not recover after invalid pointer");
        require(backend.shutdown(), "Model shutdown failed");
        std::cout << "PASS: R0/HVM memory contracts, EPT scope/RF, rollback, ownership/detach, ABI pointer isolation\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
