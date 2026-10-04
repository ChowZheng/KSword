#include "theme_recovery_test_support.h"
#include "../shared/ui/KsPainterChart.h"
#include "../Ksword5.1/Ksword5.1/UI/PerformanceChartTheme.h"
#include "../Ksword5.1/Ksword5.1/UI/ThemeColorRemap.h"
#include <QLinearGradient>
#include <QApplication>

void TestPerformanceChartTheme()
{
    using Role = KswordTheme::PerformanceRole;
    // chart 只构造项目绘图模型，不创建硬件 Dock、驱动客户端或真实系统采样器。
    QChart chart;
    auto* const readLine = new QLineSeries(&chart);
    auto* const readBaseline = new QLineSeries(&chart);
    auto* const writeLine = new QLineSeries(&chart);
    auto* const writeBaseline = new QLineSeries(&chart);
    readLine->append(1.0, 12.0);
    readLine->append(2.0, 17.0);
    writeLine->append(1.0, 4.0);
    writeLine->append(2.0, 9.0);
    readBaseline->append(1.0, 0.0);
    writeBaseline->append(1.0, 0.0);
    const QList<QPointF> originalReadPoints = readLine->points();
    const QList<QPointF> originalWritePoints = writeLine->points();

    // 两块填充区刻意使用不同 alpha，验证换色时保留透明度、线宽和数据。
    auto* const readArea = new QAreaSeries(readLine, readBaseline, &chart);
    auto* const writeArea = new QAreaSeries(writeLine, writeBaseline, &chart);
    readArea->setBrush(QBrush(QColor(11, 22, 33, 42)));
    writeArea->setBrush(QBrush(QColor(33, 22, 11, 34)));
    readArea->setPen(QPen(QColor(1, 2, 3, 190), 1.7));
    writeArea->setPen(QPen(QColor(3, 2, 1, 180), 2.3));
    chart.addSeries(readArea);
    chart.addSeries(writeArea);
    chart.setPlotAreaBackgroundBrush(QBrush(QColor(11, 22, 33, 18)));
    chart.setPlotAreaBackgroundPen(QPen(QColor(11, 22, 33, 150), 1.0));
    auto* const axis = new QValueAxis(&chart);
    axis->setRange(2.5, 48.5);
    axis->setLinePen(QPen(QColor(1, 2, 3, 140), 1.0));
    axis->setGridLinePen(QPen(QColor(1, 2, 3, 46), 0.7));
    chart.addAxis(axis, Qt::AlignLeft);

    // 先应用主题 A，再换到明显不同的 B，双曲线始终按各自角色重算。
    KswordTheme::SetDarkModeEnabled(true);
    KswordTheme::SetMainBackgroundColor(QString());
    KswordTheme::SetPrimaryAccentColor(QStringLiteral("#B266D3"));
    ks::ui::RefreshPerformanceChartTheme(&chart, Role::Read, Role::Write);
    const QColor firstReadColor = readLine->color();
    const QColor firstWriteColor = writeLine->color();
    Require(firstReadColor == KswordTheme::PerformanceColor(Role::Read), "chart A read role follows accent");
    Require(firstWriteColor == KswordTheme::PerformanceColor(Role::Write), "chart A write role follows accent");
    KswordTheme::SetPrimaryAccentColor(QStringLiteral("#52B87F"));
    ks::ui::RefreshPerformanceChartTheme(&chart, Role::Read, Role::Write);
    Require(readLine->color() != firstReadColor, "chart B replaces read color captured under A");
    Require(writeLine->color() != firstWriteColor, "chart B replaces write color captured under A");
    Require(readArea->color() == KswordTheme::WithAlpha(KswordTheme::PerformanceColor(Role::Read), 42),
        "read fill follows role and retains alpha");
    Require(writeArea->color() == KswordTheme::WithAlpha(KswordTheme::PerformanceColor(Role::Write), 34),
        "write fill follows role and retains alpha");
    Require(readArea->pen().widthF() == 1.7 && writeArea->pen().widthF() == 2.3,
        "chart refresh preserves independent pen widths");
    Require(axis->min() == 2.5 && axis->max() == 48.5, "chart refresh preserves axis range");
    Require(axis->gridLinePen().color() == KswordTheme::WithAlpha(KswordTheme::PerformanceColor(Role::Read), 46),
        "grid follows primary role and retains alpha");
    Require(chart.plotAreaBackgroundBrush().color()
        == KswordTheme::WithAlpha(KswordTheme::PerformanceColor(Role::Read), 18), "plot retains fill alpha");
    Require(readLine->points() == originalReadPoints && writeLine->points() == originalWritePoints,
        "theme refresh preserves all sample points");
    KswordTheme::SetPrimaryAccentColor(QString());
    ks::ui::RefreshPerformanceChartTheme(&chart, Role::Read, Role::Write);
    Require(readLine->color() == KswordTheme::PerformanceColor(Role::Read), "chart restores default read role");
    Require(writeLine->color() == KswordTheme::PerformanceColor(Role::Write), "chart restores default write role");

    // 独立浮窗颜色显式传入，不切换全局模式、种子或 QApplication palette。
    const QPalette applicationPalette = QApplication::palette();
    QPen readBoundaryPen = readLine->pen();
    QPen writeBoundaryPen = writeLine->pen();
    readBoundaryPen.setColor(KswordTheme::WithAlpha(readBoundaryPen.color(), 171));
    writeBoundaryPen.setColor(KswordTheme::WithAlpha(writeBoundaryPen.color(), 207));
    readLine->setPen(readBoundaryPen);
    writeLine->setPen(writeBoundaryPen);
    const int originalReadLineAlpha = readLine->color().alpha();
    const int originalWriteLineAlpha = writeLine->color().alpha();
    KswordTheme::SetPrimaryAccentColor(QStringLiteral("#B266D3"));
    for (const bool mainDarkMode : { true, false })
    {
        KswordTheme::SetDarkModeEnabled(mainDarkMode);
        const QColor mainReadColor = KswordTheme::PerformanceColor(Role::Read);
        const QColor mainWriteColor = KswordTheme::PerformanceColor(Role::Write);
        const QColor mainAccentSeed = KswordTheme::PrimaryAccentColor();
        const bool floatingDarkMode = !mainDarkMode;
        QPalette floatingPalette;
        floatingPalette.setColor(QPalette::Base, KswordTheme::DefaultSurfaceColor(floatingDarkMode));
        floatingPalette.setColor(QPalette::Text, KswordTheme::DefaultTextPrimaryColor(floatingDarkMode));
        floatingPalette.setColor(QPalette::PlaceholderText, KswordTheme::DefaultTextSecondaryColor(floatingDarkMode));
        ks::ui::RefreshPerformanceChartTheme(&chart, Role::Read, Role::Write, &floatingPalette);
        const QColor floatingReadColor = KswordTheme::EnsureTextContrast(
            mainReadColor, floatingPalette.color(QPalette::Base), 3.0);
        const QColor floatingWriteColor = KswordTheme::EnsureTextContrast(
            mainWriteColor, floatingPalette.color(QPalette::Base), 3.0);
        // 校准可能返回 HSL QColor；画笔经 WithAlpha 归一化为 8 位 RGB。
        // 比较最终 RGBA 像素，并用换色前的独立 alpha 验证透明度未被覆盖。
        Require(readLine->color().rgba() == KswordTheme::WithAlpha(floatingReadColor, originalReadLineAlpha).rgba()
            && writeLine->color().rgba() == KswordTheme::WithAlpha(floatingWriteColor, originalWriteLineAlpha).rgba(),
            "independent floating curves retain global accent roles and use local contrast");
        Require(KswordTheme::ContrastRatio(readLine->color(), floatingPalette.color(QPalette::Base)) >= 3.0
            && KswordTheme::ContrastRatio(writeLine->color(), floatingPalette.color(QPalette::Base)) >= 3.0,
            "independent floating curve RGB colors meet actual local 3 to 1 contrast");
        Require(axis->linePen().color() == KswordTheme::WithAlpha(floatingReadColor, 140)
            && axis->gridLinePen().color() == KswordTheme::WithAlpha(floatingReadColor, 46),
            "independent floating axes and grid use local contrast and retain alpha");
        Require(chart.plotAreaBackgroundBrush().color() == KswordTheme::WithAlpha(floatingReadColor, 18),
            "independent floating plot uses local role contrast and retains alpha");
        Require(axis->labelsBrush().color().rgb() == floatingPalette.color(QPalette::PlaceholderText).rgb()
            && axis->titleBrush().color().rgb() == floatingPalette.color(QPalette::Text).rgb(),
            "independent floating axes use their own light or dark text colors");
        Require(chart.titleBrush().color().rgb() == floatingPalette.color(QPalette::Text).rgb()
            && chart.legend()->labelColor() == floatingPalette.color(QPalette::PlaceholderText),
            "independent floating chart title and legend use their own palette");
        Require(KswordTheme::IsDarkModeEnabled() == mainDarkMode
            && KswordTheme::PrimaryAccentColor() == mainAccentSeed
            && QApplication::palette() == applicationPalette,
            "floating chart refresh leaves global mode accent and application palette unchanged");
        Require(readLine->points() == originalReadPoints && writeLine->points() == originalWritePoints
            && axis->min() == 2.5 && axis->max() == 48.5,
            "floating palette changes retain samples and axis range");
        ks::ui::RefreshPerformanceChartTheme(&chart, Role::Read, Role::Write);
        Require(readLine->color().rgba() == KswordTheme::WithAlpha(mainReadColor, originalReadLineAlpha).rgba()
            && writeLine->color().rgba() == KswordTheme::WithAlpha(mainWriteColor, originalWriteLineAlpha).rgba(),
            "returning from an independent float restores current main chart roles");
        Require(axis->labelsBrush().color().rgb() == KswordTheme::TextSecondaryColor().rgb(),
            "returning from an independent float restores main axis text");
    }
    KswordTheme::SetDarkModeEnabled(true);
    KswordTheme::SetPrimaryAccentColor(QString());

    // 浮窗缓存快照在主题 A 捕获，主题 B 下返还前迁移；显式角色保持显式，继承不固化。
    KswordTheme::SetMainBackgroundColor(QStringLiteral("#283143"));
    const ks::ui::ThemeColorSnapshot savedTheme = ks::ui::CaptureThemeColorSnapshot();
    const QColor savedWindowColor = KswordTheme::WithAlpha(KswordTheme::MainBackgroundColor(), 73);
    QPalette savedPalette;
    savedPalette.setColor(QPalette::Active, QPalette::Window, savedWindowColor);
    QLinearGradient gradient(0.0, 0.0, 10.0, 10.0);
    gradient.setColorAt(0.0, Qt::red);
    gradient.setColorAt(1.0, Qt::blue);
    savedPalette.setBrush(QPalette::Active, QPalette::Button, QBrush(gradient));
    const auto savedMask = savedPalette.resolveMask();
    const QString savedStyle = QStringLiteral("QWidget{background:%1;}")
        .arg(KswordTheme::MainBackgroundColorHex());
    KswordTheme::SetMainBackgroundColor(QStringLiteral("#392542"));
    const QColor expectedWindowColor = KswordTheme::WithAlpha(KswordTheme::MainBackgroundColor(), 73);
    Require(ks::ui::RemapStaleThemeColor(savedTheme, savedWindowColor) == expectedWindowColor,
        "saved standalone color migrates and preserves alpha");
    const QPalette restoredPalette = ks::ui::RemapStaleThemeColorsInPalette(savedTheme, savedPalette);
    Require(restoredPalette.color(QPalette::Active, QPalette::Window) == expectedWindowColor,
        "saved palette migrates to the current main background");
    Require(restoredPalette.resolveMask() == savedMask, "saved palette retains resolve mask");
    Require(restoredPalette.brush(QPalette::Active, QPalette::Button)
        == savedPalette.brush(QPalette::Active, QPalette::Button), "saved palette preserves gradient brushes");
    Require(ks::ui::RemapStaleThemeColorsInText(savedTheme, savedStyle)
        .contains(KswordTheme::MainBackgroundColorHex()), "saved QSS migrates before returning from floating view");
    const QColor unrelatedColor(3, 7, 11, 99);
    Require(ks::ui::RemapStaleThemeColor(savedTheme, unrelatedColor) == unrelatedColor,
        "snapshot remapping preserves unrelated colors");
    KswordTheme::SetMainBackgroundColor(QString());
    KswordTheme::SetPrimaryAccentColor(QString());
}
