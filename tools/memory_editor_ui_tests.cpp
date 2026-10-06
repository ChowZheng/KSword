// 使用生产 MemoryEditorWidget 和 Qt 鼠标/键盘事件验证行内暂存，绝不访问真实内存。
#include "../Ksword5.1/Ksword5.1/UI/MemoryEditorWidget.h"
#include "../Ksword5.1/Ksword5.1/UI/HexEditorWidget.h"
#include <QApplication>
#include <QAbstractItemDelegate>
#include <QComboBox>
#include <QDialog>
#include <QLineEdit>
#include <QTableWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QTest>
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

    // clickEditor 单击指定汇编列并返回实际委托输入框；输入是生产控件和列号。
    QLineEdit* clickEditor(ks::ui::MemoryEditorWidget& widget, int column = 2)
    {
        auto* table = widget.instructionTable();
        const auto index = table->model()->index(0, column);
        QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier,
            table->visualRect(index).center());
        flushEvents();
        QLineEdit* editor = nullptr; // 只选当前可见的委托，不误取延迟销毁的旧输入框。
        for (auto* candidate : table->findChildren<QLineEdit*>(QStringLiteral("memory_inline_assembly")))
        {
            if (candidate->isVisible())
            {
                editor = candidate;
            }
        }
        if (editor == nullptr)
        {
            std::cerr << "row count=" << table->rowCount() << " column=" << column
                << " visible=" << table->isVisible() << " current column=" << table->currentColumn()
                << " flags=" << static_cast<int>(table->model()->flags(index)) << '\n';
        }
        require(editor != nullptr && editor->isVisible(), "single click opens inline editor");
        for (auto* window : QApplication::topLevelWidgets())
        {
            require(qobject_cast<QDialog*>(window) == nullptr || !window->isVisible(), "no dialog on click");
        }
        return editor;
    }

    // findDescendantByTypeName 在 root 的全部后代里按运行时类型名片段查找控件。
    // 十六进制新组件的头文件依赖 C++20，本测试按 C++17 编译，不能直接包含，
    // 所以用 RTTI 类型名识别自绘控件；输入根控件与类型名片段，返回第一个匹配项，没有则为 nullptr。
    QWidget* findDescendantByTypeName(QWidget* root, const char* typeNamePart)
    {
        for (auto* child : root->findChildren<QWidget*>())
        {
            if (std::string(typeid(*child).name()).find(typeNamePart) != std::string::npos)
            {
                return child;
            }
        }
        return nullptr;
    }

    // submit 输入整条指令并按 Enter；只允许改变编辑器缓存。
    void submit(ks::ui::MemoryEditorWidget& widget, const char* instruction)
    {
        auto* editor = clickEditor(widget);
        editor->setText(QString::fromLatin1(instruction));
        QTest::keyClick(editor, Qt::Key_Return);
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
    require(findDescendantByTypeName(widget.hexEditor(), "ks::ui::HexCanvas") != nullptr, "embedded HEX paints with HexCanvas");
    require(widget.hexEditor()->findChild<QTableWidget*>() == nullptr, "embedded HEX has no legacy table");
    require(widget.hexEditor()->findChild<QTabWidget*>() == nullptr, "embedded HEX has no legacy tab widget");
    // setHexOnlyView(true) 由统一编辑器调用：隐藏 HexView 自带状态条，避免与统一状态条重复。
    auto* hexStatusBar = findDescendantByTypeName(widget.hexEditor(), "ks::ui::HexViewStatusBar");
    require(hexStatusBar != nullptr && hexStatusBar->isHidden(), "embedded HEX status bar hidden");

    // 单击操作数列也编辑完整指令；未改文本的提交保留原机器码。
    auto* editor = clickEditor(widget, 3);
    require(editor->text().startsWith(QStringLiteral("mov ")), "operand column edits full instruction");
    require(editor->width() > widget.instructionTable()->columnWidth(2), "editor spans both assembly columns");
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

    // 委托提交后、事务执行前换快照，旧输入必须被拒绝。
    editor = clickEditor(widget);
    editor->setText(QStringLiteral("mov eax, 4"));
    widget.instructionTable()->itemDelegate()->setModelData(editor,
        widget.instructionTable()->model(), widget.instructionTable()->model()->index(0, 2));
    widget.setSnapshot(original, base + 0x100);
    widget.showDisassemblyAt(base + 0x100);
    flushEvents();
    require(widget.data() == original && !widget.hasChanges(), "snapshot replacement rejects stale edit");

    // 架构切换不能把按 x64 编译的排队事务应用到 x86 快照。
    editor = clickEditor(widget);
    editor->setText(QStringLiteral("mov eax, 6"));
    widget.instructionTable()->itemDelegate()->setModelData(editor,
        widget.instructionTable()->model(), widget.instructionTable()->model()->index(0, 2));
    QComboBox* architecture = nullptr; // 生产架构选择器，由 x86/x64 条目识别。
    for (auto* combo : widget.findChildren<QComboBox*>())
    {
        if (combo->count() == 2 && combo->itemText(0) == QStringLiteral("x86")
            && combo->itemText(1) == QStringLiteral("x64"))
        {
            architecture = combo;
        }
    }
    require(architecture != nullptr, "architecture selector found");
    architecture->setCurrentIndex(0);
    flushEvents();
    require(widget.data() == original, "architecture change rejects queued edit");
    architecture->setCurrentIndex(1);
    flushEvents();

    // 行内事务排队后变为只读，也不得提交；只读点击不创建输入框。
    editor = clickEditor(widget);
    editor->setText(QStringLiteral("mov eax, 5"));
    widget.instructionTable()->itemDelegate()->setModelData(editor,
        widget.instructionTable()->model(), widget.instructionTable()->model()->index(0, 2));
    widget.setEditable(false);
    flushEvents();
    require(widget.data() == original, "read-only transition rejects queued edit");
    widget.setSnapshot(original, base);
    widget.showDisassemblyAt(base);
    flushEvents();
    QTest::mouseClick(widget.instructionTable()->viewport(), Qt::LeftButton, Qt::NoModifier,
        widget.instructionTable()->visualRect(widget.instructionTable()->model()->index(0, 2)).center());
    flushEvents();
    editor = widget.instructionTable()->findChild<QLineEdit*>(QStringLiteral("memory_inline_assembly"));
    require(editor == nullptr || !editor->isVisible(), "read-only click cannot edit");
    std::cout << "PASS: " << checks << " memory editor Qt checks\n";
}
