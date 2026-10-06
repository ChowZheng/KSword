// memwb_ui_tests.HexView.Export.cpp
// 作用：HexView 导出的离屏验证——
//   1) 转储格式化（纯函数）：与旧控件兼容的 "0xADDR  AA BB …  |ascii|"，每行 16 字节，越界补 "--" 与空格，
//      不可见字节点号，空数据为空串，贴着 64 位末端的地址；
//   2) 选中字节的十六进制文本（大写、空格分隔）；
//   3) 写文件层：二进制逐字节一致、文本 UTF-8 且换行经文本模式、失败路径（目录不存在、路径是目录、空路径）带原因；
//   4) HexView 的 exportXxxTo：内容含已应用的编辑、状态条与 exportFinished 信号、失败时 false 且不留文件、
//      空数据/空选区的拒绝。
// 对话框层（QFileDialog / 错误框）只有用户手势才会走到，这里只验证样式与建议文件名等不弹窗的部分。

#include "memwb_ui_hexview.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

namespace memwb_test
{
    namespace
    {
        namespace he = ks::ui::hexexport;
        using ks::ui::HexView;

        // ReadAll：读取整个文件（二进制）。
        QByteArray ReadAll(const QString& path)
        {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
            {
                return QByteArray();
            }
            return file.readAll();
        }

        // ReadText：按文本模式读取（\r\n 还原成 \n）。
        QString ReadText(const QString& path)
        {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            {
                return QString();
            }
            return QString::fromUtf8(file.readAll());
        }

        // 转储格式：逐行精确比较。
        void TestDumpFormat()
        {
            // 20 个可见字节：第一行满 16 字节，第二行 4 字节 + 12 个 "--" 补位 + 12 个空格补 ASCII。
            const QByteArray text = QByteArray("ABCDEFGHIJKLMNOPQRST");
            const QString dump = he::FormatDump(0x1000, text);
            const QStringList lines = dump.split(QChar(u'\n'));
            CHECK(lines.size() == 2);
            CHECK(lines.value(0) == QStringLiteral(
                "0x0000000000001000  41 42 43 44 45 46 47 48 49 4A 4B 4C 4D 4E 4F 50  |ABCDEFGHIJKLMNOP|"));
            CHECK(lines.value(1) == QStringLiteral(
                "0x0000000000001010  51 52 53 54 -- -- -- -- -- -- -- -- -- -- -- --  |QRST            |"));
            CHECK(!dump.endsWith(QChar(u'\n')));

            // 恰好 16 的整数倍：没有多余的半行。
            CHECK(he::FormatDump(0, QByteArray(32, 'A')).split(QChar(u'\n')).size() == 2);
            CHECK(he::FormatDump(0, QByteArray(16, 'A')).split(QChar(u'\n')).size() == 1);
            CHECK(he::FormatDump(0, QByteArray(17, 'A')).split(QChar(u'\n')).size() == 2);

            // 不可见字节（0x00、0x1F、0x7F、0x80、0xFF）一律点号；空格（0x20）与 ~（0x7E）是可见字符。
            QByteArray special;
            for (const int value : { 0x00, 0x1F, 0x20, 0x41, 0x7E, 0x7F, 0x80, 0xFF })
            {
                special.append(static_cast<char>(value));
            }
            const QString specialLine = he::FormatDump(0x10, special);
            CHECK(specialLine == QStringLiteral(
                "0x0000000000000010  00 1F 20 41 7E 7F 80 FF -- -- -- -- -- -- -- --  |.. A~...        |"));

            // 起点不对齐：第一行就从起点开始（与旧控件一致，不做画布那样的对齐补位）。
            const QString unaligned = he::FormatDump(0x1008, QByteArray(3, 'z'));
            CHECK(unaligned.startsWith(QStringLiteral("0x0000000000001008  7A 7A 7A --")));

            // 地址是 16 位大写十六进制，前缀小写 x；贴着 64 位末端的地址不回绕。
            const QString highDump = he::FormatDump(0xFFFFFFFFFFFFFFF0ULL, QByteArray(16, 'Q'));
            CHECK(highDump.startsWith(QStringLiteral("0xFFFFFFFFFFFFFFF0  51 51")));
            const QString highTwoRows = he::FormatDump(0xFFFFFFFFFFFFFFF8ULL, QByteArray(8, 'Q'));
            CHECK(highTwoRows.startsWith(QStringLiteral("0xFFFFFFFFFFFFFFF8  51")));

            // 空数据为空串；非正数行宽按 16 处理。
            CHECK(he::FormatDump(0, QByteArray()).isEmpty());
            CHECK(he::FormatDump(0, QByteArray(20, 'A'), 0).split(QChar(u'\n')).size() == 2);
            CHECK(he::FormatDump(0, QByteArray(20, 'A'), -4).split(QChar(u'\n')).size() == 2);

            // 自定义行宽：8 字节一行。
            CHECK(he::FormatDump(0, QByteArray(20, 'A'), 8).split(QChar(u'\n')).size() == 3);

            // 单行格式化：offset 越过数据末尾的列全部补位。
            CHECK(he::FormatDumpRow(0x20, QByteArray("AB"), 1, 4) == QStringLiteral("0x0000000000000020  42 -- -- --  |B   |"));
            CHECK(he::FormatDumpRow(0x20, QByteArray("AB"), 5, 2) == QStringLiteral("0x0000000000000020  -- --  |  |"));

            // 选中字节：大写、单空格分隔；空为空串。
            CHECK(he::FormatSelectedHex(QByteArray::fromHex("41ab0cFF")) == QStringLiteral("41 AB 0C FF"));
            CHECK(he::FormatSelectedHex(QByteArray()).isEmpty());
            CHECK(he::FormatSelectedHex(QByteArray(1, '\x05')) == QStringLiteral("05"));

            // 建议文件名按种类区分。
            CHECK(he::SuggestedFileName(he::Kind::Binary).endsWith(QStringLiteral(".bin")));
            CHECK(he::SuggestedFileName(he::Kind::HexDump).endsWith(QStringLiteral(".txt")));
            CHECK(he::SuggestedFileName(he::Kind::SelectedHex).endsWith(QStringLiteral(".txt")));
            CHECK(he::SuggestedFileName(he::Kind::HexDump) != he::SuggestedFileName(he::Kind::SelectedHex));
        }

        // 写文件层：成功与失败。
        void TestWriteFiles()
        {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            QString error;

            // 二进制：全部 256 个字节值 + 超过 64 KiB 的内容，逐字节一致。
            QByteArray everything;
            for (int round = 0; round < 300; ++round)
            {
                for (int value = 0; value < 256; ++value)
                {
                    everything.append(static_cast<char>(value));
                }
            }
            const QString binaryPath = directory.filePath(QStringLiteral("all.bin"));
            CHECK(he::WriteBinaryFile(binaryPath, everything, &error));
            CHECK(error.isEmpty());
            CHECK(ReadAll(binaryPath) == everything);

            // 覆盖已有文件（截断而不是追加）。
            CHECK(he::WriteBinaryFile(binaryPath, QByteArray("xy"), &error));
            CHECK(ReadAll(binaryPath) == QByteArray("xy"));

            // 空内容也是合法导出（得到空文件）。
            CHECK(he::WriteBinaryFile(binaryPath, QByteArray(), &error));
            CHECK(QFile(binaryPath).exists() && QFile(binaryPath).size() == 0);

            // 文本：UTF-8，换行经文本模式（读回统一为 \n）。
            const QString textPath = directory.filePath(QStringLiteral("t.txt"));
            CHECK(he::WriteTextFile(textPath, QStringLiteral("第一行\n第二行"), &error));
            CHECK(ReadText(textPath) == QStringLiteral("第一行\n第二行"));
            CHECK(ReadAll(textPath).startsWith(QStringLiteral("第一行").toUtf8()));

            // 失败：目录不存在、路径本身是目录、空路径与纯空白路径；原因带路径与系统说明，不留文件。
            const QString missing = directory.filePath(QStringLiteral("no_such_dir/out.bin"));
            CHECK(!he::WriteBinaryFile(missing, QByteArray("x"), &error));
            CHECK(error.contains(QStringLiteral("无法写入文件")) && error.contains(QStringLiteral("no_such_dir")));
            CHECK(!QFile::exists(missing));
            error.clear();
            CHECK(!he::WriteTextFile(directory.path(), QStringLiteral("x"), &error));
            CHECK(error.contains(QStringLiteral("无法写入文件")));
            CHECK(!he::WriteBinaryFile(QString(), QByteArray("x"), &error));
            CHECK(error == QStringLiteral("没有指定导出路径"));
            CHECK(!he::WriteBinaryFile(QStringLiteral("   "), QByteArray("x"), &error));
            CHECK(!he::WriteBinaryFile(missing, QByteArray("x"), nullptr));
        }

        // HexView 的导出：内容、状态条、信号、失败路径。
        void TestViewExport()
        {
            ApplyTheme(false);
            QTemporaryDir directory;
            CHECK(directory.isValid());
            const QByteArray data = MakePattern(100);
            auto view = MakeHexView(0x4000, data, true, QSize(1000, 420));
            QSignalSpy finished(view.get(), &HexView::exportFinished);
            view->canvas()->setFocus();

            // 先做一次编辑：导出的是含已应用编辑的缓冲。
            ClickAddress(*view->canvas(), 0x4005);
            Type(*view->canvas(), QStringLiteral("EE"));
            QByteArray expected = data;
            expected[5] = static_cast<char>(0xEE);
            CHECK(view->buffer() == expected);

            // 二进制。
            const QString binaryPath = directory.filePath(QStringLiteral("view.bin"));
            CHECK(view->exportBinaryTo(binaryPath));
            CHECK(ReadAll(binaryPath) == expected);
            CHECK(finished.count() == 1 && finished.at(0).at(0).toBool());
            CHECK(view->statusBar()->hasMessage());
            CHECK(view->statusBar()->messageKind() == ks::ui::HexViewStatusBar::Kind::Info);
            CHECK(view->statusBar()->messageText() == QStringLiteral("导出完成：100 字节 -> %1").arg(binaryPath));
            CHECK(finished.at(0).at(1).toString() == view->statusBar()->messageText());

            // 十六进制转储文本：与格式化函数一致（文本模式读回）。
            const QString dumpPath = directory.filePath(QStringLiteral("view.txt"));
            CHECK(view->exportHexTextTo(dumpPath));
            CHECK(ReadText(dumpPath) == he::FormatDump(0x4000, expected));
            CHECK(ReadText(dumpPath).startsWith(QStringLiteral("0x0000000000004000  ")));
            CHECK(ReadText(dumpPath).contains(QStringLiteral(" EE ")));
            CHECK(finished.count() == 2 && finished.at(1).at(0).toBool());

            // 选中字节：选 0x4004..0x4007 共 4 个字节。
            ClickAddress(*view->canvas(), 0x4004);
            ClickAddress(*view->canvas(), 0x4007, ks::ui::HexCanvas::ActivePane::Hex, Qt::ShiftModifier);
            const QString selectedPath = directory.filePath(QStringLiteral("sel.txt"));
            CHECK(view->exportSelectedHexTo(selectedPath));
            CHECK(ReadText(selectedPath) == he::FormatSelectedHex(expected.mid(4, 4)));
            CHECK(ReadText(selectedPath).split(QChar(u' ')).size() == 4);
            CHECK(ReadText(selectedPath).split(QChar(u' ')).at(1) == QStringLiteral("EE"));
            CHECK(view->statusBar()->messageText().contains(QStringLiteral("4 字节十六进制数据")));

            // 失败：目录不存在——false、状态条为错误、信号 ok=false、不留文件，缓冲不受影响。
            const QString missing = directory.filePath(QStringLiteral("nope/out.bin"));
            const int before = finished.count();
            CHECK(!view->exportBinaryTo(missing));
            CHECK(finished.count() == before + 1 && !finished.at(before).at(0).toBool());
            CHECK(view->statusBar()->messageKind() == ks::ui::HexViewStatusBar::Kind::Error);
            CHECK(view->statusBar()->messageText().startsWith(QStringLiteral("导出失败：")));
            CHECK(view->statusBar()->messageText().contains(QStringLiteral("无法写入文件")));
            CHECK(!QFile::exists(missing));
            CHECK(view->buffer() == expected);
            CHECK(!view->exportHexTextTo(missing));
            CHECK(!view->exportSelectedHexTo(missing));

            // 失败：路径是目录。
            CHECK(!view->exportBinaryTo(directory.path()));

            // 空数据与空选区：拒绝，文案明确，不写文件。
            view->clearBuffer();
            const QString emptyPath = directory.filePath(QStringLiteral("empty.bin"));
            CHECK(!view->exportBinaryTo(emptyPath));
            CHECK(view->statusBar()->messageText() == QStringLiteral("导出失败：当前无数据"));
            CHECK(!view->exportHexTextTo(emptyPath));
            CHECK(!view->exportSelectedHexTo(emptyPath));
            CHECK(view->statusBar()->messageText() == QStringLiteral("导出失败：未选中有效字节"));
            CHECK(!QFile::exists(emptyPath));

            // 带对话框的槽在没有数据时直接报错，不弹对话框（否则测试会挂住）。
            const int beforeSlots = finished.count();
            view->exportBinary();
            view->exportHexText();
            view->exportSelectedHex();
            CHECK(finished.count() == beforeSlots + 3);
            CHECK(!finished.at(beforeSlots).at(0).toBool());
        }
    }

    // 导出验证入口。
    void RunHexViewExportTests()
    {
        TestDumpFormat();
        TestWriteFiles();
        TestViewExport();
    }
}
