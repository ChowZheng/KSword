// ============================================================
// wpK1_tests.ProbeDecision.cpp
// 作用：WorkbenchServicesProbe 里三个无状态判据的真值表断言：
//       DecideHandleCache（保护查询进程句柄缓存的复用/复核/重开）、
//       CreateTimesMatch（同一进程实例判据）、
//       DecideCandidateRefresh（候选进程缓存的同步填充/后台刷新/直接用缓存）。
// ============================================================

#include "wpK1_common.h"

#include "../../../Ksword5.1/Ksword5.1/MemoryDock/WorkbenchServicesProbe.h"

#include <cstdint>

namespace svc = ksword::memwb_services_detail;

namespace
{
    // kRevalidate：句柄缓存复核间隔（毫秒），测试用。
    constexpr std::uint64_t kRevalidate = 2000U;

    // TestHandleCacheDecision：句柄缓存复用判据。
    void TestHandleCacheDecision()
    {
        // 没有条目：重开（不论其它参数）。
        WPK1_CHECK(svc::DecideHandleCache(false, 0U, 100U, 0U, 0U, kRevalidate) == svc::HandleCacheDecision::Reopen);
        WPK1_CHECK(svc::DecideHandleCache(false, 100U, 100U, 500U, 600U, kRevalidate) ==
            svc::HandleCacheDecision::Reopen);

        // 条目属于别的 pid：重开（缓存的句柄绝不能用于另一个 pid）。
        WPK1_CHECK(svc::DecideHandleCache(true, 200U, 100U, 500U, 600U, kRevalidate) ==
            svc::HandleCacheDecision::Reopen);

        // 同 pid、刚核验过：直接复用。
        WPK1_CHECK(svc::DecideHandleCache(true, 100U, 100U, 1000U, 1000U, kRevalidate) ==
            svc::HandleCacheDecision::UseCached);
        WPK1_CHECK(svc::DecideHandleCache(true, 100U, 100U, 1000U, 1000U + kRevalidate - 1U, kRevalidate) ==
            svc::HandleCacheDecision::UseCached);

        // 恰好到复核间隔及之后：复核（重新读创建时间比对）。
        WPK1_CHECK(svc::DecideHandleCache(true, 100U, 100U, 1000U, 1000U + kRevalidate, kRevalidate) ==
            svc::HandleCacheDecision::Revalidate);
        WPK1_CHECK(svc::DecideHandleCache(true, 100U, 100U, 1000U, 1000U + 10U * kRevalidate, kRevalidate) ==
            svc::HandleCacheDecision::Revalidate);

        // 时钟倒退：无法判断过了多久，保守复核。
        WPK1_CHECK(svc::DecideHandleCache(true, 100U, 100U, 5000U, 100U, kRevalidate) ==
            svc::HandleCacheDecision::Revalidate);

        // 复核间隔为 0：每次都复核。
        WPK1_CHECK(svc::DecideHandleCache(true, 100U, 100U, 1000U, 1000U, 0U) ==
            svc::HandleCacheDecision::Revalidate);
    }

    // TestCreateTimesMatch：同一进程实例判据——0 表示没读到，"不知道"不能当"相等"。
    void TestCreateTimesMatch()
    {
        WPK1_CHECK(svc::CreateTimesMatch(133000000000000000ULL, 133000000000000000ULL));
        WPK1_CHECK(!svc::CreateTimesMatch(133000000000000000ULL, 133000000000000001ULL));
        // 任一为 0（没读到）：不匹配，哪怕两边都是 0。
        WPK1_CHECK(!svc::CreateTimesMatch(0U, 0U));
        WPK1_CHECK(!svc::CreateTimesMatch(0U, 133000000000000000ULL));
        WPK1_CHECK(!svc::CreateTimesMatch(133000000000000000ULL, 0U));
        // 最小非零值相等。
        WPK1_CHECK(svc::CreateTimesMatch(1U, 1U));
    }

    // TestCandidateRefreshDecision：候选进程缓存刷新判据的真值表。
    void TestCandidateRefreshDecision()
    {
        using svc::CandidateRefreshDecision;
        constexpr std::uint64_t kStale = 3000U;

        // 从未填充：没有在途刷新 -> 同步填充；已有在途 -> 直接返回（空）缓存，不重复启动。
        WPK1_CHECK(svc::DecideCandidateRefresh(false, false, 0U, 0U, kStale) == CandidateRefreshDecision::RefreshSync);
        WPK1_CHECK(svc::DecideCandidateRefresh(false, true, 0U, 0U, kStale) == CandidateRefreshDecision::ReturnCache);
        // 从未填充时，时间参数不影响结论。
        WPK1_CHECK(svc::DecideCandidateRefresh(false, false, 99999U, 5U, kStale) == CandidateRefreshDecision::RefreshSync);

        // 已填充、年龄不足阈值：直接用缓存。
        WPK1_CHECK(svc::DecideCandidateRefresh(true, false, 1000U, 1000U, kStale) == CandidateRefreshDecision::ReturnCache);
        WPK1_CHECK(svc::DecideCandidateRefresh(true, false, 1000U, 1000U + kStale - 1U, kStale) ==
            CandidateRefreshDecision::ReturnCache);

        // 已填充、年龄达到阈值：后台刷新（先返回旧缓存）。
        WPK1_CHECK(svc::DecideCandidateRefresh(true, false, 1000U, 1000U + kStale, kStale) ==
            CandidateRefreshDecision::RefreshAsync);
        WPK1_CHECK(svc::DecideCandidateRefresh(true, false, 1000U, 1000U + 100U * kStale, kStale) ==
            CandidateRefreshDecision::RefreshAsync);

        // 已填充、年龄够老但已有刷新在途：不重复启动。
        WPK1_CHECK(svc::DecideCandidateRefresh(true, true, 1000U, 1000U + 100U * kStale, kStale) ==
            CandidateRefreshDecision::ReturnCache);

        // 时钟倒退：保守地后台刷新（除非已有在途）。
        WPK1_CHECK(svc::DecideCandidateRefresh(true, false, 5000U, 100U, kStale) ==
            CandidateRefreshDecision::RefreshAsync);
        WPK1_CHECK(svc::DecideCandidateRefresh(true, true, 5000U, 100U, kStale) ==
            CandidateRefreshDecision::ReturnCache);

        // 阈值为 0：已填充的缓存每次都后台刷新（但仍不会在刷新在途时重复启动）。
        WPK1_CHECK(svc::DecideCandidateRefresh(true, false, 1000U, 1000U, 0U) == CandidateRefreshDecision::RefreshAsync);
        WPK1_CHECK(svc::DecideCandidateRefresh(true, true, 1000U, 1000U, 0U) == CandidateRefreshDecision::ReturnCache);
    }
}

namespace wpK1_test
{
    void RunProbeDecisionTests()
    {
        TestHandleCacheDecision();
        TestCreateTimesMatch();
        TestCandidateRefreshDecision();
    }
}
