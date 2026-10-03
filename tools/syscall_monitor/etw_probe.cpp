// Benign Windows ETW protocol probe. Calls only ordinary Windows exports in
// this process; it never emits executable syscall samples, injects code, opens
// another process, changes privileges, or stops a session it did not create.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <objbase.h>
#include <winternl.h>

#include "../../shared/evidence/SyscallCorrelation.h"
#include "../../shared/evidence/SyscallEvidence.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#ifndef EVENT_TRACE_FLAG_SYSTEMCALL
#define EVENT_TRACE_FLAG_SYSTEMCALL 0x00000080
#endif

namespace {
using namespace ks::evidence::syscall;
constexpr GUID kPerfInfoGuid =
    {0xce1dbfb4, 0x137e, 0x4da6, {0x87, 0xb0, 0x3f, 0x59, 0xaa, 0x10, 0x2c, 0xbc}};
constexpr GUID kStackWalkGuid =
    {0xdef2fe46, 0x7bd6, 0x4b80, {0xbd, 0x94, 0xf5, 0x7f, 0xe2, 0x0d, 0x0c, 0xe3}};
constexpr auto kUnknown = (std::numeric_limits<std::uint32_t>::max)();

std::uint64_t NowMs() noexcept
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct Row {
    std::uint64_t serviceAddress = 0;
    bool ownHeader = false;
};

struct Probe {
    const DWORD ownPid = ::GetCurrentProcessId();
    Correlator<Row> correlator;
    std::atomic_bool callbackFailed{false};
    std::uint64_t headerEntries = 0;
    std::uint64_t ownEntriesFromStack = 0;
    std::uint64_t ownStacks = 0;
    std::uint64_t matched = 0;
    std::uint64_t matchedUserStack = 0;
    std::uint64_t missing = 0;
    std::uint64_t ambiguous = 0;
    std::uint64_t capacity = 0;
    std::uint64_t malformedStacks = 0;
    std::uint64_t ownExits = 0;

    // Only the ProcessTrace thread touches the correlator and these counters.
    void Publish(std::vector<Correlator<Row>::Output> output)
    {
        for (auto& result : output) {
            if (result.pid != ownPid) { continue; }
            if (result.state == CorrelationState::Matched) {
                ++matched;
                if (!result.row.ownHeader) { ++ownEntriesFromStack; }
                for (const auto pc : result.frames) {
                    if (IsUserAddress(pc)) { ++matchedUserStack; break; }
                }
            } else if (result.state == CorrelationState::Ambiguous) {
                ++ambiguous;
            } else if (result.state == CorrelationState::CapacityEvicted) {
                ++capacity;
            } else {
                ++missing;
            }
        }
    }

    void Record(const EVENT_RECORD& event)
    {
        const auto now = NowMs();
        Publish(correlator.Expire(now));
        const auto width = (event.EventHeader.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER) ? 4U : 8U;
        // ProcessorIndex overlays ProcessorNumber + Alignment. The full index
        // is meaningful only when the header flag says the extended form is used.
        const std::uint16_t cpu = ::GetEventProcessorIndex(&event);
        const Key key{static_cast<std::uint64_t>(event.EventHeader.TimeStamp.QuadPart), cpu};
        const auto opcode = event.EventHeader.EventDescriptor.Opcode;
        if (::IsEqualGUID(event.EventHeader.ProviderId, kStackWalkGuid) && opcode == 32) {
            std::uint64_t timestamp = 0;
            std::uint32_t pid = 0, tid = 0;
            std::vector<std::uint64_t> frames;
            if (!ParseStackPayload(event.UserData, event.UserDataLength, width,
                    timestamp, pid, tid, frames)) {
                ++malformedStacks;
                return;
            }
            if (pid != ownPid) { return; }
            ++ownStacks;
            Publish(correlator.AddStack({timestamp, cpu}, pid, tid, std::move(frames), now));
            return;
        }
        if (!::IsEqualGUID(event.EventHeader.ProviderId, kPerfInfoGuid)
            || (opcode != 51 && opcode != 52) || !event.UserData
            || event.UserDataLength != (opcode == 51 ? width : sizeof(std::uint32_t))) {
            return;
        }
        const auto pid = event.EventHeader.ProcessId;
        const auto tid = event.EventHeader.ThreadId;
        // An unknown header cannot be guessed from a live thread. Retain only
        // bounded raw metadata until a stack payload identifies this process.
        // Known events of any other process are immediately discarded.
        if (pid != ownPid && pid != kUnknown) { return; }
        if (opcode == 52) {
            if (pid == ownPid) { ++ownExits; }
            return;
        }
        Row row;
        row.ownHeader = pid == ownPid;
        std::memcpy(&row.serviceAddress, event.UserData, width);
        if (row.ownHeader) { ++headerEntries; }
        Publish(correlator.AddEvent(key, pid, tid, row, now));
        if (pid == kUnknown || tid == kUnknown || !event.ExtendedData) { return; }
        for (USHORT i = 0; i < event.ExtendedDataCount; ++i) {
            const auto& item = event.ExtendedData[i];
            const std::size_t frameWidth = item.ExtType == EVENT_HEADER_EXT_TYPE_STACK_TRACE64 ? 8
                : item.ExtType == EVENT_HEADER_EXT_TYPE_STACK_TRACE32 ? 4 : 0;
            if (!frameWidth || !item.DataPtr || item.DataSize <= 8
                || (item.DataSize - 8) % frameWidth != 0
                || (item.DataSize - 8) / frameWidth > 192) { continue; }
            std::vector<std::uint64_t> frames;
            const auto* bytes = reinterpret_cast<const unsigned char*>(item.DataPtr);
            for (std::size_t offset = 8; offset < item.DataSize; offset += frameWidth) {
                std::uint64_t pc = 0;
                std::memcpy(&pc, bytes + offset, frameWidth);
                frames.push_back(pc);
            }
            ++ownStacks;
            Publish(correlator.AddStack(key, pid, tid, std::move(frames), now));
        }
    }
};

void WINAPI RecordCallback(EVENT_RECORD* event) noexcept
{
    if (!event || !event->UserContext) { return; }
    auto& probe = *static_cast<Probe*>(event->UserContext);
    if (probe.callbackFailed.load()) { return; }
    try { probe.Record(*event); }
    catch (...) { probe.callbackFailed.store(true); }
}

ULONG WINAPI BufferCallback(EVENT_TRACE_LOGFILEW* logfile) noexcept
{
    if (!logfile || !logfile->Context) { return FALSE; }
    auto& probe = *static_cast<Probe*>(logfile->Context);
    try { probe.Publish(probe.correlator.Expire(NowMs())); }
    catch (...) { probe.callbackFailed.store(true); }
    return probe.callbackFailed.load() ? FALSE : TRUE;
}

// Compile-time ABI checks exercise the actual Windows SDK callback typedefs.
static_assert(std::is_convertible_v<decltype(&RecordCallback), PEVENT_RECORD_CALLBACK>);
static_assert(std::is_convertible_v<decltype(&BufferCallback), PEVENT_TRACE_BUFFER_CALLBACKW>);

struct TraceProperties {
    EVENT_TRACE_PROPERTIES properties{};
    wchar_t loggerName[160]{};
};

struct OwnedSession {
    TRACEHANDLE handle = 0;
    TraceProperties data;
    ~OwnedSession()
    {
        if (handle) {
            // This handle was returned by this probe's successful StartTrace.
            // Never enumerate, recover, or stop a same-name/foreign session.
            ::ControlTraceW(handle, nullptr, &data.properties, EVENT_TRACE_CONTROL_STOP);
        }
    }
    ULONG Stop()
    {
        const auto owned = handle;
        if (!owned) { return ERROR_INVALID_HANDLE; }
        const auto status = ::ControlTraceW(owned, nullptr, &data.properties, EVENT_TRACE_CONTROL_STOP);
        if (status == ERROR_SUCCESS || status == ERROR_WMI_INSTANCE_NOT_FOUND) {
            handle = 0;
        }
        // Retain our own handle on failure so the destructor can retry cleanup.
        return status;
    }
};

int NotVerified(const char* step, ULONG status)
{
    std::cout << "SYSCALL_ETW_PROBE=CAPTURE_NOT_VERIFIED\nSTEP=" << step
        << "\nWIN32_STATUS=" << status << '\n';
    if (status == ERROR_ACCESS_DENIED || status == ERROR_PRIVILEGE_NOT_HELD) {
        std::cout << "REASON=ACCESS_DENIED\nNO_ELEVATION_ATTEMPTED=TRUE\n";
    }
    std::cout << "QT_UI_VERIFIED=FALSE\n";
    return 2;
}

int Run()
{
    if (sizeof(void*) != 8) { return NotVerified("ARCHITECTURE", ERROR_NOT_SUPPORTED); }
    EVENT_RECORD cpuLayout{};
    cpuLayout.BufferContext.ProcessorNumber = 17;
    cpuLayout.BufferContext.Alignment = 0xa5;
    if (::GetEventProcessorIndex(&cpuLayout) != 17) {
        return NotVerified("PROCESSOR_NUMBER_LAYOUT", ERROR_INVALID_DATA);
    }
    cpuLayout.EventHeader.Flags = EVENT_HEADER_FLAG_PROCESSOR_INDEX;
    cpuLayout.BufferContext.ProcessorIndex = 0x1234;
    if (::GetEventProcessorIndex(&cpuLayout) != 0x1234) {
        return NotVerified("PROCESSOR_INDEX_LAYOUT", ERROR_INVALID_DATA);
    }
    std::cout << "WINDOWS_CALLBACK_ABI_BUILD=PASS\nCPU_INDEX_LAYOUT_CHECK=PASS\n";
    Probe probe;
    OwnedSession session;
    GUID guid{};
    const HRESULT guidStatus = ::CoCreateGuid(&guid);
    if (FAILED(guidStatus)) { return NotVerified("CoCreateGuid", static_cast<ULONG>(guidStatus)); }
    wchar_t guidText[40]{};
    if (!::StringFromGUID2(guid, guidText, 40)) {
        return NotVerified("StringFromGUID2", ERROR_INVALID_DATA);
    }
    const std::wstring name = L"KswordSyscallProbe-" + std::to_wstring(probe.ownPid) + L"-" + guidText;
    auto& properties = session.data.properties;
    properties.Wnode.BufferSize = sizeof(session.data);
    properties.Wnode.Guid = guid;
    properties.Wnode.ClientContext = 1; // Raw QPC for event + stack matching.
    properties.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    properties.LoggerNameOffset = offsetof(TraceProperties, loggerName);
    properties.LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE;
    properties.EnableFlags = EVENT_TRACE_FLAG_SYSTEMCALL;
    properties.BufferSize = 64;
    properties.MinimumBuffers = 32;
    properties.MaximumBuffers = 128;
    properties.FlushTimer = 1;
    const ULONG startStatus = ::StartTraceW(&session.handle, name.c_str(), &properties);
    if (startStatus != ERROR_SUCCESS) {
        session.handle = 0;
        return NotVerified("StartTraceW", startStatus);
    }
    CLASSIC_EVENT_ID entry{};
    entry.EventGuid = kPerfInfoGuid;
    entry.Type = 51;
    const ULONG stackStatus = ::TraceSetInformation(session.handle, TraceStackTracingInfo,
        &entry, sizeof(entry));
    if (stackStatus != ERROR_SUCCESS) { return NotVerified("TraceSetInformation", stackStatus); }

    EVENT_TRACE_LOGFILEW logfile{};
    logfile.LoggerName = const_cast<wchar_t*>(name.c_str());
    logfile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD
        | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    logfile.EventRecordCallback = &RecordCallback;
    logfile.BufferCallback = &BufferCallback;
    logfile.Context = &probe;
    const TRACEHANDLE consumer = ::OpenTraceW(&logfile);
    if (consumer == INVALID_PROCESSTRACE_HANDLE) {
        const ULONG openStatus = ::GetLastError();
        return NotVerified("OpenTraceW", openStatus);
    }
    ULONG processStatus = ERROR_NOT_READY;
    std::thread processing;
    try {
        processing = std::thread([&]() {
            TRACEHANDLE ownedConsumer = consumer;
            processStatus = ::ProcessTrace(&ownedConsumer, 1, nullptr, nullptr);
            try { probe.Publish(probe.correlator.Expire(NowMs(), true)); }
            catch (...) { probe.callbackFailed.store(true); }
        });
    } catch (...) {
        ::CloseTrace(consumer);
        return NotVerified("CONSUMER_THREAD", ERROR_NOT_ENOUGH_MEMORY);
    }

    using QueryProcess = LONG (NTAPI*)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
    QueryProcess query = nullptr;
    const auto symbol = ::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
    static_assert(sizeof(symbol) == sizeof(query));
    std::memcpy(&query, &symbol, sizeof(query));
    std::uint64_t queryCalls = 0;
    std::uint64_t querySucceeded = 0;
    // Normal exported calls, all on this process's pseudo-handle. No inline
    // assembly, dynamically generated machine code, or direct syscall fixture.
    for (unsigned i = 0; i < 200 && !probe.callbackFailed.load(); ++i) {
        if (query) {
            PROCESS_BASIC_INFORMATION basic{};
            ULONG length = 0;
            const LONG result = query(::GetCurrentProcess(), ProcessBasicInformation,
                &basic, sizeof(basic), &length);
            ++queryCalls;
            if (result >= 0) { ++querySucceeded; }
        }
        FILETIME created{}, exited{}, kernel{}, user{};
        ::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user);
        ::Sleep(5);
    }
    ::Sleep(1200); // Allow real-time buffer delivery and bounded correlation expiry.
    const ULONG stopStatus = session.Stop();
    ::CloseTrace(consumer);
    processing.join();

    std::cout << "PROBE_PID=" << probe.ownPid
        << "\nEXPORTED_QUERY_CALLS=" << queryCalls
        << "\nEXPORTED_QUERY_SUCCEEDED=" << querySucceeded
        << "\nOWN_HEADER_ENTRIES=" << probe.headerEntries
        << "\nOWN_ENTRIES_FROM_STACK=" << probe.ownEntriesFromStack
        << "\nOWN_STACK_RECORDS=" << probe.ownStacks
        << "\nEXACT_MATCHED_ENTRIES=" << probe.matched
        << "\nEXACT_MATCHED_USER_STACKS=" << probe.matchedUserStack
        << "\nOWN_ENTRIES_WITHOUT_MATCH=" << probe.missing
        << "\nOWN_AMBIGUOUS_MATCHES=" << probe.ambiguous
        << "\nOWN_CAPACITY_EVICTIONS=" << probe.capacity
        << "\nMALFORMED_STACKS=" << probe.malformedStacks
        << "\nOWN_EXITS=" << probe.ownExits
        << "\nETW_EVENTS_LOST=" << properties.EventsLost
        << "\nETW_REALTIME_BUFFERS_LOST=" << properties.RealTimeBuffersLost
        << "\nPROCESS_TRACE_STATUS=" << processStatus
        << "\nSTOP_TRACE_STATUS=" << stopStatus
        << "\nCALLBACK_FAILED=" << (probe.callbackFailed.load() ? "TRUE" : "FALSE") << '\n';
    if (probe.callbackFailed.load() || stopStatus != ERROR_SUCCESS
        || (processStatus != ERROR_SUCCESS && processStatus != ERROR_CANCELLED)
        || !querySucceeded || !probe.matched || !probe.matchedUserStack) {
        return NotVerified("ETW_PROTOCOL_COUNTS", ERROR_INVALID_DATA);
    }
    std::cout << "SYSCALL_ETW_PROBE=PASS\nCAPTURE_VERIFIED=ETW_PROTOCOL_ONLY\nQT_UI_VERIFIED=FALSE\n";
    return 0;
}
} // namespace

int main()
{
    try { return Run(); }
    catch (...) { return NotVerified("PROBE_EXCEPTION", ERROR_NOT_ENOUGH_MEMORY); }
}
