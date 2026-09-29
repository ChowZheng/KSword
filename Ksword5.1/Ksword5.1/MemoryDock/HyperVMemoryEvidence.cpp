#include "HyperVMemoryEvidence.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Pdh.h>
#include <PdhMsg.h>
#include <winternl.h>
#include <intrin.h>
#include <QDateTime>
#include <QJsonArray>
#include <QRegularExpression>
#include <algorithm>
#include <array>
#include <cstring>
#include <cwchar>
#include <limits>
#include <map>
#include <set>
#pragma comment(lib, "pdh.lib")

namespace ksword::hyperv {
namespace {
constexpr std::uint64_t page = 4096, mib = 1024 * 1024;
constexpr DWORD maxBuffer = 4 * 1024 * 1024;
constexpr std::size_t maxCounters = 4096;
QString now() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }
bool totalInstance(const QString& name) { return name.compare(QStringLiteral("_Total"), Qt::CaseInsensitive) == 0; }
struct CounterDefinition { Metric metric; const wchar_t* path; std::uint64_t multiplier; };
constexpr CounterDefinition definitions[]{
    {Metric::VidPhysical, L"\\Hyper-V VM Vid Partition(*)\\Physical Pages Allocated", page},
    {Metric::VidRemote, L"\\Hyper-V VM Vid Partition(*)\\Remote Physical Pages", page},
    {Metric::DynamicPhysical, L"\\Hyper-V Dynamic Memory VM(*)\\Physical Memory", mib},
    {Metric::GuestVisible, L"\\Hyper-V Dynamic Memory VM(*)\\Guest Visible Physical Memory", mib},
    {Metric::HypervisorTotal, L"\\Hyper-V Hypervisor\\Total Pages", page},
    {Metric::ChildDeposited, L"\\Hyper-V Hypervisor Partition(*)\\Deposited Pages", page},
    {Metric::RootDeposited, L"\\Hyper-V Hypervisor Root Partition(*)\\Deposited Pages", page},
    {Metric::ChildGpa, L"\\Hyper-V Hypervisor Partition(*)\\GPA Pages", page},
    {Metric::RootGpa, L"\\Hyper-V Hypervisor Root Partition(*)\\GPA Pages", page},
    {Metric::ChildTlb, L"\\Hyper-V Hypervisor Partition(*)\\Virtual TLB Pages", page},
    {Metric::RootTlb, L"\\Hyper-V Hypervisor Root Partition(*)\\Virtual TLB Pages", page},
    {Metric::BalancerAvailable, L"\\Hyper-V Dynamic Memory Balancer(*)\\Available Memory", mib},
    {Metric::BalancerAvailableForBalancing, L"\\Hyper-V Dynamic Memory Balancer(*)\\Available Memory For Balancing", mib}
};
struct Query { PDH_HQUERY handle = nullptr; ~Query() { if (handle) { PdhCloseQuery(handle); } } };
struct Pending { PDH_HCOUNTER handle; CounterDefinition definition; std::size_t index; };

void counters(Snapshot& out, const std::shared_ptr<Job>& job)
{
    Query query;
    const auto opened = PdhOpenQueryW(nullptr, 0, &query.handle);
    if (opened != ERROR_SUCCESS) { out.sources.push_back({QStringLiteral("PDH"), static_cast<std::uint32_t>(opened), 0, false}); return; }
    std::vector<Pending> pending;
    for (const auto& definition : definitions) {
        if (job->stopped()) { break; }
        PDH_HCOUNTER wildcard = nullptr;
        auto status = PdhAddEnglishCounterW(query.handle, definition.path, 0, &wildcard);
        std::vector<std::uint64_t> infoStorage;
        PDH_COUNTER_INFO_W* info = nullptr;
        DWORD infoBytes = 0;
        if (status == ERROR_SUCCESS) {
            status = PdhGetCounterInfoW(wildcard, TRUE, &infoBytes, nullptr);
            if (status == PDH_MORE_DATA && infoBytes <= maxBuffer) {
                infoStorage.resize((infoBytes + 7) / 8);
                info = reinterpret_cast<PDH_COUNTER_INFO_W*>(infoStorage.data());
                status = PdhGetCounterInfoW(wildcard, TRUE, &infoBytes, info);
            }
        }
        QStringList paths;
        if (status == ERROR_SUCCESS && info) {
            if (std::wcschr(definition.path, L'*')) {
                DWORD characters = 0;
                status = PdhExpandWildCardPathW(nullptr, info->szFullPath, nullptr, &characters, PDH_REFRESHCOUNTERS);
                if (status == PDH_MORE_DATA && characters && characters <= maxBuffer / sizeof(wchar_t)) {
                    std::vector<wchar_t> expanded(characters);
                    status = PdhExpandWildCardPathW(nullptr, info->szFullPath, expanded.data(), &characters, PDH_REFRESHCOUNTERS);
                    if (status == ERROR_SUCCESS) {
                        for (const wchar_t* p = expanded.data(); *p; p += std::wcslen(p) + 1) {
                            if (paths.size() >= static_cast<qsizetype>(maxCounters)) { status = PDH_INSUFFICIENT_BUFFER; break; }
                            paths.push_back(QString::fromWCharArray(p));
                        }
                    }
                }
            } else { paths.push_back(QString::fromWCharArray(info->szFullPath)); }
        }
        if (wildcard) { PdhRemoveCounter(wildcard); }
        const auto before = out.counters.size();
        if (status == ERROR_SUCCESS) {
            for (const auto& path : paths) {
                if (job->stopped() || out.counters.size() >= maxCounters) { status = PDH_INSUFFICIENT_BUFFER; break; }
                Counter sample;
                sample.metric = definition.metric;
                sample.path = path;
                if (info && info->szExplainText) { sample.explanation = QString::fromWCharArray(info->szExplainText); }
                DWORD size = 0;
                const auto wide = path.toStdWString();
                auto parsed = PdhParseCounterPathW(wide.c_str(), nullptr, &size, 0);
                if (parsed == PDH_MORE_DATA && size <= maxBuffer) {
                    std::vector<std::uint64_t> storage((size + 7) / 8);
                    auto* elements = reinterpret_cast<PDH_COUNTER_PATH_ELEMENTS_W*>(storage.data());
                    parsed = PdhParseCounterPathW(wide.c_str(), elements, &size, 0);
                    if (parsed == ERROR_SUCCESS && elements->szInstanceName) {
                        sample.instance = QString::fromWCharArray(elements->szInstanceName);
                        if (elements->dwInstanceIndex > 0) { sample.instance += QStringLiteral("#%1").arg(elements->dwInstanceIndex); }
                    }
                }
                PDH_HCOUNTER handle = nullptr;
                auto added = parsed == ERROR_SUCCESS ? PdhAddCounterW(query.handle, wide.c_str(), 0, &handle) : parsed;
                sample.status = static_cast<std::uint32_t>(added);
                out.counters.push_back(std::move(sample));
                if (added == ERROR_SUCCESS) { pending.push_back({handle, definition, out.counters.size() - 1}); }
            }
        }
        if (out.counters.size() == before) {
            Counter missing;
            missing.metric = definition.metric;
            missing.path = QString::fromWCharArray(definition.path);
            missing.status = static_cast<std::uint32_t>(status == ERROR_SUCCESS ? PDH_CSTATUS_NO_INSTANCE : status);
            out.counters.push_back(std::move(missing));
        }
    }
    // Only instantaneous gauges are requested. NOSCALE prevents a provider's
    // suggested display scaling from changing our documented page/MiB units.
    const auto collected = PdhCollectQueryData(query.handle);
    const auto sampledAt = now();
    std::uint32_t valid = 0;
    for (const auto& item : pending) {
        auto& sample = out.counters[item.index];
        sample.sampledAt = sampledAt;
        PDH_FMT_COUNTERVALUE value{};
        auto status = PdhGetFormattedCounterValue(item.handle, PDH_FMT_LARGE | PDH_FMT_NOSCALE, nullptr, &value);
        if (status == ERROR_SUCCESS && value.CStatus != PDH_CSTATUS_VALID_DATA && value.CStatus != PDH_CSTATUS_NEW_DATA) { status = value.CStatus; }
        if (status == ERROR_SUCCESS && (value.largeValue < 0 || static_cast<std::uint64_t>(value.largeValue) > std::numeric_limits<std::uint64_t>::max() / item.definition.multiplier)) { status = PDH_INVALID_DATA; }
        sample.status = static_cast<std::uint32_t>(status);
        if (status == ERROR_SUCCESS) {
            sample.raw = static_cast<std::uint64_t>(value.largeValue);
            sample.bytes = sample.raw * item.definition.multiplier;
            ++valid;
        }
    }
    out.sources.push_back({QStringLiteral("PDH"), static_cast<std::uint32_t>(collected), valid,
        !job->stopped() && valid == out.counters.size()});
}

void processes(Snapshot& out, const std::shared_ptr<Job>& job)
{
    // A single kernel process snapshot also reports protected pseudo-processes
    // such as vmmemWSL without requiring an OpenProcess handle to each one.
    using NativeQuery = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
    const auto query = reinterpret_cast<NativeQuery>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    Source source{QStringLiteral("Process working sets"), static_cast<std::uint32_t>(0xC00000BBUL), 0, false};
    if (!query) { out.sources.push_back(source); return; }
    std::vector<std::uint64_t> storage(1024 * 1024 / 8);
    ULONG length = 0;
    LONG status = static_cast<LONG>(0xC0000004UL);
    for (unsigned attempt = 0; attempt < 8 && !job->stopped(); ++attempt) {
        status = query(5, storage.data(), static_cast<ULONG>(storage.size() * 8), &length);
        if (status != static_cast<LONG>(0xC0000004UL) && status != static_cast<LONG>(0xC0000023UL)) { break; }
        const auto wanted = std::max<std::size_t>(storage.size() * 16, static_cast<std::size_t>(length) + 65536);
        if (wanted > 64ULL * 1024 * 1024) { break; }
        storage.resize((wanted + 7) / 8);
    }
    if (status >= 0 && length >= sizeof(SYSTEM_PROCESS_INFORMATION) && length <= storage.size() * 8) {
        const auto begin = reinterpret_cast<std::uintptr_t>(storage.data());
        std::size_t offset = 0;
        while (offset + sizeof(SYSTEM_PROCESS_INFORMATION) <= length && !job->stopped()) {
            const auto* entry = reinterpret_cast<const SYSTEM_PROCESS_INFORMATION*>(reinterpret_cast<const unsigned char*>(storage.data()) + offset);
            const auto nameAddress = reinterpret_cast<std::uintptr_t>(entry->ImageName.Buffer);
            if (entry->ImageName.Length % sizeof(wchar_t) != 0 || nameAddress < begin || nameAddress - begin > length || entry->ImageName.Length > length - (nameAddress - begin)) {
                if (entry->ImageName.Length) { status = static_cast<LONG>(0xC000003EUL); break; }
            }
            const auto name = entry->ImageName.Length ? QString::fromWCharArray(entry->ImageName.Buffer, entry->ImageName.Length / sizeof(wchar_t)) : QString();
            const auto lower = name.toLower();
            if (lower.startsWith(QStringLiteral("vmmem")) || lower == QStringLiteral("vmwp.exe") || lower == QStringLiteral("vmcompute.exe") || lower == QStringLiteral("wslservice.exe")) {
                Process row;
                row.pid = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(entry->UniqueProcessId));
                row.name = name; row.workingSet = entry->WorkingSetSize; row.privateCommit = entry->PrivatePageCount;
                out.processes.push_back(std::move(row));
            }
            if (!entry->NextEntryOffset) { source.complete = true; break; }
            if (entry->NextEntryOffset < sizeof(SYSTEM_PROCESS_INFORMATION) || entry->NextEntryOffset > length - offset) { status = static_cast<LONG>(0xC000003EUL); break; }
            offset += entry->NextEntryOffset;
        }
    } else if (status >= 0) { status = static_cast<LONG>(0xC000003EUL); }
    source.status = static_cast<std::uint32_t>(status);
    source.rows = static_cast<std::uint32_t>(out.processes.size());
    source.complete = source.complete && !job->stopped() && status >= 0;
    out.sources.push_back(std::move(source));
}
void put(QJsonObject& object, const char* key, Bytes value)
{
    object.insert(QString::fromLatin1(key), value ? QJsonValue(QString::number(*value)) : QJsonValue(QJsonValue::Null));
}
}

const char* metricName(Metric metric)
{
    constexpr const char* names[]{"vid_physical", "vid_remote", "dynamic_physical", "guest_visible", "hypervisor_total",
        "child_deposited", "root_deposited", "child_gpa", "root_gpa", "child_tlb", "root_tlb", "balancer_available", "balancer_available_for_balancing"};
    return names[static_cast<unsigned>(metric)];
}
QString canonicalGuid(QString text)
{
    text = text.trimmed();
    if (text.startsWith(QLatin1Char('{')) && text.endsWith(QLatin1Char('}'))) { text = text.mid(1, text.size() - 2); }
    static const QRegularExpression guid(QStringLiteral("^([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})$"));
    const auto match = guid.match(text);
    return match.hasMatch() ? match.captured(1).toLower() : QString();
}

void correlate(Snapshot& out)
{
    out.partitions.clear();
    out.observedVidBytes.reset(); out.vidTotalBytes.reset(); out.unresolvedVidBytes.reset(); out.vidConflict = false;
    // Merge WMI and HCS only by exact identity, including HCS runtime GUID aliases.
    for (const auto& source : out.inventory) {
        const auto id = canonicalGuid(source.id);
        const auto runtime = canonicalGuid(source.runtimeId);
        auto match = out.partitions.end();
        for (auto it = out.partitions.begin(); it != out.partitions.end(); ++it) {
            if ((!id.isEmpty() && it->aliases.contains(id)) || (!runtime.isEmpty() && it->aliases.contains(runtime))) {
                if (match != out.partitions.end()) { match = out.partitions.end(); break; }
                match = it;
            }
        }
        if (match == out.partitions.end()) {
            Partition row;
            static_cast<Inventory&>(row) = source;
            row.key = id.isEmpty() ? QStringLiteral("hcs:") + source.id : id;
            if (!id.isEmpty()) { row.aliases.push_back(id); }
            if (!runtime.isEmpty() && !row.aliases.contains(runtime)) { row.aliases.push_back(runtime); }
            row.inventoryMatched = true;
            out.partitions.push_back(std::move(row));
        } else {
            auto& row = *match;
            if (!id.isEmpty() && !row.aliases.contains(id)) { row.aliases.push_back(id); }
            if (!runtime.isEmpty() && !row.aliases.contains(runtime)) { row.aliases.push_back(runtime); }
            if (source.hcs) { row.owner = source.owner; row.type = source.type; row.state = source.state; row.runtimeId = source.runtimeId; row.hostingSystemId = source.hostingSystemId; row.memoryEvidence = source.memoryEvidence; }
            if (!source.name.isEmpty() && (row.name.isEmpty() || source.wmi || canonicalGuid(source.name).isEmpty())) { row.name = source.name; }
            row.hcs |= source.hcs; row.wmi |= source.wmi;
            if (source.workerPid) { row.workerPid = source.workerPid; }
            if (source.wmiCapacity) { row.wmiCapacity = source.wmiCapacity; }
            if (source.hcsNodeBytes) { row.hcsNodeBytes = source.hcsNodeBytes; }
            if (source.hcsPrivateWs) { row.hcsPrivateWs = source.hcsPrivateWs; }
            if (source.hcsCommit) { row.hcsCommit = source.hcsCommit; }
        }
    }
    for (std::size_t index = 0; index < out.counters.size(); ++index) {
        const auto& sample = out.counters[index];
        if (!sample.bytes) { continue; }
        if (totalInstance(sample.instance)) { if (sample.metric == Metric::VidPhysical) { out.vidTotalBytes = sample.bytes; } continue; }
        if (sample.metric != Metric::VidPhysical && sample.metric != Metric::VidRemote && sample.metric != Metric::DynamicPhysical
            && sample.metric != Metric::GuestVisible && sample.metric != Metric::ChildDeposited && sample.metric != Metric::ChildGpa && sample.metric != Metric::ChildTlb) { continue; }
        QString instance = sample.instance;
        if (instance.endsWith(QStringLiteral(":Hvpt"), Qt::CaseInsensitive)) { instance.chop(5); }
        const auto id = canonicalGuid(instance);
        std::vector<std::size_t> matches;
        for (std::size_t i = 0; i < out.partitions.size(); ++i) {
            const auto& row = out.partitions[i];
            if ((!id.isEmpty() && (row.aliases.contains(id) || id == row.id)) ||
                (id.isEmpty() && row.inventoryMatched && !instance.isEmpty() && instance.compare(row.name, Qt::CaseInsensitive) == 0)) { matches.push_back(i); }
        }
        std::size_t target;
        // Conflicting GUID aliases may refer to the same physical partition.
        // Preserve raw instances but withhold the sum instead of counting twice.
        if (!id.isEmpty() && matches.size() > 1) { out.vidConflict = true; }
        if (matches.size() == 1) { target = matches.front(); }
        else {
            const QString key = !id.isEmpty() ? id : QStringLiteral("counter:") + instance.toLower();
            auto existing = std::find_if(out.partitions.begin(), out.partitions.end(), [&](const Partition& row) { return !row.inventoryMatched && row.key == key; });
            if (existing != out.partitions.end()) { target = static_cast<std::size_t>(existing - out.partitions.begin()); }
            else {
                Partition row;
                row.key = key; row.id = id; row.counterInstance = sample.instance; row.ambiguous = matches.size() > 1;
                out.partitions.push_back(std::move(row)); target = out.partitions.size() - 1;
            }
        }
        auto& row = out.partitions[target];
        row.counterIndices.push_back(index);
        if (sample.metric == Metric::VidPhysical) {
            if (row.vidBytes) { row.conflictingVid = true; out.vidConflict = true; }
            else { row.vidBytes = sample.bytes; row.counterInstance = sample.instance; }
        } else if (sample.metric == Metric::DynamicPhysical) { row.dynamicBytes = sample.bytes; }
        else if (sample.metric == Metric::GuestVisible) { row.guestVisibleBytes = sample.bytes; }
        else if (sample.metric == Metric::ChildDeposited) { row.depositedBytes = sample.bytes; }
    }
    std::uint64_t sum = 0, unresolved = 0;
    bool any = false;
    for (const auto& row : out.partitions) {
        if (!row.vidBytes || row.conflictingVid) { continue; }
        if (*row.vidBytes > std::numeric_limits<std::uint64_t>::max() - sum) { out.vidConflict = true; break; }
        sum += *row.vidBytes; any = true;
        if (!row.inventoryMatched || row.ambiguous) { unresolved += *row.vidBytes; }
    }
    if (any && !out.vidConflict) { out.observedVidBytes = sum; out.unresolvedVidBytes = unresolved; }
    // Container statistics, guest capacity, _Total, and deposited pages are
    // corroborating/overlapping views and never added to this VID-only aggregate.
    std::stable_sort(out.partitions.begin(), out.partitions.end(), [](const Partition& a, const Partition& b) { return a.vidBytes.value_or(0) > b.vidBytes.value_or(0); });
}

QJsonObject toJson(const Snapshot& snapshot, const Context& context)
{
    QJsonObject root{{QStringLiteral("schema"), 1}, {QStringLiteral("started"), snapshot.started}, {QStringLiteral("finished"), snapshot.finished},
        {QStringLiteral("hypervisor_vendor"), snapshot.hypervisorVendor}, {QStringLiteral("hypervisor_present"), snapshot.hypervisorPresent},
        {QStringLiteral("cancelled"), snapshot.cancelled}, {QStringLiteral("timed_out"), snapshot.timedOut}, {QStringLiteral("resource_failure"), snapshot.resourceFailure},
        {QStringLiteral("vbs_known"), snapshot.vbsKnown}, {QStringLiteral("vbs_state"), static_cast<int>(snapshot.vbsState)}};
    put(root, "installed_bytes", snapshot.installed ? Bytes(snapshot.installed) : Bytes{});
    put(root, "windows_total_bytes", snapshot.total ? Bytes(snapshot.total) : Bytes{}); put(root, "available_bytes", snapshot.total ? Bytes(snapshot.available) : Bytes{});
    put(root, "windows_total_after_bytes", snapshot.totalAfter ? Bytes(snapshot.totalAfter) : Bytes{}); put(root, "available_after_bytes", snapshot.totalAfter ? Bytes(snapshot.availableAfter) : Bytes{});
    root.insert(QStringLiteral("elapsed_ms"), QString::number(snapshot.elapsedMs));
    put(root, "observed_vid_bytes", snapshot.observedVidBytes); put(root, "vid_total_counter_bytes", snapshot.vidTotalBytes); put(root, "unresolved_vid_bytes", snapshot.unresolvedVidBytes);
    root.insert(QStringLiteral("vid_conflict"), snapshot.vidConflict);
    QJsonArray services;
    for (const auto service : snapshot.securityServices) { services.append(static_cast<int>(service)); }
    root.insert(QStringLiteral("security_services_running"), services);
    QJsonArray counters, sources, partitions, processes;
    for (const auto& sample : snapshot.counters) {
        QJsonObject row{{QStringLiteral("metric"), QString::fromLatin1(metricName(sample.metric))}, {QStringLiteral("path"), sample.path},
            {QStringLiteral("instance"), sample.instance}, {QStringLiteral("sampled_at"), sample.sampledAt}, {QStringLiteral("status"), static_cast<qint64>(sample.status)},
            {QStringLiteral("raw"), sample.bytes ? QJsonValue(QString::number(sample.raw)) : QJsonValue(QJsonValue::Null)}, {QStringLiteral("provider_explanation"), sample.explanation}};
        put(row, "bytes", sample.bytes); counters.append(row);
    }
    for (const auto& source : snapshot.sources) {
        sources.append(QJsonObject{{QStringLiteral("source"), source.name}, {QStringLiteral("status"), static_cast<qint64>(source.status)},
            {QStringLiteral("rows"), static_cast<int>(source.rows)}, {QStringLiteral("complete"), source.complete}});
    }
    for (const auto& entry : snapshot.partitions) {
        QJsonObject row{{QStringLiteral("id"), entry.id}, {QStringLiteral("runtime_id"), entry.runtimeId}, {QStringLiteral("key"), entry.key},
            {QStringLiteral("name"), entry.name}, {QStringLiteral("owner"), entry.owner}, {QStringLiteral("type"), entry.type}, {QStringLiteral("state"), entry.state},
            {QStringLiteral("hosting_system_id"), entry.hostingSystemId}, {QStringLiteral("hcs"), entry.hcs}, {QStringLiteral("wmi"), entry.wmi},
            {QStringLiteral("identity_matched"), entry.inventoryMatched}, {QStringLiteral("ambiguous"), entry.ambiguous}, {QStringLiteral("vid_conflict"), entry.conflictingVid},
            {QStringLiteral("worker_pid"), static_cast<qint64>(entry.workerPid)}, {QStringLiteral("hcs_evidence"), entry.memoryEvidence}};
        row.insert(QStringLiteral("identity_aliases"), QJsonArray::fromStringList(entry.aliases));
        put(row, "vid_bytes", entry.vidBytes); put(row, "dynamic_bytes", entry.dynamicBytes); put(row, "guest_visible_bytes", entry.guestVisibleBytes);
        put(row, "deposited_bytes", entry.depositedBytes); put(row, "wmi_capacity_bytes", entry.wmiCapacity); put(row, "hcs_node_bytes", entry.hcsNodeBytes);
        put(row, "hcs_private_ws_bytes", entry.hcsPrivateWs); put(row, "hcs_commit_bytes", entry.hcsCommit);
        QJsonArray indices; for (auto index : entry.counterIndices) { indices.append(static_cast<qint64>(index)); } row.insert(QStringLiteral("counter_indices"), indices);
        partitions.append(row);
    }
    for (const auto& entry : snapshot.processes) {
        QJsonObject row{{QStringLiteral("name"), entry.name}, {QStringLiteral("pid"), static_cast<qint64>(entry.pid)},
            {QStringLiteral("status"), static_cast<qint64>(entry.status)}};
        put(row, "working_set_bytes", entry.workingSet); put(row, "private_commit_bytes", entry.privateCommit); processes.append(row);
    }
    QJsonObject comparison{{QStringLiteral("snapshot_time"), context.snapshotTime}, {QStringLiteral("pfn_time"), context.pfnTime}};
    put(comparison, "snapshot_remainder_bytes", context.snapshotRemainder); put(comparison, "pfn_driver_locked_bytes", context.pfnDriverLocked);
    put(comparison, "pfn_unknown_bytes", context.pfnUnknown); put(comparison, "pfn_unscanned_bytes", context.pfnUnscanned);
    root.insert(QStringLiteral("comparison_only"), comparison);
    root.insert(QStringLiteral("counters"), counters); root.insert(QStringLiteral("sources"), sources); root.insert(QStringLiteral("partitions"), partitions); root.insert(QStringLiteral("processes"), processes);
    return root;
}

void collect(const std::shared_ptr<Job>& job)
{
    std::shared_ptr<Snapshot> result;
    try {
        result = std::make_shared<Snapshot>();
        result->started = now();
        MEMORYSTATUSEX memory{}; memory.dwLength = sizeof(memory);
        if (GlobalMemoryStatusEx(&memory)) { result->total = memory.ullTotalPhys; result->available = memory.ullAvailPhys; }
        ULONGLONG installed = 0; if (GetPhysicallyInstalledSystemMemory(&installed)) { result->installed = installed * 1024; }
        int cpu[4]{}; __cpuid(cpu, 1); result->hypervisorPresent = (static_cast<unsigned>(cpu[2]) & (1U << 31)) != 0;
        if (result->hypervisorPresent) {
            __cpuid(cpu, 0x40000000); std::array<char, 13> vendor{};
            std::memcpy(vendor.data(), &cpu[1], 4); std::memcpy(vendor.data() + 4, &cpu[2], 4); std::memcpy(vendor.data() + 8, &cpu[3], 4);
            result->hypervisorVendor = QString::fromLatin1(vendor.data());
        }
        job->phase.store(1); counters(*result, job);
        if (!job->stopped()) { job->phase.store(3); collectHcs(*result, job); }
        if (!job->stopped()) { job->phase.store(4); processes(*result, job); }
        // WMI can take longer to connect on machines without Hyper-V management
        // support. Preserve the utility-VM and process evidence first.
        if (!job->stopped()) { job->phase.store(2); collectWmi(*result, job); }
        correlate(*result);
        if (GlobalMemoryStatusEx(&memory)) { result->totalAfter = memory.ullTotalPhys; result->availableAfter = memory.ullAvailPhys; }
        result->cancelled = job->cancel.load();
        result->timedOut = std::chrono::steady_clock::now() >= job->deadline;
        result->finished = now();
        result->elapsedMs = static_cast<std::uint64_t>(QDateTime::fromString(result->started, Qt::ISODateWithMs).msecsTo(QDateTime::fromString(result->finished, Qt::ISODateWithMs)));
    } catch (...) { if (result) { result->resourceFailure = true; result->finished = now(); } }
    { std::lock_guard<std::mutex> lock(job->mutex); job->result = std::move(result); }
    job->done.store(true);
}
}
