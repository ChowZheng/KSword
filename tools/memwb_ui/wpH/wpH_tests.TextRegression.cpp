// wpH_tests.TextRegression.cpp
// 作用：修复波（wave2）针对 WorkbenchTextView 的回归测试与审核缺口补测。
// - D11a：Zl/Zp（行/段分隔符）与 Cf（格式字符）一律退化成点号，不再被当成"可打印"
//   原样输出，避免 CodeEditorWidget 把它们当真换行拆散逻辑行。
// - D11b：UTF-8 路径区分"可读但非法序列"（新占位符）与"压根没读到"（旧占位符），
//   不再混用同一个乘号。
// - D11c：ANSI 页按文件头注释实现——只认 0x20..0x7E，其余（含 Latin-1 扩展区）一律点号。
// - 可疑点 6：窗口起点非 bytesPerRow 整数倍时，第一行按对齐边界裁短，后续行落在与
//   十六进制页一致的绝对地址网格上。
// - D4：en-US 下状态行不含汉字、模板里的英文字面量不被误大写。

#include "wpH_common.h"

#include "../../../Ksword5.1/Ksword5.1/Internationalization/LanguageManager.h"
#include "../../../Ksword5.1/Ksword5.1/UI/CodeEditorWidget.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/HexCanvasFormat.h"

#include <QCoreApplication>
#include <QLabel>
#include <QtTest/QtTest>

#include <algorithm>
#include <iostream>

using ks::ui::DecodeTextChunkForTest;
using ks::ui::WorkbenchTextView;
namespace hexcanvas_format = ks::ui::hexcanvas_format;

namespace wpH_test
{
    namespace
    {
        QChar unreadableGlyph()
        {
            return QChar(hexcanvas_format::kUnreadableAsciiGlyph);
        }

        // ---------------- D11a：Zl/Zp/Cf 退化成点号 ----------------
        void runSeparatorGlyphTests()
        {
            // U+2028（行分隔符）与 U+2029（段分隔符）以 UTF-16LE 喂入：isPrint() 认为它们
            // "可打印"，但在 CodeEditorWidget 里会被当成真的换行，必须退化为点号。
            {
                const std::vector<std::uint8_t> bytes{0x41, 0x00, 0x28, 0x20, 0x42, 0x00};
                const std::vector<std::uint8_t> mask(bytes.size(), 1);
                const QString text = DecodeTextChunkForTest(bytes, mask, WorkbenchTextView::Encoding::Utf16LE);
                WPH_CHECK_NOTE(text.size() == 3, QStringLiteral("应解出 3 个码元，实得 %1：%2").arg(text.size()).arg(text));
                if (text.size() == 3)
                {
                    WPH_CHECK(text.at(0) == QChar(QLatin1Char('A')));
                    WPH_CHECK_NOTE(text.at(1) == QChar(QLatin1Char('.')),
                        QStringLiteral("U+2028 应退化成点号，实得 U+%1").arg(static_cast<unsigned>(text.at(1).unicode()), 0, 16));
                    WPH_CHECK(text.at(2) == QChar(QLatin1Char('B')));
                }
            }
            // UTF-8 路径同理：U+2028 的 UTF-8 编码是 E2 80 A8。
            {
                const std::vector<std::uint8_t> bytes{0x41, 0xE2, 0x80, 0xA8, 0x42};
                const std::vector<std::uint8_t> mask(bytes.size(), 1);
                const QString text = DecodeTextChunkForTest(bytes, mask, WorkbenchTextView::Encoding::Utf8);
                WPH_CHECK_NOTE(text.size() == 3, QStringLiteral("应解出 3 个字符，实得 %1：%2").arg(text.size()).arg(text));
                if (text.size() == 3)
                {
                    WPH_CHECK(text.at(0) == QChar(QLatin1Char('A')));
                    WPH_CHECK(text.at(1) == QChar(QLatin1Char('.')));
                    WPH_CHECK(text.at(2) == QChar(QLatin1Char('B')));
                }
            }
        }

        // ---------------- D11b：UTF-8 可读但非法 vs 压根没读到 ----------------
        void runUtf8GlyphDistinctionTests()
        {
            // 0xFF：永远不是合法的 UTF-8 引导字节，但字节本身是"可读到"的（validMask=1）。
            // 0x41：一个被标记为"没读到"的普通字节（validMask=0）。两者在旧代码里画出
            // 同一个乘号，无法区分；修复后必须是两个不同的占位符。
            const std::vector<std::uint8_t> bytes{0xFF, 0x41};
            const std::vector<std::uint8_t> mask{1, 0};
            const QString text = DecodeTextChunkForTest(bytes, mask, WorkbenchTextView::Encoding::Utf8);
            WPH_CHECK_NOTE(text.size() == 2, QStringLiteral("应各自产生一个占位符，实得 %1").arg(text.size()));
            if (text.size() == 2)
            {
                WPH_CHECK_NOTE(text.at(0) != unreadableGlyph(),
                    QStringLiteral("0xFF 可读但非法，不应与'不可读'用同一个占位符，实得 U+%1")
                        .arg(static_cast<unsigned>(text.at(0).unicode()), 0, 16));
                WPH_CHECK_NOTE(text.at(1) == unreadableGlyph(),
                    QStringLiteral("0x41 被标记不可读，应该是 kUnreadableAsciiGlyph"));
                WPH_CHECK_NOTE(text.at(0) != text.at(1), QStringLiteral("两种失败原因的占位符必须能分开看"));
            }

            // 续体字节本身不可读（而不是形态非法）：这是数据缺口，不是编码错误，仍然归为
            // "不可读"，不应该被误判成"可读但非法"。
            const std::vector<std::uint8_t> truncatedBytes{0xC3, 0xA9};
            const std::vector<std::uint8_t> truncatedMask{1, 0};
            const QString truncatedText = DecodeTextChunkForTest(truncatedBytes, truncatedMask, WorkbenchTextView::Encoding::Utf8);
            WPH_CHECK_NOTE(truncatedText.size() == 2, QStringLiteral("实得 %1").arg(truncatedText.size()));
            if (truncatedText.size() == 2)
            {
                WPH_CHECK_NOTE(truncatedText.at(0) == unreadableGlyph(),
                    QStringLiteral("续体不可读时，引导字节位置也应归为不可读（数据缺口），不是'可读但非法'"));
            }
        }

        // ---------------- UTF-16 奇数尾字节不可读（hM15） ----------------
        void runUtf16OddTailTests()
        {
            const std::vector<std::uint8_t> bytes{0x41, 0x00, 0x42};
            const std::vector<std::uint8_t> mask{1, 1, 0}; // 奇数尾字节 0x42 被标记不可读。
            const QString text = DecodeTextChunkForTest(bytes, mask, WorkbenchTextView::Encoding::Utf16LE);
            WPH_CHECK_NOTE(text.size() == 2, QStringLiteral("实得 %1").arg(text.size()));
            if (text.size() == 2)
            {
                WPH_CHECK(text.at(0) == QChar(QLatin1Char('A')));
                WPH_CHECK_NOTE(text.at(1) == unreadableGlyph(),
                    QStringLiteral("不可读的奇数尾字节应该是占位符，不是点号（点号意味着'可读但不可见'）"));
            }
        }

        // ---------------- D11c：ANSI 只认 0x20..0x7E ----------------
        void runAnsiAsciiOnlyTests()
        {
            // 0xFF 在旧代码（Latin-1）里会画成 'ÿ'；按文件头注释应该是点号。
            // 0x41（'A'，可见 ASCII）与 0x09（Tab，可读但不可打印）分别做对照。
            const std::vector<std::uint8_t> bytes{0x41, 0x09, 0xFF, 0xA0};
            const std::vector<std::uint8_t> mask(bytes.size(), 1);
            const QString text = DecodeTextChunkForTest(bytes, mask, WorkbenchTextView::Encoding::Ansi);
            WPH_CHECK_NOTE(text.size() == 4, QStringLiteral("实得 %1").arg(text.size()));
            if (text.size() == 4)
            {
                WPH_CHECK(text.at(0) == QChar(QLatin1Char('A')));
                WPH_CHECK_NOTE(text.at(1) == QChar(QLatin1Char('.')), QStringLiteral("Tab 可读但不可打印，应为点号"));
                WPH_CHECK_NOTE(text.at(2) == QChar(QLatin1Char('.')),
                    QStringLiteral("0xFF 按注释应为点号（不是 Latin-1 的 'ÿ'），实得 U+%1")
                        .arg(static_cast<unsigned>(text.at(2).unicode()), 0, 16));
                WPH_CHECK_NOTE(text.at(3) == QChar(QLatin1Char('.')), QStringLiteral("0xA0 同样应为点号，不是 Latin-1 不换行空格"));
            }
        }

        // ---------------- 可疑点 6：窗口起点非对齐时的行边界 ----------------
        void runAlignmentTests()
        {
            FakeBytesProvider provider(64);
            // 窗口起点 0x1004（相对 16 字节行宽偏 4），32 字节，全部可打印字符方便数行长。
            QByteArray bytes;
            for (int i = 0; i < 32; ++i)
            {
                bytes.append(static_cast<char>('A' + (i % 26)));
            }
            const std::uint64_t windowStart = 0x1004ULL;
            provider.overlay().LoadBaseline(QStringLiteral("align").toStdString(), windowStart,
                std::vector<std::uint8_t>(bytes.begin(), bytes.end()), std::vector<std::uint8_t>(static_cast<std::size_t>(bytes.size()), 1));

            WorkbenchTextView view;
            view.setBytesProvider(&provider);
            view.setBytesPerRow(16);
            view.setWindow(windowStart, static_cast<std::uint64_t>(bytes.size()));

            const QStringList lines = view.editor()->text().split(QLatin1Char('\n'));
            // 第一行只覆盖到下一个 16 对齐边界（0x1010），也就是 12 字节；
            // 第二行满 16 字节（到 0x1020）；第三行剩余 4 字节（到 0x1024）。
            WPH_CHECK_NOTE(lines.size() == 3, QStringLiteral("应该是 3 行（12+16+4），实得 %1 行：%2").arg(lines.size()).arg(view.editor()->text()));
            if (lines.size() == 3)
            {
                WPH_CHECK_NOTE(lines.at(0).size() == 12, QStringLiteral("第一行应为 12 字符（对齐到下一个 16 边界），实得 %1").arg(lines.at(0).size()));
                WPH_CHECK_NOTE(lines.at(1).size() == 16, QStringLiteral("第二行应为满宽 16 字符，实得 %1").arg(lines.at(1).size()));
                WPH_CHECK_NOTE(lines.at(2).size() == 4, QStringLiteral("第三行应为剩余 4 字符，实得 %1").arg(lines.at(2).size()));
            }
        }

        // ---------------- 窗口上限 1 MiB（hM16） ----------------
        void runWindowCapTests()
        {
            FakeBytesProvider provider(64);
            const std::uint64_t base = 0x100000ULL;
            provider.overlay().LoadBaseline(QStringLiteral("cap").toStdString(), base,
                std::vector<std::uint8_t>(2 * 1024 * 1024, 'A'), std::vector<std::uint8_t>(2 * 1024 * 1024, 1));
            WorkbenchTextView view;
            view.setBytesProvider(&provider);
            view.setBytesPerRow(16);
            view.setWindow(base, 2ULL * 1024ULL * 1024ULL); // 请求 2 MiB，必须被夹到 1 MiB。
            const qsizetype lineCount = view.editor()->text().count(QLatin1Char('\n')) + 1;
            WPH_CHECK_NOTE(lineCount == 65536, QStringLiteral("1 MiB / 16 字节每行 = 65536 行，实得 %1").arg(lineCount));
        }

        // ---------------- D4：en-US 下状态行不含汉字 ----------------
        void runI18nTests()
        {
            QString errorText;
            const bool loaded = ks::i18n::LanguageManager::instance().initialize(QStringLiteral("en-US"), &errorText);
            WPH_CHECK_NOTE(loaded, QStringLiteral("en-US 语言包应能加载：%1").arg(errorText));
            if (!loaded)
            {
                return;
            }
            FakeBytesProvider provider(64);
            provider.overlay().LoadBaseline(QStringLiteral("i18n").toStdString(), 0x9000, std::vector<std::uint8_t>(16, 'A'), std::vector<std::uint8_t>(16, 1));
            WorkbenchTextView view;
            view.setBytesProvider(&provider);
            view.setWindow(0x9000, 16);
            ks::i18n::LanguageManager::instance().retranslateAll();
            QCoreApplication::processEvents();
            const QString status = view.findChild<QLabel*>(QStringLiteral("ksMemwbTextStatus"))->text();
            const bool hasChinese = std::any_of(status.begin(), status.end(), [](const QChar ch) { return ch.unicode() >= 0x4E00 && ch.unicode() <= 0x9FFF; });
            WPH_CHECK_NOTE(!hasChinese, QStringLiteral("en-US 下状态行不应包含汉字：%1").arg(status));
            ks::i18n::LanguageManager::instance().initialize(QStringLiteral("zh-CN"));
            ks::i18n::LanguageManager::instance().retranslateAll();
        }
    }

    void RunTextRegressionTests()
    {
        const int before = g_checks;
        const int beforeFail = g_failures;
        runSeparatorGlyphTests();
        runUtf8GlyphDistinctionTests();
        runUtf16OddTailTests();
        runAnsiAsciiOnlyTests();
        runAlignmentTests();
        runWindowCapTests();
        runI18nTests();
        std::cerr << "[TextRegression] checks=" << (g_checks - before) << " failures=" << (g_failures - beforeFail) << std::endl;
    }
}
