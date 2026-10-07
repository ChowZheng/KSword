#include "PhysicalPageConsumers.h"
#include "../ArkDriverClient/ArkDriverPfn.h"
#include <QDateTime>
#include <QUuid>
#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <map>
#include <set>
#include <tuple>

namespace ksword::pfn {
namespace {
constexpr long consumerUnsupported = static_cast<long>(0xC00000BBUL);
constexpr long consumerInvalidData = static_cast<long>(0xC000003EUL);
struct ConsumerHandle {
    HANDLE value = nullptr;
    ~ConsumerHandle() { if (value) { CloseHandle(value); } }
};
// SystemBigPoolInformation x64 ABI, already used by the snapshot audit.
// PHNT ntexapi.h: SYSTEM_BIGPOOL_ENTRY / SYSTEM_BIGPOOL_INFORMATION.
struct ConsumerBigPoolEntry { ULONG_PTR addressAndFlags; SIZE_T bytes; ULONG tag; };
struct ConsumerBigPoolPacket { ULONG count; ConsumerBigPoolEntry entries[1]; };
static_assert(sizeof(ConsumerBigPoolEntry) == 24 && offsetof(ConsumerBigPoolPacket, entries) == 8);

template<class Packet>
TranslationProof captureConsumerTranslation(const Packet& packet)
{
    TranslationProof proof;
    proof.transportOk = packet.io.ok; proof.ioStatus = packet.io.ntStatus;
    proof.lookupStatus = packet.lookupStatus; proof.walkStatus = packet.walkStatus;
    proof.version = packet.version; proof.queryStatus = packet.queryStatus; proof.fieldFlags = packet.fieldFlags;
    proof.cr3 = packet.cr3PhysicalAddress; proof.physicalAddress = packet.physicalAddress; proof.resolved = packet.resolved;
    proof.entries = {packet.pml4ePhysicalAddress, packet.pdptePhysicalAddress, packet.pdePhysicalAddress, packet.ptePhysicalAddress};
    return proof;
}
template<class Packet>
StackProof captureConsumerStack(const Packet& packet)
{
    StackProof proof;
    const auto& value = packet.response;
    proof.transportOk = packet.io.ok; proof.ioStatus = packet.io.ntStatus; proof.lastStatus = value.lastStatus;
    proof.version = value.version; proof.fieldFlags = value.fieldFlags;
    proof.threadObject = value.threadObjectAddress; proof.processObject = value.processObjectAddress;
    proof.cidThread = value.cidUniqueThread; proof.cidProcess = value.cidUniqueProcess;
    proof.limit = value.stackLimit; proof.base = value.stackBase;
    proof.sources = {value.sources.etCid, value.sources.ktStackLimit, value.sources.ktStackBase};
    proof.offsets = {value.offsets.etCid, value.offsets.ktStackLimit, value.offsets.ktStackBase};
    return proof;
}

bool consumerPage(const KSWORD_ARK_PFN_IDENTITY& page, unsigned use)
{
    const auto state = (page.frame >> 4) & 7;
    return page.frame != ~0ULL && (page.frame & 15) == use && (state == 3 || state == 4 || state == 6 || state == 7);
}
}

void collectPhysicalConsumers(Mappings& maps, std::atomic_bool& cancel,
    std::chrono::steady_clock::time_point deadline)
{
    auto evidence = std::make_shared<ConsumerEvidence>();
    maps.consumers = evidence;
    evidence->started = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    evidence->domain = QStringLiteral("windows-native-pfn-consumer-observation");
    evidence->epoch = QUuid::createUuid().toString(QUuid::WithoutBraces);
    evidence->ledgerContextEpoch = maps.ledgerContextEpoch;
    ark::PfnQueryClient pfns;
    ark::DriverClient driver;
    auto stopped = [&](bool completing = false) {
        if (cancel.load()) { evidence->cancelled = true; return true; }
        if (std::chrono::steady_clock::now() >= deadline || (!completing && evidence->candidatePages >= 256)) {
            evidence->budgetReached = true; return true;
        }
        return false;
    };
    std::set<std::uint64_t> distinct;
    auto native = [&](std::uint64_t pfn, unsigned use, KSWORD_ARK_PFN_IDENTITY& output) {
        std::vector<KSWORD_ARK_PFN_IDENTITY> pages;
        if (pfns.identities({pfn}, pages) < 0 || pages.size() != 1) { ++evidence->failed; return false; }
        output = pages[0];
        if (!consumerPage(output, use)) { ++evidence->rejected; return false; }
        return true;
    };
    auto append = [&](PageConsumer row, const KSWORD_ARK_PFN_IDENTITY& before, const KSWORD_ARK_PFN_IDENTITY& after) {
        if (before.frame != after.frame || before.backing != after.backing) { ++evidence->rejected; return; }
        row.nativeFrameBefore = before.frame; row.nativeBackingBefore = before.backing;
        row.nativeFrame = after.frame; row.nativeBacking = after.backing;
        distinct.insert(row.pfn);
        evidence->relations.push_back(row);
    };
    auto witness = [&](const Mapping& row, MappingWitnessProof& proof) {
        proof = {};
        std::vector<KSWORD_ARK_PFN_MAPPING> current;
        evidence->tableStatus = pfns.mappings(row.pid, row.processCreateTime, {row.address}, current);
        proof.mappingStatus = evidence->tableStatus;
        if (current.size() == 1) {
            proof.pfn = current[0].pfn; proof.entryStatus = current[0].status; proof.pageSize = current[0].pageSize;
        }
        return evidence->tableStatus >= 0 && current.size() == 1 && current[0].status >= 0 && current[0].pfn == row.pfn;
    };
    std::map<std::uint32_t, const Mapping*> targets;
    for (const auto& row : maps.rows) {
        if (row.pfnRevalidated && row.processCreateTime) {
            if (targets.size() < 64) { targets.emplace(row.pid, &row); }
            else if (!targets.count(row.pid)) { evidence->budgetReached = true; }
        }
    }
    std::uint64_t tableCandidates = 0;
    for (const auto& target : targets) {
        if (stopped() || tableCandidates >= 128) { if (tableCandidates >= 128) { evidence->budgetReached = true; } break; }
        const auto& row = *target.second;
        ConsumerHandle process{OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, row.pid)};
        FILETIME created{}, exited{}, kernel{}, user{};
        MappingWitnessProof initialWitness, afterTranslationWitness;
        if (!process.value || !GetProcessTimes(process.value, &created, &exited, &kernel, &user)
            || ((static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime) != row.processCreateTime
            || !witness(row, initialWitness)) { ++evidence->failed; continue; }
        ++evidence->tableProcesses;
        const auto before = driver.translateVirtualAddress(row.pid, row.address);
        if (!before.io.ok || before.lookupStatus < 0) { evidence->tableStatus = before.io.ntStatus; continue; }
        const auto after = driver.translateVirtualAddress(row.pid, row.address);
        if (!after.io.ok || after.lookupStatus < 0 || !witness(row, afterTranslationWitness)) { ++evidence->failed; continue; }
        const std::array<std::uint64_t, 5> first{before.cr3PhysicalAddress, before.pml4ePhysicalAddress,
            before.pdptePhysicalAddress, before.pdePhysicalAddress, before.ptePhysicalAddress};
        const std::array<std::uint64_t, 5> second{after.cr3PhysicalAddress, after.pml4ePhysicalAddress,
            after.pdptePhysicalAddress, after.pdePhysicalAddress, after.ptePhysicalAddress};
        const std::array<std::uint32_t, 5> flags{KSWORD_ARK_MEMORY_FIELD_PAGE_TABLE_PRESENT, KSWORD_ARK_MEMORY_FIELD_PML4E_PRESENT,
            KSWORD_ARK_MEMORY_FIELD_PDPTE_PRESENT, KSWORD_ARK_MEMORY_FIELD_PDE_PRESENT, KSWORD_ARK_MEMORY_FIELD_PTE_PRESENT};
        std::map<std::uint64_t, std::uint32_t> candidates;
        for (std::size_t level = 0; level < first.size(); ++level) {
            if (first[level] && first[level] == second[level]
                && (!flags[level] || ((before.fieldFlags & flags[level]) && (after.fieldFlags & flags[level])))) {
                candidates[first[level] >> 12] |= 1U << level;
            }
        }
        for (const auto& candidate : candidates) {
            if (stopped() || tableCandidates >= 128) { evidence->budgetReached = true; break; }
            ++tableCandidates; ++evidence->candidatePages;
            KSWORD_ARK_PFN_IDENTITY firstPage{}, lastPage{};
            if (!native(candidate.first, 3, firstPage)) { continue; }
            const auto now = driver.translateVirtualAddress(row.pid, row.address);
            const std::array<std::uint64_t, 5> currentEntries{now.cr3PhysicalAddress, now.pml4ePhysicalAddress,
                now.pdptePhysicalAddress, now.pdePhysicalAddress, now.ptePhysicalAddress};
            bool sameTables = now.io.ok && now.lookupStatus >= 0;
            for (std::size_t level = 0; level < currentEntries.size(); ++level) {
                if (candidate.second & (1U << level)) {
                    sameTables &= currentEntries[level] && (currentEntries[level] >> 12) == candidate.first
                        && (!flags[level] || (now.fieldFlags & flags[level]));
                }
            }
            MappingWitnessProof finalWitness;
            if (!sameTables || !witness(row, finalWitness) || !native(candidate.first, 3, lastPage)) { ++evidence->rejected; continue; }
            PageConsumer consumer;
            consumer.kind = ConsumerKind::PageTable; consumer.pfn = candidate.first;
            consumer.pid = row.pid; consumer.processCreateTime = row.processCreateTime;
            consumer.processWitnessVa = row.address; consumer.processWitnessPfn = row.pfn;
            consumer.virtualAddress = row.address;
            consumer.processIdentityRevalidated = true; consumer.tableLevels = candidate.second;
            consumer.translationBefore = captureConsumerTranslation(before);
            consumer.translationAfter = captureConsumerTranslation(after);
            consumer.translationFinal = captureConsumerTranslation(now);
            consumer.processWitnessBefore = initialWitness; consumer.processWitnessAfter = finalWitness;
            append(consumer, firstPage, lastPage);
        }
    }

    // Active-thread enumeration only: do not enable the expensive CID sweep.
    std::uint64_t stackCandidates = 0;
    if (!stopped()) {
        const auto threads = driver.enumerateThreads(KSWORD_ARK_ENUM_THREAD_FLAG_INCLUDE_STACK);
        evidence->stackStatus = threads.io.ok ? 0 : threads.io.ntStatus;
        for (const auto& thread : threads.entries) {
            if (stopped() || evidence->threads >= 64 || stackCandidates >= 64) {
                evidence->budgetReached = true; break;
            }
            ++evidence->threads;
            const auto detail = driver.queryThreadRuntimeDetail(thread.threadId, thread.processId,
                KSWORD_ARK_THREAD_DETAIL_FLAG_INCLUDE_IDENTITY | KSWORD_ARK_THREAD_DETAIL_FLAG_INCLUDE_STACK);
            const auto& before = detail.response;
            const auto needed = KSWORD_ARK_THREAD_DETAIL_FIELD_ETHREAD_CID | KSWORD_ARK_THREAD_DETAIL_FIELD_STACK_LIMITS;
            auto valid = [&](const auto& packet) {
                const auto& value = packet.response;
                return packet.io.ok && value.lastStatus >= 0 && (value.fieldFlags & needed) == needed
                    && value.threadObjectAddress && value.cidUniqueThread == thread.threadId && value.cidUniqueProcess == thread.processId
                    && value.stackLimit < value.stackBase && value.stackBase - value.stackLimit <= 1024 * 1024
                    && (value.stackLimit & 4095) == 0 && (value.stackBase & 4095) == 0
                    && value.sources.etCid == KSW_DYN_FIELD_SOURCE_PDB_PROFILE
                    && value.sources.ktStackLimit == KSW_DYN_FIELD_SOURCE_PDB_PROFILE && value.sources.ktStackBase == KSW_DYN_FIELD_SOURCE_PDB_PROFILE;
            };
            if (!valid(detail)) { ++evidence->rejected; continue; }
            for (auto va = before.stackLimit; va < before.stackBase && !stopped() && stackCandidates < 64; va += 4096) {
                ++stackCandidates; ++evidence->candidatePages;
                const auto translation = driver.translateVirtualAddress(0, va);
                if (!translation.io.ok || !translation.resolved) { ++evidence->failed; continue; }
                const auto pfn = translation.physicalAddress >> 12;
                KSWORD_ARK_PFN_IDENTITY firstPage{}, lastPage{};
                if (!native(pfn, 11, firstPage)) { continue; }
                const auto rechecked = driver.queryThreadRuntimeDetail(thread.threadId, thread.processId,
                    KSWORD_ARK_THREAD_DETAIL_FLAG_INCLUDE_IDENTITY | KSWORD_ARK_THREAD_DETAIL_FLAG_INCLUDE_STACK);
                const auto translatedAgain = driver.translateVirtualAddress(0, va);
                if (!valid(rechecked) || rechecked.response.threadObjectAddress != before.threadObjectAddress
                    || rechecked.response.processObjectAddress != before.processObjectAddress
                    || rechecked.response.stackLimit != before.stackLimit || rechecked.response.stackBase != before.stackBase
                    || !translatedAgain.io.ok || !translatedAgain.resolved || translatedAgain.physicalAddress != translation.physicalAddress
                    || !native(pfn, 11, lastPage)) { ++evidence->rejected; continue; }
                PageConsumer consumer;
                consumer.kind = ConsumerKind::KernelStack; consumer.pfn = pfn;
                consumer.pid = thread.processId; consumer.tid = thread.threadId; consumer.threadObject = before.threadObjectAddress;
                consumer.allocationVa = before.stackLimit; consumer.allocationBytes = before.stackBase - before.stackLimit;
                consumer.virtualAddress = va;
                consumer.threadObjectRevalidated = true;
                consumer.translationBefore = captureConsumerTranslation(translation);
                consumer.translationAfter = captureConsumerTranslation(translatedAgain);
                consumer.stackBefore = captureConsumerStack(detail); consumer.stackAfter = captureConsumerStack(rechecked);
                append(consumer, firstPage, lastPage);
            }
        }
    }

    using SystemQuery = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
    const auto query = reinterpret_cast<SystemQuery>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    std::vector<std::uint64_t> storage(8192);
    ULONG returned = 0;
    if (query) {
        while (!stopped()) {
            evidence->bigPoolStatus = query(66, storage.data(), static_cast<ULONG>(storage.size() * 8), &returned);
            if (evidence->bigPoolStatus >= 0) { break; }
            if (evidence->bigPoolStatus != static_cast<long>(0xC0000004UL) || storage.size() * 8 >= 8 * 1024 * 1024) { break; }
            const auto next = std::max<std::size_t>(storage.size() * 16, returned);
            if (next > 8 * 1024 * 1024) { evidence->budgetReached = true; break; }
            storage.resize((next + 7) / 8);
        }
    }
    const auto header = offsetof(ConsumerBigPoolPacket, entries);
    if (evidence->bigPoolStatus >= 0 && (returned < header || returned > storage.size() * 8)) { evidence->bigPoolStatus = consumerInvalidData; }
    if (evidence->bigPoolStatus >= 0) {
        const auto* packet = reinterpret_cast<const ConsumerBigPoolPacket*>(storage.data());
        if (packet->count > (returned - header) / sizeof(ConsumerBigPoolEntry)) { evidence->bigPoolStatus = consumerInvalidData; }
        else {
            struct PendingPool { PageConsumer row; KSWORD_ARK_PFN_IDENTITY before, after; };
            std::vector<PendingPool> pending;
            std::uint64_t poolCandidates = 0;
            for (ULONG index = 0; index < packet->count && !stopped() && poolCandidates < 64; ++index) {
                const auto& allocation = packet->entries[index];
                const auto va = static_cast<std::uint64_t>(allocation.addressAndFlags & ~1ULL);
                if (!va || (va & 4095) || !allocation.bytes || allocation.bytes > std::numeric_limits<std::uint64_t>::max() - va) {
                    ++evidence->rejected; continue;
                }
                ++evidence->bigPoolAllocations;
                // Two page samples per allocation are relationship evidence,
                // not a claim that all paged-pool bytes are currently resident.
                for (std::uint64_t offset = 0; offset < std::min<std::uint64_t>(allocation.bytes, 8192) && !stopped() && poolCandidates < 64; offset += 4096) {
                    ++poolCandidates; ++evidence->candidatePages;
                    const auto translated = driver.translateVirtualAddress(0, va + offset);
                    if (!translated.io.ok || !translated.resolved) { ++evidence->failed; continue; }
                    const auto pfn = translated.physicalAddress >> 12;
                    const unsigned use = allocation.addressAndFlags & 1 ? 5 : 4;
                    KSWORD_ARK_PFN_IDENTITY firstPage{}, lastPage{};
                    if (!native(pfn, use, firstPage)) { continue; }
                    const auto after = driver.translateVirtualAddress(0, va + offset);
                    if (!after.io.ok || !after.resolved || after.physicalAddress != translated.physicalAddress || !native(pfn, use, lastPage)) {
                        ++evidence->rejected; continue;
                    }
                    PageConsumer consumer;
                    consumer.kind = ConsumerKind::BigPool; consumer.pfn = pfn;
                    consumer.allocationVa = va; consumer.allocationBytes = allocation.bytes; consumer.tag = allocation.tag;
                    consumer.virtualAddress = va + offset;
                    consumer.poolBefore = PoolProof{true, allocation.addressAndFlags, allocation.bytes, allocation.tag};
                    consumer.translationBefore = captureConsumerTranslation(translated);
                    consumer.translationAfter = captureConsumerTranslation(after);
                    pending.push_back(PendingPool{consumer, firstPage, lastPage});
                }
            }
            if (poolCandidates >= 64) { evidence->budgetReached = true; }
            // Reconcile allocations by VA, nonpaged flag, size and tag against
            // a second complete bounded native packet. A recycled allocation
            // with a different tag must not inherit the former allocation's PFNs.
            if (!pending.empty() && !stopped(true) && query) {
                std::vector<std::uint64_t> afterStorage(storage.size());
                ULONG afterReturned = 0;
                evidence->bigPoolRecheckStatus = query(66, afterStorage.data(), static_cast<ULONG>(afterStorage.size() * 8), &afterReturned);
                if (evidence->bigPoolRecheckStatus >= 0 && (afterReturned < header || afterReturned > afterStorage.size() * 8)) {
                    evidence->bigPoolRecheckStatus = consumerInvalidData;
                }
                if (evidence->bigPoolRecheckStatus >= 0) {
                    const auto* afterPacket = reinterpret_cast<const ConsumerBigPoolPacket*>(afterStorage.data());
                    if (afterPacket->count > (afterReturned - header) / sizeof(ConsumerBigPoolEntry)) {
                        evidence->bigPoolRecheckStatus = consumerInvalidData;
                    } else {
                        using AllocationKey = std::tuple<std::uint64_t, std::uint64_t, std::uint32_t>;
                        std::set<AllocationKey> wanted;
                        std::map<AllocationKey, PoolProof> stillPresent;
                        for (const auto& item : pending) {
                            if (stopped(true)) { break; }
                            wanted.emplace(item.row.allocationVa | ((item.after.frame & 15) == 5 ? 1ULL : 0ULL), item.row.allocationBytes, item.row.tag);
                        }
                        for (ULONG index = 0; index < afterPacket->count; ++index) {
                            if ((index & 4095) == 0 && stopped(true)) { break; }
                            const auto& value = afterPacket->entries[index];
                            const auto key = AllocationKey{value.addressAndFlags, value.bytes, value.tag};
                            if (wanted.count(key)) { stillPresent.emplace(key, PoolProof{true, value.addressAndFlags, value.bytes, value.tag}); }
                        }
                        for (const auto& item : pending) {
                            if (stopped(true)) { break; }
                            const auto key = AllocationKey{item.row.allocationVa | ((item.after.frame & 15) == 5 ? 1ULL : 0ULL), item.row.allocationBytes, item.row.tag};
                            if (!stillPresent.count(key)) { ++evidence->rejected; continue; }
                            KSWORD_ARK_PFN_IDENTITY finalPage{};
                            const auto translatedNow = driver.translateVirtualAddress(0, item.row.virtualAddress);
                            if (!translatedNow.io.ok || !translatedNow.resolved || (translatedNow.physicalAddress >> 12) != item.row.pfn
                                || !native(item.row.pfn, static_cast<unsigned>(item.after.frame & 15), finalPage)
                                || item.after.frame != finalPage.frame || item.after.backing != finalPage.backing) { ++evidence->rejected; continue; }
                            auto retained = item.row;
                            retained.poolAfter = stillPresent.at(key);
                            retained.translationFinal = captureConsumerTranslation(translatedNow);
                            append(retained, item.before, finalPage);
                        }
                    }
                }
            }
        }
    }
    evidence->distinct = distinct.size();
    evidence->finished = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
}
}
