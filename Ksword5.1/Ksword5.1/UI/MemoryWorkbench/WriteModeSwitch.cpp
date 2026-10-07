#include "WriteModeSwitch.h"

// ============================================================
// WriteModeSwitch.cpp
// 作用：见头文件。paintEvent 只画胶囊外框/分隔线/高亮，不画图标——图标由两个
// 真实 QToolButton 承担，这样主程序启动时的全局 SVG 主题着色能自动识别到它们。
// ============================================================

#include "WorkbenchMessages.h"

#include "../../theme.h"

#include <QEvent>
#include <QFontMetrics>
#include <QIcon>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>

#include <algorithm>

namespace ks::ui
{
    using ksword::memwb::WriteMode;

    namespace
    {
        // kCapsuleHeight：胶囊固定高度，与会话条其它分段按钮保持一致的紧凑尺寸。
        constexpr int kCapsuleHeight = 28;
        // kButtonInset：按钮离胶囊边框的内边距。
        constexpr int kButtonInset = 3;
        // kHalfPadding：每一半里图标/文字离左右边缘的内边距。
        constexpr int kHalfPadding = 8;
        // kIconTextGap：图标与文字之间的间距。
        constexpr int kIconTextGap = 4;

        // CapsuleHalfButton：胶囊里的一半按钮，自己画"图标 + 文字"，完全不经过 QSS 样式。
        // 为什么自绘：主程序全局样式表给所有 QToolButton 铺了带边框、悬停整块填强调色的
        // `!important` 规则；胶囊底色/高亮由外层 WriteModeSwitch 画，这里如果再让样式引擎画
        // 一层按钮底板，左半边就会在真窗口里变成一块溢出胶囊的蓝色方块（真机反馈的 BUG）。
        // 自绘后不管全局规则怎么写都碰不到它；图标仍是 QToolButton 的图标槽位，
        // 所以 SvgThemeIconManager 的全局主题着色照常认得出来。
        //
        // "当前模式"用按钮的 checked 状态表示（由 WriteModeSwitch::setMode 设置）。点击不会自己
        // 翻转 checked：切换请不请得动是调用方（可能弹三选一、可能拒绝）说了算，高亮只能由
        // setMode 回写，所以 nextCheckState 什么都不做，clicked 信号照常发出。
        class CapsuleHalfButton final : public QToolButton
        {
        public:
            explicit CapsuleHalfButton(QWidget* parent) : QToolButton(parent)
            {
                setCheckable(true);
            }

        protected:
            // nextCheckState：点击时本来会翻转 checked，这里故意不翻转，见类注释。
            void nextCheckState() override {}

            // paintEvent：图标 + 文字水平居中；当前模式用主文字色，另一半用次文字色，悬停时提亮。
            void paintEvent(QPaintEvent* /*event*/) override
            {
                QPainter painter(this);
                painter.setRenderHint(QPainter::Antialiasing);
                painter.setFont(font());

                const QFontMetrics metrics(font());
                const QSize iconPixelSize = iconSize();
                // availableTextWidth：扣掉左右内边距与图标之后文字最多能占的宽度，放不下就右省略。
                const int availableTextWidth =
                    (std::max)(0, width() - kHalfPadding * 2 - iconPixelSize.width() - kIconTextGap);
                const QString shownText = metrics.elidedText(text(), Qt::ElideRight, availableTextWidth);
                const int textWidth = metrics.horizontalAdvance(shownText);
                const int contentWidth = iconPixelSize.width() + kIconTextGap + textWidth;
                const int left = (std::max)(kHalfPadding / 2, (width() - contentWidth) / 2);

                const QIcon::Mode iconMode = isEnabled() ? QIcon::Normal : QIcon::Disabled;
                icon().paint(
                    &painter,
                    QRect(left, (height() - iconPixelSize.height()) / 2, iconPixelSize.width(), iconPixelSize.height()),
                    Qt::AlignCenter,
                    iconMode);

                const bool active = isChecked();
                QColor textColor = (active || underMouse())
                    ? KswordTheme::TextPrimaryColor()
                    : KswordTheme::TextSecondaryColor();
                if (!isEnabled())
                {
                    textColor = KswordTheme::TextDisabledColor();
                }
                painter.setPen(textColor);
                painter.drawText(
                    QRect(left + iconPixelSize.width() + kIconTextGap, 0, textWidth + 2, height()),
                    Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine,
                    shownText);
            }
        };
    }

    WriteModeSwitch::WriteModeSwitch(QWidget* parent)
        : QWidget(parent)
    {
        // Tab 可达但鼠标点击不抢焦点，与仓库里其它自绘分段按钮的焦点策略一致。
        setFocusPolicy(Qt::TabFocus);
        setToolTip(workbench_messages::WriteModeTooltip());

        // 两个半边按钮只负责"图标 + 文字"的显示与接收点击，胶囊底色由本控件的 paintEvent 画，
        // 按钮自己也是自绘的（见 CapsuleHalfButton），不会被全局 QToolButton 样式画出底板/边框。
        // 文字（即时/暂存）与逐半边的悬停说明让用户不必猜图标含义；图标尺寸沿用紧凑图标尺寸，
        // 但不再用 ApplyCompactIconButtonMetrics 把按钮钉成 28x28（那会把文字挤没）。
        m_immediateButton = new CapsuleHalfButton(this);
        m_immediateButton->setIcon(QIcon(QStringLiteral(":/Icon/memwb_mode_immediate.svg")));
        m_immediateButton->setIconSize(KswordTheme::CompactIconSize());
        m_immediateButton->setText(workbench_messages::WriteModeImmediateLabel());
        m_immediateButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        m_immediateButton->setAutoRaise(true);
        m_immediateButton->setFocusPolicy(Qt::NoFocus);
        m_immediateButton->setToolTip(workbench_messages::WriteModeImmediateTooltip());
        connect(m_immediateButton, &QToolButton::clicked, this, [this]() {
            requestToggleToOtherSide(WriteMode::Immediate);
        });

        m_stagedButton = new CapsuleHalfButton(this);
        m_stagedButton->setIcon(QIcon(QStringLiteral(":/Icon/memwb_mode_staged.svg")));
        m_stagedButton->setIconSize(KswordTheme::CompactIconSize());
        m_stagedButton->setText(workbench_messages::WriteModeStagedLabel());
        m_stagedButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        m_stagedButton->setAutoRaise(true);
        m_stagedButton->setFocusPolicy(Qt::NoFocus);
        m_stagedButton->setToolTip(workbench_messages::WriteModeStagedTooltip());
        connect(m_stagedButton, &QToolButton::clicked, this, [this]() {
            requestToggleToOtherSide(WriteMode::StagedThenApply);
        });

        // 初始高亮：默认模式（立即写入）那一半。
        m_immediateButton->setChecked(true);
        m_stagedButton->setChecked(false);

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
        // 半边按钮按"是否当前模式"决定文字颜色，属性变了要让它们重画。
        m_immediateButton->setChecked(mode == WriteMode::Immediate);
        m_stagedButton->setChecked(mode == WriteMode::StagedThenApply);
        update();
    }

    int WriteModeSwitch::halfButtonWidth() const
    {
        // 每一半的宽度：左右内边距 + 图标 + 间距 + 两个文字里较宽的那个（两半等宽，
        // 高亮底色按"外框宽度的一半"画，两半必须等宽才对得上）。
        const QFontMetrics metrics(font());
        const int textWidth = (std::max)(
            metrics.horizontalAdvance(m_immediateButton->text()),
            metrics.horizontalAdvance(m_stagedButton->text()));
        return kHalfPadding * 2 + KswordTheme::CompactIconSize().width() + kIconTextGap + textWidth;
    }

    QSize WriteModeSwitch::sizeHint() const
    {
        // 两个半边并排加上左右内边距与中间分隔。
        return QSize(halfButtonWidth() * 2 + kButtonInset * 3, kCapsuleHeight);
    }

    QSize WriteModeSwitch::minimumSizeHint() const
    {
        // 最小也要放得下两个图标；文字放不下时半边按钮自己做右省略，所以最小宽度只算图标部分。
        const int iconHalf = kHalfPadding + KswordTheme::CompactIconSize().width() + kHalfPadding;
        return QSize(iconHalf * 2 + kButtonInset * 3, kCapsuleHeight);
    }

    bool WriteModeSwitch::event(QEvent* event)
    {
        // 运行期整句翻译会直接 setText 改半边按钮的文字（中英文宽度不同）：按钮的 updateGeometry
        // 会给本控件投递 LayoutRequest，这里据此重算自己的建议尺寸并重新摆放两半。
        if (event != nullptr && event->type() == QEvent::LayoutRequest)
        {
            updateGeometry();
            layoutButtons();
        }
        return QWidget::event(event);
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
