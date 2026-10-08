#pragma once
#include "../../../shared/evidence/PoolAllocationAnalysis.h"
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace ks::evidence::pool {

// Image IDs belong to this ETL, never to the currently running driver list.
struct TraceImage {
    std::uint64_t id = 0, base = 0, size = 0, firstTimestamp = 0, lastTimestamp = 0;
    std::uint32_t pid = 0;
    std::wstring path;
    std::uint32_t timeDateStamp = 0, checksum = 0;
    bool imageIdentityKnown = false;
};

struct TraceReadResult {
    Result analysis;
    std::vector<TraceImage> images;
    std::uint32_t status = 0;
    std::uint64_t eventsLost = 0, buffersLost = 0, unsupportedPoolEvents = 0;
    std::uint64_t lossMarkers = 0;
    std::uint64_t missingStackEvents = 0, ambiguousStackEvents = 0;
    bool lossCountsKnown = false, completed = false, cancelled = false, timedOut = false;
    bool imageHistoryTruncated = false, fileChanged = false;
};

struct TraceReadOptions {
    Limits analysisLimits;
    std::uint64_t maxFileBytes = 1024ULL * 1024 * 1024;
    std::uint64_t maxTraceEvents = 4000000;
    std::size_t maxStackRecords = 200000, maxImageRecords = 32768;
    std::uint32_t timeoutMs = 60000;
};

// Read-only, bounded offline ETL replay. No tracking session or driver is started.
TraceReadResult ReadPoolAllocationTrace(const std::wstring& path,
    const std::atomic_bool& cancel, const TraceReadOptions& options = {});

// Resolve one selected historical stack using local symbols only. Each item is
// empty if its module or a matching local image/symbol cannot be established.
// DbgHelp uses the application's shared serialization lock.
std::vector<std::wstring> ResolvePoolTraceStack(const TraceReadResult& trace,
    const Group& group, const std::atomic_bool& cancel);

} // namespace ks::evidence::pool
