// ============================================================
// HexEditorWidget.cpp
// 作用：
// - 实现 HexEditorWidget 门面：构造时在内部建一个 ks::ui::HexView 并铺满，
//   公开 API 逐个转发给它，信号原样转发回来；
// - 唯一有自己状态的地方是"变更参照"的本地记录（m_referenceOriginal / m_referencePrevious），
//   用来复刻旧控件的两条行为：setByteArray 在基址与长度都不变时保留参照、
//   setChangeReferences 在参数与上次相同时不做任何事；
// - 本文件不依赖任何主程序专有头文件（Framework.h 等），只依赖 Qt 与 HexView，
//   因此可以被离屏夹具 tools/memwb_ui 单独编译。
// 语义对照与已知差异见 HexEditorWidget.h 文件头。
// ============================================================

#include "HexEditorWidget.h"

#include "MemoryWorkbench/HexView.h"

#include <QMenu>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdlib>

namespace
{
    // kLegacyMinBytesPerRow / kLegacyMaxBytesPerRow：
    // - 旧控件对每行字节数的夹取区间 [4, 64]；
    // - 保留它，是为了让宿主传入的任何整数都有确定的结果。
    constexpr int kLegacyMinBytesPerRow = 4;
    constexpr int kLegacyMaxBytesPerRow = 64;

    // kSupportedBytesPerRow：
    // - 新画布实际支持的行宽，按升序排列；
    // - SnapBytesPerRow 在这几个值里取最近者。
    constexpr int kSupportedBytesPerRow[] = { 8, 16, 32, 48, 64 };

    // SnapBytesPerRow：
    // - 作用：把宿主请求的每行字节数换算成画布支持的行宽；
    // - 调用方式：setBytesPerRow 内部使用；
    // - 传入 requested：宿主请求值，可为任意整数；
    // - 传出：8/16/32/48/64 之一。先夹取到 [4, 64]，再取距离最近者，等距时取较大者。
    int SnapBytesPerRow(const int requested)
    {
        // clamped：与旧控件相同的夹取结果，保证后面的距离比较有界。
        const int clamped = std::clamp(requested, kLegacyMinBytesPerRow, kLegacyMaxBytesPerRow);

        // 在升序数组里找最近值；"<="让等距时后面（较大）的候选胜出。
        int best = kSupportedBytesPerRow[0];
        int bestDistance = std::abs(clamped - best);
        for (const int candidate : kSupportedBytesPerRow)
        {
            const int distance = std::abs(clamped - candidate);
            if (distance <= bestDistance)
            {
                best = candidate;
                bestDistance = distance;
            }
        }
        return best;
    }

    // SameBytes：
    // - 作用：判断两个字节数组内容是否相同；
    // - 调用方式：setChangeReferences 比较"这次的参照"与"已生效的参照"；
    // - 传入：两个 QByteArray；
    // - 传出：相同为 true。先比共享句柄（O(1)，宿主反复传同一份快照时命中），再比内容。
    bool SameBytes(const QByteArray& left, const QByteArray& right)
    {
        return left.isSharedWith(right) || left == right;
    }
}

HexEditorWidget::HexEditorWidget(QWidget* parent)
    : QWidget(parent)
{
    // 根布局：边距与间距都为 0，让内部 HexView 铺满本控件。
    QVBoxLayout* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // 内部 HexView：默认只读、每行 16 字节、工具栏与状态条显示、解释器面板按用户偏好。
    m_view = new ks::ui::HexView(this);
    rootLayout->addWidget(m_view, 1);

    // 焦点代理：宿主对本控件 setFocus 时，焦点交给 HexView（它再交给画布）。
    setFocusProxy(m_view);

    // 信号转发：HexView 的信号与旧控件的信号参数完全一致（类型都是 64 位地址与字节），
    // 直接信号到信号连接即可，不引入额外的转换层。
    connect(m_view, &ks::ui::HexView::byteEdited, this, &HexEditorWidget::byteEdited);
    connect(m_view, &ks::ui::HexView::caretMoved, this, &HexEditorWidget::currentAddressChanged);
    connect(m_view, &ks::ui::HexView::selectionChanged, this, &HexEditorWidget::selectionChanged);
    connect(m_view, &ks::ui::HexView::aboutToShowContextMenu, this, &HexEditorWidget::aboutToShowContextMenu);
}

HexEditorWidget::~HexEditorWidget() = default;

void HexEditorWidget::setHexOnlyView(const bool enabled)
{
    // 统一编辑器自己有状态条：嵌入时隐藏 HexView 的状态条，避免重复；工具栏保留。
    m_view->setStatusBarVisible(!enabled);
}

void HexEditorWidget::setByteArray(const QByteArray& bytes, const std::uint64_t baseAddress)
{
    // sameShape：基址与长度都和当前缓冲相同。旧控件在这种情况下保留变更参照，
    // 不少回写路径（放弃修改、撤销）正是靠它在换回原始内容后继续显示着色。
    const bool sameShape = baseAddress == m_view->baseAddress()
        && static_cast<std::uint64_t>(bytes.size()) == m_view->bufferSize();

    // 整块替换内容：HexView 会清选区、清暂存、清查找状态，并丢弃它自己的参照。
    m_view->setBuffer(baseAddress, bytes);

    // 形状变了：本地记录同步作废；形状没变：把保留的参照重新应用到新内容上。
    if (!sameShape)
    {
        m_referenceOriginal.clear();
        m_referencePrevious.clear();
        return;
    }
    if (!m_referenceOriginal.isEmpty())
    {
        applyStoredReference();
    }
}

void HexEditorWidget::setChangeReferences(const QByteArray& original, const QByteArray& previousRead)
{
    // size：当前缓冲长度；参照必须与它等长才有意义。
    const qsizetype size = static_cast<qsizetype>(m_view->bufferSize());

    // baseline / preceding：尺寸不符的一律当作"没有"（旧规则：不着色）。
    // original 无效时 previousRead 也一并忽略——着色必须以 original 为基线。
    const QByteArray baseline = (original.size() == size) ? original : QByteArray();
    const QByteArray preceding = (!baseline.isEmpty() && previousRead.size() == size)
        ? previousRead
        : QByteArray();

    // 与已生效的参照完全相同：什么都不做。宿主在每次 byteEdited 之后都会重复调用本函数，
    // 这里必须是 O(1)（共享句柄命中），不能每次都重建 HexView 的基线与叠加层。
    if (SameBytes(baseline, m_referenceOriginal) && SameBytes(preceding, m_referencePrevious))
    {
        return;
    }

    m_referenceOriginal = baseline;
    m_referencePrevious = preceding;
    applyStoredReference();
}

void HexEditorWidget::clearChangeHighlights()
{
    // 本来就没有生效的参照：与旧控件一样直接返回，不动用户已有的编辑显示。
    if (m_referenceOriginal.isEmpty() && m_referencePrevious.isEmpty())
    {
        return;
    }
    m_referenceOriginal.clear();
    m_referencePrevious.clear();
    applyStoredReference();
}

void HexEditorWidget::applyStoredReference()
{
    // 没有参照：让 HexView 的基线回到当前缓冲，橙色与冷色全部消失。
    if (m_referenceOriginal.isEmpty())
    {
        m_referencePrevious.clear();
        m_view->clearReference();
        return;
    }

    // 有参照：交给 HexView 着色。被拒绝（尺寸不符、暂存超限）时 HexView 已自行清除参照，
    // 本地记录跟着清空，保持"记录 == 实际生效的参照"。
    if (!m_view->setReference(m_referenceOriginal, m_referencePrevious))
    {
        m_referenceOriginal.clear();
        m_referencePrevious.clear();
    }
}

void HexEditorWidget::clearData()
{
    // 清空缓冲但保留基址（旧控件语义）；复用 setByteArray 以同步作废参照记录。
    setByteArray(QByteArray(), m_view->baseAddress());
}

void HexEditorWidget::setEditable(const bool editable)
{
    m_view->setEditable(editable);
}

bool HexEditorWidget::isEditable() const
{
    return m_view->isEditable();
}

void HexEditorWidget::setBytesPerRow(const int bytesPerRow)
{
    // 先换算成画布支持的行宽；换算结果恒合法，HexView 的返回值无需再检查。
    m_view->setBytesPerRow(SnapBytesPerRow(bytesPerRow));
}

int HexEditorWidget::bytesPerRow() const
{
    return m_view->bytesPerRow();
}

bool HexEditorWidget::jumpToAbsoluteAddress(const std::uint64_t absoluteAddress)
{
    // 范围内选中并居中；范围外或无数据时 HexView 会在状态条给出提示并返回 false。
    return m_view->jumpToAddress(absoluteAddress);
}

void HexEditorWidget::openFindPanel()
{
    m_view->openFind();
}

void HexEditorWidget::openJumpPanel()
{
    m_view->openGoto();
}

bool HexEditorWidget::setByteAtAbsoluteAddress(
    const std::uint64_t absoluteAddress,
    const std::uint8_t byteValue,
    const bool keepSelection)
{
    // 静默改字节：缓冲与画面立即更新，不发 byteEdited（宿主的回滚路径依赖这一点）。
    if (!m_view->setByteQuiet(absoluteAddress, byteValue))
    {
        return false;
    }

    // 旧实现的真实行为：keepSelection 为 true 时才把当前单元格移到被改字节上（并保证可见），
    // 为 false 时不动当前选择。参数名与行为相反是历史遗留，这里保持行为不变，见头文件第二节。
    if (keepSelection)
    {
        m_view->canvas()->setCaretAddress(absoluteAddress, false, true);
    }
    return true;
}

QByteArray HexEditorWidget::data() const
{
    return m_view->buffer();
}

std::size_t HexEditorWidget::regionSize() const
{
    return static_cast<std::size_t>(m_view->bufferSize());
}

std::uint64_t HexEditorWidget::baseAddress() const
{
    return m_view->baseAddress();
}

std::uint64_t HexEditorWidget::selectedAbsoluteAddress() const
{
    // 新画布有数据时恒有插入点；无数据（旧控件的"未选中有效字节"）时按旧语义回退为基址。
    if (m_view->bufferSize() == 0)
    {
        return m_view->baseAddress();
    }
    return m_view->caretAddress();
}

std::uint64_t HexEditorWidget::selectedOffset() const
{
    // 无数据时为 0；否则是插入点相对基址的偏移（先判空再相减，不会回绕）。
    if (m_view->bufferSize() == 0)
    {
        return 0;
    }
    return m_view->caretAddress() - m_view->baseAddress();
}
