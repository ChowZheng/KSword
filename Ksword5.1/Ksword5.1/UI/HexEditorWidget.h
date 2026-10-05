#pragma once

// ============================================================
// HexEditorWidget.h
// 作用：
// - 统一十六进制查看/编辑控件的"门面"：类名与公开 API 沿用旧控件，全仓调用点无需改动；
// - 内部全部转发给 ks::ui::HexView（UI/MemoryWorkbench/HexView.h，自绘虚拟化画布 + 工具栏 + 查找/跳转条
//   + 状态条 + 解释器面板），旧的 QTableWidget 单元格实现已整体退役；
// - 后续所有"显示字节数据"的界面仍然复用本控件。
//
// ------------------------------------------------------------
// 一、本头文件刻意不包含任何新组件头文件
// ------------------------------------------------------------
// - 只前向声明 ks::ui::HexView 并持有指针：本头文件被十几处宿主包含（含 MemoryDock.Internal.h /
//   NetworkDock.InternalCommon.h 这类扇出很大的头），tools/Invoke-MemoryEditorUiTests.ps1 还会用
//   /std:c++17 编译包含本头文件的测试，不能让它们被迫重编 HexView 的整条依赖链；
// - 旧头文件里的私有成员（表格、查找状态、选区状态、一大堆 Qt 控件的前向声明）全部删除。
//
// ------------------------------------------------------------
// 二、与旧控件的语义对照（离屏夹具 tools/memwb_ui/memwb_ui_tests.Facade*.cpp 逐条钉死）
// ------------------------------------------------------------
// - 默认：只读、每行 16 字节、工具栏与状态条显示、解释器面板隐藏（由 HexView 持久化其显隐）。
// - setByteArray(bytes, base)：整块替换内容。基址与长度都与上一份相同时，保留上一次的变更参照
//   （旧控件的行为，discardChanges 之类的回写路径依赖它）；基址或长度变了则参照作废。
// - setChangeReferences(original, previousRead)：尺寸与缓冲不符的参照一律当作"没有"（旧规则：不着色）。
//   参数与上一次实际生效的相同时 O(1) 返回——宿主在每次 byteEdited 之后都会重复调用它。
// - setByteAtAbsoluteAddress(addr, value, keepSelection)：不发 byteEdited（回滚路径依赖），越界返回 false。
//   注意 keepSelection 的真实旧语义与参数名相反：旧实现里它为 true 才把当前单元格移到被改的字节上
//   （KvmHookWizard 逐字节写补丁时只对最后一个字节传 true，让焦点落在补丁末尾），为 false 不动当前选择。
//   这里保持旧实现的行为，不按参数名"纠正"。
// - setBytesPerRow(n)：旧控件夹取到 [4, 64]；新画布只支持 8/16/32/48/64，所以先夹取到 [4, 64]，
//   再取最近的受支持值（等距时取较大者）。宿主传 16 / 32 照常；bytesPerRow() 返回实际生效值。
// - selectedAbsoluteAddress()：有数据时是插入点的绝对地址（新画布恒有插入点），无数据时是基址；
//   selectedOffset() 是它相对基址的偏移，无数据为 0。
// - jumpToAbsoluteAddress(addr)：范围内选中该字节并居中，返回 true；范围外 / 无数据返回 false。
// - 信号：byteEdited 仅在用户编辑使缓冲里的字节真的变了时发出，每个变化字节恰好一次
//   （setByteArray / setByteAtAbsoluteAddress 不发）；currentAddressChanged 对应插入点移动；
//   selectionChanged(起偏移, 止偏移（不含）, 是否有选区)；aboutToShowContextMenu 在右键菜单弹出前发出。
// - setHexOnlyView(true)：隐藏 HexView 的状态条（统一编辑器自己有状态条），工具栏保留；false 恢复。
//
// ------------------------------------------------------------
// 三、已知的、有意的行为差异
// ------------------------------------------------------------
// - 用户编辑过的字节恒显示为橙色"待提交"（HexView 的模型：叠加层补丁 == 缓冲与基线的差异），
//   即使宿主从未设置过变更参照；旧控件没有参照时不着色。clearChangeHighlights() 之后继续编辑同样会显示橙色。
// - 旧控件对"original 尺寸不符但 previousRead 尺寸相符"的组合仍会按 previousRead 着冷色；
//   现在 original 不符就整体不着色。全仓唯一的调用者（MemoryEditorWidget）总是同时传尺寸相符的两者，不受影响。
// - 旧的 ASCII 页签、选区检查器面板、摘要标签已由 HexView 自带的 ASCII 面板、解释器面板与状态条取代。
// ============================================================

#include <QByteArray>
#include <QWidget>

#include <cstddef>
#include <cstdint>

class QMenu;

namespace ks::ui
{
    class HexView;
}

// HexEditorWidget：
// - 统一十六进制查看器（门面）；
// - 支持只读和可编辑两种模式；
// - 内部由 ks::ui::HexView 承担全部显示、编辑、查找（Ctrl+F）、跳转（Ctrl+G）与导出。
class HexEditorWidget final : public QWidget
{
    Q_OBJECT

public:
    // 构造函数：
    // - parent：Qt 父控件，可空；
    // - 内部创建一个铺满本控件（布局边距为 0）的 HexView，默认只读、每行 16 字节。
    explicit HexEditorWidget(QWidget* parent = nullptr);

    // 析构函数：
    // - 内部 HexView 是本控件的子控件，随 Qt 对象树销毁；
    // - HexView 自己负责取消在途的后台查找，因此在查找进行中销毁本控件是安全的。
    ~HexEditorWidget() override;

    // setHexOnlyView：
    // - 作用：嵌入统一内存编辑器时隐藏重复的状态条（统一编辑器自己有状态条），保留工具栏；
    // - enabled：true 隐藏状态条；false 恢复显示（其它独立使用者维持默认外观）。
    void setHexOnlyView(bool enabled);

    // setByteArray：
    // - 作用：按 QByteArray 整块设置显示数据；
    // - bytes：输入字节数据；
    // - baseAddress：逻辑起始地址；
    // - 清选区、清查找状态；基址与长度都不变时保留上一次的变更参照，否则参照作废。
    void setByteArray(const QByteArray& bytes, std::uint64_t baseAddress = 0);

    // setChangeReferences：
    // - 作用：设置变更着色参照；尺寸与缓冲不符的参照被视为"没有"（不着色）；
    // - original：原始字节，缓冲里与它不同的字节显示为橙色（待提交）；
    // - previousRead：上一次读取，与 original 不同的字节显示为冷色（外部变化），可为空；
    // - 与上一次实际生效的参照完全相同时直接返回，不重建任何显示状态。
    void setChangeReferences(const QByteArray& original, const QByteArray& previousRead = QByteArray());

    // clearChangeHighlights：
    // - 作用：清除变更参照，橙色与冷色着色全部消失（缓冲内容不变）。
    void clearChangeHighlights();

    // clearData：
    // - 作用：清空缓冲、选区与查找高亮；基址保留（与旧控件一致）。
    void clearData();

    // setEditable：
    // - 作用：设置是否允许编辑；该设置跨 setByteArray / clearData 保持；
    // - editable：true=可编辑，false=只读。
    void setEditable(bool editable);

    // isEditable：
    // - 返回当前是否可编辑。
    bool isEditable() const;

    // setBytesPerRow：
    // - 作用：设置每行显示字节数；
    // - bytesPerRow：先夹取到 [4, 64]，再取最近的受支持值（8/16/32/48/64，等距取较大者）。
    void setBytesPerRow(int bytesPerRow);

    // bytesPerRow：
    // - 返回当前实际生效的每行字节数。
    int bytesPerRow() const;

    // jumpToAbsoluteAddress：
    // - 作用：选中并滚动到绝对地址；
    // - absoluteAddress：目标地址；
    // - 返回：true=成功定位，false=超范围或无数据（状态条会给出提示）。
    bool jumpToAbsoluteAddress(std::uint64_t absoluteAddress);

    // openFindPanel：
    // - 作用：显示查找条并聚焦输入框。
    void openFindPanel();

    // openJumpPanel：
    // - 作用：显示跳转条并聚焦输入框。
    void openJumpPanel();

    // setByteAtAbsoluteAddress：
    // - 作用：外部主动修改指定地址字节（例如写入失败时回滚），不发 byteEdited；
    // - absoluteAddress：绝对地址；
    // - byteValue：目标字节值；
    // - keepSelection：为 true 时把插入点移到被改的字节上并滚动到可见（旧实现的真实行为，见文件头第二节）；
    //   为 false 时不动当前选择；
    // - 返回：true=修改成功，false=地址越界或暂存超限。
    bool setByteAtAbsoluteAddress(
        std::uint64_t absoluteAddress,
        std::uint8_t byteValue,
        bool keepSelection = true);

    // data：
    // - 返回当前组件持有的数据（含已应用的编辑；QByteArray 隐式共享，不是深拷贝）。
    QByteArray data() const;

    // regionSize：
    // - 返回当前数据长度（字节）。
    std::size_t regionSize() const;

    // baseAddress：
    // - 返回当前逻辑基址。
    std::uint64_t baseAddress() const;

    // selectedAbsoluteAddress：
    // - 返回插入点（当前选中字节）对应的绝对地址；
    // - 无数据时返回 baseAddress。
    std::uint64_t selectedAbsoluteAddress() const;

    // selectedOffset：
    // - 返回插入点相对基址的字节偏移；
    // - 无数据时返回 0。
    std::uint64_t selectedOffset() const;

signals:
    // byteEdited：
    // - 作用：用户编辑使缓冲里的字节真的变了时触发，每个变化字节恰好一次；
    // - absoluteAddress：被修改字节的绝对地址；
    // - oldValue：修改前字节；
    // - newValue：修改后字节。
    void byteEdited(
        std::uint64_t absoluteAddress,
        std::uint8_t oldValue,
        std::uint8_t newValue);

    // currentAddressChanged：
    // - 作用：插入点（当前选中字节）移动时触发；
    // - absoluteAddress：新的插入点绝对地址。
    void currentAddressChanged(std::uint64_t absoluteAddress);

    // selectionChanged：
    // - 作用：选区变化时触发，让外部感知"选了哪一段字节"；
    // - startOffset：选区最小偏移（包含，相对基址），无选区时为 0；
    // - endOffset：选区最大偏移加一（不包含，相对基址），无选区时为 0；
    // - hasSelection：true 表示当前存在有效选区，false 表示无数据（没有选区）。
    void selectionChanged(
        std::uint64_t startOffset,
        std::uint64_t endOffset,
        bool hasSelection);

    // aboutToShowContextMenu：
    // - 作用：在弹出右键菜单前允许外部追加动作；
    // - menu：即将显示的菜单对象；
    // - absoluteAddress：当前菜单目标地址；
    // - hasByte：true 表示目标地址有有效字节。
    void aboutToShowContextMenu(
        QMenu* menu,
        std::uint64_t absoluteAddress,
        bool hasByte);

private:
    // applyStoredReference：
    // - 作用：把 m_referenceOriginal / m_referencePrevious 应用到 HexView；
    // - 参照为空时清除 HexView 的参照；HexView 拒绝参照（暂存超限等）时同步清空本地记录，
    //   保证"本地记录 == HexView 里实际生效的参照"这一不变式。
    void applyStoredReference();

private:
    // m_view：内部的 HexView，本控件的子控件，生命周期由 Qt 对象树管理。
    ks::ui::HexView* m_view = nullptr;

    // m_referenceOriginal：HexView 里当前生效的 original 参照；空表示没有生效的参照。
    QByteArray m_referenceOriginal;

    // m_referencePrevious：HexView 里当前生效的 previousRead 参照；空表示没有。
    QByteArray m_referencePrevious;
};
