// ============================================================
// wpJ5_tests.Navigation.cpp
// 作用：覆盖 setAddressSpace/clearAddressSpace 对画布代次与叠加层身份串的
// 影响、"换目标后旧补丁被清且 canvas 代次变化"、jumpTo 的选区与越界语义。
// ============================================================

#include "wpJ5_common.h"

#include <QApplication>

namespace wpj5_test
{
    namespace
    {
        // N1：setAddressSpace 之后，画布轴代次必须真的前进，overlay 的身份串
        // 必须更新为新调用传入的那一个。
        void TestSetAddressSpaceChangesRevisionAndIdentity()
        {
            ks::ui::WorkbenchHexPane pane;
            pane.setAddressSpace(0, 0xFFF, "identity-a");
            const std::uint64_t revisionAfterFirst = pane.canvas()->sourceRevision();
            WPJ5_CHECK(pane.overlay().IdentityKey() == "identity-a");

            pane.setAddressSpace(0x1000, 0x1FFF, "identity-b");
            WPJ5_CHECK_NOTE(
                pane.canvas()->sourceRevision() != revisionAfterFirst,
                "再次 setAddressSpace 必须让画布轴来源代次前进");
            WPJ5_CHECK(pane.overlay().IdentityKey() == "identity-b");
        }

        // N2：换目标（不同 identityKey）必须清空旧目标遗留的暂存补丁——补丁按
        // 绝对地址存储，与地址空间无关，留着会被错配进新目标（文件头明确写的
        // 原因），且画布代次必须真的变化（新目标不是同一份数据）。
        void TestRetargetClearsPendingPatches()
        {
            ks::ui::WorkbenchHexPane pane;
            pane.setAddressSpace(0, 0xFFF, "retarget-old");
            const std::vector<std::uint8_t> bytes(0x1000, 0x12);
            const ksword::memwb::BaselineLoadStatus status =
                pane.overlay().LoadBaseline("retarget-old", 0, bytes, std::vector<std::uint8_t>(bytes.size(), 1));
            WPJ5_CHECK(status == ksword::memwb::BaselineLoadStatus::Ok);
            WPJ5_CHECK(pane.overlay().Stage(0x10, {0x99}) == ksword::memwb::StageStatus::Ok);
            WPJ5_CHECK(pane.overlay().HasPendingPatches());

            const std::uint64_t revisionBeforeRetarget = pane.canvas()->sourceRevision();
            pane.setAddressSpace(0x2000, 0x2FFF, "retarget-new");

            WPJ5_CHECK_NOTE(!pane.overlay().HasPendingPatches(), "换目标之后旧补丁必须被清空");
            WPJ5_CHECK(pane.overlay().IdentityKey() == "retarget-new");
            WPJ5_CHECK_NOTE(
                pane.canvas()->sourceRevision() != revisionBeforeRetarget,
                "换目标之后画布代次必须真的变化");
        }

        // N3：jumpTo 的选区语义——跳转到 [address, address+selectLength-1]，
        // 插入点停在 address（不是选区末端），选区覆盖整段。
        void TestJumpToSelectsRange()
        {
            ks::ui::WorkbenchHexPane pane;
            pane.setAddressSpace(0, 0xFFF, "jump-range");
            WPJ5_CHECK(pane.jumpTo(0x100, 16));
            WPJ5_CHECK(pane.insertionAddress() == 0x100);
            const std::optional<ks::ui::HexCanvas::AddressRange> range = pane.canvas()->selectedRange();
            WPJ5_CHECK(range.has_value());
            if (range.has_value())
            {
                WPJ5_CHECK(range->first == 0x100);
                WPJ5_CHECK(range->last == 0x10F);
            }
        }

        // N4：jumpTo 到地址空间之外必须明确返回 false，且不改变当前插入点
        // （不静默移动到越界位置，也不静默什么都不做却返回 true）。
        void TestJumpToOutsideSpaceFails()
        {
            ks::ui::WorkbenchHexPane pane;
            pane.setAddressSpace(0x1000, 0x1FFF, "jump-bounds");
            WPJ5_CHECK(pane.jumpTo(0x1500));
            WPJ5_CHECK(pane.insertionAddress() == 0x1500);

            WPJ5_CHECK_NOTE(!pane.jumpTo(0x500), "0x500 在地址空间之外，必须返回 false");
            WPJ5_CHECK_NOTE(pane.insertionAddress() == 0x1500, "失败的跳转不应该移动插入点");

            WPJ5_CHECK_NOTE(!pane.jumpTo(0x5000), "0x5000 同样在地址空间之外");
            WPJ5_CHECK(pane.insertionAddress() == 0x1500);
        }

        // N5：clearAddressSpace 之后，之前合法的地址不再可跳转；叠加层的基线
        // 长度归零、身份串变回空串（不要求 HasBaseline() 本身的具体取值——那是
        // LoadBaseline 自身的契约，由 Core 层的 wpD 测试覆盖，这里只断言
        // WorkbenchHexPane 确实把"无目标"传给了它）。
        void TestClearAddressSpaceResets()
        {
            ks::ui::WorkbenchHexPane pane;
            pane.setAddressSpace(0, 0xFFF, "clear-me");
            WPJ5_CHECK(pane.jumpTo(0x10));

            pane.clearAddressSpace();
            WPJ5_CHECK_NOTE(!pane.jumpTo(0x10), "清空地址空间之后，旧地址不应该再可跳转");
            WPJ5_CHECK(pane.overlay().IdentityKey().empty());
            WPJ5_CHECK(pane.overlay().BaselineSize() == 0);
        }

        // N6：setAddressSpace 传入非法区间（first > last）必须整体是空操作——
        // 画布保持原地址空间、overlay 身份串不变，不会让两者分叉到互相矛盾
        // 的状态（见 WorkbenchHexPane.cpp 对这一分支的说明）。
        void TestInvalidRangeIsNoOp()
        {
            ks::ui::WorkbenchHexPane pane;
            pane.setAddressSpace(0, 0xFFF, "valid-before");
            const std::uint64_t revisionBefore = pane.canvas()->sourceRevision();

            pane.setAddressSpace(0x500, 0x10, "should-not-apply"); // first > last，非法
            WPJ5_CHECK_NOTE(pane.canvas()->sourceRevision() == revisionBefore, "非法区间不应该让画布代次前进");
            WPJ5_CHECK_NOTE(
                pane.overlay().IdentityKey() == "valid-before", "非法区间不应该让 overlay 换上新身份串");
            WPJ5_CHECK(pane.jumpTo(0x10)); // 原地址空间仍然有效
        }

        // N7（独立审核 wave3 review-wpJ5.md §1.6 夹具缺口补测）：clearAddressSpace
        // 必须像 setAddressSpace 一样先调用 pageProvider_->cancelAllInFlight()
        // 作废旧目标仍在途的页读取任务——不这样做，旧请求落地时仍会尝试回填一个
        // 已经被清空的画布（虽然会被代次/画布判空等既有防线挡住，但在途记账
        // 本身必须被清掉，不能让它继续占着读池，见 WorkbenchHexPane.cpp
        // clearAddressSpace 的实现注释）。用 hasInFlightRequests() 这个公开读数
        // 直接验证，不猜测内部调用了什么。
        void TestClearAddressSpaceCancelsInFlightRequests()
        {
            Harness h(0, std::vector<std::uint8_t>(0x10000, 0x22));
            h.AttachProcess();
            h.SetAddressSpace(0, 0xFFFF);
            // 前提：canvas_->setAddressSpace 内部的 requestVisiblePages 已经同步
            // 提交过至少一批页请求，此刻事件循环还没跑，请求必然仍在途。
            WPJ5_CHECK_NOTE(
                h.provider.hasInFlightRequests(),
                "前提：setAddressSpace 之后应当有刚提交、尚未落地的在途页请求");

            h.ClearAddressSpace();
            WPJ5_CHECK_NOTE(
                !h.provider.hasInFlightRequests(),
                "clearAddressSpace 必须调用 pageProvider_->cancelAllInFlight() 作废在途请求——"
                "审核报告 N9：这条路径此前没有任何测试覆盖，变异删除这一调用也会"
                "原样通过");
        }

        // N8（独立审核 wave3 review-wpJ5.md §1.2 夹具缺口补测）：rereadWindow()
        // 的"基线窗口 ∪ 可见页"必须真的是并集，不能退化成交集——构造基线窗口
        // 远大于屏幕可见范围（16KB vs 一屏）的真实场景（文档原话："基线窗口
        // 通常远大于屏幕可见范围"），在基线窗口尾部、屏幕完全看不到的地址上
        // 篡改后备内存，调用 rereadWindow() 后核对该地址确实被重新读到——
        // 如果退化成交集，重读范围只会是屏幕那一小段，这个地址上的旧值会
        // 原样保留，测试应该失败。
        void TestRereadWindowUsesUnionNotIntersection()
        {
            Harness h(0, std::vector<std::uint8_t>(0x10000, 0x11));
            h.AttachProcess();
            h.SetAddressSpace(0, 0xFFFF);
            WPJ5_CHECK(h.WaitUntilSettled(0, 2000));
            WPJ5_CHECK(h.WaitUntilNoInFlight(2000));

            // Harness 的 pane 默认从未 resize/show 过（高度恒为 Qt 的默认 30px，
            // 只能画 1 行），这会让 hasVisibleRange_ 在第一次 notifyVisibleRange
            // 之后被 WorkbenchHexPane::setAddressSpace 自己清空、之后再也不会
            // 被重新置位（HexCanvas 的去重逐字节比较 [first,last]，同一尺寸算出
            // 的范围和上一次完全相同，永远被去重吞掉）——这样可见范围分量会
            // 一直是"没有"，测不出并集与基线窗口本身的差别。这里真实 resize+
            // show 一次，让画布真正算出一屏（远小于 16KB）的可见范围，
            // notifyVisibleRange 才会因为"这次范围与之前不同"而真的重发一次。
            h.pane.resize(900, 420);
            h.pane.show();
            QApplication::processEvents();

            // 直接用 overlay().LoadBaseline 钉死一个 16KB 的基线窗口（不依赖
            // feeder 的真实选取逻辑是否恰好选出这么大——TestRetargetClearsPendingPatches
            // 已经示范过这种直接操作 overlay 的合法用法：WorkbenchHexPane 换
            // 目标时本身就是直接调用 overlay() 的 LoadBaseline，不经过 feeder）。
            const std::vector<std::uint8_t> baselineBytes(0x4000, 0x11);
            const ksword::memwb::BaselineLoadStatus status = h.pane.overlay().LoadBaseline(
                h.CurrentIdentityKey(), 0, baselineBytes, std::vector<std::uint8_t>(baselineBytes.size(), 1));
            WPJ5_CHECK(status == ksword::memwb::BaselineLoadStatus::Ok);

            // kFarButInsideBaseline：远在屏幕可见范围之外（离屏测试窗口一屏远
            // 小于 0x3FF0 字节），但仍在上面钉死的 16KB 基线窗口 [0, 0x3FFF] 内。
            constexpr std::uint64_t kFarButInsideBaseline = 0x3FF0;
            {
                std::lock_guard<std::mutex> lock(h.backing->mutex);
                h.backing->bytes[kFarButInsideBaseline] = 0x77;
            }

            h.pane.rereadWindow();
            WPJ5_CHECK_NOTE(
                PumpUntil(
                    [&]() { return h.pane.canvas()->cellStateAt(kFarButInsideBaseline).value == 0x77; },
                    3000),
                "rereadWindow 必须用基线窗口∪可见页的并集重读：0x3FF0 远在屏幕之外、"
                "但仍在 16KB 基线窗口内，审核报告 N7——退化成交集只会重读屏幕那一小段，"
                "读不到这里");
        }
    }

    void RunNavigationTests()
    {
        TestSetAddressSpaceChangesRevisionAndIdentity();
        TestRetargetClearsPendingPatches();
        TestJumpToSelectsRange();
        TestJumpToOutsideSpaceFails();
        TestClearAddressSpaceResets();
        TestInvalidRangeIsNoOp();
        TestClearAddressSpaceCancelsInFlightRequests();
        TestRereadWindowUsesUnionNotIntersection();
    }
}
