#include "theme_recovery_test_support.h"
#include "../Ksword5.1/Ksword5.1/theme.h"
#include "../Ksword5.1/Ksword5.1/UI/SvgThemeIconManager.h"
#include "../Ksword5.1/Ksword5.1/UI/ThemeColorRemap.h"
#include "../Ksword5.1/Ksword5.1/UI/ThemeAccentIcon.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QMenu>
#include <QPushButton>
#include <QResource>
#include <QTabWidget>
#include <QTreeWidget>
#include <algorithm>
#include <array>
#include <iostream>

namespace
{
    int assertionCount = 0; // 本轮真实行为断言总数。
    int failureCount = 0;   // 决定最终退出码的失败数量。

    // drainEvents 完成排队的图标分片与重绘，不运行生产采样或真实系统操作。
    void drainEvents()
    {
        for (int round = 0; round < 12; ++round)
        {
            QApplication::processEvents();
        }
    }

    // flatIcon 创建独立测试资源，color 决定原始颜色，返回可检查的单色图标。
    QIcon flatIcon(const QColor& color)
    {
        QPixmap pixels(24, 24);
        pixels.fill(color);
        return QIcon(pixels);
    }

    // iconColor 读取测试方块中央真实像素，避免仅比较缓存键而漏掉颜色错误。
    QColor iconColor(const QIcon& icon)
    {
        return icon.pixmap(QSize(24, 24)).toImage().pixelColor(12, 12);
    }

    // 普通图标的期望色明确覆盖中性三底，原色已满足对比时仍逐像素保留。
    QColor normalIconColor(const QColor& seed)
    {
        const QColor surfaces[] = {KswordTheme::SurfaceColor(),
            KswordTheme::SurfaceAltColor(), KswordTheme::SurfaceMutedColor()};
        return KswordTheme::EnsureTextContrastForBackgrounds(seed, surfaces, 3, 3.0);
    }

    // testRoleColors 对独立记录的原配色和多个主题种子验证 RGB 派生及对比度。
    void testRoleColors()
    {
        const std::array<QColor, 13> originalSeeds{{
            QColor(67, 160, 255), QColor(184, 99, 255), QColor(47, 125, 50),
            QColor(217, 119, 6), QColor(0, 188, 212), QColor(245, 158, 11),
            QColor(220, 50, 47), QColor(0, 150, 136), QColor(63, 81, 181),
            QColor(121, 85, 72), QColor(139, 195, 74), QColor(96, 125, 139),
            QColor(121, 76, 210)
        }};
        KswordTheme::SetMainBackgroundColor(QString());
        KswordTheme::SetPrimaryAccentColor(QString());
        for (int roleIndex = 0; roleIndex < static_cast<int>(originalSeeds.size()); ++roleIndex)
        {
            const auto role = static_cast<KswordTheme::AccentRole>(roleIndex);
            Require(KswordTheme::AccentSeed(role) == originalSeeds[roleIndex],
                "Default accent roles must preserve their exact original pixels");
        }

        const std::array<QString, 8> themes{{
            QStringLiteral("#C08040"), QStringLiteral("#20CC80"),
            QStringLiteral("#EE3311"), QStringLiteral("#8020EE"),
            QStringLiteral("#000000"), QStringLiteral("#FFFFFF"),
            QStringLiteral("#002040"), QStringLiteral("#D0E0FF")
        }};
        for (const QString& themeText : themes)
        {
            const QColor seed(themeText);
            KswordTheme::SetPrimaryAccentColor(themeText);
            for (int roleIndex = 0; roleIndex < static_cast<int>(originalSeeds.size()); ++roleIndex)
            {
                const QColor original = originalSeeds[roleIndex];
                const QColor expected(
                    std::clamp(seed.red() + original.red() - 67, 0, 255),
                    std::clamp(seed.green() + original.green() - 160, 0, 255),
                    std::clamp(seed.blue() + original.blue() - 255, 0, 255));
                Require(KswordTheme::AccentSeed(static_cast<KswordTheme::AccentRole>(roleIndex)) == expected,
                    "Every accent role must derive from the current theme seed");
            }
            for (const bool darkMode : { false, true })
            {
                KswordTheme::SetDarkModeEnabled(darkMode);
                for (int roleIndex = 0; roleIndex < 12; ++roleIndex)
                {
                    Require(KswordTheme::ContrastRatio(KswordTheme::PerformanceColor(
                        static_cast<KswordTheme::PerformanceRole>(roleIndex)), KswordTheme::SurfaceColor()) >= 2.999,
                        "Performance lines must remain visible with extreme theme seeds");
                    Require(KswordTheme::ContrastRatio(KswordTheme::TimelineColor(
                        static_cast<KswordTheme::TimelineRole>(roleIndex)), KswordTheme::SurfaceColor()) >= 2.999,
                        "Timeline roles must remain visible with extreme theme seeds");
                }
                const std::array<QColor, 4> surfaces{{ KswordTheme::MainBackgroundColor(),
                    KswordTheme::SurfaceColor(), KswordTheme::SurfaceAltColor(), KswordTheme::SurfaceMutedColor() }};
                const std::array<QColor, 3> statusText{{ KswordTheme::SuccessColor(),
                    KswordTheme::WarningColor(), KswordTheme::ErrorColor() }};
                for (const QColor& text : statusText)
                {
                    for (const QColor& surface : surfaces)
                    {
                        Require(KswordTheme::ContrastRatio(text, surface) >= 4.499,
                            "Semantic text must remain readable on every neutral surface");
                    }
                }
                const QColor glyph = KswordTheme::ControlGlyphColor(KswordTheme::SurfaceAltColor());
                Require(KswordTheme::ContrastRatio(glyph, KswordTheme::SurfaceAltColor()) >= 2.999,
                    "Themed control glyph must preserve non-text contrast");
            }
        }
    }

    // testGlyphCache 检查真实 SVG 重着色、空间路径、缓存身份及损坏缓存修复。
    void testGlyphCache(const QString& repositoryRoot)
    {
        const QString source = QStringLiteral(":/Icon/ks_control_check_white.svg");
        const QString cache = QDir(repositoryRoot).filePath(QStringLiteral(".codex-build-logs"));
        const QColor themeA(QStringLiteral("#C08040"));
        const QColor themeB(QStringLiteral("#20CC80"));
        const QString pathA = ks::ui::ThemedControlGlyphPath(source, themeA, cache);
        QFile outputA(pathA);
        Require(pathA != source && outputA.open(QIODevice::ReadOnly), "Control SVG must be cached as a readable file");
        const QByteArray pixelsA = outputA.readAll();
        outputA.close();
        Require(pixelsA.contains("stroke=\"#C08040\"") && pixelsA.contains("fill=\"none\""),
            "SVG tint must preserve transparent geometry and replace its foreground");
        Require(ks::ui::ThemedControlGlyphPath(source, themeA, cache) == pathA,
            "An unchanged SVG and theme must reuse the same cache identity");
        Require(ks::ui::ThemedControlGlyphPath(source, themeB, cache) != pathA,
            "A new theme must not reuse old SVG colors");
        QFile damagedFile(pathA);
        Require(damagedFile.open(QIODevice::WriteOnly), "Fixture must be able to corrupt its own cache file");
        damagedFile.write("damaged");
        damagedFile.close();
        (void)ks::ui::ThemedControlGlyphPath(source, themeA, cache);
        QFile repairedFile(pathA);
        Require(repairedFile.open(QIODevice::ReadOnly) && repairedFile.readAll() == pixelsA,
            "A damaged glyph cache must be repaired atomically");
    }

    // testRuntimeIcons 检查先 Polish 后设图标、动作替换、标签替换与默认恢复。
    void testRuntimeIcons(QApplication& application)
    {
        auto& manager = ks::ui::SvgThemeIconManager::instance();
        const QColor themeA(QStringLiteral("#C08040"));
        const QColor themeB(QStringLiteral("#20CC80"));
        manager.applyToApplication(&application, themeA, false);
        QPushButton button;
        button.ensurePolished();
        drainEvents();
        const QIcon original = flatIcon(QColor(67, 160, 255));
        button.setIcon(original);
        button.show();
        drainEvents();
        Require(iconColor(button.icon()) == normalIconColor(themeA), "A late button icon must follow the current theme");
        button.setIcon(flatIcon(Qt::red));
        button.repaint();
        drainEvents();
        Require(iconColor(button.icon()) == normalIconColor(themeA), "Runtime setIcon must not reset a button to its source color");

        QMenu menu;
        QAction* action = menu.addAction(original, QStringLiteral("fixture"));
        drainEvents();
        Require(iconColor(action->icon()) == normalIconColor(themeA), "A new menu action must be themed");
        action->setIcon(flatIcon(Qt::green));
        drainEvents();
        Require(iconColor(action->icon()) == normalIconColor(themeA), "ActionChanged must refresh a replaced icon");

        QTabWidget tabs;
        tabs.addTab(new QWidget, original, QStringLiteral("fixture"));
        tabs.show();
        drainEvents();
        tabs.setTabIcon(0, flatIcon(Qt::yellow));
        tabs.repaint();
        drainEvents();
        Require(iconColor(tabs.tabIcon(0)) == normalIconColor(themeA), "Runtime tab icon replacement must be themed");
        manager.applyToApplication(&application, themeB, false);
        drainEvents();
        Require(iconColor(button.icon()) == normalIconColor(themeB) && iconColor(action->icon()) == normalIconColor(themeB),
            "Switching themes must invalidate the old tinted icon cache");
        const qint64 stableKey = button.icon().cacheKey();
        button.repaint();
        drainEvents();
        Require(button.icon().cacheKey() == stableKey, "Repainting a themed icon must be idempotent");
        manager.applyToApplication(&application, KswordTheme::DefaultPrimaryAccentColor(), true);
        drainEvents();
        Require(iconColor(button.icon()) == QColor(Qt::red), "Restoring defaults must restore the latest button source");
        Require(iconColor(action->icon()) == QColor(Qt::green), "Restoring defaults must restore the latest action source");
        Require(iconColor(tabs.tabIcon(0)) == QColor(Qt::yellow), "Restoring defaults must restore the latest tab source");
    }

    // testModelIcons 检查模型中缓存同一个 QIcon 后，无需遍历条目就能换色及恢复默认。
    void testModelIcons()
    {
        QTreeWidget tree;
        auto* item = new QTreeWidgetItem(&tree);
        const QIcon icon = ks::ui::MakeThemeAccentIcon(flatIcon(KswordTheme::DefaultPrimaryAccentColor()));
        item->setIcon(0, icon);
        for (const QString& seed : { QStringLiteral("#C08040"), QStringLiteral("#20CC80"), QString() })
        {
            KswordTheme::SetPrimaryAccentColor(seed);
            Require(iconColor(item->icon(0)) == normalIconColor(KswordTheme::PrimaryAccentColor()),
                "A model icon cached during a custom theme must follow later themes and default restoration");
            // 固定位图不保证放大；使用提供足够像素的源来测试引擎自身的 DPR 合约。
            QPixmap largeSource(72, 72);
            largeSource.fill(KswordTheme::DefaultPrimaryAccentColor());
            const QPixmap scaled = ks::ui::MakeThemeAccentIcon(QIcon(largeSource)).pixmap(QSize(24, 24), 2.0);
            Require(scaled.devicePixelRatio() == 2.0 && scaled.size() == QSize(48, 48),
                "The dynamic icon engine must retain logical size at high DPI");
        }
    }

    // 极端同色种子验证真实模型/管理器图标的多态像素，并覆盖只换背景的同一缓存QIcon。
    void testExtremeIconContrast(QApplication& application)
    {
        const bool previousDark = KswordTheme::IsDarkModeEnabled(); // 退出时还原夹具主题。
        const QColor previousAccent = KswordTheme::PrimaryAccentColor();
        const QColor previousBackground = KswordTheme::CustomMainBackgroundColor;
        auto& manager = ks::ui::SvgThemeIconManager::instance(); // 使用实际应用级增量着色路径。
        QPushButton button;
        const QIcon source = flatIcon(KswordTheme::DefaultPrimaryAccentColor());
        button.setIcon(source);
        button.show();
        const QIcon modelIcon = ks::ui::MakeThemeAccentIcon(source); // 同一模型图标跨所有主题不替换。
        for (const bool dark : {false, true})
        {
            KswordTheme::SetDarkModeEnabled(dark);
            for (const QString& seed : {QStringLiteral("#000000"), QStringLiteral("#FFFFFF"),
                QStringLiteral("#20CC80"), QStringLiteral("#8020EE")})
            {
                KswordTheme::SetPrimaryAccentColor(seed);
                manager.applyToApplication(&application, QColor(seed), false);
                drainEvents();
                const qint64 managedKey = button.icon().cacheKey(); // 改背景不能依赖setIcon重新分配。
                for (const QString& background : {QStringLiteral("#000000"), QStringLiteral("#FFFFFF"),
                    QStringLiteral("#646464"), QStringLiteral("#E0FFE0")})
                {
                    KswordTheme::SetMainBackgroundColor(background);
                    const QColor surfaces[] = {KswordTheme::SurfaceColor(),
                        KswordTheme::SurfaceAltColor(), KswordTheme::SurfaceMutedColor()};
                    for (const QIcon& icon : {modelIcon, button.icon()})
                    {
                        const QColor normal = iconColor(icon); // 中央实心像素，不检查缓存元数据代替颜色。
                        for (const QColor& surface : surfaces)
                        {
                            Require(KswordTheme::ContrastRatio(normal, surface) >= 2.999,
                                "Actual normal icon remains visible on each neutral surface with extreme seeds");
                        }
                        const QColor active = icon.pixmap(QSize(24, 24), QIcon::Active, QIcon::Off)
                            .toImage().pixelColor(12, 12);
                        Require(KswordTheme::ContrastRatio(active, KswordTheme::ControlAccentColor()) >= 2.999,
                            "Actual active icon contrasts with button emphasis background");
                        for (const QIcon::Mode mode : {QIcon::Normal, QIcon::Selected})
                        {
                            const QColor checked = icon.pixmap(QSize(24, 24), mode, QIcon::On)
                                .toImage().pixelColor(12, 12);
                            Require(KswordTheme::ContrastRatio(checked, KswordTheme::PrimaryAccentColor()) >= 2.999,
                                "Actual selected or checked icon contrasts with the selected background");
                        }
                        const QImage disabled = icon.pixmap(QSize(24, 24), QIcon::Disabled, QIcon::Off).toImage();
                        Require(!disabled.isNull() && disabled.pixelColor(12, 12).alpha() > 0,
                            "Disabled icon retains its source silhouette after contrast calibration");
                    }
                    Require(button.icon().cacheKey() == managedKey,
                        "Changing background recalibrates the existing managed icon without replacing its cache identity");
                }
            }
        }
        manager.applyToApplication(&application, KswordTheme::DefaultPrimaryAccentColor(), true);
        drainEvents();
        Require(iconColor(button.icon()) == KswordTheme::DefaultPrimaryAccentColor(),
            "Contrast calibration still restores the untouched original icon");
        KswordTheme::SetDarkModeEnabled(previousDark);
        KswordTheme::SetPrimaryAccentColor(previousAccent.name(QColor::HexRgb));
        KswordTheme::SetMainBackgroundColor(previousBackground.isValid()
            ? previousBackground.name(QColor::HexRgb) : QString());
    }
}

void Require(const bool condition, const char* message)
{
    ++assertionCount;
    if (!condition)
    {
        ++failureCount;
        std::cerr << "FAIL: " << message << '\n';
    }
}

// main 只创建测试控件，资源和缓存由显式仓库参数提供；失败返回非零。
int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    if (argc != 3 || !QResource::registerResource(QString::fromLocal8Bit(argv[2])))
    {
        std::cerr << "Fixture resource arguments are invalid\n";
        return 2;
    }
    testRoleColors();
    testGlyphCache(QString::fromLocal8Bit(argv[1]));
    testRuntimeIcons(application);
    testModelIcons();
    testExtremeIconContrast(application);
    TestDockThemeIcons();
    TestPerformanceChartTheme();
    std::cout << "THEME_RECOVERY_ASSERTIONS=" << assertionCount << '\n'
        << "THEME_RECOVERY_FAILURES=" << failureCount << '\n';
    return failureCount == 0 ? 0 : 1;
}
