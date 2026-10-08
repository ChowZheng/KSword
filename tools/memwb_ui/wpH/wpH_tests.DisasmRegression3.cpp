// wpH_tests.DisasmRegression3.cpp
// 作用：第二轮独立审核（review2-wpH.md）针对 WorkbenchDisasmView 的补测，第 2 部分
// （与 DisasmRegression2.cpp 同源，单文件拆成两份保持 ≤700 行）。
// 并入自审核者补测 extra2.cpp 的 T14/T15/T16/T18/T19/T20/T21/T22/T23/T24。
// - T14：setEditable(false) 必须真的关掉打开着的编辑框（杀 rC19）。
// - T15：N3 修复——关掉编辑框之后 isEditing() 必须复位，且后续 jumpTo 不再被"编辑中
//   延后"吞掉（修复前：isEditing 永久卡 true，表格冻住）。
// - T16：db 占位行的操作数格式——固定 2 位十六进制、带 0x 前缀（杀 rC21）。
// - T18：底色优先级 Pending > SelfWritten > ExternalChange，同一行同时覆盖 SelfWritten 与
//   ExternalChange 字节时必须显示 SelfWritten 的绿色（杀 rC24）。
// - T19：refreshView 之后按地址（而不是行号）恢复选中行（杀 rC01）。
// - T20：行内编辑错误提示框的背景色必须跟随主题切换后"重下发全局样式块"的真实流程
//   （杀 rA20，像素级断言）。
// - T21/T22：N2 修复——编辑期间到达的刷新被推迟到编辑结束后才真正应用，应用之后键盘
//   焦点必须还在表格上，不能因为"我们先于视图自己的 closeEditor 处理跑了一次模型重建"
//   而丢失（T21 走 Esc，T22 模拟宿主在 stageRequested 槛里同步 refreshView）。
// - T23：被推迟的刷新必须真的在编辑结束后应用——编辑期间数据变了，编辑中表格不应该重建，
//   Esc 之后必须看到新内容（杀 rC25）。
// - T24：N4 修复——编辑期间换了目标（setBytesProvider），打开的编辑框必须被取消，不得把
//   旧目标的编辑结果当成对新目标的写入请求发出去。
// 以下是本轮（wave2 fix2）自己补的 8 个新变异判断点之一部分（M1/M2/M3/M6/M8，其余
// M4/M5/M7 在 TextRegression2.cpp / CompareRegression2.cpp），均不同于第一、二轮已有变异：
// - M1：分支白名单里的 loop 族（loop/loope/loopne/loopz/loopnz）此前两轮都没有任何用例
//   （第二轮报告明确指出"白名单每个条目都无用例"，但第二轮自己补的 T05/T06 也只覆盖了
//   call 与 jnb 族四项）。
// - M2：分支白名单里的 jo/jno/js/jns（溢出/符号标志位跳转）同样没有用例。
// - M3：cancelInlineEdit() 必须真的隐藏行内编辑错误提示，不能只关编辑框、复位标志而留着
//   一条过期的错误提示框。
// - M6：reset() 必须像 setBytesProvider 一样取消正在进行的行内编辑（N4 的后半句，T24 只
//   测了 setBytesProvider 那一半）。
// - M8：右键菜单的 QPointer 自guard（可疑点 3 的修复）本身的"正常路径"没有用例——必须
//   证明 this 存活时菜单操作（复制地址等）仍然正常工作，不是加了 guard 之后反而总是提前
//   返回什么都不做。
// - 行内汇编复核：未补 NOP 的机器码必须恰好解出一条完整指令；拒绝时保留编辑框，
//   显示错误且不暂存。真实解码器同时覆盖多指令、残片及 x86/x64 入参，防止旧长度检查回归。

#include "wpH_common.h"

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/HexCanvasFormat.h"
#include "../../../Ksword5.1/Ksword5.1/theme.h"

#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QColor>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPointer>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableView>
#include <QTimer>
#include <QtTest/QtTest>

#include <cstdlib>
#include <iostream>

using ks::ui::WorkbenchDisasmView;
namespace hexcanvas_format = ks::ui::hexcanvas_format;

namespace wpH_test
{
    namespace
    {
        std::vector<std::uint8_t> toVec3(const QByteArray& bytes)
        {
            return std::vector<std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                reinterpret_cast<const std::uint8_t*>(bytes.constData()) + bytes.size());
        }

        void loadBytes3(FakeBytesProvider& provider, const std::uint64_t base, const QByteArray& bytes)
        {
            provider.overlay().LoadBaseline(
                QStringLiteral("rig3").toStdString(), base, toVec3(bytes), std::vector<std::uint8_t>(static_cast<std::size_t>(bytes.size()), 1));
        }

        struct Rig3
        {
            FakeBytesProvider provider;
            ks::ui::WorkbenchDisasmView view;
            std::uint64_t base;

            explicit Rig3(const QByteArray& bytes, const int bits = 64, const std::uint64_t baseAddress = 0x140001000ULL)
                : provider(bits), base(baseAddress)
            {
                loadBytes3(provider, base, bytes);
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

        // openEditor3：F2 进入行内编辑，返回编辑框指针（nullptr 表示没能进入编辑）。
        QLineEdit* openEditor3(Rig3& rig, const int row)
        {
            rig.view.canvas()->setFocus();
            rig.view.canvas()->setSelectedRow(row);
            QTest::keyClick(rig.view.canvas(), Qt::Key_F2);
            return qobject_cast<QLineEdit*>(QApplication::focusWidget());
        }

        // runInlineCompleteInstructionRejectTests：注入成功汇编结果，逐项检查解码复核的拒绝契约。
        // 无入参/返回值；通过 F2/Enter 操作真实委托，旧的“只比长度后补 NOP”会错误暂存这些结果。
        void runInlineCompleteInstructionRejectTests()
        {
            // RejectedCase：每项冻结后端异常形状；多指令、尾部残片和非法字节仍走真实 Zydis。
            struct RejectedCase
            {
                const char* hex; // 成功汇编后返回的未补齐机器码
                int decodeShape; // 0=真实解码，1=失败，2=占位，3=空指令，4=缺后端，5=越界长度，6=字节不符
                const char* name; // 失败断言中的场景说明
            };
            const RejectedCase cases[] = {
                {"", 0, "empty assembly"},
                {"90", 1, "decode failure"},
                {"90", 2, "placeholder"},
                {"90", 3, "empty decoded instruction"},
                {"9090", 0, "two complete instructions"},
                {"900F", 0, "instruction and incomplete tail"},
                {"06", 0, "invalid x64 instruction"},
                {"90", 4, "missing decoder"},
                {"90", 5, "decoder exceeds available bytes"},
                {"90", 6, "decoder returns different bytes of equal length"}
            };
            for (const RejectedCase& testCase : cases)
            {
                Rig3 rig(QByteArray::fromHex("554889E590C3")); // 第 1 行旧 mov 占 3 字节，所有结果均未超长。
                const QByteArray bytes = QByteArray::fromHex(testCase.hex); // 模拟汇编成功返回的原始机器码。
                rig.view.setAssembleBackend([bytes](const QString&, std::uint64_t, bool) {
                    ks::ui::WorkbenchAssembleResult result; // 后端显式成功，不能依赖汇编失败分支挡住用例。
                    result.success = true;
                    result.bytes = bytes;
                    return result;
                });
                QSignalSpy spy(&rig.view, &WorkbenchDisasmView::stageRequested); // 记录是否错误地提交了补丁。
                const QPointer<QLineEdit> editor(openEditor3(rig, 1)); // 旧实现关闭编辑框时安全检查其生命期。
                WPH_CHECK(editor != nullptr);
                if (editor == nullptr)
                {
                    continue;
                }

                // 先打开真实 mov 的编辑框，再替换解码后端；刷新被延后，仍保留冻结的旧指令上下文。
                if (testCase.decodeShape == 4)
                {
                    rig.view.setDecodeBackend(ks::ui::DecodeOneFn{});
                }
                else
                {
                    const auto realDecode = MakeRealZydisDecodeBackend(); // 真实识别多条指令/残片，不模拟边界规则。
                    rig.view.setDecodeBackend([realDecode, testCase](const std::uint8_t* data,
                        const std::size_t available, const std::uint64_t address, const bool x64)
                        -> std::optional<ks::ui::DecodedRow> {
                        if (testCase.decodeShape == 1)
                        {
                            return std::nullopt;
                        }
                        auto row = realDecode(data, available, address, x64); // 只改变所测试的后端异常字段。
                        if (row.has_value() && testCase.decodeShape == 2)
                        {
                            row->decoded = false;
                        }
                        if (row.has_value() && testCase.decodeShape == 3)
                        {
                            row->bytes.clear();
                        }
                        if (row.has_value() && testCase.decodeShape == 5)
                        {
                            row->bytes.append(static_cast<char>(0x90));
                        }
                        if (row.has_value() && testCase.decodeShape == 6)
                        {
                            row->bytes = QByteArray::fromHex("91"); // 只比长度会放过错误的解码结果。
                        }
                        return row;
                    });
                }

                // 不依赖错误文案的具体翻译，只要求拒绝后可见说明、编辑继续且没有 stageRequested。
                editor->selectAll();
                QTest::keyClicks(editor.data(), QStringLiteral("nop"));
                QTest::keyClick(editor.data(), Qt::Key_Return);
                const QString context = QString::fromLatin1(testCase.name); // 各形状失败时的定位信息。
                WPH_CHECK_NOTE(spy.count() == 0, context);
                WPH_CHECK_NOTE(rig.view.isEditing() && editor != nullptr && editor->isVisible(), context);
                auto* error = rig.view.canvas()->viewport()->findChild<QLabel*>(QStringLiteral("ksMemwbDisasmInlineError"));
                WPH_CHECK_NOTE(error != nullptr && error->isVisible() && !error->text().isEmpty(), context);
                if (editor != nullptr && editor->isVisible())
                {
                    QTest::keyClick(editor.data(), Qt::Key_Escape); // 每项收尾，不能把编辑状态带入下一项。
                }
            }
        }

        // runInlineCompleteInstructionAcceptTests：真实短单指令可暂存，解码必须先于 NOP 补齐。
        // 无入参/返回值；分别检查 x86/x64 的真实地址、架构和解码窗口，再核对补齐后的提交字节。
        void runInlineCompleteInstructionAcceptTests()
        {
            const int architectures[] = {32, 64}; // 两种默认架构都必须传给 DecodeOneFn。
            for (const int bits : architectures)
            {
                const std::uint64_t base = bits == 64 ? 0x140012340ULL : 0x00401230ULL; // 避免总传零地址也能过。
                const QByteArray baseline = QByteArray::fromHex(bits == 64 ? "554889E590C3" : "5589E590C3");
                bool checkingInline = false; // 区分刷新模型的解码与 Enter 提交时的复核。
                int decodeCalls = 0; // 提交必须只解码第一条，并确认它吃掉完整结果。
                QByteArray decodedInput; // 保存解码入参，验证没有提前混入填充 NOP。
                std::uint64_t decodedAddress = 0; // 保存真实指令地址。
                bool decodedX64 = bits != 64; // 初值刻意取反，漏传/漏调用会失败。
                const auto realDecode = MakeRealZydisDecodeBackend(); // 正常路径保持真实 Zydis。
                // 视图先于记录状态析构，回调不会在宿主收尾时持有已经消失的引用。
                Rig3 rig(baseline, bits, base); // 旧 mov 长度分别为 3/2，nop 长度为 1。
                rig.view.setDecodeBackend([&, realDecode](const std::uint8_t* data, const std::size_t available,
                    const std::uint64_t address, const bool x64) -> std::optional<ks::ui::DecodedRow> {
                    if (checkingInline)
                    {
                        ++decodeCalls;
                        decodedInput = QByteArray(reinterpret_cast<const char*>(data), static_cast<qsizetype>(available));
                        decodedAddress = address;
                        decodedX64 = x64;
                    }
                    return realDecode(data, available, address, x64);
                });

                QSignalSpy spy(&rig.view, &WorkbenchDisasmView::stageRequested); // 最终提交只应出现一次。
                auto* editor = openEditor3(rig, 1); // 在非零行编辑，地址须带上 push 的 1 字节偏移。
                WPH_CHECK(editor != nullptr);
                if (editor == nullptr)
                {
                    continue;
                }
                editor->selectAll();
                QTest::keyClicks(editor, QStringLiteral("nop"));
                checkingInline = true;
                QTest::keyClick(editor, Qt::Key_Return);
                checkingInline = false;

                // 复核前机器码恰好是一个 nop；提交后才按旧指令长度补齐，不能用补齐窗口判单指令。
                WPH_CHECK(decodeCalls == 1);
                WPH_CHECK(decodedInput == QByteArray::fromHex("90"));
                WPH_CHECK(decodedAddress == base + 1);
                WPH_CHECK(decodedX64 == (bits == 64));
                WPH_CHECK(!rig.view.isEditing());
                WPH_CHECK(spy.count() == 1);
                if (spy.count() == 1)
                {
                    WPH_CHECK(spy.first().at(0).toULongLong() == base + 1);
                    WPH_CHECK(spy.first().at(1).toByteArray() == QByteArray(bits == 64 ? 3 : 2, static_cast<char>(0x90)));
                }
            }
        }

        // ---------------- T14（杀 rC19）：setEditable(false) 必须关掉打开着的编辑框 ----------------
        void runSetEditableClosesEditorTests()
        {
            Rig3 rig(QByteArray::fromHex("554889E590E900000000"));
            auto* editor = openEditor3(rig, 1);
            WPH_CHECK(editor != nullptr);
            if (editor == nullptr)
            {
                return;
            }
            const QPointer<QLineEdit> guard(editor);
            rig.view.setEditable(false);
            QCoreApplication::processEvents();
            QTest::qWait(20);
            WPH_CHECK_NOTE(guard.isNull() || !guard->isVisible(), QStringLiteral("setEditable(false) 必须关闭正在打开的编辑框"));
        }

        // ---------------- T15（N3 修复）：关闭编辑框之后不应永久冻住 ----------------
        void runSetEditableDoesNotFreezeTests()
        {
            Rig3 rig(QByteArray::fromHex("554889E590E900000000"));
            auto* editor = openEditor3(rig, 1);
            WPH_CHECK(editor != nullptr);
            if (editor == nullptr)
            {
                return;
            }
            rig.view.setEditable(false);
            QCoreApplication::processEvents();
            WPH_CHECK_NOTE(!rig.view.isEditing(), QStringLiteral("setEditable(false) 关闭编辑框之后 isEditing 必须复位"));
            rig.view.jumpTo(rig.base + 1);
            const auto row0 = rig.view.model()->rowAt(0);
            WPH_CHECK_NOTE(row0.has_value() && row0->address == rig.base + 1,
                QStringLiteral("关闭编辑框之后视图必须仍能正常刷新（没有被永久冻住）"));
        }

        // ---------------- T16（杀 rC21）：db 占位行操作数格式 ----------------
        void runDbOperandFormatTests()
        {
            Rig3 rig(QByteArray::fromHex("06AB90")); // 0x06 在 x64 下非法
            const auto row0 = rig.view.model()->rowAt(0);
            WPH_CHECK_NOTE(row0.has_value() && row0->mnemonic == QStringLiteral("db") && row0->operands == QStringLiteral("0x06"),
                row0.has_value() ? row0->operands : QString());
        }

        // ---------------- T18（杀 rC24）：底色优先级 SelfWritten > ExternalChange ----------------
        void runRowKindPriorityTests()
        {
            FakeBytesProvider provider(64);
            const std::uint64_t base = 0x140001000ULL;
            loadBytes3(provider, base, QByteArray::fromHex("4889E5909090"));
            QByteArray second = QByteArray::fromHex("4889E5909090");
            second[1] = '\x8A'; // 外部把第 1 字节改了（ExternalChange）
            provider.overlay().RefreshBaseline(QStringLiteral("rig3").toStdString(), base, toVec3(second), std::vector<std::uint8_t>(6, 1));
            provider.overlay().Stage(base, {0x49}); // 第 0 字节暂存（将成为 SelfWritten）
            const auto blocks = provider.overlay().DiffBlocks();
            WPH_CHECK(!blocks.empty());
            if (!blocks.empty())
            {
                provider.overlay().AcceptWrite(blocks.front(), {0x49});
            }
            WPH_CHECK(provider.overlay().ChangeKind(base) == ksword::memwb::ByteChangeKind::SelfWritten);
            WPH_CHECK(provider.overlay().ChangeKind(base + 1) == ksword::memwb::ByteChangeKind::ExternalChange);

            WorkbenchDisasmView view;
            view.setBytesProvider(&provider);
            view.setDecodeBackend(MakeRealZydisDecodeBackend());
            WPH_CHECK(view.jumpTo(base));
            // 第 0 行（49 8A E5 …，3 字节）同时覆盖 SelfWritten（第 0 字节）与 ExternalChange
            // （第 1 字节）两种变化，必须按优先级显示 SelfWritten 的绿色，不能显示 ExternalChange
            // 的青色。
            const QColor color = qvariant_cast<QColor>(view.model()->data(view.model()->index(0, 1), Qt::BackgroundRole));
            WPH_CHECK_NOTE(color.isValid() && color.hslHue() >= 80 && color.hslHue() <= 150,
                QStringLiteral("SelfWritten（绿）必须压过 ExternalChange（青），实得色相 %1").arg(color.hslHue()));
        }

        // ---------------- T19（杀 rC01）：刷新后按地址恢复选中行 ----------------
        void runSelectionSurvivesRefreshTests()
        {
            Rig3 rig(QByteArray::fromHex("554889E590E900000000"));
            rig.view.canvas()->setSelectedRow(2);
            const auto addr = rig.view.model()->rowAt(2)->address;
            rig.view.refreshView();
            QCoreApplication::processEvents();
            const auto current = rig.view.selectedInstruction();
            const auto range = rig.view.canvas()->selectedRange();
            WPH_CHECK_NOTE(current && range && current->address == addr
                    && range->first == addr
                    && range->second == addr + static_cast<std::uint64_t>(current->bytes.size() - 1),
                QStringLiteral("刷新后应按地址恢复选中指令及其完整字节范围（valid=%1）").arg(current.has_value()));
        }

        // ---------------- T20（杀 rA20）：行内错误提示框跟随主题（真实像素） ----------------
        void runInlineErrorFollowsThemeTests()
        {
            Rig3 rig(QByteArray::fromHex("554889E590E900000000"));
            auto* editor = openEditor3(rig, 1);
            WPH_CHECK(editor != nullptr);
            if (editor == nullptr)
            {
                return;
            }
            editor->selectAll();
            QTest::keyClicks(editor, QStringLiteral("push 0x1122334455667788")); // 超长，必定编译失败并弹出错误框
            QTest::keyClick(editor, Qt::Key_Return);
            auto* label = rig.view.canvas()->viewport()->findChild<QLabel*>(QStringLiteral("ksMemwbDisasmInlineError"));
            WPH_CHECK(label != nullptr && label->isVisible());
            if (label == nullptr || !label->isVisible())
            {
                return;
            }
            ApplyTheme(true);
            // 生产里切主题会重下发全局样式块（整个应用 repolish）；夹具只改调色板不够，
            // Qt 只在 polish 时才重新解析 "palette(...)" 记号，必须模拟这一步。
            qApp->setStyleSheet(QStringLiteral("QToolTip{}"));
            QCoreApplication::processEvents();
            const QImage img = label->grab().toImage().convertToFormat(QImage::Format_ARGB32);
            const QColor pixel = img.pixelColor(2, 2);
            const QColor expected = KswordTheme::SurfaceAltColor();
            ApplyTheme(false);
            qApp->setStyleSheet(QString());
            WPH_CHECK_NOTE(std::abs(pixel.red() - expected.red()) < 6 && std::abs(pixel.green() - expected.green()) < 6
                    && std::abs(pixel.blue() - expected.blue()) < 6,
                QStringLiteral("切到深色主题后错误框底色应跟随：实得 %1 期望(SurfaceAlt 深色) %2").arg(pixel.name(), expected.name()));
        }

        // ---------------- T21（N2 修复）：Esc 结束编辑后，之前推迟的刷新不应丢失焦点 ----------------
        void runFocusAfterDeferredRefreshTests()
        {
            Rig3 rig(QByteArray::fromHex("554889E590E900000000"));
            auto* editor = openEditor3(rig, 1);
            WPH_CHECK(editor != nullptr);
            if (editor == nullptr)
            {
                return;
            }
            rig.view.refreshView(); // 编辑中到达的刷新，应被记成"待刷新"而不是立刻生效
            QTest::keyClick(editor, Qt::Key_Escape);
            QCoreApplication::processEvents();
            QTest::qWait(30); // N2 的修法把真正的刷新推迟到下一次事件循环，要等它跑完
            WPH_CHECK_NOTE(QApplication::focusWidget() == rig.view.canvas(), QStringLiteral("推迟的刷新应用之后，键盘焦点必须回到表格"));
        }

        // ---------------- T22（N2 修复）：宿主在 stageRequested 槛里同步刷新的常见写法 ----------------
        void runFocusAfterHostRefreshInStageSlotTests()
        {
            Rig3 rig(QByteArray::fromHex("554889E590E900000000"));
            QObject::connect(&rig.view, &WorkbenchDisasmView::stageRequested, &rig.view, [&rig](const quint64 addr, const QByteArray& bytes) {
                rig.provider.overlay().Stage(addr, toVec3(bytes));
                rig.view.refreshView(); // 这是最自然的宿主写法：提交一次编辑后立刻刷新。
            });
            auto* editor = openEditor3(rig, 1);
            WPH_CHECK(editor != nullptr);
            if (editor == nullptr)
            {
                return;
            }
            editor->selectAll();
            QTest::keyClicks(editor, QStringLiteral("nop"));
            QTest::keyClick(editor, Qt::Key_Return);
            QCoreApplication::processEvents();
            QTest::qWait(30);
            WPH_CHECK_NOTE(QApplication::focusWidget() == rig.view.canvas(), QStringLiteral("宿主在 stageRequested 槛里同步刷新后，焦点必须回到表格"));
        }

        // ---------------- T23（杀 rC25）：推迟的刷新必须真的在编辑结束后应用 ----------------
        void runDeferredRefreshIsAppliedTests()
        {
            Rig3 rig(QByteArray::fromHex("554889E590E900000000"));
            auto* editor = openEditor3(rig, 1);
            WPH_CHECK(editor != nullptr);
            if (editor == nullptr)
            {
                return;
            }
            loadBytes3(rig.provider, rig.base, QByteArray::fromHex("90909090909090909090")); // 编辑期间数据变了
            rig.view.refreshView();
            WPH_CHECK_NOTE(rig.view.model()->rowAt(1) && rig.view.model()->rowAt(1)->mnemonic == QStringLiteral("mov"),
                QStringLiteral("编辑期间表格不应被重建，应仍显示旧内容"));
            QTest::keyClick(editor, Qt::Key_Escape);
            QCoreApplication::processEvents();
            QTest::qWait(30);
            WPH_CHECK_NOTE(rig.view.model()->rowAt(1) && rig.view.model()->rowAt(1)->mnemonic == QStringLiteral("nop"),
                QStringLiteral("编辑结束后，被推迟的刷新必须真的应用，实得 %1")
                    .arg(rig.view.model()->rowAt(1) ? rig.view.model()->rowAt(1)->mnemonic : QString()));
        }

        // ---------------- T24（N4 修复）：换目标必须取消打开着的编辑，不得带着提交给新目标 ----------------
        void runStaleEditorAcrossTargetsTests()
        {
            Rig3 rig(QByteArray::fromHex("554889E590E900000000"));
            QSignalSpy spy(&rig.view, &WorkbenchDisasmView::stageRequested);
            auto* editor = openEditor3(rig, 1);
            WPH_CHECK(editor != nullptr);
            if (editor == nullptr)
            {
                return;
            }
            const QPointer<QLineEdit> guard(editor);
            editor->selectAll();
            QTest::keyClicks(editor, QStringLiteral("nop")); // 还没提交，只是改了文本
            FakeBytesProvider other(64);
            loadBytes3(other, rig.base, QByteArray::fromHex("CCCCCCCCCCCCCCCCCCCC")); // 新目标，同地址不同内容
            rig.view.setBytesProvider(&other);
            QCoreApplication::processEvents();
            QTest::qWait(20);
            WPH_CHECK_NOTE(guard.isNull() || !guard->isVisible(), QStringLiteral("换目标后必须关闭残留的编辑框"));
            if (!guard.isNull() && guard->isVisible())
            {
                // 兜底：如果编辑框意外还活着，再按一次 Enter，确认即使这样也不会把旧编辑
                // 提交给新目标（双重保险，不让这条断言因为上一条失败而失去意义）。
                QTest::keyClick(guard.data(), Qt::Key_Return);
            }
            WPH_CHECK_NOTE(spy.count() == 0, QStringLiteral("针对旧目标的编辑不得暂存给新目标，实得 stageRequested=%1").arg(spy.count()));
        }

        // ---------------- M1：分支白名单 loop 族，此前两轮都没有用例 ----------------
        void runLoopFamilyFollowTests()
        {
            // loop/loope(je)/loopne(jne) 都是"ecx/rcx 自减后非零才跳"，立即数窗口里全部
            // 填 0x90 时不会提前把 ecx 减到 0（FakeBytesProvider 不模拟寄存器，这里只测
            // "Enter 是否把它判成分支类并发起跟随"，不依赖寄存器真实取值）。
            const char* hexes[] = {"E205", "E105", "E005"}; // loop/loope/loopne rel8
            for (const char* h : hexes)
            {
                Rig3 rig(QByteArray::fromHex(h) + QByteArray(16, '\x90'));
                rig.view.canvas()->setFocus();
                rig.view.canvas()->setSelectedRow(0);
                const auto before = rig.view.anchorAddress();
                QTest::keyClick(rig.view.canvas(), Qt::Key_Return);
                WPH_CHECK_NOTE(rig.view.anchorAddress() != before,
                    QStringLiteral("loop 族指令 %1 必须被 Enter 跟随（isEditing=%2）").arg(QString::fromLatin1(h)).arg(rig.view.isEditing()));
                if (rig.view.isEditing() && QApplication::focusWidget())
                {
                    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);
                }
            }
        }

        // ---------------- M2：分支白名单 jo/jno/js/jns，此前两轮都没有用例 ----------------
        void runFlagConditionalFollowTests()
        {
            const char* hexes[] = {"7005", "7105", "7805", "7905"}; // jo/jno/js/jns rel8
            for (const char* h : hexes)
            {
                Rig3 rig(QByteArray::fromHex(h) + QByteArray(16, '\x90'));
                rig.view.canvas()->setFocus();
                rig.view.canvas()->setSelectedRow(0);
                const auto before = rig.view.anchorAddress();
                QTest::keyClick(rig.view.canvas(), Qt::Key_Return);
                WPH_CHECK_NOTE(rig.view.anchorAddress() != before,
                    QStringLiteral("标志位条件跳转 %1 必须被 Enter 跟随（isEditing=%2）").arg(QString::fromLatin1(h)).arg(rig.view.isEditing()));
                if (rig.view.isEditing() && QApplication::focusWidget())
                {
                    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);
                }
            }
        }

        // ---------------- M3：cancelInlineEdit 必须隐藏残留的错误提示框 ----------------
        void runCancelInlineEditHidesErrorTests()
        {
            Rig3 rig(QByteArray::fromHex("554889E590E900000000"));
            auto* editor = openEditor3(rig, 1);
            WPH_CHECK(editor != nullptr);
            if (editor == nullptr)
            {
                return;
            }
            editor->selectAll();
            QTest::keyClicks(editor, QStringLiteral("push 0x1122334455667788")); // 超长，编译必定失败
            QTest::keyClick(editor, Qt::Key_Return);
            auto* errorLabel = rig.view.canvas()->viewport()->findChild<QLabel*>(QStringLiteral("ksMemwbDisasmInlineError"));
            WPH_CHECK_NOTE(errorLabel != nullptr && errorLabel->isVisible(), QStringLiteral("编译失败应先弹出错误提示框"));
            rig.view.setEditable(false); // 这里会经 cancelInlineEdit() 关掉编辑框
            QCoreApplication::processEvents();
            WPH_CHECK_NOTE(errorLabel == nullptr || !errorLabel->isVisible(),
                QStringLiteral("setEditable(false) 关闭编辑框时，残留的错误提示框也必须一起隐藏"));
        }

        // ---------------- M6：reset() 同样要取消正在进行的编辑（N4 的后半句） ----------------
        void runResetCancelsInlineEditTests()
        {
            Rig3 rig(QByteArray::fromHex("554889E590E900000000"));
            auto* editor = openEditor3(rig, 1);
            WPH_CHECK(editor != nullptr);
            if (editor == nullptr)
            {
                return;
            }
            const QPointer<QLineEdit> guard(editor);
            editor->selectAll();
            QTest::keyClicks(editor, QStringLiteral("nop")); // 还没提交
            rig.view.reset();
            QCoreApplication::processEvents();
            QTest::qWait(20);
            WPH_CHECK_NOTE(guard.isNull() || !guard->isVisible(), QStringLiteral("reset() 必须关闭正在打开的编辑框"));
            WPH_CHECK_NOTE(!rig.view.isEditing(), QStringLiteral("reset() 之后 isEditing 必须复位"));
            // reset 之后重新跳转应该能正常工作（视图没有被"编辑中"状态卡住）。
            WPH_CHECK(rig.view.jumpTo(rig.base));
            WPH_CHECK_NOTE(rig.view.model()->rowAt(0).has_value(), QStringLiteral("reset 后重新跳转应正常刷新"));
        }

        // 模态预览提交：正常路径可暂存；身份/架构/权限/原始覆盖字节变化后不得沿用旧请求。
        // 直接驱动真实右键菜单与对话框，复用同一 provider 的 reset 路径是核心回归条件。
        void runAssemblyDialogContextTests()
        {
            for (int scenario = 0; scenario < 7; ++scenario)
            {
                Rig3 rig(QByteArray::fromHex("4889E590C3"));
                FakeBytesProvider replacement(64); // 同地址/同字节的新目标，不能仅靠内容判身份
                loadBytes3(replacement, rig.base, QByteArray::fromHex("4889E590C3"));
                QSignalSpy spy(&rig.view, &WorkbenchDisasmView::stageRequested);
                bool droveDialog = false; // 证明夹具确实进入并编译了预览
                QTimer watchdog; // 超时只退出本用例弹窗，函数结束即撤销定时器
                watchdog.setSingleShot(true);
                QObject::connect(&watchdog, &QTimer::timeout, &rig.view, []() {
                    if (auto* popup = QApplication::activePopupWidget()) { popup->close(); }
                    if (auto* modal = QApplication::activeModalWidget()) { modal->close(); }
                });
                watchdog.start(3000);
                QTimer::singleShot(0, &rig.view, [&]() {
                    auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                    if (menu == nullptr) { return; }
                    QAction* assemblyAction = nullptr; // 只选择菜单中真实的汇编编辑入口
                    for (auto* action : menu->actions())
                    {
                        if (action->text().contains(QStringLiteral("汇编编辑"))) { assemblyAction = action; }
                    }
                    if (assemblyAction == nullptr) { menu->close(); return; }
                    // 下一层事件循环属于汇编对话框；回调以视图为上下文，关闭用例后不会残留。
                    QTimer::singleShot(0, &rig.view, [&]() {
                        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                        if (dialog == nullptr) { return; }
                        auto* source = dialog->findChild<QPlainTextEdit*>(QStringLiteral("ksMemwbAssemblySource"));
                        QPushButton* compile = nullptr;
                        QPushButton* stage = nullptr;
                        for (auto* button : dialog->findChildren<QPushButton*>())
                        {
                            if (button->text().contains(QStringLiteral("编译"))) { compile = button; }
                            if (button->text().contains(QStringLiteral("暂存"))) { stage = button; }
                        }
                        if (source == nullptr || compile == nullptr || stage == nullptr) { dialog->reject(); return; }
                        source->setPlainText(QStringLiteral("nop"));
                        compile->click();
                        droveDialog = stage->isEnabled();
                        if (!droveDialog) { dialog->reject(); return; }
                        // 在预览成功后才改变目标条件，使旧代码的“仅检查 this 存活”错误可观测。
                        switch (scenario)
                        {
                        case 1: rig.view.reset(); rig.view.jumpTo(rig.base); break;
                        case 2: rig.view.setBytesProvider(&replacement); break;
                        case 3: rig.view.setAddressBits(32); break;
                        case 4: rig.view.setEditable(false); rig.view.setEditable(true); break;
                        case 5: loadBytes3(rig.provider, rig.base, QByteArray::fromHex("90909090C3")); break;
                        case 6:
                            rig.provider.overlay().LoadBaseline("rig3", rig.base,
                                toVec3(QByteArray::fromHex("4889E590C3")), { 0, 1, 1, 1, 1 });
                            break;
                        default: break;
                        }
                        stage->click();
                    });
                    menu->setActiveAction(assemblyAction);
                    QTest::keyClick(menu, Qt::Key_Return);
                });
                auto* canvas = rig.view.canvas();
                canvas->setSelectedRow(0);
                emit canvas->contextMenuRequested(canvas->contentRect(0).center());
                watchdog.stop();
                WPH_CHECK(droveDialog);
                WPH_CHECK(spy.count() == (scenario == 0 ? 1 : 0));
            }
        }

        // ---------------- M8：右键菜单的 QPointer 自guard，this 存活时必须正常工作 ----------------
        void runContextMenuNormalPathStillWorksTests()
        {
            Rig3 rig(QByteArray::fromHex("554889E590E900000000"));
            // 本函数 this（rig.view）在整个过程中都存活，这里要证明"加了 QPointer 自guard
            // 之后，正常路径（this 没被销毁）仍然会继续处理菜单选中结果"——不是加了 guard
            // 反而把判断方向写反，变成"活着就什么都不做"。
            bool found = false;
            QTimer::singleShot(0, [&]() {
                if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget()))
                {
                    for (auto* action : menu->actions())
                    {
                        if (action->text().contains(QStringLiteral("复制地址")))
                        {
                            found = true;
                            menu->setActiveAction(action);
                        }
                    }
                    if (found)
                    {
                        QTest::keyClick(menu, Qt::Key_Return);
                    }
                    else
                    {
                        menu->close();
                    }
                }
            });
            QTimer::singleShot(2000, []() {
                if (auto* p = QApplication::activePopupWidget()) { p->close(); }
            });
            auto* canvas = rig.view.canvas();
            canvas->setSelectedRow(1);
            QGuiApplication::clipboard()->clear();
            emit canvas->contextMenuRequested(canvas->contentRect(1).center());
            WPH_CHECK(found);
            const auto row1 = rig.view.model()->rowAt(1);
            WPH_CHECK(row1.has_value());
            if (row1.has_value())
            {
                WPH_CHECK_NOTE(QGuiApplication::clipboard()->text() == hexcanvas_format::FormatAddress(row1->address, 16),
                    QStringLiteral("选中'复制地址'后，this 仍存活的正常路径必须真的把地址写进剪贴板，实得：%1")
                        .arg(QGuiApplication::clipboard()->text()));
            }
        }
    }

    void RunDisasmRegressionTests3()
    {
        const int before = g_checks;
        const int beforeFail = g_failures;
        runInlineCompleteInstructionRejectTests();
        runInlineCompleteInstructionAcceptTests();
        runSetEditableClosesEditorTests();
        runSetEditableDoesNotFreezeTests();
        runDbOperandFormatTests();
        runRowKindPriorityTests();
        runSelectionSurvivesRefreshTests();
        runInlineErrorFollowsThemeTests();
        runFocusAfterDeferredRefreshTests();
        runFocusAfterHostRefreshInStageSlotTests();
        runDeferredRefreshIsAppliedTests();
        runStaleEditorAcrossTargetsTests();
        runLoopFamilyFollowTests();
        runFlagConditionalFollowTests();
        runCancelInlineEditHidesErrorTests();
        runResetCancelsInlineEditTests();
        runAssemblyDialogContextTests();
        runContextMenuNormalPathStillWorksTests();
        std::cerr << "[DisasmRegression3] checks=" << (g_checks - before) << " failures=" << (g_failures - beforeFail) << std::endl;
    }
}
