// wpJ3_tests.Lifecycle.cpp
// 作用：验证 WorkbenchBaselineFeeder 自身的生命周期安全——完全空白构造（三个
// 注入点都没设置）不崩溃、QPointer 在画布被外部销毁后自动置空且后续调用不崩溃
// 不喂入、运行中把 overlay/canvas 重新置空再恢复、析构时若恰好有一个尚未到期的
// 防抖定时器不崩溃、lastWindow() 的默认值与被填充后的值、冗余的 setPolicy/
// setAddressSpaceBounds（值与上次相同）仍会强制走一次 Recompute（简单但安全的
// 设计选择，不是 bug——见报告）。

#include "wpJ3_common.h"

#include <memory>

namespace memwb_wpJ3_test
{
    namespace
    {
        constexpr std::uint64_t kBase = 0x50000000ULL;

        // ---- 完全空白构造：三个注入点都没设置，noteDirty/flushNow 都不能崩溃，
        //      也不能产生任何喂入或信号。----
        void CheckBareConstructionIsSafe()
        {
            WorkbenchBaselineFeeder feeder;
            BaselineRecorder recorder;
            recorder.Attach(feeder, feeder);

            feeder.noteDirty(kBase, 1, false);
            feeder.flushNow();
            PumpUntil([]() { return false; }, 100);

            WPJ3_CHECK(recorder.refreshedCount == 0);
            WPJ3_CHECK(recorder.unavailableCount == 0);
            WPJ3_CHECK_NOTE(!feeder.lastWindow().hasWindow, QStringLiteral("从未成功喂入过，lastWindow 应该是默认的空记录"));
        }

        // ---- lastWindow() 的默认值与成功喂入之后的值。----
        void CheckLastWindowAccessor()
        {
            HexCanvas canvas;
            NoOpPageProvider provider;
            SetupCanvas(canvas, provider, 0, 0xFFFFFFFFULL);
            ksword::memwb::MemoryDiffOverlay overlay;
            WorkbenchBaselineFeeder feeder;
            BaselineRecorder recorder;
            feeder.setCanvas(&canvas);
            feeder.setOverlay(&overlay);
            feeder.setAddressSpaceBounds(ksword::memwb::AddressSpaceBounds{0, 0xFFFFFFFFULL});
            recorder.Attach(feeder, feeder);

            WPJ3_CHECK(!feeder.lastWindow().hasWindow);

            WPJ3_CHECK(DeliverValidPage(canvas, kBase, 0x10) == HexCanvas::PageResult::Accepted);
            feeder.noteDirty(kBase, canvas.sourceRevision(), false);
            feeder.flushNow();

            WPJ3_CHECK(feeder.lastWindow().hasWindow);
            WPJ3_CHECK(feeder.lastWindow().base == kBase);
            WPJ3_CHECK(feeder.lastWindow().length == kPageBytes);
            WPJ3_CHECK(feeder.lastWindow().sourceRevision == canvas.sourceRevision());
        }

        // ---- QPointer 悬空保护：画布被外部销毁之后，canvas_ 自动置空，后续
        //      noteDirty/flushNow 必须安全退化成空操作，不解引用悬空指针。----
        void CheckCanvasDestroyedExternallyIsSafe()
        {
            auto canvas = std::make_unique<HexCanvas>();
            NoOpPageProvider provider;
            SetupCanvas(*canvas, provider, 0, 0xFFFFFFFFULL);
            ksword::memwb::MemoryDiffOverlay overlay;
            WorkbenchBaselineFeeder feeder;
            BaselineRecorder recorder;
            feeder.setCanvas(canvas.get());
            feeder.setOverlay(&overlay);
            feeder.setAddressSpaceBounds(ksword::memwb::AddressSpaceBounds{0, 0xFFFFFFFFULL});
            recorder.Attach(feeder, feeder);

            WPJ3_CHECK(DeliverValidPage(*canvas, kBase, 0x10) == HexCanvas::PageResult::Accepted);
            feeder.noteDirty(kBase, canvas->sourceRevision(), false);
            feeder.flushNow();
            WPJ3_CHECK(recorder.refreshedCount == 1);

            // 外部直接销毁画布（不经过 feeder，模拟宿主顺序出错或窗口被提前关闭）。
            canvas.reset();

            feeder.noteDirty(kBase, 999, false);
            feeder.flushNow();
            PumpUntil([]() { return false; }, 100);
            WPJ3_CHECK_NOTE(
                recorder.refreshedCount == 1,
                QStringLiteral("画布已被销毁，QPointer 应该已经自动置空，之后的调用必须是空操作"));
        }

        // ---- 运行中把 overlay 重新置空再恢复：置空期间必须安全退化为空操作，
        //      恢复之后又能正常工作（不是"一旦置空就永久失效"）。----
        void CheckOverlayClearedThenRestored()
        {
            HexCanvas canvas;
            NoOpPageProvider provider;
            SetupCanvas(canvas, provider, 0, 0xFFFFFFFFULL);
            ksword::memwb::MemoryDiffOverlay overlay;
            WorkbenchBaselineFeeder feeder;
            BaselineRecorder recorder;
            feeder.setCanvas(&canvas);
            feeder.setOverlay(&overlay);
            feeder.setAddressSpaceBounds(ksword::memwb::AddressSpaceBounds{0, 0xFFFFFFFFULL});
            recorder.Attach(feeder, feeder);

            WPJ3_CHECK(DeliverValidPage(canvas, kBase, 0x10) == HexCanvas::PageResult::Accepted);
            feeder.noteDirty(kBase, canvas.sourceRevision(), false);
            feeder.flushNow();
            WPJ3_CHECK(recorder.refreshedCount == 1);

            feeder.setOverlay(nullptr);
            feeder.noteDirty(kBase, canvas.sourceRevision(), false);
            feeder.flushNow();
            PumpUntil([]() { return false; }, 100);
            WPJ3_CHECK_NOTE(recorder.refreshedCount == 1, QStringLiteral("overlay 置空期间不应该有任何喂入"));

            feeder.setOverlay(&overlay);
            feeder.noteDirty(kBase + 1, canvas.sourceRevision(), false); // 仍在同一页窗口内，但走一次新的脏事件
            feeder.flushNow();
            // 恢复后：锚点仍在旧窗口内、代次不变，是 Keep（不是喂入），用来证明"恢复
            // 之后没有被永久卡死在空操作"——如果仍然是空操作，unavailableCount 不会动，
            // 但更有力的证据是下面故意换一个新页再验证一次真正的喂入。
            WPJ3_CHECK(recorder.refreshedCount == 1);

            // 换一个新锚点（不同页，强制 Recompute）确认恢复后真的能重新喂入。
            const std::uint64_t otherPage = kBase + kPageBytes;
            WPJ3_CHECK(DeliverValidPage(canvas, otherPage, 0x20) == HexCanvas::PageResult::Accepted);
            feeder.noteDirty(otherPage, canvas.sourceRevision(), false);
            feeder.flushNow();
            WPJ3_CHECK_NOTE(
                recorder.refreshedCount == 2,
                QStringLiteral("恢复 overlay 之后必须能重新正常喂入，不是永久失效"));
        }

        // ---- 析构安全：feeder 在自己的防抖定时器尚未到期时就被销毁，不能崩溃
        //      （QTimer 是 feeder 的子对象，随 feeder 一起销毁，不会有悬空回调）。----
        void CheckDestructorDuringPendingDebounceIsSafe()
        {
            HexCanvas canvas;
            NoOpPageProvider provider;
            SetupCanvas(canvas, provider, 0, 0xFFFFFFFFULL);
            ksword::memwb::MemoryDiffOverlay overlay;
            WPJ3_CHECK(DeliverValidPage(canvas, kBase, 0x10) == HexCanvas::PageResult::Accepted);

            {
                auto feeder = std::make_unique<WorkbenchBaselineFeeder>();
                feeder->setCanvas(&canvas);
                feeder->setOverlay(&overlay);
                feeder->setAddressSpaceBounds(ksword::memwb::AddressSpaceBounds{0, 0xFFFFFFFFULL});
                feeder->noteDirty(kBase, canvas.sourceRevision(), false); // 起算 50ms 防抖，故意不等它到期
                // feeder 在这里被销毁（unique_ptr 离开作用域），定时器随之被父子关系销毁。
            }

            // 等过原定的 50ms，确认没有任何悬空回调造成崩溃（跑到这里就是通过）。
            PumpUntil([]() { return false; }, 150);
            WPJ3_CHECK_NOTE(true, QStringLiteral("feeder 带着未到期的防抖定时器被销毁，之后继续运行没有崩溃"));
        }

        // ---- 冗余的 setAddressSpaceBounds（与当前值完全相同）仍会使记录失效，
        //      强制下一次走一次 Recompute——结果应该与原来相同（同样的窗口），
        //      只是多算了一次，不是 bug，但要确认它确实"能正常重新算出同一个
        //      窗口"而不是因为被重置就出错。----
        void CheckRedundantSameBoundsStillRecomputesCorrectly()
        {
            HexCanvas canvas;
            NoOpPageProvider provider;
            SetupCanvas(canvas, provider, 0, 0xFFFFFFFFULL);
            ksword::memwb::MemoryDiffOverlay overlay;
            WorkbenchBaselineFeeder feeder;
            BaselineRecorder recorder;
            const ksword::memwb::AddressSpaceBounds bounds{0, 0xFFFFFFFFULL};
            feeder.setCanvas(&canvas);
            feeder.setOverlay(&overlay);
            feeder.setAddressSpaceBounds(bounds);
            recorder.Attach(feeder, feeder);

            WPJ3_CHECK(DeliverValidPage(canvas, kBase, 0x10) == HexCanvas::PageResult::Accepted);
            feeder.noteDirty(kBase, canvas.sourceRevision(), false);
            feeder.flushNow();
            WPJ3_CHECK(recorder.refreshedCount == 1);

            feeder.setAddressSpaceBounds(bounds); // 完全相同的值
            feeder.noteDirty(kBase, canvas.sourceRevision(), false);
            feeder.flushNow();
            WPJ3_CHECK_NOTE(
                recorder.refreshedCount == 2,
                QStringLiteral("设置同样的边界仍然使记录失效，所以这里会多算一次（符合预期，不是 bug）"));
            WPJ3_CHECK_NOTE(
                recorder.lastBase == kBase && recorder.lastLength == kPageBytes,
                QStringLiteral("重新算出来的窗口应该和原来完全一样"));
        }
    }

    void RunLifecycleTests()
    {
        CheckBareConstructionIsSafe();
        CheckLastWindowAccessor();
        CheckCanvasDestroyedExternallyIsSafe();
        CheckOverlayClearedThenRestored();
        CheckDestructorDuringPendingDebounceIsSafe();
        CheckRedundantSameBoundsStillRecomputesCorrectly();
    }
}
