// Isolated QtWidgets integration, not the KSword main GUI. Links the real
// StandardWidgetTheme, FlatButtonTheme, MainSurfaceStyle, ThemeBinding queue and
// GlobalUiBaseStyle. The application palette below is extracted verbatim from
// MainWindow. Only unused non-button roles and the non-Windows animation query
// are adapted by the runner; no button, focus, palette or queue logic is copied.
#include "UI/StandardWidgetTheme.h"
#include "UI/FlatButtonTheme.h"
#include "UI/GlobalUiBaseStyle.h"
#include "UI/MainSurfaceStyle.h"
#include "UI/ThemeBinding.h"
#include "UI/PrimaryPageStyle.h"
#include "UI/PageControlStyle.h"
#include "UI/ThemeControlGlyphs.h"
#include "UI/ThemeStatusRole.h"
#include "UI/ThemedSlider.h"
#include "UI/ParameterWorkspace.h"
#include "theme.h"

#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QFocusEvent>
#include <QHBoxLayout>
#include <QImage>
#include <QMainWindow>
#include <QPointer>
#include <QPushButton>
#include <QStyleOption>
#include <QTest>
#include <QVBoxLayout>
#include <QWindow>

#include <cstdio>
#include <vector>

namespace ks::ui
{
    // These components have no instances in this fixture. Their references in
    // the real default enrollment/global stylesheet remain linkable without
    // bringing unrelated page navigation, translation, SVG files or group UI in.
    QString WithStandaloneInputGeometry(QString style) { return style; }
    QString WithParameterInputGeometry(QString style) { return style; }
    void StyleParameterSection(QGroupBox*) {}
    void StylePrimaryGroup(QGroupBox*) {}
    void StylePageTabs(QTabWidget*) {}
    void StylePageTabBar(QTabBar*) {}
    QString BuildPageTabsStyleSheet(bool) { return {}; }
    QString BuildStandardSliderStyle() { return {}; }
    QString BuildStatusRoleStyleRules() { return {}; }
    QString ThemedControlGlyphPath(const QString&, const QColor&, const QString&) { return {}; }
}

namespace
{
#include "ButtonIntegrationPalette.inc"

    int checks = 0;
    int failures = 0;
    QString caseName;
    QString outputDirectory;

    QString name(const QWidget* widget)
    {
        return widget->property("fixture_button_label").toString();
    }

    void check(bool condition, const char* message)
    {
        ++checks;
        if (!condition)
        {
            ++failures;
            std::fprintf(stderr, "FAIL [%s] %s\n", qPrintable(caseName), message);
        }
    }

    void pump()
    {
        for (int pass = 0; pass < 12; ++pass) QApplication::processEvents();
    }

    class FocusTrace final : public QObject
    {
    public:
        bool eventFilter(QObject* object, QEvent* event) override
        {
            if (event->type() != QEvent::FocusIn && event->type() != QEvent::FocusOut) return false;
            auto* button = qobject_cast<QAbstractButton*>(object);
            if (!button || name(button).isEmpty()) return false;
            QStyleOption option;
            option.initFrom(button);
            std::printf("BUTTON_INTEGRATION_FOCUS case=%s name=%s event=%s reason=%d native_keyboard=%d\n",
                qPrintable(caseName), qPrintable(name(button)),
                event->type() == QEvent::FocusIn ? "in" : "out",
                int(static_cast<QFocusEvent*>(event)->reason()),
                int(option.state.testFlag(QStyle::State_KeyboardFocusChange)));
            return false;
        }
    };

    struct Surface
    {
        QMainWindow host;
        QPushButton* first = nullptr;
        QPushButton* second = nullptr;
        QPushButton* third = nullptr;
        int firstClicks = 0;
        int secondClicks = 0;
        int thirdClicks = 0;

        Surface()
        {
            using namespace ks::ui;
            host.resize(540, 250);
            host.setProperty("ksword_main_surface_hierarchy", true);
            const QString style = BuildMainSurfaceStyle(false);
            host.setStyleSheet(style);
            RegisterSharedMainSurfaceButtonStyle(&host, style);
            auto* body = new QWidget(&host);
            host.setCentralWidget(body);
            auto* layout = new QVBoxLayout(body);
            first = new QPushButton(QStringLiteral("Settings action"), body);
            second = new QPushButton(QStringLiteral("Automatic action"), body);
            third = new QPushButton(QStringLiteral("Next action"), body);
            for (QPushButton* button : {first, second, third})
            {
                button->setFixedSize(180, 36);
                layout->addWidget(button, 0, Qt::AlignLeft);
            }
            first->setProperty("fixture_button_label", QStringLiteral("first"));
            second->setProperty("fixture_button_label", QStringLiteral("second"));
            third->setProperty("fixture_button_label", QStringLiteral("third"));
            // ProcessDock's settings action declares Solid. The other actions
            // use actual StandardWidgetTheme automatic enrollment/shared QSS.
            ApplyFlatButtonTheme(first, FlatButtonTone::Neutral, FlatButtonAppearance::Solid);
            QWidget::setTabOrder(first, second);
            QWidget::setTabOrder(second, third);
            QObject::connect(first, &QPushButton::clicked, &host, [this] { ++firstClicks; });
            QObject::connect(second, &QPushButton::clicked, &host, [this] { ++secondClicks; });
            QObject::connect(third, &QPushButton::clicked, &host, [this] { ++thirdClicks; });
            layout->addStretch();
            host.show();
            host.activateWindow();
            pump();
        }

        void away()
        {
            // The native cursor can remain over the opener when an offscreen
            // top-level closes. Deliver actual movement at two distinct blank
            // points instead of relying on a repeated identical cursor point.
            QTest::mouseMove(host.windowHandle(), QPoint(host.width() - 25, host.height() - 15), 1);
            QTest::mouseMove(host.windowHandle(), QPoint(host.width() - 15, host.height() - 15), 1);
            pump();
        }
    };

    void mouseClick(QAbstractButton* button)
    {
        QTest::mouseClick(button->window()->windowHandle(), Qt::LeftButton, Qt::NoModifier,
            button->mapTo(button->window(), button->rect().center()), 1);
        pump();
    }

    QColor background(QAbstractButton* button)
    {
        pump();
        const QPoint point = button->mapTo(button->window(), QPoint(8, button->height() - 8));
        return button->window()->grab().toImage().pixelColor(point);
    }

    void capture(QWidget& window, const QString& suffix)
    {
        if (!outputDirectory.isEmpty())
            window.grab().save(outputDirectory + QLatin1Char('/') + caseName + suffix + QStringLiteral(".png"));
    }

    void pointerFocus(QAbstractButton* button, QColor normal)
    {
        QStyleOption option;
        option.initFrom(button);
        const QColor actual = background(button);
        std::printf("BUTTON_INTEGRATION_STATE case=%s name=%s focus=%d hover=%d checked=%d keyboard=%d native_keyboard=%d normal=%s actual=%s\n",
            qPrintable(caseName), qPrintable(name(button)), int(button->hasFocus()), int(button->underMouse()),
            int(button->isChecked()), int(ks::ui::HasFlatButtonKeyboardFocus(button)),
            int(option.state.testFlag(QStyle::State_KeyboardFocusChange)), qPrintable(normal.name()), qPrintable(actual.name()));
        check(button->hasFocus(), "the real Qt transition did not give focus to the expected action");
        check(!button->underMouse() && !button->isChecked(), "the action had a legitimate hover or checked state");
        check(!ks::ui::HasFlatButtonKeyboardFocus(button), "automatic/mouse focus was classified as keyboard input");
        check(actual == normal, "an untouched action gained a keyboard/hover fill");
        QColor predicted;
        check(ks::ui::TryGetFlatButtonBackground(button, QIcon::Normal, QIcon::Off, &predicted)
            && predicted.rgb() == normal.rgb(), "icon contrast predicted a highlighted surface for pointer focus");
    }

    void keyboardFocus(QAbstractButton* button, QColor normal)
    {
        check(button->hasFocus(), "real keyboard input did not restore the expected action focus");
        check(!button->underMouse() && !button->isChecked(), "keyboard cue was confused with hover or checked");
        check(ks::ui::HasFlatButtonKeyboardFocus(button), "real keyboard input lost its focus classification");
        check(background(button) != normal, "real keyboard focus lost its visible cue");
    }

    enum class Removal { Disable, Hide, Delete };

    void automaticTransfer(Removal removal)
    {
        caseName = removal == Removal::Disable ? QStringLiteral("mouse-disable-neighbor")
            : removal == Removal::Hide ? QStringLiteral("mouse-hide-neighbor") : QStringLiteral("mouse-delete-neighbor");
        Surface surface;
        surface.away();
        const QColor normal = background(surface.second);
        check(ks::ui::HasWidgetThemeBinding(surface.second), "real StandardWidgetTheme did not enroll the ordinary action");
        check(surface.second->property("ksword_flat_button_shared_owner").toBool(),
            "the ordinary action did not exercise the real shared main-surface stylesheet");
        mouseClick(surface.first);
        surface.away();
        check(surface.first->hasFocus() && surface.firstClicks == 1, "the source action was not focused by real mouse input");
        if (removal == Removal::Disable) surface.first->setEnabled(false);
        else if (removal == Removal::Hide) surface.first->hide();
        else { delete surface.first; surface.first = nullptr; }
        pump();
        surface.away();
        check(surface.secondClicks == 0, "the receiving action was clicked during automatic focus transfer");
        if (removal != Removal::Delete || surface.second->hasFocus())
            pointerFocus(surface.second, normal);
        else
        {
            // Qt 6.8 offscreen clears focus when a focused QWidget is deleted;
            // unlike Disable/Hide it does not guarantee transfer to a neighbor.
            // Verify that surviving controls remain normal, then use real Tab
            // to prove the remaining chain and its keyboard cue still work.
            check(!surface.second->underMouse() && !surface.second->isChecked(),
                "the surviving action had a legitimate hover or checked state");
            check(!ks::ui::HasFlatButtonKeyboardFocus(surface.second),
                "deleting a mouse-focused action invented keyboard focus on its neighbor");
            check(background(surface.second) == normal, "deleting an action highlighted its untouched neighbor");
            QTest::keyClick(surface.host.windowHandle(), Qt::Key_Tab, Qt::NoModifier, 1);
            pump();
            keyboardFocus(surface.second, normal);
        }
        capture(surface.host, QStringLiteral("-restored"));
    }

    void keyboardAndMixedTransfer()
    {
        caseName = QStringLiteral("keyboard-and-mixed-transfer");
        Surface surface;
        mouseClick(surface.first);
        surface.away();
        const QColor firstNormal = background(surface.first);
        const QColor secondNormal = background(surface.second);
        const QColor thirdNormal = background(surface.third);
        QTest::keyClick(surface.first, Qt::Key_Tab, Qt::NoModifier, 1);
        pump();
        keyboardFocus(surface.second, secondNormal);
        QTest::keyClick(surface.second, Qt::Key_Backtab, Qt::ShiftModifier, 1);
        pump();
        keyboardFocus(surface.first, firstNormal);
        QTest::keyClick(surface.first, Qt::Key_Tab, Qt::NoModifier, 1);
        pump();
        keyboardFocus(surface.second, secondNormal);
        // Qt's window keyboard-change attribute can remain true after this
        // same-focus click. The actual last input must still win over it.
        mouseClick(surface.second);
        surface.away();
        pointerFocus(surface.second, secondNormal);
        QStyleOption option;
        option.initFrom(surface.second);
        std::printf("BUTTON_INTEGRATION_MIXED_NATIVE_KEYBOARD=%d\n",
            int(option.state.testFlag(QStyle::State_KeyboardFocusChange)));
        surface.second->setEnabled(false);
        pump();
        surface.away();
        check(surface.thirdClicks == 0, "the receiving mixed-input neighbor was clicked");
        pointerFocus(surface.third, thirdNormal);
        capture(surface.host, QStringLiteral("-neighbor"));
    }

    enum class CloseInput { Mouse, Escape, Programmatic };

    void modelessRestore(bool keyboardOpen, CloseInput closing)
    {
        const QString closeName = closing == CloseInput::Mouse ? QStringLiteral("mouse-close")
            : closing == CloseInput::Escape ? QStringLiteral("escape-close") : QStringLiteral("programmatic-close");
        caseName = QStringLiteral("modeless-%1-%2").arg(keyboardOpen ? QStringLiteral("keyboard-open")
            : QStringLiteral("mouse-open"), closeName);
        Surface surface;
        surface.away();
        const QColor normal = background(surface.second);
        QDialog dialog(&surface.host);
        dialog.setWindowModality(Qt::NonModal);
        dialog.resize(280, 140);
        auto* layout = new QVBoxLayout(&dialog);
        auto* closeButton = new QPushButton(QStringLiteral("Close settings"), &dialog);
        closeButton->setProperty("fixture_button_label", QStringLiteral("dialog-close"));
        closeButton->setFixedSize(180, 36);
        layout->addWidget(closeButton);
        QObject::connect(closeButton, &QPushButton::clicked, &dialog, &QDialog::reject);
        QObject::connect(surface.second, &QPushButton::clicked, &dialog, [&dialog]
        {
            dialog.show();
            dialog.raise();
            dialog.activateWindow();
        });
        if (keyboardOpen)
        {
            mouseClick(surface.first);
            surface.away();
            QTest::keyClick(surface.first, Qt::Key_Tab, Qt::NoModifier, 1);
            pump();
            keyboardFocus(surface.second, normal);
            QTest::keyClick(surface.second, Qt::Key_Space, Qt::NoModifier, 1);
        }
        else mouseClick(surface.second);
        pump();
        check(dialog.isVisible() && surface.secondClicks == 1,
            "real input did not open exactly one nonmodal settings dialog");
        surface.away();
        if (closing == CloseInput::Mouse) mouseClick(closeButton);
        else if (closing == CloseInput::Escape)
            QTest::keyClick(dialog.windowHandle(), Qt::Key_Escape, Qt::NoModifier, 1);
        else dialog.close(); // Separate preservation control: no input happened while closing.
        pump();
        check(!dialog.isVisible(), "the selected close input did not close the real dialog");
        // The offscreen plugin has no OS window manager. Request normal Qt
        // owner activation; never manufacture a FocusEvent or set button focus.
        surface.host.activateWindow();
        pump();
        surface.away();
        const bool keyboard = closing == CloseInput::Escape
            || (closing == CloseInput::Programmatic && keyboardOpen);
        if (keyboard) keyboardFocus(surface.second, normal);
        else pointerFocus(surface.second, normal);
        capture(surface.host, QStringLiteral("-restored"));
    }
}

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    if (argc > 1)
    {
        outputDirectory = QString::fromLocal8Bit(argv[1]);
        QDir().mkpath(outputDirectory);
    }
    // Match main.cpp: standard drawing/enrollment, final palette and global
    // base block, then the input filter, all before constructing business UI.
    KswordTheme::SetDarkModeEnabled(true);
    ks::ui::InstallStandardWidgetTheme(&application, false);
    application.setPalette(buildApplicationAppearancePalette(application.palette()));
    application.setStyleSheet(ks::ui::BuildGlobalBaseControlStyleBlock());
    ks::ui::InstallGlobalFlatButtonTheme(&application);
    FocusTrace trace;
    application.installEventFilter(&trace);
    for (Removal removal : {Removal::Disable, Removal::Hide, Removal::Delete}) automaticTransfer(removal);
    keyboardAndMixedTransfer();
    for (bool keyboard : {false, true})
        for (CloseInput closing : {CloseInput::Mouse, CloseInput::Escape, CloseInput::Programmatic})
            modelessRestore(keyboard, closing);
    std::printf("BUTTON_THEME_INTEGRATION_CHECKS=%d FAILURES=%d REAL_QTWIDGETS=1 MAIN_GUI=0 REAL_THEME_QUEUE=1\n",
        checks, failures);
    return failures ? 1 : 0;
}
