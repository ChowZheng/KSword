#include "ThemePreviewWidget.h"

#include "../Internationalization/LanguageManager.h"
#include "../theme.h"

#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSizePolicy>

namespace
{
    // PreviewGeometry 是绘制与命中测试共用的逻辑坐标，随字体/窗口宽度缩放。
    struct PreviewGeometry
    {
        qreal rowHeight; // 每行容纳当前字体及上下留白。
        QRectF navigation; // 三个导航项共用的横条。
        QRectF button; // 可悬停/按下的样例按钮。
        QRectF input; // 输入框样例，只展示颜色。
        QRectF disabled; // 禁用按钮样例。
        QRectF table; // 表头及两条示例数据。
        QRectF editor; // 静态代码片段，不连接编辑器业务。
        QRectF notification; // 静态通知，不注册真实通知卡。

        PreviewGeometry(const int width, const int fontHeight)
        {
            rowHeight = qMax(28, fontHeight + 12);
            const qreal contentWidth = qMax(0, width - 20); // 保留两侧边框留白。
            navigation = QRectF(10, 10, contentWidth, rowHeight);
            const qreal controlWidth = qMax(0.0, (contentWidth - 12) / 3); // 三个控件等宽，保留间距。
            button = QRectF(10, navigation.bottom() + 8, controlWidth, rowHeight);
            input = button.translated(controlWidth + 6, 0);
            disabled = input.translated(controlWidth + 6, 0);
            table = QRectF(10, button.bottom() + 8, contentWidth, rowHeight * 3);

            // 窄窗不缩小字体，代码与通知转为上下布局。
            const bool narrow = width < 420; // 窄窗转为单列。
            const qreal detailWidth = narrow ? contentWidth : qMax(0.0, (contentWidth - 8) / 2); // 内容卡宽度。
            editor = QRectF(10, table.bottom() + 8, detailWidth, rowHeight * 2);
            notification = narrow ? editor.translated(0, editor.height() + 8)
                : editor.translated(editor.width() + 8, 0);
        }

        // 导航和数据行使用统一分割，点击索引与最终绘制一致。
        QRectF tab(const int index) const
        {
            const qreal tabWidth = navigation.width() / 3; // 导航项等分整排。
            return QRectF(navigation.left() + tabWidth * index, navigation.top(), tabWidth, rowHeight);
        }

        QRectF row(const int index) const
        {
            return QRectF(table.left(), table.top() + rowHeight * (index + 1), table.width(), rowHeight);
        }
    };

    // drawLabel 在逻辑区域内按实际字体裁剪文字，不让英文或大字号穿出样例边框。
    void drawLabel(QPainter& painter, const QRectF& area, const QString& text,
        const QColor& color, const Qt::Alignment alignment = Qt::AlignCenter)
    {
        painter.setPen(color);
        const QRectF textArea = area.adjusted(8, 0, -8, 0); // 文本与边框保留水平留白。
        painter.drawText(textArea, alignment,
            painter.fontMetrics().elidedText(text, Qt::ElideRight, qMax(0, qRound(textArea.width()))));
    }

    // drawPanel 使用当前背景、边框角色；无需向应用全局样式写入待应用颜色。
    void drawPanel(QPainter& painter, const QRectF& area, const QColor& background, const QColor& border)
    {
        painter.setBrush(background);
        painter.setPen(border);
        painter.drawRoundedRect(area, 4, 4);
    }
}

namespace ks::ui
{
    ThemePreviewWidget::ThemePreviewWidget(QWidget* parent)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("ksword_theme_preview"));
        setMouseTracking(true);
        // 高度跟随可用宽度，水平最小值保持为零，不撑大外观设置窗口。
        QSizePolicy previewPolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        previewPolicy.setHeightForWidth(true);
        setSizePolicy(previewPolicy);
        ks::i18n::LanguageManager::instance().bindToolTip(this,
            QStringLiteral("settings.theme.preview.interaction"),
            QStringLiteral("悬停或按下样例按钮，点击导航和表格查看选中效果。"));
    }

    void ThemePreviewWidget::setPreview(const bool darkMode, const QString& accentColor,
        const QString& backgroundColor)
    {
        m_darkMode = darkMode;
        m_accentColor = QColor(accentColor);
        m_backgroundColor = QColor(backgroundColor);
        update();
    }

    QSize ThemePreviewWidget::sizeHint() const
    {
        return QSize(560, heightForWidth(560));
    }

    QSize ThemePreviewWidget::minimumSizeHint() const
    {
        return QSize(0, heightForWidth(width()));
    }

    int ThemePreviewWidget::heightForWidth(const int width) const
    {
        const PreviewGeometry geometry(width, fontMetrics().height()); // 按给定宽度计算完整内容高。
        return qRound(geometry.notification.bottom() + 10);
    }

    void ThemePreviewWidget::paintEvent(QPaintEvent* event)
    {
        Q_UNUSED(event);
        // 同步绘制范围借用未应用种子；范围退出后全局种子、palette与缓存保持原值。
        const KswordTheme::ThemePreviewSeeds seeds{m_darkMode, m_accentColor, m_backgroundColor};
        const KswordTheme::ScopedThemePreview previewScope(seeds);
        const PreviewGeometry geometry(width(), fontMetrics().height()); // 当前实际绘制区域。
        QPainter painter(this); // 仅绘制本控件。
        painter.setRenderHint(QPainter::Antialiasing);
        const QColor surface = KswordTheme::SurfaceColor(); // 内容表面与主背景独立派生。
        const QColor alternate = KswordTheme::SurfaceAltColor(); // 交替行与普通控件底色。
        const QColor border = KswordTheme::BorderColor(); // 各样例轮廓颜色。
        const QColor text = KswordTheme::TextPrimaryColor(); // 正文与标题前景。
        drawPanel(painter, QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
            KswordTheme::MainBackgroundColor(), border);

        // 导航三态底面跟随背景色；文字、图形与选中标记复用生产强调色角色。
        const QString tabNames[] = { // 每次按当前界面语言读取三个导航标签。
            ks::i18n::text(QStringLiteral("settings.theme.preview.welcome"), QStringLiteral("欢迎")),
            ks::i18n::text(QStringLiteral("settings.theme.preview.process"), QStringLiteral("进程")),
            ks::i18n::text(QStringLiteral("settings.theme.preview.network"), QStringLiteral("网络"))};
        for (int index = 0; index < 3; ++index) // index为缩略导航项索引。
        {
            const auto state = index == m_selectedTab ? KswordTheme::DockTabState::Active // 当前样例绘制态。
                : index == m_hoveredTab ? KswordTheme::DockTabState::Hover : KswordTheme::DockTabState::Inactive;
            painter.fillRect(geometry.tab(index), KswordTheme::DockTabBackgroundColor(state));
            if (state == KswordTheme::DockTabState::Active)
            {
                // 标记不改变标签几何，与真实导航底部的两像素强调边一致。
                const QRectF marker = geometry.tab(index).adjusted(0, geometry.rowHeight - 2, 0, 0);
                painter.fillRect(marker, KswordTheme::DockTabHighlightColor());
            }
            drawLabel(painter, geometry.tab(index), tabNames[index], KswordTheme::DockTabTextColor(state));
        }

        // 按钮三态及播放图形均以实际底色求前景，禁用态单独展示。
        const QColor buttonBackground = m_buttonPressed ? KswordTheme::ControlAccentPressedColor() // 实际按钮底。
            : m_buttonHovered ? KswordTheme::ControlAccentColor() : alternate;
        const QColor buttonText = m_buttonPressed || m_buttonHovered // 与当前按钮底配对的前景。
            ? KswordTheme::OnAccentColor(buttonBackground) : text;
        drawPanel(painter, geometry.button, buttonBackground, border);
        const QPointF iconCenter = geometry.button.center() - QPointF(geometry.button.width() / 3, 0); // 左侧图标中心。
        QPainterPath playIcon; // 简单播放图形按同一生产图形角色求色。
        playIcon.moveTo(iconCenter + QPointF(-3, -5));
        playIcon.lineTo(iconCenter + QPointF(5, 0));
        playIcon.lineTo(iconCenter + QPointF(-3, 5));
        playIcon.closeSubpath();
        painter.fillPath(playIcon, KswordTheme::ControlGlyphColor(buttonBackground));
        drawLabel(painter, geometry.button.adjusted(16, 0, 0, 0),
            ks::i18n::text(QStringLiteral("settings.theme.preview.button"), QStringLiteral("按钮")), buttonText);
        // 输入与禁用样例只展示颜色，不承载真实文字输入或按钮动作。
        drawPanel(painter, geometry.input, surface, border);
        drawLabel(painter, geometry.input,
            ks::i18n::text(QStringLiteral("settings.theme.preview.input"), QStringLiteral("输入框")),
            KswordTheme::TextSecondaryColor());
        drawPanel(painter, geometry.disabled, alternate, border);
        drawLabel(painter, geometry.disabled,
            ks::i18n::text(QStringLiteral("settings.theme.preview.disabled"), QStringLiteral("禁用")),
            KswordTheme::TextDisabledColor());

        // 表格保存样例选中项，换色不重建交互状态；普通与交替行仍来自生产表面角色。
        painter.fillRect(geometry.table, surface);
        const QRectF header(geometry.table.topLeft(), QSizeF(geometry.table.width(), geometry.rowHeight)); // 表头区域。
        painter.fillRect(header, alternate);
        drawLabel(painter, header,
            ks::i18n::text(QStringLiteral("settings.theme.preview.table"), QStringLiteral("表格 · 名称 / 状态")), text);
        for (int index = 0; index < 2; ++index) // index为两条样例行的索引。
        {
            const bool selected = index == m_selectedRow; // 当前样例行的选中态。
            const QColor background = selected ? KswordTheme::PrimaryAccentColor() // 此行实际绘制底。
                : index == 0 ? surface : alternate;
            const QColor preferred = index == 0 ? KswordTheme::InfoColor() : KswordTheme::WarningAccentColor(); // 状态语义色。
            painter.fillRect(geometry.row(index), background);
            drawLabel(painter, geometry.row(index), index == 0
                ? ks::i18n::text(QStringLiteral("settings.theme.preview.ready"), QStringLiteral("示例项目 · 就绪"))
                : ks::i18n::text(QStringLiteral("settings.theme.preview.warning"), QStringLiteral("示例项目 · 警告")),
                KswordTheme::EnsureTextContrast(preferred, background));
        }
        painter.setPen(border);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(geometry.table);

        // 代码与通知只是颜色样例：不创建日志、任务或实际编辑会话。
        drawPanel(painter, geometry.editor, surface, border);
        const QRectF editorTitle(geometry.editor.topLeft(), QSizeF(geometry.editor.width(), geometry.rowHeight)); // 代码卡标题。
        drawLabel(painter, editorTitle,
            ks::i18n::text(QStringLiteral("settings.theme.preview.editor"), QStringLiteral("编辑器")), text);
        drawLabel(painter, editorTitle.translated(0, geometry.rowHeight), QStringLiteral("int value = 42;"),
            KswordTheme::InfoColor(), Qt::AlignLeft | Qt::AlignVCenter);
        drawPanel(painter, geometry.notification, surface, border);
        painter.fillRect(QRectF(geometry.notification.left(), geometry.notification.top() + 4,
            3, geometry.notification.height() - 8), KswordTheme::InfoColor());
        const QRectF noticeTitle(geometry.notification.topLeft(), QSizeF(geometry.notification.width(), geometry.rowHeight)); // 通知标题。
        drawLabel(painter, noticeTitle,
            ks::i18n::text(QStringLiteral("settings.theme.preview.notification"), QStringLiteral("通知")), text);
        drawLabel(painter, noticeTitle.translated(0, geometry.rowHeight),
            ks::i18n::text(QStringLiteral("settings.theme.preview.done"), QStringLiteral("操作已完成")),
            KswordTheme::TextSecondaryColor());
    }

    void ThemePreviewWidget::changeEvent(QEvent* event)
    {
        QWidget::changeEvent(event);
        // 字体变化影响布局；其余外观/语言变化只重绘，不重新应用任何设置。
        if (event->type() == QEvent::FontChange)
        {
            updateGeometry();
        }
        update();
    }

    void ThemePreviewWidget::mouseMoveEvent(QMouseEvent* event)
    {
        const PreviewGeometry geometry(width(), fontMetrics().height()); // 鼠标命中与绘制共用坐标。
        m_buttonHovered = geometry.button.contains(event->position());
        m_hoveredTab = -1;
        for (int index = 0; index < 3; ++index)
        {
            if (geometry.tab(index).contains(event->position()))
            {
                m_hoveredTab = index;
            }
        }
        update();
    }

    void ThemePreviewWidget::mousePressEvent(QMouseEvent* event)
    {
        if (event->button() != Qt::LeftButton)
        {
            QWidget::mousePressEvent(event);
            return;
        }
        // 点击只更新缩略图选择，不发出主窗口导航或业务动作。
        const PreviewGeometry geometry(width(), fontMetrics().height()); // 本次点击对应的样例区域。
        m_buttonPressed = geometry.button.contains(event->position());
        for (int index = 0; index < 3; ++index)
        {
            if (geometry.tab(index).contains(event->position()))
            {
                m_selectedTab = index;
            }
        }
        for (int index = 0; index < 2; ++index)
        {
            if (geometry.row(index).contains(event->position()))
            {
                m_selectedRow = index;
            }
        }
        update();
    }

    void ThemePreviewWidget::mouseReleaseEvent(QMouseEvent* event)
    {
        if (event->button() == Qt::LeftButton)
        {
            m_buttonPressed = false;
            update();
        }
        QWidget::mouseReleaseEvent(event);
    }

    void ThemePreviewWidget::leaveEvent(QEvent* event)
    {
        m_buttonHovered = false;
        m_buttonPressed = false;
        m_hoveredTab = -1;
        update();
        QWidget::leaveEvent(event);
    }
}
