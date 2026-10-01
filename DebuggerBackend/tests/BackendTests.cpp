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

    struct NativeWriterState
    {
        CONTEXT source{};
        std::array<unsigned char, 256> sourceXstate{}, actualXstate{};
        bool failNext = false;
        DWORD normalWrites = 0, rollbackWrites = 0;
    };
    BOOL WINAPI writeNativeDebug(HANDLE, const CONTEXT* debug, BOOL rollback, void* opaque)
    {
        auto& state = *static_cast<NativeWriterState*>(opaque);
        require(debug->ContextFlags == CONTEXT_DEBUG_REGISTERS,
            "Native writer received a full or XSTATE context through the fixed R0 seam");
        if (!rollback)
        {
            ++state.normalWrites;
            if (state.failNext) { state.failNext = false; SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
            // Model the adapter's Windows CopyContext/full-context write. The
            // backend supplies only DR changes, never an abbreviated full state.
            model.native = state.source; state.actualXstate = state.sourceXstate;
        }
        else ++state.rollbackWrites;
        model.native.Dr0 = debug->Dr0; model.native.Dr1 = debug->Dr1;
        model.native.Dr2 = debug->Dr2; model.native.Dr3 = debug->Dr3;
        model.native.Dr6 = debug->Dr6; model.native.Dr7 = debug->Dr7;
        return TRUE;
    }
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
        require(bytes.size() <= KSWORD_ARK_MEMORY_WRITE_MAX_BYTES &&
            flags == (KSWORD_ARK_MEMORY_WRITE_FLAG_UI_CONFIRMED | KSWORD_ARK_MEMORY_WRITE_FLAG_FORCE),
            "R0 write exceeded its limit or lost the driver's required confirmation/FORCE flags");
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
            require((request.contextFlags & CONTEXT_XSTATE) != CONTEXT_XSTATE,
                "Extended state was routed through the fixed 1232-byte R0 protocol");
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
        static void attach(Backend& value)
        {
            value.attachedPid_ = GetCurrentProcessId();
            value.attachedIdentity_.reset(OpenProcess(PROCESS_QUERY_INFORMATION | SYNCHRONIZE,
                FALSE, GetCurrentProcessId()));
            value.attachmentOwner_ = Backend::AttachmentOwner::backend;
            ++value.sessionGeneration_;
        }
        static void observeDetach(Backend& value) { value.monitorAttachment_ = true; }
        static void retainModelNativeSession(Backend& value) { value.monitorAttachment_ = false; }
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

        std::uint64_t nativeGeneration = 0;
        require(backend.observeNativeSession(0, &nativeGeneration) == ERROR_INVALID_PARAMETER && nativeGeneration == 0,
            "Native adoption accepted an invalid target");
        require(backend.observeNativeSession(GetCurrentProcessId(), &nativeGeneration) == ERROR_SUCCESS && nativeGeneration != 0,
            "Native adoption failed to retain the real target identity");
        // This test process deliberately has no debug port. Suppress only the
        // real Windows detach observer while exercising the borrowed model.
        ksword::debugger::BackendTestPeer::retainModelNativeSession(backend);
        CONTEXT initialNative{}; initialNative.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        initialNative.Dr6 = 0x4000;
        require(backend.overlayNativeDebugContext(GetCurrentThread(), &initialNative, nativeGeneration) &&
            initialNative.Dr0 == 0 && initialNative.Dr6 == 0x4000,
            "Native first context read failed before any EPT shadow existed");
        std::uint64_t repeatGeneration = 0;
        require(backend.observeNativeSession(GetCurrentProcessId(), &repeatGeneration) == ERROR_SUCCESS &&
            repeatGeneration == nativeGeneration, "Repeated adoption replaced a live native session");
        require(backend.observeNativeSession(GetCurrentProcessId() + 1) == ERROR_BUSY &&
            !backend.attach(GetCurrentProcessId()) && GetLastError() == ERROR_BUSY,
            "Native adoption allowed a competing target or backend attach");
        DEBUG_EVENT nativeEvent{}; nativeEvent.dwDebugEventCode = EXCEPTION_DEBUG_EVENT;
        nativeEvent.dwProcessId = GetCurrentProcessId(); nativeEvent.dwThreadId = GetCurrentThreadId();
        nativeEvent.u.Exception.ExceptionRecord.ExceptionCode = EXCEPTION_SINGLE_STEP;
        require(backend.observeNativeEvent(nativeEvent, nativeGeneration) == ERROR_SUCCESS &&
            backend.validateNativeContinue(nativeEvent.dwProcessId, nativeEvent.dwThreadId, nativeGeneration) == ERROR_SUCCESS,
            "Native event observation changed or rejected the frontend-owned transport");
        DEBUG_EVENT ignored{};
        require(!backend.waitEvent(&ignored, 0) && GetLastError() == ERROR_BUSY &&
            !backend.continueEvent(nativeEvent.dwProcessId, nativeEvent.dwThreadId, DBG_CONTINUE) && GetLastError() == ERROR_BUSY,
            "Backend attempted to take over the borrowed Windows debug transport");

        NativeWriterState nativeWriter{};
        nativeWriter.source.ContextFlags = CONTEXT_ALL | CONTEXT_XSTATE;
        nativeWriter.source.Rip = 0x1234567890ULL;
        nativeWriter.source.Rax = 0xabcdef;
        nativeWriter.source.FltSave.XmmRegisters[0].Low = 0xfeedfaceULL;
        nativeWriter.source.Dr0 = 0x9000; nativeWriter.source.Dr6 = 0x4001; nativeWriter.source.Dr7 = 1;
        nativeWriter.sourceXstate.fill(0xa7);
        require(backend.applyNativeDebugContext(GetCurrentThread(), &nativeWriter.source, writeNativeDebug, &nativeWriter),
            "Native EPT context application failed");
        require(model.native.Rip == nativeWriter.source.Rip && model.native.Rax == nativeWriter.source.Rax &&
            model.native.FltSave.XmmRegisters[0].Low == 0xfeedfaceULL &&
            nativeWriter.actualXstate == nativeWriter.sourceXstate && model.native.Dr0 == 0 && model.native.Dr7 == 1,
            "EPT context seam discarded native GPR, SIMD, or extended state");
        CONTEXT nativeRead = model.native;
        require(backend.overlayNativeDebugContext(GetCurrentThread(), &nativeRead) &&
            nativeRead.Dr0 == 0x9000 && nativeRead.Dr6 == 0x4001 && nativeRead.Rip == nativeWriter.source.Rip &&
            nativeRead.FltSave.XmmRegisters[0].Low == 0xfeedfaceULL,
            "Native overlay changed Windows-owned hit bits, GPR, or SIMD state");
        model.resident = false;
        require(backend.validateNativeContinue(nativeEvent.dwProcessId, nativeEvent.dwThreadId, nativeGeneration) == ERROR_INVALID_STATE,
            "Native continue accepted incomplete owned EPT residency");
        model.resident = true;
        nativeWriter.source.Dr0 = 0xa000; nativeWriter.source.Rip = 0x2222222222ULL; nativeWriter.failNext = true;
        require(!backend.applyNativeDebugContext(GetCurrentThread(), &nativeWriter.source, writeNativeDebug, &nativeWriter) &&
            GetLastError() == ERROR_ACCESS_DENIED && nativeWriter.rollbackWrites == 1 && model.resident &&
            model.stops.size() == 1 && model.stops.begin()->second.address == 0x9000 &&
            model.native.Rip == 0x1234567890ULL && nativeWriter.actualXstate == nativeWriter.sourceXstate,
            "Failed Windows full-context write did not roll back EPT without replaying GPR writes");
        std::promise<DWORD> workerTidPromise; auto workerTid = workerTidPromise.get_future();
        std::promise<void> releaseWorker; auto workerReleased = releaseWorker.get_future();
        std::thread worker([&] { workerTidPromise.set_value(GetCurrentThreadId()); workerReleased.wait(); });
        const DWORD secondTid = workerTid.get();
        ksword::ark::DriverHandle secondThread(OpenThread(THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, secondTid));
        NativeWriterState secondWriter = nativeWriter; secondWriter.source.Dr0 = 0xb000;
        require(secondThread.isValid() && backend.applyNativeDebugContext(secondThread.native(), &secondWriter.source,
            writeNativeDebug, &secondWriter) && model.stops.size() == 2,
            "Second native thread stop could not be installed");
        require(backend.retireNativeThread(secondTid, nativeGeneration) == ERROR_SUCCESS && model.stops.size() == 1 &&
            model.stops.begin()->second.threadId == GetCurrentThreadId() && model.resident,
            "Native thread retirement removed another thread's stop or lost residency");
        releaseWorker.set_value(); worker.join();
        nativeEvent.dwDebugEventCode = EXIT_PROCESS_DEBUG_EVENT;
        require(backend.observeNativeEvent(nativeEvent, nativeGeneration) == ERROR_SUCCESS,
            "Native exit event observation failed");
        ksword::debugger::BackendTestPeer::observeDetach(backend);
        require(backend.status().attachedProcessId == GetCurrentProcessId() && !model.stops.empty() &&
            backend.validateNativeContinue(nativeEvent.dwProcessId, nativeEvent.dwThreadId, nativeGeneration) == ERROR_SUCCESS,
            "A status poll retired the lease while the native frontend still held its exit event");
        require(backend.releaseNativeSession(nativeGeneration) == ERROR_SUCCESS && model.stops.empty() &&
            !model.prepared && !model.resident, "Native session release left owned EPT resources");
        require(backend.observeNativeSession(GetCurrentProcessId(), &repeatGeneration) == ERROR_SUCCESS &&
            repeatGeneration != nativeGeneration, "Native session replacement reused its generation");
        ksword::debugger::BackendTestPeer::retainModelNativeSession(backend);
        require(backend.releaseNativeSession(nativeGeneration) == ERROR_INVALID_STATE &&
            backend.retireNativeThread(GetCurrentThreadId(), nativeGeneration) == ERROR_INVALID_STATE &&
            backend.observeNativeEvent(nativeEvent, nativeGeneration) == ERROR_INVALID_STATE &&
            backend.status().attachedProcessId == GetCurrentProcessId(),
            "A delayed callback retired or rewrote a newer native session");
        require(!backend.applyNativeDebugContext(GetCurrentThread(), &nativeWriter.source, writeNativeDebug,
            &nativeWriter, nativeGeneration) && GetLastError() == ERROR_INVALID_STATE &&
            !backend.overlayNativeDebugContext(GetCurrentThread(), &nativeRead, nativeGeneration) &&
            GetLastError() == ERROR_INVALID_STATE,
            "A delayed context callback changed a newer native session");
        model.prepared = true; model.resident = true;
        require(backend.releaseNativeSession(repeatGeneration) == ERROR_SUCCESS && model.prepared && model.resident,
            "Native release seized external HVM residency without owned stops");
        model.prepared = false; model.resident = false;

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
        std::cout << "PASS: R0/HVM memory, EPT rollback, native ownership/generation, context preservation, ABI pointer isolation\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
