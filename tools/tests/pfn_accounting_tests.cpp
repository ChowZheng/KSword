#include "../../shared/evidence/PfnAccounting.h"
#include <cassert>
#include <iostream>
#include <map>
#include <set>
#include <string>

int main()
{
    using namespace ksword::pfn;
    std::vector<Range> ranges{{10, 10}, {5, 10}, {20, 2}, {100, 1}, {5, 10}};
    assert(normalizeRanges(ranges));
    assert(ranges.size() == 2 && ranges[0].first == 5 && ranges[0].count == 17);
    assert(ranges[1].first == 100 && ranges[1].count == 1);
    std::vector<Range> overflow{{(1ULL << 40) - 1, 2}};
    assert(!normalizeRanges(overflow));
    std::vector<Range> zero{{0, 0}};
    assert(!normalizeRanges(zero));

    Accounting counts;
    counts.expected = 16 * 8 + 7;
    for (unsigned use = 0; use < 16; ++use) {
        for (unsigned list = 0; list < 8; ++list) {
            const Identity page{use | (static_cast<std::uint64_t>(list) << 4), 0};
            counts.add(page, classify(page));
        }
    }
    counts.failed(3);
    assert(counts.notScanned() == 4 && counts.unreadable == 3);
    assert(counts.availablePages == 48 && counts.bad() == 16);
    assert(counts.inUse(Use::Unknown) == 16); // Future use codes stay unknown.
    for (unsigned use = 0; use < 16; ++use) {
        for (unsigned list = 0; list < 8; ++list) {
            assert(counts.unknownByNativeUse[use][list] == (use >= 12 && list >= 2 ? 1U : 0U));
        }
    }
    assert(counts.reconciles()); // Partial scan retains the complete denominator.
    counts.add({~0ULL, 0}, Use::Unknown);
    assert(counts.reconciles() && counts.unreadable == 4 && counts.notScanned() == 3);

    const Identity image{1 | (6ULL << 4), 0xffff800000010001ULL};
    assert(classify(image) == Use::Image);
    assert(classify({0 | (6ULL << 4), 0}, true) == Use::Compression);
    assert(classify({15 | (0ULL << 4), 0}) == Use::Unassigned); // Free-list use bits carry no owner.
    const Identity pinned{5 | (6ULL << 4) | (1ULL << 8) | (1ULL << 60), 0};
    Accounting attributes;
    attributes.expected = 1;
    attributes.add(pinned, classify(pinned));
    assert(attributes.inUse(Use::NonpagedPool) == 1 && attributes.pinned == 1 && attributes.nonTradeable == 1);
    assert(attributes.reconciles()); // Attributes do not create additional pages.
    attributes.expected = 2;
    assert(attributes.notScanned() == 1 && attributes.reconciles());

    // A single unreadable PFN must not discard its 255 readable neighbours.
    std::vector<Identity> recovered;
    std::uint32_t calls = 0;
    const auto sparse = recoverRange(1000, 256, recovered,
        [&](std::uint64_t first, std::uint32_t count, std::vector<Identity>& pages) -> std::int32_t {
            ++calls;
            if (first <= 1033 && 1033 - first < count) { return static_cast<std::int32_t>(0xC000003EU); }
            for (std::uint32_t i = 0; i < count; ++i) { pages.push_back({6ULL << 4, first + i}); }
            return 0;
        }, [] { return false; });
    assert(sparse.initialFailed && sparse.recoveredPages == 255 && calls == sparse.queries + 1);
    assert(recovered.size() == 256 && recovered[33].frame == ~0ULL);
    Accounting salvaged;
    salvaged.expected = recovered.size();
    for (std::size_t i = 0; i < recovered.size(); ++i) {
        if (i != 33) { assert(recovered[i].backing == 1000 + i); }
        salvaged.add(recovered[i], classify(recovered[i]));
    }
    assert(salvaged.valid == 255 && salvaged.unreadable == 1 && salvaged.reconciles());

    // Persistent errors are bounded even when every subquery fails.
    calls = 0;
    const auto bounded = recoverRange(0, 4096, recovered,
        [&](std::uint64_t, std::uint32_t, std::vector<Identity>&) -> std::int32_t {
            ++calls; return static_cast<std::int32_t>(0xC000003EU);
        }, [] { return false; }, 8);
    assert(calls == 9 && bounded.queries == 8 && bounded.recoveredPages == 0 && recovered.size() == 4096);
    for (std::uint32_t status : {0xC0000002U, 0xC0000003U, 0xC0000022U, 0xC0000061U, 0xC000009AU, 0xC00000BBU}) {
        calls = 0;
        const auto fatal = recoverRange(0, 32, recovered,
            [&](std::uint64_t, std::uint32_t, std::vector<Identity>&) -> std::int32_t {
                ++calls; return static_cast<std::int32_t>(status);
            }, [] { return false; });
        assert(calls == 1 && fatal.terminal && fatal.queries == 0 && fatal.recoveredPages == 0);
    }

    // A success status with a truncated payload cannot manufacture readable pages.
    calls = 0;
    const auto truncated = recoverRange(0, 16, recovered,
        [&](std::uint64_t first, std::uint32_t count, std::vector<Identity>& pages) -> std::int32_t {
            ++calls;
            if (count == 1) { pages.push_back({6ULL << 4, first}); }
            return 0;
        }, [] { return false; });
    assert(truncated.initialFailed && truncated.recoveredPages == 16 && calls == 31);
    for (std::size_t i = 0; i < recovered.size(); ++i) { assert(recovered[i].backing == i); }
    calls = 0;
    const auto cancelled = recoverRange(0, 16, recovered,
        [&](std::uint64_t, std::uint32_t, std::vector<Identity>&) -> std::int32_t {
            ++calls; return static_cast<std::int32_t>(0xC000003EU);
        }, [&] { return calls >= 2; });
    assert(cancelled.queried && calls == 2 && cancelled.queries == 1 && recovered.size() == 16);
    const auto preCancelled = recoverRange(0, 16, recovered,
        [&](std::uint64_t, std::uint32_t, std::vector<Identity>&) -> std::int32_t { ++calls; return 0; },
        [] { return true; });
    assert(!preCancelled.queried && calls == 2);

    // Sentinel retries query exact unavailable PFNs and preserve good neighbours.
    recovered = {{6ULL << 4, 100}, {}, {6ULL << 4, 102}, {}};
    calls = 0;
    const auto sentinels = retryUnavailable(100, recovered,
        [&](const std::vector<std::uint64_t>& pfns, std::vector<Identity>& pages) -> std::int32_t {
            ++calls;
            assert((pfns == std::vector<std::uint64_t>{101, 103}));
            pages = {{6ULL << 4, 101}, {}};
            return 0;
        }, [] { return false; });
    assert(calls == 1 && sentinels.queries == 1 && sentinels.recoveredPages == 1);
    assert(recovered[0].backing == 100 && recovered[1].backing == 101 && recovered[2].backing == 102 && recovered[3].frame == ~0ULL);
    const auto shortRetry = retryUnavailable(100, recovered,
        [](const std::vector<std::uint64_t>&, std::vector<Identity>&) -> std::int32_t { return 0; },
        [] { return false; });
    assert(shortRetry.status < 0 && shortRetry.recoveredPages == 0 && recovered[3].frame == ~0ULL);
    const auto stoppedRetry = retryUnavailable(100, recovered,
        [&](const std::vector<std::uint64_t>&, std::vector<Identity>&) -> std::int32_t { ++calls; return 0; },
        [] { return true; });
    assert(stoppedRetry.queries == 0 && calls == 1);
    recovered.pop_back();
    const auto goodRetry = retryUnavailable(100, recovered,
        [&](const std::vector<std::uint64_t>&, std::vector<Identity>&) -> std::int32_t { ++calls; return 0; },
        [] { return false; });
    assert(goodRetry.queries == 0 && calls == 1);

    // An exact-identity ABI denial disables that retry path, while ranged reads
    // still progress. Its diagnostic failure survives later successful batches.
    IdentityRetryPolicy retryPolicy;
    retryPolicy.record({1, 0, static_cast<std::int32_t>(0xC000003EU)});
    assert(retryPolicy.enabled && retryPolicy.lastFailure < 0);
    retryPolicy.record({1, 0, static_cast<std::int32_t>(0xC0000022U)});
    assert(!retryPolicy.enabled && static_cast<std::uint32_t>(retryPolicy.lastFailure) == 0xC0000022U);
    recovered.push_back({});
    const auto disabledRetry = retryUnavailable(100, recovered,
        [&](const std::vector<std::uint64_t>&, std::vector<Identity>&) -> std::int32_t { ++calls; return 0; },
        [&] { return !retryPolicy.enabled; });
    retryPolicy.record(disabledRetry);
    assert(disabledRetry.queries == 0 && calls == 1);
    const auto rangedAfterDenial = recoverRange(200, 2, recovered,
        [](std::uint64_t first, std::uint32_t count, std::vector<Identity>& pages) -> std::int32_t {
            for (std::uint32_t i = 0; i < count; ++i) { pages.push_back({6ULL << 4, first + i}); }
            return 0;
        }, [] { return false; });
    assert(rangedAfterDenial.lastStatus == 0 && recovered.size() == 2 && recovered[1].backing == 201);
    retryPolicy.record({1, 0, 0});
    assert(!retryPolicy.enabled && static_cast<std::uint32_t>(retryPolicy.lastFailure) == 0xC0000022U);

    // Category refinement preserves every physical page and its original state.
    Accounting refined;
    refined.expected = 8;
    std::array<std::uint64_t, 8> store{};
    for (unsigned list = 0; list < 8; ++list) {
        const Identity page{static_cast<std::uint64_t>(list) << 4, 0};
        refined.add(page, classify(page));
        store[list] = inUseState(list) ? 1 : 0;
    }
    refined.reclassify(Use::Private, Use::Compression, store);
    assert(refined.inUse(Use::Compression) == 4 && refined.inUse(Use::Private) == 0 && refined.reconciles());
    assert(refined.availablePages == 3 && refined.bad() == 1);

    // Recycled/contradictory native keys remain unresolved for the entire scan.
    struct TestOwner { std::uint32_t pid = 0; std::string name; bool seenBefore = false, seenAfter = false; };
    std::map<std::uint64_t, TestOwner> owners;
    std::set<std::uint64_t> conflicts;
    const auto same = [](const TestOwner& a, const TestOwner& b) { return a.pid == b.pid && a.name == b.name; };
    mergeOwnerSnapshot(owners, conflicts, 0, TestOwner{100, "redacted"}, false, same);
    assert(owners.empty() && conflicts.empty());
    mergeOwnerSnapshot(owners, conflicts, 10, TestOwner{100, "stable"}, false, same);
    mergeOwnerSnapshot(owners, conflicts, 10, TestOwner{100, "stable"}, true, same);
    assert(owners.at(10).seenBefore && owners.at(10).seenAfter);
    mergeOwnerSnapshot(owners, conflicts, 11, TestOwner{101, "early"}, false, same);
    mergeOwnerSnapshot(owners, conflicts, 12, TestOwner{102, "late"}, true, same);
    assert(owners.at(11).seenBefore && !owners.at(11).seenAfter);
    assert(!owners.at(12).seenBefore && owners.at(12).seenAfter);
    mergeOwnerSnapshot(owners, conflicts, 11, TestOwner{201, "new"}, true, same);
    mergeOwnerSnapshot(owners, conflicts, 11, TestOwner{101, "early"}, true, same);
    assert(!owners.count(11) && conflicts.count(11));
    mergeOwnerSnapshot(owners, conflicts, 12, TestOwner{102, "different"}, true, same);
    assert(!owners.count(12) && conflicts.count(12));

    // Known native type is not a resolved consumer. Available/bad are N/A;
    // pool, locked, Shareable and file keys alone remain owner-unresolved.
    OwnerCoverage coverage;
    coverage.initialize(counts);
    for (std::size_t use = 0; use < useCount; ++use) {
        for (unsigned list = 0; list < 8; ++list) {
            assert(coverage.resolved[use][list] == 0);
            assert(coverage.unresolved[use][list] == (inUseState(list) ? counts.byUseAndState[use][list] : 0));
            assert(coverage.notApplicable[use][list] == (inUseState(list) ? 0 : counts.byUseAndState[use][list]));
        }
    }
    assert(coverage.reconciles(counts));
    assert(coverage.inUseUnresolved(Use::NonpagedPool) == counts.inUse(Use::NonpagedPool));
    assert(coverage.inUseUnresolved(Use::Shareable) == counts.inUse(Use::Shareable));
    coverage.objectKeyKnown[static_cast<std::size_t>(Use::MappedFile)][6] = 1;
    assert(coverage.inUseUnresolved(Use::MappedFile) == counts.inUse(Use::MappedFile) && coverage.reconciles(counts));
    std::array<std::uint64_t, 8> consumer{};
    consumer[6] = 1;
    assert(coverage.resolve(Use::Private, consumer));
    assert(coverage.resolved[static_cast<std::size_t>(Use::Private)][6] == 1 && coverage.reconciles(counts));
    assert(!coverage.resolve(Use::Private, consumer)); // Duplicated evidence cannot over-resolve the denominator.
    assert(coverage.knownInUseUnresolved() + counts.inUse(Use::Unknown) + 1 == 16 * 4);

    // The proposed <=64 MiB criterion requires three distinct, complete epochs
    // with validated semantics in the same build, architecture and domain.
    AuditCriterionSample sample{"domain-A", "epoch-A", "x64", 10, 0, 26100, 64ULL * 1024 * 1024 / pageBytes, 0, 0, true, true, true};
    std::array<AuditCriterionSample, 3> sequence{sample, sample, sample};
    assert(!proposedThreeScanCriterion(sequence));
    sequence[1].epoch = "epoch-B";
    sequence[2].epoch = "epoch-C";
    assert(proposedThreeScanCriterion(sequence));
    sequence[2].unknownInUsePages++;
    assert(!proposedThreeScanCriterion(sequence));
    sequence[2] = sample;
    sequence[2].epoch = "epoch-C";
    sequence[2].semanticsValidated = false;
    assert(!proposedThreeScanCriterion(sequence));
    sequence[2].semanticsValidated = true;
    sequence[2].unreadablePages = 1;
    assert(!proposedThreeScanCriterion(sequence));
    sequence[2].unreadablePages = 0;
    sequence[2].unscannedPages = 1;
    assert(!proposedThreeScanCriterion(sequence));
    sequence[2].unscannedPages = 0;
    sequence[2].reconciles = false;
    assert(!proposedThreeScanCriterion(sequence));
    sequence[2].reconciles = true;
    sequence[2].windowsBuild++;
    assert(!proposedThreeScanCriterion(sequence));
    sequence[2].windowsBuild = sample.windowsBuild;
    sequence[2].domain = "domain-B";
    assert(!proposedThreeScanCriterion(sequence));
    sequence[2].domain = sample.domain;
    sequence[2].architecture = "arm64";
    assert(!proposedThreeScanCriterion(sequence));
    sequence[2].architecture = sample.architecture;
    sequence[2].complete = false;
    assert(!proposedThreeScanCriterion(sequence));

    // Spooling uses one encoded record, checks every byte written, and stops
    // appending on a short disk write; no complete footer can then be claimed.
    RawEvidenceProgress spool;
    std::string disk;
    auto writer = [&](const char* bytes, std::size_t size) -> std::int64_t { disk.append(bytes, size); return static_cast<std::int64_t>(size); };
    const std::string record = "{\"kind\":\"fixture\"}\n";
    for (unsigned chunk = 0; chunk < 10000; ++chunk) { assert(appendRawEvidence(spool, record, writer)); }
    assert(spool.bytes == record.size() * 10000 && spool.records == 10000 && spool.peakEncodedBytes == record.size());
    const auto shortWrite = appendRawEvidence(spool, record,
        [](const char*, std::size_t size) -> std::int64_t { return static_cast<std::int64_t>(size - 1); });
    assert(!shortWrite && spool.failed && !spool.finalized);
    assert(!appendRawEvidence(spool, record, writer) && spool.records == 10000);
    RawEvidenceProgress deniedSpool;
    assert(!appendRawEvidence(deniedSpool, record, [](const char*, std::size_t) -> std::int64_t { return -1; }));
    assert(deniedSpool.failed && deniedSpool.bytes == 0);
    RawEvidenceProgress finalizedSpool;
    finalizedSpool.finalized = true;
    assert(!appendRawEvidence(finalizedSpool, record, writer));
    std::cout << "PFN_ACCOUNTING_TESTS=PASS\n";
}
