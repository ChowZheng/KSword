// wpH_tests.TextRegression2.cpp
// 作用：第二轮独立审核（review2-wpH.md）针对 WorkbenchTextView 的补测。
// 并入自审核者补测 extra2.cpp 的 T09/T10/T27，改写成本仓库的 WPH_CHECK 断言风格。
// - T09（杀 rC13/rC14/rC16，回归 rA17）：U+2029（段分隔符）与 U+200B（零宽空格，Cf 类）
//   都要退化成点号；UTF-8 续体被块尾截断时归"不可读"；续体字节都读到了但形态非法时
//   是 U+FFFD（可读但非法），不是不可读占位符。
// - T10（杀 rC15）：窗口比"对齐前导"还短时，首行长度必须夹到窗口实际长度，不能越读。
// - T27（N7 修复）：UTF-16 窗口起点为奇数地址、首行按对齐边界裁成奇数长度之后，第二行及
//   以后仍必须按正确的码元边界配对，不能整体错位解出乱码。
// - M4（本轮自补新变异）：窗口起点为偶数地址（天然对齐）时，各行的奇偶判定必须是"偶数/
//   不需要特殊处理"，不能把 N7 的修法用反方向——否则本来不需要特殊处理的对齐场景反而
//   被改坏，这是"确认新修法没有把好的场景改坏"而不是"确认新场景被修好"，与 T27 判断的
//   方向互补。
// - M5（本轮自补新变异）：UTF-16 奇数起点块的"悬空首字节"必须按它自己的 validMask 区分
//   不可读/可读但不可见，不能统一画成点号——这是 N7 新增分支里从未被断言过的一半。

#include "wpH_common.h"

#include "../../../Ksword5.1/Ksword5.1/UI/CodeEditorWidget.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/HexCanvasFormat.h"

#include <QtTest/QtTest>

#include <iostream>

using ks::ui::DecodeTextChunkForTest;
using ks::ui::WorkbenchTextView;

namespace wpH_test
{
    namespace
    {
        QChar unreadableGlyph2()
        {
            return QChar(ks::ui::hexcanvas_format::kUnreadableAsciiGlyph);
        }

        std::vector<std::uint8_t> toVecTxt2(const QByteArray& bytes)
        {
            return std::vector<std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                reinterpret_cast<const std::uint8_t*>(bytes.constData()) + bytes.size());
        }

        void loadBytesTxt2(FakeBytesProvider& provider, const std::uint64_t base, const QByteArray& bytes)
        {
            provider.overlay().LoadBaseline(
                QStringLiteral("txt2").toStdString(), base, toVecTxt2(bytes), std::vector<std::uint8_t>(static_cast<std::size_t>(bytes.size()), 1));
        }

        // decodeFull：全部字节都当作有效（mask 全 1），直接调用生产函数，省去每个用例重复写掩码。
        QString decodeFull(const std::vector<std::uint8_t>& bytes, const WorkbenchTextView::Encoding encoding)
        {
            return DecodeTextChunkForTest(bytes, std::vector<std::uint8_t>(bytes.size(), 1), encoding);
        }

        // ---------------- T09（杀 rC13/rC14/rC16，回归 rA17）：若干字形判据 ----------------
        void runGlyphDistinctionTests()
        {
            const QChar dot(QLatin1Char('.'));

            // U+2029（段分隔符）：UTF-8 编码是 E2 80 A9。
            QString s = decodeFull({0x41, 0xE2, 0x80, 0xA9, 0x42}, WorkbenchTextView::Encoding::Utf8);
            WPH_CHECK_NOTE(s.size() == 3 && s.at(1) == dot, QStringLiteral("U+2029 应退化成点号，实得：%1").arg(s));

            // U+200B（零宽空格，Cf 类）：UTF-8 编码是 E2 80 8B。
            s = decodeFull({0x41, 0xE2, 0x80, 0x8B, 0x42}, WorkbenchTextView::Encoding::Utf8);
            WPH_CHECK_NOTE(s.size() == 3 && s.at(1) == dot, QStringLiteral("U+200B（Cf）应退化成点号，实得：%1").arg(s));

            // 同样两个码位的 UTF-16LE 编码（小端两字节）。
            s = decodeFull({0x29, 0x20}, WorkbenchTextView::Encoding::Utf16LE); // U+2029
            WPH_CHECK_NOTE(s.size() == 1 && s.at(0) == dot, QStringLiteral("UTF-16 的 U+2029 应退化成点号，实得：%1").arg(s));
            s = decodeFull({0x0B, 0x20}, WorkbenchTextView::Encoding::Utf16LE); // U+200B
            WPH_CHECK_NOTE(s.size() == 1 && s.at(0) == dot, QStringLiteral("UTF-16 的 U+200B 应退化成点号，实得：%1").arg(s));

            // 引导字节被这段字节的末尾截断（续体缺失）：这是数据缺口，不是编码错误，归不可读。
            s = decodeFull({0x41, 0xE4}, WorkbenchTextView::Encoding::Utf8);
            WPH_CHECK_NOTE(s.size() == 2 && s.at(1) == unreadableGlyph2(),
                QStringLiteral("被块尾截断的引导字节应归为不可读，实得：%1").arg(s));

            // 续体字节都读到了，但形态不是 10xxxxxx（0x41/0x42 都不满足）：可读但非法，U+FFFD。
            s = decodeFull({0xE4, 0x41, 0x42}, WorkbenchTextView::Encoding::Utf8);
            WPH_CHECK_NOTE(s.size() == 3 && s.at(0) == QChar(0xFFFDU) && s.at(1) == QChar(QLatin1Char('A')),
                QStringLiteral("续体形态非法但都读到了，应为 U+FFFD 而不是不可读占位符，实得：%1").arg(s));
        }

        // 形态正确的续体仍可能编码了非法 scalar：覆盖三种过长编码、代理区两端、
        // Unicode 上界之外和非法四字节引导范围；后面的 ASCII 不能被吞掉。
        void runUtf8ScalarBoundaryTests()
        {
            const std::vector<QByteArray> invalid{
                QByteArray::fromHex("C1A141"), QByteArray::fromHex("E081A141"),
                QByteArray::fromHex("F08081A141"), QByteArray::fromHex("EDA08041"),
                QByteArray::fromHex("EDBFBF41"), QByteArray::fromHex("F490808041"),
                QByteArray::fromHex("F580808041")
            };
            const QString expected = QString(QChar(0xFFFDU)) + QLatin1Char('A');
            for (const QByteArray& bytes : invalid)
            {
                const QString decoded = decodeFull(toVecTxt2(bytes), WorkbenchTextView::Encoding::Utf8);
                WPH_CHECK_NOTE(decoded == expected,
                    QStringLiteral("非法 UTF-8 %1 应为替换字符并保留 A，实得：%2").arg(QString::fromLatin1(bytes.toHex()), decoded));
            }

            // 合法边界继续使用既定显示规则：非 BMP 为点号，BMP 可打印字符原样显示。
            const QString decoded = decodeFull(toVecTxt2(QByteArray::fromHex("DFBFE0A080F0908080F48FBFBF41")),
                WorkbenchTextView::Encoding::Utf8);
            const QString expectedValid = QString(QChar(0x07FFU)) + QChar(0x0800U) + QStringLiteral("..A");
            WPH_CHECK_NOTE(decoded == expectedValid, decoded);

            const QString unreadable = DecodeTextChunkForTest({0xED, 0xA0, 0x80}, {1, 0, 1},
                WorkbenchTextView::Encoding::Utf8);
            WPH_CHECK(!unreadable.isEmpty() && unreadable.at(0) == unreadableGlyph2());
        }

        // ---------------- T10（杀 rC15）：窗口比对齐前导还短时首行不能越读 ----------------
        void runShortWindowDoesNotOverreadTests()
        {
            FakeBytesProvider provider(64);
            QByteArray letters;
            for (int i = 0; i < 64; ++i)
            {
                letters.append(static_cast<char>('a' + (i % 26)));
            }
            loadBytesTxt2(provider, 0x1000, letters);

            WorkbenchTextView view;
            view.setBytesProvider(&provider);
            view.setWindow(0x1004, 5); // 对齐边界在 0x1010，还差 12 字节，但窗口只有 5 字节
            WPH_CHECK_NOTE(view.editor()->text() == QStringLiteral("efghi"),
                QStringLiteral("窗口只有 5 字节，首行长度必须夹到 5，不能按对齐前导读到 12，实得：%1").arg(view.editor()->text()));
            view.setWindow(0x100F, 1); // 单字节窗口，同样不能越读
            WPH_CHECK_NOTE(view.editor()->text() == QStringLiteral("p"), view.editor()->text());
        }

        // ---------------- T27（N7 修复）：UTF-16 奇数起点，后续行不得整体错位 ----------------
        void runUtf16OddStartFramingTests()
        {
            FakeBytesProvider provider(64);
            QByteArray utf16;
            for (char c = 'A'; c <= 'Z'; ++c)
            {
                utf16.append(c);
                utf16.append('\0');
            }
            loadBytesTxt2(provider, 0x1001, utf16); // 窗口起点 0x1001 是奇数地址

            WorkbenchTextView view;
            view.setBytesProvider(&provider);
            view.setEncoding(WorkbenchTextView::Encoding::Utf16LE);
            view.setWindow(0x1001, static_cast<std::uint64_t>(utf16.size()));
            const QStringList lines = view.editor()->text().split(QLatin1Char('\n'));
            // 首行因为按 16 字节对齐边界裁短，长度是奇数（15 字节=7 个完整字符+1 个
            // 悬空字节），这一半的处理从修复前就是对的；修复要钉住的是"第二行开始"：
            // 如果错位没有被修正，这里会解出跟 I/J/K... 毫不相关的乱码字符。
            WPH_CHECK_NOTE(lines.size() >= 2 && lines.at(1).contains(QStringLiteral("IJKLMNO")),
                QStringLiteral("第二行及以后必须按正确码元边界配对，实得：%1").arg(lines.join(QStringLiteral(" / "))));
        }

        // ---------------- M4：偶数起点（天然对齐）场景不应被 N7 的奇偶判定改坏 ----------------
        void runUtf16EvenStartStillFramesCorrectlyTests()
        {
            FakeBytesProvider provider(64);
            QByteArray utf16;
            for (char c = 'A'; c <= 'Z'; ++c)
            {
                utf16.append(c);
                utf16.append('\0');
            }
            loadBytesTxt2(provider, 0x1000, utf16); // 窗口起点 0x1000 是偶数地址，天然对齐

            WorkbenchTextView view;
            view.setBytesProvider(&provider);
            view.setEncoding(WorkbenchTextView::Encoding::Utf16LE);
            view.setWindow(0x1000, static_cast<std::uint64_t>(utf16.size()));
            const QStringList lines = view.editor()->text().split(QLatin1Char('\n'));
            // 对齐场景下首行满 16 字节（8 个字符 ABCDEFGH），第二行从 I 开始，不应有任何
            // 悬空字节处理——如果 N7 的奇偶判定方向被写反，这个本来正常的场景反而会被
            // 错误地在每行开头吞掉一个字节，解出乱码。
            WPH_CHECK_NOTE(lines.size() >= 2 && lines.at(0) == QStringLiteral("ABCDEFGH") && lines.at(1).startsWith(QStringLiteral("IJKLMNOP")),
                QStringLiteral("偶数起点（天然对齐）场景不应被奇偶判定误伤，实得：%1").arg(lines.join(QStringLiteral(" / "))));
        }

        // ---------------- M5：悬空首字节要按自己的 validMask 区分不可读/可读不可见 ----------------
        void runUtf16OddLeadingByteRespectsValidMaskTests()
        {
            const QChar dot(QLatin1Char('.'));
            // oddLeadingByte=true、这个悬空字节本身不可读（validMask[0]=0）：必须是不可读
            // 占位符，不能统一画成点号（点号的语义是"可读但看不出字符"，两者不能混用）。
            QString unreadable = DecodeTextChunkForTest({0x00, 0x49, 0x00}, {0, 1, 1}, WorkbenchTextView::Encoding::Utf16LE, true);
            WPH_CHECK_NOTE(!unreadable.isEmpty() && unreadable.at(0) == unreadableGlyph2(),
                QStringLiteral("悬空首字节不可读时必须是不可读占位符，不是点号，实得：%1").arg(unreadable));
            // 同样的悬空字节，这次标记为可读：必须是点号（可读但看不出字符），不是不可读占位符。
            QString readable = DecodeTextChunkForTest({0x00, 0x49, 0x00}, {1, 1, 1}, WorkbenchTextView::Encoding::Utf16LE, true);
            WPH_CHECK_NOTE(!readable.isEmpty() && readable.at(0) == dot,
                QStringLiteral("悬空首字节可读时必须是点号，不是不可读占位符，实得：%1").arg(readable));
        }
    }

    void RunTextRegressionTests2()
    {
        const int before = g_checks;
        const int beforeFail = g_failures;
        runGlyphDistinctionTests();
        runUtf8ScalarBoundaryTests();
        runShortWindowDoesNotOverreadTests();
        runUtf16OddStartFramingTests();
        runUtf16EvenStartStillFramesCorrectlyTests();
        runUtf16OddLeadingByteRespectsValidMaskTests();
        std::cerr << "[TextRegression2] checks=" << (g_checks - before) << " failures=" << (g_failures - beforeFail) << std::endl;
    }
}
