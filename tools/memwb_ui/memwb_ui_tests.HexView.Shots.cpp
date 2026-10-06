// memwb_ui_tests.HexView.Shots.cpp
// 作用：HexView 的截图（深/浅 x 窄/宽），保存到 shots 目录供目视核对：
//   工具栏全貌（含参照着色与插入点/选区）、查找条带命中（十六进制与文本）、查找条回绕提示、查找条无效模式错误、
//   跳转条带行内错误、跳转条历史菜单、解释器展开、极简嵌入外观、行宽菜单、导出菜单。
// 每张图都由真实控件按真实手势构造（点击、键盘输入、公开接口），不是拼图；弹出菜单的图把菜单窗口合成到控件图上。

#include "memwb_ui_hexview.h"

#include <QApplication>
#include <QDir>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPixmap>

namespace memwb_test
{
    namespace
    {
        using ks::ui::HexCanvas;
        using ks::ui::HexFindBar;
        using ks::ui::HexView;
        using Pane = HexCanvas::ActivePane;

        // kBase：截图数据的起始地址。
        constexpr std::uint64_t kBase = 0x00401000ULL;

        // MakeShotData：4 KiB 的可读数据：循环的英文句子（ASCII 列有内容），夹着几处二进制特征。
        //   0x10 / 0x70 / 0xD0  4D 5A 90 00（"MZ" 头样式，供十六进制查找）
        //   0x40 / 0x120 / 0x1A0 "hello" 大小写混合（供文本查找）
        QByteArray MakeShotData()
        {
            const QByteArray sentence = QByteArray("The quick brown fox jumps over the lazy dog 0123456789. ");
            QByteArray data;
            while (data.size() < 4096)
            {
                data.append(sentence);
            }
            data.truncate(4096);
            const auto put = [&data](int offset, const QByteArray& bytes) {
                for (int index = 0; index < bytes.size(); ++index)
                {
                    data[offset + index] = bytes.at(index);
                }
            };
            for (const int offset : { 0x10, 0x70, 0xD0 })
            {
                put(offset, QByteArray::fromHex("4D5A9000"));
            }
            put(0x40, QByteArray("hello"));
            put(0x120, QByteArray("Hello"));
            put(0x1A0, QByteArray("HELLO"));
            return data;
        }

        // Shot：把控件抓成图保存；失败计为一次断言失败。
        void Shot(QWidget& widget, const QString& dir, const QString& name)
        {
            Flush();
            const bool saved = widget.grab().save(QDir(dir).filePath(name));
            CHECK_NOTE(saved, name);
        }

        // ShotWithMenu：把弹出的菜单合成到控件图上再保存。
        // 传入：控件、菜单、弹出位置（控件坐标）、目录与文件名。
        void ShotWithMenu(QWidget& widget, QMenu* menu, const QPoint& position, const QString& dir, const QString& name)
        {
            emit menu->aboutToShow();
            menu->popup(widget.mapToGlobal(position));
            Flush();
            QPixmap composite = widget.grab();
            QPainter painter(&composite);
            painter.drawPixmap(widget.mapFromGlobal(menu->pos()), menu->grab());
            painter.end();
            menu->hide();
            Flush();
            CHECK_NOTE(composite.save(QDir(dir).filePath(name)), name);
        }

        // 构造一个带数据、选区与参照着色的控件（工具栏全貌用）。
        std::unique_ptr<HexView> MakeOverview(const QSize& size)
        {
            const QByteArray data = MakeShotData();
            auto view = MakeHexView(kBase, data, true, size);

            // 参照：一处待提交（橙）与一处外部变化（冷）。
            QByteArray original = data;
            for (const int index : { 0x22, 0x23, 0x61 })
            {
                original[index] = static_cast<char>(~original.at(index));
            }
            QByteArray previous = original;
            for (const int index : { 0x40, 0x41 })
            {
                previous[index] = static_cast<char>(~previous.at(index));
            }
            view->setReference(original, previous);

            // 插入点与选区。
            ClickAddress(*view->canvas(), kBase + 0x30);
            ClickAddress(*view->canvas(), kBase + 0x3B, Pane::Hex, Qt::ShiftModifier);
            view->canvas()->setFocus();
            return view;
        }

        // 工具栏全貌：浅/深 x 宽。
        void ShotOverview(const QString& dir)
        {
            for (const bool dark : { false, true })
            {
                ApplyTheme(dark);
                auto view = MakeOverview(QSize(1100, 520));
                Shot(*view, dir, dark ? QStringLiteral("02-toolbar-dark-wide.png") : QStringLiteral("01-toolbar-light-wide.png"));
            }
        }

        // 查找条：宽 + 深色带十六进制命中；窄 + 浅色文本回绕；宽 + 浅色无效模式。
        void ShotFind(const QString& dir)
        {
            // 十六进制命中（深色，宽）：视口里三处 MZ 头，选中第一处之后的下一处。
            ApplyTheme(true);
            {
                auto view = MakeHexView(kBase, MakeShotData(), false, QSize(1100, 520));
                view->openFind();
                view->findBar()->setMode(HexFindBar::Mode::Hex);
                view->findBar()->setPatternText(QStringLiteral("4D 5A 90 00"));
                CHECK(view->findBar()->findNext());
                PumpUntil([&]() { return !view->findBar()->isSearching(); }, 5000);
                Flush();
                CHECK(view->findBar()->findNext());
                PumpUntil([&]() { return !view->findBar()->isSearching(); }, 5000);
                Shot(*view, dir, QStringLiteral("03-find-hits-dark-wide.png"));
            }

            // 文本回绕（浅色，窄）：hello 忽略大小写，从最后一个命中继续下一个，回绕到开头。
            ApplyTheme(false);
            {
                auto view = MakeHexView(kBase, MakeShotData(), false, QSize(640, 420));
                view->openFind();
                view->findBar()->setMode(HexFindBar::Mode::TextUtf8);
                view->findBar()->setPatternText(QStringLiteral("hello"));
                for (int index = 0; index < 4; ++index)
                {
                    CHECK(view->findBar()->findNext());
                    PumpUntil([&]() { return !view->findBar()->isSearching(); }, 5000);
                    Flush();
                }
                Shot(*view, dir, QStringLiteral("04-find-wrapped-light-narrow.png"));
            }

            // 无效模式（浅色，宽）：4D5 少一位。
            {
                auto view = MakeHexView(kBase, MakeShotData(), false, QSize(1100, 420));
                view->openFind();
                view->findBar()->setPatternText(QStringLiteral("4D5"));
                CHECK(!view->findBar()->findNext());
                Shot(*view, dir, QStringLiteral("05-find-invalid-light-wide.png"));
            }
        }

        // 跳转条：行内错误（浅色宽）；历史菜单（深色窄）。
        void ShotGoto(const QString& dir)
        {
            ApplyTheme(false);
            {
                auto view = MakeHexView(kBase, MakeShotData(), false, QSize(1100, 420));
                view->openGoto();
                view->gotoBar()->setInputText(QStringLiteral("9999"));
                CHECK(!view->gotoBar()->submit());
                Shot(*view, dir, QStringLiteral("06-goto-error-light-wide.png"));
            }

            ApplyTheme(true);
            {
                QSettings settings;
                settings.clear();
                settings.sync();
                auto view = MakeHexView(kBase, MakeShotData(), false, QSize(640, 420));
                view->openGoto();
                for (const QString& text : { QStringLiteral("401010"), QStringLiteral("401070"), QStringLiteral("4010D0") })
                {
                    view->gotoBar()->setInputText(text);
                    CHECK(view->gotoBar()->submit());
                }
                view->gotoBar()->setInputText(QString());
                const QPoint corner = view->gotoBar()->historyButton()->mapTo(
                    view.get(), view->gotoBar()->historyButton()->rect().bottomLeft());
                ShotWithMenu(*view, view->gotoBar()->historyMenu(), corner, dir, QStringLiteral("07-goto-history-dark-narrow.png"));
                settings.clear();
                settings.sync();
            }
        }

        // 解释器展开：深色宽、浅色窄。
        void ShotInspector(const QString& dir)
        {
            for (const bool dark : { true, false })
            {
                ApplyTheme(dark);
                QSettings settings;
                settings.clear();
                settings.sync();
                const QSize size = dark ? QSize(1200, 540) : QSize(820, 480);
                auto view = MakeHexView(0x00400000ULL, MakeInspectorData(), true, size);
                view->setInspectorVisible(true);
                Flush();
                ClickAddress(*view->canvas(), 0x00400010);
                ClickAddress(*view->canvas(), 0x00400013, Pane::Hex, Qt::ShiftModifier);
                view->canvas()->setFocus();
                Shot(*view, dir, dark ? QStringLiteral("08-inspector-dark-wide.png") : QStringLiteral("09-inspector-light-narrow.png"));
            }
        }

        // 极简嵌入外观：工具栏与状态条都隐藏，只剩画布；浅/深各一张。
        void ShotMinimal(const QString& dir)
        {
            for (const bool dark : { false, true })
            {
                ApplyTheme(dark);
                auto view = MakeHexView(kBase, MakeShotData(), false, QSize(620, 300));
                view->setToolbarVisible(false);
                view->setStatusBarVisible(false);
                view->setBytesPerRow(16);
                ClickAddress(*view->canvas(), kBase + 0x20);
                ClickAddress(*view->canvas(), kBase + 0x2A, Pane::Hex, Qt::ShiftModifier);
                Shot(*view, dir, dark ? QStringLiteral("11-embedded-minimal-dark.png") : QStringLiteral("10-embedded-minimal-light.png"));
            }
        }

        // 菜单：行宽菜单（浅色）、导出菜单（深色）。
        void ShotMenus(const QString& dir)
        {
            ApplyTheme(false);
            {
                auto view = MakeOverview(QSize(900, 420));
                const QPoint corner = view->rowWidthButton()->mapTo(view.get(), view->rowWidthButton()->rect().bottomLeft());
                ShotWithMenu(*view, view->rowWidthMenu(), corner, dir, QStringLiteral("12-rowwidth-menu-light.png"));
            }
            ApplyTheme(true);
            {
                auto view = MakeOverview(QSize(900, 420));
                const QPoint corner = view->exportButton()->mapTo(view.get(), view->exportButton()->rect().bottomLeft());
                ShotWithMenu(*view, view->exportMenu(), corner, dir, QStringLiteral("13-export-menu-dark.png"));
            }
        }

        // 状态条瞬时消息：只读视图下尝试填充，状态条显示原因（浅色）。
        void ShotStatusMessage(const QString& dir)
        {
            ApplyTheme(false);
            auto view = MakeHexView(kBase, MakeShotData(), false, QSize(900, 380));
            ClickAddress(*view->canvas(), kBase + 0x20);
            view->canvas()->fillSelection(0x00);
            Shot(*view, dir, QStringLiteral("14-status-readonly-light.png"));
        }
    }

    // 截图入口。
    void RunHexViewShots(const QString& shotsDir)
    {
        QDir().mkpath(shotsDir);
        const QString dir = QDir(shotsDir).absolutePath();
        ShotOverview(dir);
        ShotFind(dir);
        ShotGoto(dir);
        ShotInspector(dir);
        ShotMinimal(dir);
        ShotMenus(dir);
        ShotStatusMessage(dir);
        ApplyTheme(false);
    }
}
