#include "WriteModeSwitch.h"

// ============================================================
// WriteModeSwitch.cpp
// 作用：见头文件。paintEvent 只画胶囊外框/分隔线/高亮，不画图标——图标由两个
// 真实 QToolButton 承担，这样主程序启动时的全局 SVG 主题着色能自动识别到它们。
// ============================================================

#include "WorkbenchMessages.h"

#include "../../theme.h"

#include <QEvent>
#include <QIcon>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>

namespace ks::ui
{
    using ksword::memwb::WriteMode;

    namespace
    {
        // kCapsuleHeight：胶囊固定高度，与会话条其它分段按钮保持一致的紧凑尺寸。
        constexpr int kCapsuleHeight = 28;
        // kButtonInset：按钮离胶囊边框的内边距。
        constexpr int kButtonInset = 3;
    }

    WriteModeSwitch::WriteModeSwitch(QWidget* parent)
        : QWidget(parent)
    {
        // Tab 可达但鼠标点击不抢焦点，与仓库里其它自绘分段按钮的焦点策略一致。
        setFocusPolicy(Qt::TabFocus);
        setToolTip(workbench_messages::WriteModeTooltip());

        // 两个图标按钮只负责显示图标与接收点击，胶囊底色由本控件的 paintEvent 画，
        // 因此按钮本身必须是扁平、无边框，否则会在高亮底色上方再画一层按钮底板。
        m_immediateButton = new QToolButton(this);
        m_immediateButton->setIcon(QIcon(QStringLiteral(":/Icon/memwb_mode_immediate.svg")));
        m_immediateButton->setAutoRaise(true);
        m_immediateButton->setFocusPolicy(Qt::NoFocus);
        m_immediateButton->setToolTip(workbench_messages::WriteModeTooltip());
        KswordTheme::ApplyCompactIconButtonMetrics(m_immediateButton);
        connect(m_immediateButton, &QToolButton::clicked, this, [this]() {
            requestToggleToOtherSide(WriteMode::Immediate);
        });

        m_stagedButton = new QToolButton(this);
        m_stagedButton->setIcon(QIcon(QStringLiteral(":/Icon/memwb_mode_staged.svg")));
        m_stagedButton->setAutoRaise(true);
        m_stagedButton->setFocusPolicy(Qt::NoFocus);
        m_stagedButton->setToolTip(workbench_messages::WriteModeTooltip());
        KswordTheme::ApplyCompactIconButtonMetrics(m_stagedButton);
        connect(m_stagedButton, &QToolButton::clicked, this, [this]() {
            requestToggleToOtherSide(WriteMode::StagedThenApply);
        });

        layoutButtons();
    }

    WriteMode WriteModeSwitch::mode() const
    {
        return m_mode;
    }

    void WriteModeSwitch::setMode(const WriteMode mode)
    {
        if (m_mode == mode)
        {
            return;
        }
        m_mode = mode;
        update();
    }

    QSize WriteModeSwitch::sizeHint() const
    {
        // 两个紧凑按钮并排加上左右内边距与中间分隔的估算宽度。
        const QSize buttonSize = KswordTheme::CompactIconButtonSize();
        const int width = buttonSize.width() * 2 + kButtonInset * 3;
        return QSize(width, kCapsuleHeight);
    }

    void WriteModeSwitch::requestToggleToOtherSide(const WriteMode clickedSide)
    {
        // 点的就是当前已高亮的那一半，不构成"切换请求"。
        if (clickedSide == m_mode)
        {
            return;
        }
        emit modeToggleRequested(clickedSide);
    }

    void WriteModeSwitch::keyPressEvent(QKeyEvent* event)
    {
        if (event != nullptr && event->key() == Qt::Key_Space)
        {
            const WriteMode other = (m_mode == WriteMode::Immediate)
                ? WriteMode::StagedThenApply
                : WriteMode::Immediate;
            emit modeToggleRequested(other);
            event->accept();
            return;
        }
        QWidget::keyPressEvent(event);
    }

    void WriteModeSwitch::changeEvent(QEvent* event)
    {
        QWidget::changeEvent(event);
        // 同 WorkbenchSessionBar::changeEvent 的 N7 修复（第二轮修复排查同类问题时
        // 顺带发现）：QEvent::ApplicationPaletteChange 是发给 QApplication 自己的
        // 事件，QWidget::event 不会把它转发成子控件的 changeEvent，这条分支过去
        // 一直是死代码——胶囊高亮色在 paintEvent 里现取 theme.h 静态颜色没错，但
        // 主题切换后根本没人调 update() 让它重绘，颜色会停在旧主题直到控件因为
        // 别的原因（resize、父级重绘）重画一次。改听 QEvent::PaletteChange，并带上
        // StyleChange，与仓库其它监听主题变化的控件写法一致。
        if (event != nullptr
            && (event->type() == QEvent::PaletteChange
                || event->type() == QEvent::ApplicationPaletteChange
                || event->type() == QEvent::StyleChange))
        {
            update();
        }
    }

    void WriteModeSwitch::resizeEvent(QResizeEvent* event)
    {
        QWidget::resizeEvent(event);
        layoutButtons();
    }

    void WriteModeSwitch::layoutButtons()
    {
        if (m_immediateButton == nullptr || m_stagedButton == nullptr)
        {
            return;
        }
        const int halfWidth = (width() - kButtonInset * 3) / 2;
        const int buttonHeight = height() - kButtonInset * 2;
        m_immediateButton->setGeometry(kButtonInset, kButtonInset, halfWidth, buttonHeight);
        m_stagedButton->setGeometry(
            kButtonInset * 2 + halfWidth, kButtonInset, halfWidth, buttonHeight);
    }

    void WriteModeSwitch::paintEvent(QPaintEvent* /*event*/)
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const QRectF outer(0.5, 0.5, width() - 1.0, height() - 1.0);
        const qreal radius = outer.height() / 2.0;

        // 外框：底色用 SurfaceAlt，边线用 Border，两者都现取静态颜色，不烧死在构造期。
        QPainterPath capsulePath;
        capsulePath.addRoundedRect(outer, radius, radius);
        painter.fillPath(capsulePath, KswordTheme::SurfaceAltColor());
        painter.setPen(QPen(KswordTheme::BorderColor(), 1.0));
        painter.drawPath(capsulePath);

        // 高亮：当前模式对应的那一半画强调色的浅底，和分段按钮的选中态视觉呼应。
        const bool immediateSide = (m_mode == WriteMode::Immediate);
        const qreal halfWidth = outer.width() / 2.0;
        const QRectF highlightRect = immediateSide
            ? QRectF(outer.left(), outer.top(), halfWidth, outer.height())
            : QRectF(outer.left() + halfWidth, outer.top(), halfWidth, outer.height());

        painter.save();
        painter.setClipPath(capsulePath);
        painter.fillRect(highlightRect, KswordTheme::PrimaryBlueSubtleColor());
        painter.restore();

        // 分隔线：胶囊正中的一条竖线，帮助区分两个按钮的点击区域。
        painter.setPen(QPen(KswordTheme::BorderColor(), 1.0));
        const qreal centerX = outer.left() + halfWidth;
        painter.drawLine(QPointF(centerX, outer.top() + 2.0), QPointF(centerX, outer.bottom() - 2.0));
    }
}
