#pragma once

// ============================================================
// memwb_ui_facade.h
// 作用：HexEditorWidget 门面（UI/HexEditorWidget.h/.cpp，HexView 之上的薄转发层）离屏夹具的公共设施——
//       夹具构造、取内部 HexView / 画布 / 单元格状态的访问函数、字节辅助，以及三组测试的入口。
//       只被 memwb_ui_tests.Facade*.cpp 与 memwb_ui_tests.cpp 使用，不属于主程序。
// 门面本身只依赖 Qt 与 HexView，不依赖 Framework.h 等主程序专有头文件，所以夹具可以直接编译它。
// 文件分工：
//   memwb_ui_tests.Facade.cpp          公共设施定义 + 总入口 RunFacadeTests + 默认值 / 数据模型 / 可编辑与行宽 / 变更参照
//   memwb_ui_tests.Facade.Signals.cpp  byteEdited / setByteAtAbsoluteAddress / 选区与插入点信号 / 跳转 / 查找与跳转条 /
//                                      右键菜单转发 / setHexOnlyView
//   memwb_ui_tests.Facade.Hosts.cpp    类名链（TextSearchReplaceSupport 的判断）/ FileDock 使用形态 /
//                                      DiskEditor 使用形态 / 销毁安全
// ============================================================

#include "memwb_ui_hexview.h"

#include "../../Ksword5.1/Ksword5.1/UI/HexEditorWidget.h"

#include <QByteArray>
#include <QLineEdit>
#include <QSize>
#include <QString>

#include <cstdint>
#include <memory>

namespace memwb_test
{
    // MakeFacade：构造并显示一个 HexEditorWidget，载入数据。
    // 传入：基址、数据、是否可编辑、窗口大小；传出：已 show、已处理一轮事件的门面控件。
    std::unique_ptr<HexEditorWidget> MakeFacade(
        std::uint64_t base,
        const QByteArray& data,
        bool editable,
        const QSize& size);

    // ViewOf：门面内部的 HexView（找不到时为空指针）。
    // 传入：门面控件；传出：它的唯一一个 HexView 子控件。
    inline ks::ui::HexView* ViewOf(HexEditorWidget& widget)
    {
        return widget.findChild<ks::ui::HexView*>();
    }

    // CanvasOf：门面内部 HexView 的画布。调用前应已确认 ViewOf 非空（RunFacadeCoreTests 的第一个用例做了）。
    inline ks::ui::HexCanvas* CanvasOf(HexEditorWidget& widget)
    {
        return ViewOf(widget)->canvas();
    }

    // FacadeCell：取画布上某地址的显示状态（所见即所绘，change 字段用来验证橙色 / 冷色着色）。
    inline ks::ui::HexCanvas::CellState FacadeCell(HexEditorWidget& widget, std::uint64_t address)
    {
        return CanvasOf(widget)->cellStateAt(address);
    }

    // HexByteText：字节的两位大写十六进制文本，用来向画布键入。
    inline QString HexByteText(int value)
    {
        return QStringLiteral("%1").arg(value & 0xFF, 2, 16, QLatin1Char('0')).toUpper();
    }

    // ByteValue：数组里第 index 个字节的无符号值。
    inline int ByteValue(const QByteArray& bytes, qsizetype index)
    {
        return static_cast<std::uint8_t>(bytes.at(index));
    }

    // OtherByte：取一个与 current 不同的字节值（保证"这次编辑一定真的改变了字节"）。
    // 传入：当前值、期望值；传出：期望值，若与当前值相同则取下一个。
    inline int OtherByte(int current, int preferred)
    {
        return (preferred & 0xFF) == (current & 0xFF) ? ((preferred + 1) & 0xFF) : (preferred & 0xFF);
    }

    // 各组测试入口（定义在对应的 .cpp，由 RunFacadeTests 依次调用）。
    void RunFacadeCoreTests();
    void RunFacadeSignalTests();
    void RunFacadeHostTests();

    // RunFacadeTests：门面全部验证的总入口，由 memwb_ui_tests.cpp 的 main 调用。
    // 作用：重定向默认 QSettings（HexView 会持久化解释器面板偏好）后依次运行三组测试，并打印一行汇总。
    void RunFacadeTests();
}
