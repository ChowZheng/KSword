// HexInspectorWidgets.cpp
// 作用：HexInspectorWidgets.h 里三个自绘小控件的实现。
// 约定：所有颜色在每次 paintEvent 里现取 KswordTheme 的静态颜色访问器，不缓存、不用样式表。

#include "HexInspectorWidgets.h"

#include "../../theme.h"

#include <QFontDatabase>
#include <QFontMetrics>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>

#include <algorithm>

namespace ks::ui
{
    // 统一的等宽字体。
    QFont HexInspectorFixedFont()
    {
        // font：以系统等宽字体为底，再把 Consolas 排到最前，中文回退给 Microsoft YaHei UI。
        QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        font.setStyleHint(QFont::Monospace);
        font.setFixedPitch(true);
        font.setFamilies(QStringList{
            QStringLiteral("Consolas"),
            font.family(),
            QStringLiteral("Microsoft YaHei UI") });
        return font;
    }

    // 纯文本转原样显示的富文本提示。
    QString HexInspectorRichToolTip(const QString& plainText)
    {
        if (plainText.isEmpty())
        {
            return QString();
        }
        return QStringLiteral("<div style=\"white-space:pre\">%1</div>").arg(plainText.toHtmlEscaped());
    }

    // ========================= HexInspectorGlyphButton =========================

    // 构造：可勾选、开启悬停重绘，焦点策略保持默认（可用 Tab 到达）。
    HexInspectorGlyphButton::HexInspectorGlyphButton(Glyph glyph, QWidget* parent)
        : QAbstractButton(parent)
        , m_glyph(glyph)
    {
        // WA_Hover：鼠标进入/离开时让控件自动重绘，悬停底色才会出现与消失。
        setAttribute(Qt::WA_Hover, true);
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
    }

    // 图形种类。
    HexInspectorGlyphButton::Glyph HexInspectorGlyphButton::glyph() const
    {
        return m_glyph;
    }

    // 固定的紧凑尺寸，随字体高度略有伸缩，保证高分屏/大字体下图形不被裁掉。
    QSize HexInspectorGlyphButton::sizeHint() const
    {
        const int height = std::max(24, QFontMetrics(font()).height() + 6);
        return QSize(28, height);
    }

    // 绘制按钮：底板 + 图形。
    void HexInspectorGlyphButton::paintEvent(QPaintEvent* event)
    {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        // 本次绘制用到的主题色，全部现取。
        const QColor surface = KswordTheme::SurfaceAltColor();
        const QColor accent = KswordTheme::PrimaryAccentColor();
        const QColor border = KswordTheme::BorderColor();
        const QColor text = KswordTheme::TextPrimaryColor();
        const QColor disabledText = KswordTheme::TextDisabledColor();

        // 底板：选中用强调色淡化混合，按下更深，悬停用边框色轻混合，平时透明（直接露出顶栏底色）。
        const QRectF plate = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);
        QColor plateColor = Qt::transparent;
        if (isChecked())
        {
            plateColor = KswordTheme::BlendColors(surface, accent, isDown() ? 130 : 95);
        }
        else if (isDown())
        {
            plateColor = KswordTheme::BlendColors(surface, border, 160);
        }
        else if (underMouse())
        {
            plateColor = KswordTheme::BlendColors(surface, border, 110);
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(plateColor);
        painter.drawRoundedRect(plate, 4.0, 4.0);

        // 选中时再描一圈强调色细边，让"当前选择"在色弱情况下也靠轮廓可辨。
        if (isChecked())
        {
            painter.setPen(QPen(accent, 1.0));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(plate, 4.0, 4.0);
        }

        // 键盘焦点环：只在拥有焦点时画，颜色用强调色。
        if (hasFocus())
        {
            painter.setPen(QPen(accent, 1.0, Qt::DotLine));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(plate.adjusted(1.5, 1.5, -1.5, -1.5), 3.0, 3.0);
        }

        // 图形颜色：禁用灰、选中用强调色（保证对底板的对比度），其余用主文字色。
        QColor ink = text;
        if (!isEnabled())
        {
            ink = disabledText;
        }
        else if (isChecked())
        {
            ink = KswordTheme::EnsureTextContrast(accent, plateColor.isValid() && plateColor.alpha() > 0 ? plateColor : surface);
        }

        // 图形区域：以按钮中心为基准的 16x16 逻辑方块。
        const QPointF center = QRectF(rect()).center();
        const QRectF box(center.x() - 8.0, center.y() - 8.0, 16.0, 16.0);

        if (m_glyph == Glyph::LittleEndian || m_glyph == Glyph::BigEndian)
        {
            // 三根竖条：高度依次递增（小端）或递减（大端），底边对齐在箭头上方。
            const bool little = (m_glyph == Glyph::LittleEndian);
            const qreal barWidth = 3.0;
            const qreal gap = 1.5;
            const qreal baseY = box.bottom() - 5.0;
            const qreal heights[3] = { 3.0, 6.0, 9.0 };
            painter.setPen(Qt::NoPen);
            painter.setBrush(ink);
            for (int index = 0; index < 3; ++index)
            {
                // barHeight：第 index 根竖条的高度；little 时从左到右由矮变高。
                const qreal barHeight = little ? heights[index] : heights[2 - index];
                const qreal left = box.left() + 1.5 + index * (barWidth + gap);
                painter.drawRoundedRect(QRectF(left, baseY - barHeight, barWidth, barHeight), 0.8, 0.8);
            }

            // 底部向右的箭头：表示地址递增方向（箭杆 + 箭头）。
            QPen arrowPen(ink, 1.2);
            arrowPen.setCapStyle(Qt::RoundCap);
            arrowPen.setJoinStyle(Qt::RoundJoin);
            painter.setPen(arrowPen);
            painter.setBrush(Qt::NoBrush);
            const qreal arrowY = box.bottom() - 1.5;
            painter.drawLine(QPointF(box.left() + 1.0, arrowY), QPointF(box.right() - 1.0, arrowY));
            painter.drawLine(QPointF(box.right() - 3.5, arrowY - 2.0), QPointF(box.right() - 1.0, arrowY));
            painter.drawLine(QPointF(box.right() - 3.5, arrowY + 2.0), QPointF(box.right() - 1.0, arrowY));
            return;
        }

        // 指针宽度：徽标 "32" / "64"，字号随控件字体缩小，居中绘制。
        QFont badgeFont = font();
        badgeFont.setBold(true);
        badgeFont.setPixelSize(std::max(9, QFontMetrics(font()).height() - 4));
        painter.setFont(badgeFont);
        painter.setPen(ink);
        painter.setBrush(Qt::NoBrush);
        painter.drawText(
            QRectF(rect()),
            Qt::AlignCenter,
            m_glyph == Glyph::Pointer32 ? QStringLiteral("32") : QStringLiteral("64"));
    }

    // ========================= HexInspectorTopBar =========================

    // 构造：固定为一行高度，由布局决定宽度。
    HexInspectorTopBar::HexInspectorTopBar(QWidget* parent)
        : QWidget(parent)
    {
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    }

    // 设置地址文字，内容没变就不重绘。
    void HexInspectorTopBar::setAddressText(const QString& text)
    {
        if (m_addressText == text)
        {
            return;
        }
        m_addressText = text;
        update();
    }

    // 地址文字。
    QString HexInspectorTopBar::addressText() const
    {
        return m_addressText;
    }

    // 设置提示文字。
    void HexInspectorTopBar::setHintText(const QString& text)
    {
        if (m_hintText == text)
        {
            return;
        }
        m_hintText = text;
        update();
    }

    // 提示文字。
    QString HexInspectorTopBar::hintText() const
    {
        return m_hintText;
    }

    // 设置限定右边界的控件。
    void HexInspectorTopBar::setTextLimitWidget(QWidget* widget)
    {
        m_limitWidget = widget;
        update();
    }

    // 建议尺寸：高度为按钮高度加上下内边距。
    QSize HexInspectorTopBar::sizeHint() const
    {
        const int height = std::max(30, QFontMetrics(font()).height() + 12);
        return QSize(160, height);
    }

    // 绘制顶栏。
    void HexInspectorTopBar::paintEvent(QPaintEvent* event)
    {
        Q_UNUSED(event);
        QPainter painter(this);

        // 底色与下边框。
        painter.fillRect(rect(), KswordTheme::SurfaceAltColor());
        painter.setPen(KswordTheme::BorderColor());
        painter.drawLine(0, height() - 1, width(), height() - 1);

        // 文字可用区域：左 10 像素内边距，右边到限定控件左边缘再留 8 像素。
        int rightEdge = width() - 8;
        if (m_limitWidget != nullptr)
        {
            rightEdge = std::min(rightEdge, m_limitWidget->geometry().left() - 8);
        }
        const QRect textArea(10, 0, std::max(0, rightEdge - 10), height() - 1);
        painter.setFont(font());
        const QFontMetrics metrics(font());

        // 地址：主文字色，放不下就中间省略（两端的高位与低位都保留）。
        const QString address = metrics.elidedText(m_addressText, Qt::ElideMiddle, textArea.width());
        painter.setPen(KswordTheme::TextPrimaryColor());
        painter.drawText(textArea, Qt::AlignVCenter | Qt::AlignLeft, address);

        // 提示：灰色，紧跟在地址右侧，剩余宽度不够就省略，再不够就不画。
        if (!m_hintText.isEmpty())
        {
            const int addressWidth = metrics.horizontalAdvance(address);
            const int hintLeft = textArea.left() + addressWidth + 12;
            const int hintWidth = textArea.right() - hintLeft + 1;
            if (hintWidth > metrics.horizontalAdvance(QStringLiteral("0")) * 3)
            {
                painter.setPen(KswordTheme::TextSecondaryColor());
                painter.drawText(
                    QRect(hintLeft, textArea.top(), hintWidth, textArea.height()),
                    Qt::AlignVCenter | Qt::AlignLeft,
                    metrics.elidedText(m_hintText, Qt::ElideRight, hintWidth));
            }
        }
    }

    // ========================= HexInspectorStatusBar =========================

    // 构造：固定两行高度，由布局决定宽度。
    HexInspectorStatusBar::HexInspectorStatusBar(QWidget* parent)
        : QWidget(parent)
    {
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    }

    // 设置常驻提示。
    void HexInspectorStatusBar::setIdleHint(const QString& hint)
    {
        m_idleHint = hint;
        refreshToolTip();
        update();
    }

    // 显示一条消息。
    void HexInspectorStatusBar::setMessage(Kind kind, const QString& text)
    {
        if (text.isEmpty())
        {
            clearMessage();
            return;
        }
        m_kind = kind;
        m_message = text;
        refreshToolTip();
        update();
    }

    // 清除消息。
    void HexInspectorStatusBar::clearMessage()
    {
        m_kind = Kind::Hint;
        m_message.clear();
        refreshToolTip();
        update();
    }

    // 当前显示的文字。
    QString HexInspectorStatusBar::messageText() const
    {
        return m_message.isEmpty() ? m_idleHint : m_message;
    }

    // 当前种类。
    HexInspectorStatusBar::Kind HexInspectorStatusBar::kind() const
    {
        return m_kind;
    }

    // 建议尺寸：两行文字高度加内边距。
    QSize HexInspectorStatusBar::sizeHint() const
    {
        const int height = QFontMetrics(font()).lineSpacing() * 2 + 10;
        return QSize(160, height);
    }

    // 同步悬停提示：消息里可能含目标内存里的字符串，必须按纯文本转义显示。
    void HexInspectorStatusBar::refreshToolTip()
    {
        setToolTip(HexInspectorRichToolTip(messageText()));
    }

    // 绘制状态条。
    void HexInspectorStatusBar::paintEvent(QPaintEvent* event)
    {
        Q_UNUSED(event);
        QPainter painter(this);

        // 底色与上边框。
        const QColor surface = KswordTheme::SurfaceColor();
        painter.fillRect(rect(), surface);
        painter.setPen(KswordTheme::BorderColor());
        painter.drawLine(0, 0, width(), 0);

        // 文字颜色与左侧色条颜色：错误用语义红，信息用强调色，提示用禁用灰。
        QColor textColor = KswordTheme::TextDisabledColor();
        QColor barColor = Qt::transparent;
        if (m_kind == Kind::Error && !m_message.isEmpty())
        {
            textColor = KswordTheme::EnsureTextContrast(KswordTheme::ErrorColor(), surface);
            barColor = textColor;
        }
        else if (m_kind == Kind::Info && !m_message.isEmpty())
        {
            textColor = KswordTheme::TextSecondaryColor();
            barColor = KswordTheme::PrimaryAccentColor();
        }

        // 色条：消息存在时在左侧画 3 像素宽的竖条。
        if (barColor.alpha() > 0)
        {
            painter.fillRect(QRect(0, 1, 3, height() - 1), barColor);
        }

        // 文字：按单词/任意字符换行，裁剪在控件内，超出两行的部分被裁掉（全文在悬停提示里）。
        painter.setFont(font());
        painter.setPen(textColor);
        const QRect textArea(10, 3, std::max(0, width() - 16), std::max(0, height() - 5));
        painter.setClipRect(textArea);
        painter.drawText(
            textArea,
            Qt::AlignVCenter | Qt::AlignLeft | Qt::TextWrapAnywhere,
            messageText());
    }
}
