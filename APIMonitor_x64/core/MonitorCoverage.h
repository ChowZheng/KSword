#pragma once
#include "pch.h"
#include "MonitorConfig.h"

namespace apimon
{
    struct CoverageSnapshot
    {
        std::uint64_t session = 0;
        std::uint64_t revision = 0;
        std::vector<ks::winapi_monitor::ApiMonitorEventPacket> items;
    };
    void BeginCoverageSession(const MonitorConfig& config);
    void EndCoverageSession();
    std::uint64_t CurrentMonitorSessionIdentity();
    void PublishCoverageSnapshot(std::vector<ks::winapi_monitor::ApiMonitorEventPacket> items);
    std::shared_ptr<const CoverageSnapshot> LatestCoverageSnapshot();
    std::uint32_t RuntimeApiId(const wchar_t* module, const wchar_t* api);
}
