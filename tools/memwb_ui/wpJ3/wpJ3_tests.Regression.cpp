// wpJ3_tests.Regression.cpp
// 作用：wave3 独立审核报告确认的
// D1/D2/D3 三个缺陷的回归测试，以及审核者变异测试揪出的、原夹具没有牙齿的其余缺口
// （V05a/V05b/V06/V07/V08/V09/V10/V11/V12，命名前缀 Gap）。D4 的回归测试放在
// wpJ3_tests.Decision.cpp（CheckRedirectClearsPendingAnchorAndTimer），H04 放在
// wpJ3_tests.SettledPages.cpp（CheckSettledSingleAddressQuery），V01/V02/V03 放在
// wpJ3_tests.Debounce.cpp——都更贴近那几个文件原有的主题，不重复搬到这里。
//
// 本文件的 Fixture 固定用远离地址 0 的 kBase，与 wpJ3_tests.Decision.cpp 的做法
// 一致，避开 setAddressSpace 自动产生的 Pending 页干扰。

#include "wpJ3_common.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>
#include <QTimer>

#include <memory>

namespace memwb_wpJ3_test
{
    namespace
    {
        // Fixture：画布 + 不响应提供者 + 真实叠加层 + Feeder + 录像机，地址空间
        // [0, 0xFFFFFFFF]，测试页都放在 kBase 附近，避开地址 0 周边的自动 Pending 页。
        struct Fixture
        {
            HexCanvas canvas;
            NoOpPageProvider provider;
            ksword::memwb::MemoryDiffOverlay overlay;
            WorkbenchBaselineFeeder feeder;
            BaselineRecorder recorder;
            static constexpr std::uint64_t kBase = 0x60000000ULL;

            Fixture()
            {
                SetupCanvas(canvas, provider, 0, 0xFFFFFFFFULL);
                feeder.setCanvas(&canvas);
                feeder.setOverlay(&overlay);
                feeder.setAddressSpaceBounds(ksword::memwb::AddressSpaceBounds{0, 0xFFFFFFFFULL});
                recorder.Attach(feeder, feeder);
            }

            // Page：第 index 页（可正可负）的起始地址，供需要多页连续布局的测试使用。
            static std::uint64_t Page(int index)
            {
                return kBase + static_cast<std::uint64_t>(index) * kPageBytes;
            }
        };

        // ================== D1：在途门控取消不了已起算的防抖定时器 ==================

        // ---- D1 之一：noteDirty(...,true) 到来时，必须取消此前已经起算、尚未到期
        //      的那次防抖定时器，而不是只"不再新起算"。否则旧定时器到期时仍会按
        //      抢跑前的旧状态触发一次判定。----
        void CheckDefect_InFlightCancelsArmedTimer()
        {
            Fixture fx;
            WPJ3_CHECK(DeliverValidPage(fx.canvas, Fixture::kBase, 0x10) == HexCanvas::PageResult::Accepted);
            fx.feeder.noteDirty(Fixture::kBase, 1, false); // 起算 50ms 防抖
            fx.feeder.noteDirty(Fixture::kBase, 1, true);  // 紧接着在途：必须取消上面那次

            const bool fired = PumpUntil(
                [&]() { return fx.recorder.refreshedCount >= 1 || fx.recorder.unavailableCount >= 1; }, 200);
            WPJ3_CHECK_NOTE(
                !fired,
                QStringLiteral("hasInFlightRequests=true 到来后，此前已起算的定时器必须被取消，不能到期自触发"));
        }

        // ---- D1 之二（后果）：如果上面那条定时器没被取消，它到期时会把"还没读完"
        //      的旧窗口当成最终结果记成新代次，真正读完之后的 noteDirty 反而因为
        //      "代次已经匹配"被误判为 Keep，基线从此停在陈旧字节上——用户把字节改
        //      回去的操作会被静默吞掉。----
        void CheckDefect_StaleTimerDoesNotPoisonBaseline()
        {
            Fixture fx;
            QByteArray before(static_cast<int>(kPageBytes), static_cast<char>(0x10));
            WPJ3_CHECK(DeliverCustomPage(fx.canvas, Fixture::kBase, before) == HexCanvas::PageResult::Accepted);
            fx.feeder.noteDirty(Fixture::kBase, 1, false);
            fx.feeder.flushNow(); // 先成功喂一次，record_.sourceRevision=1

            fx.feeder.noteDirty(Fixture::kBase + 1, 1, false); // 起算一次（代次仍是 1，本不会真正触发喂入）
            fx.feeder.noteDirty(Fixture::kBase + 1, 2, true);  // 紧接着在途，代次变成 2：必须取消上一次
            PumpUntil([]() { return false; }, 120); // 等过原定的 50ms

            QByteArray after = before;
            after[5] = static_cast<char>(0x99);
            WPJ3_CHECK(DeliverCustomPage(fx.canvas, Fixture::kBase, after) == HexCanvas::PageResult::Accepted);
            fx.feeder.noteDirty(Fixture::kBase + 1, 2, false); // 真正读完：不再在途
            const bool fired = PumpUntil([&]() { return fx.recorder.refreshedCount >= 2; }, 300);
            WPJ3_CHECK(fired);

            const std::optional<std::uint8_t> baseline = fx.overlay.BaselineByte(Fixture::kBase + 5);
            WPJ3_CHECK_NOTE(
                baseline.has_value() && *baseline == 0x99,
                QStringLiteral("基线必须是最终真正读到的字节 0x99，不能停在抢跑旧定时器喂入的 0x10"));
            WPJ3_CHECK(
                fx.overlay.ChangeKind(Fixture::kBase + 5) == ksword::memwb::ByteChangeKind::ExternalChange);
        }

        // ================== D2：record_ 不与叠加层真实基线对账 ==================

        // ---- D2 之一：叠加层被同一个 identityKey 重新 LoadBaseline（宿主绕过本类
        //      直接操作叠加层）之后，record_ 仍以为窗口没变，必须对账失效并重喂，
        //      不能永远 Keep 着一个叠加层早已不认的旧窗口。----
        void CheckDefect_OverlayReloadedBehindFeedersBack()
        {
            Fixture fx;
            fx.feeder.setIdentityKey("A");
            WPJ3_CHECK(DeliverValidPage(fx.canvas, Fixture::kBase, 0x10) == HexCanvas::PageResult::Accepted);
            fx.feeder.noteDirty(Fixture::kBase, 1, false);
            fx.feeder.flushNow();
            WPJ3_CHECK(fx.overlay.BaselineSize() == kPageBytes);

            fx.overlay.LoadBaseline("A", 0, {}, {}); // 宿主绕过 Feeder 直接重载：基线变空
            WPJ3_CHECK(fx.overlay.BaselineSize() == 0);

            fx.feeder.setIdentityKey("A"); // 同值，不经过 setIdentityKey 的 Reset 路径
            fx.feeder.noteDirty(Fixture::kBase, 1, false); // 锚点/代次都与 record_ 记的一样
            fx.feeder.flushNow();
            WPJ3_CHECK_NOTE(
                fx.overlay.BaselineSize() == kPageBytes,
                QStringLiteral("record_ 必须与叠加层对账失效并重新喂入，不能对着叠加层已经变空的旧窗口 Keep"));
        }

        // ---- D2 之二：setOverlay 换成一个全新的对象后，record_ 仍指着旧对象的窗口，
        //      必须对账失效并在新对象上重新喂入。----
        void CheckDefect_SetOverlayToFreshObjectRefeeds()
        {
            Fixture fx;
            WPJ3_CHECK(DeliverValidPage(fx.canvas, Fixture::kBase, 0x10) == HexCanvas::PageResult::Accepted);
            fx.feeder.noteDirty(Fixture::kBase, 1, false);
            fx.feeder.flushNow();

            ksword::memwb::MemoryDiffOverlay other; // 全新对象，从未 LoadBaseline
            fx.feeder.setOverlay(&other);
            fx.feeder.noteDirty(Fixture::kBase, 1, false); // 锚点/代次都与 record_ 记的一样
            fx.feeder.flushNow();
            WPJ3_CHECK_NOTE(
                other.HasBaseline() && other.BaselineSize() == kPageBytes,
                QStringLiteral("换成全新 overlay 对象后必须重新喂入，不能对着旧对象的窗口记录 Keep 在新对象上什么都不做"));
        }

        // ---- D2 之三：只核对 base/length 不够——叠加层被同身份以外的身份、但*相同*
        //      base/length 重新 LoadBaseline（host 绕过 setIdentityKey 直接改了叠加
        //      层的身份）时，base/length 两项都不会触发不一致，必须靠 IdentityKey()
        //      这一项才能发现 record_ 已经失效。本测试单独隔离出"只有身份变了"这一
        //      个变量，防止 D2 的对账退化成只比 base/length 两项。----
        void CheckDefect_OverlayIdentityChangedSameWindowReconciles()
        {
            Fixture fx;
            fx.feeder.setIdentityKey("A");
            WPJ3_CHECK(DeliverValidPage(fx.canvas, Fixture::kBase, 0x10) == HexCanvas::PageResult::Accepted);
            fx.feeder.noteDirty(Fixture::kBase, 1, false);
            fx.feeder.flushNow();
            WPJ3_CHECK(fx.overlay.IdentityKey() == std::string("A"));

            // 宿主绕过 Feeder，直接用*相同*的 base/length、但*不同*的身份重新载入——
            // base/length 两项都与 record_ 完全一致，只有身份变了。
            std::vector<std::uint8_t> bytesVec(static_cast<std::size_t>(kPageBytes), static_cast<std::uint8_t>(0x10));
            std::vector<std::uint8_t> maskVec(static_cast<std::size_t>(kPageBytes), static_cast<std::uint8_t>(1));
            fx.overlay.LoadBaseline("B", Fixture::kBase, bytesVec, maskVec);
            WPJ3_CHECK(fx.overlay.IdentityKey() == std::string("B"));

            // Feeder 自己的 identityKey_ 仍是 "A"（没人调用 setIdentityKey("B")）；
            // 用与 record_ 完全相同的锚点/代次触发一次判定。
            fx.feeder.noteDirty(Fixture::kBase, 1, false);
            fx.feeder.flushNow();
            WPJ3_CHECK_NOTE(
                fx.overlay.IdentityKey() == std::string("A"),
                QStringLiteral("record_ 必须因身份不符而失效重喂，把叠加层的身份纠正回 Feeder 自己记的 A"));
        }

        // ================== D3：挂起入口与写事务嵌套事件循环 ==================

        // ---- D3：确认框/审计这类嵌套事件循环期间，LRU 挤页会把窗口里另一页挤出
        //      缓存，若 Feeder 此时照常重算，会用挤页后的旧内容覆盖刚被 AcceptWrite
        //      标记为"自己写入"的基线字节。setSuspended(true) 必须让 Feeder 在整个
        //      嵌套循环期间完全不重算，恢复后基线字节与 SelfWritten 标记都应该还在。----
        void CheckDefect_SuspendedDuringCommitDoesNotClobberAcceptedWrite()
        {
            Fixture fx;
            fx.feeder.setPolicy(ksword::memwb::BaselineWindowPolicy{kPageBytes, 4});
            WPJ3_CHECK(DeliverValidPage(fx.canvas, Fixture::Page(0), 0x10) == HexCanvas::PageResult::Accepted); // 最旧，先被挤
            WPJ3_CHECK(DeliverValidPage(fx.canvas, Fixture::Page(1), 0x20) == HexCanvas::PageResult::Accepted); // 锚点页

            const std::uint64_t target = Fixture::Page(1) + 8;
            fx.feeder.noteDirty(target, 1, false);
            fx.feeder.flushNow();
            WPJ3_CHECK(fx.recorder.refreshedCount == 1);

            ksword::memwb::DiffBlock block;
            block.address = target;
            block.before = {0x20};
            block.after = {0x77};
            WPJ3_CHECK(fx.overlay.AcceptWrite(block, {0x77}) == ksword::memwb::AcceptWriteStatus::Ok);
            WPJ3_CHECK(fx.overlay.ChangeKind(target) == ksword::memwb::ByteChangeKind::SelfWritten);

            fx.feeder.setSuspended(true);
            WPJ3_CHECK(fx.feeder.isSuspended());
            fx.feeder.noteDirty(target, 1, false); // 挂起期间：只记录，不计时、不重算

            // 用一个嵌套事件循环模拟写事务确认框的模态循环：循环中途照常有 255 页
            // 到达（恰好挤掉 Page(0)，Page(1) 留着），模拟真实的 LRU 挤页场景。
            QEventLoop loop;
            QTimer::singleShot(10, &loop, [&]()
            {
                for (int index = 0; index < 255; ++index)
                {
                    DeliverValidPage(fx.canvas, Fixture::kBase + 0x10000000ULL + static_cast<std::uint64_t>(index) * kPageBytes, 0x33);
                }
                fx.feeder.noteDirty(target, 1, false); // 循环中途再来一次脏事件，挂起期间仍只记录
            });
            QTimer::singleShot(150, &loop, &QEventLoop::quit);
            loop.exec();

            WPJ3_CHECK_NOTE(
                fx.overlay.ChangeKind(target) == ksword::memwb::ByteChangeKind::SelfWritten,
                QStringLiteral("挂起期间 LRU 挤页不能清掉 SelfWritten 标记——Feeder 这段时间根本不该重算"));
            WPJ3_CHECK_NOTE(
                fx.overlay.BaselineByte(target).value_or(0) == 0x77,
                QStringLiteral("挂起期间不能用挤页后的旧内容覆盖刚 AcceptWrite 写入的基线字节"));
            WPJ3_CHECK_NOTE(
                fx.recorder.refreshedCount == 1,
                QStringLiteral("挂起期间不应该发生任何一次重新喂入"));

            fx.feeder.setSuspended(false);
            WPJ3_CHECK(!fx.feeder.isSuspended());
        }

        // ---- setSuspended 的进入/恢复两个分支必须各自做对应的事：挂起必须真正
        //      停掉已经起算的定时器（不能反而重新起算它），恢复之后若有有效的待
        //      处理请求必须重新起算并最终喂入（不能反而把它停掉）。上面的 D3 测试
        //      只检查"挂起期间没有被挤页覆盖"，没有单独验证这两个分支各自的动作
        //      方向，补这一条把两者分开验证。----
        void CheckSuspendStopsTimerAndResumeRearms()
        {
            Fixture fx;
            WPJ3_CHECK(DeliverValidPage(fx.canvas, Fixture::kBase, 0x10) == HexCanvas::PageResult::Accepted);
            fx.feeder.noteDirty(Fixture::kBase, 1, false); // 起算 50ms 防抖
            fx.feeder.setSuspended(true); // 挂起：必须停掉它，不能反而重新起算

            const bool firedWhileSuspended = PumpUntil([&]() { return fx.recorder.refreshedCount >= 1; }, 150);
            WPJ3_CHECK_NOTE(
                !firedWhileSuspended,
                QStringLiteral("挂起之后，此前起算的定时器必须被真正停止，不能照常甚至被重新起算后触发"));

            fx.feeder.setSuspended(false); // 恢复：挂起前记下过有效待处理请求，必须重新起算
            const bool firedAfterResume = PumpUntil([&]() { return fx.recorder.refreshedCount >= 1; }, 1000);
            WPJ3_CHECK_NOTE(
                firedAfterResume,
                QStringLiteral("恢复之后，若挂起前记下过有效的待处理请求，必须重新起算并最终喂入，不能反而被停掉"));
        }

        // ================== 原夹具真实缺口（审核者变异 V05a/V05b/V06/V07/V08/V09/V10/V11/V12）==================

        // ---- 堵 V06/V07：RefreshSameSpan 必须原位沿用记录里的 base/length（不能
        //      退化成每次都 Recompute 重新居中），且重喂之后同一代次的下一次判定
        //      必须是 Keep（证明 record_ 确实在 RefreshSameSpan 分支里也被正确更新，
        //      不是只有 Recompute 分支才更新）。----
        void CheckGap_RefreshSameSpanKeepsPositionThenKeeps()
        {
            Fixture fx;
            fx.feeder.setPolicy(ksword::memwb::BaselineWindowPolicy{kPageBytes, 3});
            for (int index = 0; index < 5; ++index)
            {
                WPJ3_CHECK(
                    DeliverValidPage(fx.canvas, Fixture::Page(index), static_cast<std::uint8_t>(0x10 + index))
                    == HexCanvas::PageResult::Accepted);
            }

            fx.feeder.noteDirty(Fixture::Page(0) + 10, 1, false);
            fx.feeder.flushNow();
            WPJ3_CHECK(fx.recorder.refreshedCount == 1);
            WPJ3_CHECK(fx.recorder.lastBase == Fixture::Page(0) && fx.recorder.lastLength == 3 * kPageBytes);

            fx.feeder.noteDirty(Fixture::Page(2) + 5, 2, false); // 位置仍在窗口内，仅代次前进：RefreshSameSpan
            fx.feeder.flushNow();
            WPJ3_CHECK_NOTE(
                fx.recorder.refreshedCount == 2,
                QStringLiteral("代次前进且位置不变必须是 RefreshSameSpan 并真正重喂一次"));
            WPJ3_CHECK_NOTE(
                fx.recorder.lastBase == Fixture::Page(0) && fx.recorder.lastLength == 3 * kPageBytes,
                QStringLiteral("RefreshSameSpan 必须原位沿用旧窗口，不能按新锚点重新居中"));

            fx.feeder.noteDirty(Fixture::Page(2) + 6, 2, false); // 同代次再来一次：必须是 Keep
            fx.feeder.flushNow();
            WPJ3_CHECK_NOTE(
                fx.recorder.refreshedCount == 2,
                QStringLiteral("RefreshSameSpan 之后 record_ 必须被正确更新为新代次，否则这里会被误判成代次仍不同而再喂一次"));
        }

        // ---- 堵 V08：SelectBaselineSpan 判失败（插入点页未落定）之后，不能清空
        //      record_——旧窗口此刻仍与叠加层真实基线一致，锚点回到旧窗口内时必须
        //      还是 Keep，不能因为中间那次失败的尝试被迫重新走一次 Recompute。----
        void CheckGap_FailedRecomputeKeepsOldRecord()
        {
            Fixture fx;
            WPJ3_CHECK(DeliverValidPage(fx.canvas, Fixture::kBase, 0x10) == HexCanvas::PageResult::Accepted);
            fx.feeder.noteDirty(Fixture::kBase, 1, false);
            fx.feeder.flushNow();
            WPJ3_CHECK(fx.recorder.refreshedCount == 1);

            fx.feeder.noteDirty(Fixture::kBase + 0x1000000ULL, 1, false); // 远处未落定页
            fx.feeder.flushNow();
            WPJ3_CHECK(fx.recorder.unavailableCount == 1 && fx.recorder.refreshedCount == 1);

            fx.feeder.noteDirty(Fixture::kBase + 7, 1, false); // 回到旧窗口内、代次不变
            fx.feeder.flushNow();
            WPJ3_CHECK_NOTE(
                fx.recorder.refreshedCount == 1,
                QStringLiteral("失败的中间尝试不能清空 record_；回到旧窗口必须仍是 Keep"));
        }

        // ---- 堵 V09：canvas 或 overlay 任一为空时 noteDirty 必须是完全空操作
        //      （不仅是"不喂入"，连 pendingValid_/定时器都不该被触碰）——之后把
        //      canvas 补上，被忽略的那次脏事件也不应该凭空复活去触发一次喂入。----
        void CheckGap_NoteDirtyWithoutCanvasLeavesNoPendingWork()
        {
            HexCanvas canvas;
            NoOpPageProvider provider;
            SetupCanvas(canvas, provider, 0, 0xFFFFFFFFULL);
            WPJ3_CHECK(DeliverValidPage(canvas, Fixture::kBase, 0x10) == HexCanvas::PageResult::Accepted);
            ksword::memwb::MemoryDiffOverlay overlay;
            WorkbenchBaselineFeeder feeder;
            BaselineRecorder recorder;
            recorder.Attach(feeder, feeder);
            feeder.setOverlay(&overlay); // 只设 overlay，canvas 留空
            feeder.setAddressSpaceBounds(ksword::memwb::AddressSpaceBounds{0, 0xFFFFFFFFULL});

            feeder.noteDirty(Fixture::kBase, 1, false); // canvas 为空：必须整体忽略
            feeder.setCanvas(&canvas); // 之后才补上 canvas
            PumpUntil([]() { return false; }, 200);
            WPJ3_CHECK_NOTE(
                recorder.refreshedCount == 0,
                QStringLiteral("canvas 为空时被忽略的那次 noteDirty 不应该在补上 canvas 之后凭空复活"));
        }

        // ---- 堵 V11：防抖定时器起算之后，若 overlay 在到期前被置空，recomputeAndFeed
        //      必须安全地什么都不做（不能解引用空指针崩溃）。----
        void CheckGap_TimerFiresAfterOverlayCleared()
        {
            Fixture fx;
            WPJ3_CHECK(DeliverValidPage(fx.canvas, Fixture::kBase, 0x10) == HexCanvas::PageResult::Accepted);
            fx.feeder.noteDirty(Fixture::kBase, 1, false); // 起算 50ms 防抖
            fx.feeder.setOverlay(nullptr); // 到期前置空
            PumpUntil([]() { return false; }, 150);
            WPJ3_CHECK_NOTE(
                fx.recorder.refreshedCount == 0 && fx.recorder.unavailableCount == 0,
                QStringLiteral("定时器到期时 overlay 已经为空，必须安全退化为空操作，不能崩溃也不能发信号"));
        }

        // ---- 堵 V10：防抖定时器起算之后，若画布在到期前被销毁（QPointer 自动置
        //      空），recomputeAndFeed 必须安全地什么都不做。----
        void CheckGap_TimerFiresAfterCanvasDestroyed()
        {
            auto canvas = std::make_unique<HexCanvas>();
            NoOpPageProvider provider;
            SetupCanvas(*canvas, provider, 0, 0xFFFFFFFFULL);
            WPJ3_CHECK(DeliverValidPage(*canvas, Fixture::kBase, 0x10) == HexCanvas::PageResult::Accepted);
            ksword::memwb::MemoryDiffOverlay overlay;
            WorkbenchBaselineFeeder feeder;
            BaselineRecorder recorder;
            recorder.Attach(feeder, feeder);
            feeder.setCanvas(canvas.get());
            feeder.setOverlay(&overlay);
            feeder.setAddressSpaceBounds(ksword::memwb::AddressSpaceBounds{0, 0xFFFFFFFFULL});

            feeder.noteDirty(Fixture::kBase, 1, false); // 起算 50ms 防抖
            canvas.reset(); // 到期前销毁：QPointer 自动置空
            PumpUntil([]() { return false; }, 150);
            WPJ3_CHECK_NOTE(
                recorder.refreshedCount == 0 && recorder.unavailableCount == 0,
                QStringLiteral("定时器到期时画布已经被销毁，必须安全退化为空操作，不能解引用悬空指针"));
        }

        // ---- 堵 V12：baselineRefreshed 信号的槛内重入 flushNow 只应该再喂一次
        //      （而不是因为 record_.Set 发生在 emit 之后，让重入时读到的还是上一次
        //      的旧记录，从而误判成需要再喂一次，导致无限重入或重复喂入）。----
        void CheckGap_ReentrantFlushInsideRefreshedSlot()
        {
            Fixture fx;
            WPJ3_CHECK(DeliverValidPage(fx.canvas, Fixture::kBase, 0x10) == HexCanvas::PageResult::Accepted);
            bool reentered = false;
            QObject::connect(
                &fx.feeder,
                &WorkbenchBaselineFeeder::baselineRefreshed,
                &fx.feeder,
                [&](quint64, quint64)
                {
                    if (!reentered)
                    {
                        reentered = true;
                        fx.feeder.flushNow(); // 槛内重入：此时 record_ 必须已经是最新的
                    }
                });
            fx.feeder.noteDirty(Fixture::kBase, 1, false);
            fx.feeder.flushNow();
            WPJ3_CHECK_NOTE(
                fx.recorder.refreshedCount == 1,
                QStringLiteral("record_.Set 必须发生在 emit 之前，否则槛内重入的 flushNow 会误判成需要再喂一次"));
        }

        // ---- 堵 V05a/V05b：在途（hasInFlightRequests=true）时 noteDirty 仍必须记下
        //      本次的锚点与来源代次——否则在途结束后下一次 noteDirty 到来时，
        //      flushNow/防抖用的还是上一次（甚至构造时默认值）的锚点/代次，而不是
        //      在途期间最新的那一次。----
        void CheckGap_InFlightNoteDirtyStillRecordsPending()
        {
            Fixture fx;
            WPJ3_CHECK(DeliverValidPage(fx.canvas, Fixture::kBase, 0x10) == HexCanvas::PageResult::Accepted);
            fx.feeder.noteDirty(Fixture::kBase + 5, 7, true); // 在途：不计时，但必须记下锚点/代次
            fx.feeder.flushNow(); // 没有新的非在途 noteDirty，直接 flush 应该用上面记下的值
            WPJ3_CHECK(fx.recorder.refreshedCount == 1);
            WPJ3_CHECK_NOTE(
                fx.feeder.lastWindow().sourceRevision == 7,
                QStringLiteral("在途期间记下的来源代次必须是 7，不能是构造时的默认值 0"));
            WPJ3_CHECK_NOTE(
                fx.recorder.lastBase == Fixture::kBase,
                QStringLiteral("在途期间记下的锚点（kBase+5）必须参与窗口选取，窗口应该围绕它所在的页"));
        }
    }

    void RunRegressionTests()
    {
        CheckDefect_InFlightCancelsArmedTimer();
        CheckDefect_StaleTimerDoesNotPoisonBaseline();
        CheckDefect_OverlayReloadedBehindFeedersBack();
        CheckDefect_SetOverlayToFreshObjectRefeeds();
        CheckDefect_OverlayIdentityChangedSameWindowReconciles();
        CheckDefect_SuspendedDuringCommitDoesNotClobberAcceptedWrite();
        CheckSuspendStopsTimerAndResumeRearms();
        CheckGap_RefreshSameSpanKeepsPositionThenKeeps();
        CheckGap_FailedRecomputeKeepsOldRecord();
        CheckGap_NoteDirtyWithoutCanvasLeavesNoPendingWork();
        CheckGap_TimerFiresAfterOverlayCleared();
        CheckGap_TimerFiresAfterCanvasDestroyed();
        CheckGap_ReentrantFlushInsideRefreshedSlot();
        CheckGap_InFlightNoteDirtyStillRecordsPending();
    }
}
