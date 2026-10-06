#include "PerformanceChartTheme.h"
#include "../../../shared/ui/KsPainterChart.h"

namespace ks::ui
{
    void RefreshPerformanceChartTheme(
        QChart* chart,
        const KswordTheme::PerformanceRole primaryRole,
        const KswordTheme::PerformanceRole secondaryRole,
        const QPalette* const surfacePalette)
    {
        if (chart == nullptr)
        {
            return;
        }

        // primaryColor 用途：坐标网格与绘图区遵循主曲线；副曲线保留自己的稳定角色。
        const auto roleColor = [surfacePalette](const KswordTheme::PerformanceRole role) {
            const QColor color = KswordTheme::PerformanceColor(role);
            return surfacePalette != nullptr
                ? KswordTheme::EnsureTextContrast(color, surfacePalette->color(QPalette::Base), 3.0)
                : color;
        };
        const QColor primaryColor = roleColor(primaryRole);
        const QColor secondaryColor = roleColor(secondaryRole);
        const auto recoloredPen = [](QPen pen, const QColor& color) {
            pen.setColor(KswordTheme::WithAlpha(color, pen.color().alpha()));
            return pen;
        };
        const auto recoloredBrush = [](QBrush brush, const QColor& color) {
            brush.setColor(KswordTheme::WithAlpha(color, brush.color().alpha()));
            return brush;
        };

        // seriesIndex 只统计可见业务曲线；填充区的上下边界不另占角色序号。
        int seriesIndex = 0;
        for (QAbstractSeries* const series : chart->series())
        {
            const QColor color = seriesIndex == 0 ? primaryColor : secondaryColor;
            if (auto* const area = dynamic_cast<QAreaSeries*>(series))
            {
                area->setPen(recoloredPen(area->pen(), color));
                area->setBrush(recoloredBrush(area->brush(), color));
                for (QLineSeries* const boundary : { area->upperSeries(), area->lowerSeries() })
                {
                    if (boundary != nullptr)
                    {
                        boundary->setPen(recoloredPen(boundary->pen(), color));
                    }
                }
                ++seriesIndex;
            }
            else if (auto* const line = dynamic_cast<QLineSeries*>(series))
            {
                line->setPen(recoloredPen(line->pen(), color));
                ++seriesIndex;
            }
        }

        // 每条轴保持原线宽、透明度、显隐与范围；只替换主题角色颜色。
        const QColor textColor = surfacePalette != nullptr
            ? surfacePalette->color(QPalette::Text) : KswordTheme::TextPrimaryColor();
        const QColor secondaryTextColor = surfacePalette != nullptr
            ? surfacePalette->color(QPalette::PlaceholderText) : KswordTheme::TextSecondaryColor();
        for (QAbstractAxis* const axis : chart->axes())
        {
            axis->setLinePen(recoloredPen(axis->linePen(), primaryColor));
            axis->setGridLinePen(recoloredPen(axis->gridLinePen(), primaryColor));
            axis->setLabelsBrush(recoloredBrush(axis->labelsBrush(), secondaryTextColor));
            axis->setTitleBrush(recoloredBrush(axis->titleBrush(), textColor));
        }
        chart->setTitleBrush(recoloredBrush(chart->titleBrush(), textColor));
        chart->legend()->setLabelColor(secondaryTextColor);
        chart->setPlotAreaBackgroundBrush(recoloredBrush(chart->plotAreaBackgroundBrush(), primaryColor));
        chart->setPlotAreaBackgroundPen(recoloredPen(chart->plotAreaBackgroundPen(), primaryColor));
        chart->update();
    }
}
