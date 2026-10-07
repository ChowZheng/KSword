"""Compile concrete PFN consumer providers with bounded deterministic API mocks."""
import shutil
import subprocess
import tempfile
from pathlib import Path

from test_physical_page_mappings import ROOT, SOURCE, STRING, DATE, UUID, MOCK

TEST = r'''
#include "PhysicalPageConsumers.cpp"
#include <iostream>
#include <cstdlib>
unsigned checks = 0;
void require(bool condition, const char* message) {
    ++checks; if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
void prepare(unsigned threadCount = 1, unsigned poolCount = 2) {
    mock::reset(); mock::consumersAvailable = true;
    mock::consumerThreadCount = threadCount; mock::consumerPoolCount = poolCount;
    for (unsigned pfn = 500; pfn < 504; ++pfn) mock::consumerPages[pfn] = {3 | (6ULL << 4), pfn, 0};
    for (unsigned thread = 0; thread < threadCount; ++thread) {
        for (unsigned page = 0; page < 2; ++page) {
            const auto va = 0xffff800000010000ULL + thread * 0x10000 + page * 4096;
            const auto pfn = 600 + thread * 2 + page;
            mock::kernelMappings[va] = pfn; mock::consumerPages[pfn] = {11 | (6ULL << 4), pfn, va};
        }
    }
    for (unsigned allocation = 0; allocation < poolCount; ++allocation) {
        for (unsigned page = 0; page < 2; ++page) {
            const auto va = 0xffff900000010000ULL + allocation * 0x10000 + page * 4096;
            const auto pfn = 700 + allocation * 2 + page;
            const auto use = allocation % 2 == 0 ? 5 : 4;
            mock::kernelMappings[va] = pfn; mock::consumerPages[pfn] = {use | (6ULL << 4), pfn, va};
        }
    }
}
ksword::pfn::Mappings mapping(unsigned processes = 1) {
    ksword::pfn::Mappings result;
    result.epoch = QString(std::wstring(L"mapping-epoch"));
    result.ledgerContextEpoch = QString(std::wstring(L"ledger-context"));
    mock::secondPages = mock::pages; mock::secondRegions = mock::regions;
    for (unsigned process = 0; process < processes; ++process) {
        ksword::pfn::Mapping row;
        row.pid = 42 + process; row.processCreateTime = 100; row.address = 0x1000; row.pfn = 20;
        row.pfnRevalidated = true;
        result.rows.push_back(row);
    }
    return result;
}
std::shared_ptr<ksword::pfn::ConsumerEvidence> run(ksword::pfn::Mappings& result, bool cancelled = false, unsigned seconds = 30) {
    std::atomic_bool cancel{cancelled};
    ksword::pfn::collectPhysicalConsumers(result, cancel, std::chrono::steady_clock::now() + std::chrono::seconds(seconds));
    require(mock::opened == mock::closed, "consumer process identity handles always close");
    require(bool(result.consumers), "consumer evidence publishes even if unsupported or cancelled");
    return result.consumers;
}
std::size_t count(const ksword::pfn::ConsumerEvidence& evidence, ksword::pfn::ConsumerKind kind) {
    return std::count_if(evidence.relations.begin(), evidence.relations.end(), [kind](const auto& relation) { return relation.kind == kind; });
}
int main() {
    using namespace ksword::pfn;
    prepare(); auto maps = mapping(); auto result = run(maps);
    require(maps.rows.size() == 1 && maps.rows.front().pfn == 20, "consumer relationships leave mapped PFN evidence unchanged");
    require(count(*result, ConsumerKind::PageTable) == 4 && count(*result, ConsumerKind::KernelStack) == 2
        && count(*result, ConsumerKind::BigPool) == 4, "page tables, PDB stacks and paged/nonpaged Big Pool receive concrete native-validated PFN edges");
    require(result->candidatePages == 10 && result->distinct == 10, "root CR3 and PML4 entry share a single physical-page candidate");
    const auto root = std::find_if(result->relations.begin(), result->relations.end(), [](const auto& relation) { return relation.pfn == 500; });
    require(root != result->relations.end() && root->tableLevels == 3 && root->processIdentityRevalidated
        && root->processCreateTime == 100 && root->processWitnessVa == 0x1000 && root->processWitnessPfn == 20,
        "page-table consumers preserve exact PID, creation identity, witness and observed levels");
    require(root->translationBefore.transportOk && root->translationAfter.transportOk && root->translationFinal.transportOk
        && root->translationBefore.cr3 == 500 * 4096 && root->translationAfter.cr3 == root->translationBefore.cr3
        && root->translationFinal.entries[0] == root->translationBefore.entries[0]
        && root->translationBefore.entries[0] == 500 * 4096 + 8 && root->translationBefore.entries[3] == 503 * 4096 + 32
        && root->translationBefore.fieldFlags == 0xf800 && root->translationFinal.fieldFlags == 0xf800
        && root->processWitnessBefore.pfn == 20 && root->processWitnessAfter.pfn == 20
        && root->processWitnessBefore.mappingStatus == 0 && root->processWitnessAfter.entryStatus == 0,
        "raw CR3, entry addresses, PRESENT bits and process witness packets reproduce table proof");
    require(!result->epoch.isEmpty() && result->epoch.compare(maps.epoch, Qt::CaseSensitive) != 0
        && result->ledgerContextEpoch.compare(maps.ledgerContextEpoch, Qt::CaseSensitive) == 0,
        "consumer query has an independent epoch with immutable ledger context");
    for (const auto& relation : result->relations) {
        require(relation.nativeFrameBefore == relation.nativeFrame && relation.nativeBackingBefore == relation.nativeBacking,
            "each edge retains matching before/after native identity");
        if (relation.kind == ConsumerKind::KernelStack) {
            require(relation.threadObjectRevalidated && !relation.threadCreationTimeKnown,
                "stack object/CID/range proof explicitly lacks thread creation identity");
            require(relation.stackBefore.transportOk && relation.stackAfter.transportOk
                && relation.stackBefore.threadObject == relation.stackAfter.threadObject
                && relation.stackBefore.cidThread == 1001 && relation.stackAfter.cidProcess == 42
                && relation.stackBefore.limit == relation.allocationVa && relation.stackAfter.base - relation.stackAfter.limit == 8192
                && relation.stackBefore.sources == std::array<std::uint32_t,3>{4,4,4}
                && relation.stackAfter.offsets == std::array<std::uint32_t,3>{8,16,24}
                && relation.translationBefore.physicalAddress == relation.pfn * 4096
                && relation.translationAfter.physicalAddress == relation.translationBefore.physicalAddress,
                "raw stack CID, object, ranges, PDB fields and translation payloads reproduce stack proof");
        }
        if (relation.kind == ConsumerKind::BigPool) {
            require(relation.tag == 0x41424344 && relation.allocationBytes == 8192 && !relation.driverModuleKnown,
                "Big Pool allocation and tag do not infer a driver module or allocator");
            require(relation.poolBefore.matchingAllocation && relation.poolAfter.matchingAllocation
                && relation.poolBefore.addressAndFlags == relation.poolAfter.addressAndFlags
                && relation.poolAfter.bytes == 8192 && relation.poolAfter.tag == relation.tag
                && (relation.poolAfter.addressAndFlags & ~1ULL) == relation.allocationVa
                && relation.translationBefore.physicalAddress == relation.pfn * 4096
                && relation.translationAfter.physicalAddress == relation.translationBefore.physicalAddress
                && relation.translationFinal.physicalAddress == relation.translationBefore.physicalAddress,
                "native second-packet allocation flags, size, tag and all physical translations reproduce pool proof");
        }
    }
    prepare(); maps = mapping(2); result = run(maps);
    require(count(*result, ConsumerKind::PageTable) == 8 && result->distinct == 10,
        "two observed address-space consumers retain shared table PFNs without duplicate physical counts");
    prepare(); mock::tableChanged = true; maps = mapping(); result = run(maps);
    require(count(*result, ConsumerKind::PageTable) == 0, "table migration without changing the mapped data PFN suppresses stale table consumers");
    prepare(); mock::tableFieldsMissing = true; maps = mapping(); result = run(maps);
    require(count(*result, ConsumerKind::PageTable) == 1 && result->relations.front().tableLevels == 1,
        "nonzero entry addresses without PRESENT fields cannot become table consumers");
    prepare(); mock::consumerPages[500].frame = 5 | (6ULL << 4); maps = mapping(); result = run(maps);
    require(count(*result, ConsumerKind::PageTable) == 3 && result->rejected > 0,
        "native primary use must agree with observed page-table purpose");
    prepare(); mock::recycleDuringConsumer = true; maps = mapping(); result = run(maps);
    require(count(*result, ConsumerKind::PageTable) == 0, "a PID replaced during translation cannot inherit the old process consumer proof");
    prepare(); mock::stackChanged = true; maps = mapping(); result = run(maps);
    require(count(*result, ConsumerKind::KernelStack) == 0, "thread object replacement prevents stale stack ownership");
    prepare(); mock::stackSourceMissing = true; maps = mapping(); result = run(maps);
    require(count(*result, ConsumerKind::KernelStack) == 0, "stack fields without a precise PDB profile remain unresolved");
    prepare(); mock::consumerPages[600].frame = 10 | (6ULL << 4); maps = mapping(); result = run(maps);
    require(count(*result, ConsumerKind::KernelStack) == 1, "locked pages are not forced into stack purpose by a range overlap");
    prepare(); mock::poolChanged = true; maps = mapping(); result = run(maps);
    require(count(*result, ConsumerKind::BigPool) == 2 && result->rejected >= 2,
        "recycled allocation tags cannot inherit the previous allocation PFN samples");
    prepare(); mock::poolMalformed = true; maps = mapping(); result = run(maps);
    require(count(*result, ConsumerKind::BigPool) == 0 && result->bigPoolStatus < 0,
        "oversized native Big Pool record counts fail before reading entries");
    prepare(64, 40); maps = mapping(64); result = run(maps);
    require(result->candidatePages == 256 && result->relations.size() == 256 && result->budgetReached,
        "the final allowed pool candidates retain their proof without exceeding the shared 256-page discovery cap");
    require(result->threads <= 64 && result->tableProcesses <= 64 && result->distinct < result->relations.size(),
        "bounded consumers and shared PFN references preserve distinct physical accounting");
    prepare(); maps = mapping(); result = run(maps, true);
    require(result->cancelled && result->relations.empty() && result->candidatePages == 0, "cancellation prevents consumer target work");
    prepare(); maps = mapping(); result = run(maps, false, 0);
    require(result->budgetReached && result->relations.empty(), "shared time budget prevents late consumer probing");
    prepare(); mock::consumersAvailable = false; maps = mapping(); result = run(maps);
    require(result->relations.empty() && result->stackStatus < 0 && result->bigPoolStatus < 0,
        "missing providers remain explicit capability gaps");
    std::cout << "PHYSICAL_PAGE_CONSUMERS_TESTS=PASS checks=" << checks << '\n';
}
'''


def main():
    compiler = shutil.which("g++") or shutil.which("clang++")
    if not compiler:
        raise SystemExit("G++ or Clang++ is required.")
    build_root = ROOT / ".codex-tmp/physical-page-consumers"
    build_root.mkdir(parents=True, exist_ok=True)
    if not build_root.resolve().is_relative_to(ROOT.resolve()):
        raise SystemExit("Consumer test output must remain inside the repository.")
    with tempfile.TemporaryDirectory(prefix="run-", dir=build_root) as temporary:
        build = Path(temporary)
        for name, content in {"QString": STRING, "QDateTime": DATE, "QUuid": UUID, "FakePfnClient.h": MOCK}.items():
            (build / name).write_text(content, encoding="utf-8")
        for name in ("PhysicalPageMappings.h", "PhysicalPageConsumers.h"):
            (build / name).write_text((SOURCE / name).read_text(encoding="utf-8-sig"), encoding="utf-8")
        production = (SOURCE / "PhysicalPageConsumers.cpp").read_text(encoding="utf-8-sig")
        production = production.replace('#include "../ArkDriverClient/ArkDriverPfn.h"', '#include "FakePfnClient.h"')
        (build / "PhysicalPageConsumers.cpp").write_text(production, encoding="utf-8")
        (build / "test.cpp").write_text(TEST, encoding="utf-8")
        exe = build / "consumer-tests.exe"
        subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2", "-I", str(build),
            str(build / "test.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=60)


if __name__ == "__main__":
    main()
