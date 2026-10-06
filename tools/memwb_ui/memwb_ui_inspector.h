#pragma once

// ============================================================
// memwb_ui_inspector.h
// 作用：HexInspectorPanel（数据解释器面板）离屏夹具的公共设施——"画布 + 面板并排"的场景、
//       测试数据、假的指针命名器、行查找与输入模拟辅助。只被 memwb_ui_tests.Inspector*.cpp 使用。
// ============================================================

#include "memwb_ui_common.h"

#include "../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/HexInspectorPanel.h"
#include "../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/HexInspectorRowView.h"
#include "../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/HexInspectorWidgets.h"

#include <QSplitter>
#include <QString>
#include <QTemporaryDir>

#include <memory>
#include <string>

namespace memwb_test
{
    // kInspectorBase：测试数据的起始地址（4096 对齐，便于直接投递整页）。
    inline constexpr std::uint64_t kInspectorBase = 0x00400000ULL;

    // FakeNamer：假的指针命名器，只认一个地址，并统计被调用次数。
    class FakeNamer final : public ksword::memwb::IPointerNamer
    {
    public:
        // Describe：地址等于 kKnownAddress 或 kParenAddress 时命中并给出描述，否则未命中。
        bool Describe(std::uint64_t address, std::string& out) override;

        // kKnownAddress：能命中的指针值，描述是 "kernel32.dll+0x1A40"。
        static constexpr std::uint64_t kKnownAddress = 0x00007FF612341A40ULL;

        // kParenAddress：描述本身含括号的指针值，描述是 "ntdll!Foo (bar)"（验证值/地址拆分不会被描述里的括号带偏）。
        static constexpr std::uint64_t kParenAddress = 0x00007FF600000123ULL;

        int calls = 0;      // Describe 被调用的次数
    };

    // InspectorScene：一个"画布 + 解释器面板"并排的场景。
    // 成员声明顺序决定析构顺序：splitter（及其子控件画布与面板）最先销毁，overlay/provider 最后。
    struct InspectorScene
    {
        ksword::memwb::MemoryDiffOverlay overlay;       // 暂存叠加层
        RecordingProvider provider;                     // 异步场景用的记录型页提供者
        FakeNamer namer;                                // 指针命名器
        QByteArray data;                                // 底层数据
        QTemporaryDir settingsDir;                      // 存放设置 INI 的临时目录
        std::unique_ptr<QSplitter> splitter;            // 顶层容器，画布与面板是它的子控件
        ks::ui::HexCanvas* canvas = nullptr;            // 画布（由 splitter 持有）
        ks::ui::HexInspectorPanel* panel = nullptr;     // 解释器面板（由 splitter 持有）

        // settingsPath：该场景使用的设置 INI 路径。
        QString settingsPath() const;
    };

    // MakeInspectorData：生成测试数据：整数、ASCII、指针、浮点、GUID、UTF-16 等放在固定偏移上。
    // 偏移表见实现；其余字节是 MakePattern 的确定性内容（不含 0x00）。
    QByteArray MakeInspectorData();

    // MakeInspectorScene：构造静态数据场景并显示。
    // 传入：数据、是否可编辑、是否挂叠加层、整体尺寸、面板宽度。
    // 传出：场景；画布已载入数据与叠加层基线，面板已关联画布并使用场景自己的设置文件。
    std::unique_ptr<InspectorScene> MakeInspectorScene(
        const QByteArray& data,
        bool editable,
        bool withOverlay,
        const QSize& size,
        int panelWidth);

    // MakeAsyncInspectorScene：构造"页由测试手动投递"的场景（画布地址空间 [base, base+0x3FFF]，不自动供页）。
    std::unique_ptr<InspectorScene> MakeAsyncInspectorScene(bool editable, const QSize& size, int panelWidth);

    // RowIndexOf：按类型键（"u32" 等）找行号，找不到返回 -1。
    int RowIndexOf(const ks::ui::HexInspectorPanel& panel, const QString& typeKey);

    // RowOf：按类型键取行数据（找不到时返回空行）。
    const ks::ui::HexInspectorRowData& RowOf(const ks::ui::HexInspectorPanel& panel, const QString& typeKey);

    // ClickAddress：先保证地址在画布里可见（不可见就滚动过去），再在它的单元格上做一次真实的鼠标点击。
    // 为什么需要：画布只给可见单元格几何，不可见地址的 cellRect 为空，直接点击会点到视口原点。
    void ClickAddress(
        ks::ui::HexCanvas& canvas,
        std::uint64_t address,
        ks::ui::HexCanvas::ActivePane pane = ks::ui::HexCanvas::ActivePane::Hex,
        Qt::KeyboardModifiers modifiers = Qt::NoModifier);

    // DoubleClickRow：在行的值列上做一次真实的鼠标双击。
    void DoubleClickRow(ks::ui::HexInspectorPanel& panel, const QString& typeKey);

    // TypeIntoEditor：向当前行内编辑器键入文字（替换全选内容），不按 Enter。
    void TypeIntoEditor(ks::ui::HexInspectorPanel& panel, const QString& text);

    // PressInEditor：向当前行内编辑器发送一次按键。
    void PressInEditor(ks::ui::HexInspectorPanel& panel, Qt::Key key);

    // FlushDeferred：处理挂起事件并执行延迟删除，使 deleteLater 的编辑器真正消失。
    void FlushDeferred();

    // 各组测试入口（定义在对应的 .cpp）。
    void RunInspectorViewTests();
    void RunInspectorEditTests();
    void RunInspectorShotsAndBench(const QString& shotsDir);

    // RunInspectorSignalTests：面板侧的信号接线验证（无轮询定时器、订阅 contentChanged/editableChanged、
    // 编辑只经画布 stageBytes 且不重复发信号），定义在 memwb_ui_tests.Signals.Inspector.cpp，
    // 由 RunSignalTests 调用。
    void RunInspectorSignalTests();
}
