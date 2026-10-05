#pragma once

#include <QColor>
#include <QWidget>

namespace ks::ui
{
    // ThemePreviewWidget 绘制待应用配色的缩略界面，不创建业务页面或执行按钮动作。
    // 由设置页持有；种子复用生产主题计算，导航/表格可点击查看选中态。
    class ThemePreviewWidget final : public QWidget
    {
    public:
        // parent 接管生命周期；初始使用默认浅色，设置页随后传入当前待应用配置。
        explicit ThemePreviewWidget(QWidget* parent = nullptr);

        // setPreview 接收深浅模式和两个 #RRGGBB 种子；空字符串表示恢复默认。
        // 只请求重绘，不应用主题、不保存配置、不发送业务事件。
        void setPreview(bool darkMode, const QString& accentColor, const QString& backgroundColor);

        // 尺寸按可用宽度和实际字体计算；窄窗把编辑器与通知改为上下排列。
        QSize sizeHint() const override;
        QSize minimumSizeHint() const override;
        int heightForWidth(int width) const override;

    protected:
        // 绘制生产颜色角色组成的导航、按钮/图标、表格、代码和通知缩略图。
        void paintEvent(QPaintEvent* event) override;
        // 主题/语言/字体变化只更新预览，跟随系统模式由设置页重新传入。
        void changeEvent(QEvent* event) override;
        // 鼠标仅改变样例状态；释放/离开后移除按下与悬停效果。
        void mouseMoveEvent(QMouseEvent* event) override;
        void mousePressEvent(QMouseEvent* event) override;
        void mouseReleaseEvent(QMouseEvent* event) override;
        void leaveEvent(QEvent* event) override;

    private:
        bool m_darkMode = false; // 待应用的主题模式。
        QColor m_accentColor; // 未应用的主体色种子，无效为默认。
        QColor m_backgroundColor; // 未应用的背景种子，无效为默认。
        int m_selectedTab = 0; // 缩略导航当前选中项。
        int m_selectedRow = 0; // 缩略表格当前选中行。
        int m_hoveredTab = -1; // 鼠标所在导航项，-1表示无。
        bool m_buttonHovered = false; // 样例按钮悬停状态。
        bool m_buttonPressed = false; // 样例按钮左键按下状态。
    };
}
