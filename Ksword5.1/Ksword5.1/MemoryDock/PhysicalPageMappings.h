#pragma once
#include <QString>
#include <atomic>
#include <cstdint>
#include <memory>
#include <map>
#include <mutex>
#include <vector>

namespace ksword::pfn {
struct Mapping {
    std::uint64_t pfn = 0, address = 0;
    std::uint32_t pid = 0, backingIndex = 0, pageSize = 0, shareCount = 0;
    bool locked = false, shared = false, attributesKnown = false;
};
struct Backing { QString process, path; std::uint32_t kind = 0; };
struct Mappings {
    QString sampledAt;
    std::vector<Mapping> rows;
    std::vector<Backing> backing;
    std::map<std::uint64_t, QString> observedFileNames;
    std::uint64_t tested = 0, failed = 0, distinct = 0, large = 0, locked = 0, multiplyMapped = 0;
    std::uint32_t processes = 0, inaccessible = 0, scannedProcesses = 0;
    long status = 0;
    bool cancelled = false, budgetReached = false, driverAvailable = false, failedAllocation = false;
};
struct MappingJob {
    std::atomic_bool cancel{false}, done{false};
    std::atomic<std::uint64_t> tested{0};
    std::mutex mutex;
    std::shared_ptr<Mappings> result;
};
void collectPhysicalMappings(const std::shared_ptr<MappingJob>& job, unsigned seconds);
}
