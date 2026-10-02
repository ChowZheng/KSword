#include "pch.h"
#include "MonitorCoverage.h"
#include "../hook/HookEngine.h"
#include "ApiMonitorMetadata.h"

namespace apimon
{
    namespace
    {
        std::atomic_uint64_t g_coverageSession{0};
        auto& g_coverageMutex = *new std::mutex;
        auto& g_snapshot = *new std::shared_ptr<const CoverageSnapshot>;
        auto& g_runtimeIds = *new std::unordered_map<std::wstring, std::uint32_t>;
        std::uint32_t g_nextRuntimeId = 0x80000000u;
        std::uint64_t g_revision = 0, g_revisionSession = 0;

        bool SameRows(const std::vector<ks::winapi_monitor::ApiMonitorEventPacket>& a,
            const std::vector<ks::winapi_monitor::ApiMonitorEventPacket>& b)
        {
            if (a.size() != b.size()) return false;
            for (std::size_t i = 0; i < a.size(); ++i)
            {
                if (a[i].apiId != b[i].apiId || a[i].coverageState != b[i].coverageState
                    || a[i].hookKind != b[i].hookKind || a[i].hookAddress != b[i].hookAddress
                    || wcscmp(a[i].moduleName, b[i].moduleName) || wcscmp(a[i].apiName, b[i].apiName)
                    || wcscmp(a[i].detailText, b[i].detailText)) return false;
            }
            return true;
        }
    }
    void BeginCoverageSession(const MonitorConfig& config)
    {
        const std::lock_guard<std::mutex> lock(g_coverageMutex);
        const auto session = ks::winapi_monitor::sessionIdentity(config.sessionId);
        if (g_revisionSession != session) { g_revision = 0; g_revisionSession = session; }
        g_snapshot.reset();
        g_coverageSession.store(ks::winapi_monitor::sessionIdentity(config.sessionId));
    }
    void EndCoverageSession()
    {
        const std::lock_guard<std::mutex> lock(g_coverageMutex);
        g_coverageSession.store(0);
        g_snapshot.reset();
    }
    std::uint64_t CurrentMonitorSessionIdentity() { return g_coverageSession.load(); }
    std::uint32_t RuntimeApiId(const wchar_t* module, const wchar_t* api)
    {
        const auto definition = FindApiDefinitionId(module, api);
        if (definition) return definition;
        const std::lock_guard<std::mutex> lock(g_coverageMutex);
        std::wstring key = module ? module : L"";
        std::transform(key.begin(), key.end(), key.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
        if (key.size() > 4 && key.substr(key.size() - 4) == L".dll") key.resize(key.size() - 4);
        key += L"!"; key += api ? api : L"";
        auto [iterator, inserted] = g_runtimeIds.try_emplace(key, g_nextRuntimeId);
        if (inserted) ++g_nextRuntimeId;
        return iterator->second;
    }
    void PublishCoverageSnapshot(std::vector<ks::winapi_monitor::ApiMonitorEventPacket> items)
    {
        ScopedInlineHookInternalBypass bypass;
        const std::lock_guard<std::mutex> lock(g_coverageMutex);
        const auto session = CurrentMonitorSessionIdentity();
        if (!session) return;
        const auto previous = g_snapshot;
        if (previous && previous->session == session && SameRows(previous->items, items)) return;
        auto snapshot = std::make_shared<CoverageSnapshot>();
        snapshot->session = session; snapshot->revision = ++g_revision;
        for (std::size_t i = 0; i < items.size(); ++i)
        {
            auto& packet = items[i];
            packet.pid = ::GetCurrentProcessId();
            packet.eventKind = static_cast<std::uint32_t>(ks::winapi_monitor::EventKind::CoverageItem);
            packet.sessionIdentity = session;
            packet.snapshotRevision = snapshot->revision;
            packet.snapshotIndex = static_cast<std::uint32_t>(i);
            packet.snapshotCount = static_cast<std::uint32_t>(items.size());
            std::memcpy(packet.definitionSha256, kDefinitionSha256, sizeof(packet.definitionSha256));
        }
        snapshot->items = std::move(items);
        g_snapshot = std::move(snapshot);
    }
    std::shared_ptr<const CoverageSnapshot> LatestCoverageSnapshot() { const std::lock_guard<std::mutex> lock(g_coverageMutex); return g_snapshot; }
}
