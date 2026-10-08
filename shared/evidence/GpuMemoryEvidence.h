#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace ksword::gpu_memory {
enum class Metric { AdapterShared, AdapterDedicated, ProcessShared, ProcessDedicated };
constexpr bool processMetric(Metric value) { return value == Metric::ProcessShared || value == Metric::ProcessDedicated; }
struct Counter {
    Metric metric = Metric::AdapterShared;
    std::wstring path, instance;
    std::uint64_t bytes = 0, sampledUtc100ns = 0;
    std::uint32_t pid = 0, status = 0;
    bool valueKnown = false, processIdentityKnown = false;
    std::uint64_t processCreateTime = 0;
};
struct Snapshot {
    std::uint64_t startedUtc100ns = 0, finishedUtc100ns = 0;
    std::uint32_t queryStatus = 0;
    std::uint32_t firstCollectionStatus = 0;
    bool queryAttempted = false, firstCollectionAttempted = false, collectionAttempted = false;
    bool cancelled = false, truncated = false;
    std::vector<Counter> counters;
};
// Observed consumer metrics. No per-process sum is a unique physical RAM total;
// dedicated metrics are a separate device scope, including possible UMA.
std::uint32_t processId(const std::wstring& instance) noexcept;
void collect(Snapshot& result, const std::atomic_bool& cancel);
}
