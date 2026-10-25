// Links production FlatButtonTheme, MainSurfaceStyle and both Hex glyph sources.
// Only ThemeBinding scheduling is replaced; all widgets, input and painting are Qt.
#include "UI/FlatButtonTheme.h"
#include "UI/ThemeBinding.h"
#include "UI/MainSurfaceStyle.h"
#include "UI/MemoryWorkbench/HexInspectorWidgets.h"
#include "UI/MemoryWorkbench/HexViewWidgets.h"
#include "theme.h"
#include <QApplication>
#include <QDir>
#include <QFocusEvent>
#include <QDialog>
#include <QLineEdit>
#include <QPainter>
#include <QImage>
#include <QKeyEvent>
#include <QMenu>
#include <QPushButton>
#include <QStyleFactory>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QWindow>
#include <cstdio>
#include <memory>

namespace ks::ui {
// A deterministic standalone binding adapter; production binding queues and
// full main-window lifecycle are outside this fixture's validation boundary.
bool BindWidgetTheme(QWidget* widget, std::function<void()> refresh, ThemePalettePolicy, bool)
{
    widget->setProperty("fixture_theme_binding", true);
    refresh();
    return true;
}
bool HasWidgetThemeBinding(const QWidget* widget)
{ return widget->property("fixture_theme_binding").toBool(); }
void PrepareWidgetThemeBindingsForShow(QWidget*) {}
}

namespace {
int checks = 0, failures = 0;
QString caseName, outputDirectory;
void check(bool ok, const char* message)
{
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL [%s] %s\n", qPrintable(caseName), message);
    }
}
void flush() { for (int i = 0; i != 6; ++i) QApplication::processEvents(); }
void move(QWidget& target, QPoint position)
{
    auto* window = target.window()->windowHandle();
    QTest::mouseMove(window, target.mapTo(target.window(), position), 1);
    flush();
}
void click(QAbstractButton& button)
{
    QTest::mouseClick(button.window()->windowHandle(), Qt::LeftButton, Qt::NoModifier,
        button.mapTo(button.window(), button.rect().center()), 1);
    flush();
}
QColor background(QAbstractButton& button)
{
    flush();
    const QPoint point = button.mapTo(button.window(), QPoint(8, button.height() - 8));
    return button.window()->grab().toImage().pixelColor(point);
}
void iconMatches(QAbstractButton& button, QColor painted, QIcon::Mode mode = QIcon::Normal)
{
    QColor predicted;
    check(ks::ui::TryGetFlatButtonBackground(&button, mode, QIcon::Off, &predicted),
        "shared icon background lookup was unavailable");
    check(predicted.rgb() == painted.rgb(), "icon background disagrees with the actual painted pixel");
}
void screenshot(QWidget& widget, const QString& suffix)
{
    if (!outputDirectory.isEmpty())
        widget.grab().save(outputDirectory + QLatin1Char('/') + caseName + suffix + QStringLiteral(".png"));
}

void exercise(bool dark, bool tool, ks::ui::FlatButtonTone tone,
    ks::ui::FlatButtonAppearance appearance, bool shared)
{
    using namespace ks::ui;
    caseName = QStringLiteral("%1-%2-tone%3-view%4-%5")
        .arg(dark ? "dark" : "light", tool ? "tool" : "push")
        .arg(int(tone)).arg(int(appearance)).arg(shared ? "shared" : "local");
    KswordTheme::SetDarkModeEnabled(dark);
    QPalette palette = QApplication::palette();
    palette.setColor(QPalette::Window, dark ? QColor("#20242b") : QColor("#f5f6f8"));
    palette.setColor(QPalette::Base, palette.color(QPalette::Window));
    palette.setColor(QPalette::AlternateBase, dark ? QColor("#38404b") : QColor("#dfe3e8"));
    palette.setColor(QPalette::Text, dark ? Qt::white : Qt::black);
    palette.setColor(QPalette::Highlight, QColor("#2563eb"));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    QApplication::setPalette(palette);
    QWidget host;
    host.setPalette(palette);
    host.resize(520, 180);
    host.setFocusPolicy(Qt::StrongFocus);
    QPushButton previous("Previous", &host), next("Next", &host);
    previous.setGeometry(10, 90, 120, 40);
    next.setGeometry(160, 90, 120, 40);
    std::unique_ptr<QAbstractButton> button = tool
        ? std::unique_ptr<QAbstractButton>(new QToolButton(&host))
        : std::unique_ptr<QAbstractButton>(new QPushButton(&host));
    button->setGeometry(10, 10, 160, 50);
    button->setText(QStringLiteral("Action"));
    button->setFocusPolicy(Qt::StrongFocus);
    QWidget::setTabOrder(&previous, button.get());
    QWidget::setTabOrder(button.get(), &next);
    if (shared) {
        host.setProperty("ksword_main_surface_hierarchy", true);
        const QString style = BuildSharedMainSurfaceButtonStyle();
        host.setStyleSheet(style);
        RegisterSharedMainSurfaceButtonStyle(&host, style);
    }
    ApplyFlatButtonTheme(button.get(), tone, appearance);
    host.show(); host.activateWindow(); flush();
    host.setFocus(Qt::OtherFocusReason);
    move(host, QPoint(470, 150));
    const QColor normal = background(*button);
    iconMatches(*button, normal);
    if (shared) check(button->property("ksword_flat_button_shared_owner").toBool(),
        "shared main-surface test did not exercise the shared QSS owner");
    move(*button, button->rect().center());
    check(button->underMouse(), "window input did not generate a real hover state");
    const QColor hover = background(*button);
    check(normal != hover, "hover painting did not differ from normal painting");
    iconMatches(*button, hover);
    int clicks = 0;
    QObject::connect(button.get(), &QAbstractButton::clicked, &host, [&clicks] { ++clicks; });
    click(*button); move(host, QPoint(470, 150));
    check(clicks == 1, "mouse input lost the real click action");
    check(button->hasFocus() && !button->underMouse() && !button->isChecked(),
        "mouse reproduction did not retain focus outside the unchecked button");
    check(background(*button) == normal, "mouse click kept the hover fill after the pointer left");
    iconMatches(*button, normal);
    iconMatches(*button, normal, QIcon::Active);
    screenshot(host, QStringLiteral("-mouse"));

    previous.setFocus(Qt::OtherFocusReason);
    QTest::keyClick(&previous, Qt::Key_Tab, Qt::NoModifier, 1); flush();
    check(button->hasFocus(), "Tab did not move keyboard focus to the action");
    check(background(*button) != normal, "Tab focus lost its visible keyboard hint");
    QColor keyboardRecipe;
    check(TryGetFlatButtonBackground(button.get(), QIcon::Normal, QIcon::Off, &keyboardRecipe)
        && keyboardRecipe.rgb() == hover.rgb(), "keyboard icon recipe lost its hover contrast surface");
    screenshot(host, QStringLiteral("-keyboard"));
    next.setFocus(Qt::OtherFocusReason);
    QTest::keyClick(&next, Qt::Key_Backtab, Qt::ShiftModifier, 1); flush();
        check(button->hasFocus() && background(*button) != normal,
        "Shift+Tab did not retain a visible keyboard hint");
    // Mouse input must change origin even if this button already holds focus.
    move(*button, button->rect().center()); click(*button); move(host, QPoint(470, 150));
    check(button->hasFocus() && background(*button) == normal,
        "mouse click on an already keyboard-focused button kept the keyboard fill");
    QTest::keyClick(button.get(), Qt::Key_Space, Qt::NoModifier, 1); flush();
    check(clicks == 3 && button->hasFocus() && background(*button) != normal,
        "keyboard activation on a mouse-focused button lost its action or focus hint");
    previous.setFocus(Qt::OtherFocusReason); button->setFocus(Qt::ShortcutFocusReason); flush();
        check(button->hasFocus() && background(*button) != normal,
        "shortcut focus lost its visible keyboard hint");

    // QWidget's QTest overload delivers a direct MouseButtonDblClick; unlike
    // the window overload it does not hide this case behind an initial press.
    move(*button, button->rect().center());
    QTest::mouseDClick(button.get(), Qt::LeftButton, Qt::NoModifier, button->rect().center(), 1);
    QTest::mouseRelease(button.get(), Qt::LeftButton, Qt::NoModifier, button->rect().center(), 1);
    move(host, QPoint(470, 150));
    check(button->hasFocus() && background(*button) == normal,
        "direct double-click kept the old keyboard focus highlight after the pointer left");
    iconMatches(*button, normal, QIcon::Active);
    const int beforeKeyboard = clicks;
    QTest::keyClick(button.get(), Qt::Key_Space, Qt::NoModifier, 1); flush();
    check(clicks == beforeKeyboard + 1 && button->hasFocus() && background(*button) != normal,
        "keyboard activation after double-click lost its action or visible focus hint");

    button->setCheckable(true); button->setChecked(true); flush();
    previous.setFocus(Qt::OtherFocusReason); button->setFocus(Qt::MouseFocusReason); flush();
    const QColor checked = background(*button);
    iconMatches(*button, checked);
    button->clearFocus(); move(host, QPoint(470, 150));
    check(button->isChecked() && background(*button) == checked,
        "real checked state was erased together with mouse focus emphasis");
    button->setEnabled(false); flush();
    const QColor disabled = background(*button);
    button->clearFocus(); flush();
    check(!button->isEnabled() && background(*button) == disabled, "disabled appearance depended on focus");
    button->setEnabled(true); button->setChecked(false); button->setCheckable(false);

    if (tool) {
        auto* toolButton = static_cast<QToolButton*>(button.get());
        QMenu menu;
        menu.addAction(QStringLiteral("One action"));
        toolButton->setMenu(&menu);
        toolButton->setPopupMode(QToolButton::InstantPopup);
        previous.setFocus(Qt::OtherFocusReason);
        move(*button, button->rect().center());
        QTimer::singleShot(0, &menu, &QMenu::close);
        click(*button); move(host, QPoint(470, 150));
        // Offscreen has no window manager to reactivate the owner; deliver the
        // real ActiveWindowFocusReason restoration that the OS normally supplies.
        host.activateWindow(); flush(); button->setFocus(Qt::ActiveWindowFocusReason); flush();
        check(!menu.isVisible() && !toolButton->isDown(), "mouse popup did not close and release its pressed state");
        check(background(*button) == normal, "mouse popup restoration kept a sticky hover fill");
        iconMatches(*button, normal);
        previous.setFocus(Qt::OtherFocusReason); button->setFocus(Qt::TabFocusReason); flush();
        QTimer::singleShot(0, &menu, &QMenu::close);
        toolButton->showMenu(); flush();
        host.activateWindow(); flush(); button->setFocus(Qt::ActiveWindowFocusReason); flush();
        check(button->hasFocus() && background(*button) != normal,
            "keyboard popup restoration lost the original keyboard hint");
        QColor recipe;
        check(TryGetFlatButtonBackground(button.get(), QIcon::Normal, QIcon::Off, &recipe) && recipe.rgb() == hover.rgb(),
            "keyboard popup changed the icon contrast recipe");
    }
}

void settingsDialogRestoration(bool dark)
{
    // Matches ProcessDock's non-checkable 28px settings QPushButton and reused
    // nonmodal dialog, without creating processes or the production main GUI.
    using namespace ks::ui;
    caseName = dark ? QStringLiteral("dark-settings-dialog") : QStringLiteral("light-settings-dialog");
    KswordTheme::SetDarkModeEnabled(dark);
    QWidget host;
    host.resize(340, 140); host.setFocusPolicy(Qt::StrongFocus);
    QPushButton previous("Previous", &host), gear(&host);
    previous.setGeometry(70, 20, 100, 30);
    gear.move(20, 20);
    QPixmap glyph(16, 16); glyph.fill(Qt::transparent);
    { QPainter painter(&glyph); painter.setPen(Qt::white); painter.drawEllipse(3, 3, 10, 10); }
    gear.setIcon(QIcon(glyph));
    KswordTheme::ApplyCompactIconButtonMetrics(&gear);
    ApplyFlatButtonTheme(&gear, FlatButtonTone::Neutral, FlatButtonAppearance::Solid);
    QWidget::setTabOrder(&previous, &gear);
    QDialog dialog(&host);
    dialog.resize(220, 100);
    QLineEdit field(&dialog); field.setGeometry(10, 10, 180, 30);
    QObject::connect(&gear, &QPushButton::clicked, &dialog, [&] {
        dialog.show(); dialog.raise(); dialog.activateWindow(); field.setFocus(Qt::ActiveWindowFocusReason);
    });
    host.show(); host.activateWindow(); flush();
    host.setFocus(Qt::OtherFocusReason); move(host, QPoint(300, 110));
    const QColor normal = background(gear);
    move(gear, gear.rect().center()); click(gear);
    check(dialog.isVisible(), "settings action did not show its nonmodal dialog");
    move(host, QPoint(300, 110)); dialog.close(); flush();
    host.activateWindow(); flush(); gear.setFocus(Qt::ActiveWindowFocusReason); flush();
    check(gear.hasFocus() && !gear.underMouse() && !gear.isChecked(),
        "settings restoration did not retain the mouse-focused unchecked action");
    check(background(gear) == normal, "closing mouse-opened settings kept the settings action highlighted");
    iconMatches(gear, normal);
    screenshot(host, QStringLiteral("-mouse-restored"));
    previous.setFocus(Qt::OtherFocusReason); QTest::keyClick(&previous, Qt::Key_Tab, Qt::NoModifier, 1); flush();
    check(gear.hasFocus() && background(gear) != normal, "settings action lost its Tab focus hint");
    QTest::keyClick(&gear, Qt::Key_Space, Qt::NoModifier, 1); flush();
    check(dialog.isVisible(), "keyboard activation did not reopen the same settings dialog");
    dialog.close(); flush(); host.activateWindow(); flush(); gear.setFocus(Qt::ActiveWindowFocusReason); flush();
    check(gear.hasFocus() && background(gear) != normal,
        "closing keyboard-opened settings lost the original keyboard focus hint");
    screenshot(host, QStringLiteral("-keyboard-restored"));
}

void paintedGlyphFocus(bool dark, bool inspector)
{
    using namespace ks::ui;
    caseName = QStringLiteral("%1-hex-%2").arg(dark ? "dark" : "light", inspector ? "inspector" : "toolbar");
    KswordTheme::SetDarkModeEnabled(dark);
    QWidget host;
    host.resize(340, 140); host.setFocusPolicy(Qt::StrongFocus);
    QPushButton previous("Previous", &host); previous.setGeometry(100, 10, 110, 40);
    std::unique_ptr<QAbstractButton> button = inspector
        ? std::unique_ptr<QAbstractButton>(new HexInspectorGlyphButton(HexInspectorGlyphButton::Glyph::Pointer32, &host))
        : std::unique_ptr<QAbstractButton>(new HexViewGlyphButton(HexViewGlyphButton::Glyph::Find, &host));
    button->setGeometry(15, 15, 36, 36); button->setFocusPolicy(Qt::StrongFocus); button->setCheckable(false);
    QWidget::setTabOrder(&previous, button.get());
    host.show(); host.activateWindow(); flush(); host.setFocus(Qt::OtherFocusReason);
    move(host, QPoint(300, 110));
    const QImage normal = button->grab().toImage();
    move(*button, button->rect().center()); click(*button); move(host, QPoint(300, 110));
    check(button->hasFocus() && !button->underMouse(), "glyph mouse path did not retain actual focus");
    check(button->grab().toImage() == normal, "painted glyph retained a mouse focus outline or hover surface");
    previous.setFocus(Qt::OtherFocusReason); QTest::keyClick(&previous, Qt::Key_Tab, Qt::NoModifier, 1); flush();
    check(button->hasFocus(), "Tab did not reach the painted glyph");
    check(button->grab().toImage() != normal, "painted glyph lost the actual keyboard focus outline");
    if (inspector)
        check(!button->property("ksword_flat_button_managed").toBool(),
            "pure QAbstractButton focus tracking unexpectedly installed push/tool QSS");
    screenshot(host, QStringLiteral("-keyboard"));
    move(*button, button->rect().center()); click(*button); move(host, QPoint(300, 110));
    check(button->grab().toImage() == normal, "mouse activation did not retire painted glyph keyboard emphasis");
    button->setCheckable(true); button->setChecked(true); flush();
    const QImage checked = button->grab().toImage();
    button->clearFocus(); flush();
    check(button->isChecked() && button->grab().toImage() == checked,
        "painted glyph checked state changed with mouse focus");
}
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    if (argc > 1) { outputDirectory = QString::fromLocal8Bit(argv[1]); QDir().mkpath(outputDirectory); }
    ks::ui::InstallGlobalFlatButtonTheme(&app);
    using namespace ks::ui;
    for (bool dark : {false, true}) {
        for (auto tone : {FlatButtonTone::Neutral, FlatButtonTone::Accent, FlatButtonTone::Danger, FlatButtonTone::Success})
            exercise(dark, false, tone, FlatButtonAppearance::Solid, false);
        exercise(dark, false, FlatButtonTone::Neutral, FlatButtonAppearance::Flat, false);
        exercise(dark, true, FlatButtonTone::Neutral, FlatButtonAppearance::Auto, false);
        exercise(dark, true, FlatButtonTone::Neutral, FlatButtonAppearance::Solid, false);
        exercise(dark, true, FlatButtonTone::Neutral, FlatButtonAppearance::Navigation, false);
        exercise(dark, false, FlatButtonTone::Neutral, FlatButtonAppearance::Auto, true);
        settingsDialogRestoration(dark);
        paintedGlyphFocus(dark, true);
        paintedGlyphFocus(dark, false);
    }
    std::printf("BUTTON_FOCUS_CHECKS=%d FAILURES=%d REAL_QTWIDGETS=1 MAIN_GUI=0\n", checks, failures);
    return failures ? 1 : 0;
}
