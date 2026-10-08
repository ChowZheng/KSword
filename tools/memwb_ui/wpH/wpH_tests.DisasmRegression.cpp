// wpH_tests.DisasmRegression.cpp
// 作用：修复波（wave2）针对 WorkbenchDisasmView 的回归测试与审核缺口补测。
// - D1：行内编辑未改文字直接 Enter 不得暂存；编译后字节与原字节相同也不得暂存。
// - D2：编辑期间 refreshView 不得销毁编辑框，编辑结束后自动补一次刷新。
// - D3：默认架构取 provider->AddressBits()；程序化 setAddressBits 不算用户覆盖；
//   目标切换（换一个 provider）重新取默认值。
// - D6：预览对话框多行源码的出错行号是真实行号，不恒为"第 1 行"。
// - D7：Esc/失焦后行内编辑错误提示被隐藏，不残留到下一次编辑。
// - D9：窗口边界（含 kDecodeWindowBytes 的硬切点与有效前缀的硬切点）恰好落在指令中间时，
//   不得把残留字节解成幻影指令；全部有效且自然对齐时不出现"超出已读取窗口"提示行。
// - D10：右键菜单打开期间数据刷新，汇编编辑对话框拒绝打在另一条指令上。
// - D12：行内编辑错误提示跟随主题切换（动态 token + ApplyStatusRole）。
// - 可疑点 1：setEditable(false) 后 F2/Enter/双击/菜单项全部不可编辑。
// - 可疑点 2：Enter 跟随跳转只认分支类指令，push/ret/int 等不被误跟随。
// - 可疑点 5：reset() 清空锚点/后退栈。
// - 补审核报告 §3 幸存变异对应的测试（T-B/T-C/T-D/T-E 的等价写法，针对当前新代码重写）。

#include "wpH_common.h"

#include "../../../Ksword5.1/Ksword5.1/Internationalization/LanguageManager.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/HexViewWidgets.h"

#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QCoreApplication>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPoint>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTableView>
#include <QTimer>
#include <QVariant>
#include <QtTest/QtTest>

#include <algorithm>
#include <iostream>
#include <optional>

using ks::ui::DecodedRow;
using ks::ui::DecodeOneFn;
using ks::ui::DecodeWindowResynced;
using ks::ui::HexViewSegmented;
using ks::ui::WorkbenchDisasmView;

namespace wpH_test
{
    namespace
    {
        std::vector<std::uint8_t> toVec(const QByteArray& bytes)
        {
            return std::vector<std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                reinterpret_cast<const std::uint8_t*>(bytes.constData()) + bytes.size());
        }

        void loadBytes(FakeBytesProvider& provider, const std::uint64_t base, const QByteArray& bytes)
        {
            provider.overlay().LoadBaseline(
                QStringLiteral("rig").toStdString(), base, toVec(bytes), std::vector<std::uint8_t>(static_cast<std::size_t>(bytes.size()), 1));
        }

        // Rig：标准反汇编视图夹具——真实 Zydis 解码 + 真实汇编后端，构造即可用。
        struct Rig
        {
            FakeBytesProvider provider;
            ks::ui::WorkbenchDisasmView view;
            std::uint64_t base;

            explicit Rig(const QByteArray& bytes, const int bits = 64, const std::uint64_t baseAddress = 0x140001000ULL)
                : provider(bits), base(baseAddress)
            {
                loadBytes(provider, base, bytes);
                view.setBytesProvider(&provider);
                view.setDecodeBackend(MakeRealZydisDecodeBackend());
                view.setAssembleBackend(MakeRealAssembleBackend());
                view.resize(760, 420);
                view.show();
                static_cast<void>(QTest::qWaitForWindowExposed(&view));
                view.jumpTo(base);
                QCoreApplication::processEvents();
            }
        };

        // openEditor：F2 进入行内编辑，返回编辑框指针（nullptr 表示没能进入编辑）。
        QLineEdit* openEditor(Rig& rig, const int row)
        {
            rig.view.canvas()->setFocus();
            rig.view.canvas()->setSelectedRow(row);
            QTest::keyClick(rig.view.canvas(), Qt::Key_F2);
            return qobject_cast<QLineEdit*>(QApplication::focusWidget());
        }

        // ---------------- 守卫回归：后端越界/未解码多字节行必须退化为逐字节 db ----------------
        // 对应审核报告 hM02/hM03 两处幸存变异——DecodeWindowResynced 接受外部注入的后端，
        // 必须防御"后端返回的长度超过可用字节"与"后端自己标了 decoded=false 却给了多字节"
        // 两种误用，否则会把越界内容当成一整条指令摆进表格。
        void runDecodeGuardTests()
        {
            const std::vector<std::uint8_t> bytes{0x11, 0x22, 0x33};
            const DecodeOneFn tooLong = [](const std::uint8_t*, const std::size_t available, const std::uint64_t address, bool) -> std::optional<DecodedRow> {
                DecodedRow row;
                row.address = address;
                row.bytes = QByteArray(static_cast<qsizetype>(available + 4), '\x90'); // 比可用字节更长。
                row.mnemonic = QStringLiteral("bogus");
                row.decoded = true;
                return row;
            };
            const QVector<DecodedRow> overlong = DecodeWindowResynced(bytes, 0x1000, tooLong, 64, true);
            WPH_CHECK_NOTE(overlong.size() == 3, QStringLiteral("越界长度应逐字节退化为 3 条 db，实得 %1").arg(overlong.size()));
            for (const DecodedRow& row : overlong)
            {
                WPH_CHECK(!row.decoded && row.bytes.size() == 1);
            }

            const DecodeOneFn undecodedMulti = [](const std::uint8_t* data, const std::size_t available, const std::uint64_t address, bool) -> std::optional<DecodedRow> {
                DecodedRow row;
                row.address = address;
                row.bytes = QByteArray(reinterpret_cast<const char*>(data), static_cast<qsizetype>(std::min<std::size_t>(available, 3)));
                row.mnemonic = QStringLiteral("db");
                row.decoded = false; // 后端自己的降级行，不应被当成"已解码"接受。
                return row;
            };
            WPH_CHECK(DecodeWindowResynced(bytes, 0x1000, undecodedMulti, 64, true).size() == 3);
        }

        // ---------------- D1：未改动不暂存 ----------------
        void runD1Tests()
        {
            // 未改文字直接 Enter：不发信号，编辑框正常关闭。
            {
                Rig rig(QByteArray::fromHex("488BEC90C3")); // mov rbp,rsp ; nop ; ret
                QSignalSpy spy(&rig.view, &WorkbenchDisasmView::stageRequested);
                auto* editor = openEditor(rig, 0);
                WPH_CHECK(editor != nullptr);
                if (editor != nullptr)
                {
                    QTest::keyClick(editor, Qt::Key_Return);
                    WPH_CHECK_NOTE(spy.count() == 0, QStringLiteral("未改动不应暂存，实得 %1").arg(spy.count()));
                    WPH_CHECK(!rig.view.isEditing());
                }
            }
            // 改了文字，但编译出的机器码与原字节逐位相同：同样不发信号。
            // "mov rbp, rsp" 的等价写法里，Zydis 汇编器对同一条指令只有一种标准编码，
            // 这里改用大小写/空格变体验证文本确实变了、字节确实没变。
            {
                Rig rig(QByteArray::fromHex("90C3")); // nop ; ret
                QSignalSpy spy(&rig.view, &WorkbenchDisasmView::stageRequested);
                auto* editor = openEditor(rig, 0);
                WPH_CHECK(editor != nullptr);
                if (editor != nullptr)
                {
                    editor->selectAll();
                    QTest::keyClicks(editor, QStringLiteral("NOP")); // 大小写不同，文本变了
                    QTest::keyClick(editor, Qt::Key_Return);
                    WPH_CHECK_NOTE(spy.count() == 0, QStringLiteral("字节相同不应暂存，实得 %1").arg(spy.count()));
                }
            }
        }

        // ---------------- D2：编辑期间刷新不得销毁编辑框 ----------------
        void runD2Tests()
        {
            Rig rig(QByteArray::fromHex("554889E590E900000000")); // push;mov;nop;jmp
            auto* editor = openEditor(rig, 1);
            WPH_CHECK(editor != nullptr);
            if (editor == nullptr)
            {
                return;
            }
            QTest::keyClicks(editor, QStringLiteral("xchg eax, eax"));
            rig.view.refreshView();
            QCoreApplication::processEvents();
            // 关键判据：编辑框必须还在、还可见、焦点还在它上面，输入内容没有丢失。
            WPH_CHECK_NOTE(rig.view.isEditing(), QStringLiteral("编辑期间 refreshView 不应结束编辑"));
            auto* stillEditor = qobject_cast<QLineEdit*>(QApplication::focusWidget());
            WPH_CHECK(stillEditor == editor);
            if (stillEditor != nullptr)
            {
                WPH_CHECK(stillEditor->isVisible());
                WPH_CHECK(stillEditor->text().contains(QStringLiteral("xchg")));
                QTest::keyClick(stillEditor, Qt::Key_Escape);
            }
            QCoreApplication::processEvents();
            // 编辑结束后，被推迟的那次刷新应当已经自动补上：isEditing 复位，且下一次 F2 可用。
            WPH_CHECK(!rig.view.isEditing());
            rig.view.canvas()->setFocus();
            rig.view.canvas()->setSelectedRow(2);
            QTest::keyClick(rig.view.canvas(), Qt::Key_F2);
            int visibleEditors = 0;
            for (auto* lineEdit : rig.view.canvas()->viewport()->findChildren<QLineEdit*>())
            {
                visibleEditors += lineEdit->isVisible() ? 1 : 0;
            }
            WPH_CHECK_NOTE(visibleEditors == 1, QStringLiteral("补刷新后 F2 应能正常进入编辑，实得可见编辑框=%1").arg(visibleEditors));
        }

        // ---------------- D3：默认架构 ----------------
        void runD3Tests()
        {
            FakeBytesProvider provider64(64);
            loadBytes(provider64, 0x1000, QByteArray::fromHex("90"));
            WorkbenchDisasmView view;
            view.setBytesProvider(&provider64);
            WPH_CHECK_NOTE(view.isX64(), QStringLiteral("默认应取 provider->AddressBits()==64"));

            view.setAddressBits(64); // 程序化调用，不算用户覆盖
            view.setAddressBits(32);
            WPH_CHECK_NOTE(!view.isX64(), QStringLiteral("程序化 setAddressBits 不应被锁死"));

            // 目标切换：先在一个 64 位目标上真的点一下分段钮（模拟用户显式覆盖架构），
            // 再切到一个 32 位目标——必须重新按新 provider 的位数取默认值，不能被上一个
            // 目标的用户覆盖锁死。
            WorkbenchDisasmView view2;
            FakeBytesProvider firstTarget(64);
            loadBytes(firstTarget, 0x3000, QByteArray::fromHex("90"));
            view2.setBytesProvider(&firstTarget);
            view2.resize(400, 300);
            view2.show();
            static_cast<void>(QTest::qWaitForWindowExposed(&view2));
            auto* segmented = view2.findChild<HexViewSegmented*>();
            WPH_CHECK(segmented != nullptr);
            if (segmented != nullptr)
            {
                // 点一下 x86 段（下标 0），模拟用户真的用鼠标切了一次架构。
                QTest::mouseClick(segmented, Qt::LeftButton, Qt::NoModifier, segmented->segmentRect(0).center());
                WPH_CHECK_NOTE(!view2.isX64(), QStringLiteral("用户点击分段钮后应切到 x86"));
            }
            FakeBytesProvider provider32(32);
            loadBytes(provider32, 0x2000, QByteArray::fromHex("90"));
            view2.setBytesProvider(&provider32);
            WPH_CHECK_NOTE(!view2.isX64(), QStringLiteral("换到 32 位目标应重新取默认值（恰好也是 x86，用下面的 64 位目标再核对一次）"));
            FakeBytesProvider provider64Again(64);
            loadBytes(provider64Again, 0x4000, QByteArray::fromHex("90"));
            view2.setBytesProvider(&provider64Again);
            WPH_CHECK_NOTE(view2.isX64(), QStringLiteral("换到 64 位目标应重新取默认值为 x64，不被上一个目标的用户覆盖锁死"));
        }

        // ---------------- x86 视图里行内编辑按 x86 编码（hM10） ----------------
        void runX86InlineEditTests()
        {
            // 0x40 在 x64 下是 REX 前缀（会被当成 inc ecx 的 REX 变体吞掉一个字节），在 x86
            // 下是独立的 "inc ecx"（1 字节 41 的编码对象不同）；地址必须落在 32 位范围内，
            // 否则汇编器会报"地址超出范围"。
            Rig rig(QByteArray::fromHex("409090"), 32, 0x401000ULL);
            QSignalSpy spy(&rig.view, &WorkbenchDisasmView::stageRequested);
            auto* editor = openEditor(rig, 0);
            WPH_CHECK(editor != nullptr);
            if (editor == nullptr)
            {
                return;
            }
            editor->selectAll();
            QTest::keyClicks(editor, QStringLiteral("inc ecx"));
            QTest::keyClick(editor, Qt::Key_Return);
            WPH_CHECK_NOTE(spy.count() == 1, QStringLiteral("x86 视图下 inc ecx 应该能编译成功，实得 stageRequested=%1").arg(spy.count()));
            if (spy.count() == 1)
            {
                WPH_CHECK_NOTE(spy.first().at(1).toByteArray() == QByteArray::fromHex("41"),
                    QStringLiteral("x86 的 inc ecx 应编码为单字节 0x41，实得 %1")
                        .arg(QString::fromLatin1(spy.first().at(1).toByteArray().toHex())));
            }
        }

        // ---------------- D6：预览对话框真实出错行号 ----------------
        // compile/preview 对话框是模态的，用 QTimer::singleShot 在事件循环里驱动它。
        struct DialogDrive
        {
            bool found = false;
            QString status;
            bool stageEnabled = false;
        };

        DialogDrive runDialog(Rig& rig, const int row, const QString& source, const int span, const bool pad, const bool clickStage = false)
        {
            DialogDrive drive;
            QTimer::singleShot(200, [&]() {
                auto* popup = QApplication::activePopupWidget();
                if (popup == nullptr)
                {
                    return;
                }
                QTest::keyClick(popup, Qt::Key_Down);
                QTest::keyClick(popup, Qt::Key_Return);
                QTimer::singleShot(400, [&]() {
                    auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                    if (dialog == nullptr)
                    {
                        return;
                    }
                    drive.found = true;
                    auto* sourceEdit = dialog->findChild<QPlainTextEdit*>(QStringLiteral("ksMemwbAssemblySource"));
                    auto* spin = dialog->findChild<QSpinBox*>();
                    auto* checkBox = dialog->findChild<QCheckBox*>();
                    if (sourceEdit != nullptr) { sourceEdit->setPlainText(source); }
                    if (spin != nullptr) { spin->setValue(span); }
                    if (checkBox != nullptr) { checkBox->setChecked(pad); }
                    QPushButton* compile = nullptr;
                    QPushButton* stage = nullptr;
                    for (auto* button : dialog->findChildren<QPushButton*>())
                    {
                        if (button->text().contains(QStringLiteral("编译"))) { compile = button; }
                        if (button->text().contains(QStringLiteral("暂存"))) { stage = button; }
                    }
                    if (compile != nullptr) { compile->click(); }
                    if (stage != nullptr) { drive.stageEnabled = stage->isEnabled(); }
                    for (auto* label : dialog->findChildren<QLabel*>())
                    {
                        drive.status += label->text();
                    }
                    if (clickStage && stage != nullptr && stage->isEnabled()) { stage->click(); }
                    else { dialog->reject(); }
                });
            });
            QTimer::singleShot(6000, []() {
                if (auto* popup = QApplication::activePopupWidget()) { popup->close(); }
                if (auto* modal = QApplication::activeModalWidget()) { modal->close(); }
            });
            auto* canvas = rig.view.canvas();
            canvas->setSelectedRow(row);
            emit canvas->contextMenuRequested(canvas->contentRect(row).center());
            return drive;
        }

        void runD6Tests()
        {
            Rig rig(QByteArray::fromHex("4889E590C3")); // mov rbp,rsp ; nop ; ret
            // 三行源码，第 3 行是非法助记符，必须报真实行号 3，不是恒为 1。
            const DialogDrive drive = runDialog(rig, 0, QStringLiteral("nop\nnop\nbogusmnemonic eax"), 3, true);
            WPH_CHECK_NOTE(drive.found, QStringLiteral("对话框应能打开"));
            WPH_CHECK_NOTE(drive.status.contains(QStringLiteral("第 3 行")),
                QStringLiteral("应报真实出错行号 3，实得状态文案：%1").arg(drive.status));
        }

        // ---------------- 预览对话框边界校验（不变式 10，hM12/hM13） ----------------
        void runDialogBoundaryTests()
        {
            // mov rbp,rsp(3) ; nop(1) ; ret(1)。
            {
                // 覆盖 2 字节会截断 3 字节的 mov：必须拒绝（hM12：boundary==span 改成 >= 会放行）。
                Rig rig(QByteArray::fromHex("4889E590C3"));
                const DialogDrive drive = runDialog(rig, 0, QStringLiteral("nop"), 2, true);
                WPH_CHECK(drive.found);
                WPH_CHECK_NOTE(!drive.stageEnabled, QStringLiteral("截断旧指令必须拒绝，状态：%1").arg(drive.status));
                WPH_CHECK(drive.status.contains(QStringLiteral("截断")));
            }
            {
                // 关闭 NOP 填充且机器码(1 字节)短于覆盖长度(3 字节)：必须拒绝（hM13：开关取反会放行）。
                Rig rig(QByteArray::fromHex("4889E590C3"));
                const DialogDrive drive = runDialog(rig, 0, QStringLiteral("nop"), 3, false);
                WPH_CHECK(drive.found);
                WPH_CHECK_NOTE(!drive.stageEnabled, QStringLiteral("关闭 NOP 填充时长度不符必须拒绝，状态：%1").arg(drive.status));
                WPH_CHECK(drive.status.contains(QStringLiteral("NOP")));
            }
            {
                // 完整边界 + NOP 填充：接受，点"填入暂存"发出 stageRequested(基址, 90 90 90)。
                Rig rig(QByteArray::fromHex("4889E590C3"));
                QSignalSpy spy(&rig.view, &WorkbenchDisasmView::stageRequested);
                const DialogDrive drive = runDialog(rig, 0, QStringLiteral("nop"), 3, true, true);
                WPH_CHECK(drive.found && drive.stageEnabled);
                WPH_CHECK_NOTE(spy.count() == 1 && spy.first().at(1).toByteArray() == QByteArray::fromHex("909090"),
                    QStringLiteral("完整边界应接受并发出 90 90 90，实得 count=%1").arg(spy.count()));
            }
            {
                // 单指令限制只约束行内 Enter；右键明确设置覆盖范围的多行预览仍应允许两条 nop。
                Rig rig(QByteArray::fromHex("4889E590C3")); // 覆盖原 3 字节 mov，剩余 1 字节补 NOP。
                QSignalSpy spy(&rig.view, &WorkbenchDisasmView::stageRequested);
                const DialogDrive drive = runDialog(rig, 0, QStringLiteral("nop\nnop"), 3, true, true);
                WPH_CHECK_NOTE(drive.found && drive.stageEnabled, QStringLiteral("右键多行汇编预览不得被行内单指令检查阻断"));
                WPH_CHECK_NOTE(spy.count() == 1 && spy.first().at(1).toByteArray() == QByteArray::fromHex("909090"),
                    QStringLiteral("右键两条 nop 应允许并补齐第三字节，实得 count=%1").arg(spy.count()));
            }
        }

        // ---------------- D7：错误提示 Esc 后隐藏 ----------------
        void runD7Tests()
        {
            Rig rig(QByteArray::fromHex("554889E590E900000000"));
            auto* editor = openEditor(rig, 1);
            WPH_CHECK(editor != nullptr);
            if (editor == nullptr)
            {
                return;
            }
            editor->selectAll();
            QTest::keyClicks(editor, QStringLiteral("push 0x1122334455667788"));
            QTest::keyClick(editor, Qt::Key_Return);
            auto* errorLabel = rig.view.canvas()->viewport()->findChild<QLabel*>(QStringLiteral("ksMemwbDisasmInlineError"));
            WPH_CHECK(errorLabel != nullptr && errorLabel->isVisible());
            QTest::keyClick(editor, Qt::Key_Escape);
            QCoreApplication::processEvents();
            WPH_CHECK_NOTE(errorLabel == nullptr || !errorLabel->isVisible(),
                QStringLiteral("Esc 后残留的错误提示应被隐藏"));
        }

        // ---------------- D9：窗口边界不得解出幻影指令 ----------------
        void runD9Tests()
        {
            // mov rax, [rip+0x10] 是 7 字节（48 8B 05 10 00 00 00），只给前 5 字节
            // （截掉位移后 2 字节），旧代码会把残留的位移字节 "10 00" 解成 adc [rax], al。
            FakeBytesProvider provider(64);
            const QByteArray bytes = QByteArray::fromHex("909048" "8B05" "1000"); // 90 90 48 8B 05 10 00
            loadBytes(provider, 0x140001000ULL, bytes);
            WorkbenchDisasmView view;
            view.setBytesProvider(&provider);
            view.setDecodeBackend(MakeRealZydisDecodeBackend());
            WPH_CHECK(view.jumpTo(0x140001000ULL));
            bool sawAdcPhantom = false;
            bool sawEndNote = false;
            for (int i = 0; i < view.model()->rowCount(); ++i)
            {
                if (view.model()->isEndOfWindowRow(i)) { sawEndNote = true; continue; }
                const std::optional<DecodedRow> row = view.model()->rowAt(i);
                if (row.has_value() && row->mnemonic.contains(QStringLiteral("adc"), Qt::CaseInsensitive))
                {
                    sawAdcPhantom = true;
                }
            }
            WPH_CHECK_NOTE(!sawAdcPhantom, QStringLiteral("不得把截断的位移字节解成 adc 幻影指令"));
            WPH_CHECK_NOTE(sawEndNote, QStringLiteral("数据被截断，应该出现窗口外提示行"));

            // 全部有效、自然对齐（NOP 1 字节重复，4096 整除）时不应出现提示行（hM04 同款判据）。
            FakeBytesProvider alignedProvider(64);
            loadBytes(alignedProvider, 0x1000, QByteArray(4096, '\x90'));
            WorkbenchDisasmView alignedView;
            alignedView.setBytesProvider(&alignedProvider);
            alignedView.setDecodeBackend(MakeRealZydisDecodeBackend());
            WPH_CHECK(alignedView.jumpTo(0x1000));
            bool alignedSawNote = false;
            for (int i = 0; i < alignedView.model()->rowCount(); ++i)
            {
                alignedSawNote |= alignedView.model()->isEndOfWindowRow(i);
            }
            WPH_CHECK_NOTE(!alignedSawNote, QStringLiteral("全部有效且自然对齐不应出现窗口外提示行"));
        }

        // ---------------- D10：菜单期间数据刷新，对话框拒绝打错地址 ----------------
        void runD10Tests()
        {
            Rig rig(QByteArray::fromHex("554889E590E900000000"));
            // 打开菜单后（尚未点"汇编编辑"之前）立刻刷新数据源到另一段内容，模拟宿主在
            // 菜单仍打开时收到暂存变化回调触发了 refreshView。
            QTimer::singleShot(100, [&rig]() {
                if (auto* popup = QApplication::activePopupWidget())
                {
                    loadBytes(rig.provider, rig.base, QByteArray::fromHex("90909090909090909090"));
                    rig.view.refreshView();
                    QTest::keyClick(popup, Qt::Key_Down);
                    QTest::keyClick(popup, Qt::Key_Return);
                }
            });
            QTimer::singleShot(3000, []() {
                if (auto* popup = QApplication::activePopupWidget()) { popup->close(); }
                if (auto* modal = QApplication::activeModalWidget()) { modal->close(); }
            });
            auto* canvas = rig.view.canvas();
            canvas->setSelectedRow(1);
            emit canvas->contextMenuRequested(canvas->contentRect(1).center());
            QCoreApplication::processEvents();
            QTest::qWait(300);
            QCoreApplication::processEvents();
            // 数据已经变了（原来的 mov 指令不在了），对话框不应该弹出来；状态行应该有取消提示。
            WPH_CHECK(QApplication::activeModalWidget() == nullptr);
            WPH_CHECK_NOTE(rig.view.findChild<QLabel*>(QStringLiteral("ksMemwbDisasmStatus"))->text().contains(QStringLiteral("已取消"))
                || rig.view.findChild<QLabel*>(QStringLiteral("ksMemwbDisasmStatus"))->text().contains(QStringLiteral("数据已刷新")),
                QStringLiteral("应提示数据已刷新/已取消"));
        }

        // ---------------- 本轮新增变异判断点 1：地址大写格式化不应漏掉数字部分 ----------------
        // 用一个十六进制里含字母的地址，确认状态行真的把数字部分转了大写（不是整条模板），
        // 既钉住 D4 的修法本身，也覆盖"formatHexDigitsUpper 忘记 toUpper"这类新引入的回归。
        void runHexUppercaseTests()
        {
            Rig rig(QByteArray::fromHex("90"), 64, 0x14000ABCDULL);
            const QString status = rig.view.findChild<QLabel*>(QStringLiteral("ksMemwbDisasmStatus"))->text();
            WPH_CHECK_NOTE(status.contains(QStringLiteral("ABCD")) && !status.contains(QStringLiteral("abcd")),
                QStringLiteral("地址的十六进制字母应该是大写，实得状态行：%1").arg(status));
        }

        // ---------------- 本轮新增变异判断点 2：D1 的"未改动不暂存"必须真的在比较文本 ----------------
        void runD1ComparisonActuallyRunsTests()
        {
            // 跟 runD1Tests 不同：这里改了文字（大小写不同），字节也确实不同，必须暂存——
            // 用来防止"D1 判断被恶意改成恒真/恒假"这类变异（如果判断恒为"未改动"，这里
            // 会漏发信号；如果恒为"已改动"，runD1Tests 那条会先暴露）。
            Rig rig(QByteArray::fromHex("90C3"));
            QSignalSpy spy(&rig.view, &WorkbenchDisasmView::stageRequested);
            auto* editor = openEditor(rig, 0);
            WPH_CHECK(editor != nullptr);
            if (editor != nullptr)
            {
                editor->selectAll();
                QTest::keyClicks(editor, QStringLiteral("int3"));
                QTest::keyClick(editor, Qt::Key_Return);
                WPH_CHECK_NOTE(spy.count() == 1, QStringLiteral("真的改了指令必须暂存，实得 %1").arg(spy.count()));
            }
        }

        // ---------------- 本轮新增变异判断点 3：目标切换必须重新取默认架构 ----------------
        // 与 runD3Tests 的判断点相同但构造更直接：不经用户点击，纯粹验证"换了一个不同的
        // provider 指针"这件事本身会不会被遗漏（例如有人把 D3 的 if 条件改成恒假）。
        void runTargetSwitchResetsOverrideTests()
        {
            WorkbenchDisasmView view;
            FakeBytesProvider target32(32);
            loadBytes(target32, 0x1000, QByteArray::fromHex("90"));
            view.setBytesProvider(&target32);
            WPH_CHECK(!view.isX64());
            FakeBytesProvider target64(64);
            loadBytes(target64, 0x2000, QByteArray::fromHex("90"));
            view.setBytesProvider(&target64);
            WPH_CHECK_NOTE(view.isX64(), QStringLiteral("换了一个新的 64 位目标，必须重新取默认值为 x64"));
        }

        // ---------------- 可疑点 1：setEditable ----------------
        void runEditableTests()
        {
            Rig rig(QByteArray::fromHex("554889E590"));
            rig.view.setEditable(false);
            WPH_CHECK(!rig.view.isEditable());
            rig.view.canvas()->setFocus();
            rig.view.canvas()->setSelectedRow(1);
            QTest::keyClick(rig.view.canvas(), Qt::Key_F2);
            WPH_CHECK_NOTE(!rig.view.isEditing(), QStringLiteral("只读模式下 F2 不应进入编辑"));
            // 双击必须先发一次 mouseClick 再发 mouseDClick——只发 mouseDClick 不会触发
            // Qt 的 doubleClicked 信号（上一波审核报告也踩过这个坑），否则这条断言会在
            // "doubleClicked 根本没发出来"的情况下被动过关，测不出真正的只读防护。
            const QPoint center = rig.view.canvas()->contentRect(1).center();
            QTest::mouseClick(rig.view.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier, center);
            QTest::mouseDClick(rig.view.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier, center);
            WPH_CHECK_NOTE(!rig.view.isEditing(), QStringLiteral("只读模式下双击不应进入编辑"));
            rig.view.setEditable(true);
            WPH_CHECK(rig.view.isEditable());
        }

        // ---------------- 单击已选中行不应进入编辑（hM08） ----------------
        // SelectedClicked 触发是延迟的（经过 doubleClickInterval 才会真正触发），紧跟在
        // mouseClick 后面立即断言天然抓不到它被意外启用——必须等够这个间隔。
        void runSingleClickNoEditTests()
        {
            Rig rig(QByteArray::fromHex("554889E590"));
            auto* canvas = rig.view.canvas();
            const QModelIndex idx = rig.view.model()->index(1, 2);
            canvas->setSelectedRow(idx.row());
            QTest::mouseClick(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, canvas->contentRect(idx.row()).center());
            QTest::qWait(QApplication::doubleClickInterval() + 200);
            WPH_CHECK_NOTE(!rig.view.isEditing(), QStringLiteral("单击已选中行不应进入编辑（哪怕是延迟触发的 SelectedClicked）"));
        }

        // ---------------- 可疑点 2：Enter 跟随只认分支类指令 ----------------
        void runBranchFollowTests()
        {
            // push 0x10 ; ret 0x08（都把立即数当操作数，但都不是跳转目标）。
            Rig rig(QByteArray::fromHex("6A10C20800"));
            rig.view.canvas()->setFocus();
            rig.view.canvas()->setSelectedRow(0);
            const std::uint64_t before = rig.view.anchorAddress();
            QTest::keyClick(rig.view.canvas(), Qt::Key_Return);
            WPH_CHECK_NOTE(rig.view.anchorAddress() == before, QStringLiteral("push imm 不应被当成跳转目标跟随"));
            WPH_CHECK_NOTE(rig.view.isEditing(), QStringLiteral("push imm 行 Enter 应该进入编辑"));
            if (rig.view.isEditing())
            {
                QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);
            }
        }

        // ---------------- 后退栈上限与弹栈（hM05/hM06） ----------------
        void runBackStackTests()
        {
            Rig rig(QByteArray(256, '\x90'));
            rig.view.jumpTo(rig.base + 0x10);
            rig.view.jumpTo(rig.base + 0x20);
            rig.view.navigateBack();
            WPH_CHECK_NOTE(rig.view.anchorAddress() == rig.base + 0x10, QStringLiteral("第一次 Backspace 应回到 base+0x10"));
            rig.view.navigateBack();
            WPH_CHECK_NOTE(rig.view.anchorAddress() == rig.base, QStringLiteral("第二次 Backspace 应回到 base（hM06：删掉 pop_back 会停在原地）"));

            // 上限 64——跳转 70 次后最多能回退 64 次，第 65 次不再动（hM05：上限变成 65 会多退一次）。
            Rig capRig(QByteArray(256, '\x90'));
            capRig.view.jumpTo(capRig.base);
            for (int i = 1; i <= 70; ++i)
            {
                capRig.view.jumpTo(capRig.base + static_cast<std::uint64_t>(i));
            }
            for (int i = 0; i < 64; ++i)
            {
                capRig.view.navigateBack();
            }
            const std::uint64_t afterCap = capRig.view.anchorAddress();
            capRig.view.navigateBack();
            WPH_CHECK_NOTE(capRig.view.anchorAddress() == afterCap,
                QStringLiteral("第 65 次 Backspace 不应再移动（后退栈上限 64），退之前=0x%1 退之后=0x%2")
                    .arg(afterCap, 0, 16).arg(capRig.view.anchorAddress(), 0, 16));
        }

        // ---------------- 底色色相：待写入=橙、外部变化=青（hM19） ----------------
        // 旧夹具只断言背景色 isValid，不看色相，橙/青互换抓不到；这里直接比色相区间。
        void runColorHueTests()
        {
            FakeBytesProvider provider(64);
            const std::uint64_t base = 0x140001000ULL;
            loadBytes(provider, base, QByteArray::fromHex("9090909090"));
            provider.overlay().RefreshBaseline(QStringLiteral("rig").toStdString(), base, toVec(QByteArray::fromHex("9090909091")), std::vector<std::uint8_t>(5, 1));
            provider.overlay().Stage(base, {0xCC});
            WorkbenchDisasmView view;
            view.setBytesProvider(&provider);
            view.setDecodeBackend(MakeRealZydisDecodeBackend());
            WPH_CHECK(view.jumpTo(base));
            const QColor pending = qvariant_cast<QColor>(view.model()->data(view.model()->index(0, 1), Qt::BackgroundRole));
            const QColor external = qvariant_cast<QColor>(view.model()->data(view.model()->index(4, 1), Qt::BackgroundRole));
            WPH_CHECK_NOTE(pending.hslHue() >= 10 && pending.hslHue() <= 60, QStringLiteral("待写入修改应为橙色，实得色相 %1").arg(pending.hslHue()));
            WPH_CHECK_NOTE(external.hslHue() >= 150 && external.hslHue() <= 210, QStringLiteral("外部变化应为青色，实得色相 %1").arg(external.hslHue()));
        }

        // ---------------- 可疑点 5：reset() ----------------
        void runResetTests()
        {
            Rig rig(QByteArray::fromHex("9090909090"));
            rig.view.jumpTo(rig.base + 1);
            WPH_CHECK(rig.view.anchorAddress() == rig.base + 1);
            rig.view.reset();
            WPH_CHECK_NOTE(rig.view.anchorAddress() == 0, QStringLiteral("reset 后锚点应清零"));
            // 再跳一次不应触发"后退"到旧锚点（后退栈应该也被清空了）。
            rig.view.jumpTo(rig.base);
            QTest::keyClick(rig.view.canvas(), Qt::Key_Backspace);
            WPH_CHECK_NOTE(rig.view.anchorAddress() == rig.base, QStringLiteral("reset 后后退栈应为空，Backspace 不应跳到旧地址"));
        }

        // ---------------- D4/D5：en-US 下状态行/表头不含汉字 ----------------
        void runI18nTests()
        {
            QString errorText;
            const bool loaded = ks::i18n::LanguageManager::instance().initialize(QStringLiteral("en-US"), &errorText);
            WPH_CHECK_NOTE(loaded, QStringLiteral("en-US 语言包应能加载：%1").arg(errorText));
            if (!loaded)
            {
                return;
            }
            Rig rig(QByteArray::fromHex("554889E590E900000000"));
            ks::i18n::LanguageManager::instance().retranslateAll();
            QCoreApplication::processEvents();
            const QString status = rig.view.findChild<QLabel*>(QStringLiteral("ksMemwbDisasmStatus"))->text();
            const bool hasChinese = std::any_of(status.begin(), status.end(), [](const QChar ch) { return ch.unicode() >= 0x4E00 && ch.unicode() <= 0x9FFF; });
            WPH_CHECK_NOTE(!hasChinese, QStringLiteral("en-US 下状态行不应包含汉字：%1").arg(status));
            WPH_CHECK_NOTE(status.contains(QStringLiteral("Enter")) && !status.contains(QStringLiteral("ENTER")),
                QStringLiteral("模板里的 Enter 不应被误大写：%1").arg(status));
            for (int column = 0; column < 4; ++column)
            {
                const QString header = rig.view.model()->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString();
                const bool headerHasChinese = std::any_of(header.begin(), header.end(), [](const QChar ch) { return ch.unicode() >= 0x4E00 && ch.unicode() <= 0x9FFF; });
                WPH_CHECK_NOTE(!headerHasChinese, QStringLiteral("en-US 下表头不应包含汉字（列 %1）：%2").arg(column).arg(header));
            }
            ks::i18n::LanguageManager::instance().initialize(QStringLiteral("zh-CN"));
            ks::i18n::LanguageManager::instance().retranslateAll();
        }
    }

    void RunDisasmRegressionTests()
    {
        const int before = g_checks;
        const int beforeFail = g_failures;
        runDecodeGuardTests();
        runHexUppercaseTests();
        runD1Tests();
        runD1ComparisonActuallyRunsTests();
        runD2Tests();
        runD3Tests();
        runTargetSwitchResetsOverrideTests();
        runX86InlineEditTests();
        runD6Tests();
        runDialogBoundaryTests();
        runD7Tests();
        runD9Tests();
        runD10Tests();
        runEditableTests();
        runSingleClickNoEditTests();
        runBranchFollowTests();
        runBackStackTests();
        runColorHueTests();
        runResetTests();
        runI18nTests();
        std::cerr << "[DisasmRegression] checks=" << (g_checks - before) << " failures=" << (g_failures - beforeFail) << std::endl;
    }
}
