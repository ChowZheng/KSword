#include "../../shared/evidence/MemorySnapshotAccounting.h"
#include <cassert>
#include <iostream>

int main()
{
    using namespace ksword::memoryaudit;
    // Permission failures, unnamed sections, disappearing views, and truncated
    // paths all lack backing proof. None is classified as a pagefile section.
    for (const auto error : {0U, 5U, 299U, 487U}) {
        const auto failed = mappedPathOutcome(0, 32768, error);
        assert(!failed.valid && failed.error == error);
        assert(classifyResident(RegionKind::Mapped, true, failed.valid) == ResidentKind::MappedBackingUnknown);
    }
    const auto truncated = mappedPathOutcome(32768, 32768, 0);
    assert(!truncated.valid && truncated.error == 122);
    assert(mappedPathOutcome(12, 32768, 5).valid);
    assert(classifyResident(RegionKind::Mapped, true, true) == ResidentKind::MappedFile);
    assert(classifyResident(RegionKind::Image, true, false) == ResidentKind::Image);
    assert(classifyResident(RegionKind::Mapped, true, false, BackingProof::Pagefile) == ResidentKind::PagefileSection);
    assert(classifyResident(RegionKind::Mapped, true, true, BackingProof::Pagefile) == ResidentKind::MappedBackingUnknown);
    assert(classifyResident(RegionKind::Mapped, false, false, BackingProof::Pagefile) == ResidentKind::PrivateMappedCopy);
    assert(classifyResident(RegionKind::Mapped, true, false, BackingProof::DataFile) == ResidentKind::MappedFile);
    assert(classifyResident(RegionKind::Mapped, true, false, BackingProof::Physical) == ResidentKind::MappedBackingUnknown);
    assert(classifyResident(RegionKind::Mapped, true, true, BackingProof::Physical) == ResidentKind::MappedBackingUnknown);
    for (const auto region : {RegionKind::Image, RegionKind::Mapped}) {
        for (const bool path : {false, true}) {
            assert(classifyResident(region, false, path) == ResidentKind::PrivateMappedCopy);
        }
    }
    assert(classifyResident(RegionKind::Private, false, false) == ResidentKind::Private);
    assert(classifyResident(RegionKind::Unknown, false, false) == ResidentKind::Unknown);

    SystemCommit zero;
    zero.recordPublic(true, 0, 0, 100, 0, 4096);
    assert(zero.valid && zero.bytes == 0 && zero.limit == 409600);
    zero.recordNative(0, 56, 56, 500, 1000, 900);
    assert(zero.source == CommitSource::PerformanceInfo && zero.bytes == 0);
    SystemCommit fallback;
    fallback.recordPublic(false, 5, 123, 456, 789, 4096);
    assert(!fallback.valid && fallback.publicStatus == 5 && fallback.bytes == 0);
    fallback.recordNative(0, 56, 56, 0, 1000, 900);
    assert(fallback.valid && fallback.source == CommitSource::NativeMemoryUsage && fallback.bytes == 0);
    for (const auto shortLength : {0U, 8U, 55U, 57U}) {
        SystemCommit incomplete;
        incomplete.recordPublic(false, 5, 0, 0, 0, 4096);
        incomplete.recordNative(0, shortLength, 56, 0, 1000, 900);
        assert(!incomplete.valid && incomplete.nativeStatus < 0);
    }
    SystemCommit denied;
    denied.recordPublic(false, 5, 0, 0, 0, 4096);
    denied.recordNative(static_cast<std::int32_t>(0xC0000022U), 56, 56, 10, 20, 30);
    assert(!denied.valid && denied.publicAttempted && denied.nativeAttempted);
    SystemCommit overflow;
    overflow.recordPublic(true, 0, (std::numeric_limits<std::uint64_t>::max)(), 1, 1, 4096);
    assert(!overflow.valid && overflow.publicStatus == 534);
    overflow.recordNative(0, 56, 56, 10, 20, 30);
    assert(overflow.valid && overflow.bytes == 10);
    SystemCommit missingPageSize;
    missingPageSize.recordPublic(true, 0, 0, 1, 1, 0);
    assert(!missingPageSize.valid);

    assert(reservedEstimate(true, true, 16000, 15000).bytes == 1000);
    assert(reservedEstimate(true, true, 15000, 15000).valid);
    assert(!reservedEstimate(false, true, 0, 15000).valid);
    assert(!reservedEstimate(true, false, 16000, 0).valid);
    assert(!reservedEstimate(true, true, 14000, 15000).valid);
    std::cout << "MEMORY_SNAPSHOT_ACCOUNTING_TESTS=PASS\n";
}
