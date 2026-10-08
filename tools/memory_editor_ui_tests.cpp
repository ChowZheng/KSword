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
#include <QDialog>
#include <QLineEdit>
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
}

int main(int argc, char** argv)
{
    QApplication application(argc, argv); // 离屏 Qt 事件循环。
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
    std::cout << "PASS: " << checks << " memory editor Qt checks\n";
}
