// HexInspectorRowView.Paint.cpp
// 作用：HexInspectorRowView 的绘制。
// 约定：所有颜色都在本文件的 makePalette 里"每次 paintEvent 现取"KswordTheme 静态访问器，
// 不缓存颜色、不使用 palette(...) 样式串，所以切换主题后下一次绘制就是新主题，不需要任何通知。

#include "HexInspectorRowView.h"

#include "../../theme.h"

#include <QColor>
#include <QFontMetrics>
#include <QLineEdit>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QScrollBar>

#include <algorithm>

namespace ks::ui
{
    // PaintPalette：一次绘制用到的全部颜色。
    struct HexInspectorRowView::PaintPalette
    {
        QColor surface;         // 行底色
        QColor surfaceAlt;      // 标题底色
        QColor border;          // 标题下边框
        QColor borderSoft;      // 行与行之间的淡分隔线
        QColor text;            // 主文字（值列）
        QColor textSecondary;   // 次文字（类型列、十六进制列、标题）
        QColor textDisabled;    // 禁用文字（不可用行）
        QColor accent;          // 强调色（编辑提示描边）
        QColor selActiveBg;     // 当前行底色（列表有焦点）
        QColor selActiveFg;     // 当前行文字（列表有焦点）
        QColor selInactiveBg;   // 当前行底色（列表没有焦点）
        QColor selInactiveFg;   // 当前行文字（列表没有焦点）
        QColor hoverBg;         // 悬停行底色
        QColor warning;         // 内容不合法（超范围、孤立代理项）的文字色，已校准到对表面色可读
    };

    // 生成本次绘制的调色板。
    HexInspectorRowView::PaintPalette HexInspectorRowView::makePalette() const
    {
        PaintPalette palette;
        palette.surface = KswordTheme::SurfaceColor();
        palette.surfaceAlt = KswordTheme::SurfaceAltColor();
        palette.border = KswordTheme::BorderColor();
        palette.borderSoft = KswordTheme::BlendColors(palette.surface, palette.border, 110);
        palette.text = KswordTheme::TextPrimaryColor();
        palette.textSecondary = KswordTheme::TextSecondaryColor();
        palette.textDisabled = KswordTheme::TextDisabledColor();
        palette.accent = KswordTheme::PrimaryAccentColor();

        // 当前行：有焦点用编辑器选区色，没有焦点把它按固定比例淡化后混入表面色（与 HexCanvas 的非活动面板同一做法）。
        const QColor selection = KswordTheme::EditorSelectionColor();
        palette.selActiveBg = selection;
        palette.selActiveFg = KswordTheme::OnAccentColor(selection);
        palette.selInactiveBg = KswordTheme::BlendColors(palette.surface, selection, 120);
        palette.selInactiveFg = KswordTheme::EnsureTextContrast(palette.text, palette.selInactiveBg);

        // 悬停：强调色极淡地混入表面色；警告色按表面色校准对比度。
        palette.hoverBg = KswordTheme::BlendColors(palette.surface, palette.accent, 34);
        palette.warning = KswordTheme::EnsureTextContrast(KswordTheme::WarningColor(), palette.surface);
        return palette;
    }

    // 绘制整个视口：先铺底，再画可见行，最后画标题盖住行区域顶部的溢出。
    void HexInspectorRowView::paintEvent(QPaintEvent* event)
    {
        Q_UNUSED(event);
        QPainter painter(viewport());
        const PaintPalette palette = makePalette();
        painter.fillRect(viewport()->rect(), palette.surface);
        painter.setFont(font());

        // 可见行范围：由竖向滚动量与视口高度换算，夹取到有效行号。
        const Columns columns = computeColumns();
        const int count = rowCount();
        const int scrollY = verticalScrollBar()->value();
        const int viewWidth = viewport()->width();
        const int viewHeight = viewport()->height();
        if (count > 0)
        {
            const int firstRow = std::max(0, scrollY / std::max(1, m_rowHeight));
            const int lastRow = std::min(count - 1, (scrollY + viewHeight) / std::max(1, m_rowHeight));
            painter.save();
            painter.setClipRect(0, m_headerHeight, viewWidth, std::max(0, viewHeight - m_headerHeight));
            for (int row = firstRow; row <= lastRow; ++row)
            {
                paintRow(painter, palette, columns, row);
            }
            painter.restore();
        }

        // 标题最后画。
        paintHeader(painter, palette, columns);
    }

    // 绘制列标题。
    void HexInspectorRowView::paintHeader(QPainter& painter, const PaintPalette& palette, const Columns& columns) const
    {
        const int viewWidth = viewport()->width();
        painter.fillRect(0, 0, viewWidth, m_headerHeight, palette.surfaceAlt);
        painter.setPen(palette.border);
        painter.drawLine(0, m_headerHeight - 1, viewWidth, m_headerHeight - 1);

        // 三列标题文字，次文字色；列之间画淡色竖线帮助对齐。
        painter.setPen(palette.textSecondary);
        const int textHeight = m_headerHeight - 1;
        painter.drawText(
            QRect(columns.nameX + kCellPadding, 0, std::max(0, columns.nameW - 2 * kCellPadding), textHeight),
            Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine,
            QStringLiteral("类型"));
        painter.drawText(
            QRect(columns.valueX + kCellPadding, 0, std::max(0, columns.valueW - 2 * kCellPadding), textHeight),
            Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine,
            QStringLiteral("值"));
        painter.drawText(
            QRect(columns.hexX + kCellPadding, 0, std::max(0, columns.hexW - 2 * kCellPadding), textHeight),
            Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine,
            QStringLiteral("十六进制"));
        painter.setPen(palette.borderSoft);
        painter.drawLine(columns.valueX, 4, columns.valueX, m_headerHeight - 5);
        painter.drawLine(columns.hexX, 4, columns.hexX, m_headerHeight - 5);
    }

    // 绘制一行。
    void HexInspectorRowView::paintRow(QPainter& painter, const PaintPalette& palette, const Columns& columns, int row) const
    {
        const HexInspectorRowData& rowData = m_rows[static_cast<std::size_t>(row)];
        const QRect full = rowRect(row);
        const bool isCurrent = (row == m_currentRow);
        const bool isHover = (row == m_hoverRow);
        const bool listActive = hasFocus() || (m_editor != nullptr && m_editRow == row);

        // 底色：当前行（有/无焦点两档）> 悬停 > 普通。
        QColor background = palette.surface;
        QColor currentFg = palette.text;
        if (isCurrent)
        {
            background = listActive ? palette.selActiveBg : palette.selInactiveBg;
            currentFg = listActive ? palette.selActiveFg : palette.selInactiveFg;
        }
        else if (isHover)
        {
            background = palette.hoverBg;
        }
        painter.fillRect(full, background);
        painter.setPen(palette.borderSoft);
        painter.drawLine(full.left(), full.bottom(), full.right(), full.bottom());

        // 三列文字颜色：不可用行整行灰；内容不合法的值用警告色；当前行统一用当前行前景色（不可用时再淡一档）。
        QColor nameColor = isCurrent ? currentFg : palette.textSecondary;
        QColor valueColor = palette.text;
        QColor hexColor = isCurrent ? currentFg : palette.textSecondary;
        if (!rowData.available)
        {
            const QColor muted = isCurrent
                ? KswordTheme::BlendColors(background, currentFg, 150)
                : palette.textDisabled;
            nameColor = muted;
            valueColor = muted;
            hexColor = muted;
        }
        else if (isCurrent)
        {
            valueColor = currentFg;
        }
        else if (!rowData.valid)
        {
            valueColor = palette.warning;
        }

        // 类型列：不省略（列宽按最宽类型名算过），值列右省略，十六进制列中间省略保留高位与低位。
        const QFontMetrics metrics(font());
        const int cellHeight = full.height() - 1;
        painter.setPen(nameColor);
        painter.drawText(
            QRect(columns.nameX + kCellPadding, full.top(), std::max(0, columns.nameW - 2 * kCellPadding), cellHeight),
            Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine,
            rowData.typeName);

        const int valueTextWidth = std::max(0, columns.valueW - 2 * kCellPadding);
        painter.setPen(valueColor);
        painter.drawText(
            QRect(columns.valueX + kCellPadding, full.top(), valueTextWidth, cellHeight),
            Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine,
            metrics.elidedText(rowData.valueText, Qt::ElideRight, valueTextWidth));

        const int hexTextWidth = std::max(0, columns.hexW - 2 * kCellPadding);
        painter.setPen(hexColor);
        painter.drawText(
            QRect(columns.hexX + kCellPadding, full.top(), hexTextWidth, cellHeight),
            Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine,
            metrics.elidedText(rowData.hexText, Qt::ElideMiddle, hexTextWidth));

        // 可编辑行悬停时给值列+十六进制列描一圈细边，提示"这里可以双击编辑"；正在编辑的行由编辑器自己的边框表达。
        if (isHover && rowData.editEnabled && rowData.available && m_editor == nullptr)
        {
            painter.save();
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setPen(QPen(KswordTheme::BlendColors(background, palette.accent, 140), 1.0));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(
                QRectF(columns.valueX + 1.5, full.top() + 2.5, columns.valueW + columns.hexW - 3.0, full.height() - 5.0),
                3.0,
                3.0);
            painter.restore();
        }

        // 复制图标：只在可用且被悬停或被选中的行显示。
        if (rowData.available && (isHover || isCurrent))
        {
            paintCopyGlyph(painter, copyGlyphRect(row), isCurrent ? currentFg : palette.textSecondary);
        }
    }

    // 绘制复制图标：两张叠放的纸（后一张只画上边与左边，前一张画完整圆角框）。
    void HexInspectorRowView::paintCopyGlyph(QPainter& painter, const QRect& rect, const QColor& color) const
    {
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);
        QPen pen(color, 1.2);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);

        // 基准点：矩形左上角，图标整体在 16x16 里。
        const qreal left = rect.left();
        const qreal top = rect.top();
        QPainterPath back;
        back.moveTo(left + 9.5, top + 2.5);
        back.lineTo(left + 3.5, top + 2.5);
        back.lineTo(left + 3.5, top + 10.5);
        painter.drawPath(back);
        painter.drawRoundedRect(QRectF(left + 6.5, top + 5.5, 7.0, 8.5), 1.5, 1.5);
        painter.restore();
    }
}
