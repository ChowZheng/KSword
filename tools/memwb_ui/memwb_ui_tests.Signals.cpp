// memwb_ui_tests.Signals.cpp
// 作用：第二轮接口补全的"画布侧"离屏验证（第一组）与公共设施定义——
//   1) contentChanged：排队、同一轮事件循环内合并只发一次、变化已落地才发、槽内再改会再排、不该发时不发、
//      画布销毁时排队的调用作废；
//   2) editableChanged：只在值真的变了才发。
// 其余分组见 memwb_ui_signals.h 的文件分工：Stage（stageBytes）、View（视口命中/占位符/图标）、
// Inspector（面板侧信号接线）。总入口 RunSignalTests 在本文件末尾，依次调用四组。
// 全部手势经 QTest 模拟的真实输入或真实的公开接口进入画布，结果从公开访问器与信号读回。

#include "memwb_ui_signals.h"

#include <QApplication>
#include <QSignalSpy>

#include <iostream>

namespace memwb_test
{
    // 处理一轮事件，让排队的 contentChanged 发出。
    void Flush()
    {
        QApplication::processEvents();
    }

    // 按文字在菜单里找动作，找不到返回空指针。
    QAction* FindAction(QMenu* menu, const QString& text)
    {
        for (QAction* action : menu->actions())
        {
            if (action->text() == text)
            {
                return action;
            }
        }
        return nullptr;
    }

    // 叠加后的字节视图。
    std::vector<std::uint8_t> Effective(
        const ksword::memwb::MemoryDiffOverlay& overlay,
        std::uint64_t address,
        std::uint64_t length)
    {
        return overlay.Materialize(address, length).bytes;
    }

    // 构造地址空间 [first, last] 的异步画布。
    std::unique_ptr<AsyncCanvas> MakeAsyncCanvas(std::uint64_t first, std::uint64_t last, bool editable, bool loadBaseline)
    {
        auto holder = std::make_unique<AsyncCanvas>();
        holder->canvas = std::make_unique<ks::ui::HexCanvas>();
        holder->canvas->resize(900, 420);
        holder->canvas->show();
        holder->canvas->setAddressSpace(first, last);
        holder->canvas->setPageProvider(&holder->provider);
        holder->canvas->setOverlay(&holder->overlay);
        holder->canvas->setEditable(editable);
        if (loadBaseline)
        {
            const std::uint64_t size = last - first + 1ULL;
            const QByteArray pattern = MakePattern(static_cast<int>(size));
            const std::vector<std::uint8_t> bytes(
                reinterpret_cast<const std::uint8_t*>(pattern.constData()),
                reinterpret_cast<const std::uint8_t*>(pattern.constData()) + pattern.size());
            holder->overlay.LoadBaseline("memwb-signals", first, bytes, std::vector<std::uint8_t>(bytes.size(), 1));
        }
        Flush();
        return holder;
    }

    // 向画布投递一页有效数据（MakePattern 内容）。
    ks::ui::HexCanvas::PageResult DeliverPage(ks::ui::HexCanvas& canvas, std::uint64_t pageStart)
    {
        return canvas.deliverPage(pageStart, MakePattern(4096), QByteArray(4096, '\x01'), canvas.sourceRevision());
    }

    // 区域内与给定底色不同的像素数。
    int InkPixels(const QImage& image, const QRect& rect, const QColor& background)
    {
        int ink = 0;
        const QRect clipped = rect.intersected(image.rect());
        for (int y = clipped.top(); y <= clipped.bottom(); ++y)
        {
            for (int x = clipped.left(); x <= clipped.right(); ++x)
            {
                ink += (image.pixelColor(x, y) != background) ? 1 : 0;
            }
        }
        return ink;
    }

    // 两个单元格里字形形状的差异（与文字颜色无关）。
    int InkDifference(
        const QImage& leftImage,
        const QRect& leftRect,
        const QImage& rightImage,
        const QRect& rightRect,
        const QColor& background)
    {
        int different = 0;
        for (int y = 0; y < leftRect.height() && y < rightRect.height(); ++y)
        {
            for (int x = 0; x < leftRect.width() && x < rightRect.width(); ++x)
            {
                const bool leftInk = leftImage.pixelColor(leftRect.left() + x, leftRect.top() + y) != background;
                const bool rightInk = rightImage.pixelColor(rightRect.left() + x, rightRect.top() + y) != background;
                different += (leftInk != rightInk) ? 1 : 0;
            }
        }
        return different;
    }

    namespace
    {
        using ks::ui::HexCanvas;

        // contentChanged 的排队与合并：多次触发只发一次、变化落地后才发、不该发时不发、槽内再改会再排、销毁安全。
        void TestContentChangedCoalescing()
        {
            ApplyTheme(false);
            HexCanvas canvas;
            RecordingProvider provider;
            canvas.resize(800, 360);
            canvas.show();
            QSignalSpy spy(&canvas, &HexCanvas::contentChanged);

            // 换空间 + 换提供者 = 两次触发：排队，调用返回时还没发，处理一轮事件后恰好一个。
            canvas.setAddressSpace(0x100000, 0x100000 + 64ULL * 4096ULL - 1ULL);
            canvas.setPageProvider(&provider);
            CHECK(spy.count() == 0);
            Flush();
            CHECK(spy.count() == 1);
            Flush();
            CHECK(spy.count() == 1);

            // 异步供页：一口气回填 50 页（信号风暴场景），事件循环之前一个都没发，之后恰好一个。
            const std::uint64_t revision = canvas.sourceRevision();
            bool allAccepted = true;
            for (std::uint64_t page = 0; page < 50; ++page)
            {
                allAccepted = allAccepted
                    && canvas.deliverPage(0x100000 + page * 4096ULL, MakePattern(4096), QByteArray(4096, '\x01'), revision)
                        == HexCanvas::PageResult::Accepted;
            }
            CHECK(allAccepted);
            CHECK(spy.count() == 1);
            Flush();
            CHECK(spy.count() == 2);
            Flush();
            CHECK(spy.count() == 2);

            // 整页不可读同样触发（一个信号）。
            ks::ui::HexFetchRange unreadable;
            unreadable.firstPageStart = 0x100000 + 60ULL * 4096ULL;
            unreadable.pageCount = 2;
            CHECK(canvas.deliverUnreadable(unreadable, revision) == HexCanvas::PageResult::Accepted);
            Flush();
            CHECK(spy.count() == 3);

            // 被拒收的回填（陈旧代次、空间之外）不改任何显示，不发信号。
            CHECK(canvas.deliverPage(0x100000, MakePattern(4096), QByteArray(4096, '\x01'), revision + 12345ULL)
                == HexCanvas::PageResult::RejectedStaleRevision);
            CHECK(canvas.deliverPage(0x900000, MakePattern(4096), QByteArray(4096, '\x01'), revision)
                != HexCanvas::PageResult::Accepted);
            Flush();
            CHECK(spy.count() == 3);

            // refresh：连续多次也只发一次。
            canvas.refresh();
            canvas.refresh();
            canvas.refresh();
            Flush();
            CHECK(spy.count() == 4);

            // 选区/插入点/滚动/行宽/分组/高亮层：没有改任何地址的值，不发。
            canvas.setCaretAddress(0x100050);
            canvas.setCaretAddress(0x100060, true);
            canvas.selectAll();
            canvas.scrollToAddress(0x100000 + 30ULL * 4096ULL);
            canvas.setBytesPerRow(32);
            canvas.setGroupSize(4);
            canvas.setHighlightRanges(1, { { 0x100100, 0x100110 } }, QColor(Qt::yellow), QStringLiteral("t"));
            canvas.clearAllHighlightRanges();
            ks::ui::HexFetchRange given;
            if (!provider.requests.empty() && !provider.requests.front().ranges.empty())
            {
                given = provider.requests.front().ranges.front();
            }
            canvas.cancelPages(given);
            Flush();
            CHECK(spy.count() == 4);

            // 换叠加层与宿主直接改叠加层后的通知，各发一次。
            ksword::memwb::MemoryDiffOverlay overlay;
            canvas.setOverlay(&overlay);
            Flush();
            CHECK(spy.count() == 5);
            canvas.notifyOverlayChanged();
            canvas.notifyOverlayChanged();
            Flush();
            CHECK(spy.count() == 6);

            // 清空地址空间：原来有数据，发一次；已经没有数据时再清空不发。
            canvas.clearAddressSpace();
            Flush();
            CHECK(spy.count() == 7);
            canvas.clearAddressSpace();
            Flush();
            CHECK(spy.count() == 7);
            canvas.setOverlay(nullptr);
        }

        // setStaticData 同步供满可见页（多页回填）也只产生一个信号；信号发出时变化已落地；槽内再改会再排一次。
        void TestContentChangedOrderingAndReentry()
        {
            ApplyTheme(false);
            HexCanvas canvas;
            canvas.resize(800, 360);
            canvas.show();
            QSignalSpy spy(&canvas, &HexCanvas::contentChanged);

            // 同步供页：2 MiB 数据，内置提供者在 setStaticData 内同步回填首屏与预取页。
            canvas.setStaticData(0x1000, MakePattern(2 * 1024 * 1024));
            CHECK(spy.count() == 0);
            CHECK(canvas.cellStateAt(0x1000).hasValue);
            Flush();
            CHECK(spy.count() == 1);

            // 落地顺序：槽里读到的是新值，而不是"未加载"。
            std::vector<bool> loadedInSlot;
            QMetaObject::Connection connection = QObject::connect(&canvas, &HexCanvas::contentChanged, &canvas, [&]() {
                loadedInSlot.push_back(canvas.cellStateAt(0x1000).hasValue);
            });
            canvas.refresh();
            Flush();
            QObject::disconnect(connection);
            CHECK(loadedInSlot.size() == 1);
            CHECK(!loadedInSlot.empty() && loadedInSlot[0]);

            // 槽内再改内容：会再排一次（不被"已排队"吞掉，也不递归死循环）。
            int slotCalls = 0;
            connection = QObject::connect(&canvas, &HexCanvas::contentChanged, &canvas, [&]() {
                ++slotCalls;
                if (slotCalls == 1)
                {
                    canvas.refresh();
                }
            });
            const int before = spy.count();
            canvas.refresh();
            Flush();
            CHECK(slotCalls == 1);
            CHECK(spy.count() == before + 1);
            Flush();
            CHECK(slotCalls == 2);
            CHECK(spy.count() == before + 2);
            Flush();
            CHECK(slotCalls == 2);
            QObject::disconnect(connection);
        }

        // 画布在信号排队期间被销毁：排队的调用随画布作废，不崩溃、也不会向已销毁对象发信号。
        void TestContentChangedDestroyedWhilePending()
        {
            ApplyTheme(false);
            int received = 0;
            {
                auto canvas = std::make_unique<HexCanvas>();
                canvas->resize(600, 300);
                canvas->show();
                QObject::connect(canvas.get(), &HexCanvas::contentChanged, qApp, [&]() { ++received; });
                canvas->setStaticData(0x1000, MakePattern(4096));
                CHECK(received == 0);
            }
            Flush();
            Flush();
            CHECK(received == 0);
        }

        // 编辑路径与 contentChanged：暂存成功、丢弃都发（排队），失败的暂存不发。
        void TestContentChangedOnEdits()
        {
            ApplyTheme(false);
            auto fixture = MakeStaticFixture(0x1000, MakePattern(4096), true, QSize(900, 420));
            HexCanvas& canvas = *fixture->canvas;
            Flush();
            QSignalSpy content(&canvas, &HexCanvas::contentChanged);
            QSignalSpy staged(&canvas, &HexCanvas::editStaged);
            QSignalSpy discarded(&canvas, &HexCanvas::editDiscarded);

            // 键盘暂存一个字节：editStaged 同步，contentChanged 排队。
            canvas.setCaretAddress(0x1010);
            Type(canvas, QStringLiteral("4A"));
            CHECK(staged.count() == 1);
            CHECK(content.count() == 0);
            Flush();
            CHECK(content.count() == 1);

            // 填充 16 字节只算一次变化。
            canvas.setCaretAddress(0x1040);
            canvas.setCaretAddress(0x104F, true);
            canvas.fillSelection(0x00);
            Flush();
            CHECK(content.count() == 2);

            // Backspace 丢弃暂存：发 editDiscarded 与一次 contentChanged。
            canvas.setCaretAddress(0x1011);
            Key(canvas, Qt::Key_Backspace);
            CHECK(discarded.count() == 1);
            Flush();
            CHECK(content.count() == 3);

            // 没有暂存可丢弃的 Backspace：什么都不发。
            canvas.setCaretAddress(0x1200);
            Key(canvas, Qt::Key_Backspace);
            CHECK(discarded.count() == 1);
            Flush();
            CHECK(content.count() == 3);

            // 被拒绝的暂存（只读）不发 contentChanged。
            canvas.setEditable(false);
            QString reason;
            CHECK(!canvas.stageBytes(0x1300, QByteArray("\x01", 1), &reason));
            Flush();
            CHECK(content.count() == 3);

            // 宿主绕过画布改叠加层后通知：重绘并排队一次，但不取消用户敲到一半的半字节
            // （宿主写回后用户可能正敲到一半；半字节只记着高位与目标地址，叠加层变化不会让它失效）。
            canvas.setEditable(true);
            canvas.setCaretAddress(0x1500);
            Type(canvas, QStringLiteral("4"));
            CHECK(canvas.cellStateAt(0x1500).nibblePreview);
            fixture->overlay.DiscardAll();
            canvas.notifyOverlayChanged();
            CHECK(canvas.cellStateAt(0x1500).nibblePreview);
            Flush();
            CHECK(content.count() == 4);
            Key(canvas, Qt::Key_Escape);
            CHECK(!canvas.cellStateAt(0x1500).nibblePreview);
        }

        // editableChanged：只在值真的变了才发，参数是新状态。
        void TestEditableChanged()
        {
            ApplyTheme(false);
            HexCanvas canvas;
            ksword::memwb::MemoryDiffOverlay overlay;
            canvas.resize(600, 300);
            canvas.show();
            canvas.setOverlay(&overlay);
            QSignalSpy spy(&canvas, &HexCanvas::editableChanged);

            // 默认只读：再设只读没有变化。
            CHECK(!canvas.isEditable());
            canvas.setEditable(false);
            CHECK(spy.count() == 0);

            canvas.setEditable(true);
            CHECK(canvas.isEditable());
            CHECK(spy.count() == 1);
            CHECK(spy.count() == 1 && spy.at(0).at(0).toBool());
            canvas.setEditable(true);
            CHECK(spy.count() == 1);

            canvas.setEditable(false);
            CHECK(!canvas.isEditable());
            CHECK(spy.count() == 2);
            CHECK(spy.count() == 2 && !spy.at(1).at(0).toBool());
            canvas.setEditable(false);
            CHECK(spy.count() == 2);

            // 设为只读仍然取消未完成的半字节（原有行为不受信号影响）。
            auto fixture = MakeStaticFixture(0x1000, MakePattern(4096), true, QSize(900, 420));
            QSignalSpy fixtureSpy(fixture->canvas.get(), &HexCanvas::editableChanged);
            fixture->canvas->setCaretAddress(0x1010);
            Type(*fixture->canvas, QStringLiteral("4"));
            CHECK(fixture->canvas->cellStateAt(0x1010).nibblePreview);
            fixture->canvas->setEditable(false);
            CHECK(!fixture->canvas->cellStateAt(0x1010).nibblePreview);
            CHECK(fixtureSpy.count() == 1);

            // 没有叠加层时可编辑状态本身仍照常发信号（面板另有 contentChanged 反映 canEdit 变化）。
            canvas.setOverlay(nullptr);
            canvas.setEditable(true);
            CHECK(spy.count() == 3);
        }
    }

    // 第一组入口：contentChanged 与 editableChanged。
    void RunSignalContentTests()
    {
        TestContentChangedCoalescing();
        TestContentChangedOrderingAndReentry();
        TestContentChangedDestroyedWhilePending();
        TestContentChangedOnEdits();
        TestEditableChanged();
    }

    // 总入口：画布侧三组、面板侧一组依次运行，并单独报告这一轮的断言数与失败数。
    void RunSignalTests(const QString& shotsDir)
    {
        const int checksBefore = g_checks;
        const int failuresBefore = g_failures;
        RunSignalContentTests();
        RunSignalStageTests();
        RunSignalViewTests(shotsDir);
        RunInspectorSignalTests();
        ApplyTheme(false);
        std::cout << "signal tests: " << (g_checks - checksBefore) << " checks, "
                  << (g_failures - failuresBefore) << " failures" << std::endl;
    }
}
