// wpJ3_tests.Decision.cpp
// 作用：验证 WorkbenchBaselineFeeder 的核心判定——Keep/RefreshSameSpan/Recompute
// 三分支、插入点未落定/超出地址空间时的 baselineUnavailable、identityKey 变化
// 不得续喂旧身份窗口、setPolicy/setAddressSpaceBounds 使记录失效、窗口跟随跨过
// 页边界/地址空间边界、窗口页数上限、重读后 previous 保留使 ExternalChange 成立。

#include "wpJ3_common.h"

namespace memwb_wpJ3_test
{
    namespace
    {
        // Fixture：地址空间 [0, 0xFFFFFFFF]，所有测试页都放在远离地址 0 的区域
        // （base），避开 setAddressSpace 自动产生的 Pending 页。
        struct Fixture
        {
            HexCanvas canvas;
            NoOpPageProvider provider;
            ksword::memwb::MemoryDiffOverlay overlay;
            WorkbenchBaselineFeeder feeder;
            BaselineRecorder recorder;
            static constexpr std::uint64_t kBase = 0x40000000ULL;

            Fixture()
            {
                SetupCanvas(canvas, provider, 0, 0xFFFFFFFFULL);
                feeder.setCanvas(&canvas);
                feeder.setOverlay(&overlay);
                feeder.setAddressSpaceBounds(ksword::memwb::AddressSpaceBounds{0, 0xFFFFFFFFULL});
                recorder.Attach(feeder, feeder);
            }

            // Flush：先 noteDirty 把指定锚点与"画布当前来源代次"记成待处理请求，
            // 再 flushNow 同步执行一次判定与喂入。本文件几乎每个测试都需要在一个
            // 明确指定的锚点（不是默认的 0）上触发判定，用这个小helper避免每处都
            // 重复写"noteDirty 再 flushNow"这一对调用，也避免漏写 noteDirty 导致
            // 实际用的是构造时默认锚点 0（那是另一类不同的测试，见
            // wpJ3_tests.Debounce.cpp 的 CheckFlushNowWithoutPriorNoteDirty）。
            void Flush(std::uint64_t anchor)
            {
                feeder.noteDirty(anchor, canvas.sourceRevision(), false);
                feeder.flushNow();
            }
        };

        // ---- 孤立的单页窗口：插入点所在页落定，左右邻页都是 NotLoaded，窗口应该
        //      恰好是这一页（不会"脑补"邻页）。----
        void CheckRecomputeSinglePageWindow()
        {
            Fixture fx;
            WPJ3_CHECK(DeliverValidPage(fx.canvas, Fixture::kBase, 0x10) == HexCanvas::PageResult::Accepted);
            fx.Flush(Fixture::kBase);
            WPJ3_CHECK(fx.recorder.refreshedCount == 1);
            WPJ3_CHECK(fx.recorder.lastBase == Fixture::kBase);
            WPJ3_CHECK(fx.recorder.lastLength == kPageBytes);
            WPJ3_CHECK(fx.feeder.lastWindow().hasWindow);
            WPJ3_CHECK(fx.feeder.lastWindow().base == Fixture::kBase);
            WPJ3_CHECK(fx.feeder.lastWindow().length == kPageBytes);
            // 直接核对叠加层里实际装载的窗口，不只信赖信号参数——如果喂给
            // RefreshBaseline 的 base/length 与信号报出来的、或与 record_ 记的不一致
            // （三份本该永远一致），这里能抓到那种"信号说对了、实际装错了"的偏差。
            WPJ3_CHECK(fx.overlay.BaseAddress() == Fixture::kBase);
            WPJ3_CHECK(fx.overlay.BaselineSize() == kPageBytes);
        }

        // ---- Keep：锚点在窗口内移动、来源代次不变，不触发任何喂入；AcceptWrite
        //      标记的 SelfWritten 在 Keep 期间必须原样保留（因为根本没调用
        //      RefreshBaseline，不是"保留"而是"没碰"）。----
        void CheckKeepDoesNotFeedAndPreservesSelfWritten()
        {
            Fixture fx;
            WPJ3_CHECK(DeliverValidPage(fx.canvas, Fixture::kBase, 0x10) == HexCanvas::PageResult::Accepted);
            fx.Flush(Fixture::kBase);
            WPJ3_CHECK(fx.recorder.refreshedCount == 1);

            // 用一次 AcceptWrite 把 kBase 处标记成"自己写入"。
            ksword::memwb::DiffBlock block;
            block.address = Fixture::kBase;
            block.before = {0x10};
            block.after = {0x77};
            const std::vector<std::uint8_t> readBack = {0x77};
            WPJ3_CHECK(fx.overlay.AcceptWrite(block, readBack) == ksword::memwb::AcceptWriteStatus::Ok);
            WPJ3_CHECK(fx.overlay.ChangeKind(Fixture::kBase) == ksword::memwb::ByteChangeKind::SelfWritten);

            // 锚点仍在窗口内（页长 4096，挪到页中部），来源代次不变。
            const std::uint64_t sameRevision = fx.canvas.sourceRevision();
            fx.feeder.noteDirty(Fixture::kBase + 100, sameRevision, false);
            const bool fired = PumpUntil([&]() { return fx.recorder.refreshedCount >= 2; }, 300);
            WPJ3_CHECK_NOTE(!fired, QStringLiteral("窗口内移动、代次不变必须是 Keep，不应该再喂入一次"));
            WPJ3_CHECK(fx.recorder.refreshedCount == 1);
            WPJ3_CHECK_NOTE(
                fx.overlay.ChangeKind(Fixture::kBase) == ksword::memwb::ByteChangeKind::SelfWritten,
                QStringLiteral("Keep 期间叠加层完全没被碰过，SelfWritten 标记应该原样还在"));
        }

        // ---- 插入点所在页未落定：发 baselineUnavailable(InsertionPageNotSettled)，
        //      不调用 RefreshBaseline（refreshedCount 恒为 0）。----
        void CheckInsertionPageNotSettled()
        {
            Fixture fx;
            // 故意什么都不喂：kBase 所在页从头到尾都是 NotLoaded。
            fx.Flush(Fixture::kBase);
            WPJ3_CHECK(fx.recorder.refreshedCount == 0);
            WPJ3_CHECK(fx.recorder.unavailableCount == 1);
            WPJ3_CHECK(
                fx.recorder.lastStatus == ksword::memwb::BaselineSpanStatus::InsertionPageNotSettled);
        }

        // ---- 插入点超出地址空间边界：发 baselineUnavailable(InsertionOutsideSpace)。----
        void CheckInsertionOutsideSpace()
        {
            Fixture fx;
            fx.feeder.setAddressSpaceBounds(ksword::memwb::AddressSpaceBounds{0x1000, 0x2000});
            fx.Flush(0x9999999);
            WPJ3_CHECK(fx.recorder.refreshedCount == 0);
            WPJ3_CHECK(fx.recorder.unavailableCount >= 1);
            WPJ3_CHECK(
                fx.recorder.lastStatus == ksword::memwb::BaselineSpanStatus::InsertionOutsideSpace);
        }

        // ---- 重读（RefreshSameSpan）：位置不变、只是来源代次变了，previous 得以
        //      保留，ExternalChange 对真正变化的字节成立、对没变的字节不成立。----
        void CheckRefreshSameSpanPreservesPreviousForExternalChange()
        {
            Fixture fx;
            QByteArray first(static_cast<int>(kPageBytes), static_cast<char>(0x10));
            WPJ3_CHECK(DeliverCustomPage(fx.canvas, Fixture::kBase, first) == HexCanvas::PageResult::Accepted);

            fx.Flush(Fixture::kBase);
            WPJ3_CHECK(fx.recorder.refreshedCount == 1);
            WPJ3_CHECK_NOTE(!fx.overlay.HasPreviousRead(), QStringLiteral("第一次载入没有旧基线，不该有上次读取"));

            // 模拟一次重读：换新来源代次，同一页只改一个字节（offset 5），其余不变。
            fx.canvas.refresh();
            QByteArray second = first;
            second[5] = static_cast<char>(0x99);
            WPJ3_CHECK(DeliverCustomPage(fx.canvas, Fixture::kBase, second) == HexCanvas::PageResult::Accepted);

            fx.feeder.noteDirty(Fixture::kBase, fx.canvas.sourceRevision(), false);
            const bool fired = PumpUntil([&]() { return fx.recorder.refreshedCount >= 2; }, 1000);
            WPJ3_CHECK(fired);
            WPJ3_CHECK(fx.recorder.refreshedCount == 2);
            WPJ3_CHECK_NOTE(fx.recorder.lastBase == Fixture::kBase, QStringLiteral("RefreshSameSpan 必须原位沿用同一个 base"));
            WPJ3_CHECK_NOTE(fx.recorder.lastLength == kPageBytes, QStringLiteral("RefreshSameSpan 必须原位沿用同一个 length"));

            WPJ3_CHECK_NOTE(fx.overlay.HasPreviousRead(), QStringLiteral("位置不变的重读之后应该有上次读取可供比对"));
            WPJ3_CHECK_NOTE(
                fx.overlay.ChangeKind(Fixture::kBase + 5) == ksword::memwb::ByteChangeKind::ExternalChange,
                QStringLiteral("真正变化的字节（0x10->0x99）应该判为外部变化（青色）"));
            WPJ3_CHECK_NOTE(
                fx.overlay.ChangeKind(Fixture::kBase + 6) == ksword::memwb::ByteChangeKind::Unchanged,
                QStringLiteral("没变的字节不该被误判为外部变化"));
        }

        // ---- identityKey 变化：不得续喂旧身份的窗口。换身份后用旧锚点 noteDirty，
        //      必须走 Recompute（而不是 Keep）并用新身份重新喂入，overlay 的
        //      IdentityKey() 要切换到新值，不能停在旧值上。----
        void CheckIdentityChangeForcesRecompute()
        {
            Fixture fx;
            fx.feeder.setIdentityKey("process-A");
            WPJ3_CHECK(DeliverValidPage(fx.canvas, Fixture::kBase, 0x10) == HexCanvas::PageResult::Accepted);
            fx.Flush(Fixture::kBase);
            WPJ3_CHECK(fx.recorder.refreshedCount == 1);
            WPJ3_CHECK(fx.overlay.IdentityKey() == std::string("process-A"));

            fx.feeder.setIdentityKey("process-B");
            // 用和旧窗口完全相同的锚点与来源代次——如果 record 没失效，
            // DecideBaselineRefeed 会判定 Keep（锚点仍在窗口内、代次未变、页仍落定），
            // 那就永远不会用新身份重新喂入。
            fx.feeder.noteDirty(Fixture::kBase, fx.canvas.sourceRevision(), false);
            const bool fired = PumpUntil([&]() { return fx.recorder.refreshedCount >= 2; }, 1000);
            WPJ3_CHECK_NOTE(fired, QStringLiteral("身份变化后必须重新喂入一次，不能停在 Keep"));
            WPJ3_CHECK(fx.recorder.refreshedCount == 2);
            WPJ3_CHECK_NOTE(
                fx.overlay.IdentityKey() == std::string("process-B"),
                QStringLiteral("叠加层的基线身份必须切换到新身份，不能停在旧身份上"));
        }

        // ---- 相同身份串重复设置：不打断、不清记录（下一次仍然可以是 Keep）。----
        void CheckSameIdentityIsNoOp()
        {
            Fixture fx;
            fx.feeder.setIdentityKey("process-A");
            WPJ3_CHECK(DeliverValidPage(fx.canvas, Fixture::kBase, 0x10) == HexCanvas::PageResult::Accepted);
            fx.Flush(Fixture::kBase);
            WPJ3_CHECK(fx.recorder.refreshedCount == 1);

            fx.feeder.setIdentityKey("process-A"); // 同值重复设置
            fx.feeder.noteDirty(Fixture::kBase, fx.canvas.sourceRevision(), false);
            const bool fired = PumpUntil([&]() { return fx.recorder.refreshedCount >= 2; }, 300);
            WPJ3_CHECK_NOTE(!fired, QStringLiteral("身份串没变时设置 record 不应该被打断，应该仍是 Keep"));
            WPJ3_CHECK(fx.recorder.refreshedCount == 1);
        }

        // ---- setAddressSpaceBounds 使记录失效：换到一段完全不重叠的新边界后，用
        //      旧锚点 noteDirty 必须走 Recompute 并因越界报 baselineUnavailable，
        //      而不是对着已经不属于新边界的旧窗口返回 Keep。----
        void CheckSetAddressSpaceBoundsInvalidatesRecord()
        {
            Fixture fx;
            WPJ3_CHECK(DeliverValidPage(fx.canvas, Fixture::kBase, 0x10) == HexCanvas::PageResult::Accepted);
            fx.Flush(Fixture::kBase);
            WPJ3_CHECK(fx.recorder.refreshedCount == 1);

            // 换到一段与旧窗口完全不重叠的新边界（模拟换了目标范围）。
            const std::uint64_t newLow = Fixture::kBase + 0x10000000ULL;
            const std::uint64_t newHigh = newLow + 0xFFFFFULL;
            fx.feeder.setAddressSpaceBounds(ksword::memwb::AddressSpaceBounds{newLow, newHigh});

            // 用旧锚点（已经不在新边界内）发一次脏事件：如果记录没失效，锚点仍"落在
            // 旧窗口内"会被判 Keep；记录失效后必须走 Recompute，而旧锚点在新边界之外，
            // SelectBaselineSpan 必然拒绝为 InsertionOutsideSpace。
            fx.Flush(Fixture::kBase);
            WPJ3_CHECK_NOTE(
                fx.recorder.refreshedCount == 1,
                QStringLiteral("越界的旧锚点不应该产生新的成功喂入"));
            WPJ3_CHECK_NOTE(
                fx.recorder.unavailableCount >= 1,
                QStringLiteral("record 必须失效并重新判定，不能对着新边界之外的旧窗口返回 Keep"));
            WPJ3_CHECK(fx.recorder.lastStatus == ksword::memwb::BaselineSpanStatus::InsertionOutsideSpace);
        }

        // ---- 独立审核 D4：换地址空间（相当于换目标）之后，旧的待处理锚点/代次不
        //      能继续生效——否则 flushNow 会拿着旧目标的锚点在新地址空间上误判
        //      （探针 P4a），或者旧目标起算的防抖定时器到期后用旧锚点在新目标上
        //      喂入（探针 P4b）。本测试同时覆盖这两条路径。----
        void CheckRedirectClearsPendingAnchorAndTimer()
        {
            Fixture fx;
            WPJ3_CHECK(DeliverValidPage(fx.canvas, Fixture::kBase, 0x10) == HexCanvas::PageResult::Accepted);
            // 起算一次防抖但不等它到期：模拟"旧目标刚发生过一次脏事件，防抖定时器
            // 正在跑"这一瞬间恰好换了目标。
            fx.feeder.noteDirty(Fixture::kBase, fx.canvas.sourceRevision(), false);

            // 换到一段与旧窗口完全不重叠的新地址空间（相当于换目标），并在新范围
            // 里立刻喂好一页——这一页与旧锚点毫无关系，如果旧锚点继续生效，判定会
            // 落在这页之外报错，而不是用新锚点命中它。
            const std::uint64_t newLow = Fixture::kBase + 0x20000000ULL;
            const std::uint64_t newHigh = newLow + 0xFFFFFULL;
            fx.feeder.setAddressSpaceBounds(ksword::memwb::AddressSpaceBounds{newLow, newHigh});
            WPJ3_CHECK(DeliverValidPage(fx.canvas, newLow, 0x20) == HexCanvas::PageResult::Accepted);

            // 探针 P4b：旧目标起算的防抖定时器必须已经被取消——等过原定的 50ms，不
            // 应该出现任何用旧锚点自动判定产生的信号（成功喂入或 unavailable 都算）。
            const bool firedByOldTimer = PumpUntil(
                [&]() { return fx.recorder.refreshedCount >= 1 || fx.recorder.unavailableCount >= 1; }, 150);
            WPJ3_CHECK_NOTE(
                !firedByOldTimer,
                QStringLiteral("换地址空间必须取消旧目标起算的防抖定时器，不能用旧锚点自动判定新目标"));

            // 探针 P4a：flushNow 在没有新 noteDirty 的情况下也必须是空操作——不会用
            // 清空前残留的旧锚点在新地址空间上判定（那会报出虚假的 InsertionOutsideSpace）。
            fx.feeder.flushNow();
            WPJ3_CHECK_NOTE(
                fx.recorder.refreshedCount == 0 && fx.recorder.unavailableCount == 0,
                QStringLiteral("换目标后 flushNow 不该用旧锚点在新地址空间上判定，必须等真正的 noteDirty"));

            // 真正的 noteDirty 到来后，应该能在新地址空间上正常喂入。
            fx.feeder.noteDirty(newLow, fx.canvas.sourceRevision(), false);
            fx.feeder.flushNow();
            WPJ3_CHECK(fx.recorder.refreshedCount == 1);
            WPJ3_CHECK(fx.recorder.lastBase == newLow);
        }

        // ---- 独立审核 D4（身份通道）：换 identityKey（相当于换目标）之后，旧的
        //      待处理锚点/代次同样不能继续生效——与 setAddressSpaceBounds 的那条
        //      测试并行覆盖，确保 setIdentityKey 里清空 pending 三项的那部分代码
        //      也真的被测试覆盖到（不是只有地址空间通道有牙齿）。----
        void CheckIdentityRedirectClearsPendingAnchorAndTimer()
        {
            Fixture fx;
            fx.feeder.setIdentityKey("A");
            WPJ3_CHECK(DeliverValidPage(fx.canvas, Fixture::kBase, 0x10) == HexCanvas::PageResult::Accepted);
            // 起算一次防抖但不等它到期，模拟"旧身份刚发生过一次脏事件"这一瞬间换身份。
            fx.feeder.noteDirty(Fixture::kBase, fx.canvas.sourceRevision(), false);

            fx.feeder.setIdentityKey("B"); // 换身份：相当于换目标

            // 旧身份起算的防抖定时器必须已经被取消；等过原定的 50ms，不应该出现任何
            // 用旧锚点自动判定产生的信号。
            const bool firedByOldTimer = PumpUntil(
                [&]() { return fx.recorder.refreshedCount >= 1 || fx.recorder.unavailableCount >= 1; }, 150);
            WPJ3_CHECK_NOTE(
                !firedByOldTimer,
                QStringLiteral("换 identityKey 必须取消旧身份起算的防抖定时器，不能自动用旧锚点判定"));

            // flushNow 在没有新 noteDirty 的情况下也必须是空操作。
            fx.feeder.flushNow();
            WPJ3_CHECK_NOTE(
                fx.recorder.refreshedCount == 0 && fx.recorder.unavailableCount == 0,
                QStringLiteral("换 identityKey 后 flushNow 不该用旧的待处理请求判定，必须等真正的 noteDirty"));

            // 真正的 noteDirty 到来后，应该能用新身份正常喂入。
            fx.feeder.noteDirty(Fixture::kBase, fx.canvas.sourceRevision(), false);
            fx.feeder.flushNow();
            WPJ3_CHECK(fx.recorder.refreshedCount == 1);
            WPJ3_CHECK(fx.overlay.IdentityKey() == std::string("B"));
        }

        // ---- setPolicy 使记录失效：扩大 maxPages 之后，即便锚点/代次都没变，也
        //      必须立刻按新上限重新选取一次更大的窗口，而不是停留在旧上限选出的
        //      窄窗口上（Keep 会让这次放宽设置的效果被无限期推迟）。----
        void CheckSetPolicyInvalidatesRecordAndWindowGrows()
        {
            Fixture fx;
            fx.feeder.setPolicy(ksword::memwb::BaselineWindowPolicy{kPageBytes, 1});

            // 连续 5 页都落定，但初始策略上限为 1 页。
            for (int i = -2; i <= 2; ++i)
            {
                const std::uint64_t pageStart = Fixture::kBase + static_cast<std::uint64_t>(i) * kPageBytes;
                WPJ3_CHECK(DeliverValidPage(fx.canvas, pageStart, 0x10) == HexCanvas::PageResult::Accepted);
            }

            fx.Flush(Fixture::kBase);
            WPJ3_CHECK(fx.recorder.refreshedCount == 1);
            WPJ3_CHECK_NOTE(fx.recorder.lastLength == kPageBytes, QStringLiteral("上限为 1 页时窗口应该恰好是 1 页"));

            // 放宽上限到 5 页，锚点与来源代次都不变。
            fx.feeder.setPolicy(ksword::memwb::BaselineWindowPolicy{kPageBytes, 5});
            fx.feeder.noteDirty(Fixture::kBase, fx.canvas.sourceRevision(), false);
            const bool fired = PumpUntil([&]() { return fx.recorder.refreshedCount >= 2; }, 1000);
            WPJ3_CHECK_NOTE(fired, QStringLiteral("放宽上限后必须立刻重新选取一次，不能停在 Keep"));
            WPJ3_CHECK(fx.recorder.refreshedCount == 2);
            WPJ3_CHECK_NOTE(
                fx.recorder.lastLength == 5 * kPageBytes,
                QStringLiteral("新上限是 5 页，且 5 页全部落定，窗口应该长到 5 页"));
        }

        // ---- 窗口跟随跨过页边界：锚点从一页滚到相邻的下一页（都已落定），即便
        //      来源代次不变，也必须 Recompute 到新锚点所在的页，不能停在旧窗口。----
        void CheckWindowFollowsAcrossPageBoundary()
        {
            Fixture fx;
            fx.feeder.setPolicy(ksword::memwb::BaselineWindowPolicy{kPageBytes, 1}); // 窄窗口，便于判断"跟没跟"
            const std::uint64_t pageA = Fixture::kBase;
            const std::uint64_t pageB = Fixture::kBase + kPageBytes;
            WPJ3_CHECK(DeliverValidPage(fx.canvas, pageA, 0x10) == HexCanvas::PageResult::Accepted);
            WPJ3_CHECK(DeliverValidPage(fx.canvas, pageB, 0x20) == HexCanvas::PageResult::Accepted);

            fx.Flush(pageA);
            WPJ3_CHECK(fx.recorder.lastBase == pageA);

            // 锚点挪到下一页，来源代次不变——anchorInside(record=pageA 窗口) 应该为假，
            // 必须 Recompute 跟过去。
            fx.feeder.noteDirty(pageB + 10, fx.canvas.sourceRevision(), false);
            const bool fired = PumpUntil([&]() { return fx.recorder.refreshedCount >= 2; }, 1000);
            WPJ3_CHECK_NOTE(fired, QStringLiteral("锚点跨到相邻页之后必须重新喂入一次"));
            WPJ3_CHECK_NOTE(fx.recorder.lastBase == pageB, QStringLiteral("窗口必须跟到锚点所在的新页"));
        }

        // ---- 窗口跟随跨过地址空间边界：边界恰好只容纳 2 页时，即便策略允许更多
        //      页，窗口也只能到边界为止，不会试图越界。----
        void CheckWindowStopsAtAddressSpaceBoundary()
        {
            Fixture fx;
            fx.feeder.setPolicy(ksword::memwb::BaselineWindowPolicy{kPageBytes, 256});
            const std::uint64_t pageA = Fixture::kBase;
            const std::uint64_t pageB = Fixture::kBase + kPageBytes;
            // 边界恰好只有这两页，哪怕策略允许多到 256 页。
            fx.feeder.setAddressSpaceBounds(ksword::memwb::AddressSpaceBounds{pageA, pageB + kPageBytes - 1});
            WPJ3_CHECK(DeliverValidPage(fx.canvas, pageA, 0x10) == HexCanvas::PageResult::Accepted);
            WPJ3_CHECK(DeliverValidPage(fx.canvas, pageB, 0x20) == HexCanvas::PageResult::Accepted);

            fx.Flush(pageA);
            WPJ3_CHECK(fx.recorder.refreshedCount == 1);
            WPJ3_CHECK_NOTE(
                fx.recorder.lastLength == 2 * kPageBytes,
                QStringLiteral("地址空间只有 2 页，窗口不该超出边界去装一个不存在的第三页"));
        }
    }

    void RunDecisionTests()
    {
        CheckRecomputeSinglePageWindow();
        CheckKeepDoesNotFeedAndPreservesSelfWritten();
        CheckInsertionPageNotSettled();
        CheckInsertionOutsideSpace();
        CheckRefreshSameSpanPreservesPreviousForExternalChange();
        CheckIdentityChangeForcesRecompute();
        CheckSameIdentityIsNoOp();
        CheckSetAddressSpaceBoundsInvalidatesRecord();
        CheckRedirectClearsPendingAnchorAndTimer();
        CheckIdentityRedirectClearsPendingAnchorAndTimer();
        CheckSetPolicyInvalidatesRecordAndWindowGrows();
        CheckWindowFollowsAcrossPageBoundary();
        CheckWindowStopsAtAddressSpaceBoundary();
    }
}
