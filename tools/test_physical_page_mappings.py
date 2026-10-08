"""Run the production PFN mapping collector against deterministic Windows API mocks.

Only dependency includes are replaced. No target process or driver is opened;
the complete collector and worker publication path execute unchanged.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "Ksword5.1/Ksword5.1/MemoryDock"

STRING = r'''#pragma once
#include <algorithm>
#include <cwctype>
#include <string>
namespace Qt { enum CaseSensitivity { CaseSensitive, CaseInsensitive }; constexpr int ISODateWithMs = 1; }
class QString {
    std::wstring value;
public:
    QString() = default;
    explicit QString(std::wstring input) : value(std::move(input)) {}
    static QString fromWCharArray(const wchar_t* input, int length) { return QString(std::wstring(input, length)); }
    bool isEmpty() const { return value.empty(); }
    std::wstring toStdWString() const { return value; }
    int compare(const QString& other, Qt::CaseSensitivity mode) const {
        auto left = value, right = other.value;
        if (mode == Qt::CaseInsensitive) {
            std::transform(left.begin(), left.end(), left.begin(), ::towlower);
            std::transform(right.begin(), right.end(), right.begin(), ::towlower);
        }
        return left.compare(right);
    }
    bool operator<(const QString& other) const { return value < other.value; }
};
#define QStringLiteral(value) QString(std::wstring(L##value))
'''

DATE = r'''#pragma once
#include "QString"
struct QDateTime {
    static QDateTime currentDateTime() { return {}; }
    QString toString(int) const { return QString(std::wstring(L"sample")); }
};
'''

UUID = r'''#pragma once
#include "QString"
struct QUuid {
    enum { WithoutBraces };
    static QUuid createUuid() { return {}; }
    QString toString(int) const { static unsigned next = 0; return QString(std::to_wstring(++next)); }
};
'''

MOCK = r'''#pragma once
#include <Windows.h>
#include <Psapi.h>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>
constexpr std::size_t KSWORD_ARK_PFN_MAX_MAPPINGS = 256;
struct KSWORD_ARK_PFN_MAPPING { std::uint64_t pfn; std::uint32_t pageSize; long status; };
struct KSWORD_ARK_PFN_IDENTITY { std::uint64_t frame, pfn, backing; };
namespace mock {
struct Region { std::uint64_t base, bytes, allocation; DWORD state, type, protect; std::wstring path; };
struct Page { std::uint64_t pfn, file; std::uint32_t size; unsigned use; bool locked; };
inline std::vector<Region> regions, secondRegions;
inline std::vector<DWORD> pids;
inline std::map<std::uint64_t, Page> pages, secondPages;
inline std::map<std::pair<DWORD, std::uint64_t>, unsigned> translations;
inline std::map<std::uint64_t, unsigned> identities;
inline unsigned opened, closed, pathQueries, exQueries, mappingCalls, identityCalls;
inline bool denyRead, readIdentityChanged, changedPfn, changedFile, changedRegion, invalidAfterTranslate;
inline bool truncatePath, failProbe, failConfirm, malformedRegion, available, unsupported, remapStarted;
inline bool objectsAvailable, regionInfoAvailable, explicitPagefile, contradictoryRegion, regionTruncated;
inline bool objectChanged, wrongView, fieldsMissing, remapForObject, objectStarted;
inline unsigned objectCalls, memorySamples;
inline bool consumersAvailable, tableChanged, tableFieldsMissing, stackChanged, stackSourceMissing, poolChanged, poolMalformed, pidRecycled;
inline bool recycleDuringConsumer, recycledPfn, missingRegionApi;
inline unsigned tableLookupFailureAt, tableWalkFailureAt, tableTransportFailureAt, apiSetLoads;
inline unsigned consumerTranslations, threadQueries, poolQueries, consumerThreadCount, consumerPoolCount;
inline std::map<std::uint64_t, KSWORD_ARK_PFN_IDENTITY> consumerPages;
inline std::map<std::uint64_t, std::uint64_t> kernelMappings;
inline unsigned batchLimit = 256, confirmBatchLimit = 256;
inline DWORD lastError;
inline std::uint64_t initialCreate = 100;
inline const Region* region(std::uint64_t address, DWORD pid = 42) {
    for (const auto& entry : pid == 42 ? regions : secondRegions) if (address >= entry.base && address - entry.base < entry.bytes) return &entry;
    return nullptr;
}
inline auto& processPages(DWORD pid) { return pid == 42 ? pages : secondPages; }
inline DWORD handlePid(HANDLE handle) { return static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(handle) >> 2); }
inline bool readHandle(HANDLE handle) { return (reinterpret_cast<std::uintptr_t>(handle) & 3) == 2; }
inline void reset() {
    opened = closed = pathQueries = exQueries = mappingCalls = identityCalls = 0;
    denyRead = readIdentityChanged = changedPfn = changedFile = changedRegion = invalidAfterTranslate = false;
    truncatePath = failProbe = failConfirm = malformedRegion = unsupported = remapStarted = false;
    objectsAvailable = explicitPagefile = contradictoryRegion = regionTruncated = false; regionInfoAvailable = true;
    objectChanged = wrongView = fieldsMissing = remapForObject = objectStarted = false; objectCalls = memorySamples = 0;
    consumersAvailable = tableChanged = tableFieldsMissing = stackChanged = stackSourceMissing = poolChanged = poolMalformed = pidRecycled = false;
    recycleDuringConsumer = recycledPfn = false;
    tableLookupFailureAt = tableWalkFailureAt = tableTransportFailureAt = 0;
    consumerTranslations = threadQueries = poolQueries = 0; consumerThreadCount = 1; consumerPoolCount = 2; consumerPages.clear(); kernelMappings.clear();
    available = true; batchLimit = confirmBatchLimit = 256; lastError = 0; translations.clear(); identities.clear();
    pids = {42}; secondRegions.clear(); secondPages.clear();
    regions = {{0, 0x1000, 0, MEM_FREE, 0, 0, {}},
        {0x1000, 0x2000, 0x1000, MEM_COMMIT, MEM_PRIVATE, PAGE_READWRITE, {}},
        {0x3000, 0x2000, 0x3000, MEM_RESERVE, MEM_PRIVATE, PAGE_READWRITE, {}},
        {0x5000, 0x4000, 0x5000, MEM_RESERVE, MEM_PRIVATE, PAGE_NOACCESS, {}},
        {0x9000, 0x2000, 0x9000, MEM_COMMIT, MEM_IMAGE, PAGE_READONLY, L"\\Device\\A.dll"},
        {0xb000, 0x1000, 0xb000, MEM_COMMIT, MEM_MAPPED, PAGE_READONLY, L"\\Device\\M.dat"},
        {0xc000, 0x1000, 0xc000, MEM_COMMIT, MEM_IMAGE, PAGE_READONLY, L"\\Device\\A.dll"}};
    pages = {{0x1000, {20, 0, 2 * 1024 * 1024, 0, true}},
        {0x2000, {20, 0, 2 * 1024 * 1024, 0, true}},
        {0x4000, {30, 0, 4096, 9, true}},
        {0x6000, {40, 0, 4096, 0, false}}, // PAGE_NOACCESS reservation must be skipped.
        {0x9000, {50, 0x1234, 4096, 1, false}},
        {0xa000, {60, 0, 4096, 0, false}}, // A private COW image page has no native file key.
        {0xb000, {70, 0x9998, 4096, 8, false}},
        {0xc000, {50, 0x1234, 4096, 1, false}}};
}
}
inline BOOL FakeEnumProcesses(DWORD* pids, DWORD bytes, DWORD* returned) {
    assert(bytes >= mock::pids.size() * sizeof(DWORD)); std::copy(mock::pids.begin(), mock::pids.end(), pids);
    *returned = static_cast<DWORD>(mock::pids.size() * sizeof(DWORD)); return TRUE;
}
inline HANDLE FakeOpenProcess(DWORD access, BOOL, DWORD pid) {
    assert(pid >= 42 && pid < 128 && (access & PROCESS_QUERY_INFORMATION));
    if ((access & PROCESS_VM_READ) && mock::denyRead) { mock::lastError = ERROR_ACCESS_DENIED; return nullptr; }
    ++mock::opened;
    return reinterpret_cast<HANDLE>((std::uintptr_t(pid) << 2) | (access & PROCESS_VM_READ ? 2 : 1));
}
inline BOOL FakeCloseHandle(HANDLE value) { assert(value); ++mock::closed; return TRUE; }
inline BOOL FakeGetProcessTimes(HANDLE value, FILETIME* created, FILETIME*, FILETIME*, FILETIME*) {
    const auto time = mock::initialCreate + (mock::readHandle(value) && mock::readIdentityChanged ? 1 : 0);
    created->dwLowDateTime = static_cast<DWORD>(time); created->dwHighDateTime = static_cast<DWORD>(time >> 32); return TRUE;
}
inline BOOL FakeQueryFullProcessImageNameW(HANDLE, DWORD, wchar_t* path, DWORD* size) {
    const std::wstring value = L"process.exe"; assert(*size > value.size());
    std::copy(value.begin(), value.end(), path); *size = static_cast<DWORD>(value.size()); return TRUE;
}
inline SIZE_T FakeVirtualQueryEx(HANDLE handle, const void* pointer, MEMORY_BASIC_INFORMATION* output, SIZE_T size) {
    assert(size == sizeof(*output));
    const auto* entry = mock::region(reinterpret_cast<std::uint64_t>(pointer), mock::handlePid(handle));
    if (!entry) { mock::lastError = ERROR_INVALID_PARAMETER; return 0; }
    *output = {}; output->BaseAddress = reinterpret_cast<void*>(entry->base);
    output->AllocationBase = reinterpret_cast<void*>(entry->allocation + (mock::changedRegion && !entry->path.empty() ? mock::pathQueries * 0x100000ULL : 0));
    output->RegionSize = mock::malformedRegion ? 0 : entry->bytes;
    output->State = entry->state; output->Type = entry->type; output->AllocationProtect = entry->protect;
    return sizeof(*output);
}
inline DWORD FakeGetLastError() { return mock::lastError; }
inline BOOL FakeGetProcessMemoryInfo(HANDLE, PROCESS_MEMORY_COUNTERS* output, DWORD bytes) {
    assert(bytes == sizeof(PROCESS_MEMORY_COUNTERS_EX)); ++mock::memorySamples;
    auto& value = *reinterpret_cast<PROCESS_MEMORY_COUNTERS_EX*>(output); value = {}; value.cb = bytes;
    value.WorkingSetSize = 1024 * 1024 + mock::memorySamples * 65536;
    value.PrivateUsage = 2 * 1024 * 1024 + mock::memorySamples * 65536;
    return TRUE;
}
struct FakeRegionInfo {
    PVOID AllocationBase; ULONG AllocationProtect;
    ULONG Private : 1, MappedDataFile : 1, MappedImage : 1, MappedPageFile : 1, MappedPhysical : 1, DirectMapped : 1, Reserved : 26;
    SIZE_T RegionSize, CommitSize;
};
inline BOOL WINAPI FakeQueryVirtualMemoryInformation(HANDLE handle, const VOID* address, int kind, PVOID result, SIZE_T size, PSIZE_T returned) {
    assert(kind == 0 && size == sizeof(FakeRegionInfo)); if (!mock::regionInfoAvailable) return FALSE;
    const auto* region = mock::region(reinterpret_cast<std::uint64_t>(address), mock::handlePid(handle)); assert(region);
    auto& output = *static_cast<FakeRegionInfo*>(result); output = {};
    output.AllocationBase = reinterpret_cast<void*>(region->allocation); output.AllocationProtect = region->protect;
    output.RegionSize = region->bytes; output.CommitSize = region->state == MEM_COMMIT ? region->bytes : 0;
    output.Private = region->type == MEM_PRIVATE; output.MappedImage = region->type == MEM_IMAGE;
    output.MappedDataFile = region->type == MEM_MAPPED && !region->path.empty();
    output.MappedPageFile = region->type == MEM_MAPPED && region->path.empty() && mock::explicitPagefile;
    if (mock::contradictoryRegion) { output.MappedDataFile = 1; output.MappedPageFile = 1; }
    *returned = mock::regionTruncated ? size - 1 : size; return TRUE;
}
inline HMODULE FakeGetModuleHandleW(const wchar_t* name) {
    return reinterpret_cast<HMODULE>(std::wstring(name) == L"ntdll.dll" ? 3 : 1);
}
inline HMODULE FakeLoadLibraryExW(const wchar_t* name, HANDLE file, DWORD flags) {
    assert(std::wstring(name) == L"api-ms-win-core-memory-l1-1-4.dll" && !file && flags == LOAD_LIBRARY_SEARCH_SYSTEM32);
    ++mock::apiSetLoads; return mock::missingRegionApi ? nullptr : reinterpret_cast<HMODULE>(2);
}
struct FakePoolEntry { ULONG_PTR addressAndFlags; SIZE_T bytes; ULONG tag; };
struct FakePoolPacket { ULONG count; FakePoolEntry entries[1]; };
inline LONG NTAPI FakeNtQuerySystemInformation(ULONG kind, PVOID output, ULONG bytes, PULONG returned) {
    assert(kind == 66); ++mock::poolQueries;
    if (!mock::consumersAvailable) return static_cast<long>(0xC00000BBUL);
    const auto needed = offsetof(FakePoolPacket, entries) + mock::consumerPoolCount * sizeof(FakePoolEntry);
    *returned = static_cast<ULONG>(needed);
    if (bytes < needed) return static_cast<long>(0xC0000004UL);
    auto* packet = static_cast<FakePoolPacket*>(output); packet->count = mock::poolMalformed ? 0xffffffffUL : mock::consumerPoolCount;
    for (unsigned i = 0; i < mock::consumerPoolCount; ++i) {
        const auto va = 0xffff900000010000ULL + i * 0x10000;
        packet->entries[i] = {va | (i % 2 == 0 ? 1ULL : 0ULL), 8192, 0x41424344UL + (mock::poolChanged && mock::poolQueries > 1 && i == 0 ? 1 : 0)};
    }
    return 0;
}
inline void* FakeGetProcAddress(HMODULE module, const char* name) {
    if (std::string(name) == "NtQuerySystemInformation") return reinterpret_cast<void*>(&FakeNtQuerySystemInformation);
    assert(std::string(name) == "QueryVirtualMemoryInformation");
    return module == reinterpret_cast<HMODULE>(2) ? reinterpret_cast<void*>(&FakeQueryVirtualMemoryInformation) : nullptr;
}
inline BOOL FakeQueryWorkingSet(HANDLE handle, void* output, DWORD bytes) {
    assert(mock::readHandle(handle)); const auto pid = mock::handlePid(handle);
    if (mock::recycledPfn && pid == 43) {
        mock::pages.clear();
        mock::consumerPages[20] = {0 | (6ULL << 4) | (0x2222ULL << 9), 20, 0x2000};
    }
    std::vector<std::uint64_t> addresses;
    for (const auto& entry : mock::processPages(pid)) {
        const auto* region = mock::region(entry.first, pid);
        if (region && region->state == MEM_COMMIT && entry.second.size == 4096 && entry.second.use != 9) addresses.push_back(entry.first);
    }
    if (bytes < sizeof(ULONG_PTR) + addresses.size() * sizeof(PSAPI_WORKING_SET_BLOCK)) { mock::lastError = ERROR_BAD_LENGTH; return FALSE; }
    auto* ws = static_cast<PSAPI_WORKING_SET_INFORMATION*>(output); ws->NumberOfEntries = addresses.size();
    for (std::size_t i = 0; i < addresses.size(); ++i) {
        ws->WorkingSetInfo[i].Flags = 0; ws->WorkingSetInfo[i].VirtualPage = addresses[i] >> 12;
    }
    return TRUE;
}
inline BOOL FakeQueryWorkingSetEx(HANDLE handle, void* output, DWORD bytes) {
    ++mock::exQueries;
    assert(bytes && bytes % sizeof(PSAPI_WORKING_SET_EX_INFORMATION) == 0);
    auto* entries = static_cast<PSAPI_WORKING_SET_EX_INFORMATION*>(output);
    const auto pid = mock::handlePid(handle);
    const bool confirmation = mock::translations[{pid, reinterpret_cast<std::uint64_t>(entries[0].VirtualAddress)}] % 2 != 0;
    if (mock::failProbe && !confirmation) { mock::failProbe = false; return FALSE; }
    if (mock::failConfirm && confirmation) { mock::failConfirm = false; return FALSE; }
    auto& pages = mock::processPages(pid);
    for (std::size_t i = 0; i < bytes / sizeof(*entries); ++i) {
        const auto address = reinterpret_cast<std::uint64_t>(entries[i].VirtualAddress);
        const auto page = pages.find(address);
        entries[i].VirtualAttributes.Flags = 0;
        if (page != pages.end() && !(mock::invalidAfterTranslate && mock::translations[{pid, address}] % 2)) {
            entries[i].VirtualAttributes.Valid = 1;
            entries[i].VirtualAttributes.Locked = page->second.locked;
            entries[i].VirtualAttributes.Shared = mock::recycledPfn ? 0 : 1;
            entries[i].VirtualAttributes.ShareCount = mock::recycledPfn ? 0 : 2;
        }
    }
    return TRUE;
}
inline DWORD FakeGetMappedFileNameW(HANDLE value, void* pointer, wchar_t* output, DWORD size) {
    assert(mock::readHandle(value)); ++mock::pathQueries;
    const auto* entry = mock::region(reinterpret_cast<std::uint64_t>(pointer), mock::handlePid(value));
    assert(entry); mock::remapStarted = true;
    if (entry->path.empty()) { mock::lastError = ERROR_FILE_NOT_FOUND; return 0; }
    if (mock::truncatePath) { std::fill_n(output, size, L'x'); return size; }
    assert(entry->path.size() < size); std::copy(entry->path.begin(), entry->path.end(), output);
    return static_cast<DWORD>(entry->path.size());
}
#undef EnumProcesses
#define EnumProcesses FakeEnumProcesses
#define OpenProcess FakeOpenProcess
#define CloseHandle FakeCloseHandle
#define GetProcessTimes FakeGetProcessTimes
#define QueryFullProcessImageNameW FakeQueryFullProcessImageNameW
#define VirtualQueryEx FakeVirtualQueryEx
#define GetLastError FakeGetLastError
#undef GetProcessMemoryInfo
#define GetProcessMemoryInfo FakeGetProcessMemoryInfo
#define GetModuleHandleW FakeGetModuleHandleW
#define LoadLibraryExW FakeLoadLibraryExW
#define GetProcAddress FakeGetProcAddress
#undef QueryWorkingSet
#define QueryWorkingSet FakeQueryWorkingSet
#undef QueryWorkingSetEx
#define QueryWorkingSetEx FakeQueryWorkingSetEx
#undef GetMappedFileNameW
#define GetMappedFileNameW FakeGetMappedFileNameW
constexpr unsigned KSWORD_ARK_SECTION_FIELD_SECTION_OBJECT_PRESENT = 1, KSWORD_ARK_SECTION_FIELD_CONTROL_AREA_PRESENT = 2;
constexpr unsigned KSWORD_ARK_SECTION_FIELD_MAPPING_LIST_PRESENT = 4, KSWORD_ARK_SECTION_FIELD_REMOTE_MAPPING_UNSUPPORTED = 16;
constexpr unsigned KSWORD_ARK_SECTION_MAP_TYPE_PROCESS = 1, KSWORD_ARK_FILE_SECTION_FIELD_MAPPING_LIST_PRESENT = 16;
constexpr unsigned KSWORD_ARK_FILE_SECTION_FIELD_IMAGE_CONTROL_AREA_PRESENT = 8, KSWORD_ARK_FILE_SECTION_FIELD_DATA_CONTROL_AREA_PRESENT = 4;
constexpr unsigned KSWORD_ARK_FILE_SECTION_KIND_IMAGE = 2, KSWORD_ARK_FILE_SECTION_KIND_DATA = 1;
constexpr unsigned KSWORD_ARK_SECTION_QUERY_FLAG_INCLUDE_ALL = 3, KSWORD_ARK_FILE_SECTION_QUERY_FLAG_INCLUDE_ALL = 3;
constexpr unsigned KSWORD_ARK_MEMORY_FIELD_PML4E_PRESENT = 0x1000, KSWORD_ARK_MEMORY_FIELD_PDPTE_PRESENT = 0x2000;
constexpr unsigned KSWORD_ARK_MEMORY_FIELD_PAGE_TABLE_PRESENT = 0x800;
constexpr unsigned KSWORD_ARK_MEMORY_FIELD_PDE_PRESENT = 0x4000, KSWORD_ARK_MEMORY_FIELD_PTE_PRESENT = 0x8000;
constexpr unsigned KSWORD_ARK_ENUM_THREAD_FLAG_INCLUDE_STACK = 1;
constexpr unsigned KSWORD_ARK_THREAD_DETAIL_FLAG_INCLUDE_IDENTITY = 1, KSWORD_ARK_THREAD_DETAIL_FLAG_INCLUDE_STACK = 8;
constexpr unsigned KSWORD_ARK_THREAD_DETAIL_FIELD_ETHREAD_CID = 4, KSWORD_ARK_THREAD_DETAIL_FIELD_STACK_LIMITS = 128;
constexpr unsigned KSW_DYN_FIELD_SOURCE_PDB_PROFILE = 4;
namespace ksword::ark {
struct Io { bool ok = false; long ntStatus = static_cast<long>(0xC00000BBUL); };
struct View { unsigned viewMapType = 1, processId = 42, sectionKind = 2; std::uint64_t startVa = 0, endVa = 0, controlAreaAddress = 0; };
struct ProcessSection {
    Io io; long lastStatus = 0; unsigned version = 1, queryStatus = 0, fieldFlags = 0, processId = 42;
    std::uint64_t sectionObjectAddress = 0, controlAreaAddress = 0, dynDataCapabilityMask = 0;
    unsigned epSectionObjectOffset = 4, mmSectionControlAreaOffset = 8, mmControlAreaListHeadOffset = 12, mmControlAreaLockOffset = 16;
    std::vector<View> mappings;
};
struct FileSection {
    Io io; long lastStatus = 0; unsigned version = 1, queryStatus = 0, fieldFlags = 0;
    std::uint64_t dataControlAreaAddress = 0, imageControlAreaAddress = 0, dynDataCapabilityMask = 0;
    unsigned mmControlAreaListHeadOffset = 12, mmControlAreaLockOffset = 16;
    std::vector<View> mappings;
};
struct ConsumerTranslation {
    Io io; long lookupStatus = 0, walkStatus = 0; unsigned version = 1, queryStatus = 1, fieldFlags = 0;
    std::uint64_t cr3PhysicalAddress = 0, pml4ePhysicalAddress = 0, pdptePhysicalAddress = 0, pdePhysicalAddress = 0, ptePhysicalAddress = 0, physicalAddress = 0;
    bool resolved = false;
};
struct ConsumerThread { unsigned threadId, processId; };
struct ConsumerThreads { Io io; std::vector<ConsumerThread> entries; };
struct ConsumerThreadSources { unsigned etCid = 4, ktStackLimit = 4, ktStackBase = 4; };
struct ConsumerThreadOffsets { unsigned etCid = 8, ktStackLimit = 16, ktStackBase = 24; };
struct ConsumerThreadResponse {
    long lastStatus = 0; unsigned version = 1, fieldFlags = 132;
    std::uint64_t threadObjectAddress = 0, processObjectAddress = 0xee42, cidUniqueThread = 0, cidUniqueProcess = 42;
    std::uint64_t stackLimit = 0, stackBase = 0;
    ConsumerThreadSources sources;
    ConsumerThreadOffsets offsets;
};
struct ConsumerThreadDetail { Io io; ConsumerThreadResponse response; };
struct DriverClient {
    ConsumerTranslation translateVirtualAddress(unsigned pid, std::uint64_t address) {
        ConsumerTranslation result; if (!mock::consumersAvailable) return result;
        result.io.ok = true; result.io.ntStatus = 0; result.resolved = true;
        if (pid) {
            ++mock::consumerTranslations;
            if (mock::tableLookupFailureAt == mock::consumerTranslations) { result.lookupStatus = static_cast<long>(0xC000000BUL); return result; }
            if (mock::tableWalkFailureAt == mock::consumerTranslations) { result.walkStatus = static_cast<long>(0xC0000005UL); return result; }
            if (mock::tableTransportFailureAt == mock::consumerTranslations) { result.io.ok = false; result.io.ntStatus = static_cast<long>(0xC0000022UL); return result; }
            if (mock::recycleDuringConsumer) mock::pidRecycled = true;
            const auto shifted = mock::tableChanged && mock::consumerTranslations > 2 ? 20ULL : 0;
            result.cr3PhysicalAddress = (500 + shifted) * 4096;
            result.pml4ePhysicalAddress = result.cr3PhysicalAddress + 8;
            result.pdptePhysicalAddress = (501 + shifted) * 4096 + 16;
            result.pdePhysicalAddress = (502 + shifted) * 4096 + 24;
            result.ptePhysicalAddress = (503 + shifted) * 4096 + 32;
            result.fieldFlags = mock::tableFieldsMissing ? 0x800 : 0xf800;
            result.physicalAddress = mock::processPages(pid).at(address).pfn * 4096;
        } else {
            const auto found = mock::kernelMappings.find(address);
            if (found == mock::kernelMappings.end()) { result.resolved = false; }
            else { result.physicalAddress = found->second * 4096; }
        }
        return result;
    }
    ConsumerThreads enumerateThreads(unsigned flags) {
        assert(flags == 1); ConsumerThreads result;
        if (mock::consumersAvailable) {
            result.io.ok = true; result.io.ntStatus = 0;
            for (unsigned i = 0; i < mock::consumerThreadCount; ++i) result.entries.push_back({1001 + i, 42});
        }
        return result;
    }
    ConsumerThreadDetail queryThreadRuntimeDetail(unsigned tid, unsigned pid, unsigned flags) {
        assert(flags == 9 && pid == 42); ++mock::threadQueries;
        ConsumerThreadDetail result; result.io.ok = mock::consumersAvailable; if (result.io.ok) result.io.ntStatus = 0;
        result.response.threadObjectAddress = 0xdd0000 + tid + (mock::stackChanged && mock::threadQueries > 1 ? 1 : 0);
        result.response.cidUniqueThread = tid;
        result.response.stackLimit = 0xffff800000010000ULL + (tid - 1001) * 0x10000;
        result.response.stackBase = result.response.stackLimit + 8192;
        if (mock::stackSourceMissing) result.response.sources.ktStackLimit = 1;
        return result;
    }
    ProcessSection queryProcessSection(unsigned pid) {
        ++mock::objectCalls; mock::objectStarted = true;
        ProcessSection result; result.processId = pid;
        if (mock::objectsAvailable) {
            result.io.ok = true; result.io.ntStatus = 0; result.fieldFlags = 7; result.sectionObjectAddress = 0x1111; result.controlAreaAddress = 0x2222;
            result.mappings = {{1, pid, 2, 0x9000, 0xb000, 0x2222}};
            if (mock::objectChanged && mock::objectCalls % 2 == 0) { ++result.sectionObjectAddress; ++result.controlAreaAddress; }
            if (mock::wrongView) result.mappings[0].processId = 999;
            if (mock::fieldsMissing) result.fieldFlags = 1;
        }
        return result;
    }
    FileSection queryFileSectionMappings(const std::wstring& path) {
        ++mock::objectCalls; mock::objectStarted = true;
        FileSection result;
        if (mock::objectsAvailable) {
            result.io.ok = true; result.io.ntStatus = 0; result.fieldFlags = 28; result.dataControlAreaAddress = 0x4444; result.imageControlAreaAddress = 0x3333;
            if (path == L"\\Device\\A.dll") result.mappings = {{1,42,2,0x9000,0xb000,0x3333},{1,42,2,0xc000,0xd000,0x3333}};
            if (path == L"\\Device\\M.dat") result.mappings = {{1,42,1,0xb000,0xc000,0x4444}};
            if (mock::objectChanged && mock::objectCalls % 2 == 0) {
                ++result.dataControlAreaAddress; ++result.imageControlAreaAddress;
                for (auto& view : result.mappings) ++view.controlAreaAddress;
            }
            if (mock::wrongView) for (auto& view : result.mappings) view.processId = 999;
            if (mock::fieldsMissing) result.fieldFlags = 16;
        }
        return result;
    }
};
struct PfnQueryClient {
    bool mappingAvailable() { return mock::available; }
    long mappings(std::uint32_t pid, std::uint64_t created, const std::vector<std::uint64_t>& addresses,
        std::vector<KSWORD_ARK_PFN_MAPPING>& result) {
        assert(pid >= 42 && pid < 128 && created == mock::initialCreate && !addresses.empty() && addresses.size() <= 256);
        ++mock::mappingCalls; result.clear();
        if (mock::pidRecycled) return static_cast<long>(0xC000000BUL);
        if (mock::unsupported) return static_cast<long>(0xC00000BBUL);
        const auto limit = mock::mappingCalls % 2 ? mock::batchLimit : mock::confirmBatchLimit;
        for (std::size_t i = 0; i < std::min<std::size_t>(addresses.size(), limit); ++i) {
            const auto at = addresses[i]; const auto& pages = mock::processPages(pid);
            const auto found = pages.find(at);
            if (found == pages.end()) { result.push_back({~0ULL, 0, static_cast<long>(0xC0000017UL)}); continue; }
            const unsigned observation = ++mock::translations[{pid, at}];
            result.push_back({found->second.pfn + ((mock::changedPfn && observation % 2 == 0)
                || (mock::remapForObject && mock::objectStarted) ? 100 : 0), found->second.size, 0});
        }
        return 0;
    }
    long identities(const std::vector<std::uint64_t>& pfns, std::vector<KSWORD_ARK_PFN_IDENTITY>& result) {
        ++mock::identityCalls; result.clear();
        for (const auto pfn : pfns) {
            if (mock::consumerPages.count(pfn)) { result.push_back(mock::consumerPages.at(pfn)); continue; }
            const mock::Page* matched = nullptr;
            for (const auto* pages : {&mock::pages, &mock::secondPages}) {
                const auto found = std::find_if(pages->begin(), pages->end(), [pfn](const auto& item) { return item.second.pfn == pfn; });
                if (found != pages->end()) { matched = &found->second; break; }
            }
            assert(matched); const auto& page = *matched;
            const unsigned observation = ++mock::identities[pfn];
            result.push_back({page.use | (6ULL << 4), pfn,
                page.file + (mock::changedFile && page.file && observation % 2 == 0 ? 0x1000 : 0)});
        }
        return 0;
    }
};
}
'''

TEST = r'''
#include "PhysicalPageMappings.cpp"
#include "PhysicalPageConsumers.cpp"
#include <iostream>
#include <cstdlib>
unsigned checks = 0;
void require(bool result, const char* why) {
    ++checks; if (!result) { std::cerr << "FAIL: " << why << '\n'; std::exit(1); }
}
std::shared_ptr<ksword::pfn::Mappings> run(unsigned seconds = 90, bool cancel = false) {
    auto job = std::make_shared<ksword::pfn::MappingJob>(); job->cancel.store(cancel);
    job->ledgerContextEpoch = QString(std::wstring(L"ledger-context"));
    ksword::pfn::collectPhysicalMappings(job, seconds);
    require(job->done.load() && bool(job->result), "worker publishes a completed result");
    require(mock::opened == mock::closed, "every opened process handle closes");
    return job->result;
}
int main(int argc, char**) {
    mock::reset(); mock::missingRegionApi = argc > 1;
    auto result = run();
    require(mock::apiSetLoads == 1, "module-sensitive fixture requires the system API-set fallback");
    if (mock::missingRegionApi) {
        require(std::none_of(result->backing.begin(), result->backing.end(), [](const auto& b) { return b.regionInformationKnown || b.mappedPageFile; }), "missing API remains unknown");
        std::cout << "PHYSICAL_PAGE_MAPPINGS_MISSING_API=PASS\n"; return 0;
    }
    require(!result->epoch.isEmpty() && !result->domain.isEmpty() && !result->finished.isEmpty()
        && result->epoch.compare(result->ledgerContextEpoch, Qt::CaseSensitive) != 0,
        "mapping observation has an independent epoch and a separately labelled ledger context");
    require(result->rows.size() == 7 && result->tested == 7 && result->failed == 0, "all resident committed and AWE pages survive verification");
    require(result->virtualPagesProbed == 8, "free and ordinary no-access reservations are excluded");
    require(result->distinct == 5 && result->large == 1 && result->locked == 2 && result->multiplyMapped == 2,
        "physical attributes count unique PFNs despite aliasing");
    require(result->observerBeforeKnown && result->observerAfterKnown && result->observerMemoryKnown
        && result->observerMemorySamples >= 2 && result->observerMemoryPeakIsSampled
        && result->observerWorkingSetMax >= result->observerWorkingSetBefore && result->observerWorkingSetMax >= result->observerWorkingSetAfter
        && result->observerPrivateMax >= result->observerPrivateBefore && result->observerPrivateMax >= result->observerPrivateAfter,
        "observer endpoint counters retain validity and explicitly sampled maxima");
    require(result->scannedProcesses == 1 && result->workingSetProcesses == 1 && result->regionQueryFailures == 0,
        "ordinary WS completion is distinct from completed supplemental VA traversal");
    require(result->observedFileNames.size() == 2 && result->observedFileNames.count(0x1234) && result->observedFileNames.count(0x9998),
        "only stable native file and metafile identities receive paths");
    for (const auto& row : result->rows) {
        require(row.attributesKnown && row.shareCount == 2 && row.backingIndex < result->backing.size(), "mapping rows retain verified attributes and valid backing indices");
        require(row.address != 0x6000, "ordinary reserved pages never acquire ownership");
        require(row.processCreateTime == 100 && row.pfnRevalidated && row.nativeIdentityKnown
            && row.nativeFrame == row.nativeFrameBefore && row.nativeBacking == row.nativeBackingBefore,
            "mapper edges retain validated process and native PFN identity evidence");
        const auto& backing = result->backing[row.backingIndex];
        require(backing.pid == row.pid && backing.processCreateTime == row.processCreateTime
            && row.address >= backing.regionBase && row.address - backing.regionBase < backing.regionSize,
            "every mapping edge names a process identity and a containing observed allocation region");
    }
    mock::reset(); mock::objectsAvailable = true; result = run();
    require(result->rows.size() == 7 && result->distinct == 5 && result->verifiedObjectRelations == 3,
        "verified object relations enrich observed views without creating physical consumption");
    for (const auto& backing : result->backing) {
        require(!backing.creatorKnown, "current mapper and object identity cannot infer a Section creator");
        if (backing.allocationBase == 0x9000) require(backing.sectionObject == 0x1111 && backing.controlArea == 0x2222
            && backing.objectSource == ksword::pfn::MappingObjectSource::VerifiedMainImageSection
            && backing.objectWitnessPfn == 50 && backing.objectWitnessVa == 0x9000,
            "main-image Section proof retains its exact revalidated witness");
        if (backing.allocationBase == 0x9000) {
            require(bool(backing.objectEvidence), "attempted main-image provider retains bounded proof payload");
            const auto& p = *backing.objectEvidence;
            require(p.before.provider == ksword::pfn::ObjectQueryProvider::ProcessImageSection && p.after.provider == p.before.provider
                && p.before.sectionObject == 0x1111 && p.after.sectionObject == p.before.sectionObject
                && p.before.controlArea == 0x2222 && p.after.controlArea == p.before.controlArea
                && p.before.matchingView && p.after.matchingView && p.before.viewPid == 42 && p.before.queryPid == 42
                && p.before.viewStart == 0x9000 && p.after.viewEnd == 0xb000 && p.before.fieldFlags == 7 && p.before.queryFlags == 3
                && p.before.offsets[0] == 4 && p.before.offsets[1] == 8,
                "raw main-image IDs, matched-view interval, flags and profile offsets replay the object join");
            require(p.witnessBefore.mappingStatus == 0 && p.witnessAfter.mappingStatus == 0
                && p.witnessBefore.pfn == 50 && p.witnessAfter.pfn == 50
                && p.witnessBefore.nativeQueried && p.witnessAfter.nativeQueried
                && p.witnessBefore.nativeFrame == p.witnessAfter.nativeFrame
                && p.witnessBefore.allocationBase == 0x9000 && p.witnessAfter.regionKind == MEM_IMAGE,
                "object witness captures actual mapping, native PFN and allocation proof before and after");
        }
        if (backing.allocationBase == 0xc000) require(backing.sectionObject == 0 && backing.controlArea == 0x3333
            && backing.objectSource == ksword::pfn::MappingObjectSource::VerifiedFileControlArea,
            "file ControlArea proof does not fabricate a Section object address");
        if (backing.allocationBase == 0xc000) {
            const auto& p = *backing.objectEvidence;
            require(p.before.provider == ksword::pfn::ObjectQueryProvider::FileControlArea
                && p.before.sectionObject == 0 && p.after.sectionObject == 0
                && p.before.controlArea == 0x3333 && p.after.controlArea == 0x3333
                && p.before.viewControlArea == 0x3333 && p.after.viewControlArea == 0x3333
                && p.before.viewKind == 2 && p.after.viewPid == 42 && p.before.viewStart == 0xc000 && p.after.viewEnd == 0xd000,
                "file ControlArea raw view fields remain separate from a nonexistent Section identity");
        }
        if (backing.kind == MEM_PRIVATE) require(!backing.objectEvidence, "unattempted allocation nodes retain no per-object proof allocation");
    }
    mock::reset(); mock::objectsAvailable = mock::objectChanged = true; result = run();
    require(result->rows.size() == 7 && result->verifiedObjectRelations == 0, "object replacement between queries suppresses relation proof");
    require(std::any_of(result->backing.begin(), result->backing.end(), [](const auto& backing) {
        return backing.objectEvidence && backing.objectEvidence->before.transportOk && backing.objectEvidence->after.transportOk
            && backing.objectEvidence->before.controlArea != backing.objectEvidence->after.controlArea;
    }), "rejected object replacement still retains the conflicting raw provider responses");
    mock::reset(); mock::objectsAvailable = mock::wrongView = true; result = run();
    require(result->verifiedObjectRelations == 0, "object identity without the same PID and view VA cannot own a mapping");
    mock::reset(); mock::objectsAvailable = mock::fieldsMissing = true; result = run();
    require(result->verifiedObjectRelations == 0, "transport success and handles without required Section/view fields are insufficient");
    mock::reset(); mock::objectsAvailable = mock::remapForObject = true; result = run();
    require(result->rows.size() == 7 && result->verifiedObjectRelations == 0, "a PFN remapped during object observation cannot receive a stale relation");
    mock::reset(); mock::explicitPagefile = true;
    mock::regions[5].path.clear(); mock::pages[0xb000] = {70, 0xf0f0, 4096, 2, false}; result = run();
    const auto shareable = std::find_if(result->rows.begin(), result->rows.end(), [](const auto& row) { return row.pfn == 70; });
    require(shareable != result->rows.end() && shareable->nativeBacking == 0xf0f0 && shareable->nativeFileKey == 0,
        "shareable pages retain opaque native backing evidence rather than a guessed Section or file key");
    const auto& pagefile = result->backing[shareable->backingIndex];
    require(pagefile.regionInformationKnown && pagefile.mappedPageFile && pagefile.path.isEmpty()
        && pagefile.sectionObject == 0 && pagefile.controlArea == 0 && !pagefile.creatorKnown,
        "a closed-handle pagefile view stays observed while Section identity and creator remain unknown");
    require(result->anonymousSectionProviderStatus < 0 && result->cacheFileNameProviderStatus < 0,
        "unsupported anonymous Section and cache-only reverse-key providers remain explicit");
    mock::reset(); mock::regions[5].path.clear(); result = run();
    const auto failedName = std::find_if(result->backing.begin(), result->backing.end(), [](const auto& backing) { return backing.allocationBase == 0xb000; });
    require(failedName != result->backing.end() && !failedName->mappedPageFile
        && failedName->pathStatus == ksword::pfn::MappingPathStatus::Unavailable,
        "a failed mapped-file name query cannot imply a pagefile Section");
    mock::reset(); mock::contradictoryRegion = true; result = run();
    require(std::none_of(result->backing.begin(), result->backing.end(), [](const auto& backing) { return backing.regionInformationKnown; }),
        "contradictory public region flags fail closed without changing resident observations");
    mock::reset(); mock::regionTruncated = true; result = run();
    require(result->rows.size() == 7 && std::none_of(result->backing.begin(), result->backing.end(), [](const auto& backing) { return backing.regionInformationKnown; }),
        "partial region-information structures never classify pagefile mappings");
    mock::reset(); mock::batchLimit = mock::confirmBatchLimit = 1; result = run();
    require(result->rows.size() == 7 && result->tested == 7, "driver partial batches resume the exact unprocessed suffix");
    mock::reset(); mock::confirmBatchLimit = 1; result = run();
    require(result->rows.size() == 7 && result->tested == 7, "a shorter confirmation batch also resumes without skipping addresses");
    mock::reset(); mock::denyRead = true; result = run();
    require(result->rows.size() == 7 && result->inaccessible == 0 && mock::pathQueries == 0,
        "lack of VM_READ reduces filename coverage without losing resident mapping coverage");
    require(result->observedFileNames.empty(), "unavailable file paths remain unnamed");
    mock::reset(); mock::readIdentityChanged = true; result = run();
    require(result->rows.size() == 7 && mock::pathQueries == 0, "a reused PID cannot supply paths from a replacement process handle");
    mock::reset(); mock::changedPfn = true; result = run();
    require(result->rows.empty() && result->changedMappings >= 7 && result->observedFileNames.empty(),
        "changed VA translations suppress both mapping and filename evidence");
    mock::reset(); mock::invalidAfterTranslate = true; result = run();
    require(result->rows.empty() && result->changedMappings >= 7, "pages leaving residency cannot publish stale attributes");
    mock::reset(); mock::changedFile = true; result = run();
    require(result->rows.size() == 7 && result->observedFileNames.empty(), "recycled native file identities do not receive stale paths");
    for (const auto& row : result->rows) {
        if (row.pfn == 50 || row.pfn == 70) require(result->backing[row.backingIndex].path.isEmpty(), "unstable file owners also suppress the per-mapping path");
    }
    mock::reset(); mock::changedRegion = true; result = run();
    require(result->rows.size() == 3 && result->changedMappings >= 4 && result->observedFileNames.empty(),
        "allocation replacement during a path query discards ambiguous mapping observations");
    mock::reset(); mock::truncatePath = true; result = run();
    require(result->rows.size() == 7 && result->observedFileNames.empty(), "truncated paths never become file identities");
    mock::reset(); mock::regions.back().path = L"\\Device\\B.dll"; result = run();
    require(result->rows.size() == 7 && !result->observedFileNames.count(0x1234) && result->conflictingFileKeys == 1,
        "conflicting paths suppress the shared file-key join while preserving physical observations");
    mock::reset(); mock::failProbe = true; result = run();
    require(result->rows.size() == 5 && result->workingSetQueryFailures == 1 && result->distinct == 4,
        "failed resident queries remain explicit coverage gaps");
    mock::reset(); mock::failConfirm = true; result = run();
    require(result->rows.size() == 7 && result->workingSetQueryFailures == 1 && result->failed == 2 && result->changedMappings == 0,
        "a failed attribute recheck is a query failure rather than a claimed mapping change");
    mock::reset(); mock::malformedRegion = true; result = run();
    require(result->rows.empty() && result->regionQueryFailures > 0 && result->scannedProcesses == 0,
        "nonadvancing virtual regions fail closed without an infinite scan");
    mock::reset(); mock::unsupported = true; result = run();
    require(result->rows.empty() && mock::mappingCalls == 1 && result->scannedProcesses == 0,
        "old drivers fail promptly without repetitive mapping calls");
    mock::reset(); result = run(0);
    require(result->budgetReached && result->rows.empty() && mock::opened == 0, "time budget bounds all process work");
    mock::reset(); result = run(90, true);
    require(result->cancelled && result->rows.empty() && mock::opened == 0, "cancellation stops before querying a process");
    mock::reset(); mock::regions.resize(2);
    mock::regions.back().bytes = (16ULL * 1024 * 1024 + 1) * 4096;
    mock::pages.clear(); mock::pages[16ULL * 1024 * 1024 * 4096] = {99, 0, 4096, 0, false};
    result = run();
    require(result->budgetReached && result->virtualPagesProbed == 16ULL * 1024 * 1024,
        "virtual-page probe budget bounds sparse mappings");
    require(result->rows.size() == 1 && result->rows.front().pfn == 99 && result->scannedProcesses == 0,
        "residents discovered in the final allowed probe are retained without claiming complete coverage");
    mock::reset();
    constexpr auto arenaBytes = 64ULL * 1024 * 1024 * 1024;
    constexpr auto lateAddress = 0x1000 + arenaBytes;
    mock::regions = {{0, 0x1000, 0, MEM_FREE, 0, 0, {}},
        {0x1000, arenaBytes, 0x1000, MEM_RESERVE, MEM_PRIVATE, PAGE_READWRITE, {}},
        {lateAddress, 0x1000, lateAddress, MEM_COMMIT, MEM_IMAGE, PAGE_READONLY, L"\\Device\\Late.dll"}};
    mock::pages = {{lateAddress, {50, 0x1234, 4096, 1, false}}};
    result = run();
    require(result->budgetReached && result->rows.size() == 1 && result->rows.front().address == lateAddress,
        "a large sparse reservation cannot suppress later ordinary working-set residents");
    require(result->workingSetProcesses == 1 && result->scannedProcesses == 0 && result->observedFileNames.count(0x1234),
        "baseline file ownership survives exhausted supplemental probe coverage");
    mock::reset(); mock::pids = {42, 43};
    mock::regions = {{0, 0x1000, 0, MEM_FREE, 0, 0, {}},
        {0x1000, arenaBytes, 0x1000, MEM_RESERVE, MEM_PRIVATE, PAGE_READWRITE, {}}};
    mock::pages.clear();
    mock::secondRegions = {{0, 0x1000, 0, MEM_FREE, 0, 0, {}},
        {0x1000, 0x1000, 0x1000, MEM_COMMIT, MEM_IMAGE, PAGE_READONLY, L"\\Device\\Other.dll"}};
    mock::secondPages = {{0x1000, {80, 0x2344, 4096, 1, false}}};
    result = run();
    require(result->budgetReached && result->rows.size() == 1 && result->rows.front().pid == 43,
        "every process ordinary working set has priority over an earlier process sparse reservation");
    require(result->workingSetProcesses == 2 && result->observedFileNames.count(0x2344), "later process baseline owner remains available");
    mock::reset(); mock::pids = {42, 43}; mock::secondRegions = mock::regions; mock::secondPages = mock::pages;
    result = run();
    require(result->rows.size() == 14 && result->distinct == 5 && result->multiplyMapped == 5,
        "PID and VA dedup preserves distinct process mappings without duplicating either pass");
    require(result->workingSetProcesses == 2 && result->scannedProcesses == 2, "both completed coverage stages count processes independently");
    mock::reset(); mock::recycledPfn = true;
    mock::pids = {42, 43};
    mock::regions = {{0,0x1000,0,MEM_FREE,0,0,{}},{0x1000,0x1000,0x1000,MEM_COMMIT,MEM_PRIVATE,PAGE_READWRITE,{}}};
    mock::secondRegions = {{0,0x2000,0,MEM_FREE,0,0,{}},{0x2000,0x1000,0x2000,MEM_COMMIT,MEM_PRIVATE,PAGE_READWRITE,{}}};
    mock::pages = {{0x1000,{20,0,4096,0,false}}}; mock::secondPages = {{0x2000,{20,0,4096,0,false}}};
    mock::consumerPages[20] = {0 | (6ULL << 4) | (0x1111ULL << 9),20,0x1000};
    result = run();
    require(result->rows.size() == 2 && result->distinct == 1, "both sequential PFN observations are retained");
    require(result->rows[0].nativeFrame != result->rows[1].nativeFrame && !result->rows[0].shared && !result->rows[1].shared, "two private generations have incompatible native identities");
    require(result->multiplyMapped == 0 && result->identityConflicted == 1, "recycled PFN does not masquerade as shared alias");
    mock::reset(); mock::available = false; result = run();
    require(!result->driverAvailable && mock::opened == 0, "missing driver cannot produce a mapping result");
    std::cout << "PHYSICAL_PAGE_MAPPINGS_TESTS=PASS checks=" << checks << '\n';
}
'''


def main():
    compiler = shutil.which("g++") or shutil.which("clang++")
    if not compiler:
        raise SystemExit("G++ or Clang++ is required.")
    build_root = ROOT / ".codex-tmp/physical-page-mappings"
    build_root.mkdir(parents=True, exist_ok=True)
    if not build_root.resolve().is_relative_to(ROOT.resolve()):
        raise SystemExit("Mapping test output must remain inside the repository.")
    with tempfile.TemporaryDirectory(prefix="run-", dir=build_root) as temporary:
        build = Path(temporary)
        (build / "QString").write_text(STRING, encoding="utf-8")
        (build / "QDateTime").write_text(DATE, encoding="utf-8")
        (build / "QUuid").write_text(UUID, encoding="utf-8")
        (build / "FakePfnClient.h").write_text(MOCK, encoding="utf-8")
        (build / "PhysicalPageMappings.h").write_text((SOURCE / "PhysicalPageMappings.h").read_text(encoding="utf-8-sig"), encoding="utf-8")
        source = (SOURCE / "PhysicalPageMappings.cpp").read_text(encoding="utf-8-sig")
        source = source.replace('#include "../ArkDriverClient/ArkDriverPfn.h"', '#include "FakePfnClient.h"')
        (build / "PhysicalPageMappings.cpp").write_text(source, encoding="utf-8")
        (build / "PhysicalPageConsumers.h").write_text((SOURCE / "PhysicalPageConsumers.h").read_text(encoding="utf-8-sig"), encoding="utf-8")
        consumer = (SOURCE / "PhysicalPageConsumers.cpp").read_text(encoding="utf-8-sig").replace('#include "../ArkDriverClient/ArkDriverPfn.h"', '#include "FakePfnClient.h"')
        (build / "PhysicalPageConsumers.cpp").write_text(consumer, encoding="utf-8")
        (build / "test.cpp").write_text(TEST, encoding="utf-8")
        exe = build / "physical-page-mapping-tests.exe"
        subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2", "-I", str(build),
            str(build / "test.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=60)
        subprocess.run([str(exe), "--missing-region-api"], check=True, timeout=60)


if __name__ == "__main__":
    main()
