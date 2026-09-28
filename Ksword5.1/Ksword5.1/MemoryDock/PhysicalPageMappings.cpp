#include "PhysicalPageMappings.h"
#include "../ArkDriverClient/ArkDriverPfn.h"
#include <QDateTime>
#include <Psapi.h>
#include <algorithm>
#include <chrono>
#include <map>

namespace ksword::pfn {
namespace {
constexpr std::size_t maxReferences = 2 * 1024 * 1024;
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value) { CloseHandle(value); } }
};

std::shared_ptr<Mappings> collect(const std::shared_ptr<MappingJob>& job, unsigned seconds)
{
    auto result = std::make_shared<Mappings>();
    result->sampledAt = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    ark::PfnQueryClient client;
    result->driverAvailable = client.mappingAvailable();
    if (!result->driverAvailable) { return result; }
    std::vector<DWORD> pids(65536);
    DWORD pidBytes = 0;
    if (!EnumProcesses(pids.data(), static_cast<DWORD>(pids.size() * sizeof(DWORD)), &pidBytes)) {
        result->status = static_cast<long>(0xC0000001UL);
        return result;
    }
    if (pidBytes == pids.size() * sizeof(DWORD)) { result->budgetReached = true; }
    pids.resize(pidBytes / sizeof(DWORD));
    result->processes = static_cast<std::uint32_t>(pids.size());
    auto stopped = [&] {
        if (job->cancel.load()) { result->cancelled = true; return true; }
        if (std::chrono::steady_clock::now() >= deadline || result->rows.size() >= maxReferences) {
            result->budgetReached = true;
            return true;
        }
        return false;
    };
    for (DWORD pid : pids) {
        if (stopped()) { break; }
        Handle process{OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid)};
        if (!process.value) { ++result->inaccessible; continue; }
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetProcessTimes(process.value, &created, &exited, &kernel, &user)) { ++result->inaccessible; continue; }
        const auto creation = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        wchar_t processPath[32768]{};
        DWORD chars = static_cast<DWORD>(std::size(processPath));
        QueryFullProcessImageNameW(process.value, 0, processPath, &chars);
        const QString processName = QString::fromWCharArray(processPath);
        // QueryWorkingSet omits some AWE/large-page mappings. The UI explicitly
        // reports observed mappings, never a complete system reference count.
        std::vector<std::uint64_t> wsStorage(8192);
        bool haveWs = false;
        while (!stopped() && wsStorage.size() * sizeof(std::uint64_t) <= 64ULL * 1024 * 1024) {
            if (QueryWorkingSet(process.value, wsStorage.data(), static_cast<DWORD>(wsStorage.size() * sizeof(std::uint64_t)))) { haveWs = true; break; }
            if (GetLastError() != ERROR_BAD_LENGTH) { break; }
            if (wsStorage.size() * sizeof(std::uint64_t) >= 64ULL * 1024 * 1024) { break; }
            wsStorage.resize(wsStorage.size() * 2);
        }
        if (!haveWs) { ++result->inaccessible; continue; }
        const auto* ws = reinterpret_cast<const PSAPI_WORKING_SET_INFORMATION*>(wsStorage.data());
        if (ws->NumberOfEntries > wsStorage.size() - 1) { ++result->inaccessible; continue; }
        std::vector<std::uint64_t> addresses;
        addresses.reserve(ws->NumberOfEntries);
        for (ULONG_PTR i = 0; i < ws->NumberOfEntries; ++i) { addresses.push_back(ws->WorkingSetInfo[i].VirtualPage << 12); }
        std::sort(addresses.begin(), addresses.end());
        addresses.erase(std::unique(addresses.begin(), addresses.end()), addresses.end());
        std::vector<std::uint64_t>().swap(wsStorage);
        std::map<std::uint64_t, std::uint32_t> backings;
        MEMORY_BASIC_INFORMATION region{};
        std::uint64_t regionEnd = 0;
        for (std::size_t offset = 0; offset < addresses.size() && !stopped(); ) {
            const auto count = std::min<std::size_t>(KSWORD_ARK_PFN_MAX_MAPPINGS, addresses.size() - offset);
            std::vector<std::uint64_t> batch(addresses.begin() + offset, addresses.begin() + offset + count);
            std::vector<KSWORD_ARK_PFN_MAPPING> translated;
            result->status = client.mappings(pid, creation, batch, translated);
            if (result->status < 0) {
                result->failed += addresses.size() - offset;
                // An old driver must not produce thousands of repeated failed calls.
                if (result->status == static_cast<long>(0xC00000BBUL)) { return result; }
                break;
            }
            std::vector<PSAPI_WORKING_SET_EX_INFORMATION> extended(translated.size());
            std::vector<std::uint64_t> pfns;
            std::vector<std::size_t> positions;
            for (std::size_t i = 0; i < translated.size(); ++i) {
                if (translated[i].status >= 0 && translated[i].pfn != ~0ULL) { pfns.push_back(translated[i].pfn); positions.push_back(i); }
            }
            std::vector<KSWORD_ARK_PFN_IDENTITY> identities;
            std::vector<std::uint64_t> fileKeys(translated.size());
            if (!pfns.empty() && client.identities(pfns, identities) >= 0) {
                for (std::size_t i = 0; i < identities.size(); ++i) {
                    const auto use = identities[i].frame & 15;
                    if ((use == 1 || use == 8) && ((identities[i].frame >> 4) & 7) == 6) {
                        fileKeys[positions[i]] = identities[i].backing & ~3ULL;
                    }
                }
            }
            for (std::size_t i = 0; i < extended.size(); ++i) { extended[i].VirtualAddress = reinterpret_cast<void*>(batch[i]); }
            const bool extendedOk = QueryWorkingSetEx(process.value, extended.data(), static_cast<DWORD>(extended.size() * sizeof(extended[0]))) != FALSE;
            for (std::size_t i = 0; i < translated.size(); ++i) {
                ++result->tested;
                if (translated[i].status < 0 || translated[i].pfn == ~0ULL) { ++result->failed; continue; }
                const auto address = batch[i];
                if (address >= regionEnd || address < reinterpret_cast<std::uint64_t>(region.BaseAddress)) {
                    region = {};
                    if (VirtualQueryEx(process.value, reinterpret_cast<void*>(address), &region, sizeof(region)) != sizeof(region)) { regionEnd = 0; }
                    else { regionEnd = reinterpret_cast<std::uint64_t>(region.BaseAddress) + region.RegionSize; }
                }
                const auto allocation = reinterpret_cast<std::uint64_t>(region.AllocationBase);
                auto backing = backings.find(allocation);
                if (backing == backings.end()) {
                    Backing detail{processName, {}, region.Type};
                    if (region.Type == MEM_MAPPED || region.Type == MEM_IMAGE) {
                        wchar_t path[32768]{};
                        const DWORD length = GetMappedFileNameW(process.value, reinterpret_cast<void*>(address), path, static_cast<DWORD>(std::size(path)));
                        if (length) { detail.path = QString::fromWCharArray(path, static_cast<int>(length)); }
                    }
                    backing = backings.emplace(allocation, static_cast<std::uint32_t>(result->backing.size())).first;
                    result->backing.push_back(std::move(detail));
                }
                Mapping row;
                row.pfn = translated[i].pfn;
                row.address = address;
                row.pid = pid;
                row.pageSize = translated[i].pageSize;
                row.backingIndex = backing->second;
                const auto& backingPath = result->backing[row.backingIndex].path;
                if (fileKeys[i] && !backingPath.isEmpty() && result->observedFileNames.size() < 65536) {
                    // Retain an observed path for the native file identity; never join by PFN alone across snapshots.
                    result->observedFileNames.emplace(fileKeys[i], backingPath);
                }
                if (extendedOk && extended[i].VirtualAttributes.Valid) {
                    row.attributesKnown = true;
                    row.locked = extended[i].VirtualAttributes.Locked != 0;
                    row.shared = extended[i].VirtualAttributes.Shared != 0;
                    row.shareCount = static_cast<std::uint32_t>(extended[i].VirtualAttributes.ShareCount);
                }
                result->rows.push_back(row);
                if (result->rows.size() >= maxReferences) { result->budgetReached = true; break; }
            }
            offset += translated.size();
            job->tested.store(result->tested);
        }
        ++result->scannedProcesses;
    }
    std::sort(result->rows.begin(), result->rows.end(), [](const Mapping& a, const Mapping& b) {
        if (a.pfn != b.pfn) { return a.pfn < b.pfn; }
        if (a.pid != b.pid) { return a.pid < b.pid; }
        return a.address < b.address;
    });
    for (std::size_t i = 0; i < result->rows.size(); ) {
        std::size_t end = i + 1;
        while (end < result->rows.size() && result->rows[end].pfn == result->rows[i].pfn) { ++end; }
        bool large = false, locked = false;
        for (std::size_t j = i; j < end; ++j) { large |= result->rows[j].pageSize > 4096; locked |= result->rows[j].locked; }
        ++result->distinct;
        result->large += large ? 1 : 0;
        result->locked += locked ? 1 : 0;
        result->multiplyMapped += end - i > 1 ? 1 : 0;
        i = end;
    }
    return result;
}
}
void collectPhysicalMappings(const std::shared_ptr<MappingJob>& job, unsigned seconds)
{
    std::shared_ptr<Mappings> result;
    try { result = collect(job, seconds); }
    catch (...) {
        try { result = std::make_shared<Mappings>(); result->failedAllocation = true; }
        catch (...) { /* Null also denotes resource failure. */ }
    }
    {
        std::lock_guard<std::mutex> lock(job->mutex);
        job->result = std::move(result);
    }
    job->done.store(true);
}
}
