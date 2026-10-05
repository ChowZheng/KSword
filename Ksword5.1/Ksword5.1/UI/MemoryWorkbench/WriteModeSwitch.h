#pragma once

// ============================================================
// WriteModeSwitch.h
// 作用：
// - 会话条里的写入模式胶囊开关：左半边"立即写入"、右半边"暂存后应用"，当前
//   选中的半边画高亮底色；点击另一半或按空格键都请求切换到那一半。
// - 本控件自己不持有"能不能切"的业务判断（有暂存补丁时要不要弹三选一）——
//   它只负责"用户想切到哪个模式"这一个请求信号，真正的模式由调用方通过
//   setMode() 回写；调用方拒绝切换时不回写，胶囊会保持原来高亮的那一半，
//   从用户角度看就是"切换被挡住了"。
// - 两个图标用真实 qrc 资源（memwb_mode_immediate / memwb_mode_staged）画在
//   两个内部 QToolButton 上，这样能复用 SvgThemeIconManager 的全局主题着色
//   （它只认事件来源是 QAbstractButton 的图标槽位，自绘 QPainter 画的图形它认
//   不出来）；本控件自己只画胶囊外框、分隔线与选中高亮，颜色现取 theme.h。
// ============================================================

#include <QWidget>

#include "../../../../shared/evidence/memory_workbench/MemoryWriteTransaction.h"

class QToolButton;

namespace ks::ui
{
    // WriteModeSwitch：写入模式胶囊开关。
    class WriteModeSwitch final : public QWidget
    {
        Q_OBJECT

    public:
        explicit WriteModeSwitch(QWidget* parent = nullptr);

        // mode：当前显示（高亮）的模式，由 setMode 设置，不代表已经生效的业务状态。
        ksword::memwb::WriteMode mode() const;

        // setMode：外部确认模式之后调用，纯粹改变高亮的那一半，不发任何信号。
        void setMode(ksword::memwb::WriteMode mode);

        // sizeHint：按两个图标按钮加内边距估算的紧凑尺寸。
        QSize sizeHint() const override;

    signals:
        // modeToggleRequested：用户点击了非当前模式的那一半，或按空格切换；
        // 参数是用户想切到的模式。调用方决定是否真的调用 setMode 回写。
        void modeToggleRequested(ksword::memwb::WriteMode requestedMode);

    protected:
        // paintEvent：画胶囊外框、分隔线与当前模式一侧的高亮底色。
        void paintEvent(QPaintEvent* event) override;

        // keyPressEvent：空格键请求切换到另一侧。
        void keyPressEvent(QKeyEvent* event) override;

        // changeEvent：主题（调色板）变化时请求重绘，因为高亮底色是现取的静态主题色。
        void changeEvent(QEvent* event) override;

        // resizeEvent：重新摆放两个内部按钮的位置。
        void resizeEvent(QResizeEvent* event) override;

    private:
        // requestToggleToOtherSide：点了非当前模式的那一半时调用，发出请求信号。
        void requestToggleToOtherSide(ksword::memwb::WriteMode clickedSide);

        // layoutButtons：按当前控件尺寸把两个按钮摆到胶囊左右两半。
        void layoutButtons();

        // m_mode：当前高亮显示的模式。
        ksword::memwb::WriteMode m_mode = ksword::memwb::WriteMode::Immediate;
        // m_immediateButton / m_stagedButton：两个图标按钮，分别代表左右两半。
        QToolButton* m_immediateButton = nullptr;
        QToolButton* m_stagedButton = nullptr;
    };
}
