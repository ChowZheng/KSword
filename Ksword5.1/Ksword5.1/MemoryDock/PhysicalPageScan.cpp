#include "PhysicalPageScan.h"
#include "../ArkDriverClient/ArkDriverPfn.h"
#include <QDateTime>
#include <chrono>
#include <cstring>
#include <intrin.h>
#include <new>
#include <tuple>

namespace ksword::pfn {
namespace {
void memoryTotals(std::uint64_t& total, std::uint64_t& availableBytes)
{
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) { total = memory.ullTotalPhys; availableBytes = memory.ullAvailPhys; }
}

std::shared_ptr<Scan> collect(const std::shared_ptr<ScanJob>& job)
{
    auto scan = std::make_shared<Scan>();
    const auto start = std::chrono::steady_clock::now();
    scan->started = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    memoryTotals(scan->totalBefore, scan->availableBefore);
    ULONGLONG installedKb = 0;
    if (GetPhysicallyInstalledSystemMemory(&installedKb)) { scan->installed = installedKb * 1024; }
    int cpu[4]{};
    __cpuid(cpu, 1);
    scan->hypervisor = (static_cast<unsigned>(cpu[2]) & (1U << 31)) != 0;
    using Query = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
    const auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    // SYSTEM_ISOLATED_USER_MODE_INFORMATION starts with SecureKernelRunning.
    std::array<unsigned char, 16> isolated{};
    if (query && query(165, isolated.data(), static_cast<ULONG>(isolated.size()), nullptr) >= 0) {
        scan->secureKnown = true;
        scan->secureKernel = (isolated[0] & 1) != 0;
    }
    ark::PfnQueryClient client;
    std::vector<KSWORD_ARK_PFN_RANGE> ranges;
    scan->rangesStatus = client.ranges(ranges);
    if (scan->rangesStatus >= 0) {
        for (const auto& range : ranges) { scan->ranges.push_back({range.firstPfn, range.pageCount}); }
        if (!normalizeRanges(scan->ranges) || scan->ranges.empty()) {
            scan->rangesStatus = static_cast<long>(0xC000003EUL);
            scan->ranges.clear();
        }
    }
    for (const auto& range : scan->ranges) { scan->accounting.expected += range.count; }
    job->total.store(scan->accounting.expected);
    if (!scan->ranges.empty()) {
        std::vector<KSWORD_ARK_PFN_OWNER> owners;
        scan->ownersStatus = client.owners(owners);
        for (const auto& owner : owners) {
            if (!owner.processKey) { continue; } // A redacted/null key must never own all unresolved private pages.
            scan->owners.emplace(owner.processKey, Owner{owner.processId,
                QString::fromLatin1(owner.imageName, static_cast<qsizetype>(strnlen_s(owner.imageName, 16)))});
        }
        // A group is a backing identity, never another physical accounting total.
        std::map<std::pair<Use, std::uint64_t>, std::size_t> groupIndex;
        std::vector<KSWORD_ARK_PFN_IDENTITY> batch;
        unsigned consecutiveFailures = 0;
        for (const auto& range : scan->ranges) {
            for (std::uint64_t offset = 0; offset < range.count && !job->cancel.load(); ) {
                const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(KSWORD_ARK_PFN_MAX_PAGES, range.count - offset));
                scan->lastPageStatus = client.pages(range.first + offset, count, batch);
                if (scan->lastPageStatus < 0) {
                    ++scan->failedBatches;
                    scan->accounting.failed(count);
                    // Stop unsupported/denied queries quickly; leave remaining pages unscanned.
                    if (++consecutiveFailures >= 3) { break; }
                } else {
                    consecutiveFailures = 0;
                    for (const auto& record : batch) {
                        const Identity page{record.frame, record.backing};
                        const auto ownerIt = scan->owners.find(processKey(page));
                        const bool compression = nativeUse(page) == 0 && ownerIt != scan->owners.end()
                            && ownerIt->second.name.compare(QStringLiteral("MemCompression"), Qt::CaseInsensitive) == 0;
                        const Use use = classify(page, compression);
                        scan->accounting.add(page, use);
                        if (page.frame == ~0ULL) { continue; }
                        auto& examples = scan->examples[static_cast<std::size_t>(use)];
                        if (!available(state(page)) && examples.size() < 256) { examples.emplace_back(record.pfn, page); }
                        if (available(state(page)) || state(page) == 5) { continue; }
                        const bool process = nativeUse(page) == 0;
                        const bool file = nativeUse(page) == 1 || nativeUse(page) == 8;
                        const auto key = process ? processKey(page) : (file ? page.backing & ~3ULL : 0ULL);
                        const auto indexKey = std::make_pair(use, key);
                        auto entry = groupIndex.find(indexKey);
                        if (entry == groupIndex.end()) {
                            if (scan->groups.size() >= 65536) { ++scan->groupedOverflowPages; continue; }
                            Group group;
                            group.use = use;
                            group.key = key;
                            group.firstPfn = record.pfn;
                            if (process && ownerIt != scan->owners.end()) { group.pid = ownerIt->second.pid; group.name = ownerIt->second.name; }
                            entry = groupIndex.emplace(indexKey, scan->groups.size()).first;
                            scan->groups.push_back(std::move(group));
                        }
                        auto& group = scan->groups[entry->second];
                        ++group.pages;
                        group.activePages += state(page) == 6 ? 1 : 0;
                    }
                }
                offset += count;
                job->visited.store(scan->accounting.visited);
            }
            if (job->cancel.load() || consecutiveFailures >= 3) { break; }
        }
        // A hot-add/remove changes the denominator; don't label it a complete snapshot.
        std::vector<KSWORD_ARK_PFN_RANGE> afterRanges;
        if (client.ranges(afterRanges) >= 0) {
            std::vector<Range> normalized;
            for (const auto& range : afterRanges) { normalized.push_back({range.firstPfn, range.pageCount}); }
            scan->rangesChanged = !normalizeRanges(normalized) || normalized.size() != scan->ranges.size();
            for (std::size_t i = 0; !scan->rangesChanged && i < normalized.size(); ++i) {
                scan->rangesChanged = normalized[i].first != scan->ranges[i].first || normalized[i].count != scan->ranges[i].count;
            }
        } else { scan->rangesChanged = true; }
    }
    scan->cancelled = job->cancel.load();
    scan->complete = scan->accounting.expected != 0 && !scan->cancelled && !scan->rangesChanged
        && scan->accounting.visited == scan->accounting.expected && scan->accounting.unreadable == 0;
    scan->nativeBatches = client.nativeBatches;
    scan->driverBatches = client.driverBatches;
    memoryTotals(scan->totalAfter, scan->availableAfter);
    scan->finished = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    scan->elapsedMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
    std::sort(scan->groups.begin(), scan->groups.end(), [](const Group& a, const Group& b) { return a.pages > b.pages; });
    return scan;
}
}

void collectPhysicalPages(const std::shared_ptr<ScanJob>& job)
{
    std::shared_ptr<Scan> result;
    try { result = collect(job); }
    catch (...) {
        // No detached worker may terminate the GUI process on resource exhaustion.
        try { result = std::make_shared<Scan>(); result->resourceFailure = true; result->rangesStatus = static_cast<long>(0xC000009AUL); }
        catch (...) { /* A null result is also an explicit failure at the UI boundary. */ }
    }
    {
        std::lock_guard<std::mutex> lock(job->mutex);
        job->result = std::move(result);
    }
    job->done.store(true);
}
}
