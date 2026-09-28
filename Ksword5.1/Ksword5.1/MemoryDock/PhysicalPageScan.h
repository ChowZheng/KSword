#pragma once
#include "../../../shared/evidence/PfnAccounting.h"
#include <QString>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>

namespace ksword::pfn {
struct Owner { std::uint32_t pid = 0; QString name; };
struct Group {
    Use use = Use::Unknown;
    std::uint64_t key = 0, pages = 0, activePages = 0, firstPfn = 0;
    QString name;
    std::uint32_t pid = 0;
};
struct Scan {
    Accounting accounting;
    std::vector<Range> ranges;
    std::vector<Group> groups;
    std::map<std::uint64_t, Owner> owners;
    // Bounded sample of PFN identities for drill-down, never the accounting input.
    std::array<std::vector<std::pair<std::uint64_t, Identity>>, useCount> examples;
    QString started, finished;
    std::uint64_t totalBefore = 0, availableBefore = 0, totalAfter = 0, availableAfter = 0, installed = 0;
    std::uint64_t nativeBatches = 0, driverBatches = 0, failedBatches = 0, groupedOverflowPages = 0;
    std::uint64_t elapsedMs = 0;
    long rangesStatus = 0, ownersStatus = 0, lastPageStatus = 0;
    bool cancelled = false, complete = false, hypervisor = false, secureKernel = false, secureKnown = false;
    bool resourceFailure = false, rangesChanged = false;
};
struct ScanJob {
    std::atomic_bool cancel{false}, done{false};
    std::atomic<std::uint64_t> visited{0}, total{0};
    std::mutex mutex;
    std::shared_ptr<Scan> result;
};
void collectPhysicalPages(const std::shared_ptr<ScanJob>& job);
}
