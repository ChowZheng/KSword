#include "GpuMemoryEvidence.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Pdh.h>
#include <PdhMsg.h>
#include <algorithm>
#include <chrono>
#include <limits>
#include <map>
#include <utility>

#ifdef _MSC_VER
#pragma comment(lib, "pdh.lib")
#endif

namespace ksword::gpu_memory {
std::uint32_t processId(const std::wstring& instance) noexcept
{
    if (instance.compare(0, 4, L"pid_") != 0) { return 0; }
    std::uint64_t value = 0;
    std::size_t i = 4;
    for (; i < instance.size() && instance[i] >= L'0' && instance[i] <= L'9'; ++i) {
        value = value * 10 + static_cast<unsigned>(instance[i] - L'0');
        if (value > (std::numeric_limits<std::uint32_t>::max)()) { return 0; }
    }
    return i > 4 && i < instance.size() && instance[i] == L'_' ? static_cast<std::uint32_t>(value) : 0;
}
namespace {
constexpr std::size_t maxCounters = 4096;
constexpr DWORD maxBuffer = 4 * 1024 * 1024;
std::uint64_t now()
{
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}
struct Query { PDH_HQUERY handle = nullptr; ~Query() { if (handle) { PdhCloseQuery(handle); } } };
struct Definition { Metric metric; const wchar_t* path; };
constexpr Definition definitions[]{
    {Metric::AdapterShared, L"\\GPU Adapter Memory(*)\\Shared Usage"},
    {Metric::AdapterDedicated, L"\\GPU Adapter Memory(*)\\Dedicated Usage"},
    {Metric::ProcessShared, L"\\GPU Process Memory(*)\\Shared Usage"},
    {Metric::ProcessDedicated, L"\\GPU Process Memory(*)\\Dedicated Usage"}
};
std::uint64_t creation(std::uint32_t pid)
{
    const auto handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!handle) { return 0; }
    FILETIME created{}, exited{}, kernel{}, user{};
    const auto ok = GetProcessTimes(handle, &created, &exited, &kernel, &user);
    CloseHandle(handle);
    return ok ? (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime : 0;
}
}
void collect(Snapshot& result, const std::atomic_bool& cancel)
{
    result = {};
    result.startedUtc100ns = now();
    if (cancel.load()) { result.cancelled = true; result.finishedUtc100ns = now(); return; }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    const auto stopped = [&] {
        if (cancel.load()) { result.cancelled = true; return true; }
        if (std::chrono::steady_clock::now() >= deadline) { result.truncated = true; return true; }
        return false;
    };
    const auto append = [&](Counter row) {
        if (result.counters.size() >= maxCounters) { result.truncated = true; return; }
        result.counters.push_back(std::move(row));
    };
    Query query;
    result.queryAttempted = true;
    result.queryStatus = static_cast<std::uint32_t>(PdhOpenQueryW(nullptr, 0, &query.handle));
    if (result.queryStatus != ERROR_SUCCESS) { result.finishedUtc100ns = now(); return; }
    struct Pending { PDH_HCOUNTER handle; Definition definition; };
    std::vector<Pending> pending;
    for (const auto& definition : definitions) {
        if (stopped()) { break; }
        PDH_HCOUNTER handle = nullptr;
        const auto status = PdhAddEnglishCounterW(query.handle, definition.path, 0, &handle);
        if (status == ERROR_SUCCESS) { pending.push_back({handle, definition}); }
        else {
            Counter row;
            row.metric = definition.metric;
            row.path = definition.path;
            row.status = static_cast<std::uint32_t>(status);
            append(std::move(row));
        }
    }
    // Read current process creation identities on both sides of the one PDH
    // sample; names embedded in counters are PID observations, not lifetimes.
    std::map<std::uint32_t, std::uint64_t> before;
    auto enumerate = [&](PDH_HCOUNTER handle, std::vector<std::uint64_t>& storage,
        DWORD& count, PDH_FMT_COUNTERVALUE_ITEM_W*& items, DWORD& usedBytes) -> PDH_STATUS {
        usedBytes = 0;
        DWORD bytes = 0;
        auto status = PdhGetFormattedCounterArrayW(handle, PDH_FMT_LARGE | PDH_FMT_NOSCALE,
            &bytes, &count, nullptr);
        const auto moreData = static_cast<PDH_STATUS>(PDH_MORE_DATA);
        if (bytes > maxBuffer || count > maxCounters) { result.truncated = true; return PDH_INSUFFICIENT_BUFFER; }
        if (status != moreData || !bytes) { return status == moreData ? static_cast<PDH_STATUS>(PDH_INSUFFICIENT_BUFFER) : status; }
        storage.assign((bytes + 7) / 8, 0);
        items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(storage.data());
        status = PdhGetFormattedCounterArrayW(handle, PDH_FMT_LARGE | PDH_FMT_NOSCALE, &bytes, &count, items);
        if (status == ERROR_SUCCESS && (bytes > storage.size() * 8 || count > maxCounters
            || count > bytes / sizeof(*items))) { return PDH_INVALID_DATA; }
        if (status == ERROR_SUCCESS) { usedBytes = bytes; }
        return status;
    };
    if (stopped()) { result.finishedUtc100ns = now(); return; }
    result.firstCollectionAttempted = true;
    result.firstCollectionStatus = static_cast<std::uint32_t>(PdhCollectQueryData(query.handle));
    for (const auto& item : pending) {
        if (stopped()) { break; }
        if (item.definition.metric != Metric::ProcessShared && item.definition.metric != Metric::ProcessDedicated) { continue; }
        std::vector<std::uint64_t> storage;
        DWORD count = 0, usedBytes = 0;
        PDH_FMT_COUNTERVALUE_ITEM_W* items = nullptr;
        if (enumerate(item.handle, storage, count, items, usedBytes) != ERROR_SUCCESS) { continue; }
        const auto base = reinterpret_cast<std::uintptr_t>(storage.data()), end = base + usedBytes;
        for (DWORD i = 0; i < count; ++i) {
            if (stopped()) { break; }
            const auto pointer = reinterpret_cast<std::uintptr_t>(items[i].szName);
            if (!pointer || pointer < base || pointer >= end || pointer % alignof(wchar_t)) { continue; }
            const auto capacity = (end - pointer) / sizeof(wchar_t);
            std::size_t length = 0;
            while (length < capacity && items[i].szName[length]) { ++length; }
            if (length == capacity) { continue; }
            const auto pid = processId(std::wstring(items[i].szName, length));
            if (pid && !before.count(pid)) { before.emplace(pid, creation(pid)); }
        }
    }
    if (stopped()) { result.finishedUtc100ns = now(); return; }
    result.collectionAttempted = true;
    result.queryStatus = static_cast<std::uint32_t>(PdhCollectQueryData(query.handle));
    const auto sampled = now();
    std::map<std::uint32_t, std::uint64_t> afterIdentities;
    for (const auto& item : pending) {
        if (stopped()) { break; }
        if (result.counters.size() >= maxCounters) { result.truncated = true; break; }
        std::vector<std::uint64_t> storage;
        DWORD count = 0, usedBytes = 0;
        PDH_FMT_COUNTERVALUE_ITEM_W* items = nullptr;
        auto status = result.queryStatus == ERROR_SUCCESS ? enumerate(item.handle, storage, count, items, usedBytes) : static_cast<PDH_STATUS>(result.queryStatus);
        if (status != ERROR_SUCCESS || !count) {
            Counter row;
            row.metric = item.definition.metric; row.path = item.definition.path;
            row.status = static_cast<std::uint32_t>(status == ERROR_SUCCESS ? PDH_CSTATUS_NO_INSTANCE : status);
            row.sampledUtc100ns = sampled;
            append(std::move(row));
            continue;
        }
        const auto base = reinterpret_cast<std::uintptr_t>(storage.data()), end = base + usedBytes;
        for (DWORD i = 0; i < count; ++i) {
            if (stopped()) { break; }
            if (result.counters.size() >= maxCounters) { result.truncated = true; break; }
            Counter row;
            row.metric = item.definition.metric; row.path = item.definition.path; row.sampledUtc100ns = sampled;
            row.status = static_cast<std::uint32_t>(items[i].FmtValue.CStatus);
            const auto pointer = reinterpret_cast<std::uintptr_t>(items[i].szName);
            if (!pointer || pointer < base || pointer >= end || pointer % alignof(wchar_t)) { row.status = PDH_INVALID_DATA; }
            else {
                const auto capacity = (end - pointer) / sizeof(wchar_t);
                std::size_t length = 0;
                while (length < capacity && items[i].szName[length]) { ++length; }
                if (length == capacity) { row.status = PDH_INVALID_DATA; }
                else { row.instance.assign(items[i].szName, length); }
            }
            row.valueKnown = (row.status == PDH_CSTATUS_VALID_DATA || row.status == PDH_CSTATUS_NEW_DATA) && items[i].FmtValue.largeValue >= 0;
            if (row.valueKnown) { row.bytes = static_cast<std::uint64_t>(items[i].FmtValue.largeValue); }
            else if (items[i].FmtValue.largeValue < 0) { row.status = PDH_INVALID_DATA; }
            row.pid = processId(row.instance);
            if (row.pid) {
                const auto found = before.find(row.pid);
                auto observed = afterIdentities.find(row.pid);
                if (observed == afterIdentities.end()) { observed = afterIdentities.emplace(row.pid, creation(row.pid)).first; }
                const auto after = observed->second;
                row.processIdentityKnown = found != before.end() && found->second && found->second == after;
                if (row.processIdentityKnown) { row.processCreateTime = after; }
            }
            append(std::move(row));
        }
    }
    result.finishedUtc100ns = now();
}
}
