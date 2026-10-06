// memwb_ui_tests.Signals.View.cpp
// 作用：第二轮接口补全的"画布侧"离屏验证（第三组）——
//   1) addressForViewportPos / paneAtViewportPos：与单元格几何逐一对应、与鼠标点击同一套命中规则，
//      表头/地址列/补空位/末行之下/行尾右侧空白不吸附，横向滚动下仍正确；
//   2) ASCII 面板的不可读占位符不再是 '?'：状态与像素（字形形状）两层都与真实 0x3F 字节区分；
//   3) 填充菜单项换了图标（不再是循环箭头）；
//   4) 截图（深浅两套主题）保存到 shots 目录供目视核对。
// 全部手势经 QTest 模拟的真实输入或真实的公开接口进入画布。

#include "memwb_ui_signals.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFontMetrics>
#include <QIcon>
#include <QScrollBar>

namespace memwb_test
{
    namespace
    {
        using ks::ui::HexCanvas;
        using Pane = HexCanvas::ActivePane;

        // 视口坐标命中：与单元格几何逐一对应；表头、地址列、补空位、末行之下、行尾右侧空白不吸附。
        void TestViewportQueries()
        {
            ApplyTheme(false);

            // 无数据：全部未命中。
            HexCanvas empty;
            empty.resize(900, 420);
            empty.show();
            CHECK(!empty.addressForViewportPos(QPoint(100, 100)).has_value());
            CHECK(!empty.paneAtViewportPos(QPoint(100, 100)).has_value());

            // 与单元格几何逐一对应：当前可见的每个地址，在两个面板里中心点命中它自己。
            auto fixture = MakeStaticFixture(0x1000, MakePattern(4096), false, QSize(1300, 420));
            HexCanvas& canvas = *fixture->canvas;
            bool allHex = true;
            bool allAscii = true;
            bool panesRight = true;
            int checked = 0;
            for (std::uint64_t address = 0x1000; address < 0x1000 + 16ULL * 12ULL; ++address)
            {
                const QRect hexCell = canvas.cellRect(address, Pane::Hex);
                const QRect asciiCell = canvas.cellRect(address, Pane::Ascii);
                if (hexCell.isNull() || asciiCell.isNull())
                {
                    continue;
                }
                ++checked;
                allHex = allHex && canvas.addressForViewportPos(hexCell.center()) == std::optional<std::uint64_t>(address);
                allAscii = allAscii && canvas.addressForViewportPos(asciiCell.center()) == std::optional<std::uint64_t>(address);
                panesRight = panesRight
                    && canvas.paneAtViewportPos(hexCell.center()) == std::optional<Pane>(Pane::Hex)
                    && canvas.paneAtViewportPos(asciiCell.center()) == std::optional<Pane>(Pane::Ascii);
            }
            CHECK(checked >= 160);
            CHECK(allHex);
            CHECK(allAscii);
            CHECK(panesRight);

            // 表头与地址列：没有字节也没有面板。
            const QRect first = canvas.cellRect(0x1000, Pane::Hex);
            CHECK(!canvas.addressForViewportPos(QPoint(first.center().x(), 3)).has_value());
            CHECK(!canvas.paneAtViewportPos(QPoint(first.center().x(), 3)).has_value());
            CHECK(!canvas.addressForViewportPos(QPoint(4, first.center().y())).has_value());
            CHECK(!canvas.paneAtViewportPos(QPoint(4, first.center().y())).has_value());

            // 十六进制单元格之间的空隙：与鼠标点击同一规则，归属左边那一格；落点的地址与点击后的插入点一致。
            const QPoint gap(first.right() + 1, first.center().y());
            CHECK(canvas.addressForViewportPos(gap) == std::optional<std::uint64_t>(0x1000));
            canvas.setCaretAddress(0x1005);
            QTest::mouseClick(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, gap);
            CHECK(canvas.caretAddress() == 0x1000);

            // 越过十六进制区末列的空隙与 ASCII 区右侧的空白：面板明确，但没有字节。
            // 十六进制区的尾部空隙：末列单元格（宽 2 个字符）向右 2.5 个字符之后、ASCII 区之前（ASCII 区从 3 个字符处起）。
            const QRect lastHex = canvas.cellRect(0x100F, Pane::Hex);
            const QRect lastAscii = canvas.cellRect(0x100F, Pane::Ascii);
            const int charWidth = lastHex.width() / 2;
            const QPoint hexTail(lastHex.left() + 2 * charWidth + (charWidth * 3) / 4, lastHex.center().y());
            CHECK(!canvas.addressForViewportPos(hexTail).has_value());
            CHECK(canvas.paneAtViewportPos(hexTail) == std::optional<Pane>(Pane::Hex));
            const QPoint asciiTail(lastAscii.right() + 6, lastAscii.center().y());
            CHECK(!canvas.addressForViewportPos(asciiTail).has_value());
            CHECK(canvas.paneAtViewportPos(asciiTail) == std::optional<Pane>(Pane::Ascii));
        }

        // 末行之下的空白、首行补空位、横向滚动：都按规则处理；点击选区的吸附行为不变。
        void TestViewportQueriesEdges()
        {
            ApplyTheme(false);

            // 数据只有 4 行（64 字节），视口高得多：末行之下的空白不命中，但点击仍吸附到末行。
            auto shortFixture = MakeStaticFixture(0x2000, MakePattern(64), false, QSize(900, 420));
            HexCanvas& shortCanvas = *shortFixture->canvas;
            const QRect lastRow = shortCanvas.cellRect(0x203F, Pane::Hex);
            const QPoint below(lastRow.center().x(), lastRow.bottom() + 40);
            CHECK(below.y() < shortCanvas.viewport()->height());
            CHECK(!shortCanvas.addressForViewportPos(below).has_value());
            CHECK(!shortCanvas.paneAtViewportPos(below).has_value());
            QTest::mouseClick(shortCanvas.viewport(), Qt::LeftButton, Qt::NoModifier, below);
            CHECK(shortCanvas.caretAddress() == 0x203F);

            // 首行补空位：空间起点 0x3005 不是行首，第 0..4 列没有字节。
            auto padded = MakeStaticFixture(0x3005, MakePattern(200), false, QSize(900, 420));
            HexCanvas& paddedCanvas = *padded->canvas;
            const QRect start = paddedCanvas.cellRect(0x3005, Pane::Hex);
            const QRect next = paddedCanvas.cellRect(0x3006, Pane::Hex);
            const int stride = next.left() - start.left();
            const QPoint paddingHex = start.center() - QPoint(stride * 3, 0);
            CHECK(!paddedCanvas.addressForViewportPos(paddingHex).has_value());
            CHECK(paddedCanvas.paneAtViewportPos(paddingHex) == std::optional<Pane>(Pane::Hex));
            CHECK(paddedCanvas.addressForViewportPos(start.center()) == std::optional<std::uint64_t>(0x3005));
            const QRect asciiStart = paddedCanvas.cellRect(0x3005, Pane::Ascii);
            const QPoint paddingAscii = asciiStart.center() - QPoint(asciiStart.width() * 2, 0);
            CHECK(!paddedCanvas.addressForViewportPos(paddingAscii).has_value());
            CHECK(paddedCanvas.paneAtViewportPos(paddingAscii) == std::optional<Pane>(Pane::Ascii));

            // 横向滚动：窄视口出现横向滚动条，滚到最右后 ASCII 单元格的命中仍与几何一致。
            auto narrow = MakeStaticFixture(0x4000, MakePattern(4096), false, QSize(420, 360));
            HexCanvas& narrowCanvas = *narrow->canvas;
            QScrollBar* hBar = narrowCanvas.horizontalScrollBar();
            CHECK(hBar->maximum() > 0);
            hBar->setValue(hBar->maximum());
            QApplication::processEvents();
            const QRect scrolledAscii = narrowCanvas.cellRect(0x4003, Pane::Ascii);
            CHECK(!scrolledAscii.isNull());
            CHECK(narrowCanvas.addressForViewportPos(scrolledAscii.center()) == std::optional<std::uint64_t>(0x4003));
            CHECK(narrowCanvas.paneAtViewportPos(scrolledAscii.center()) == std::optional<Pane>(Pane::Ascii));
        }

        // 不可读占位符：ASCII 面板里不可读的字节不再画成 '?'，与真实的 0x3F 字节在状态与像素上都不同。
        void TestUnreadablePlaceholder()
        {
            ApplyTheme(false);

            // 字形：Consolas 与微软雅黑都有 U+00B7 与 U+00D7（主入口已加载这两个字体文件）。
            for (const char32_t glyph : { char32_t(0x00B7), char32_t(0x00D7) })
            {
                CHECK(QFontMetrics(QFont(QStringLiteral("Consolas"))).inFontUcs4(glyph));
                CHECK(QFontMetrics(QFont(QStringLiteral("Microsoft YaHei UI"))).inFontUcs4(glyph));
            }

            // 页：0x10000 是真实的 0x3F，0x10001 没读到，0x10002 是真实的 0x2E，0x10003 是不可见字节 0x01；
            // 第 2 行（0x10010..0x1001F）整行没读到，第 3 行（0x10020..0x1002F）整行是真实的 0x3F。
            auto holder = MakeAsyncCanvas(0x10000, 0x10FFF, false, false);
            HexCanvas& canvas = *holder->canvas;
            QByteArray page = MakePattern(4096);
            QByteArray mask(4096, '\x01');
            page[0] = 0x3F;
            mask[1] = '\0';
            page[2] = 0x2E;
            page[3] = 0x01;
            for (int index = 0x10; index < 0x20; ++index)
            {
                mask[index] = '\0';
            }
            for (int index = 0x20; index < 0x30; ++index)
            {
                page[index] = 0x3F;
            }
            CHECK(canvas.deliverPage(0x10000, page, mask, canvas.sourceRevision()) == HexCanvas::PageResult::Accepted);

            const HexCanvas::CellState realQuestion = canvas.cellStateAt(0x10000);
            const HexCanvas::CellState unreadable = canvas.cellStateAt(0x10001);
            const HexCanvas::CellState realDot = canvas.cellStateAt(0x10002);
            const HexCanvas::CellState nonPrintable = canvas.cellStateAt(0x10003);
            CHECK(realQuestion.hasValue && realQuestion.asciiChar == QLatin1Char('?'));
            CHECK(!unreadable.hasValue && unreadable.byteState == HexCanvas::ByteState::Unreadable);
            CHECK(unreadable.asciiChar == QChar(0x00D7));
            CHECK(unreadable.asciiChar != QLatin1Char('?'));
            CHECK(unreadable.asciiChar != realQuestion.asciiChar);
            CHECK(unreadable.hexText == QStringLiteral("??"));
            CHECK(realDot.asciiChar == QLatin1Char('.'));
            CHECK(nonPrintable.asciiChar == QLatin1Char('.'));

            // 整个不可读行都是乘号，整个真实行都是问号（逐地址核对，不靠抽样）。
            bool unreadableRow = true;
            bool questionRow = true;
            for (std::uint64_t offset = 0; offset < 16; ++offset)
            {
                unreadableRow = unreadableRow && canvas.cellStateAt(0x10010 + offset).asciiChar == QChar(0x00D7);
                questionRow = questionRow && canvas.cellStateAt(0x10020 + offset).asciiChar == QLatin1Char('?');
            }
            CHECK(unreadableRow);
            CHECK(questionRow);

            // 未加载（在途）：另一块没回填的画布，ASCII 画中点、十六进制画两个中点。
            auto pendingHolder = MakeAsyncCanvas(0x10000, 0x10FFF, false, false);
            const HexCanvas::CellState loading = pendingHolder->canvas->cellStateAt(0x10005);
            CHECK(!loading.hasValue);
            CHECK(loading.asciiChar == QChar(0x00B7));
            CHECK(loading.hexText == QString(2, QChar(0x00B7)));
            CHECK(loading.asciiChar != unreadable.asciiChar);

            // 像素实测：真正画出来的 ASCII 单元格——不可读的 × 与真实的 ? 不同，也与未加载的 · 不同；每格都有字。
            // 比的是"字形形状"（InkDifference，与文字颜色无关），所以把占位符改回 '?' 会被抓到。
            const QColor surface = KswordTheme::SurfaceColor();
            const QImage image = GrabImage(canvas);
            const QImage pendingImage = GrabImage(*pendingHolder->canvas);
            const QRect questionRect = canvas.cellRect(0x10000, Pane::Ascii);
            const QRect unreadableRect = canvas.cellRect(0x10001, Pane::Ascii);
            const QRect loadingRect = pendingHolder->canvas->cellRect(0x10005, Pane::Ascii);
            CHECK(!questionRect.isNull() && questionRect.size() == unreadableRect.size());
            const int questionVsUnreadable = InkDifference(image, questionRect, image, unreadableRect, surface);
            const int unreadableVsLoading = InkDifference(image, unreadableRect, pendingImage, loadingRect, surface);
            CHECK_NOTE(questionVsUnreadable >= 6, QString::number(questionVsUnreadable));
            CHECK_NOTE(unreadableVsLoading >= 6, QString::number(unreadableVsLoading));
            CHECK(InkPixels(image, unreadableRect, surface) > 4);
            CHECK(InkPixels(image, questionRect, surface) > 4);
            CHECK(InkPixels(pendingImage, loadingRect, surface) > 4);

            // 同一行内每个不可读单元格画得一样（同一个字形），并且都不同于紧邻的真实问号行里的同列单元格。
            bool sameGlyph = true;
            bool differsFromQuestion = true;
            for (std::uint64_t offset = 1; offset < 16; ++offset)
            {
                const QRect unreadableCell = canvas.cellRect(0x10010 + offset, Pane::Ascii);
                const QRect questionCell = canvas.cellRect(0x10020 + offset, Pane::Ascii);
                sameGlyph = sameGlyph
                    && InkDifference(image, canvas.cellRect(0x10010, Pane::Ascii), image, unreadableCell, surface) == 0;
                differsFromQuestion = differsFromQuestion
                    && InkDifference(image, unreadableCell, image, questionCell, surface) >= 6;
            }
            CHECK(sameGlyph);
            CHECK(differsFromQuestion);
        }

        // 填充菜单项：图标不再是循环箭头（夹具 qrc 里已没有该别名，旧别名解析为空），三项图标相同且与复制/粘贴不同。
        void TestFillMenuIcons()
        {
            ApplyTheme(false);
            auto fixture = MakeStaticFixture(0x1000, MakePattern(4096), true, QSize(900, 420));
            HexCanvas& canvas = *fixture->canvas;
            canvas.setCaretAddress(0x1010);
            canvas.setCaretAddress(0x1013, true);
            std::unique_ptr<QMenu> menu(canvas.buildContextMenu(0x1012, true));

            QAction* zero = FindAction(menu.get(), QStringLiteral("填充 00"));
            QAction* ff = FindAction(menu.get(), QStringLiteral("填充 FF"));
            QAction* nop = FindAction(menu.get(), QStringLiteral("NOP 填充（0x90）"));
            QAction* copy = FindAction(menu.get(), QStringLiteral("复制十六进制"));
            QAction* paste = FindAction(menu.get(), QStringLiteral("粘贴"));
            CHECK(zero != nullptr && ff != nullptr && nop != nullptr && copy != nullptr && paste != nullptr);
            if (zero == nullptr || ff == nullptr || nop == nullptr || copy == nullptr || paste == nullptr)
            {
                return;
            }

            // 旧别名（循环箭头）在夹具 qrc 里已不存在：如果填充项还在用它，图标会没有内容，下面的"画出了东西"就会失败。
            CHECK(!QFile::exists(QStringLiteral(":/Icon/codeeditor_replace.svg")));
            CHECK(QFile::exists(QStringLiteral(":/Icon/settings_background_reset.svg")));
            CHECK(!zero->icon().isNull() && !ff->icon().isNull() && !nop->icon().isNull());

            // 三个填充项图标一致（同一个动作族），且与复制、粘贴图标像素不同。
            const QSize size(16, 16);
            const QImage zeroImage = zero->icon().pixmap(size).toImage();
            const QImage ffImage = ff->icon().pixmap(size).toImage();
            const QImage nopImage = nop->icon().pixmap(size).toImage();
            const QImage copyImage = copy->icon().pixmap(size).toImage();
            const QImage pasteImage = paste->icon().pixmap(size).toImage();
            CHECK(zeroImage == ffImage && ffImage == nopImage);
            CHECK(zeroImage != copyImage);
            CHECK(zeroImage != pasteImage);

            // 确实画出了东西（不是全透明）。
            int opaque = 0;
            for (int y = 0; y < zeroImage.height(); ++y)
            {
                for (int x = 0; x < zeroImage.width(); ++x)
                {
                    opaque += qAlpha(zeroImage.pixel(x, y)) > 0 ? 1 : 0;
                }
            }
            CHECK(opaque > 8);
        }

        // 截图：不可读占位符对照与填充菜单，供目视核对（保存到 shots 目录）。
        void RenderShots(const QString& shotsDir)
        {
            QDir().mkpath(shotsDir);
            for (const bool dark : { false, true })
            {
                ApplyTheme(dark);
                auto holder = MakeAsyncCanvas(0x10000, 0x10FFF, true, true);
                HexCanvas& canvas = *holder->canvas;
                QByteArray page = MakePattern(4096);
                QByteArray mask(4096, '\x01');
                for (int index = 0x10; index < 0x20; ++index)
                {
                    mask[index] = '\0';
                }
                for (int index = 0x20; index < 0x30; ++index)
                {
                    page[index] = 0x3F;
                }
                mask[0x05] = '\0';
                page[0x00] = 0x3F;
                canvas.deliverPage(0x10000, page, mask, canvas.sourceRevision());
                canvas.setCaretAddress(0x10000);
                Flush();
                const QString suffix = dark ? QStringLiteral("dark") : QStringLiteral("light");
                GrabImage(canvas).save(QDir(shotsDir).filePath(QStringLiteral("signals-placeholders-%1.png").arg(suffix)));

                canvas.setCaretAddress(0x10040);
                canvas.setCaretAddress(0x10047, true);
                std::unique_ptr<QMenu> menu(canvas.buildContextMenu(0x10042, true));
                menu->popup(QPoint(40, 40));
                QApplication::processEvents();
                menu->grab().save(QDir(shotsDir).filePath(QStringLiteral("signals-menu-%1.png").arg(suffix)));
                menu->close();
            }
            ApplyTheme(false);
        }
    }

    // 第三组入口：视口坐标命中、占位符、填充图标与截图。
    void RunSignalViewTests(const QString& shotsDir)
    {
        TestViewportQueries();
        TestViewportQueriesEdges();
        TestUnreadablePlaceholder();
        TestFillMenuIcons();
        RenderShots(shotsDir);
    }
}
