// memwb_ui_tests.HexView.Reference.cpp
// 作用：HexView 兼容层里"变更着色参照"的离屏验证——
//   setReference（尺寸不符不着色并清除参照、一致时橙色 Pending / 冷色 ExternalChange 正确、
//   优先级、previousRead 尺寸不符被忽略、与旧控件规则一致）、clearReference、
//   参照下继续编辑（页缓存不陈旧）、保持选区与滚动位置、不清查找高亮、暂存超限的失败分支、像素级颜色区分。

#include "memwb_ui_hexview.h"

#include <QApplication>

#include <initializer_list>
#include <map>

namespace memwb_test
{
    namespace
    {
        using ks::ui::HexCanvas;
        using ks::ui::HexView;
        using Pane = HexCanvas::ActivePane;
        using ChangeKind = HexCanvas::ChangeKind;

        // ByteAt：缓冲里第 index 个字节。
        int ByteAt(const QByteArray& buffer, qsizetype index)
        {
            return static_cast<std::uint8_t>(buffer.at(index));
        }

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
        // 传入：视图、基址、字节数、期望表（下标 -> 种类）、失败说明；传出：是否全部符合。
        bool KindsMatch(
            HexView& view,
            std::uint64_t base,
            int count,
            const std::map<int, ChangeKind>& expected,
            QString* firstMismatch)
        {
            for (int index = 0; index < count; ++index)
            {
                const auto found = expected.find(index);
                const ChangeKind want = found == expected.end() ? ChangeKind::Unchanged : found->second;
                const ChangeKind got = CellOf(view, base + static_cast<std::uint64_t>(index)).change;
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

        // 尺寸不符：不着色，并清除参照（与旧控件"引用尺寸与缓冲尺寸不符时不着色"一致）。
        void TestMismatchedSize()
        {
            ApplyTheme(false);
            const QByteArray buffer = MakePattern(64);
            auto view = MakeHexView(0x1000, buffer, true, QSize(900, 400));
            QString mismatch;

            CHECK(!view->setReference(QByteArray(32, 'x')));
            CHECK(!view->hasReference());
            CHECK_NOTE(KindsMatch(*view, 0x1000, 64, {}, &mismatch), mismatch);
            CHECK(!view->setReference(QByteArray(65, 'x')));
            CHECK(!view->setReference(QByteArray()));
            CHECK(!view->hasReference());
            CHECK_NOTE(KindsMatch(*view, 0x1000, 64, {}, &mismatch), mismatch);
            CHECK(view->buffer() == buffer);

            // 先有一个有效参照，再给一个尺寸不符的：参照被清除，着色消失。
            const QByteArray original = Altered(buffer, { 5, 6 });
            CHECK(view->setReference(original));
            CHECK_NOTE(KindsMatch(*view, 0x1000, 64, { { 5, ChangeKind::Pending }, { 6, ChangeKind::Pending } }, &mismatch), mismatch);
            CHECK(!view->setReference(QByteArray(10, 'y')));
            CHECK(!view->hasReference());
            CHECK_NOTE(KindsMatch(*view, 0x1000, 64, {}, &mismatch), mismatch);
            CHECK(!view->canvas()->overlay()->HasPendingPatches());

            // 空缓冲：只有 original 也为空才算"等长"。
            HexView empty;
            empty.resize(500, 300);
            empty.show();
            CHECK(empty.setReference(QByteArray()));
            CHECK(!empty.setReference(QByteArray(4, 'z')));
        }

        // 尺寸一致：橙色 / 冷色 / 优先级 / previousRead 尺寸不符被忽略。
        void TestMatchedReference()
        {
            ApplyTheme(false);
            const QByteArray buffer = MakePattern(256);
            auto view = MakeHexView(0x1000, buffer, true, QSize(1000, 480));
            QString mismatch;

            // 只有 original：缓冲与基线不同的字节（3、4、10）是橙色待提交；显示值是缓冲值。
            const QByteArray original = Altered(buffer, { 3, 4, 10 });
            CHECK(view->setReference(original));
            CHECK(view->hasReference());
            CHECK_NOTE(KindsMatch(*view, 0x1000, 256,
                { { 3, ChangeKind::Pending }, { 4, ChangeKind::Pending }, { 10, ChangeKind::Pending } }, &mismatch), mismatch);
            CHECK(view->buffer() == buffer);
            for (const int index : { 3, 4, 10, 11 })
            {
                CHECK(CellOf(*view, 0x1000 + static_cast<std::uint64_t>(index)).value == ByteAt(buffer, index));
            }

            // 补丁块：[3,4] 与 [10]，before 是参照字节，after 是缓冲字节。
            const std::vector<ksword::memwb::DiffBlock> blocks = view->canvas()->overlay()->DiffBlocks();
            CHECK(blocks.size() == 2);
            if (blocks.size() == 2)
            {
                // 期望的两个补丁块内容先放进局部变量（宏参数里的花括号逗号会被预处理器当成参数分隔）。
                const std::vector<std::uint8_t> expectedBefore = {
                    static_cast<std::uint8_t>(ByteAt(original, 3)), static_cast<std::uint8_t>(ByteAt(original, 4)) };
                const std::vector<std::uint8_t> expectedAfter = {
                    static_cast<std::uint8_t>(ByteAt(buffer, 3)), static_cast<std::uint8_t>(ByteAt(buffer, 4)) };
                CHECK(blocks[0].address == 0x1003 && blocks[0].after.size() == 2);
                CHECK(blocks[0].before == expectedBefore);
                CHECK(blocks[0].after == expectedAfter);
                CHECK(blocks[1].address == 0x100A && blocks[1].after.size() == 1);
            }

            // previousRead：与 original 不同的字节显示冷色"外部变化"；已有补丁的字节橙色优先（3）。
            const QByteArray previous = Altered(original, { 20, 3 });
            CHECK(view->setReference(original, previous));
            CHECK_NOTE(KindsMatch(*view, 0x1000, 256,
                { { 3, ChangeKind::Pending }, { 4, ChangeKind::Pending }, { 10, ChangeKind::Pending },
                  { 20, ChangeKind::ExternalChange } }, &mismatch), mismatch);
            CHECK(CellOf(*view, 0x1014).value == ByteAt(buffer, 20));

            // previousRead 与 original 完全相同：没有任何外部变化。
            CHECK(view->setReference(original, original));
            CHECK_NOTE(KindsMatch(*view, 0x1000, 256,
                { { 3, ChangeKind::Pending }, { 4, ChangeKind::Pending }, { 10, ChangeKind::Pending } }, &mismatch), mismatch);

            // previousRead 尺寸不符：被忽略（没有冷色），不影响橙色，返回值仍为 true。
            CHECK(view->setReference(original, QByteArray(10, 'q')));
            CHECK_NOTE(KindsMatch(*view, 0x1000, 256,
                { { 3, ChangeKind::Pending }, { 4, ChangeKind::Pending }, { 10, ChangeKind::Pending } }, &mismatch), mismatch);

            // original 与缓冲完全一致：参照有效但没有任何着色。
            CHECK(view->setReference(buffer));
            CHECK(view->hasReference());
            CHECK_NOTE(KindsMatch(*view, 0x1000, 256, {}, &mismatch), mismatch);
            CHECK(!view->canvas()->overlay()->HasPendingPatches());

            // clearReference：基线回到当前缓冲，着色全部消失，缓冲不变。
            CHECK(view->setReference(Altered(buffer, { 7 }), Altered(buffer, { 8 })));
            CHECK(view->canvas()->overlay()->HasPendingPatches());
            view->clearReference();
            CHECK(!view->hasReference());
            CHECK_NOTE(KindsMatch(*view, 0x1000, 256, {}, &mismatch), mismatch);
            CHECK(!view->canvas()->overlay()->HasPendingPatches());
            CHECK(view->buffer() == buffer);
            CHECK(CellOf(*view, 0x1007).value == ByteAt(buffer, 7));
        }

        // 着色在屏幕上真的不同：待提交（橙）、外部变化（冷）、无变化三种底色两两可分。
        void TestReferenceColorsOnScreen()
        {
            for (const bool dark : { false, true })
            {
                ApplyTheme(dark);
                const QByteArray buffer = MakePattern(256);
                auto view = MakeHexView(0x1000, buffer, true, QSize(1000, 480));
                HexCanvas& canvas = *view->canvas();
                const QByteArray original = Altered(buffer, { 3 });
                const QByteArray previous = Altered(original, { 20 });
                CHECK(view->setReference(original, previous));
                Flush();

                // 取单元格左上角内侧一点的底色（避开文字笔画）。
                const QImage image = GrabImage(canvas);
                const auto backgroundOf = [&](std::uint64_t address) {
                    const QRect rect = canvas.cellRect(address, Pane::Hex);
                    return QColor(image.pixel(rect.left() + 1, rect.top() + 1));
                };
                const QColor pending = backgroundOf(0x1003);
                const QColor external = backgroundOf(0x1014);
                const QColor plain = backgroundOf(0x1030);
                CHECK_NOTE(ColorDistance(pending, plain) > 25, QStringLiteral("pending vs plain dark=%1").arg(dark));
                CHECK_NOTE(ColorDistance(external, plain) > 25, QStringLiteral("external vs plain dark=%1").arg(dark));
                CHECK_NOTE(ColorDistance(pending, external) > 25, QStringLiteral("pending vs external dark=%1").arg(dark));
            }
            ApplyTheme(false);
        }

        // 参照下继续编辑：补丁相对参照计算，改回参照值时橙色消失，显示值是参照值（页缓存不陈旧）。
        void TestEditAgainstReference()
        {
            ApplyTheme(false);
            const QByteArray buffer = MakePattern(256);
            auto view = MakeHexView(0x1000, buffer, true, QSize(1000, 480));
            HexCanvas& canvas = *view->canvas();
            const QByteArray original = Altered(buffer, { 3 });
            CHECK(view->setReference(original));
            QSignalSpy edited(view.get(), &HexView::byteEdited);
            canvas.setFocus();

            // 字节 3 当前是橙色（缓冲值 != 参照值）。键入参照值：补丁消失，显示参照值，信号 (旧=缓冲值, 新=参照值)。
            const int referenceValue = ByteAt(original, 3);
            ClickAddress(canvas, 0x1003);
            Type(canvas, QStringLiteral("%1").arg(referenceValue, 2, 16, QLatin1Char('0')).toUpper());
            CHECK(edited.count() == 1);
            if (edited.count() == 1)
            {
                CHECK(edited.at(0).at(0).toULongLong() == 0x1003);
                CHECK(edited.at(0).at(1).toInt() == ByteAt(buffer, 3));
                CHECK(edited.at(0).at(2).toInt() == referenceValue);
            }
            CHECK(ByteAt(view->buffer(), 3) == referenceValue);
            CHECK(CellOf(*view, 0x1003).change == ChangeKind::Unchanged);
            CHECK(CellOf(*view, 0x1003).value == referenceValue);
            CHECK(!canvas.overlay()->HasPendingPatches());

            // 再改成另一个值：补丁的 before 是参照值（不是最初的缓冲值）。
            const int other = (referenceValue + 7) & 0xFF;
            ClickAddress(canvas, 0x1003);
            Type(canvas, QStringLiteral("%1").arg(other, 2, 16, QLatin1Char('0')).toUpper());
            CHECK(CellOf(*view, 0x1003).change == ChangeKind::Pending);
            const std::vector<ksword::memwb::DiffBlock> blocks = canvas.overlay()->DiffBlocks();
            CHECK(blocks.size() == 1 && blocks[0].before == std::vector<std::uint8_t>{ static_cast<std::uint8_t>(referenceValue) });

            // 静默回滚到参照值同样让橙色消失。
            CHECK(view->setByteQuiet(0x1003, static_cast<std::uint8_t>(referenceValue)));
            CHECK(CellOf(*view, 0x1003).change == ChangeKind::Unchanged);
            CHECK(!canvas.overlay()->HasPendingPatches());
        }

        // 换参照不动选区与滚动位置，不清查找高亮；setBuffer 重置参照。
        void TestReferenceKeepsViewState()
        {
            ApplyTheme(false);
            const QByteArray buffer = MakePattern(0x4000);
            auto view = MakeHexView(0x1000, buffer, true, QSize(900, 360));
            HexCanvas& canvas = *view->canvas();
            canvas.scrollToAddress(0x3000, HexCanvas::ScrollAlign::Top);
            Flush();
            ClickAddress(canvas, 0x3010);
            ClickAddress(canvas, 0x3013, Pane::Hex, Qt::ShiftModifier);
            const std::uint64_t firstRow = canvas.firstVisibleRow();
            CHECK(firstRow > 0);

            // 查找一次，让高亮处于活动状态。
            view->findBar()->setMode(ks::ui::HexFindBar::Mode::Hex);
            view->findBar()->setPatternText(QStringLiteral("%1").arg(ByteAt(buffer, 0x2010), 2, 16, QLatin1Char('0')));
            QSignalSpy completed(view->findBar(), &ks::ui::HexFindBar::searchCompleted);
            CHECK(view->findBar()->findNext());
            CHECK(PumpUntil([&]() { return completed.count() == 1; }, 3000));
            CHECK(view->findBar()->highlightActive());
            const std::uint64_t rowAfterFind = canvas.firstVisibleRow();
            const std::optional<HexCanvas::AddressRange> selectionAfterFind = canvas.selectedRange();

            // 换参照：滚动位置、选区、查找高亮都不变（内容没有变）。
            const QByteArray original = Altered(buffer, { 0x2010, 0x0100 });
            CHECK(view->setReference(original));
            Flush();
            CHECK(canvas.firstVisibleRow() == rowAfterFind);
            CHECK(canvas.selectedRange() == selectionAfterFind);
            CHECK(view->findBar()->highlightActive());
            CHECK(CellOf(*view, 0x3010).change == ChangeKind::Pending);
            canvas.scrollToAddress(0x3100, HexCanvas::ScrollAlign::Top);
            canvas.scrollToAddress(0x1100, HexCanvas::ScrollAlign::Center);
            Flush();
            CHECK(CellOf(*view, 0x1100).change == ChangeKind::Pending);

            // setBuffer 重置参照与着色。
            view->setBuffer(0x1000, buffer);
            CHECK(!view->hasReference());
            CHECK(CellOf(*view, 0x1100).change == ChangeKind::Unchanged);
            CHECK(!view->canvas()->overlay()->HasPendingPatches());
        }

        // 暂存总量超限（缓冲与参照相差超过 64 MiB）：返回 false，退回无参照，不留半截补丁。
        void TestReferenceTooLarge()
        {
            ApplyTheme(false);
            const int size = 65 * 1024 * 1024;
            const QByteArray buffer(size, '\x11');
            const QByteArray original(size, '\x22');
            auto view = std::make_unique<HexView>();
            view->resize(700, 400);
            view->show();
            view->setBuffer(0, buffer);
            CHECK(!view->setReference(original));
            CHECK(!view->hasReference());
            CHECK(!view->canvas()->overlay()->HasPendingPatches());
            CHECK(CellOf(*view, 0).change == ChangeKind::Unchanged);
            CHECK(view->buffer() == buffer);
            CHECK(CellOf(*view, 0).value == 0x11);
        }
    }

    // 参照着色验证入口。
    void RunHexViewReferenceTests()
    {
        TestMismatchedSize();
        TestMatchedReference();
        TestReferenceColorsOnScreen();
        TestEditAgainstReference();
        TestReferenceKeepsViewState();
        TestReferenceTooLarge();
    }
}
