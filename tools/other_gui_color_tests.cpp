// 离屏颜色回归：调用 HUD 真实图表、卡片与进程 delegate；不启动采样或生产窗口。
#include "../KswordHUD/HudColors.h"
#include "../KswordHUD/HudPerformancePanel.h"
#include "../KswordHUD/HudProcessListPanel.h"
#include "../KswordHUD/PerformanceNavCard.h"
#include "../shared/ui/KsPainterChart.h"
#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QPalette>
#include <QStyledItemDelegate>
#include <QTreeWidget>
#include <cstdio>

namespace
{
    int checks = 0; // 已执行的行为断言数。
    int failures = 0; // 未满足的行为断言数。
    void check(const bool condition, const char* description)
    {
        ++checks;
        if (!condition)
        {
            ++failures;
            std::printf("FAIL: %s\n", description);
        }
    }

    // 计算实色像素数量，用实际绘制而非仅检查 setter 是否存储颜色。
    int countPixels(const QImage& image, const QColor& color)
    {
        int count = 0;
        for (int row = 0; row < image.height(); ++row)
        {
            for (int column = 0; column < image.width(); ++column)
            {
                if (image.pixelColor(column, row).rgb() == color.rgb())
                {
                    ++count;
                }
            }
        }
        return count;
    }

    void checkColorMatrix()
    {
        const int channels[] = {0, 32, 96, 160, 224, 255};
        const QColor preferred[] = {QColor(Qt::white), QColor(Qt::black), QColor(185, 205, 225), QColor(184, 99, 255)};
        for (const int red : channels)
        {
            for (const int green : channels)
            {
                for (const int blue : channels)
                {
                    const QColor background(red, green, blue);
                    for (const QColor& seed : preferred)
                    {
                        check(KswordHudColors::Contrast(KswordHudColors::Readable(seed, background), background) >= 4.5,
                            "text meets contrast on independent background");
                        check(KswordHudColors::Contrast(KswordHudColors::Readable(seed, background, 3.0), background) >= 3.0,
                            "data stroke meets contrast on independent background");
                        for (const double ratio : {0.0, 0.4, 1.0})
                        {
                            const QColor text = KswordHudColors::SelectedText(seed, background, ratio);
                            const QColor selected = KswordHudColors::Composite(QColor(89, 139, 214, 88), background);
                            const QColor track = KswordHudColors::Composite(
                                KswordHudColors::LegibleOverlay(QColor(255, 255, 255, 18), selected, text), selected);
                            const QColor fill = KswordHudColors::Composite(
                                KswordHudColors::LegibleOverlay(KswordHudColors::UsageHighlight(ratio), track, text), track);
                            check(KswordHudColors::Contrast(text, selected) >= 4.5, "selected base remains legible");
                            check(KswordHudColors::Contrast(text, track) >= 4.5, "selected empty track remains legible");
                            check(KswordHudColors::Contrast(text, fill) >= 4.5, "selected usage fill remains legible");
                        }
                    }
                }
            }
        }
        check(KswordHudColors::Composite(QColor(89, 139, 214, 88), QColor(Qt::white)) == QColor(198, 215, 241),
            "alpha composite uses original selected overlay");
    }

    void checkDelegate()
    {
        HudProcessListPanel panel; // 隐藏构造不会触发 showEvent 的进程采样。
        QTreeWidget* tree = panel.findChild<QTreeWidget*>();
        check(tree != nullptr, "real production process tree exists");
        if (tree == nullptr)
        {
            return;
        }
        auto* row = new QTreeWidgetItem(tree);
        row->setText(2, QStringLiteral("100.00%"));
        row->setData(2, Qt::UserRole + 2, 1.0);
        const QColor backgrounds[] = {QColor(Qt::white), QColor(10, 15, 22), QColor(70, 70, 70)};
        for (const QColor& background : backgrounds)
        {
            const QColor ordinary = background == QColor(Qt::white) ? QColor(Qt::black) : QColor(Qt::white);
            panel.setTableTextColor(ordinary);
            panel.setEffectiveBackgroundColor(background);
            const QModelIndex index = tree->indexFromItem(row, 2);
            for (const bool selected : {false, true})
            {
                QImage image(280, 28, QImage::Format_ARGB32_Premultiplied);
                image.fill(background);
                QPainter painter(&image);
                QStyleOptionViewItem option;
                option.initFrom(tree);
                option.rect = image.rect();
                option.font = tree->font();
                option.state = QStyle::State_Enabled | (selected ? QStyle::State_Selected : QStyle::State_None);
                tree->itemDelegate()->paint(&painter, option, index);
                painter.end();
                const QColor expected = selected ? KswordHudColors::SelectedText(ordinary, background, 1.0) : ordinary;
                check(countPixels(image, expected) > 5, "actual delegate paints configured normal or readable selected text");
                if (selected)
                {
                    check(KswordHudColors::Contrast(expected, image.pixelColor(2, 14)) >= 4.5,
                        "actual selected backdrop contrasts with text");
                    check(KswordHudColors::Contrast(expected, image.pixelColor(20, 14)) >= 4.5,
                        "actual resource fill backdrop contrasts with text");
                }
            }
        }
    }

    void checkChartsAndCard()
    {
        QChart chart;
        auto* line = new QLineSeries(&chart);
        const QColor seed(184, 99, 255);
        line->setColor(seed);
        line->append(0.0, 25.0);
        line->append(1.0, 75.0);
        chart.addSeries(line);
        auto* axis = new QValueAxis(&chart);
        axis->setGridLineColor(QColor(184, 99, 255, 35));
        chart.addAxis(axis, Qt::AlignLeft);
        const QColor backgrounds[] = {QColor(Qt::white), QColor(10, 15, 22), QColor(200, 240, 200), QColor(Qt::white)};
        for (const QColor& background : backgrounds)
        {
            HudPerformancePanel::applyChartColors(&chart, background);
            check(KswordHudColors::Contrast(chart.titleBrush().color(), background) >= 4.5, "real chart title is legible");
            check(KswordHudColors::Contrast(axis->labelsBrush().color(), background) >= 4.5, "real chart labels are legible");
            check(KswordHudColors::Contrast(line->color(), background) >= 3.0, "real series stroke is visible");
            check(line->property("ksword_hud_series_seed").value<QColor>() == seed, "switching retains original series color seed");
            check(line->count() == 2 && line->points().at(1) == QPointF(1.0, 75.0), "color changes preserve actual chart samples");

            PerformanceNavCard card;
            card.resize(286, 78);
            card.setTitleText(QStringLiteral("CPU"));
            card.setSubtitleText(QStringLiteral("25%"));
            card.setAccentColor(seed);
            QPalette palette = card.palette();
            const QColor primary = KswordHudColors::Readable(QColor(242, 246, 252), background);
            palette.setColor(QPalette::Window, background);
            palette.setColor(QPalette::WindowText, primary);
            palette.setColor(QPalette::Mid, KswordHudColors::Readable(QColor(185, 205, 225), background));
            card.setPalette(palette);
            QImage image(card.size(), QImage::Format_ARGB32_Premultiplied);
            image.fill(background);
            QPainter painter(&image);
            card.render(&painter);
            painter.end();
            check(countPixels(image, primary) > 5, "real performance card paints palette primary text");
        }
    }
}

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    checkColorMatrix();
    checkDelegate();
    checkChartsAndCard();
    std::printf("OTHER_GUI_COLOR_ASSERTIONS=%d FAILURES=%d\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
