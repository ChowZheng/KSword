// memwb_ui_tests.Facade.cpp
// 作用：HexEditorWidget 门面的离屏验证（第一组：默认值、数据模型、可编辑与行宽、变更参照），
//       以及门面验证的公共设施定义与总入口 RunFacadeTests。
// 这些用例钉死的是"旧 HexEditorWidget 的可观察语义"，不是 HexView 的语义——
// HexView 自己的行为已由 memwb_ui_tests.HexView*.cpp 覆盖，这里只验证门面转发后宿主看到的结果。
// 信号、选区、跳转、查找条在 memwb_ui_tests.Facade.Signals.cpp；宿主使用形态在 memwb_ui_tests.Facade.Hosts.cpp。

#include "memwb_ui_facade.h"

#include <QApplication>
#include <QLayout>
#include <QMargins>

#include <initializer_list>
#include <iostream>
#include <map>

namespace memwb_test
{
    // 构造并显示门面。
    std::unique_ptr<HexEditorWidget> MakeFacade(
        std::uint64_t base,
        const QByteArray& data,
        bool editable,
        const QSize& size)
    {
        auto widget = std::make_unique<HexEditorWidget>();
        widget->resize(size);
        widget->show();
        widget->setByteArray(data, base);
        widget->setEditable(editable);
        QApplication::processEvents();
        return widget;
    }

    namespace
    {
        using ks::ui::HexCanvas;
        using ks::ui::HexView;
        using Pane = HexCanvas::ActivePane;
        using ChangeKind = HexCanvas::ChangeKind;

        // Altered：复制一份数据并把给定下标的字节取反（保证与原值不同）。
        QByteArray Altered(const QByteArray& source, std::initializer_list<int> indexes)
        {
            QByteArray copy = source;
            for (const int index : indexes)
            {
                copy[index] = static_cast<char>(~copy.at(index));
            }
            return copy;
        }

        // KindsMatch：检查 [0, count) 里每个下标的变化种类是否符合期望表（默认 Unchanged）。
        // 每个单元格还必须"有值"：没有值的单元格变化种类恒为 Unchanged，不检查的话"全部 Unchanged"会空过。
        // 传入：门面、基址、字节数、期望表（下标 -> 种类）、第一处不符的说明输出；传出：是否全部符合。
        bool KindsMatch(
            HexEditorWidget& widget,
            std::uint64_t base,
            int count,
            const std::map<int, ChangeKind>& expected,
            QString* firstMismatch)
        {
            for (int index = 0; index < count; ++index)
            {
                const auto found = expected.find(index);
                const ChangeKind want = found == expected.end() ? ChangeKind::Unchanged : found->second;
                const HexCanvas::CellState cell = FacadeCell(widget, base + static_cast<std::uint64_t>(index));
                const ChangeKind got = cell.hasValue ? cell.change : ChangeKind::Unreadable;
                if (got != want)
                {
                    if (firstMismatch != nullptr)
                    {
                        *firstMismatch = QStringLiteral("index %1 want %2 got %3")
                            .arg(index).arg(static_cast<int>(want)).arg(static_cast<int>(got));
                    }
                    return false;
                }
            }
            return true;
        }

        // 默认值：与旧控件一致（只读、每行 16 字节、空数据），并且内部 HexView 铺满、工具栏与状态条显示。
        void TestDefaults()
        {
            ApplyTheme(false);
            HexEditorWidget widget;
            widget.resize(900, 420);
            widget.show();
            QApplication::processEvents();

            // 类名：TextSearchReplaceSupport 靠类名里的 HexEditorWidget 判断"自带查找栏"，必须保持。
            CHECK(QString::fromLatin1(widget.metaObject()->className()) == QStringLiteral("HexEditorWidget"));

            // 旧控件的默认状态：只读、16 字节一行、没有数据、基址为 0、没有选中字节。
            CHECK(!widget.isEditable());
            CHECK(widget.bytesPerRow() == 16);
            CHECK(widget.regionSize() == 0);
            CHECK(widget.baseAddress() == 0);
            CHECK(widget.data().isEmpty());
            CHECK(widget.selectedAbsoluteAddress() == 0);
            CHECK(widget.selectedOffset() == 0);

            // 内部结构：恰好一个 HexView，是本控件的直接子控件，布局边距为 0 并铺满。
            HexView* view = ViewOf(widget);
            CHECK(view != nullptr);
            CHECK(widget.findChildren<HexView*>().size() == 1);
            if (view == nullptr)
            {
                return;
            }
            CHECK(view->parentWidget() == &widget);
            CHECK(widget.layout() != nullptr && widget.layout()->contentsMargins() == QMargins(0, 0, 0, 0));
            CHECK(view->geometry() == widget.rect());

            // 独立使用者的默认外观：工具栏与状态条显示，查找条 / 跳转条 / 解释器面板隐藏。
            CHECK(view->toolbarVisible() && view->statusBarVisible());
            CHECK(!view->findBar()->isVisible() && !view->gotoBar()->isVisible());
            CHECK(!view->inspectorVisible());
            CHECK(!view->isEditable() && view->bytesPerRow() == 16);

            // 窗口缩放时内部 HexView 跟着铺满。
            widget.resize(700, 300);
            QApplication::processEvents();
            CHECK(view->geometry() == widget.rect());
        }

        // 数据模型：setByteArray / data / regionSize / baseAddress / 选中地址，副本语义，空数据，clearData。
        void TestDataModel()
        {
            ApplyTheme(false);
            const QByteArray data = MakePattern(8192);
            auto widget = MakeFacade(0x1000, data, false, QSize(900, 420));

            // 基本读数：整块数据、长度、基址；刚载入时选中的是第一个字节。
            CHECK(widget->data() == data);
            CHECK(widget->regionSize() == 8192);
            CHECK(widget->baseAddress() == 0x1000);
            CHECK(widget->selectedAbsoluteAddress() == 0x1000);
            CHECK(widget->selectedOffset() == 0);

            // 画面：可见范围内的单元格显示的就是传入的字节，且没有变化着色。
            for (const std::uint64_t address : { 0x1000ULL, 0x1001ULL, 0x1010ULL, 0x10F0ULL })
            {
                const HexCanvas::CellState cell = FacadeCell(*widget, address);
                CHECK(cell.inSpace && cell.hasValue
                    && cell.value == ByteValue(data, static_cast<qsizetype>(address - 0x1000)));
                CHECK(cell.change == ChangeKind::Unchanged);
            }

            // 默认基址是 0。
            widget->setByteArray(MakePattern(64, 1));
            CHECK(widget->baseAddress() == 0 && widget->regionSize() == 64);

            // 换基址换长度：旧范围不再属于地址空间，选中地址回到新基址。
            const QByteArray second = MakePattern(2048, 9);
            widget->setByteArray(second, 0x5000);
            CHECK(widget->data() == second);
            CHECK(widget->baseAddress() == 0x5000 && widget->regionSize() == 2048);
            CHECK(FacadeCell(*widget, 0x5000).hasValue && !FacadeCell(*widget, 0x1000).inSpace);
            CHECK(widget->selectedAbsoluteAddress() == 0x5000 && widget->selectedOffset() == 0);

            // 副本语义：外部缓冲之后被改，控件内容不受影响；取出的 data() 被改，控件内容同样不受影响。
            QByteArray source = MakePattern(256, 5);
            const QByteArray sourceCopy = source;
            widget->setByteArray(source, 0x100);
            source[0] = static_cast<char>(~source.at(0));
            CHECK(widget->data() == sourceCopy);
            QByteArray taken = widget->data();
            taken[1] = static_cast<char>(~taken.at(1));
            CHECK(widget->data() == sourceCopy);

            // 空数组：长度 0、基址取传入值、选中地址是基址、偏移为 0、单元格不属于地址空间。
            widget->setByteArray(QByteArray(), 0x77);
            CHECK(widget->regionSize() == 0 && widget->data().isEmpty());
            CHECK(widget->baseAddress() == 0x77);
            CHECK(widget->selectedAbsoluteAddress() == 0x77 && widget->selectedOffset() == 0);
            CHECK(!FacadeCell(*widget, 0x77).inSpace);

            // clearData：数据与选区归零，基址保留（旧控件语义：clearData 等价于用旧基址设置空数据）。
            widget->setByteArray(second, 0x9000);
            CHECK(widget->jumpToAbsoluteAddress(0x9010));
            CHECK(widget->selectedAbsoluteAddress() == 0x9010 && widget->selectedOffset() == 0x10);
            widget->clearData();
            CHECK(widget->regionSize() == 0 && widget->data().isEmpty());
            CHECK(widget->baseAddress() == 0x9000);
            CHECK(widget->selectedAbsoluteAddress() == 0x9000 && widget->selectedOffset() == 0);
            CHECK(!FacadeCell(*widget, 0x9000).inSpace);

            // 清空之后可以再次载入，状态完整恢复。
            widget->setByteArray(data, 0x1000);
            CHECK(widget->data() == data && widget->baseAddress() == 0x1000 && widget->regionSize() == 8192);
            CHECK(widget->jumpToAbsoluteAddress(0x1800));
            CHECK(widget->selectedAbsoluteAddress() == 0x1800);

            // 从未载入过数据的控件：clearData 无害。
            HexEditorWidget fresh;
            fresh.clearData();
            CHECK(fresh.regionSize() == 0 && fresh.baseAddress() == 0 && fresh.selectedAbsoluteAddress() == 0);
        }

        // 可编辑状态与每行字节数。
        void TestEditableAndRows()
        {
            ApplyTheme(false);
            const QByteArray data = MakePattern(4096);
            auto widget = MakeFacade(0x1000, data, false, QSize(1800, 420));
            HexView* view = ViewOf(*widget);
            CHECK(view != nullptr);
            if (view == nullptr)
            {
                return;
            }

            // 切换可编辑：门面与内部画布同步；重复设置同一值无害。
            CHECK(!widget->isEditable() && !view->canvas()->isEditable());
            widget->setEditable(true);
            CHECK(widget->isEditable() && view->canvas()->isEditable());
            widget->setEditable(true);
            CHECK(widget->isEditable());
            widget->setEditable(false);
            CHECK(!widget->isEditable() && !view->canvas()->isEditable());

            // 可编辑状态跨 setByteArray / clearData 保持（DiskEditor 先设可编辑再反复换数据）。
            widget->setEditable(true);
            widget->setByteArray(MakePattern(1024, 2), 0x2000);
            CHECK(widget->isEditable());
            widget->clearData();
            CHECK(widget->isEditable());
            widget->setEditable(false);
            widget->setByteArray(MakePattern(1024, 3), 0x2000);
            CHECK(!widget->isEditable());

            // 没有数据时也能切换。
            HexEditorWidget fresh;
            fresh.setEditable(true);
            CHECK(fresh.isEditable());
            fresh.setEditable(false);
            CHECK(!fresh.isEditable());

            // 每行字节数：先夹取到 [4, 64]，再取最近的受支持值（8/16/32/48/64，等距取较大者）。
            struct RowCase
            {
                int requested;  // 宿主传入值
                int expected;   // 实际生效值
            };
            const RowCase cases[] = {
                { 16, 16 }, { 8, 8 }, { 32, 32 }, { 48, 48 }, { 64, 64 },
                { 4, 8 }, { 5, 8 }, { 6, 8 }, { 9, 8 }, { 12, 16 }, { 20, 16 },
                { 24, 32 }, { 40, 48 }, { 56, 64 }, { 63, 64 }, { 65, 64 }, { 100, 64 },
                { 0, 8 }, { -7, 8 }, { 16, 16 } };
            for (const RowCase& rowCase : cases)
            {
                widget->setBytesPerRow(rowCase.requested);
                CHECK_NOTE(widget->bytesPerRow() == rowCase.expected && view->bytesPerRow() == rowCase.expected,
                    QStringLiteral("requested %1 expected %2 got %3 / %4")
                        .arg(rowCase.requested).arg(rowCase.expected).arg(widget->bytesPerRow()).arg(view->bytesPerRow()));
            }

            // 行宽真的作用在画布上：16 字节一行时 +16 换行，32 字节一行时 +16 仍在同一行。
            widget->setBytesPerRow(16);
            QApplication::processEvents();
            HexCanvas& canvas = *view->canvas();
            CHECK(!canvas.cellRect(0x2000, Pane::Hex).isNull() && !canvas.cellRect(0x2010, Pane::Hex).isNull());
            CHECK(canvas.cellRect(0x2000, Pane::Hex).top() == canvas.cellRect(0x200F, Pane::Hex).top());
            CHECK(canvas.cellRect(0x2010, Pane::Hex).top() > canvas.cellRect(0x2000, Pane::Hex).top());
            widget->setBytesPerRow(32);
            QApplication::processEvents();
            CHECK(!canvas.cellRect(0x201F, Pane::Hex).isNull());
            CHECK(canvas.cellRect(0x2000, Pane::Hex).top() == canvas.cellRect(0x201F, Pane::Hex).top());
            CHECK(canvas.cellRect(0x2020, Pane::Hex).top() > canvas.cellRect(0x2000, Pane::Hex).top());

            // 改行宽不动数据与基址。
            CHECK(widget->data() == MakePattern(1024, 3) && widget->baseAddress() == 0x2000);

            // 行宽设置先于数据，也跨载入保持（宿主构造时先 setBytesPerRow(16)）。
            HexEditorWidget early;
            early.setBytesPerRow(32);
            CHECK(early.bytesPerRow() == 32);
            early.setByteArray(MakePattern(512), 0x10);
            CHECK(early.bytesPerRow() == 32);
            early.clearData();
            CHECK(early.bytesPerRow() == 32);
        }

        // 变更参照：尺寸不符不着色；尺寸一致时橙色 / 冷色；重复设置 O(1)；跨 setByteArray 的保留与作废；清除。
        void TestReferences()
        {
            ApplyTheme(false);
            const QByteArray buffer = MakePattern(256);
            auto widget = MakeFacade(0x1000, buffer, true, QSize(1000, 480));
            QString mismatch;

            // 尺寸不符（短、长、空）：不着色。传入的数组与缓冲在若干字节上确实不同，所以"不着色"不是巧合。
            widget->setChangeReferences(Altered(buffer, { 5, 6 }).left(128));
            CHECK_NOTE(KindsMatch(*widget, 0x1000, 256, {}, &mismatch), mismatch);
            widget->setChangeReferences(QByteArray(257, 'x'));
            CHECK_NOTE(KindsMatch(*widget, 0x1000, 256, {}, &mismatch), mismatch);
            widget->setChangeReferences(QByteArray());
            CHECK_NOTE(KindsMatch(*widget, 0x1000, 256, {}, &mismatch), mismatch);
            CHECK(widget->data() == buffer);

            // 尺寸一致：缓冲与 original 不同的字节（3、4、10）是橙色 Pending；显示值与 data() 仍是缓冲值。
            const QByteArray original = Altered(buffer, { 3, 4, 10 });
            widget->setChangeReferences(original);
            CHECK_NOTE(KindsMatch(*widget, 0x1000, 256,
                { { 3, ChangeKind::Pending }, { 4, ChangeKind::Pending }, { 10, ChangeKind::Pending } }, &mismatch), mismatch);
            CHECK(widget->data() == buffer);
            for (const int index : { 3, 4, 10, 11 })
            {
                CHECK(FacadeCell(*widget, 0x1000 + static_cast<std::uint64_t>(index)).value == ByteValue(buffer, index));
            }

            // previousRead：与 original 不同的字节显示冷色 ExternalChange；已是橙色的字节橙色优先（3）。
            const QByteArray previous = Altered(original, { 20, 3 });
            widget->setChangeReferences(original, previous);
            CHECK_NOTE(KindsMatch(*widget, 0x1000, 256,
                { { 3, ChangeKind::Pending }, { 4, ChangeKind::Pending }, { 10, ChangeKind::Pending },
                  { 20, ChangeKind::ExternalChange } }, &mismatch), mismatch);

            // previousRead 尺寸不符：被忽略，只剩橙色。
            widget->setChangeReferences(original, QByteArray(100, 'z'));
            CHECK_NOTE(KindsMatch(*widget, 0x1000, 256,
                { { 3, ChangeKind::Pending }, { 4, ChangeKind::Pending }, { 10, ChangeKind::Pending } }, &mismatch), mismatch);

            // original 尺寸不符（previousRead 尺寸相符）：整体不着色，已有着色消失（有意差异，见 HexEditorWidget.h 第三节）。
            widget->setChangeReferences(QByteArray(10, 'q'), previous);
            CHECK_NOTE(KindsMatch(*widget, 0x1000, 256, {}, &mismatch), mismatch);

            // clearChangeHighlights：着色全部消失，重复调用无害；缓冲不变。
            widget->setChangeReferences(original, previous);
            widget->clearChangeHighlights();
            CHECK_NOTE(KindsMatch(*widget, 0x1000, 256, {}, &mismatch), mismatch);
            widget->clearChangeHighlights();
            CHECK_NOTE(KindsMatch(*widget, 0x1000, 256, {}, &mismatch), mismatch);
            CHECK(widget->data() == buffer);

            // 重复设置同样的参照 O(1)：来源代次不变（HexView 重建基线会让代次前进）。
            // 同一份共享句柄与"内容相同的深拷贝"都不应触发重建；改一个字节才会。
            widget->setChangeReferences(original, previous);
            HexCanvas& canvas = *CanvasOf(*widget);
            const std::uint64_t revisionBefore = canvas.sourceRevision();
            widget->setChangeReferences(original, previous);
            CHECK(canvas.sourceRevision() == revisionBefore);
            widget->setChangeReferences(
                QByteArray(original.constData(), original.size()),
                QByteArray(previous.constData(), previous.size()));
            CHECK(canvas.sourceRevision() == revisionBefore);
            widget->setChangeReferences(Altered(original, { 50 }), previous);
            CHECK(canvas.sourceRevision() != revisionBefore);
            CHECK_NOTE(KindsMatch(*widget, 0x1000, 256,
                { { 3, ChangeKind::Pending }, { 4, ChangeKind::Pending }, { 10, ChangeKind::Pending },
                  { 50, ChangeKind::Pending }, { 20, ChangeKind::ExternalChange } }, &mismatch), mismatch);

            // 参照下继续编辑：用户改的字节橙色，改回参照值（= 原缓冲值）橙色消失，data() 随之变化。
            widget->setChangeReferences(original);
            canvas.setFocus();
            ClickAddress(canvas, 0x1000 + 30);
            const int old30 = ByteValue(buffer, 30);
            Type(canvas, HexByteText(OtherByte(old30, 0xA7)));
            CHECK_NOTE(KindsMatch(*widget, 0x1000, 256,
                { { 3, ChangeKind::Pending }, { 4, ChangeKind::Pending }, { 10, ChangeKind::Pending },
                  { 30, ChangeKind::Pending } }, &mismatch), mismatch);
            CHECK(ByteValue(widget->data(), 30) == OtherByte(old30, 0xA7));
            ClickAddress(canvas, 0x1000 + 30);
            Type(canvas, HexByteText(old30));
            CHECK_NOTE(KindsMatch(*widget, 0x1000, 256,
                { { 3, ChangeKind::Pending }, { 4, ChangeKind::Pending }, { 10, ChangeKind::Pending } }, &mismatch), mismatch);
            CHECK(widget->data() == buffer);
        }

        // 变更参照跨 setByteArray：基址与长度都不变时保留（旧控件行为），任一变化或 clearData 则作废。
        void TestReferenceRetention()
        {
            ApplyTheme(false);
            const QByteArray buffer = MakePattern(256);
            auto widget = MakeFacade(0x1000, buffer, true, QSize(1000, 480));
            QString mismatch;
            const QByteArray original = Altered(buffer, { 3, 4, 10 });
            const QByteArray previous = Altered(original, { 20, 3 });

            // 同基址同长度换内容：参照保留，着色按新内容相对旧 original 重算。
            widget->setChangeReferences(original, previous);
            const QByteArray buffer2 = Altered(buffer, { 7 });
            widget->setByteArray(buffer2, 0x1000);
            CHECK(widget->data() == buffer2);
            CHECK_NOTE(KindsMatch(*widget, 0x1000, 256,
                { { 3, ChangeKind::Pending }, { 4, ChangeKind::Pending }, { 7, ChangeKind::Pending },
                  { 10, ChangeKind::Pending }, { 20, ChangeKind::ExternalChange } }, &mismatch), mismatch);

            // 保留之后再传同样的参照：本地记录与实际生效的参照一致，着色必须仍在（不能因为"相同"而丢失）。
            widget->setChangeReferences(original, previous);
            CHECK_NOTE(KindsMatch(*widget, 0x1000, 256,
                { { 3, ChangeKind::Pending }, { 4, ChangeKind::Pending }, { 7, ChangeKind::Pending },
                  { 10, ChangeKind::Pending }, { 20, ChangeKind::ExternalChange } }, &mismatch), mismatch);

            // discardChanges 形态：用户改过字节后把原始内容写回（同形状），橙色全部消失；参照保留，继续编辑仍着色。
            widget->setChangeReferences(original);
            CanvasOf(*widget)->setFocus();
            ClickAddress(*CanvasOf(*widget), 0x1000 + 30);
            Type(*CanvasOf(*widget), HexByteText(OtherByte(ByteValue(buffer, 30), 0x6B)));
            CHECK(FacadeCell(*widget, 0x101E).change == ChangeKind::Pending);
            widget->setByteArray(original, 0x1000);
            CHECK(widget->data() == original);
            CHECK_NOTE(KindsMatch(*widget, 0x1000, 256, {}, &mismatch), mismatch);
            ClickAddress(*CanvasOf(*widget), 0x1000 + 40);
            Type(*CanvasOf(*widget), HexByteText(OtherByte(ByteValue(original, 40), 0x4E)));
            CHECK_NOTE(KindsMatch(*widget, 0x1000, 256, { { 40, ChangeKind::Pending } }, &mismatch), mismatch);

            // 基址变了：参照作废，新基址下一个字节都不着色。
            widget->setChangeReferences(original, previous);
            widget->setByteArray(buffer2, 0x2000);
            CHECK_NOTE(KindsMatch(*widget, 0x2000, 256, {}, &mismatch), mismatch);

            // 作废之后重新设置参照仍然生效（本地记录没有残留导致"相同所以跳过"）。
            widget->setChangeReferences(original, previous);
            CHECK_NOTE(KindsMatch(*widget, 0x2000, 256,
                { { 3, ChangeKind::Pending }, { 4, ChangeKind::Pending }, { 7, ChangeKind::Pending },
                  { 10, ChangeKind::Pending }, { 20, ChangeKind::ExternalChange } }, &mismatch), mismatch);

            // 长度变了：参照作废。
            widget->setByteArray(MakePattern(128), 0x2000);
            CHECK_NOTE(KindsMatch(*widget, 0x2000, 128, {}, &mismatch), mismatch);

            // clearData 作废：之后用同形状的数据重新载入也不会着色。
            widget->setChangeReferences(Altered(MakePattern(128), { 1 }));
            CHECK_NOTE(KindsMatch(*widget, 0x2000, 128, { { 1, ChangeKind::Pending } }, &mismatch), mismatch);
            widget->clearData();
            widget->setByteArray(MakePattern(128), 0x2000);
            CHECK_NOTE(KindsMatch(*widget, 0x2000, 128, {}, &mismatch), mismatch);

            // 清除之后同形状换内容不会"复活"参照。
            widget->setChangeReferences(Altered(MakePattern(128), { 1 }));
            widget->clearChangeHighlights();
            widget->setByteArray(Altered(MakePattern(128), { 9 }), 0x2000);
            CHECK_NOTE(KindsMatch(*widget, 0x2000, 128, {}, &mismatch), mismatch);
        }
    }

    // 第一组：默认值、数据模型、可编辑与行宽、变更参照。
    void RunFacadeCoreTests()
    {
        TestDefaults();
        TestDataModel();
        TestEditableAndRows();
        TestReferences();
        TestReferenceRetention();
    }

    // 总入口：设置重定向在整个门面验证期间有效。
    void RunFacadeTests()
    {
        const int checksBefore = g_checks;
        const int failuresBefore = g_failures;
        SettingsRedirect redirect;
        ApplyTheme(false);

        RunFacadeCoreTests();
        RunFacadeSignalTests();
        RunFacadeHostTests();

        std::cout << "facade: " << (g_checks - checksBefore) << " checks, "
                  << (g_failures - failuresBefore) << " failures" << std::endl;
    }
}
