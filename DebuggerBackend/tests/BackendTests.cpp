#include "../KswordDebuggerBackend.h"

#include <cstring>
#include <future>
#include <fstream>
#include <filesystem>
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
        bool executableMemory = true, failShadowWrite = false, invalidShadowView = false, quarantined = false;
        bool workingSetValid = true, workingSetShared = false, currentPfnMatches = true;
        DWORD memoryType = MEM_PRIVATE;
        int failShadowWriteAfter = -1;
        int failShadowAddAfter = -1;
        DWORD controlCalls = 0, contextReads = 0, contextWrites = 0, shadowMutations = 0;
        bool failContextRead = false;
        bool eptSupported = true;
        struct ShadowPage
        {
            std::bitset<4096> patches, stops;
            std::array<unsigned char, 4096> bytes{};
        };
        std::map<std::uint64_t, ShadowPage> shadowPages;
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
        ++model.controlCalls;
        HvmControlResult result{}; result.io = success(sizeof(result.response));
        if (command == KSWORD_ARK_HVM_CONTROL_PREPARE) model.prepared = true;
        if (command == KSWORD_ARK_HVM_CONTROL_START_RESIDENT)
        {
            if (model.quarantined)
            { result.response.status = KSWORD_ARK_HVM_CONTROL_STATUS_LIFECYCLE_GUARD_FAILED; result.response.lastStatus = static_cast<LONG>(0xC0000184UL); return result; }
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
    VirtualMemoryQueryResult DriverClient::queryVirtualMemory(std::uint32_t, std::uint64_t address, unsigned long, DriverHandle*) const
    {
        VirtualMemoryQueryResult result{}; result.io = success();
        result.queryStatus = KSWORD_ARK_MEMORY_QUERY_STATUS_OK; result.fieldFlags = KSWORD_ARK_MEMORY_FIELD_BASIC_PRESENT;
        result.baseAddress = address & ~0xfffULL; result.regionSize = 4096; result.state = MEM_COMMIT;
        result.protect = model.executableMemory ? PAGE_EXECUTE_READ : PAGE_READWRITE;
        result.type = model.memoryType;
        return result;
    }
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
            response = {}; response.version = KSWORD_ARK_HVM_DEBUG_VERSION; response.size = sizeof(response); response.supported = model.eptSupported ? 1U : 0U;
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
            response.capabilities = KSWORD_ARK_DEBUGGER_CAP_SHADOW_INT3 | KSWORD_ARK_DEBUGGER_CAP_SHADOW_WRITES;
            response.reserved1 = model.invalidShadowView || model.quarantined ? KSWORD_ARK_DEBUGGER_SHADOW_INVALID_VIEW : 0;
            require((request.contextFlags & CONTEXT_XSTATE) != CONTEXT_XSTATE,
                "Extended state was routed through the fixed 1232-byte R0 protocol");
            if (request.operation == KSWORD_ARK_DEBUGGER_GET_CONTEXT)
            {
                ++model.contextReads;
                if (model.failContextRead) { response.status = static_cast<LONG>(0xC0000001UL); return success(outputBytes); }
                CONTEXT native = model.native; native.ContextFlags = request.contextFlags;
                std::memcpy(response.context, &native, sizeof(native));
            }
            if (request.operation == KSWORD_ARK_DEBUGGER_SET_CONTEXT) { ++model.contextWrites; std::memcpy(&model.native, request.context, sizeof(model.native)); }
            if (request.operation >= KSWORD_ARK_DEBUGGER_SHADOW_ADD)
            {
                ++model.shadowMutations;
                require(!model.resident, "Shadow mutation occurred while HVM was resident");
                if (request.operation == KSWORD_ARK_DEBUGGER_SHADOW_QUARANTINE)
                { model.quarantined = true; return success(outputBytes); }
                if (request.operation == KSWORD_ARK_DEBUGGER_SHADOW_RESTORE && request.address == 0 && request.bytes == 0)
                {
                    for (auto page = model.shadowPages.begin(); page != model.shadowPages.end();)
                    {
                        page->second.patches.reset();
                        if (page->second.stops.none()) page = model.shadowPages.erase(page); else ++page;
                    }
                    model.quarantined = false; return success(outputBytes);
                }
                if (request.operation == KSWORD_ARK_DEBUGGER_SHADOW_ADD || request.operation == KSWORD_ARK_DEBUGGER_SHADOW_WRITE)
                {
                    // Model the driver's post-pin eligibility proof. Allocation
                    // type stays IMAGE/MAPPED after COW; Shared, not type, is decisive.
                    if ((model.memoryType != MEM_PRIVATE && model.memoryType != MEM_IMAGE && model.memoryType != MEM_MAPPED) ||
                        !model.workingSetValid || model.workingSetShared)
                    {
                        response.reserved1 = model.workingSetShared ? KSWORD_ARK_DEBUGGER_SHADOW_BACKING_SHARED :
                            KSWORD_ARK_DEBUGGER_SHADOW_BACKING_UNVERIFIABLE;
                        response.status = static_cast<LONG>(0xC00000BBUL); return success(outputBytes);
                    }
                    if (!model.currentPfnMatches)
                    {
                        response.reserved1 = KSWORD_ARK_DEBUGGER_SHADOW_MAPPING_CHANGED;
                        response.status = static_cast<LONG>(0xC0000141UL); return success(outputBytes);
                    }
                }
                if (request.operation == KSWORD_ARK_DEBUGGER_SHADOW_WRITE && model.failShadowWrite)
                { response.status = static_cast<LONG>(0xC00000BBUL); return success(outputBytes); }
                if (request.operation == KSWORD_ARK_DEBUGGER_SHADOW_ADD && model.failShadowAddAfter >= 0 && model.failShadowAddAfter-- == 0)
                { model.failShadowAddAfter = -1; response.status = static_cast<LONG>(0xC0000017UL); return success(outputBytes); }
                if (request.operation == KSWORD_ARK_DEBUGGER_SHADOW_WRITE && model.failShadowWriteAfter >= 0)
                {
                    if (model.failShadowWriteAfter-- == 0)
                    { model.failShadowWriteAfter = -1; response.status = static_cast<LONG>(0xC00000BBUL); return success(outputBytes); }
                }
                const auto base = request.address & ~0xfffULL;
                const auto offset = static_cast<SIZE_T>(request.address & 0xfffULL);
                require(request.bytes != 0 && request.bytes <= 4096 - offset,
                    "Shadow mutation exceeded a single page");
                if (request.operation == KSWORD_ARK_DEBUGGER_SHADOW_WRITE)
                    require(request.bytes <= KSWORD_ARK_DEBUGGER_CONTEXT_BYTES, "Shadow write exceeded payload capacity");
                auto& page = model.shadowPages[base];
                for (SIZE_T index = 0; index < request.bytes; ++index)
                {
                    if (request.operation == KSWORD_ARK_DEBUGGER_SHADOW_WRITE)
                    { page.patches.set(offset + index); page.bytes[offset + index] = request.context[index]; }
                    if (request.operation == KSWORD_ARK_DEBUGGER_SHADOW_RESTORE) page.patches.reset(offset + index);
                    if (request.operation == KSWORD_ARK_DEBUGGER_SHADOW_ADD) page.stops.set(offset + index);
                    if (request.operation == KSWORD_ARK_DEBUGGER_SHADOW_REMOVE) page.stops.reset(offset + index);
                }
                if (page.patches.none() && page.stops.none()) model.shadowPages.erase(base);
                response.bytes = request.bytes;
            }
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
        static BOOL nativeContext(Backend& value, HANDLE thread, CONTEXT& context)
        { return value.nativeContext(thread, &context, false); }
        static void ownNormalPreparation(Backend& value, bool owned) { value.ownsResident_ = owned; value.shadowPrepared_ = false; }
        static void nativeOwner(Backend& value) { value.attachmentOwner_ = Backend::AttachmentOwner::native; }
    };
}

namespace
{
    void contextPreflightTests()
    {
        using ksword::debugger::Backend;
        using ksword::debugger::BackendTestPeer;
        struct Capture
        {
            wchar_t previous[4096]{};
            std::filesystem::path path;
            Capture()
            {
                (void)GetEnvironmentVariableW(L"KSWORD_DEBUGGER_LOG_FILE", previous, _countof(previous));
                wchar_t executable[32768]{};
                require(GetModuleFileNameW(nullptr, executable, _countof(executable)) != 0, "Cannot locate in-repository test artifact directory");
                path = std::filesystem::path(executable).parent_path() / L"BackendContextPreflight.log";
                std::ofstream(path, std::ios::trunc).close();
                require(SetEnvironmentVariableW(L"KSWORD_DEBUGGER_LOG_FILE", path.c_str()) != FALSE, "Cannot configure context diagnostic capture");
            }
            ~Capture() { (void)SetEnvironmentVariableW(L"KSWORD_DEBUGGER_LOG_FILE", previous[0] == 0 ? nullptr : previous); }
            std::string read() const { std::ifstream stream(path); return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()}; }
        } capture;
        const auto occurrences = [](const std::string& text, const std::string& needle) {
            SIZE_T count = 0, offset = 0;
            while ((offset = text.find(needle, offset)) != std::string::npos) { ++count; offset += needle.size(); }
            return count;
        };
        const auto configure = [](Backend& value) {
            require(value.initialize(), "Context preflight backend initialization failed");
            auto options = value.options(); options.mode = KSWORD_DEBUGGER_MODE_STEALTH;
            options.shadowMemoryWrites = 1; options.allowFallback = 0;
            require(value.setOptions(options) == ERROR_SUCCESS && value.setUseHvm(true) == ERROR_SUCCESS, "Context preflight policy setup failed");
            BackendTestPeer::attach(value);
        };
        CONTEXT arm{}; arm.ContextFlags = CONTEXT_DEBUG_REGISTERS; arm.Dr0 = 0x6100; arm.Dr7 = 1;
        for (const bool resident : {false, true})
        {
            model = Model{}; model.prepared = true; model.resident = resident;
            Backend value; configure(value);
            for (int attempt = 0; attempt < 12; ++attempt)
                require(!value.setContext(GetCurrentThread(), &arm) && GetLastError() == ERROR_BUSY,
                    "Foreign prepared/resident ownership was seized or lost its actual busy error");
            require(model.controlCalls == 0 && model.contextReads == 0 && model.contextWrites == 0 && model.shadowMutations == 0 &&
                model.prepared && model.resident == resident && !BackendTestPeer::hasStops(value),
                "Preparation rejection mutated foreign HVM, native context, or breakpoint state");
            CONTEXT clear{}; clear.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            require(value.setContext(GetCurrentThread(), &clear) && model.contextWrites == 1 && model.controlCalls == 0,
                "Unbound DR clear incorrectly entered Shadow preparation or paused foreign residency");
            require(value.shutdown() && model.controlCalls == 0, "Foreign preparation was stopped during empty adapter shutdown");
        }
        require(capture.read().find("rollback") == std::string::npos && capture.read().find("owning UI") != std::string::npos,
            "Pre-mutation busy rejection reported rollback or omitted the actionable foreign-owner reason");
        model = Model{}; model.prepared = true; model.resident = true;
        {
            Backend value; configure(value); BackendTestPeer::ownNormalPreparation(value, true);
            require(!value.setContext(GetCurrentThread(), &arm) && GetLastError() == ERROR_BUSY && model.controlCalls == 0 && model.contextWrites == 0,
                "Adapter normal-EPT preparation was silently changed into Shadow residency");
            BackendTestPeer::ownNormalPreparation(value, false);
            require(value.shutdown(), "Normal-preparation model cleanup failed");
        }
        require(capture.read().find("normal EPT preparation") != std::string::npos, "Own normal-EPT preparation was misreported as foreign residency");

        // Normal data-only fallback uses visible DRs while another caller's
        // resident remains untouched, including replacement, clear and detach.
        model = Model{}; model.prepared = true; model.resident = true; model.eptSupported = false;
        {
            Backend value; require(value.initialize() && value.setUseHvm(true) == ERROR_SUCCESS, "Native data watchpoint setup failed");
            BackendTestPeer::attach(value);
            CONTEXT data = arm; data.Dr7 = 1 | (1ULL << 16);
            require(value.setContext(GetCurrentThread(), &data) && model.native.Dr0 == data.Dr0 && model.native.Dr7 == data.Dr7,
                "Data-only fallback did not preserve its explicitly allowed visible native DR");
            auto actual = value.policyStatus(); auto stealth = value.options(); stealth.mode = KSWORD_DEBUGGER_MODE_STEALTH;
            require(actual.activePath == KSWORD_DEBUGGER_PATH_NATIVE && actual.activeBreakpoints == 1 && actual.canChangeOptions == 0 &&
                value.setOptions(stealth) == ERROR_BUSY && model.controlCalls == 0 && model.shadowMutations == 0,
                "Native data binding lost its actual path/count/policy guard or changed foreign HVM");
            data.Dr0 = 0x7200;
            require(value.setContext(GetCurrentThread(), &data) && model.native.Dr0 == data.Dr0 && model.controlCalls == 0,
                "Native data replacement incorrectly paused foreign residency");
            CONTEXT clear{}; clear.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            require(value.setContext(GetCurrentThread(), &clear) && model.native.Dr7 == 0 && value.policyStatus().canChangeOptions == 1 &&
                model.controlCalls == 0 && model.shadowMutations == 0 && model.prepared && model.resident,
                "Native data clear stranded the policy guard or changed foreign HVM");
            require(value.setContext(GetCurrentThread(), &data), "Native data continuation setup failed");
            BackendTestPeer::nativeOwner(value);
            DEBUG_EVENT event{}; event.dwDebugEventCode = EXCEPTION_DEBUG_EVENT;
            event.dwProcessId = GetCurrentProcessId(); event.dwThreadId = GetCurrentThreadId();
            event.u.Exception.ExceptionRecord.ExceptionCode = EXCEPTION_SINGLE_STEP;
            require(value.observeNativeEvent(event) == ERROR_SUCCESS && value.validateNativeContinue(event.dwProcessId, event.dwThreadId) == ERROR_SUCCESS,
                "Visible data-only metadata incorrectly required owned HVM for native continuation");
            require(value.retireNativeThread(GetCurrentThreadId()) == ERROR_SUCCESS && value.policyStatus().canChangeOptions == 1 &&
                model.controlCalls == 0 && model.shadowMutations == 0 && model.prepared && model.resident,
                "Native data thread retirement changed foreign HVM or stranded the options guard");
            require(value.setContext(GetCurrentThread(), &data) && value.releaseNativeSession() == ERROR_SUCCESS &&
                value.policyStatus().canChangeOptions == 1 && value.shutdown() && model.controlCalls == 0 && model.shadowMutations == 0 && model.prepared && model.resident,
                "Native data detach/session retirement seized or stopped foreign HVM");
        }
        for (const bool stealthMode : {false, true})
        {
            model = Model{}; model.prepared = true; model.resident = true; model.eptSupported = false;
            Backend value; require(value.initialize() && value.setUseHvm(true) == ERROR_SUCCESS, "Data rejection setup failed");
            BackendTestPeer::attach(value); auto options = value.options(); options.mode = stealthMode ? KSWORD_DEBUGGER_MODE_STEALTH : KSWORD_DEBUGGER_MODE_NORMAL;
            options.allowFallback = stealthMode ? 1U : 0U;
            require(value.setOptions(options) == ERROR_SUCCESS, "Data rejection policy setup failed");
            CONTEXT data = arm; data.Dr7 = 1 | (1ULL << 16);
            require(!value.setContext(GetCurrentThread(), &data) && GetLastError() == ERROR_NOT_SUPPORTED && model.contextWrites == 0 && model.controlCalls == 0 && model.shadowMutations == 0,
                "Stealth or fallback-disabled policy silently armed a visible native data DR");
            require(value.shutdown() && model.prepared && model.resident, "Data rejection retirement changed foreign HVM");
        }
        require(capture.read().find("visible native hardware debug register") != std::string::npos,
            "Native data fallback did not log its visible register destination/result");

        // A failure after the first new hidden INT3 is a real mutation and must
        // still compensate to the previous binding, unlike preparation failure.
        model = Model{};
        {
            Backend value; configure(value);
            require(value.setContext(GetCurrentThread(), &arm), "Shadow rollback baseline install failed");
            CONTEXT replacement = arm; replacement.Dr0 = 0x7100; replacement.Dr1 = 0x8200; replacement.Dr7 = 5;
            model.failShadowAddAfter = 1;
            require(!value.setContext(GetCurrentThread(), &replacement) && GetLastError() == ERROR_NOT_ENOUGH_MEMORY,
                "Injected partial Shadow binding failure was accepted or lost its native error");
            CONTEXT actual{}; actual.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            require(value.getContext(GetCurrentThread(), &actual) && actual.Dr0 == arm.Dr0 && actual.Dr7 == arm.Dr7 &&
                model.shadowPages.size() == 1 && model.shadowPages.count(0x6000) == 1 && model.shadowPages.at(0x6000).stops.test(0x100),
                "Partial Shadow mutation did not restore the previous logical/physical hidden breakpoint");
            require(value.shutdown(), "Shadow rollback model cleanup failed");
        }
        require(capture.read().find("ShadowPage context transaction failed: 8; rollback 0") != std::string::npos,
            "Real partial mutation did not report its completed compensating rollback");

        model = Model{}; model.failContextRead = true;
        {
            Backend value; require(value.initialize(), "Fallback count test initialization failed");
            ksword::ark::DriverHandle thread(CreateThread(nullptr, 0, [](LPVOID) -> DWORD { return 0; }, nullptr, CREATE_SUSPENDED, nullptr));
            require(thread.isValid(), "Cannot create the test-owned suspended context thread");
            for (int attempt = 0; attempt < 12; ++attempt)
            {
                CONTEXT context{}; context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                require(BackendTestPeer::nativeContext(value, thread.native(), context), "R0 error31 did not fall back to the real Windows context API");
                SetLastError(ERROR_ACCESS_DENIED); value.logRepeated("interleaved context preflight diagnostic");
                require(GetLastError() == ERROR_ACCESS_DENIED, "Repeated-warning logging changed caller LastError");
            }
            const auto policy = value.policyStatus();
            require(model.contextReads == 1 && policy.fallbackCount == 12 && policy.lastFallbackError == ERROR_GEN_FAILURE,
                "Cached fallback logging lost occurrence counts or the real R0 source error31");
            SetLastError(ERROR_INVALID_HANDLE); value.recordFallback("R0 thread context", "Windows thread context", ERROR_NOT_SUPPORTED, "changed native failure reason");
            require(GetLastError() == ERROR_INVALID_HANDLE && value.policyStatus().fallbackCount == 13 && value.policyStatus().lastFallbackError == ERROR_NOT_SUPPORTED,
                "Changed fallback diagnostic lost actual errors/counts or caller LastError");
            require(ResumeThread(thread.native()) != MAXDWORD && WaitForSingleObject(thread.native(), 1000) == WAIT_OBJECT_0,
                "Test-owned context thread did not finish cleanly");
            require(value.shutdown(), "Fallback count model shutdown failed");
        }
        const auto logs = capture.read();
        require(occurrences(logs, "Fallback R0 thread context") == 4 && occurrences(logs, "interleaved context preflight diagnostic") == 2 &&
            logs.find("occurrences=10") != std::string::npos && logs.find("changed native failure reason") != std::string::npos,
            "Identical transient logs were not aggregated independently or changed reasons were suppressed");
        std::cout << "PASS: context pre-mutation ownership rejection/no rollback, harmless clear, real Shadow rollback, error31 fallback counts and aggregated diagnostics\n";
    }
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

        const auto legacyOptions = backend.options();
        require(legacyOptions.mode == KSWORD_DEBUGGER_MODE_NORMAL && legacyOptions.shadowMemoryWrites == 0 &&
            legacyOptions.allowFallback == 1 && legacyOptions.logFallback == 1 && legacyOptions.maxShadowPages == 32,
            "Legacy adapters changed their default memory behavior");
        auto shadowOptions = legacyOptions; shadowOptions.shadowMemoryWrites = 1; shadowOptions.maxShadowPages = 1;
        require(backend.setOptions(shadowOptions) == ERROR_SUCCESS, "Shadow options were rejected");
        auto invalidOptions = shadowOptions; invalidOptions.logFallback = 0;
        require(backend.setOptions(invalidOptions) == ERROR_INVALID_PARAMETER && backend.options().logFallback == 1,
            "Critical fallback logging could be disabled");
        invalidOptions = shadowOptions; invalidOptions.reserved[0] = 1;
        require(backend.setOptions(invalidOptions) == ERROR_INVALID_PARAMETER, "Unknown options fields were accepted");
        require(backend.setUseHvm(false) == ERROR_SUCCESS, "Shadow policy HVM-off setup failed");
        unsigned char codePatch[3] = {0x90, 0x91, 0x92};
        const auto ordinaryWritesBefore = model.writeSizes.size();
        require(!backend.writeMemory(process.native(), reinterpret_cast<void*>(0x10040), codePatch, 3, &done) &&
            GetLastError() == ERROR_INVALID_STATE && model.writeSizes.size() == ordinaryWritesBefore,
            "HVM-off Shadow code write silently modified original code");
        require(backend.setUseHvm(true) == ERROR_SUCCESS &&
            backend.writeMemory(process.native(), reinterpret_cast<void*>(0x10040), codePatch, 3, &done) && done == 3 &&
            backend.policyStatus().shadowWritePages == 1 && model.shadowPages[0x10000].bytes[0x41] == 0x91 &&
            model.writeSizes.size() == ordinaryWritesBefore && model.resident,
            "Execution-only Shadow code patch changed ordinary memory or failed to start");
        unsigned char changedPatch[3] = {0x20, 0x21, 0x22};
        model.failStart = true;
        require(!backend.writeMemory(process.native(), reinterpret_cast<void*>(0x10040), changedPatch, 3, &done) &&
            done == 0 && model.shadowPages[0x10000].bytes[0x41] == 0x91 && model.resident &&
            backend.policyStatus().shadowWritePages == 1,
            "Failed residency start did not restore previous Shadow patch bytes and mask");
        require(backend.restoreShadowWrites(0x10041, 1) == ERROR_SUCCESS,
            "Disjoint prior Shadow mask setup failed");
        model.failStart = true; model.failShadowWriteAfter = 2;
        require(!backend.writeMemory(process.native(), reinterpret_cast<void*>(0x10040), changedPatch, 3, &done) && !model.resident &&
            !backend.writeMemory(process.native(), reinterpret_cast<void*>(0x10040), codePatch, 3, &done) &&
            GetLastError() == ERROR_INVALID_STATE &&
            !backend.continueEvent(GetCurrentProcessId(), GetCurrentThreadId(), DBG_CONTINUE) && GetLastError() == ERROR_INVALID_STATE,
            "Failed second saved-mask replay allowed a later write/continue to resume incomplete code");
        const auto directStart = ksword::ark::DriverClient{}.controlHvm(KSWORD_ARK_HVM_CONTROL_START_RESIDENT,
            0, false, false, true);
        require(model.quarantined && directStart.response.status == KSWORD_ARK_HVM_CONTROL_STATUS_LIFECYCLE_GUARD_FAILED && !model.resident,
            "A direct driver start bypassed content-rollback quarantine");
        require(backend.restoreShadowWrites() == ERROR_SUCCESS &&
            backend.writeMemory(process.native(), reinterpret_cast<void*>(0x10040), codePatch, 3, &done),
            "Full Shadow restore did not clear incomplete-content rollback quarantine");
        std::vector<unsigned char> oversizedPatch(KSWORD_ARK_DEBUGGER_CONTEXT_BYTES + 1, 0x90);
        require(!backend.writeMemory(process.native(), reinterpret_cast<void*>(0x10080), oversizedPatch.data(), oversizedPatch.size(), &done) &&
            done == 0 && model.shadowPages[0x10000].patches.count() == 3,
            "Oversized Shadow edit left a partially applied first chunk");
        require(!backend.writeMemory(process.native(), reinterpret_cast<void*>(0x10fff), codePatch, 3, &done) &&
            done == 0 && model.shadowPages.size() == 1,
            "Cross-page Shadow edit applied half of the code patch");
        model.memoryType = MEM_IMAGE; model.workingSetShared = true;
        require(!backend.writeMemory(process.native(), reinterpret_cast<void*>(0x10040), changedPatch, 3, &done) &&
            GetLastError() == ERROR_NOT_SUPPORTED && done == 0 && model.shadowPages.size() == 1 &&
            model.shadowPages[0x10000].bytes[0x41] == 0x91,
            "Physical execution patch was applied to shared image backing");
        model.workingSetShared = false;
        require(backend.writeMemory(process.native(), reinterpret_cast<void*>(0x10040), changedPatch, 3, &done) &&
            done == 3 && model.memoryType == MEM_IMAGE && model.shadowPages[0x10000].bytes[0x41] == 0x21 &&
            model.writeSizes.size() == ordinaryWritesBefore,
            "Private COW image backing was rejected by allocation type or visibly written");
        model.memoryType = MEM_MAPPED;
        require(backend.writeMemory(process.native(), reinterpret_cast<void*>(0x10040), codePatch, 3, &done) &&
            done == 3 && model.shadowPages[0x10000].bytes[0x41] == 0x91 && model.writeSizes.size() == ordinaryWritesBefore,
            "Private COW mapped backing was rejected by allocation type or visibly written");
        model.workingSetValid = false;
        require(!backend.writeMemory(process.native(), reinterpret_cast<void*>(0x10040), changedPatch, 3, &done) &&
            GetLastError() == ERROR_NOT_SUPPORTED && done == 0 && model.shadowPages[0x10000].bytes[0x41] == 0x91,
            "Unverifiable image/mapped working-set backing was accepted");
        model.workingSetValid = true; model.currentPfnMatches = false;
        require(!backend.writeMemory(process.native(), reinterpret_cast<void*>(0x10040), changedPatch, 3, &done) &&
            done == 0 && model.shadowPages[0x10000].bytes[0x41] == 0x91,
            "A remapped private COW page reused a stale physical execution view");
        model.currentPfnMatches = true; model.workingSetShared = true; model.memoryType = MEM_PRIVATE;
        require(!backend.writeMemory(process.native(), reinterpret_cast<void*>(0x10040), changedPatch, 3, &done) &&
            GetLastError() == ERROR_NOT_SUPPORTED && done == 0 && model.shadowPages[0x10000].bytes[0x41] == 0x91,
            "A physically shared private allocation bypassed the working-set proof");
        model.workingSetShared = false;
        model.memoryType = MEM_PRIVATE;
        unsigned char original = 0;
        require(backend.readMemory(process.native(), reinterpret_cast<void*>(0x10041), &original, 1, &done) && original == 0x3a,
            "Public memory reads exposed execution-view patch bytes");
        require(backend.setOptions(legacyOptions) == ERROR_BUSY && backend.setOptions(shadowOptions) == ERROR_SUCCESS &&
            backend.policyStatus().canChangeOptions == 0 && backend.setUseHvm(false) == ERROR_BUSY,
            "Active Shadow patch did not protect policy/resident ownership");
        model.invalidShadowView = true;
        require(!backend.writeMemory(process.native(), reinterpret_cast<void*>(0x10040), changedPatch, 3, &done) &&
            GetLastError() == ERROR_INVALID_STATE && !model.resident,
            "A later successful Shadow mutation resumed residency with a missing owned view");
        model.invalidShadowView = false;
        require(backend.restoreShadowWrites() == ERROR_SUCCESS &&
            backend.writeMemory(process.native(), reinterpret_cast<void*>(0x10040), codePatch, 3, &done) && model.resident,
            "Repaired Shadow views could not resume");
        require(!backend.writeMemory(process.native(), reinterpret_cast<void*>(0x11040), codePatch, 3, &done) &&
            GetLastError() == ERROR_NOT_ENOUGH_MEMORY && backend.policyStatus().shadowWritePages == 1,
            "Configured Shadow page budget was not enforced");
        ksword::debugger::BackendTestPeer::attach(backend);
        CONTEXT merged{}; merged.ContextFlags = CONTEXT_DEBUG_REGISTERS; merged.Dr0 = 0x10041; merged.Dr7 = 1;
        require(backend.setContext(GetCurrentThread(), &merged) && model.shadowPages.size() == 1 &&
            model.shadowPages[0x10000].patches.test(0x41) && model.shadowPages[0x10000].stops.test(0x41),
            "Same-page breakpoint did not merge with the execution patch within the page budget");
        require(backend.restoreShadowWrites() == ERROR_SUCCESS && backend.policyStatus().shadowWritePages == 0 &&
            model.shadowPages[0x10000].patches.none() && model.shadowPages[0x10000].stops.test(0x41) && model.resident,
            "Restoring Shadow memory patches removed a hidden breakpoint");
        require(backend.setContext(GetCurrentThread(), &cleared) && model.shadowPages.empty(), "Merged hidden breakpoint cleanup failed");
        shadowOptions.mode = KSWORD_DEBUGGER_MODE_STEALTH;
        require(backend.setOptions(shadowOptions) == ERROR_SUCCESS, "Stealth options failed");
        require(backend.setContext(GetCurrentThread(), &merged) &&
            backend.writeMemory(process.native(), reinterpret_cast<void*>(0x10040), codePatch, 3, &done) &&
            model.shadowPages[0x10000].patches.test(0x41) && model.shadowPages[0x10000].stops.test(0x41),
            "Code patch after an existing hidden breakpoint did not merge on the same page");
        require(backend.restoreShadowWrites() == ERROR_SUCCESS && backend.setContext(GetCurrentThread(), &cleared) &&
            model.shadowPages.empty(), "Reverse-order merged patch cleanup failed");
        model.executableMemory = false;
        const auto fallbackBefore = backend.policyStatus().fallbackCount;
        require(backend.writeMemory(process.native(), reinterpret_cast<void*>(0x10040), codePatch, 3, &done) && done == 3 &&
            backend.policyStatus().fallbackCount == fallbackBefore + 1 && backend.policyStatus().lastFallbackError == ERROR_NOT_SUPPORTED &&
            model.shadowPages.empty(), "Stealth data freeze did not perform and record its explicit ordinary fallback");
        shadowOptions.allowFallback = 0;
        require(backend.setOptions(shadowOptions) == ERROR_SUCCESS &&
            !backend.writeMemory(process.native(), reinterpret_cast<void*>(0x10040), codePatch, 3, &done) &&
            GetLastError() == ERROR_NOT_SUPPORTED && backend.policyStatus().fallbackCount == fallbackBefore + 1,
            "Strict data write silently fell back");
        model.executableMemory = true; model.failShadowWrite = true;
        shadowOptions.allowFallback = 1;
        require(backend.setOptions(shadowOptions) == ERROR_SUCCESS &&
            !backend.writeMemory(process.native(), reinterpret_cast<void*>(0x10040), codePatch, 3, &done) &&
            GetLastError() == ERROR_NOT_SUPPORTED && model.writeSizes.size() == ordinaryWritesBefore,
            "Unsupported Shadow code write became an original code write");
        model.failShadowWrite = false;
        require(backend.setOptions(legacyOptions) == ERROR_SUCCESS, "Policy cleanup failed");

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
        KSWORD_DEBUGGER_OPTIONS offlineOptions{};
        call.command = KSWORD_DEBUGGER_GET_OPTIONS; call.output = reinterpret_cast<std::uintptr_t>(&offlineOptions);
        call.outputBytes = sizeof(offlineOptions); call.inputBytes = 0;
        require(KSwordDebuggerCall(&call) == ERROR_SUCCESS && call.bytesReturned == sizeof(offlineOptions) &&
            offlineOptions.size == sizeof(offlineOptions), "Offline options persistence requires a connected driver");
        contextPreflightTests();
        std::cout << "PASS: R0/HVM memory, EPT rollback, native ownership/generation, context preservation, Shadow code/data policy, patch/INT3 restore, ABI pointer isolation\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
