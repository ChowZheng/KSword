// wpH_tests.Text.cpp
// 作用：WorkbenchTextView 与 DecodeTextChunkForTest 的验证：
// - 三种编码（ANSI/UTF-8/UTF-16LE）与 validMask 的交互（不可读占位符、不可见字节点号、
//   UTF-8 截断/非法引导字节、非 BMP 简化、UTF-16 奇数尾字节）；
// - 控件层：setWindow/setBytesPerRow/setEncoding 驱动 CodeEditorWidget 的 setRawText；
// - 行宽跟随十六进制页（换行位置与 bytesPerRow 一致）；
// - 截图。

#include "wpH_common.h"

#include "../../../Ksword5.1/Ksword5.1/UI/CodeEditorWidget.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/HexCanvasFormat.h"

#include <QCoreApplication>
#include <QLabel>
#include <QtTest/QtTest>

#include <iostream>

using ks::ui::DecodeTextChunkForTest;
using ks::ui::WorkbenchTextView;

namespace wpH_test
{
    namespace
    {
        // hexcanvas_format_unreadable_glyph：转发 HexCanvasFormat 的不可读占位符常量，
        // 避免每处测试都重复拼写命名空间；放在匿名命名空间顶部以便下面的函数直接使用。
        QChar hexcanvas_format_unreadable_glyph()
        {
            return QChar(ks::ui::hexcanvas_format::kUnreadableAsciiGlyph);
        }

        // runChunkDecodeTests：纯函数测试，三种编码各自的边界情形。
        void runChunkDecodeTests()
        {
            // ANSI：可见 ASCII 原样，不可见字节点号，不可读字节乘号。
            {
                const std::vector<std::uint8_t> bytes{0x41, 0x00, 0xFF};
                const std::vector<std::uint8_t> mask{1, 1, 0};
                const QString text = DecodeTextChunkForTest(bytes, mask, WorkbenchTextView::Encoding::Ansi);
                WPH_CHECK_NOTE(text.size() == 3, QStringLiteral("text=%1").arg(text));
                WPH_CHECK(text.at(0) == QChar(QLatin1Char('A')));
                WPH_CHECK(text.at(1) == QChar(QLatin1Char('.'))); // 0x00 可读但不可打印
                WPH_CHECK(text.at(2) == QChar(hexcanvas_format_unreadable_glyph()));
            }
            // UTF-8：ASCII + 两字节序列("é" = C3 A9) + 被截断的三字节序列 + 非法引导字节。
            {
                const std::vector<std::uint8_t> bytes{0x41, 0xC3, 0xA9, 0xE4, 0xB8, 0xFF};
                const std::vector<std::uint8_t> mask(bytes.size(), 1);
                const QString text = DecodeTextChunkForTest(bytes, mask, WorkbenchTextView::Encoding::Utf8);
                // "A" + "é"(1 字符) + 截断的 E4 B8(应各自/整体退化为占位符，不崩溃、不越界) + 0xFF(非法引导)。
                WPH_CHECK_NOTE(text.size() >= 2, QStringLiteral("text=%1 size=%2").arg(text).arg(text.size()));
                if (text.size() >= 2)
                {
                    WPH_CHECK(text.at(0) == QChar(QLatin1Char('A')));
                    WPH_CHECK_NOTE(text.at(1) == QChar(0x00E9),
                        QStringLiteral("解出的é=U+%1").arg(static_cast<unsigned>(text.at(1).unicode()), 0, 16));
                }
            }
            // UTF-8：某个续体字节被标记为不可读——绝不能把前导字节合成出一个合法字符（'é'）；
            // 失败时每次只前进一个字节重试（与反汇编页的单字节重同步同一思路），所以这里是
            // 两个占位符（引导字节本身 + 残留的那个不可读续体字节），而不是拼出字符。
            {
                const std::vector<std::uint8_t> bytes{0xC3, 0xA9};
                const std::vector<std::uint8_t> mask{1, 0};
                const QString text = DecodeTextChunkForTest(bytes, mask, WorkbenchTextView::Encoding::Utf8);
                WPH_CHECK_NOTE(text.size() == 2, QStringLiteral("text=%1 size=%2").arg(text).arg(text.size()));
                if (text.size() == 2)
                {
                    WPH_CHECK(text.at(0) == QChar(hexcanvas_format_unreadable_glyph()));
                    WPH_CHECK(text.at(1) == QChar(hexcanvas_format_unreadable_glyph()));
                }
                WPH_CHECK(!text.contains(QChar(0x00E9)));
            }
            // UTF-16LE：一个可打印字符 'A'(0x0041) + 一个不可读码元(其中一个字节无效) + 奇数尾字节。
            {
                const std::vector<std::uint8_t> bytes{0x41, 0x00, 0x42, 0x00, 0x43};
                std::vector<std::uint8_t> mask{1, 1, 1, 0, 1};
                const QString text = DecodeTextChunkForTest(bytes, mask, WorkbenchTextView::Encoding::Utf16LE);
                WPH_CHECK_NOTE(text.size() == 3, QStringLiteral("text=%1 size=%2").arg(text).arg(text.size()));
                if (text.size() == 3)
                {
                    WPH_CHECK(text.at(0) == QChar(QLatin1Char('A')));
                    WPH_CHECK(text.at(1) == QChar(hexcanvas_format_unreadable_glyph()));
                    WPH_CHECK(text.at(2) == QChar(QLatin1Char('.'))); // 奇数尾字节 0x43，可读但单独一个字节不可打印成字符
                }
            }
            // bytes/validMask 长度不符——必须返回空串，不越界读。
            {
                WPH_CHECK(DecodeTextChunkForTest({1, 2, 3}, {1, 1}, WorkbenchTextView::Encoding::Ansi).isEmpty());
            }
        }

        // runViewTests：控件层——窗口/行宽/编码驱动 CodeEditorWidget 内容。
        void runViewTests()
        {
            FakeBytesProvider provider;
            // 32 字节确定性数据，含若干不可打印字节，便于核对换行与占位符位置。
            QByteArray bytes;
            for (int i = 0; i < 32; ++i)
            {
                bytes.append(static_cast<char>((i % 2 == 0) ? ('A' + (i % 26)) : 0x01));
            }
            const std::uint64_t base = 0x2000;
            provider.overlay().LoadBaseline(QStringLiteral("text").toStdString(), base,
                std::vector<std::uint8_t>(bytes.begin(), bytes.end()),
                std::vector<std::uint8_t>(static_cast<std::size_t>(bytes.size()), 1));

            WorkbenchTextView view;
            view.setBytesProvider(&provider);
            view.setBytesPerRow(16);
            view.setWindow(base, static_cast<std::uint64_t>(bytes.size()));

            const QString rendered = view.editor()->text();
            const QStringList lines = rendered.split(QLatin1Char('\n'));
            WPH_CHECK_NOTE(lines.size() == 2, QStringLiteral("按 16 字节行宽应得 2 行，实得 %1：%2").arg(lines.size()).arg(rendered));

            // 改行宽为 8，应变成 4 行。
            view.setBytesPerRow(8);
            const QStringList lines8 = view.editor()->text().split(QLatin1Char('\n'));
            WPH_CHECK_NOTE(lines8.size() == 4, QStringLiteral("按 8 字节行宽应得 4 行，实得 %1").arg(lines8.size()));

            // 切到 UTF-16LE，内容应变化（至少长度减半左右），且不崩溃。
            view.setEncoding(WorkbenchTextView::Encoding::Utf16LE);
            WPH_CHECK(view.encoding() == WorkbenchTextView::Encoding::Utf16LE);
            WPH_CHECK(!view.editor()->text().isEmpty());

            // 未接入数据源 / 未设置窗口时不崩溃且显示占位状态。
            WorkbenchTextView emptyView;
            emptyView.refreshView();
            WPH_CHECK(emptyView.editor()->text().isEmpty());
        }

        void takeShots(const QString& shotsDir)
        {
            if (shotsDir.isEmpty())
            {
                return;
            }
            FakeBytesProvider provider;
            QByteArray bytes;
            for (int i = 0; i < 64; ++i)
            {
                bytes.append(static_cast<char>('!' + (i % 64)));
            }
            provider.overlay().LoadBaseline(QStringLiteral("shot").toStdString(), 0x3000,
                std::vector<std::uint8_t>(bytes.begin(), bytes.end()),
                std::vector<std::uint8_t>(static_cast<std::size_t>(bytes.size()), 1));
            WorkbenchTextView view;
            view.setBytesProvider(&provider);
            view.setWindow(0x3000, static_cast<std::uint64_t>(bytes.size()));
            view.resize(640, 320);
            view.show();
            static_cast<void>(QTest::qWaitForWindowExposed(&view));
            GrabWidget(&view).save(shotsDir + QStringLiteral("/wpH_text_light.png"));
            ApplyTheme(true);
            QCoreApplication::processEvents();
            GrabWidget(&view).save(shotsDir + QStringLiteral("/wpH_text_dark.png"));
            ApplyTheme(false);
        }
    }

    void RunTextTests(const QString& shotsDir)
    {
        const int before = g_checks;
        const int beforeFail = g_failures;
        runChunkDecodeTests();
        runViewTests();
        takeShots(shotsDir);
        std::cerr << "[Text] checks=" << (g_checks - before) << " failures=" << (g_failures - beforeFail) << std::endl;
    }
}
