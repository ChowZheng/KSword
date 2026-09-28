#include "ArkDriverPfn.h"
#include "../../../third_party/systeminformer_dyn/PfnNative.h"
#include <algorithm>
#include <cstddef>
#include <cstring>

namespace ksword::ark {
namespace {
constexpr long unsupported = static_cast<long>(0xC00000BBUL);
constexpr long invalidData = static_cast<long>(0xC000003EUL);
static_assert(sizeof(KSW_PF_IDENTITY) == sizeof(KSWORD_ARK_PFN_IDENTITY));
static_assert(offsetof(KSW_PF_QUERY, Pages) == 192);
static_assert(sizeof(KSW_PF_PRIVATE_INFO) == 96);
}

PfnQueryClient::PfnQueryClient()
{
    // Enable required privileges on a temporary impersonation token, never on
    // the GUI process token or another worker's token.
    if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY | TOKEN_IMPERSONATE, TRUE, &m_previousToken)
        && GetLastError() != ERROR_NO_TOKEN) { return; }
    if (!ImpersonateSelf(SecurityImpersonation)) { return; }
    m_impersonating = true;
    HANDLE token = nullptr;
    if (OpenThreadToken(GetCurrentThread(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, TRUE, &token)) {
        for (const wchar_t* name : { SE_PROF_SINGLE_PROCESS_NAME, SE_DEBUG_NAME }) {
            TOKEN_PRIVILEGES privileges{};
            privileges.PrivilegeCount = 1;
            if (LookupPrivilegeValueW(nullptr, name, &privileges.Privileges[0].Luid)) {
                privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr);
            }
        }
        CloseHandle(token);
    }
}

PfnQueryClient::~PfnQueryClient()
{
    if (m_impersonating) {
        if (m_previousToken) { SetThreadToken(nullptr, m_previousToken); }
        else { RevertToSelf(); }
    }
    if (m_previousToken) { CloseHandle(m_previousToken); }
}

long PfnQueryClient::nativeQuery(unsigned long kind, void* buffer, unsigned long bytes) const
{
    using Query = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
    const auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    if (!query) { return unsupported; }
    KSW_PF_SUPERFETCH info{};
    info.Version = 45;
    info.Magic = 0x6B756843;
    info.InfoClass = kind;
    info.Buffer = buffer;
    info.Length = bytes;
    return query(79, &info, sizeof(info), nullptr);
}

long PfnQueryClient::driverQuery(unsigned long kind, std::uint64_t first, std::uint32_t count,
    void* records, unsigned long capacity, unsigned long stride, unsigned long& returned,
    const std::vector<std::uint64_t>* pfns)
{
    returned = 0;
    if (!m_driver.isValid()) { m_driver = m_client.openSilently(); }
    if (!m_driver.isValid()) { return unsupported; }
    KSWORD_ARK_PFN_REQUEST request{KSWORD_ARK_PFN_VERSION, kind, count, 0, first};
    std::vector<std::uint64_t> input;
    if (pfns) {
        input.resize(sizeof(request) / sizeof(std::uint64_t) + pfns->size());
        std::memcpy(input.data(), &request, sizeof(request));
        std::copy(pfns->begin(), pfns->end(), input.begin() + sizeof(request) / sizeof(std::uint64_t));
    }
    const std::size_t bytes = sizeof(KSWORD_ARK_PFN_RESPONSE) + static_cast<std::size_t>(capacity) * stride;
    std::vector<std::uint64_t> storage((bytes + 7) / 8);
    auto io = m_client.deviceIoControl(IOCTL_KSWORD_ARK_QUERY_PFN_BATCH,
        pfns ? static_cast<void*>(input.data()) : static_cast<void*>(&request),
        pfns ? static_cast<ULONG>(input.size() * sizeof(std::uint64_t)) : static_cast<ULONG>(sizeof(request)),
        storage.data(), static_cast<unsigned long>(bytes), &m_driver);
    if (!io.ok) { return io.ntStatus < 0 ? io.ntStatus : unsupported; }
    const auto* response = reinterpret_cast<const KSWORD_ARK_PFN_RESPONSE*>(storage.data());
    if (io.bytesReturned < sizeof(*response) || response->version != KSWORD_ARK_PFN_VERSION
        || response->operation != kind || response->count > capacity
        || io.bytesReturned < sizeof(*response) + static_cast<std::size_t>(response->count) * stride) { return invalidData; }
    if (response->nativeStatus < 0) { return response->nativeStatus; }
    returned = response->count;
    std::memcpy(records, response + 1, static_cast<std::size_t>(returned) * stride);
    ++driverBatches;
    return response->nativeStatus;
}

long PfnQueryClient::ranges(std::vector<KSWORD_ARK_PFN_RANGE>& result)
{
    result.clear();
    const auto bytes = static_cast<unsigned long>(offsetof(KSW_PF_RANGES_V2, Ranges)
        + KSWORD_ARK_PFN_MAX_RANGES * sizeof(KSW_PF_RANGE));
    std::vector<std::uint64_t> storage((bytes + 7) / 8);
    auto* query = reinterpret_cast<KSW_PF_RANGES_V2*>(storage.data());
    query->Version = 2;
    long status = nativeQuery(17, query, bytes);
    if (status >= 0 && query->Count > 0 && query->Count <= KSWORD_ARK_PFN_MAX_RANGES) {
        for (SIZE_T i = 0; i < query->Count; ++i) { result.push_back({query->Ranges[i].Base, query->Ranges[i].Count}); }
        ++nativeBatches;
        return status;
    }
    // Older systems support range ABI v1 (two ULONGs before the same records).
    std::fill(storage.begin(), storage.end(), 0);
    auto* fields = reinterpret_cast<ULONG*>(storage.data());
    fields[0] = 1;
    status = nativeQuery(17, fields, bytes);
    if (status >= 0 && fields[1] > 0 && fields[1] <= KSWORD_ARK_PFN_MAX_RANGES) {
        const auto* ranges = reinterpret_cast<const KSW_PF_RANGE*>(fields + 2);
        for (ULONG i = 0; i < fields[1]; ++i) { result.push_back({ranges[i].Base, ranges[i].Count}); }
        ++nativeBatches;
        return status;
    }
    result.resize(KSWORD_ARK_PFN_MAX_RANGES);
    unsigned long count = 0;
    const long nativeStatus = status < 0 ? status : invalidData;
    const long driverStatus = driverQuery(KSWORD_ARK_PFN_RANGES, 0, 0, result.data(), KSWORD_ARK_PFN_MAX_RANGES, sizeof(result[0]), count);
    result.resize(count);
    // An absent/old driver must not hide the original privilege or ABI error.
    return driverStatus == unsupported ? nativeStatus : driverStatus;
}

long PfnQueryClient::owners(std::vector<KSWORD_ARK_PFN_OWNER>& result)
{
    result.clear();
    const auto bytes = static_cast<unsigned long>(offsetof(KSW_PF_PRIVATE_QUERY, Sources)
        + KSWORD_ARK_PFN_MAX_OWNERS * sizeof(KSW_PF_PRIVATE_INFO));
    std::vector<std::uint64_t> storage((bytes + 7) / 8);
    auto* query = reinterpret_cast<KSW_PF_PRIVATE_QUERY*>(storage.data());
    query->Version = 8;
    query->Count = KSWORD_ARK_PFN_MAX_OWNERS;
    long status = nativeQuery(8, query, bytes);
    if (status >= 0 && query->Count <= KSWORD_ARK_PFN_MAX_OWNERS) {
        for (ULONG i = 0; i < query->Count; ++i) {
            const auto& source = query->Sources[i];
            if (source.Source.Type != 2) { continue; }
            KSWORD_ARK_PFN_OWNER owner{};
            owner.processKey = reinterpret_cast<ULONG_PTR>(source.EProcess) & 0xFFFFFFFFFFFFULL;
            owner.processId = source.Source.ProcessId;
            owner.sessionId = source.SessionId;
            std::memcpy(owner.imageName, source.ImageName, sizeof(owner.imageName));
            result.push_back(owner);
        }
        ++nativeBatches;
        return status;
    }
    result.resize(KSWORD_ARK_PFN_MAX_OWNERS);
    unsigned long count = 0;
    const long nativeStatus = status < 0 ? status : invalidData;
    const long driverStatus = driverQuery(KSWORD_ARK_PFN_OWNERS, 0, 0, result.data(), KSWORD_ARK_PFN_MAX_OWNERS, sizeof(result[0]), count);
    result.resize(count);
    return driverStatus == unsupported ? nativeStatus : driverStatus;
}

long PfnQueryClient::pages(std::uint64_t first, std::uint32_t count, std::vector<KSWORD_ARK_PFN_IDENTITY>& result)
{
    result.clear();
    if (count == 0 || count > KSWORD_ARK_PFN_MAX_PAGES || first > (1ULL << 40) - count) { return invalidData; }
    long status = unsupported;
    if (!m_driverPages) {
        const auto bytes = static_cast<unsigned long>(offsetof(KSW_PF_QUERY, Pages) + count * sizeof(KSW_PF_IDENTITY));
        std::vector<std::uint64_t> storage((bytes + 7) / 8);
        auto* query = reinterpret_cast<KSW_PF_QUERY*>(storage.data());
        query->Version = 1;
        query->Count = count;
        for (std::uint32_t i = 0; i < count; ++i) { query->Pages[i] = {~0ULL, first + i, 0}; }
        status = nativeQuery(6, query, bytes);
        if (status >= 0 && query->Count == count) {
            result.resize(count);
            std::memcpy(result.data(), query->Pages, count * sizeof(KSW_PF_IDENTITY));
            ++nativeBatches;
        }
    }
    if (result.empty()) {
        result.resize(count);
        unsigned long returned = 0;
        const long driverStatus = driverQuery(KSWORD_ARK_PFN_PAGES, first, count,
            result.data(), count, sizeof(result[0]), returned);
        result.resize(returned);
        if (driverStatus >= 0) { m_driverPages = true; status = driverStatus; }
        else if (m_driverPages || driverStatus != unsupported) { status = driverStatus; }
    }
    if (status < 0) { result.clear(); return status; }
    if (result.size() != count) { result.clear(); return invalidData; }
    for (std::uint32_t i = 0; i < count; ++i) {
        if (result[i].pfn != first + i) { result.clear(); return invalidData; }
    }
    return status;
}

long PfnQueryClient::identities(const std::vector<std::uint64_t>& pfns, std::vector<KSWORD_ARK_PFN_IDENTITY>& result)
{
    result.clear();
    if (pfns.empty() || pfns.size() > KSWORD_ARK_PFN_MAX_PAGES
        || std::any_of(pfns.begin(), pfns.end(), [](auto pfn) { return pfn >= (1ULL << 40); })) { return invalidData; }
    const auto count = static_cast<ULONG>(pfns.size());
    const auto bytes = static_cast<ULONG>(offsetof(KSW_PF_QUERY, Pages) + count * sizeof(KSW_PF_IDENTITY));
    std::vector<std::uint64_t> storage((bytes + 7) / 8);
    auto* query = reinterpret_cast<KSW_PF_QUERY*>(storage.data());
    query->Version = 1;
    query->Count = count;
    for (ULONG i = 0; i < count; ++i) { query->Pages[i] = {~0ULL, pfns[i], 0}; }
    long status = m_driverPages ? unsupported : nativeQuery(6, query, bytes);
    if (status >= 0 && query->Count == count) {
        result.resize(count);
        std::memcpy(result.data(), query->Pages, count * sizeof(KSW_PF_IDENTITY));
        ++nativeBatches;
    } else {
        result.resize(count);
        unsigned long returned = 0;
        status = driverQuery(KSWORD_ARK_PFN_IDENTITIES, 0, count, result.data(), count, sizeof(result[0]), returned, &pfns);
        result.resize(returned);
    }
    if (status < 0) { result.clear(); return status; }
    if (result.size() != count) { result.clear(); return invalidData; }
    for (ULONG i = 0; i < count; ++i) {
        if (result[i].pfn != pfns[i]) { result.clear(); return invalidData; }
    }
    return status;
}

bool PfnQueryClient::mappingAvailable()
{
    if (!m_driver.isValid()) { m_driver = m_client.openSilently(); }
    return m_driver.isValid();
}

long PfnQueryClient::mappings(std::uint32_t pid, std::uint64_t created,
    const std::vector<std::uint64_t>& addresses, std::vector<KSWORD_ARK_PFN_MAPPING>& result)
{
    result.clear();
    if (addresses.empty() || addresses.size() > KSWORD_ARK_PFN_MAX_MAPPINGS) { return invalidData; }
    if (!mappingAvailable()) { return unsupported; }
    KSWORD_ARK_PFN_MAPPING_REQUEST request{};
    request.version = KSWORD_ARK_PFN_VERSION;
    request.processId = pid;
    request.count = static_cast<ULONG>(addresses.size());
    request.createTime = created;
    std::copy(addresses.begin(), addresses.end(), request.addresses);
    KSWORD_ARK_PFN_MAPPING_RESPONSE response{};
    auto io = m_client.deviceIoControl(IOCTL_KSWORD_ARK_QUERY_PFN_MAPPINGS, &request, sizeof(request),
        &response, sizeof(response), &m_driver);
    if (!io.ok) { return io.ntStatus < 0 ? io.ntStatus : unsupported; }
    if (io.bytesReturned != sizeof(response) || response.version != KSWORD_ARK_PFN_VERSION
        || response.count == 0 || response.count > addresses.size()) { return invalidData; }
    result.assign(response.entries, response.entries + response.count);
    return 0;
}
}
