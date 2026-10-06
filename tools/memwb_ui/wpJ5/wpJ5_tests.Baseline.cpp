// ============================================================
// wpJ5_tests.Baseline.cpp
// 作用：覆盖"基线窗口含插入点"、"窗口跟随跨页（滚动→feeder 重算，Keep 分支不
// 喂入）"、增量②（setSourceRevisionProvider 默认 0 / 注入值被使用）、以及
// "两条数轴故意错开下一切正常"。
// ============================================================

#include "wpJ5_common.h"

#include <QApplication>
#include <QSignalSpy>

namespace wpj5_test
{
    namespace
    {
        // B1：基线窗口必须含插入点——先让画布读到数据，移动插入点并 flushNow，
        // 核对 feeder.lastWindow() 的 [base, base+length) 覆盖插入点本身。
        void TestBaselineWindowContainsInsertionPoint()
        {
            Harness h(0, std::vector<std::uint8_t>(0x10000, 0x33));
            h.AttachProcess();
            h.SetAddressSpace(0, 0xFFFF);
            WPJ5_CHECK(h.WaitUntilSettled(0x2000, 2000));

            h.pane.canvas()->setCaretAddress(0x2000, false, false);

            // onBaselineRefreshed 必须把 baselineRefreshed 转发成
            // canvas_->notifyOverlayChanged()（排队发出 contentChanged）——
            // 用信号计数直接验证这条转发，不是只看 lastWindow() 的最终结果
            // （那个结果即使转发缺失也会正确，缺失的只是"画布被通知"这一步）。
            QSignalSpy contentChangedSpy(h.pane.canvas(), &ks::ui::HexCanvas::contentChanged);
            h.feeder.flushNow();
            // contentChanged 是排队发出的（HexCanvas.h"四之二"：合并到下一轮事件
            // 循环才真正 emit），单次 processEvents() 不一定覆盖它排队使用的确切
            // 机制（可能是零延时定时器），用 PumpUntil 更稳妥。
            WPJ5_CHECK_NOTE(
                PumpUntil([&]() { return contentChangedSpy.count() >= 1; }, 500),
                "onBaselineRefreshed 必须调用 canvas_->notifyOverlayChanged()，画布必须收到 contentChanged");

            const ksword::memwb::BaselineWindowRecord& window = h.feeder.lastWindow();
            WPJ5_CHECK(window.hasWindow);
            WPJ5_CHECK_NOTE(
                window.base <= 0x2000 && 0x2000 < window.base + window.length,
                "基线窗口必须覆盖插入点 0x2000");
        }

        // B2：窗口跟随跨页——插入点移动到距离很远、从未读过的一页，必须经过
        // "重新请求页→落地→jobLanded 驱动 noteDirty→recompute"这一整条链路，
        // 窗口最终重新围绕新插入点选取（base 变化，且新窗口仍含新插入点）。
        void TestWindowFollowsAcrossPages()
        {
            Harness h(0, std::vector<std::uint8_t>(0x20000, 0x44));
            h.AttachProcess();
            h.SetAddressSpace(0, 0x1FFFF);
            WPJ5_CHECK(h.WaitUntilSettled(0, 2000));

            h.pane.canvas()->setCaretAddress(0, false, false);
            h.feeder.flushNow();
            const std::uint64_t firstBase = h.feeder.lastWindow().base;

            // 跳到地址空间另一端，远超出当前窗口与当前已缓存的页——必须真正
            // 触发新的页请求。
            constexpr std::uint64_t kFarAddress = 0x1FF00;
            h.pane.canvas()->setCaretAddress(kFarAddress, false, true); // ensureVisible=true 会滚动、触发新的可见范围
            WPJ5_CHECK_NOTE(h.WaitUntilSettled(kFarAddress, 3000), "跳到远端地址后必须真正读到那一页");
            WPJ5_CHECK(h.WaitUntilNoInFlight(3000));

            // jobLanded 已经把 noteDirty 再驱动了一次；给防抖定时器一点时间，
            // 或者直接 flushNow 强制立即执行（两者皆可，这里用 flushNow 保证
            // 测试不依赖真实 50ms 定时器的墙钟时间）。
            h.feeder.flushNow();
            const ksword::memwb::BaselineWindowRecord& window = h.feeder.lastWindow();
            WPJ5_CHECK(window.hasWindow);
            WPJ5_CHECK_NOTE(window.base != firstBase, "窗口必须真的重新围绕新插入点选取，不能停在旧窗口");
            WPJ5_CHECK_NOTE(
                window.base <= kFarAddress && kFarAddress < window.base + window.length,
                "新窗口必须覆盖新插入点");
        }

        // B3：Keep 分支——插入点在同一页内小幅移动（仍落在当前窗口内）不应该
        // 触发任何额外的 RefreshBaseline/RefreshSameSpan（baselineRefreshed
        // 不应该再发），窗口 base/length 保持不变。
        void TestKeepBranchDoesNotRefeed()
        {
            Harness h(0, std::vector<std::uint8_t>(0x10000, 0x55));
            h.AttachProcess();
            h.SetAddressSpace(0, 0xFFFF);
            WPJ5_CHECK(h.WaitUntilSettled(0x100, 2000));

            h.pane.canvas()->setCaretAddress(0x100, false, false);
            h.feeder.flushNow();
            const ksword::memwb::BaselineWindowRecord before = h.feeder.lastWindow();
            WPJ5_CHECK(before.hasWindow);

            int refreshCount = 0;
            QObject::connect(&h.feeder, &ks::ui::WorkbenchBaselineFeeder::baselineRefreshed, &h.feeder,
                [&refreshCount](quint64, quint64) { ++refreshCount; });

            // 挪到同一页内的另一个字节（仍在窗口内）。
            h.pane.canvas()->setCaretAddress(0x140, false, false);
            h.feeder.flushNow();

            const ksword::memwb::BaselineWindowRecord& after = h.feeder.lastWindow();
            WPJ5_CHECK_NOTE(refreshCount == 0, "Keep 分支不应该触发任何 baselineRefreshed");
            WPJ5_CHECK(after.base == before.base);
            WPJ5_CHECK(after.length == before.length);
        }

        // B4：setSourceRevisionProvider 未设置时退回 0——Harness 默认
        // sourceRevisionOverride=0，不需要额外设置，直接核对 lastWindow 的
        // sourceRevision 字段。
        void TestSourceRevisionProviderDefaultsToZero()
        {
            Harness h(0, std::vector<std::uint8_t>(0x1000, 0x66));
            h.AttachProcess();
            h.SetAddressSpace(0, 0xFFF);
            WPJ5_CHECK(h.WaitUntilSettled(0, 2000));

            h.pane.canvas()->setCaretAddress(0, false, false);
            h.feeder.flushNow();
            WPJ5_CHECK(h.feeder.lastWindow().sourceRevision == 0);
        }

        // B5：setSourceRevisionProvider 注入的值必须被 WorkbenchHexPane 实际
        // 使用——改成一个非零常量，从一开始（窗口首次建立）就应该反映在
        // lastWindow().sourceRevision 里，不是 0、也不是画布自己的 sourceRevision()。
        void TestSourceRevisionProviderInjectedValueIsUsed()
        {
            Harness h(0, std::vector<std::uint8_t>(0x1000, 0x77));
            h.sourceRevisionOverride = 777;
            h.AttachProcess();
            h.SetAddressSpace(0, 0xFFF);
            WPJ5_CHECK(h.WaitUntilSettled(0, 2000));

            h.pane.canvas()->setCaretAddress(0, false, false);
            h.feeder.flushNow();
            WPJ5_CHECK_NOTE(
                h.feeder.lastWindow().sourceRevision == 777,
                "lastWindow().sourceRevision 必须等于注入回调返回的值");
            WPJ5_CHECK_NOTE(
                h.feeder.lastWindow().sourceRevision != h.pane.canvas()->sourceRevision(),
                "目标轴（注入值）与画布轴（canvas 自己的计数器）必须是两个独立的数");
        }

        // B7：纯滚动（不移动插入点）必须靠 visibleRangeChanged 的视口中心锚点
        // 驱动 noteDirty——只调用 scrollToAddress，从不调用 setCaretAddress，
        // 窗口最终仍要跟着挪到新的可见区域附近。这条专门堵住"caretMoved 与
        // visibleRangeChanged 两条接线哪一条真正在起作用"的空子：B2 用的是
        // setCaretAddress，单独这一条不会动用 visibleRangeChanged 那一路。
        void TestScrollOnlyFollowsViaVisibleRangeAnchor()
        {
            Harness h(0, std::vector<std::uint8_t>(0x20000, 0x99));
            h.AttachProcess();
            h.SetAddressSpace(0, 0x1FFFF);
            WPJ5_CHECK(h.WaitUntilSettled(0, 2000));

            h.pane.canvas()->setCaretAddress(0, false, false);
            h.feeder.flushNow();
            const std::uint64_t firstBase = h.feeder.lastWindow().base;

            // 只滚动可见范围，插入点依旧停在地址 0。
            constexpr std::uint64_t kFarAddress = 0x1FF00;
            WPJ5_CHECK(h.pane.canvas()->scrollToAddress(kFarAddress, ks::ui::HexCanvas::ScrollAlign::Center));
            WPJ5_CHECK(h.pane.canvas()->caretAddress() == 0); // 插入点确实没有被滚动带动

            WPJ5_CHECK_NOTE(h.WaitUntilSettled(kFarAddress, 3000), "滚动到的新区域必须真正读到");
            WPJ5_CHECK(h.WaitUntilNoInFlight(3000));
            h.feeder.flushNow();

            const ksword::memwb::BaselineWindowRecord& window = h.feeder.lastWindow();
            WPJ5_CHECK(window.hasWindow);
            WPJ5_CHECK_NOTE(
                window.base != firstBase,
                "纯滚动（插入点未变）也必须让窗口跟着可见范围的视口中心重新选取");
        }

        // B8：页请求落地（jobLanded）必须自动再驱动一次 noteDirty，不依赖调用方
        // 手动调用 flushNow——只用真实的 50ms 防抖定时器 + 事件循环等待，不在
        // 测试里插一次 flushNow 走捷径（flushNow 本身不检查"此刻是否仍有在途
        // 请求"，会掩盖 jobLanded 没接好的缺口，见 wpJ5_common.cpp 的接线分析）。
        void TestJobLandedAutoRetriggersWithoutManualFlush()
        {
            Harness h(0, std::vector<std::uint8_t>(0x20000, 0xA5));
            h.AttachProcess();
            h.SetAddressSpace(0, 0x1FFFF);
            WPJ5_CHECK(h.WaitUntilSettled(0, 2000));
            h.pane.canvas()->setCaretAddress(0, false, false);
            h.feeder.flushNow();

            int refreshCount = 0;
            QObject::connect(&h.feeder, &ks::ui::WorkbenchBaselineFeeder::baselineRefreshed, &h.feeder,
                [&refreshCount](quint64, quint64) { ++refreshCount; });

            // 跳到一段从未读过的新页，之后只泵事件循环，不手动调用 flushNow。
            constexpr std::uint64_t kFarAddress = 0x1FF00;
            h.pane.canvas()->setCaretAddress(kFarAddress, false, true);
            WPJ5_CHECK_NOTE(
                PumpUntil([&]() { return refreshCount >= 1; }, 3000),
                "不手动 flushNow：jobLanded 必须自动再驱动一次 noteDirty，让 50ms 防抖定时器"
                "真正跑起来并喂入新窗口");
            WPJ5_CHECK_NOTE(
                h.feeder.lastWindow().hasWindow
                    && h.feeder.lastWindow().base <= kFarAddress
                    && kFarAddress < h.feeder.lastWindow().base + h.feeder.lastWindow().length,
                "自动喂入的窗口必须覆盖新插入点");
        }

        // B6：两条数轴故意错开下一切正常——画布轴（HexCanvas::sourceRevision，
        // 经过 setAddressSpace 之后恒大于 1）与目标轴（sourceRevisionOverride，
        // 这里故意设成一个完全不相关的大数）数值不同，整条读/跳转/基线链路都
        // 必须正常工作，不因为"两个数不相等"而拒绝或崩溃。
        void TestMismatchedAxesStillWork()
        {
            Harness h(0, std::vector<std::uint8_t>(0x1000, 0x88));
            h.sourceRevisionOverride = 9999999ULL; // 与画布轴（个位数量级）刻意差很远
            h.AttachProcess();
            h.SetAddressSpace(0, 0xFFF);
            WPJ5_CHECK_NOTE(
                h.pane.canvas()->sourceRevision() != h.sourceRevisionOverride,
                "测试前提：两条数轴确实不同（画布轴是小个位数，目标轴是刻意设置的大数）");

            WPJ5_CHECK(h.WaitUntilSettled(0, 2000));
            WPJ5_CHECK(h.pane.jumpTo(0x10) == true);
            h.pane.canvas()->setCaretAddress(0x10, false, false);
            h.feeder.flushNow();
            WPJ5_CHECK(h.feeder.lastWindow().hasWindow);
            WPJ5_CHECK(h.feeder.lastWindow().sourceRevision == 9999999ULL);
        }

        // B9（独立审核 wave3 review-wpJ5.md §1.1 PLAUSIBLE 缺陷的回归测试）：
        // 换目标但地址空间边界完全相同、从头到尾不触碰插入点/滚动条（典型：
        // Process 范围内切换到位数相同的另一个进程，用户还没碰过画布）。这个
        // 场景下 HexCanvas::notifyVisibleRange()/applySelectionChange() 的去重
        // 逻辑都不会发出任何信号（见 WorkbenchHexPane.cpp setAddressSpace 末尾
        // 新增那段注释的逐行论证）。
        //
        // 光靠"换目标必然触发一次新的页请求→jobLanded"这条路径本身不足以单独
        // 验证本次修复：canvas_->setAddressSpace 内部总会重建一个全新的空页
        // 缓存并同步发起 requestVisiblePages，只要 Gate 可用，这次换目标恒会
        // 提交一个新 job，落地时 jobLanded 驱动的 noteDirty 用的是"旧锚点"，
        // 但由于本场景边界相同、旧锚点与新锚点的数值也刚好相近，单看
        // sourceRevision 是否更新测不出这条 jobLanded 路径到底有没有在打掩护
        // （真的试过：只留 jobLanded 这条路径、去掉本函数的显式 noteDirty，
        // sourceRevision 依然会被动更新，SURVIVED）。
        //
        // 要把"setAddressSpace 必须自己补一次 noteDirty"这件事从"jobLanded
        // 迟早也会补上"中单独剥离出来，必须构造一个 jobLanded **根本不会发生**
        // 的换目标场景：把 Gate 设成不可用（gateAvailable=false）。此时
        // canvas_->setAddressSpace 内部的 requestVisiblePages 会在
        // WorkbenchPageProvider::RequestPages 的 Gate 判定处直接 cancelPages
        // 返回，不提交任何 job，jobLanded 永远不会为这次换目标发出——基线喂入器
        // 能否知道"这次换目标发生过"，完全只能靠 setAddressSpace 末尾那次显式
        // noteDirty。换目标后插入点所在页显然没有落定（Gate 挡住了所有读取），
        // feeder 应该发 baselineUnavailable(InsertionPageNotSettled)；没有这次
        // 显式 noteDirty，feeder 对这次换目标一无所知，baselineUnavailable 一次
        // 都不会发。
        void TestRetargetSameBoundsAndCaretStillTriggersNoteDirty()
        {
            Harness h(0, std::vector<std::uint8_t>(0x10000, 0x11));
            h.sourceRevisionOverride = 1001;
            h.AttachProcess(1001, 1);
            h.SetAddressSpace(0, 0xFFFF);
            WPJ5_CHECK(h.WaitUntilSettled(0, 2000));
            WPJ5_CHECK(h.WaitUntilNoInFlight(2000));
            h.feeder.flushNow();
            WPJ5_CHECK_NOTE(h.feeder.lastWindow().hasWindow, "前提：首次 setAddressSpace 之后基线必须先成功喂入一次");

            int unavailableCount = 0;
            QObject::connect(&h.feeder, &ks::ui::WorkbenchBaselineFeeder::baselineUnavailable, &h.feeder,
                [&unavailableCount](ksword::memwb::BaselineSpanStatus) { ++unavailableCount; });

            // ---- 换目标：不同 pid（不同身份），地址空间边界与上面完全相同，
            // 从不触碰插入点/滚动条，且 Gate 不可用——这次换目标不会提交任何
            // 页请求，jobLanded 不会发生，只能靠 setAddressSpace 末尾的显式
            // noteDirty 唤醒 feeder。 ----
            h.gateAvailable = false;
            h.sourceRevisionOverride = 2002;
            h.AttachProcess(2002, 1);
            h.SetAddressSpace(0, 0xFFFF); // 边界与上面完全相同

            WPJ5_CHECK_NOTE(
                !h.provider.hasInFlightRequests(),
                "前提：Gate 不可用时这次换目标不应该提交任何页请求（jobLanded 路径必须完全关闭，"
                "否则测不出 setAddressSpace 自身有没有补 noteDirty）");
            WPJ5_CHECK_NOTE(
                PumpUntil([&]() { return unavailableCount >= 1; }, 1000),
                "换目标但地址空间边界相同、插入点未动、Gate 又恰好不可用（没有 jobLanded 兜底）"
                "时，setAddressSpace 必须自己补一次 noteDirty 才能让 feeder 知道这次换目标——"
                "不依赖画布重发 visibleRangeChanged/caretMoved，也不依赖 jobLanded（审核报告"
                "wave3/review-wpJ5.md §1.1）");
        }
    }

    void RunBaselineTests()
    {
        TestBaselineWindowContainsInsertionPoint();
        TestWindowFollowsAcrossPages();
        TestKeepBranchDoesNotRefeed();
        TestSourceRevisionProviderDefaultsToZero();
        TestSourceRevisionProviderInjectedValueIsUsed();
        TestScrollOnlyFollowsViaVisibleRangeAnchor();
        TestJobLandedAutoRetriggersWithoutManualFlush();
        TestMismatchedAxesStillWork();
        TestRetargetSameBoundsAndCaretStillTriggersNoteDirty();
    }
}
