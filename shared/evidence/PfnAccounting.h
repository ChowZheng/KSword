#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ksword::pfn {
constexpr std::uint64_t pageBytes = 4096;
enum class Use : std::uint8_t {
    Private, MappedFile, Shareable, PageTable, PagedPool, NonpagedPool,
    SystemPte, SessionPrivate, Metafile, Awe, DriverLocked, KernelStack,
    Image, Compression, Unassigned, Unknown, Count
};
constexpr std::size_t useCount = static_cast<std::size_t>(Use::Count);
struct Range { std::uint64_t first = 0, count = 0; };
struct Identity { std::uint64_t frame = ~0ULL, backing = 0; };

// Native keys can be redacted, duplicated, or recycled while a scan is running.
// Retain endpoint observations, but once a key contradicts itself no later
// duplicate may restore its owner in this scan. SameOwner compares identity
// evidence, not the endpoint flags (and does not promise an atomic snapshot).
template <class Owners, class Conflicts, class SameOwner>
void mergeOwnerSnapshot(Owners& owners, Conflicts& conflicts, std::uint64_t key,
    typename Owners::mapped_type candidate, bool after, SameOwner&& sameOwner)
{
    if (!key || conflicts.count(key)) { return; }
    const auto found = owners.find(key);
    if (found == owners.end()) {
        candidate.seenBefore = !after;
        candidate.seenAfter = after;
        owners.emplace(key, std::move(candidate));
    } else if (!sameOwner(found->second, candidate)) {
        owners.erase(found);
        conflicts.insert(key);
    } else if (after) { found->second.seenAfter = true; }
    else { found->second.seenBefore = true; }
}

// A handle retained from before PFN sampling prevents its process object
// from being recycled. Both source endpoints and live handle checks are needed.
inline bool ownerLifetimeVerified(bool seenBefore, bool seenAfter, bool held,
    std::uint64_t createdBefore, std::uint64_t createdAfter)
{
    return seenBefore && seenAfter && held && createdBefore != 0 && createdBefore == createdAfter;
}

// Normalize overlapping/unsorted RAM extents before querying. Holes are never
// queried or converted into hardware/hypervisor consumption.
inline bool normalizeRanges(std::vector<Range>& ranges)
{
    for (const auto& range : ranges) {
        if (!range.count || range.first >= (1ULL << 40) || range.count > (1ULL << 40) - range.first) { return false; }
    }
    std::sort(ranges.begin(), ranges.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<Range> merged;
    for (const auto& range : ranges) {
        if (!merged.empty() && range.first <= merged.back().first + merged.back().count) {
            auto& last = merged.back();
            last.count = std::max(last.first + last.count, range.first + range.count) - last.first;
        } else { merged.push_back(range); }
    }
    ranges.swap(merged);
    return true;
}

inline unsigned state(const Identity& page) { return static_cast<unsigned>((page.frame >> 4) & 7); }
inline unsigned nativeUse(const Identity& page) { return static_cast<unsigned>(page.frame & 15); }
inline std::uint64_t processKey(const Identity& page) { return (page.frame >> 9) & 0xFFFFFFFFFFFFULL; }
inline bool available(unsigned list) { return list <= 2; }
inline bool inUseState(unsigned list) { return list == 3 || list == 4 || list == 6 || list == 7; }
inline Use classify(const Identity& page, bool compressionOwner = false)
{
    const unsigned use = nativeUse(page);
    if (page.frame == ~0ULL) { return Use::Unknown; }
    if (state(page) < 2) { return Use::Unassigned; }
    if (use > 11) { return Use::Unknown; }
    if (use == 0 && compressionOwner) { return Use::Compression; }
    if (use == 1 && (page.backing & 1) != 0) { return Use::Image; }
    return static_cast<Use>(use);
}

struct Accounting {
    std::array<std::array<std::uint64_t, 8>, useCount> byUseAndState{};
    // Preserve future/unsupported native codes as evidence, without inventing a
    // classification for them or losing their physical page state.
    std::array<std::array<std::uint64_t, 8>, 16> unknownByNativeUse{};
    std::uint64_t expected = 0, visited = 0, valid = 0, unreadable = 0, availablePages = 0;
    std::uint64_t pinned = 0, nonTradeable = 0;
    void add(const Identity& page, Use use) {
        ++visited;
        if (page.frame == ~0ULL) { ++unreadable; return; }
        ++valid;
        const auto list = state(page);
        ++byUseAndState[static_cast<std::size_t>(use)][list];
        if (use == Use::Unknown) { ++unknownByNativeUse[nativeUse(page)][list]; }
        availablePages += available(list) ? 1 : 0;
        pinned += (page.frame >> 8) & 1;
        nonTradeable += (page.frame >> 60) & 1;
    }
    void failed(std::uint64_t count) { visited += count; unreadable += count; }
    std::uint64_t inUse(Use use) const {
        std::uint64_t sum = 0;
        // Bad pages are not usable in-use RAM; retain them in the state matrix.
        for (unsigned list : {3U, 4U, 6U, 7U}) { sum += byUseAndState[static_cast<std::size_t>(use)][list]; }
        return sum;
    }
    std::uint64_t bad() const {
        std::uint64_t sum = 0;
        for (const auto& row : byUseAndState) { sum += row[5]; }
        return sum;
    }
    std::uint64_t notScanned() const { return expected > visited ? expected - visited : 0; }
    bool reconciles() const {
        std::uint64_t counted = availablePages + bad() + unreadable + notScanned();
        for (std::size_t use = 0; use < useCount; ++use) { counted += inUse(static_cast<Use>(use)); }
        return counted == expected && visited == valid + unreadable;
    }
    void reclassify(Use from, Use to, const std::array<std::uint64_t, 8>& pages) {
        // Caller supplies an already-counted disjoint owner group. Moving a
        // category never changes the physical denominator or page-state totals.
        for (unsigned list = 0; list < 8; ++list) {
            byUseAndState[static_cast<std::size_t>(from)][list] -= pages[list];
            byUseAndState[static_cast<std::size_t>(to)][list] += pages[list];
        }
    }
};

struct OwnerCoverage {
    using Matrix = std::array<std::array<std::uint64_t, 8>, useCount>;
    Matrix resolved{}, unresolved{}, notApplicable{}, objectKeyKnown{};
    void initialize(const Accounting& accounting) {
        *this = {};
        for (std::size_t use = 0; use < useCount; ++use) {
            for (unsigned list = 0; list < 8; ++list) {
                // Available and bad pages do not require an active consumer;
                // standby may still have a separately reported backing clue.
                auto& target = available(list) || list == 5 ? notApplicable : unresolved;
                target[use][list] = accounting.byUseAndState[use][list];
            }
        }
    }
    bool resolve(Use use, const std::array<std::uint64_t, 8>& pages) {
        const auto index = static_cast<std::size_t>(use);
        for (unsigned list : {3U, 4U, 6U, 7U}) { if (pages[list] > unresolved[index][list]) { return false; } }
        for (unsigned list : {3U, 4U, 6U, 7U}) {
            // The caller must supply a disjoint, directly evidenced consumer.
            const auto amount = pages[list];
            unresolved[index][list] -= amount;
            resolved[index][list] += amount;
        }
        return true;
    }
    std::uint64_t inUseUnresolved(Use use) const {
        std::uint64_t result = 0;
        for (unsigned list : {3U, 4U, 6U, 7U}) { result += unresolved[static_cast<std::size_t>(use)][list]; }
        return result;
    }
    std::uint64_t knownInUseUnresolved() const {
        std::uint64_t result = 0;
        for (std::size_t use = 0; use < useCount; ++use) {
            if (use != static_cast<std::size_t>(Use::Unknown)) { result += inUseUnresolved(static_cast<Use>(use)); }
        }
        return result;
    }
    bool reconciles(const Accounting& accounting) const {
        for (std::size_t use = 0; use < useCount; ++use) {
            for (unsigned list = 0; list < 8; ++list) {
                if (resolved[use][list] + unresolved[use][list] + notApplicable[use][list]
                    != accounting.byUseAndState[use][list]) { return false; }
            }
        }
        return true;
    }
};

struct AuditCriterionSample {
    std::string domain, epoch, architecture;
    std::uint32_t windowsMajor = 0, windowsMinor = 0, windowsBuild = 0;
    std::uint64_t unknownInUsePages = 0, unreadablePages = 0, unscannedPages = 0;
    bool complete = false, reconciles = false, semanticsValidated = false;
};

// A proposed engineering acceptance predicate, not proof that a Windows build
// is supported or that consumers were attributed. The live collector never
// sets semanticsValidated from query success or from this arithmetic predicate.
inline bool proposedThreeScanCriterion(const std::array<AuditCriterionSample, 3>& samples)
{
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const auto& sample = samples[i];
        if (!sample.complete || !sample.reconciles || !sample.semanticsValidated
            || sample.domain.empty() || sample.epoch.empty() || sample.architecture.empty()
            || !sample.windowsBuild || sample.unreadablePages || sample.unscannedPages
            || sample.unknownInUsePages > (64ULL * 1024 * 1024 / pageBytes)) { return false; }
        if (sample.domain != samples[0].domain || sample.architecture != samples[0].architecture
            || sample.windowsMajor != samples[0].windowsMajor || sample.windowsMinor != samples[0].windowsMinor
            || sample.windowsBuild != samples[0].windowsBuild) { return false; }
        for (std::size_t previous = 0; previous < i; ++previous) {
            if (sample.epoch == samples[previous].epoch) { return false; }
        }
    }
    return true;
}

struct RawEvidenceProgress {
    std::uint64_t bytes = 0, records = 0, ledgerPages = 0, attemptPages = 0, chunks = 0, peakEncodedBytes = 0;
    bool failed = false, headerWritten = false, finalized = false;
};

// One record at a time, with no whole-scan identity buffer. A short/failed write
// is explicit and disables later appends; prior records remain inspectable.
template <class Writer>
bool appendRawEvidence(RawEvidenceProgress& progress, std::string_view record, Writer&& writer)
{
    if (progress.failed || progress.finalized) { return false; }
    progress.peakEncodedBytes = std::max(progress.peakEncodedBytes, static_cast<std::uint64_t>(record.size()));
    const auto written = writer(record.data(), record.size());
    if (written > 0) { progress.bytes += static_cast<std::uint64_t>(written); }
    if (written < 0 || static_cast<std::uint64_t>(written) != record.size()) { progress.failed = true; return false; }
    ++progress.records;
    return true;
}

inline bool terminalQueryFailure(std::int32_t status)
{
    // Splitting cannot repair privilege, unsupported-ABI or resource failures.
    // Retrying these per physical page would produce an unbounded denial loop.
    switch (static_cast<std::uint32_t>(status)) {
    case 0xC0000002U: // STATUS_NOT_IMPLEMENTED
    case 0xC0000003U: // STATUS_INVALID_INFO_CLASS
    case 0xC0000022U: // STATUS_ACCESS_DENIED
    case 0xC0000061U: // STATUS_PRIVILEGE_NOT_HELD
    case 0xC000009AU: // STATUS_INSUFFICIENT_RESOURCES
    case 0xC00000BBU: // STATUS_NOT_SUPPORTED
        return true;
    default: return false;
    }
}

struct RangeRecovery {
    std::uint64_t queries = 0, failedQueries = 0, recoveredPages = 0;
    std::int32_t lastStatus = 0;
    bool initialFailed = false, terminal = false, queried = false;
};

// A failed bulk query does not establish that every PFN in it is unreadable.
// Salvage smaller extents with a per-batch query budget. The returned vector
// still contains exactly one final identity per requested PFN; retries never
// become additional accounting records. Reader must validate PFN order/length.
template <class Reader, class Stopped>
RangeRecovery recoverRange(std::uint64_t first, std::uint32_t count,
    std::vector<Identity>& result, Reader&& reader, Stopped&& stopped,
    std::uint32_t retryBudget = 64)
{
    RangeRecovery stats;
    result.assign(count, Identity{});
    std::vector<Identity> batch;
    std::uint64_t initiallyValid = 0;
    auto queryPart = [&](auto&& self, std::uint32_t offset, std::uint32_t amount, bool initial) -> void {
        if (stopped() || stats.terminal) { return; }
        if (!initial) {
            if (!retryBudget) { return; }
            --retryBudget;
            ++stats.queries;
        }
        batch.clear();
        stats.queried = true;
        stats.lastStatus = reader(first + offset, amount, batch);
        if (stats.lastStatus >= 0 && batch.size() != amount) { stats.lastStatus = static_cast<std::int32_t>(0xC000003EU); }
        if (stats.lastStatus >= 0) {
            std::copy(batch.begin(), batch.end(), result.begin() + offset);
            if (initial) {
                for (const auto& page : batch) { initiallyValid += page.frame != ~0ULL ? 1 : 0; }
            }
            return;
        }
        ++stats.failedQueries;
        stats.initialFailed |= initial;
        stats.terminal = terminalQueryFailure(stats.lastStatus);
        if (!stats.terminal && amount > 1) {
            const auto half = amount / 2;
            self(self, offset, half, false);
            self(self, offset + half, amount - half, false);
        }
    };
    if (count) { queryPart(queryPart, 0, count, true); }
    std::uint64_t finallyValid = 0;
    for (const auto& page : result) { finallyValid += page.frame != ~0ULL ? 1 : 0; }
    stats.recoveredPages = finallyValid - initiallyValid;
    return stats;
}

struct IdentityRetry {
    std::uint64_t queries = 0, recoveredPages = 0;
    std::int32_t status = 0;
};

struct IdentityRetryPolicy {
    bool enabled = true;
    std::int32_t lastFailure = 0;
    void record(const IdentityRetry& retry) {
        if (retry.queries && retry.status < 0) {
            lastFailure = retry.status;
            if (terminalQueryFailure(retry.status)) { enabled = false; }
        }
    }
};

// Re-query only unavailable slots once, never replace an already-readable
// identity or accept a short payload. Reader validates exact PFN order.
template <class Reader, class Stopped>
IdentityRetry retryUnavailable(std::uint64_t first, std::vector<Identity>& pages,
    Reader&& reader, Stopped&& stopped)
{
    IdentityRetry stats;
    std::vector<std::uint64_t> pfns;
    std::vector<std::size_t> pageIndices;
    for (std::size_t i = 0; i < pages.size(); ++i) {
        if (pages[i].frame == ~0ULL) { pfns.push_back(first + i); pageIndices.push_back(i); }
    }
    if (pfns.empty() || stopped()) { return stats; }
    std::vector<Identity> retried;
    ++stats.queries;
    stats.status = reader(pfns, retried);
    if (stats.status >= 0 && retried.size() != pfns.size()) { stats.status = static_cast<std::int32_t>(0xC000003EU); }
    if (stats.status >= 0) {
        for (std::size_t i = 0; i < retried.size(); ++i) {
            if (retried[i].frame != ~0ULL) { pages[pageIndices[i]] = retried[i]; ++stats.recoveredPages; }
        }
    }
    return stats;
}
}
