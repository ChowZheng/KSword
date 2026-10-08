// 使用生产 MemoryEditorWidget 和 Qt 鼠标/键盘事件验证行内暂存，绝不访问真实内存。
#include "../Ksword5.1/Ksword5.1/UI/MemoryEditorWidget.h"
#include "../Ksword5.1/Ksword5.1/UI/HexEditorWidget.h"
#include <QApplication>
#include "../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchDisasmView.h"
#include "../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchTextView.h"
#include "../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/MemoryRowCanvas.h"
#include "../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/HexCanvas.h"
#include "../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/HexViewWidgets.h"
#include "../Ksword5.1/Ksword5.1/UI/MemorySnapshotBytesProvider.h"
#include <QComboBox>
#include <QCheckBox>
#include <QClipboard>
#include <QDialog>
#include <QDir>
#include <QFontDatabase>
#include <QLineEdit>
#include <QLabel>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QTableWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QTest>
#include <QTimer>
#include <cstdlib>
#include <iostream>
#include <string>
#include <typeinfo>

namespace
{
    unsigned checks = 0; // 已执行的断言数。
    QString previewDirectory;

    // require 接收断言及说明，失败退出非零，成功累计计数。
    void require(bool condition, const char* description)
    {
        ++checks;
        if (!condition)
        {
            std::cerr << "FAIL [" << checks << "]: " << description << '\n';
            std::exit(1);
        }
    }

    // flushEvents 处理委托关闭与排队的缓存事务，不进行目标进程读写。
    void flushEvents()
    {
        QApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QApplication::processEvents();
    }

    // Exercise the production row canvas, including its selection/edit distinction.
    QLineEdit* clickEditor(ks::ui::MemoryEditorWidget& widget)
    {
        auto* view = widget.disassemblyView();
        auto* canvas = view->canvas();
        if (view->isEditing())
        {
            if (auto* active = view->findChild<QLineEdit*>(QStringLiteral("ksMemwbDisasmInlineEditor")))
                QTest::keyClick(active, Qt::Key_Escape);
            flushEvents();
        }
        const auto point = canvas->contentRect(0).center();
        QTest::mouseClick(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, point);
        flushEvents();
        require(!view->isEditing(), "single click selects without starting assembly editing");
        QTest::mouseDClick(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, point);
        flushEvents();
        auto* editor = view->findChild<QLineEdit*>(QStringLiteral("ksMemwbDisasmInlineEditor"));
        require(editor != nullptr && editor->isVisible(), "double click opens the canvas inline editor");
        for (auto* window : QApplication::topLevelWidgets())
            require(qobject_cast<QDialog*>(window) == nullptr || !window->isVisible(), "inline editing opens no dialog");
        return editor;
    }

    // submit 输入整条指令并按 Enter；只允许改变编辑器缓存。
    void submit(ks::ui::MemoryEditorWidget& widget, const char* instruction)
    {
        auto* editor = clickEditor(widget);
        editor->setText(QString::fromLatin1(instruction));
        QTest::keyClick(editor, Qt::Key_Return);
        flushEvents();
    }

    // 真实菜单/汇编预览的父销毁与陈旧请求回归。只操作假快照，不访问任何目标内存。
    // 场景 0/1 在菜单/对话框期间销毁宿主；2/3 在成功预览后暂停权限或替换目标；4 正常提交。
    void checkModalLifetimeAndStaleRequests()
    {
        constexpr std::uint64_t modalBase = 0x1000; // 所有目标共用地址，避免只靠地址误通过
        const QByteArray bytes = QByteArray::fromHex("b801000000c3");
        for (int scenario = 0; scenario < 5; ++scenario)
        {
            QPointer<ks::ui::MemoryEditorWidget> owner = new ks::ui::MemoryEditorWidget;
            owner->resize(1100, 650);
            owner->setSnapshot(bytes, modalBase);
            owner->setEditable(true);
            owner->show();
            owner->showDisassemblyAt(modalBase);
            flushEvents();
            bool drovePopup = false; // 必须确实进入目标弹窗，不能把未触发测试当成安全
            QTimer watchdog; // 超时仅关闭本用例弹窗，作用域退出便撤销定时任务
            watchdog.setSingleShot(true);
            QObject::connect(&watchdog, &QTimer::timeout, &watchdog, []() {
                if (auto* popup = QApplication::activePopupWidget()) { popup->close(); }
                if (auto* modal = QApplication::activeModalWidget()) { modal->close(); }
            });
            watchdog.start(3000);
            QTimer::singleShot(0, owner.data(), [&]() {
                auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                if (menu == nullptr) { return; }
                if (scenario == 0)
                {
                    drovePopup = true;
                    delete owner.data();
                    return;
                }
                QAction* assemblyAction = nullptr; // 冻结菜单中真实汇编动作
                for (auto* action : menu->actions())
                {
                    if (action->text().contains(QStringLiteral("汇编编辑"))) { assemblyAction = action; }
                }
                if (assemblyAction == nullptr) { menu->close(); return; }
                QTimer::singleShot(0, owner.data(), [&]() {
                    auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                    if (dialog == nullptr) { return; }
                    if (scenario == 1)
                    {
                        drovePopup = true;
                        delete owner.data();
                        return;
                    }
                    auto* source = dialog->findChild<QPlainTextEdit*>(QStringLiteral("ksMemwbAssemblySource"));
                    QPushButton* compile = nullptr;
                    QPushButton* stage = nullptr;
                    for (auto* button : dialog->findChildren<QPushButton*>())
                    {
                        if (button->text().contains(QStringLiteral("编译"))) { compile = button; }
                        if (button->text().contains(QStringLiteral("填入暂存"))) { stage = button; }
                    }
                    if (source == nullptr || compile == nullptr || stage == nullptr) { dialog->reject(); return; }
                    source->setPlainText(QStringLiteral("nop"));
                    compile->click();
                    drovePopup = stage->isEnabled();
                    if (!drovePopup) { dialog->reject(); return; }
                    if (scenario == 2) { owner->setEditable(false); owner->setEditable(true); }
                    if (scenario == 3)
                    {
                        owner->setSnapshot(bytes, modalBase, ks::ui::DisassemblyArchitecture::X64,
                            modalBase, QStringLiteral("new-target"));
                    }
                    stage->click();
                });
                menu->setActiveAction(assemblyAction);
                QTest::keyClick(menu, Qt::Key_Return);
            });
            auto* canvas = owner->disassemblyView()->canvas();
            emit canvas->contextMenuRequested(canvas->rowRect(0).center());
            watchdog.stop();
            require(drovePopup, "modal regression enters the requested popup");
            if (scenario < 2)
            {
                require(owner.isNull(), "parent destruction exits modal interaction safely");
            }
            else
            {
                require(owner != nullptr, "normal modal return keeps its owner");
                require(owner->hasChanges() == (scenario == 4), "stale assembly payload cannot change new context");
                delete owner.data();
            }
            flushEvents();
        }
    }

    void checkSnapshotTextBounds()
    {
        using namespace ks::ui;
        MemoryEditorWidget owner;
        constexpr std::uint64_t snapshotBase = 0x2000;
        owner.setSnapshot(QByteArray(128, 'A'), snapshotBase,
            DisassemblyArchitecture::X64, snapshotBase + 8);
        owner.textView()->setEncoding(WorkbenchTextView::Encoding::Utf8);
        owner.findChild<QTabWidget*>()->setCurrentIndex(2);
        auto* text = owner.textView();
        require(!text->canvas()->rows().isEmpty(), "snapshot text starts from captured bytes");
        text->canvas()->requestMore(-1, 3);
        require(text->windowAddress() == snapshotBase && !text->canvas()->rows().isEmpty(),
            "snapshot text backward browse clamps to its actual captured base");
        require(text->canvas()->rows().front().address == snapshotBase,
            "snapshot text lookbehind cannot read below the captured base");
        text->setWindow(snapshotBase + 127, 1);
        text->canvas()->requestMore(1, 3);
        require(text->windowAddress() == snapshotBase + 127,
            "snapshot text forward browse stops at its actual captured tail");
        require(text->canvas()->rows().size() == 1 && text->canvas()->rows().front().bytes == QByteArray("A"),
            "captured tail remains visible after a rejected forward move");

        owner.setSnapshot(QByteArray(16, 'X'), 0xFFFFFFF0ULL, DisassemblyArchitecture::X86);
        text->setWindow(0xFFFFFFFFULL, 1);
        text->canvas()->requestMore(1, 3);
        require(text->windowAddress() == 0xFFFFFFFFULL && !text->canvas()->rows().isEmpty(),
            "32-bit snapshot text does not browse beyond its last captured address");

        owner.setSnapshot(QByteArray("Z"), UINT64_MAX, DisassemblyArchitecture::X64);
        text->canvas()->requestMore(-1, 3);
        text->canvas()->requestMore(1, 3);
        require(text->windowAddress() == UINT64_MAX && text->canvas()->rows().size() == 1
            && text->canvas()->rows().front().bytes == QByteArray("Z"),
            "one-byte UINT64_MAX snapshot keeps bounded text navigation without overflow");
        owner.clear();
        require(text->canvas()->rows().isEmpty(), "empty snapshot has no text evidence");
        text->canvas()->requestMore(1, 3);
        require(text->canvas()->rows().isEmpty(), "empty snapshot cannot browse stale captured bytes");
    }

    void checkKernelModalParentLifetime()
    {
        QPointer<ks::ui::KernelDisassemblyDialog> owner = new ks::ui::KernelDisassemblyDialog;
        owner->setSnapshot(QByteArray::fromHex("90c3"), 0xFFFF800000002000ULL,
            ks::ui::DisassemblyArchitecture::X64, QStringLiteral("lifetime fixture"));
        owner->setKernelMutationEnabled(true);
        bool entered = false;
        QTimer::singleShot(0, owner, [&]() {
            entered = QApplication::activeModalWidget() != nullptr;
            delete owner.data();
        });
        owner->requestModifyBytes(0xFFFF800000002000ULL, QByteArray::fromHex("90"));
        require(entered && owner.isNull(), "kernel modal evidence owner may retire without deleting a stack dialog");
        flushEvents();
    }

    QByteArray capturedRowBytes(const ks::ui::MemoryRowCanvas* canvas)
    {
        QByteArray bytes;
        for (const auto& row : canvas->rows()) bytes += row.bytes;
        return bytes;
    }

    void saveFilePreview(ks::ui::MemoryEditorWidget& owner, const QString& name)
    {
        if (previewDirectory.isEmpty()) return;
        for (const auto& size : {QSize(1100, 650), QSize(700, 480)})
        {
            owner.resize(size);
            flushEvents();
            const auto path = QDir(previewDirectory).filePath(name + QLatin1Char('-')
                + QString::number(size.width()) + QLatin1Char('x') + QString::number(size.height()) + QStringLiteral(".png"));
            require(owner.grab().save(path), "file shared-view preview is saved");
        }
        owner.resize(1100, 650);
        flushEvents();
    }

    // File coordinates must remain 64-bit independently of the code decoder.
    // All bytes here are synthetic snapshots; no disk or process is written.
    void checkFileSnapshotViews()
    {
        using namespace ks::ui;
        constexpr std::uint64_t fileOffset = 0x100002000ULL;
        const auto bytes = QByteArray::fromHex("b801000000c3");
        MemoryEditorWidget owner;
        require(owner.addressKind() == SnapshotAddressKind::MemoryAddress,
            "existing snapshot owners default to memory addresses");
        owner.setAddressKind(SnapshotAddressKind::FileOffset);
        owner.setEditable(false);
        owner.setSnapshot(bytes, fileOffset, DisassemblyArchitecture::X86,
            fileOffset, QStringLiteral("fixture-file-one"));
        owner.resize(1100, 650);
        owner.show();
        owner.activateWindow();
        auto* tabs = owner.findChild<QTabWidget*>();
        require(tabs != nullptr && tabs->count() == 4,
            "file snapshots expose shared hex disassembly text and comparison tabs");
        require(owner.baseAddress() == fileOffset && owner.data() == bytes
            && owner.hexEditor()->baseAddress() == fileOffset,
            "hex retains file offset above four GiB without changing bytes");
        require(!owner.hexEditor()->isEditable() && !owner.disassemblyView()->isEditable(),
            "file owner read-only permission applies to both editing surfaces");
        owner.setAddressKind(SnapshotAddressKind::FileOffset);
        require(owner.data() == bytes, "reapplying the same coordinate domain keeps the snapshot");

        owner.showDisassemblyAt(fileOffset);
        flushEvents();
        auto* disasm = owner.disassemblyView();
        auto* canvas = disasm->canvas();
        require(!disasm->isX64() && owner.currentArchitecture() == DisassemblyArchitecture::X86,
            "file address width does not force the x86 decoder into x64");
        require(canvas->rows().size() >= 2 && canvas->rows().front().address == fileOffset
            && canvas->rows().at(1).address == fileOffset + 5 && canvas->rows().at(1).bytes == QByteArray::fromHex("c3"),
            "x86 decoded instruction rows retain full 64-bit file offsets");
        require(canvas->rows().front().tokens.front().text.compare(QStringLiteral("mov"), Qt::CaseInsensitive) == 0
            && capturedRowBytes(canvas) == bytes,
            "disassembly uses exactly the shared snapshot without zero padding");
        require(!canvas->rows().back().selectable && canvas->rows().back().bytes.isEmpty()
            && canvas->rows().back().tokens.front().text == QStringLiteral("超出已读取窗口"),
            "a file snapshot ending on a full instruction reports its actual captured limit");
        auto* decodedStatus = disasm->findChild<QLabel*>(QStringLiteral("ksMemwbDisasmStatus"));
        require(decodedStatus != nullptr && decodedStatus->text().contains(QStringLiteral("只读"))
            && !decodedStatus->text().contains(QStringLiteral("行内编辑")),
            "read-only file disassembly status describes the actual permission");
        owner.setEditable(true);
        require(decodedStatus->text().contains(QStringLiteral("行内编辑"))
            && !decodedStatus->text().contains(QStringLiteral("只读")),
            "live permission enable immediately restores editing status");
        owner.setEditable(false);
        require(decodedStatus->text().contains(QStringLiteral("只读"))
            && !decodedStatus->text().contains(QStringLiteral("行内编辑"))
            && disasm->anchorAddress() == fileOffset && capturedRowBytes(canvas) == bytes,
            "live permission pause immediately restores read-only status without replacing bytes");
        saveFilePreview(owner, QStringLiteral("file-disassembly"));
        canvas->setSelectedRow(1);
        const auto selectedInstruction = owner.selectedInstruction();
        require(owner.hexEditor()->selectedAbsoluteAddress() == fileOffset + 5
            && selectedInstruction && selectedInstruction->address == fileOffset + 5,
            "instruction selection synchronizes the absolute file offset to hex");
        owner.showDisassemblyAt(fileOffset + 5);
        flushEvents();
        require(capturedRowBytes(canvas) == QByteArray::fromHex("c3"),
            "disassembly at the captured tail contains only its actual final byte");
        canvas->requestMore(1, 3);
        flushEvents();
        require(disasm->anchorAddress() == fileOffset + 5
            && capturedRowBytes(canvas) == QByteArray::fromHex("c3"),
            "file disassembly cannot browse beyond its captured tail");
        owner.showDisassemblyAt(fileOffset);
        flushEvents();
        canvas->requestMore(-1, 3);
        flushEvents();
        require(disasm->anchorAddress() == fileOffset && capturedRowBytes(canvas) == bytes,
            "file disassembly cannot browse below its captured start");

        owner.openFindPanel();
        flushEvents();
        auto* disasmFind = disasm->findChild<QLineEdit*>(QStringLiteral("ksMemwbDisasmFind"));
        require(tabs->currentIndex() == 1 && disasmFind != nullptr && disasmFind->isVisible()
            && QApplication::focusWidget() == disasmFind,
            "shared find command opens and focuses disassembly search in place");
        tabs->setCurrentIndex(2);
        owner.jumpToAddress(fileOffset);
        owner.textView()->setEncoding(WorkbenchTextView::Encoding::Utf8);
        flushEvents();
        auto* text = owner.textView();
        require(text->windowAddress() == fileOffset && !text->canvas()->rows().isEmpty()
            && text->canvas()->rows().front().address == fileOffset
            && capturedRowBytes(text->canvas()) == bytes,
            "x86 file text retains full offset and reads the same bytes as hex and disassembly");
        text->canvas()->requestMore(-1, 3);
        require(text->windowAddress() == fileOffset && capturedRowBytes(text->canvas()) == bytes,
            "file text lookbehind clamps at the captured offset");
        text->setWindow(fileOffset + 5, 1);
        text->canvas()->requestMore(1, 3);
        require(text->windowAddress() == fileOffset + 5
            && capturedRowBytes(text->canvas()) == QByteArray::fromHex("c3"),
            "file text at the captured tail contains no invented padding");
        owner.openFindPanel();
        flushEvents();
        auto* textFind = text->findChild<QLineEdit*>(QStringLiteral("ksMemwbTextFind"));
        require(tabs->currentIndex() == 2 && textFind != nullptr && QApplication::focusWidget() == textFind,
            "shared find command focuses text search in place");

        tabs->setCurrentIndex(1);
        owner.showDisassemblyAt(fileOffset);
        flushEvents();
        QTest::mouseDClick(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, canvas->contentRect(0).center());
        QTest::keyClick(canvas, Qt::Key_Return);
        QTest::keyClick(canvas, Qt::Key_F2);
        disasm->beginSelectedInstructionEdit();
        emit disasm->stageRequested(fileOffset, QByteArray::fromHex("90"));
        flushEvents();
        require(!disasm->isEditing() && owner.data() == bytes && !owner.hasChanges(),
            "read-only disassembly rejects double click keyboard and stale stage requests");
        int editingControls = 0;
        for (const auto* button : owner.findChildren<QPushButton*>())
        {
            if (button->text().contains(QStringLiteral("汇编"))
                || button->text().contains(QStringLiteral("撤销"))
                || button->text().contains(QStringLiteral("重做")))
            {
                ++editingControls;
                require(button->isHidden(), "file read-only view hides unavailable editing controls");
            }
        }
        require(editingControls >= 3, "read-only visibility checks cover assembly undo and redo controls");
        tabs->setCurrentIndex(0);
        owner.jumpToAddress(fileOffset);
        auto* hexCanvas = owner.hexEditor()->findChild<HexCanvas*>();
        require(hexCanvas != nullptr, "file hex uses the production canvas");
        hexCanvas->setCaretAddress(fileOffset);
        QTest::keyClicks(hexCanvas, "90");
        QApplication::clipboard()->setText(QStringLiteral("90"));
        QTest::keyClick(hexCanvas, Qt::Key_V, Qt::ControlModifier);
        owner.undo();
        owner.redo();
        flushEvents();
        require(owner.data() == bytes && !owner.hasChanges(),
            "read-only file keyboard paste undo and redo cannot stage bytes");

        tabs->setCurrentIndex(3);
        auto* comparison = owner.findChild<QTableWidget*>(QStringLiteral("memory_comparison_table"));
        require(comparison != nullptr && comparison->horizontalHeaderItem(0)->text() == QStringLiteral("文件偏移"),
            "file comparison names its coordinate column as file offset");
        QCheckBox* onlyDifferences = nullptr;
        for (auto* check : owner.findChildren<QCheckBox*>())
            if (check->text() == QStringLiteral("仅显示差异")) onlyDifferences = check;
        require(onlyDifferences != nullptr, "comparison difference filter is available");
        onlyDifferences->setChecked(false);
        flushEvents();
        require(comparison->rowCount() == 1
            && comparison->item(0, 0)->text().toULongLong(nullptr, 16) == fileOffset
            && QByteArray::fromHex(comparison->item(0, 1)->text().toLatin1()) == bytes
            && QByteArray::fromHex(comparison->item(0, 2)->text().toLatin1()) == bytes,
            "file comparison renders the same exact snapshot and full-width coordinate");
        QComboBox* baseline = nullptr;
        for (auto* combo : owner.findChildren<QComboBox*>())
            if (combo->count() == 2 && combo->itemText(0) == QStringLiteral("读取基线")) baseline = combo;
        require(baseline != nullptr, "comparison baseline selector is available");
        const auto reread = QByteArray::fromHex("b802000000c3");
        owner.setSnapshot(reread, fileOffset, DisassemblyArchitecture::X86,
            fileOffset, QStringLiteral("fixture-file-one"));
        baseline->setCurrentIndex(1);
        onlyDifferences->setChecked(true);
        flushEvents();
        require(comparison->rowCount() == 1
            && QByteArray::fromHex(comparison->item(0, 1)->text().toLatin1()) == bytes
            && QByteArray::fromHex(comparison->item(0, 2)->text().toLatin1()) == reread
            && !owner.hasChanges(),
            "actual same-file rereads compare their bytes without manufacturing pending edits");
        owner.setSnapshot(reread, fileOffset, DisassemblyArchitecture::X86,
            fileOffset, QStringLiteral("fixture-file-two"));
        flushEvents();
        require(comparison->rowCount() == 0, "comparison cannot inherit a different file baseline");
        owner.openFindPanel();
        require(tabs->currentIndex() == 0, "comparison find switches to the shared hex search surface");

        const auto utf8 = QByteArray::fromHex("efbbbf48656c6c6fe4b896e7958c");
        owner.setSnapshot(utf8, fileOffset, DisassemblyArchitecture::X86);
        tabs->setCurrentIndex(2);
        text->setEncoding(WorkbenchTextView::Encoding::Auto);
        flushEvents();
        require(text->hasBom() && text->effectiveEncoding() == WorkbenchTextView::Encoding::Utf8
            && text->renderedText().contains(QStringLiteral("Hello世界")),
            "file snapshots reuse BOM-aware Unicode text decoding");
        saveFilePreview(owner, QStringLiteral("file-text"));
        owner.clear();
        flushEvents();
        require(owner.data().isEmpty() && owner.originalBytes().isEmpty()
            && capturedRowBytes(canvas).isEmpty() && text->canvas()->rows().isEmpty()
            && comparison->rowCount() == 0 && !owner.selectedInstruction(),
            "clearing a failed file read removes old bytes from every shared view");
        canvas->requestMore(1, 3);
        text->canvas()->requestMore(-1, 3);
        require(capturedRowBytes(canvas).isEmpty() && text->canvas()->rows().isEmpty(),
            "cleared file views cannot recover stale bytes by browsing");
        owner.setSnapshot(bytes, fileOffset, DisassemblyArchitecture::X86);
        owner.setSnapshot(QByteArray(2, 'X'), UINT64_MAX, DisassemblyArchitecture::X86);
        flushEvents();
        require(owner.data().isEmpty() && capturedRowBytes(canvas).isEmpty()
            && text->canvas()->rows().isEmpty(), "wrapping file snapshots clear previous evidence");
        owner.setSnapshot(QByteArray("Z"), UINT64_MAX, DisassemblyArchitecture::X86);
        text->setWindow(UINT64_MAX, 1);
        text->canvas()->requestMore(1, 3);
        require(text->windowAddress() == UINT64_MAX && capturedRowBytes(text->canvas()) == QByteArray("Z"),
            "x86 file text preserves the final 64-bit offset without overflow");
    }

    void checkFileOffsetZero()
    {
        using namespace ks::ui;
        const auto bytes = QByteArray::fromHex("b801000000c3");
        const auto reread = QByteArray::fromHex("b802000000c3");
        MemoryEditorWidget owner;
        owner.setAddressKind(SnapshotAddressKind::FileOffset);
        owner.setEditable(false);
        owner.setSnapshot(bytes, 0, DisassemblyArchitecture::X86, 0, QStringLiteral("file-origin-fixture"));
        owner.showDisassemblyAt(0);
        flushEvents();
        auto* disasm = owner.disassemblyView();
        require(disasm->hasAnchor() && disasm->anchorAddress() == 0 && capturedRowBytes(disasm->canvas()) == bytes,
            "the first file snapshot establishes a real decode anchor at offset zero");
        const auto first = owner.selectedInstruction();
        require(first && first->address == 0 && first->originalBytes == QByteArray::fromHex("b801000000"),
            "file origin selects the exact first mov instruction instead of an empty unanchored view");

        owner.setSnapshot(reread, 0, DisassemblyArchitecture::X86, 0, QStringLiteral("file-origin-fixture"));
        flushEvents();
        const auto refreshed = owner.selectedInstruction();
        require(disasm->hasAnchor() && disasm->anchorAddress() == 0 && capturedRowBytes(disasm->canvas()) == reread
            && refreshed && refreshed->address == 0 && refreshed->originalBytes == QByteArray::fromHex("b802000000"),
            "refeeding file offset zero decodes the new bytes after the view resets its anchor");
        owner.clear();
        owner.setSnapshot(QByteArray::fromHex("90"), 0, DisassemblyArchitecture::X64);
        flushEvents();
        const auto reloaded = owner.selectedInstruction();
        require(disasm->hasAnchor() && disasm->anchorAddress() == 0 && capturedRowBytes(disasm->canvas()) == QByteArray::fromHex("90")
            && reloaded && reloaded->address == 0 && reloaded->originalBytes == QByteArray::fromHex("90"),
            "a cleared file view reloads and selects the exact new byte at offset zero");
    }

    void checkCapturedDisassemblyEndNote()
    {
        using namespace ks::ui;
        MemoryEditorWidget owner;
        constexpr std::uint64_t offset = 0x100002000ULL;
        owner.setAddressKind(SnapshotAddressKind::FileOffset);
        owner.setSnapshot(QByteArray(5000, static_cast<char>(0x90)), offset, DisassemblyArchitecture::X86);
        owner.showDisassemblyAt(offset);
        flushEvents();
        auto* canvas = owner.disassemblyView()->canvas();
        require(capturedRowBytes(canvas).size() == 4096
            && !canvas->rows().back().selectable && canvas->rows().back().bytes.isEmpty()
            && canvas->rows().back().tokens.front().text == QStringLiteral("继续滚动以读取下一段"),
            "a bounded decode window still offers continuation when more captured bytes exist");
        owner.showDisassemblyAt(offset + 4096);
        flushEvents();
        require(capturedRowBytes(canvas).size() == 904
            && canvas->rows().back().tokens.front().text == QStringLiteral("超出已读取窗口"),
            "moving into the final captured decode window replaces the continuation hint");
        owner.setSnapshot(QByteArray::fromHex("90"), UINT64_MAX, DisassemblyArchitecture::X86);
        owner.showDisassemblyAt(UINT64_MAX);
        flushEvents();
        require(capturedRowBytes(canvas) == QByteArray::fromHex("90")
            && canvas->rows().back().tokens.front().text == QStringLiteral("超出已读取窗口"),
            "the one-byte UINT64_MAX file offset reports its captured limit without overflow");
    }

    // Context-menu population is exercised without triggering a debugger action.
    // The portable runner substitutes only the external navigation helper.
    void checkFileContextIsolation()
    {
        using namespace ks::ui;
        MemoryEditorWidget owner;
        constexpr std::uint64_t offset = 0x2000;
        const auto bytes = QByteArray::fromHex("90c3");
        owner.setSnapshot(bytes, offset, DisassemblyArchitecture::X64,
            offset, QStringLiteral("same-numeric-source"));
        owner.setProcessContext(123, 456);
        QMenu processMenu;
        emit owner.disassemblyView()->contextMenuAboutToShow(&processMenu, offset, true);
        require(!processMenu.actions().isEmpty(), "memory snapshot retains explicitly supplied process navigation");
        owner.setAddressKind(SnapshotAddressKind::FileOffset);
        require(owner.data().isEmpty(), "changing coordinate domain clears the old snapshot");
        owner.setSnapshot(bytes, offset, DisassemblyArchitecture::X86,
            offset, QStringLiteral("same-numeric-source"));
        owner.setProcessContext(123, 456);
        QMenu fileDisasmMenu, fileTextMenu;
        emit owner.disassemblyView()->contextMenuAboutToShow(&fileDisasmMenu, offset, true);
        emit owner.textView()->contextMenuAboutToShow(&fileTextMenu, offset, true);
        require(fileDisasmMenu.actions().isEmpty() && fileTextMenu.actions().isEmpty(),
            "file offsets reject inherited and explicitly resupplied process targets");
        owner.setAddressKind(SnapshotAddressKind::MemoryAddress);
        owner.setSnapshot(bytes, offset, DisassemblyArchitecture::X64,
            offset, QStringLiteral("same-numeric-source"));
        QMenu newMemoryMenu;
        emit owner.disassemblyView()->contextMenuAboutToShow(&newMemoryMenu, offset, true);
        require(newMemoryMenu.actions().isEmpty(), "returning to memory mode cannot resurrect an old process target");

        QPointer<MemoryEditorWidget> retiring = new MemoryEditorWidget;
        retiring->setSnapshot(bytes, offset);
        QObject::connect(retiring, &MemoryEditorWidget::bytesChanged, retiring,
            [retiring]() { delete retiring.data(); });
        retiring->setAddressKind(SnapshotAddressKind::FileOffset);
        require(retiring.isNull(), "coordinate change remains safe when clearing destroys its owner");
        flushEvents();
    }
}

int main(int argc, char** argv)
{
    QApplication application(argc, argv); // 离屏 Qt 事件循环。
    QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
    if (QFontDatabase::families().contains(QStringLiteral("Microsoft YaHei")))
        application.setFont(QFont(QStringLiteral("Microsoft YaHei"), 9));
    if (argc > 1)
    {
        previewDirectory = QString::fromLocal8Bit(argv[1]);
        require(QDir().mkpath(previewDirectory), "preview output directory is available");
    }
    ks::ui::MemoryEditorWidget widget; // 生产编辑器，快照完全由测试提供。
    constexpr std::uint64_t base = 0x1000;
    const auto original = QByteArray::fromHex("b801000000c3"); // mov eax,1; ret。
    widget.resize(1100, 650);
    widget.setSnapshot(original, base);
    widget.setEditable(true);
    widget.show();
    widget.showDisassemblyAt(base);
    flushEvents();

    // 内嵌十六进制编辑器已是 HexView 门面：内部是自绘 HexCanvas，不再有旧的页签与 18 列表格。
    require(widget.hexEditor()->findChild<ks::ui::HexCanvas*>() != nullptr, "embedded HEX paints with HexCanvas");
    require(widget.hexEditor()->findChild<QTableWidget*>() == nullptr, "embedded HEX has no legacy table");
    require(widget.hexEditor()->findChild<QTabWidget*>() == nullptr, "embedded HEX has no legacy tab widget");
    // setHexOnlyView(true) 由统一编辑器调用：隐藏 HexView 自带状态条，避免与统一状态条重复。
    ks::ui::HexViewStatusBar* hexStatusBar = nullptr;
    for (auto* child : widget.hexEditor()->findChildren<QWidget*>())
        if (auto* bar = dynamic_cast<ks::ui::HexViewStatusBar*>(child)) hexStatusBar = bar;
    require(hexStatusBar != nullptr && hexStatusBar->isHidden(), "embedded HEX status bar hidden");

    require(widget.disassemblyView()->findChild<QTableWidget*>() == nullptr, "disassembly has no legacy instruction table");
    require(widget.disassemblyView()->canvas()->rows().size() >= 2, "canvas exposes actual instruction rows");
    auto* editor = clickEditor(widget);
    require(editor->text().startsWith(QStringLiteral("mov ")), "content pane edits the complete instruction");
    require(editor->width() > 100, "inline editor spans the right content pane");
    QTest::keyClick(editor, Qt::Key_Return);
    flushEvents();
    require(widget.data() == original && !widget.hasChanges(), "unchanged edit preserves bytes");

    // 错误空白和较长指令不能影响缓存或相邻 ret。
    submit(widget, "mov eax, 1 0");
    require(widget.data() == original, "invalid whitespace leaves cache unchanged");
    submit(widget, "mov rax, 1122334455667788");
    require(widget.data() == original, "long instruction cannot overwrite neighbor");
    submit(widget, "mov eax, 2");
    require(widget.data() == QByteArray::fromHex("b802000000c3"), "Enter stages valid instruction");
    require(widget.originalBytes() == original, "staging does not accept original snapshot");
    widget.undo();
    require(widget.data() == original, "inline edit participates in undo");
    widget.redo();
    require(widget.data() == QByteArray::fromHex("b802000000c3"), "inline edit participates in redo");
    widget.undo();

    // Esc 取消；缩短的指令填 NOP，长度和下一条指令保持不变。
    editor = clickEditor(widget);
    editor->setText(QStringLiteral("mov eax, 3"));
    QTest::keyClick(editor, Qt::Key_Escape);
    flushEvents();
    require(widget.data() == original, "Escape cancels inline edit");
    submit(widget, "nop");
    require(widget.data() == QByteArray::fromHex("9090909090c3"), "short instruction pads original span");
    widget.undo();
    require(widget.data() == original, "padded edit undoes as one transaction");

    // New snapshots, architecture and permissions cancel the frozen editor.
    QPointer<QLineEdit> frozenEditor = clickEditor(widget);
    frozenEditor->setText(QStringLiteral("mov eax, 4"));
    widget.setSnapshot(original, base + 0x100);
    widget.showDisassemblyAt(base + 0x100);
    flushEvents();
    require(!frozenEditor || !frozenEditor->isVisible(), "snapshot replacement cancels frozen inline input");
    require(widget.data() == original && !widget.hasChanges(), "replacement snapshot remains unchanged");

    frozenEditor = clickEditor(widget);
    frozenEditor->setText(QStringLiteral("mov eax, 6"));
    QComboBox* architecture = nullptr;
    for (auto* combo : widget.findChildren<QComboBox*>())
        if (combo->count() == 2 && combo->itemText(0) == QStringLiteral("x86")
            && combo->itemText(1) == QStringLiteral("x64")) architecture = combo;
    require(architecture != nullptr, "snapshot architecture selector found");
    architecture->setCurrentIndex(0);
    flushEvents();
    require(!frozenEditor || !frozenEditor->isVisible(), "architecture change cancels frozen inline input");
    require(widget.data() == original, "architecture change preserves snapshot bytes");
    architecture->setCurrentIndex(1);
    flushEvents();

    frozenEditor = clickEditor(widget);
    frozenEditor->setText(QStringLiteral("mov eax, 5"));
    widget.setEditable(false);
    flushEvents();
    require(!frozenEditor || !frozenEditor->isVisible(), "read-only transition cancels input");
    require(widget.data() == original, "read-only transition preserves cache");
    widget.setSnapshot(original, base);
    widget.showDisassemblyAt(base);
    flushEvents();
    auto* canvas = widget.disassemblyView()->canvas();
    QTest::mouseDClick(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, canvas->contentRect(0).center());
    flushEvents();
    require(!widget.disassemblyView()->isEditing(), "read-only double click cannot edit");

    // A selected instruction maps to its complete range in the HEX facade.
    std::uint64_t selectionStart = 99, selectionEnd = 99;
    bool selected = false;
    const auto selectionConnection = QObject::connect(widget.hexEditor(), &HexEditorWidget::selectionChanged, &widget,
        [&](std::uint64_t first, std::uint64_t last, bool valid) {
            selectionStart = first; selectionEnd = last; selected = valid;
        });
    canvas->setSelectedRow(1);
    canvas->setSelectedRow(0);
    require(selected && selectionStart == 0 && selectionEnd == 5, "instruction selection synchronizes all five bytes to HEX");
    QObject::disconnect(selectionConnection);

    // Provider bounds, references and priority are tested independently of painting.
    ks::ui::MemorySnapshotBytesProvider provider;
    const auto baseline = QByteArray::fromHex("01020304");
    provider.setSnapshot(base, QByteArray::fromHex("01090304"), baseline,
        QByteArray::fromHex("00080304"), 32, true);
    const auto window = provider.FetchWindow(base, 65536);
    using Kind = ksword::memwb::ByteChangeKind;
    require(window.ok && window.bytes.size() == 4, "provider clips at the captured tail");
    require(window.validMask == std::vector<std::uint8_t>(4, 1), "only actual snapshot bytes are valid");
    require(window.changeKinds[0] == Kind::ExternalChange, "actual reread changes retain the previous reference");
    require(window.changeKinds[1] == Kind::Pending, "pending edits take priority over external changes");
    require(provider.AddressBits() == 32 && provider.HasPreviousRead(), "provider publishes architecture and previous-read availability");
    require(!provider.FetchWindow(base - 1, 2).ok && !provider.FetchWindow(base + 4, 1).ok,
        "provider rejects uncaptured address ranges");
    require(provider.FetchWindow(base, 0).ok, "empty requests have an explicit successful empty window");
    provider.setSnapshot(base, baseline, baseline, {}, 64, false);
    require(!provider.HasPreviousRead() && provider.FetchWindow(base, 4).changeKinds[0] == Kind::Unchanged,
        "highlight suppression and unavailable previous reads stay separate");
    checkModalLifetimeAndStaleRequests();
    checkSnapshotTextBounds();
    checkKernelModalParentLifetime();
    checkFileSnapshotViews();
    checkFileOffsetZero();
    checkCapturedDisassemblyEndNote();
    checkFileContextIsolation();
    std::cout << "PASS: " << checks << " memory editor Qt checks\n";
}
