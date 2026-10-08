#pragma once
#include "PhysicalPageMappings.h"
#include <chrono>

namespace ksword::pfn {
enum class ConsumerKind : std::uint32_t { PageTable, KernelStack, BigPool };
struct TranslationProof {
    bool transportOk = false, resolved = false;
    long ioStatus = static_cast<long>(0xC00000BBUL), lookupStatus = static_cast<long>(0xC00000BBUL), walkStatus = static_cast<long>(0xC00000BBUL);
    std::uint32_t version = 0, queryStatus = 0, fieldFlags = 0;
    std::uint64_t cr3 = 0, physicalAddress = 0;
    std::array<std::uint64_t, 4> entries{};
};
struct StackProof {
    bool transportOk = false;
    long ioStatus = static_cast<long>(0xC00000BBUL), lastStatus = static_cast<long>(0xC00000BBUL);
    std::uint32_t version = 0, fieldFlags = 0;
    std::uint64_t threadObject = 0, processObject = 0, cidThread = 0, cidProcess = 0, limit = 0, base = 0;
    // etCid, ktStackLimit, ktStackBase; retain exact profile source and offsets.
    std::array<std::uint32_t, 3> sources{}, offsets{};
};
struct PoolProof {
    bool matchingAllocation = false;
    std::uint64_t addressAndFlags = 0, bytes = 0;
    std::uint32_t tag = 0;
};
struct PageConsumer {
    ConsumerKind kind = ConsumerKind::PageTable;
    std::uint64_t pfn = 0, nativeFrameBefore = 0, nativeBackingBefore = 0, nativeFrame = 0, nativeBacking = 0;
    std::uint32_t pid = 0, tid = 0, tableLevels = 0, tag = 0;
    std::uint64_t processCreateTime = 0, processWitnessVa = 0, processWitnessPfn = 0;
    std::uint64_t threadObject = 0, allocationVa = 0, allocationBytes = 0, virtualAddress = 0;
    bool processIdentityRevalidated = false, threadObjectRevalidated = false;
    bool threadCreationTimeKnown = false, driverModuleKnown = false;
    TranslationProof translationBefore, translationAfter, translationFinal;
    MappingWitnessProof processWitnessBefore, processWitnessAfter;
    StackProof stackBefore, stackAfter;
    PoolProof poolBefore, poolAfter;
};
struct ConsumerEvidence {
    QString started, finished, domain, epoch, ledgerContextEpoch;
    std::vector<PageConsumer> relations;
    std::uint64_t candidatePages = 0, failed = 0, rejected = 0, distinct = 0;
    std::uint32_t tableProcesses = 0, threads = 0, bigPoolAllocations = 0;
    long tableStatus = static_cast<long>(0xC00000BBUL), stackStatus = static_cast<long>(0xC00000BBUL);
    long bigPoolStatus = static_cast<long>(0xC00000BBUL);
    long bigPoolRecheckStatus = static_cast<long>(0xC00000BBUL);
    bool budgetReached = false, cancelled = false;
    bool threadCreationProviderAvailable = false, lockOwnerProviderAvailable = false;
};
void collectPhysicalConsumers(Mappings& maps, std::atomic_bool& cancel,
    std::chrono::steady_clock::time_point deadline);
}
