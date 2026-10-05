// memwb_ui_render.cpp
// 作用：HexCanvas 的渲染验证与截图——深/浅主题 x 窄/宽 x 选区/暂存编辑/含未读字节，
// 同时做主题切换（不通知画布）与"各变化着色在两套主题下可区分且文字可读"的像素断言。
// 截图输出到 shotsDir，文件名自解释。

#include "memwb_ui_common.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QImage>
#include <QMenu>

#include <algorithm>

namespace memwb_test
{
    namespace
    {
        using ks::ui::HexCanvas;
        using Pane = HexCanvas::ActivePane;

        // Scene：一个截图场景持有的全部对象。overlay 与 provider 先于 canvas 声明，保证 canvas 先析构。
        struct Scene
        {
            ksword::memwb::MemoryDiffOverlay overlay;           // 暂存叠加层
            RecordingProvider provider;                         // 未读场景用的记录型提供者
            QByteArray data;                                    // 底层数据
            std::unique_ptr<HexCanvas> canvas;                  // 被测画布
        };

        // ToVector：QByteArray 转 vector，用于喂叠加层。
        std::vector<std::uint8_t> ToVector(const QByteArray& bytes)
        {
            return std::vector<std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                reinterpret_cast<const std::uint8_t*>(bytes.constData()) + bytes.size());
        }

        // MakeTextData：生成一块含大量可见 ASCII 的数据，截图里 ASCII 面板才有可读的内容。
        QByteArray MakeTextData(int size)
        {
            QByteArray bytes = MakePattern(size);
            const QByteArray text("The quick brown fox jumps over the lazy dog 0123456789 KSword HexCanvas! ");
            for (int offset = 0x40; offset + text.size() < size; offset += 0x180)
            {
                bytes.replace(offset, text.size(), text);
            }
            return bytes;
        }

        // BuildSelectionScene：选区场景——跨行、跨分组的选区，外加两个通用高亮层（搜索命中与书签）。
        std::unique_ptr<Scene> BuildSelectionScene(const QSize& size, int bytesPerRow)
        {
            auto scene = std::make_unique<Scene>();
            scene->data = MakeTextData(4096);
            scene->canvas = std::make_unique<HexCanvas>();
            scene->canvas->resize(size);
            scene->canvas->show();
            scene->canvas->setBytesPerRow(bytesPerRow);
            scene->canvas->setStaticData(0x00401000, scene->data);
            scene->canvas->setCaretAddress(0x00401000 + 0x23);
            scene->canvas->setCaretAddress(0x00401000 + 0x5A, true);
            scene->canvas->setHighlightRanges(1, { { 0x00401000 + 0x80, 0x00401000 + 0x8B }, { 0x00401000 + 0xC4, 0x00401000 + 0xC9 } }, QColor(255, 196, 0), QStringLiteral("搜索命中"));
            scene->canvas->setHighlightRanges(2, { { 0x00401000 + 0xB0, 0x00401000 + 0xB3 } }, QColor(0, 192, 220), QStringLiteral("书签"));
            QApplication::processEvents();
            return scene;
        }

        // BuildStagedScene：暂存编辑场景——三种变化着色同时出现，并停在半字节输入中。
        // 数据 B 是"当前内容"；叠加层基线先载入 A 再 Refresh 成 B，于是 A、B 不同的字节是外部变化。
        std::unique_ptr<Scene> BuildStagedScene(const QSize& size, int bytesPerRow)
        {
            auto scene = std::make_unique<Scene>();
            const std::uint64_t base = 0x00401000;
            const QByteArray before = MakeTextData(2048);
            QByteArray after = before;
            for (const int offset : { 0x24, 0x25, 0x26, 0x57, 0x70, 0x71 })
            {
                after[offset] = static_cast<char>(after.at(offset) ^ 0x5A);
            }
            // 自己写入：先暂存、再确认写入并回读，数据 B 里这几个字节就是写入后的值。
            const std::vector<std::uint8_t> written = { 0x11, 0x22, 0x33 };
            for (std::size_t index = 0; index < written.size(); ++index)
            {
                after[0xA0 + static_cast<int>(index)] = static_cast<char>(written[index]);
            }
            scene->data = after;

            scene->canvas = std::make_unique<HexCanvas>();
            scene->canvas->resize(size);
            scene->canvas->show();
            scene->canvas->setBytesPerRow(bytesPerRow);
            scene->canvas->setEditable(true);
            scene->canvas->setOverlay(&scene->overlay);
            scene->canvas->setStaticData(base, scene->data);

            const std::string key = "memwb-shot-staged";
            scene->overlay.LoadBaseline(key, base, ToVector(before), std::vector<std::uint8_t>(before.size(), 1));
            scene->overlay.RefreshBaseline(key, base, ToVector(after), std::vector<std::uint8_t>(after.size(), 1));

            // 自己写入标记。
            ksword::memwb::DiffBlock block;
            block.address = base + 0xA0;
            block.before = ToVector(after.mid(0xA0, 3));
            block.after = written;
            scene->overlay.AcceptWrite(block, written);

            // 暂存编辑：通过画布的真实输入手势，改 4 个字节，再在下一个字节停在半字节。
            scene->canvas->setCaretAddress(base + 0x40);
            Type(*scene->canvas, QStringLiteral("DEADBEEF"));
            Type(*scene->canvas, QStringLiteral("9"));
            scene->canvas->setCaretAddress(base + 0xD0);
            scene->canvas->setCaretAddress(base + 0xD7, true);
            scene->canvas->fillSelection(0x90);
            scene->canvas->setCaretAddress(base + 0x44);
            Type(*scene->canvas, QStringLiteral("C"));
            QApplication::processEvents();
            return scene;
        }

        // BuildMissingScene：含未读字节场景——部分读（?? 间隔）、整页不可读（??）、在途与未加载（··）。
        // 地址取用户态高地址，同时展示 16 位地址列。
        std::unique_ptr<Scene> BuildMissingScene(const QSize& size, int bytesPerRow)
        {
            auto scene = std::make_unique<Scene>();
            const std::uint64_t base = 0x00007FFF12340000ULL;
            scene->canvas = std::make_unique<HexCanvas>();
            scene->canvas->resize(size);
            scene->canvas->show();
            scene->canvas->setBytesPerRow(bytesPerRow);
            scene->canvas->setAddressSpace(base, base + 0x4FFF);
            scene->canvas->setPageProvider(&scene->provider);

            const std::uint64_t revision = scene->provider.requests.empty() ? scene->canvas->sourceRevision() : scene->provider.requests.back().revision;
            // 第 0 页：读到了，但页尾附近两段没读到（部分读，画成 ??）。
            QByteArray page = MakeTextData(4096);
            QByteArray mask(4096, '\x01');
            for (int index = 0xFD0; index < 0xFD8; ++index)
            {
                mask[index] = '\0';
            }
            for (int index = 0xFF0; index < 0xFF5; ++index)
            {
                mask[index] = '\0';
            }
            scene->canvas->deliverPage(base, page, mask, revision);
            // 第 1 页：整页不可读。第 2 页保持在途，第 3 页以后保持未加载。
            ks::ui::HexFetchRange unreadable;
            unreadable.firstPageStart = base + 0x1000;
            unreadable.pageCount = 1;
            scene->canvas->deliverUnreadable(unreadable, revision);
            // 视图停在第 0 页末尾：先看到带 ?? 空洞的有效数据，再看到整页不可读的第 1 页。
            scene->canvas->scrollToAddress(base + 0xFC0, HexCanvas::ScrollAlign::Top);
            scene->canvas->setCaretAddress(base + 0xFC6);
            scene->canvas->setCaretAddress(base + 0x1009, true);
            QApplication::processEvents();
            return scene;
        }

        // 场景工厂的统一签名。
        using SceneBuilder = std::unique_ptr<Scene> (*)(const QSize&, int);

        // MaxContrastInRect：矩形内任一像素与底色的最大对比度，用来判断文字是否可读。
        double MaxContrastInRect(const QImage& image, const QRect& rect, const QColor& background)
        {
            double best = 1.0;
            const QRect clipped = rect.intersected(image.rect());
            for (int y = clipped.top(); y <= clipped.bottom(); ++y)
            {
                for (int x = clipped.left(); x <= clipped.right(); ++x)
                {
                    best = std::max(best, KswordTheme::ContrastRatio(image.pixelColor(x, y), background));
                }
            }
            return best;
        }

        // ProbeBackground：取单元格底色探针（单元格左侧内边距的第一行像素，不会碰到文字或外框）。
        QColor ProbeBackground(const QImage& image, const QRect& cell)
        {
            return image.pixelColor(cell.x() - cell.width() / 8, cell.y());
        }

        // 渲染一整组截图。
        void SaveShots(const QString& shotsDir)
        {
            struct SceneSpec
            {
                const char* name;           // 文件名里的场景名
                SceneBuilder builder;       // 构造函数
            };
            const SceneSpec specs[] = {
                { "selection", &BuildSelectionScene },
                { "staged-edit", &BuildStagedScene },
                { "unread-bytes", &BuildMissingScene },
            };
            struct WidthSpec
            {
                const char* name;           // 文件名里的宽度名
                QSize size;                 // 画布尺寸
                int bytesPerRow;            // 每行字节数（窄屏用 8 个，宽屏用 16 个）
            };
            const WidthSpec widths[] = {
                { "narrow", QSize(520, 340), 8 },
                { "wide", QSize(1100, 420), 16 },
            };

            for (const bool dark : { true, false })
            {
                ApplyTheme(dark);
                for (const WidthSpec& width : widths)
                {
                    for (const SceneSpec& spec : specs)
                    {
                        auto scene = spec.builder(width.size, width.bytesPerRow);
                        const QString path = QStringLiteral("%1/hexcanvas-%2-%3-%4.png")
                            .arg(shotsDir)
                            .arg(dark ? QStringLiteral("dark") : QStringLiteral("light"))
                            .arg(QLatin1String(width.name))
                            .arg(QLatin1String(spec.name));
                        const QPixmap shot = scene->canvas->grab();
                        CHECK_NOTE(shot.save(path), path);
                        CHECK(!shot.isNull() && shot.width() == width.size.width());
                    }
                }

                // 附加：窄屏 16 字节一行（出现横向滚动条）、4 字节分组、48 字节宽行。
                auto narrow16 = BuildSelectionScene(QSize(520, 300), 16);
                CHECK(narrow16->canvas->grab().save(QStringLiteral("%1/hexcanvas-%2-narrow-16-hscroll.png")
                    .arg(shotsDir).arg(dark ? QStringLiteral("dark") : QStringLiteral("light"))));
                auto grouped = BuildSelectionScene(QSize(1100, 300), 16);
                grouped->canvas->setGroupSize(4);
                CHECK(grouped->canvas->grab().save(QStringLiteral("%1/hexcanvas-%2-wide-group4.png")
                    .arg(shotsDir).arg(dark ? QStringLiteral("dark") : QStringLiteral("light"))));

                // 附加：第 1 页末尾到第 2 页开头，整页不可读（??）紧接着在途页（··）。
                auto boundary = BuildMissingScene(QSize(1100, 300), 16);
                boundary->canvas->scrollToAddress(0x00007FFF12340000ULL + 0x1FA0, HexCanvas::ScrollAlign::Top);
                CHECK(boundary->canvas->cellStateAt(0x00007FFF12340000ULL + 0x1FA0).hexText == QStringLiteral("??"));
                CHECK(boundary->canvas->cellStateAt(0x00007FFF12340000ULL + 0x2010).hexText == QString(2, QChar(0x00B7)));
                CHECK(boundary->canvas->grab().save(QStringLiteral("%1/hexcanvas-%2-wide-unread-to-loading.png")
                    .arg(shotsDir).arg(dark ? QStringLiteral("dark") : QStringLiteral("light"))));

                // 右键菜单：带图标、不透明背景，深浅各一张。
                auto menuScene = BuildStagedScene(QSize(700, 330), 16);
                menuScene->canvas->setCaretAddress(0x00401000 + 0x40);
                menuScene->canvas->setCaretAddress(0x00401000 + 0x47, true);
                QMenu* menu = menuScene->canvas->buildContextMenu(0x00401000 + 0x42, true);
                menu->popup(QPoint(30, 30));
                QApplication::processEvents();
                CHECK(menu->grab().save(QStringLiteral("%1/hexcanvas-%2-context-menu.png")
                    .arg(shotsDir).arg(dark ? QStringLiteral("dark") : QStringLiteral("light"))));
                menu->close();
                delete menu;
            }
        }

        // 主题：画布不收任何通知，切换主题后下一次绘制就是新主题。
        void TestThemeSwitch()
        {
            ApplyTheme(false);
            auto scene = BuildStagedScene(QSize(1100, 420), 16);
            HexCanvas& canvas = *scene->canvas;
            const QImage light = GrabImage(canvas);
            const QPoint corner(light.width() - 4, canvas.cellRect(0x00401000 + 0x40, Pane::Hex).y() + 3);
            CHECK(light.pixelColor(corner) == KswordTheme::SurfaceColor());

            ApplyTheme(true);
            const QImage dark = GrabImage(canvas);
            CHECK(dark.pixelColor(corner) == KswordTheme::SurfaceColor());
            CHECK(dark.pixelColor(corner) != light.pixelColor(corner));
            CHECK(dark != light);

            // 回到浅色，画面逐像素回到原样：没有任何缓存颜色残留。
            ApplyTheme(false);
            CHECK(GrabImage(canvas) == light);
        }

        // 变化着色：深浅两套主题下，暂存/外部变化/自己写入/选区/高亮彼此可区分，并且文字与底色对比度足够。
        void TestChangeKindColors()
        {
            for (const bool dark : { false, true })
            {
                ApplyTheme(dark);
                auto staged = BuildStagedScene(QSize(1100, 420), 16);
                HexCanvas& canvas = *staged->canvas;
                // 去掉半字节预览与选区，避免盖住被测单元格。
                canvas.setCaretAddress(0x00401000 + 0x300, false, false);
                canvas.setActivePane(Pane::Hex);
                const QImage image = GrabImage(canvas);
                const std::uint64_t base = 0x00401000;
                const QColor surface = KswordTheme::SurfaceColor();

                // 取各类单元格：0x40 暂存、0x57 外部变化、0xA0 自己写入、0x30 无变化。
                const QRect pendingCell = canvas.cellRect(base + 0x40, Pane::Hex);
                const QRect externalCell = canvas.cellRect(base + 0x57, Pane::Hex);
                const QRect selfCell = canvas.cellRect(base + 0xA0, Pane::Hex);
                const QRect plainCell = canvas.cellRect(base + 0x30, Pane::Hex);
                CHECK(canvas.cellStateAt(base + 0x40).change == HexCanvas::ChangeKind::Pending);
                CHECK(canvas.cellStateAt(base + 0x57).change == HexCanvas::ChangeKind::ExternalChange);
                CHECK(canvas.cellStateAt(base + 0xA0).change == HexCanvas::ChangeKind::SelfWritten);
                CHECK(canvas.cellStateAt(base + 0x30).change == HexCanvas::ChangeKind::Unchanged);

                const QColor plainBg = ProbeBackground(image, plainCell);
                const QColor pendingBg = ProbeBackground(image, pendingCell);
                const QColor externalBg = ProbeBackground(image, externalCell);
                const QColor selfBg = ProbeBackground(image, selfCell);
                CHECK(plainBg == surface);
                const QString note = QStringLiteral("dark=%1 pending=%2 external=%3 self=%4")
                    .arg(dark).arg(pendingBg.name()).arg(externalBg.name()).arg(selfBg.name());
                CHECK_NOTE(ColorDistance(pendingBg, plainBg) >= 30, note);
                CHECK_NOTE(ColorDistance(externalBg, plainBg) >= 30, note);
                CHECK_NOTE(ColorDistance(selfBg, plainBg) >= 15, note);
                CHECK_NOTE(ColorDistance(pendingBg, externalBg) >= 30, note);
                CHECK_NOTE(ColorDistance(pendingBg, selfBg) >= 20, note);
                CHECK_NOTE(ColorDistance(externalBg, selfBg) >= 20, note);

                // 文字可读：每种底色上，单元格内最强的文字像素与底色的对比度 >= 4.5（WCAG AA 正文）。
                CHECK_NOTE(MaxContrastInRect(image, pendingCell, pendingBg) >= 4.5, note);
                CHECK_NOTE(MaxContrastInRect(image, externalCell, externalBg) >= 4.5, note);
                CHECK_NOTE(MaxContrastInRect(image, selfCell, selfBg) >= 4.5, note);
                CHECK_NOTE(MaxContrastInRect(image, plainCell, plainBg) >= 4.5, note);

                // 选区：活动面板与镜像面板的底色不同，且都可读。
                canvas.setCaretAddress(base + 0x100);
                canvas.setCaretAddress(base + 0x10F, true);
                const QImage selected = GrabImage(canvas);
                const QRect hexSel = canvas.cellRect(base + 0x105, Pane::Hex);
                const QRect asciiSel = canvas.cellRect(base + 0x105, Pane::Ascii);
                const QColor hexSelBg = ProbeBackground(selected, hexSel);
                const QColor asciiSelBg = ProbeBackground(selected, asciiSel);
                CHECK_NOTE(ColorDistance(hexSelBg, surface) >= 40, hexSelBg.name());
                CHECK_NOTE(ColorDistance(asciiSelBg, surface) >= 20, asciiSelBg.name());
                CHECK_NOTE(ColorDistance(hexSelBg, asciiSelBg) >= 20, QStringLiteral("%1 %2").arg(hexSelBg.name()).arg(asciiSelBg.name()));
                CHECK_NOTE(MaxContrastInRect(selected, hexSel, hexSelBg) >= 4.5, hexSelBg.name());
                CHECK_NOTE(MaxContrastInRect(selected, asciiSel, asciiSelBg) >= 4.5, asciiSelBg.name());

                // 切到 ASCII 面板后活动/镜像互换。
                canvas.setActivePane(Pane::Ascii);
                const QImage swapped = GrabImage(canvas);
                CHECK(ColorDistance(ProbeBackground(swapped, asciiSel), asciiSelBg) >= 20);
                CHECK(ColorDistance(ProbeBackground(swapped, asciiSel), ProbeBackground(swapped, hexSel)) >= 20);

                // 未读/不可读单元格：用禁用色，仍然可见（对比度 >= 3）。
                auto missing = BuildMissingScene(QSize(1100, 420), 16);
                const QImage missingImage = GrabImage(*missing->canvas);
                const std::uint64_t gap = 0x00007FFF12340000ULL + 0xFD2;
                const std::uint64_t unreadablePage = 0x00007FFF12340000ULL + 0x1010;
                CHECK(MaxContrastInRect(missingImage, missing->canvas->cellRect(gap, Pane::Hex), surface) >= 3.0);
                CHECK(MaxContrastInRect(missingImage, missing->canvas->cellRect(unreadablePage, Pane::Hex), surface) >= 3.0);
                CHECK(missing->canvas->cellStateAt(gap).hexText == QStringLiteral("??"));
                CHECK(missing->canvas->cellStateAt(unreadablePage).hexText == QStringLiteral("??"));
            }
            ApplyTheme(false);
        }
    }

    // 本文件全部测试与截图的入口。
    void RunRenderTests(const QString& shotsDir)
    {
        QDir().mkpath(shotsDir);
        TestThemeSwitch();
        TestChangeKindColors();
        SaveShots(shotsDir);
    }
}
