// wpH_tests.Disasm.cpp
// 作用：WorkbenchDisasmView 与 DecodeWindowResynced 的验证：
// - 纯算法重同步（受控假后端，覆盖"失败只插一个 db 字节并继续重同步"的多种排列）；
// - 真实 Zydis 后端的两个集成场景（连续真实指令 / 缓冲区末尾截断必然失败）；
// - 单击不进编辑、双击/F2/Enter 进编辑；可跟随地址时 Enter 改为跳转；Backspace 返回；
// - 行内编辑：真实汇编后端编译成功（含 NOP 补齐）与超长被拒绝（编辑框不关、错误原因可见）；
// - 窗口外提示行；截图（深/浅 × 窄/宽）。

#include "wpH_common.h"

#include <QApplication>
#include <QCoreApplication>
#include <QLabel>
#include <QLineEdit>
#include <QPair>
#include <QSignalSpy>
#include <QTableView>
#include <QtTest/QtTest>

#include <iostream>
#include <optional>

using ks::ui::DecodedRow;
using ks::ui::DecodeOneFn;
using ks::ui::DecodeWindowResynced;
using ks::ui::WorkbenchDisasmView;

namespace wpH_test
{
    namespace
    {
        // byteArrayToVector：QByteArray -> std::vector<uint8_t>，测试构造输入用的小转换。
        std::vector<std::uint8_t> byteArrayToVector(const QByteArray& bytes)
        {
            return std::vector<std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                reinterpret_cast<const std::uint8_t*>(bytes.constData()) + bytes.size());
        }

        // makeFakeDecoder：受控假后端——letters 里值为 0 的位置表示"该字节后端解码失败"，
        // 否则该位置消耗 letters[i] 个字节、decoded=true，助记符按偏移编号便于断言。
        DecodeOneFn makeFakeDecoder(const std::vector<int>& consumeByOffset)
        {
            return [consumeByOffset](const std::uint8_t* bytes, const std::size_t available, const std::uint64_t address, bool)
                -> std::optional<DecodedRow> {
                const auto index = static_cast<std::size_t>(bytes[0]); // 用首字节当"位置键"，见调用方构造
                if (index >= consumeByOffset.size() || consumeByOffset[index] <= 0)
                {
                    return std::nullopt;
                }
                const auto length = static_cast<std::size_t>(consumeByOffset[index]);
                if (length > available)
                {
                    return std::nullopt;
                }
                DecodedRow row;
                row.address = address;
                row.bytes = QByteArray(reinterpret_cast<const char*>(bytes), static_cast<qsizetype>(length));
                row.mnemonic = QStringLiteral("fake%1").arg(index);
                row.decoded = true;
                return row;
            };
        }

        // runResyncAlgorithmTests：纯算法测试，假后端按"首字节是位置键"的约定模拟任意失败排列。
        void runResyncAlgorithmTests()
        {
            // 场景 1：中间恰好一个字节失败，前后都是 2 字节"指令"。
            // 字节序列：[10,A][11,B][99(无映射→失败)][12,C][13,D]；位置键 10/11/12/13 各消耗 2 字节，
            // 99 超出 consumeByOffset 范围，必然失败。
            {
                const std::vector<std::uint8_t> bytes{10, 0xAA, 11, 0xBB, 99, 12, 0xCC, 13, 0xDD};
                std::vector<int> consume(14, 0);
                consume[10] = 2;
                consume[11] = 2;
                consume[12] = 2;
                consume[13] = 2;
                const auto decoder = makeFakeDecoder(consume);
                const QVector<DecodedRow> rows = DecodeWindowResynced(bytes, 0x1000, decoder, 1024, true);
                // 5 行：两条有效(0x1000/0x1002) + 一条 db(0x1004) + 两条有效(0x1005/0x1007)。
                WPH_CHECK_NOTE(rows.size() == 5, QStringLiteral("rows=%1").arg(rows.size()));
                if (rows.size() == 5)
                {
                    WPH_CHECK(rows[0].decoded && rows[0].bytes.size() == 2 && rows[0].address == 0x1000);
                    WPH_CHECK(rows[1].decoded && rows[1].bytes.size() == 2 && rows[1].address == 0x1002);
                    WPH_CHECK(rows[2].decoded == false && rows[2].bytes.size() == 1 && rows[2].mnemonic == QStringLiteral("db"));
                    WPH_CHECK_NOTE(rows[2].address == 0x1004, QStringLiteral("db 行地址=0x%1").arg(rows[2].address, 0, 16));
                    WPH_CHECK(rows[3].decoded && rows[3].bytes.size() == 2 && rows[3].address == 0x1005);
                    WPH_CHECK(rows[4].decoded && rows[4].bytes.size() == 2 && rows[4].address == 0x1007);
                }
            }
            // 场景 2：连续两个字节都失败——必须各自产生独立的一条 db 行（不得合并成一条多字节行），
            // 这正是修旧缺陷 E-02 的核心判据：整段降级解码器会把失败点之后的内容一起按猜测式切分，
            // 本算法必须逐字节独立重试。
            {
                const std::vector<std::uint8_t> bytes{9, 9, 0, 0xAA};
                const auto decoder = makeFakeDecoder({2, 0, 0});
                const QVector<DecodedRow> rows = DecodeWindowResynced(bytes, 0x2000, decoder, 1024, true);
                WPH_CHECK_NOTE(rows.size() == 3, QStringLiteral("rows=%1").arg(rows.size()));
                if (rows.size() == 3)
                {
                    WPH_CHECK(!rows[0].decoded && rows[0].bytes.size() == 1 && rows[0].address == 0x2000);
                    WPH_CHECK(!rows[1].decoded && rows[1].bytes.size() == 1 && rows[1].address == 0x2001);
                    WPH_CHECK(rows[2].decoded && rows[2].bytes.size() == 2 && rows[2].address == 0x2002);
                }
            }
            // 场景 3：maxInstructions 提前停止——不应该解码出超过上限的行数。
            {
                const std::vector<std::uint8_t> bytes{0, 0, 0, 0, 0, 0};
                const auto decoder = makeFakeDecoder({1});
                const QVector<DecodedRow> rows = DecodeWindowResynced(bytes, 0x3000, decoder, 3, true);
                WPH_CHECK_NOTE(rows.size() == 3, QStringLiteral("rows=%1").arg(rows.size()));
            }
            // 场景 4：空输入 / 空后端——必须返回空列表，不崩溃。
            {
                WPH_CHECK(DecodeWindowResynced({}, 0, makeFakeDecoder({1}), 10, true).isEmpty());
                WPH_CHECK(DecodeWindowResynced({0, 0}, 0, DecodeOneFn(), 10, true).isEmpty());
            }
        }

        // runRealZydisTests：真实 Zydis 后端的两个场景，见文件头说明。
        void runRealZydisTests()
        {
            const DecodeOneFn decoder = MakeRealZydisDecodeBackend();
            WPH_CHECK(static_cast<bool>(decoder));

            // 场景 A：push rbp; mov rbp, rsp; sub rsp, 0x10; ret —— 四条真实指令全部正确解码。
            const std::vector<std::uint8_t> prologue{0x55, 0x48, 0x89, 0xE5, 0x48, 0x83, 0xEC, 0x10, 0xC3};
            const QVector<DecodedRow> rows = DecodeWindowResynced(prologue, 0x140001000ULL, decoder, 64, true);
            WPH_CHECK_NOTE(rows.size() == 4, QStringLiteral("真实 Zydis 应解出 4 行，实得 %1").arg(rows.size()));
            if (rows.size() == 4)
            {
                WPH_CHECK(rows[0].decoded && rows[0].bytes.size() == 1);
                WPH_CHECK(rows[1].decoded && rows[1].bytes.size() == 3);
                WPH_CHECK(rows[2].decoded && rows[2].bytes.size() == 4);
                WPH_CHECK(rows[3].decoded && rows[3].bytes.size() == 1);
            }

            // 场景 B：缓冲区末尾只剩一个 0x0F（两字节转义前缀，没有后续字节必然解码失败），
            // 必须恰好退化成一条 db 行，不得影响前一条已解码的 NOP。
            const std::vector<std::uint8_t> truncated{0x90, 0x0F};
            const QVector<DecodedRow> truncatedRows = DecodeWindowResynced(truncated, 0x400000ULL, decoder, 64, true);
            WPH_CHECK_NOTE(truncatedRows.size() == 2, QStringLiteral("截断场景应为 2 行，实得 %1").arg(truncatedRows.size()));
            if (truncatedRows.size() == 2)
            {
                WPH_CHECK(truncatedRows[0].decoded && truncatedRows[0].mnemonic.contains(QStringLiteral("nop"), Qt::CaseInsensitive));
                WPH_CHECK(!truncatedRows[1].decoded && truncatedRows[1].bytes == QByteArray(1, '\x0F'));
            }
        }

        // runViewInteractionTests：单击/双击/F2/Enter 分支、Backspace 返回栈、窗口外提示行。
        void runViewInteractionTests()
        {
            FakeBytesProvider provider;
            // push rbp(1); mov rbp,rsp(3); nop(1); jmp rel32=0(5，目标=本行地址+5)。
            const QByteArray bytes = QByteArray::fromHex("554889E590E900000000");
            const std::uint64_t base = 0x140001000ULL;
            const QByteArray mask(bytes.size(), '\x01');
            provider.overlay().LoadBaseline(QStringLiteral("test").toStdString(), base, byteArrayToVector(bytes), byteArrayToVector(mask));

            WorkbenchDisasmView view;
            view.setBytesProvider(&provider);
            view.setDecodeBackend(MakeRealZydisDecodeBackend());
            view.setAssembleBackend(MakeRealAssembleBackend());
            // D3 修复后默认架构已经取 provider->AddressBits()（FakeBytesProvider 默认就是
            // 64 位），这里的 setAddressBits(64) 严格来说是多余的；保留它是为了在 D3 修复
            // 被回归时这条用例依然成立（程序化调用不应改变已经正确的默认值），并明确表达
            // "本测试的字节序列按 x64 编写"这件事不依赖隐式默认值。
            view.setAddressBits(64);
            WPH_CHECK(view.isX64());
            view.resize(760, 420);
            view.show();
            WPH_CHECK(QTest::qWaitForWindowExposed(&view));

            // 暂存第 0 行的字节（push→pop，仍是 1 字节、仍可解码），用来钉住"暂存补丁行必须
            // 着色、未改动行不能着色"这条规则，不只靠人工看截图。
            provider.overlay().Stage(base, {0x5D});

            WPH_CHECK(view.jumpTo(base));
            QCoreApplication::processEvents();
            // 4 条真实指令 + 1 条"超出已读取窗口"提示行（基线只加载了这 10 字节，窗口请求
            // 的是固定的 kDecodeWindowBytes，之后的字节本就没有数据，提示行是正确行为）。
            WPH_CHECK_NOTE(view.model()->rowCount() == 5, QStringLiteral("rowCount=%1").arg(view.model()->rowCount()));
            {
                const QVariant pendingBg = view.model()->data(view.model()->index(0, 1), Qt::BackgroundRole);
                const QVariant plainBg = view.model()->data(view.model()->index(1, 1), Qt::BackgroundRole);
                WPH_CHECK(pendingBg.canConvert<QColor>() && qvariant_cast<QColor>(pendingBg).isValid());
                WPH_CHECK(!plainBg.canConvert<QColor>() || !qvariant_cast<QColor>(plainBg).isValid());
            }

            // 单击只选择：对第 0 行发送单击不应打开编辑器。
            QTableView* table = view.table();
            const QModelIndex pushRow = view.model()->index(0, 2);
            table->setCurrentIndex(pushRow);
            QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, table->visualRect(pushRow).center());
            WPH_CHECK(!view.isEditing());

            // F2：对"mov rbp, rsp"（第 1 行）打开编辑器。
            const QModelIndex movRow = view.model()->index(1, 2);
            table->setCurrentIndex(movRow);
            QTest::keyClick(table, Qt::Key_F2);
            auto* editor = qobject_cast<QLineEdit*>(QApplication::focusWidget());
            WPH_CHECK(editor != nullptr);
            if (editor != nullptr)
            {
                // 输入超长指令（原指令 3 字节，"push 0x11223344" 编译后是 5 字节的 68 id 编码）——
                // 应被拒绝、编辑框不关。
                editor->selectAll();
                QTest::keyClicks(editor, QStringLiteral("push 0x11223344"));
                QTest::keyClick(editor, Qt::Key_Return);
                WPH_CHECK(view.isEditing());
                auto* errorLabel = table->viewport()->findChild<QLabel*>(QStringLiteral("ksMemwbDisasmInlineError"));
                WPH_CHECK(errorLabel != nullptr && errorLabel->isVisible() && !errorLabel->text().isEmpty());

                // 改成 1 字节的 nop，应编译成功并按原指令长度（3 字节）补 NOP。
                QSignalSpy stageSpy(&view, &WorkbenchDisasmView::stageRequested);
                editor->selectAll();
                QTest::keyClicks(editor, QStringLiteral("nop"));
                QTest::keyClick(editor, Qt::Key_Return);
                WPH_CHECK_NOTE(stageSpy.count() == 1, QStringLiteral("stageRequested 次数=%1").arg(stageSpy.count()));
                if (stageSpy.count() == 1)
                {
                    const QList<QVariant> args = stageSpy.takeFirst();
                    WPH_CHECK(args.at(0).toULongLong() == base + 1);
                    WPH_CHECK_NOTE(args.at(1).toByteArray() == QByteArray::fromHex("909090"),
                        QStringLiteral("payload=%1").arg(QString::fromLatin1(args.at(1).toByteArray().toHex())));
                }
                // 提交成功后编辑框已被关闭销毁（但只是排队 deleteLater，这里必须先跑一次事件
                // 循环把它真正销毁，否则下一次 findChild 可能先摸到这个僵尸对象）；
                // editor 指针也不再有效，不能继续操作它。
            }
            QCoreApplication::processEvents();

            // Enter 在"nop"（无操作数）行上应进入编辑，而不是跳转。
            const QModelIndex nopRow = view.model()->index(2, 2);
            table->setCurrentIndex(nopRow);
            QTest::keyClick(table, Qt::Key_Return);
            auto* nopEditor = qobject_cast<QLineEdit*>(QApplication::focusWidget());
            WPH_CHECK(nopEditor != nullptr);
            if (nopEditor != nullptr)
            {
                QTest::keyClick(nopEditor, Qt::Key_Escape);
            }
            QCoreApplication::processEvents();

            // 边界用例：新指令长度与原指令长度**恰好相等**（1 字节换 1 字节）必须接受——
            // 这是 "> oldBytes.size()" 与 ">= oldBytes.size()" 两种写法唯一会给出不同答案的
            // 输入，专门用来钉住"超出原指令长度"判据不多拒一个字节。
            {
                const QModelIndex pushCell = view.model()->index(0, 2);
                table->setCurrentIndex(pushCell);
                QTest::keyClick(table, Qt::Key_F2);
                auto* exactEditor = qobject_cast<QLineEdit*>(QApplication::focusWidget());
                WPH_CHECK(exactEditor != nullptr);
                if (exactEditor != nullptr)
                {
                    QSignalSpy exactSpy(&view, &WorkbenchDisasmView::stageRequested);
                    exactEditor->selectAll();
                    QTest::keyClicks(exactEditor, QStringLiteral("nop"));
                    QTest::keyClick(exactEditor, Qt::Key_Return);
                    WPH_CHECK_NOTE(exactSpy.count() == 1, QStringLiteral("等长替换应被接受，实得 stageRequested=%1").arg(exactSpy.count()));
                    if (exactSpy.count() == 1)
                    {
                        WPH_CHECK(exactSpy.takeFirst().at(1).toByteArray() == QByteArray::fromHex("90"));
                    }
                }
                QCoreApplication::processEvents();
            }

            // Enter 在"jmp 0x...."（单个绝对地址操作数）行上应跟随跳转，不进入编辑。
            const std::uint64_t jmpAddress = base + 1 + 3 + 1; // push(1)+mov(3)+nop(1) 之后
            const std::uint64_t expectedTarget = jmpAddress + 5; // jmp 本身 5 字节，rel32=0
            const QModelIndex jmpRow = view.model()->index(3, 0);
            table->setCurrentIndex(jmpRow);
            QTest::keyClick(table, Qt::Key_Return);
            WPH_CHECK(!view.isEditing());
            WPH_CHECK_NOTE(view.anchorAddress() == expectedTarget,
                QStringLiteral("跳转后锚点=0x%1，期望 0x%2").arg(view.anchorAddress(), 0, 16).arg(expectedTarget, 0, 16));

            // Backspace 返回：应回到跳转前的原窗口起点。
            QTest::keyClick(table, Qt::Key_Backspace);
            WPH_CHECK_NOTE(view.anchorAddress() == base, QStringLiteral("返回后锚点=0x%1").arg(view.anchorAddress(), 0, 16));

            // 窗口外提示行：只把前 2 字节标为有效，之后解码应出现"超出已读取窗口"行。
            FakeBytesProvider partial;
            const QByteArray partialMask = QByteArray(2, '\x01') + QByteArray(bytes.size() - 2, '\x00');
            partial.overlay().LoadBaseline(QStringLiteral("partial").toStdString(), base, byteArrayToVector(bytes), byteArrayToVector(partialMask));
            WorkbenchDisasmView partialView;
            partialView.setBytesProvider(&partial);
            partialView.setDecodeBackend(MakeRealZydisDecodeBackend());
            WPH_CHECK(partialView.jumpTo(base));
            bool sawEndNote = false;
            for (int row = 0; row < partialView.model()->rowCount(); ++row)
            {
                if (partialView.model()->isEndOfWindowRow(row))
                {
                    sawEndNote = true;
                }
            }
            WPH_CHECK(sawEndNote);
        }

        // takeShots：深/浅 × 窄/宽四张截图，人工核对布局与着色。
        void takeShots(const QString& shotsDir)
        {
            if (shotsDir.isEmpty())
            {
                return;
            }
            FakeBytesProvider provider;
            const QByteArray bytes = QByteArray::fromHex("554889E54883EC10C39090");
            const std::uint64_t base = 0x140001000ULL;
            provider.overlay().LoadBaseline(QStringLiteral("shot").toStdString(), base, byteArrayToVector(bytes), byteArrayToVector(QByteArray(bytes.size(), '\x01')));
            // 暂存一处补丁，用来在截图里确认橙色底色真的画出来了。
            provider.overlay().Stage(base, {0x5D});

            WorkbenchDisasmView view;
            view.setBytesProvider(&provider);
            view.setDecodeBackend(MakeRealZydisDecodeBackend());
            view.setAssembleBackend(MakeRealAssembleBackend());
            view.setAddressBits(64);
            view.jumpTo(base);

            const QList<QPair<bool, QSize>> combos{{false, QSize(840, 360)}, {false, QSize(420, 360)}, {true, QSize(840, 360)}, {true, QSize(420, 360)}};
            for (const auto& combo : combos)
            {
                ApplyTheme(combo.first);
                view.resize(combo.second);
                view.show();
                static_cast<void>(QTest::qWaitForWindowExposed(&view));
                QCoreApplication::processEvents();
                const QString name = QStringLiteral("%1/wpH_disasm_%2_%3.png")
                    .arg(shotsDir, combo.first ? QStringLiteral("dark") : QStringLiteral("light"),
                        combo.second.width() > 600 ? QStringLiteral("wide") : QStringLiteral("narrow"));
                GrabWidget(&view).save(name);
            }
            ApplyTheme(false);
        }
    }

    void RunDisasmTests(const QString& shotsDir)
    {
        const int before = g_checks;
        const int beforeFail = g_failures;
        runResyncAlgorithmTests();
        runRealZydisTests();
        runViewInteractionTests();
        takeShots(shotsDir);
        std::cerr << "[Disasm] checks=" << (g_checks - before) << " failures=" << (g_failures - beforeFail) << std::endl;
    }
}
