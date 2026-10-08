// Runs the production collector with deterministic PDH/process endpoints.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Pdh.h>
#include <PdhMsg.h>
#include <array>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace fixture {
struct Entry { std::wstring name; LONGLONG value = 0; DWORD status = PDH_CSTATUS_VALID_DATA; bool badName = false, unterminated = false; };
std::array<std::vector<Entry>, 4> entries;
std::array<PDH_STATUS, 4> arrayStatus{};
unsigned phase = 0, added = 0;
bool failOpen = false, reused = false, delayAfter = false, delayed = false, oversized = false;
void reset() {
    entries = {}; arrayStatus = {}; phase = added = 0;
    failOpen = reused = delayAfter = delayed = oversized = false;
}
PDH_STATUS WINAPI open(LPCWSTR, DWORD_PTR, PDH_HQUERY* query) {
    if (failOpen) { return 5; }
    phase = added = 0;
    *query = reinterpret_cast<PDH_HQUERY>(1); return ERROR_SUCCESS;
}
PDH_STATUS WINAPI close(PDH_HQUERY) { return ERROR_SUCCESS; }
PDH_STATUS WINAPI add(PDH_HQUERY, LPCWSTR, DWORD_PTR, PDH_HCOUNTER* counter) {
    *counter = reinterpret_cast<PDH_HCOUNTER>(static_cast<std::uintptr_t>(++added)); return ERROR_SUCCESS;
}
PDH_STATUS WINAPI collect(PDH_HQUERY) { ++phase; return ERROR_SUCCESS; }
PDH_STATUS WINAPI array(PDH_HCOUNTER handle, DWORD, LPDWORD bytes, LPDWORD count, PPDH_FMT_COUNTERVALUE_ITEM_W output) {
    const auto index = reinterpret_cast<std::uintptr_t>(handle) - 1;
    if (arrayStatus[index]) { return arrayStatus[index]; }
    const auto& values = entries[index];
    std::size_t required = values.size() * sizeof(PDH_FMT_COUNTERVALUE_ITEM_W);
    for (const auto& value : values) { required += (value.name.size() + 1) * sizeof(wchar_t); }
    *count = static_cast<DWORD>(values.size());
    if (oversized && index == 0) { *bytes = 5 * 1024 * 1024; return PDH_MORE_DATA; }
    if (!output) { *bytes = static_cast<DWORD>(required); return required ? static_cast<PDH_STATUS>(PDH_MORE_DATA) : ERROR_SUCCESS; }
    assert(*bytes >= required);
    auto* names = reinterpret_cast<wchar_t*>(output + values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        output[i].FmtValue.CStatus = values[i].status; output[i].FmtValue.largeValue = values[i].value;
        output[i].szName = values[i].badName ? reinterpret_cast<LPWSTR>(1) : names;
        std::memcpy(names, values[i].name.c_str(), (values[i].name.size() + 1) * sizeof(wchar_t));
        if (values[i].unterminated) { names[values[i].name.size()] = L'x'; }
        names += values[i].name.size() + 1;
    }
    *bytes = static_cast<DWORD>(required); return ERROR_SUCCESS;
}
HANDLE WINAPI process(DWORD, BOOL, DWORD pid) { return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(pid)); }
BOOL WINAPI times(HANDLE handle, LPFILETIME created, LPFILETIME, LPFILETIME, LPFILETIME) {
    if (delayAfter && phase == 2 && !delayed) {
        delayed = true; std::this_thread::sleep_for(std::chrono::milliseconds(5200));
    }
    auto value = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(handle)) * 1000 + 100;
    if (reused && phase == 2) { ++value; }
    created->dwHighDateTime = static_cast<DWORD>(value >> 32); created->dwLowDateTime = static_cast<DWORD>(value); return TRUE;
}
BOOL WINAPI closeProcess(HANDLE) { return TRUE; }
}

#define PdhOpenQueryW fixture::open
#define PdhCloseQuery fixture::close
#define PdhAddEnglishCounterW fixture::add
#define PdhCollectQueryData fixture::collect
#define PdhGetFormattedCounterArrayW fixture::array
#define OpenProcess fixture::process
#define GetProcessTimes fixture::times
#define CloseHandle fixture::closeProcess
#include "../../shared/evidence/GpuMemoryEvidence.cpp"
#undef PdhOpenQueryW
#undef PdhCloseQuery
#undef PdhAddEnglishCounterW
#undef PdhCollectQueryData
#undef PdhGetFormattedCounterArrayW
#undef OpenProcess
#undef GetProcessTimes
#undef CloseHandle

int main()
{
    using namespace ksword::gpu_memory;
    std::atomic_bool cancel{false};
    fixture::reset(); fixture::failOpen = true;
    Snapshot failed; collect(failed, cancel);
    assert(failed.queryAttempted && failed.queryStatus == 5 && failed.counters.empty() && !failed.collectionAttempted);
    fixture::reset();
    fixture::entries[0] = {{L"luid_0_phys_0", 0}};
    fixture::entries[1] = {{L"luid_0_phys_0", -1}};
    fixture::entries[2] = {{L"pid_123_luid_0_phys_0", 4096}};
    fixture::entries[3] = {{L"pid_123_luid_0_phys_0", 0, PDH_CSTATUS_NO_INSTANCE}};
    Snapshot values; collect(values, cancel);
    assert(values.counters.size() == 4 && values.firstCollectionAttempted && values.collectionAttempted);
    assert(values.counters[0].valueKnown && values.counters[0].bytes == 0);
    assert(!values.counters[1].valueKnown && values.counters[1].status == PDH_INVALID_DATA);
    assert(values.counters[2].valueKnown && values.counters[2].processIdentityKnown && values.counters[2].processCreateTime == 123100);
    assert(!values.counters[3].valueKnown && values.counters[3].bytes == 0);
    fixture::reused = true;
    Snapshot reuse; collect(reuse, cancel);
    assert(!reuse.counters[2].processIdentityKnown && reuse.counters[2].processCreateTime == 0);
    fixture::reset();
    fixture::entries[0] = {{L"invalid", 99, PDH_CSTATUS_VALID_DATA, true}};
    Snapshot bad; collect(bad, cancel);
    assert(!bad.counters[0].valueKnown && bad.counters[0].status == PDH_INVALID_DATA);
    fixture::reset();
    fixture::entries[0] = {{L"ab", 99, PDH_CSTATUS_VALID_DATA, false, true}};
    Snapshot terminator; collect(terminator, cancel);
    assert(!terminator.counters[0].valueKnown && terminator.counters[0].status == PDH_INVALID_DATA); // zero alignment padding is not API payload
    fixture::reset(); fixture::oversized = true;
    Snapshot bufferCap; collect(bufferCap, cancel);
    assert(bufferCap.truncated && !bufferCap.counters[0].valueKnown);
    fixture::reset();
    fixture::entries[0].assign(4096, fixture::Entry{L"luid_0_phys_0", 0});
    fixture::arrayStatus[1] = PDH_NO_DATA;
    Snapshot capped; collect(capped, cancel);
    assert(capped.counters.size() == 4096 && capped.truncated);
    fixture::reset(); fixture::delayAfter = true;
    fixture::entries[2] = {{L"pid_123_luid_0_phys_0", 4096}, {L"pid_123_luid_0_phys_1", 4096}};
    Snapshot timed; collect(timed, cancel);
    std::size_t processRows = 0;
    for (const auto& row : timed.counters) { processRows += processMetric(row.metric) && !row.instance.empty() ? 1 : 0; }
    assert(timed.truncated && processRows == 1); // deadline checked within the final row loop
    std::cout << "GPU_MEMORY_EVIDENCE_MOCK_TESTS=PASS\n";
}
