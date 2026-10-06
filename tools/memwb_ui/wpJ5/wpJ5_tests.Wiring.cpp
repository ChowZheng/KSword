// ============================================================
// wpJ5_tests.Wiring.cpp
// 作用：覆盖"三条管线任一缺失仍能构造并显示'无数据'初态，不崩溃"、三个
// setXxxProvider/Feeder/Controller 的接线是否真的生效（通过观察接线之后的
// 行为，而不是猜测内部调用了什么）、增量①（hasInFlightRequests/jobLanded）
// 的读数，以及"管线先于本类销毁"这一正常析构顺序下的安全性。
// ============================================================

#include "wpJ5_common.h"

#include <QMenu>
#include <QSignalSpy>

namespace wpj5_test
{
    namespace
    {
        // T1：裸构造一个 WorkbenchHexPane，不注入任何管线——对应"三条管线任一
        // 为空时仍能构造并显示无数据初态，不崩溃"的最极端情形（三条都没有）。
        void TestBareConstructionNoCrash()
        {
            ks::ui::WorkbenchHexPane pane;
            WPJ5_CHECK_NOTE(pane.canvas() != nullptr, "画布必须在构造时就建好");
            WPJ5_CHECK_NOTE(pane.inspector() != nullptr, "解释器面板必须在构造时就建好");
            WPJ5_CHECK_NOTE(pane.findBar() != nullptr, "查找条必须在构造时就建好");
            WPJ5_CHECK(pane.insertionAddress() == 0);
            // 没有 setAddressSpace 过：jumpTo 必须明确返回 false，不静默失败。
            WPJ5_CHECK(pane.jumpTo(0x1000) == false);
            // setEditable/rereadWindow/openFind/closeFindBar 在没有任何管线、
            // 没有地址空间的情况下都不应该崩溃。
            pane.setEditable(true);
            pane.rereadWindow();
            pane.openFind();
            pane.closeFindBar();
            WPJ5_CHECK_NOTE(true, "裸构造 + 全套空状态操作均未崩溃（能执行到这里就是通过）");
        }

        // T2：setAddressSpace 在没有任何管线注入时仍然要真正生效于画布本身
        // （画布不依赖三条管线才能"有地址空间"），jumpTo 到空间内的地址应返回
        // true（即使那里从未读到任何字节——jumpTo 只问"是否在地址空间内"）。
        void TestAddressSpaceWithoutPipelines()
        {
            ks::ui::WorkbenchHexPane pane;
            pane.setAddressSpace(0x1000, 0x1FFF, "bare-identity");
            WPJ5_CHECK(pane.jumpTo(0x1500) == true);
            WPJ5_CHECK(pane.insertionAddress() == 0x1500);
            WPJ5_CHECK(pane.jumpTo(0x500) == false); // 不在地址空间内
            pane.clearAddressSpace();
            WPJ5_CHECK(pane.jumpTo(0x1500) == false); // 清空之后同一地址不再合法
        }

        // T3：setPageProvider/setBaselineFeeder/setWriteController 传 nullptr
        // （显式解除接线）不崩溃，且之后 insertionAddress/rereadWindow 等仍能
        // 正常工作（画布本身不受影响）。
        void TestNullptrSettersSafe()
        {
            ks::ui::WorkbenchHexPane pane;
            pane.setPageProvider(nullptr);
            pane.setBaselineFeeder(nullptr);
            pane.setWriteController(nullptr);
            pane.setAddressSpace(0, 0xFFF, "id");
            pane.rereadWindow(); // pageProvider_ 为空，必须是空操作，不崩溃
            WPJ5_CHECK(pane.jumpTo(0x10) == true);
        }

        // T3b：rereadWindow 在 pageProvider_ 为空时必须是空操作——专门构造
        // "haveRange 确实为真"的场景再拔掉 provider，不依赖"裸 pane 从未
        // show() 过，可见范围缓存可能仍是初值"这类不确定前提（T3 本身没有
        // show()，hasVisibleRange_ 是否已经被置位取决于布局是否已经跑过一轮，
        // 不够确定）。做法：先用一个真实 Harness 建立起真实的可见范围缓存
        // （等画布真读到字节、visibleRangeChanged 真发过），再显式
        // pane.setPageProvider(nullptr) 拔掉管线，这时 rereadWindow() 内部
        // 的 haveRange 必然为真（可见范围缓存还在），如果 "pageProvider_ 为空
        // 时提前返回" 这道判空被去掉，一定会经过 `pageProvider_->rereadByteRange`
        // 解引用空指针。
        void TestRereadWindowNullProviderAfterVisibleRangeEstablished()
        {
            Harness h(0, std::vector<std::uint8_t>(0x1000, 0x5A));
            h.AttachProcess();
            h.SetAddressSpace(0, 0xFFF);
            WPJ5_CHECK(h.WaitUntilSettled(0, 2000));
            // 到这里 onCanvasVisibleRangeChanged 必然至少触发过一次
            // （installSpace 内部同步调用 notifyVisibleRange），hasVisibleRange_
            // 已经是真——这正是本测试要的确定性前提。

            h.pane.setPageProvider(nullptr); // 拔掉管线，但可见范围缓存仍然有效
            h.pane.rereadWindow(); // 不应该崩溃
            WPJ5_CHECK_NOTE(true, "pageProvider_ 拔掉之后、可见范围缓存仍有效时，rereadWindow 必须安全返回");
        }

        // T3c：画布的 editRejected/contextMenuAboutToShow 必须原样转发成本类的
        // 同名信号（buildUi 里是信号转信号的直接 connect，没有任何业务逻辑，
        // 容易被改动时不小心漏连或改错参数——用真实触发核对，不只是读源码里
        // 有没有 connect 这一行）。
        void TestEditRejectedAndContextMenuForwarding()
        {
            ks::ui::WorkbenchHexPane pane;
            pane.setAddressSpace(0, 0xFFF, "forward-test");

            QSignalSpy rejectedSpy(&pane, &ks::ui::WorkbenchHexPane::editRejected);
            // 画布默认不可编辑（没有调用过 setEditable(true)），任何 stageBytes
            // 尝试都会被拒绝并发 editRejected——不需要装配任何管线。
            QString reason;
            const bool staged = pane.canvas()->stageBytes(0x10, QByteArray::fromHex("AA"), &reason);
            WPJ5_CHECK(!staged);
            WPJ5_CHECK_NOTE(rejectedSpy.count() == 1, "画布的 editRejected 必须原样转发成本类的 editRejected");

            QSignalSpy menuSpy(&pane, &ks::ui::WorkbenchHexPane::contextMenuAboutToShow);
            QMenu* menu = pane.canvas()->buildContextMenu(0x10, true);
            WPJ5_CHECK(menu != nullptr);
            WPJ5_CHECK_NOTE(
                menuSpy.count() == 1, "画布的 contextMenuAboutToShow 必须原样转发成本类的 contextMenuAboutToShow");
            delete menu;
        }

        // T4：完整装配下，setPageProvider 的接线必须真的生效——验证方式是端到端
        // 行为而不是猜测内部调用：setAddressSpace 之后画布会自动请求可见页
        // （HexCanvas::installSpace 内部调用 requestVisiblePages），如果
        // WorkbenchHexPane 没有把 provider 接到画布（canvas_->setPageProvider），
        // 这次请求会石沉大海，WaitUntilSettled 必然超时。
        void TestPageProviderWiringEndToEnd()
        {
            Harness h(0, std::vector<std::uint8_t>(0x10000, 0xAA));
            h.AttachProcess();
            h.SetAddressSpace(0, 0xFFFF);
            WPJ5_CHECK_NOTE(h.WaitUntilSettled(0, 2000), "setPageProvider 接线生效：地址 0 处的页应当被真正读到");
            WPJ5_CHECK(h.pane.canvas()->cellStateAt(0).hasValue);
            WPJ5_CHECK(h.pane.canvas()->cellStateAt(0).value == 0xAA);
        }

        // T5：setBaselineFeeder 的接线必须真的把 canvas_/overlay_ 传给了 feeder
        // ——验证方式同样是端到端行为：先让画布读到数据，再显式移动插入点（驱动
        // noteDirty），flushNow() 强制立即执行；如果 WorkbenchHexPane 没有调用
        // feeder.setCanvas/setOverlay，flushNow 会因为"任一为空直接忽略"而永远
        // 不发 baselineRefreshed，overlay 也永远不会有基线。
        void TestBaselineFeederWiringEndToEnd()
        {
            Harness h(0, std::vector<std::uint8_t>(0x10000, 0xBB));
            h.AttachProcess();
            h.SetAddressSpace(0, 0xFFFF);
            WPJ5_CHECK(h.WaitUntilSettled(0, 2000));

            int refreshCount = 0;
            QObject::connect(&h.feeder, &ks::ui::WorkbenchBaselineFeeder::baselineRefreshed, &h.feeder,
                [&refreshCount](quint64, quint64) { ++refreshCount; });

            h.pane.canvas()->setCaretAddress(0, false, false); // 触发 onCanvasCaretMoved -> noteDirty
            h.feeder.flushNow();
            WPJ5_CHECK_NOTE(refreshCount >= 1, "setBaselineFeeder 接线生效：flushNow 应当真正喂入一次基线");
            WPJ5_CHECK(h.pane.overlay().HasBaseline());
            WPJ5_CHECK(h.pane.overlay().BaselineSize() > 0);
        }

        // T6：setWriteController 的接线必须真的把 overlay_ 传给了 controller——
        // 验证方式：对 overlay 暂存一个字节后调用 controller.onEditCompleted()，
        // 若 WorkbenchHexPane 没有调用 controller.setOverlay(&overlay_)，
        // ensureTransaction() 会因为 overlay_ 仍是 nullptr 而失败，commitFinished
        // 不会发出。
        void TestWriteControllerWiringEndToEnd()
        {
            Harness h(0, std::vector<std::uint8_t>(0x1000, 0x11));
            h.AttachProcess();
            h.SetAddressSpace(0, 0xFFF);
            WPJ5_CHECK(h.WaitUntilSettled(0, 2000));

            // Stage 要求地址落在 overlay 当前基线窗口内；本测试只关心
            // controller 的接线，不需要走真实的 BaselineFeeder 异步链路，
            // 直接给 overlay 载入一段基线即可（与 BaselineFeeder 的接线由
            // T5 单独覆盖）。
            WPJ5_CHECK(
                h.pane.overlay().LoadBaseline(h.CurrentIdentityKey(), 0, {0x11}, {1})
                == ksword::memwb::BaselineLoadStatus::Ok);

            const ksword::memwb::StageStatus stage = h.pane.overlay().Stage(0, {0x22});
            WPJ5_CHECK(stage == ksword::memwb::StageStatus::Ok);
            h.controller.onEditCompleted();
            WPJ5_CHECK_NOTE(h.commitFinishedCount >= 1, "setWriteController 接线生效：commitFinished 应当被发出");
        }

        // T7：增量①——hasInFlightRequests 在请求提交后立即为真（同步可见，不需要
        // 等事件循环），落地之后变回假；jobLanded 恰好随之发出。
        void TestHasInFlightAndJobLanded()
        {
            Harness h(0, std::vector<std::uint8_t>(0x10000, 0xCC));
            h.AttachProcess();
            WPJ5_CHECK(h.provider.hasInFlightRequests() == false);

            h.SetAddressSpace(0, 0xFFFF); // installSpace 内部同步发起 RequestPages
            WPJ5_CHECK_NOTE(
                h.provider.hasInFlightRequests() == true,
                "RequestPages 提交之后、落地之前，hasInFlightRequests 必须同步可见为真");

            const int before = h.jobLandedCount;
            WPJ5_CHECK(h.WaitUntilNoInFlight(2000));
            WPJ5_CHECK_NOTE(h.jobLandedCount > before, "页请求落地必须发出 jobLanded");
        }

        // T8：正常析构顺序（管线先于本类销毁）：Harness 自身的声明顺序已经保证
        // provider/feeder/controller 先于 pane 析构（见 wpJ5_common.h 的顺序
        // 注释）；这里额外验证"仅销毁三条管线、保留 pane"时 pane 不崩溃——
        // QPointer 会自动探活，pane 的后续调用全部判空。
        void TestPipelinesDestroyedBeforePaneSafe()
        {
            auto provider = std::make_unique<ks::ui::WorkbenchPageProvider>(
                []() -> std::unique_ptr<ksword::memwb::IMemoryIoPort> { return nullptr; }, nullptr);
            auto feeder = std::make_unique<ks::ui::WorkbenchBaselineFeeder>();
            auto controller = std::make_unique<ks::ui::WorkbenchWriteController>();
            auto pane = std::make_unique<ks::ui::WorkbenchHexPane>();

            pane->setPageProvider(provider.get());
            pane->setBaselineFeeder(feeder.get());
            pane->setWriteController(controller.get());
            pane->setAddressSpace(0, 0xFF, "id");

            // 按正常顺序销毁：三条管线先死。
            controller.reset();
            feeder.reset();
            provider.reset();

            // pane 仍然活着，继续调用它的公开方法不应该崩溃（QPointer 已探活为空）。
            pane->setEditable(true);
            pane->rereadWindow();
            WPJ5_CHECK(pane->jumpTo(0x10) == true);
            pane.reset();
            WPJ5_CHECK_NOTE(true, "管线先销毁、pane 后销毁：全程未崩溃");
        }
    }

    void RunWiringTests()
    {
        TestBareConstructionNoCrash();
        TestAddressSpaceWithoutPipelines();
        TestNullptrSettersSafe();
        TestRereadWindowNullProviderAfterVisibleRangeEstablished();
        TestEditRejectedAndContextMenuForwarding();
        TestPageProviderWiringEndToEnd();
        TestBaselineFeederWiringEndToEnd();
        TestWriteControllerWiringEndToEnd();
        TestHasInFlightAndJobLanded();
        TestPipelinesDestroyedBeforePaneSafe();
    }
}
