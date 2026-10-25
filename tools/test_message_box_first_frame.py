#!/usr/bin/env python3
"""Exercise the production message-box styler before its first QtWidgets paint.

The real Qt controls, styles, layout and event filter are compiled from production
source. Only Windows icon/chrome integration and translations are adapted for
this Linux offscreen fixture. It does not launch the KSword Windows application.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from disk_monitor_columns_test import qt_include_root
from test_memory_dock_layout_gate import find_matching, scan_cpp

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / "Ksword5.1" / "Ksword5.1"


def production_source(source: str | None = None) -> str:
    if source is None:
        source = (APP / "UI/ThemedMessageBox.cpp").read_text(encoding="utf-8-sig")
    source = source.removeprefix("\ufeff")
    source = source.replace('#include "ThemedMessageBox.h"',
                            f'#include "{APP / "UI/ThemedMessageBox.h"}"')
    source = source.replace('#include "./ToolbarMetrics.h"',
                            f'#include "{APP / "UI/ToolbarMetrics.h"}"')
    source = source.replace('#include "../theme.h"', f'#include "{APP / "theme.h"}"')
    for include in ('#include "WindowChrome.h"', '#include <qt_windows.h>',
                    '#include "../Internationalization/LanguageManager.h"'):
        source = source.replace(include, "")
    start = source.index("QIcon resolveApplicationIcon(")
    brace = source.index("{", start)
    end = find_matching(scan_cpp(source, False), brace, "{", "}")
    if end < 0:
        raise SystemExit("Cannot locate production native-icon function")
    source = (source[:start]
              + "QIcon resolveApplicationIcon(const QMessageBox*) { return QApplication::windowIcon(); }"
              + source[end + 1:])
    adapters = r'''
#include <QApplication>
#include <QString>
namespace ks::i18n {
    inline QString contextText(const QString&, const QString& fallback) { return fallback; }
}
namespace ks::ui { inline void ApplyWindowChrome(QWidget*) {} }
'''
    return adapters + source


FIXTURE = r'''
#include <QShowEvent>
#include <QImage>
#include <QPainter>
#include <iostream>
#include <stdexcept>

static int checks = 0;
static void check(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
    ++checks;
}
static void drain()
{
    for (int turn = 0; turn < 4; ++turn) {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
    }
}
static bool prepared(QMessageBox* box)
{
    if (box->objectName() != QStringLiteral("KswordThemedMessageBox")
        || box->styleSheet().isEmpty()) return false;
    auto* label = box->findChild<QLabel*>(QStringLiteral("qt_msgbox_label"));
    // QMessageBox may disable wrapping for a short single line during its own
    // final size pass. The shared width constraint, not that transient flag,
    // is the invariant that prevents an oversized or late-resizing dialog.
    if (!label || label->maximumWidth() >= QWIDGETSIZE_MAX) return false;
    for (QAbstractButton* abstract : box->buttons()) {
        auto* button = qobject_cast<QPushButton*>(abstract);
        if (!button) continue;
        if (!button->property("ksword_primary").isValid()
            || button->minimumHeight() != button->maximumHeight()
            || button->minimumHeight() < 28) return false;
    }
    return true;
}
class FirstPaint final : public QObject
{
public:
    int paints = 0;
    bool allReady = true;
    bool closeOnFirstPaint = false;
    bool staticStable = false;
    QHash<QMessageBox*, QSize> firstSizes;
    bool eventFilter(QObject* object, QEvent* event) override
    {
        auto* box = qobject_cast<QMessageBox*>(object);
        if (box && event->type() == QEvent::Show) firstSizes.remove(box);
        if (box && event->type() == QEvent::Paint && !firstSizes.contains(box)) {
            ++paints;
            const bool ready = prepared(box);
            if (!ready) {
                std::cerr << "Unprepared first paint: name=" << box->objectName().toStdString()
                          << " style=" << box->styleSheet().size() << " buttons=" << box->buttons().size() << '\n';
                for (auto* button : box->buttons()) {
                    std::cerr << "  button=" << button->text().toStdString()
                              << " role=" << button->property("ksword_primary").isValid()
                              << " min=" << button->minimumHeight() << " max=" << button->maximumHeight() << '\n';
                }
            }
            allReady = allReady && ready;
            firstSizes.insert(box, box->size());
            if (closeOnFirstPaint) {
                const QPointer<QMessageBox> target(box);
                const QSize first = box->size();
                QTimer::singleShot(0, box, [this, target, first] {
                    if (!target) return;
                    staticStable = prepared(target.data()) && target->size() == first;
                    target->accept();
                });
            }
        }
        return false;
    }
};
class DeleteDuringStyle final : public QObject
{
public:
    bool eventFilter(QObject* object, QEvent* event) override
    {
        if (event->type() == QEvent::StyleChange) {
            delete object;
            return true;
        }
        return false;
    }
};
int main(int argc, char** argv)
{
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    ks::ui::InstallGlobalMessageBoxTheme(&app);
    FirstPaint paints;
    app.installEventFilter(&paints);
    try {
        for (bool dark : {false, true}) {
            KswordTheme::SetDarkModeEnabled(dark);
            QMessageBox box(QMessageBox::Warning, QStringLiteral("Fixture"),
                QStringLiteral("First frame must already have the final theme and geometry."),
                QMessageBox::Ok | QMessageBox::Cancel);
            box.setInformativeText(QStringLiteral("An additional line of information."));
            box.setDetailedText(QStringLiteral("Details stay in their native message-box widget."));
            box.show();
            check(prepared(&box), "Show returned before defaults were applied");
            check(!box.property("ksword_message_theme_pending").toBool(), "Show left its old request pending");
            check(box.width() >= 360 && box.width() <= 820, "First-show width is outside the shared limits");
            QEvent update(QEvent::UpdateRequest);
            QCoreApplication::sendEvent(&box, &update);
            check(!box.property("ksword_message_first_frame_pending").toBool(),
                  "The window update did not consume first-frame layout preparation");
            const auto revision = box.property("ksword_message_theme_revision").toULongLong();
            const auto style = box.styleSheet();
            const auto size = box.size();
            QImage first(box.size(), QImage::Format_ARGB32);
            first.fill(Qt::transparent);
            box.render(&first);
            check(first.pixelColor(6, 6).rgb() == KswordTheme::WindowColor().rgb(),
                  "First rendered background does not use the final theme");
            drain();
            check(prepared(&box), "Queued work lost the first-show preparation");
            check(box.property("ksword_message_theme_revision").toULongLong() == revision,
                  "An obsolete constructor timer repeated the Show preparation");
            if (box.styleSheet() != style || box.size() != size)
                std::cerr << "Settled change: size=" << size.width() << 'x' << size.height()
                          << "->" << box.width() << 'x' << box.height()
                          << " styleEqual=" << (box.styleSheet() == style) << '\n';
            check(box.styleSheet() == style && box.size() == size,
                  "Queued work changed the already painted style or geometry");
            box.hide();
            box.setInformativeText(QStringLiteral("A different and longer informative text for the next Show."));
            box.show();
            QEvent reshowUpdate(QEvent::UpdateRequest);
            QCoreApplication::sendEvent(&box, &reshowUpdate);
            check(prepared(&box), "Reshow did not prepare changed text and layout");
            const auto reshow = box.property("ksword_message_theme_revision").toULongLong();
            ks::ui::RefreshGlobalMessageBoxTheme();
            const auto explicitRefresh = box.property("ksword_message_theme_revision").toULongLong();
            check(explicitRefresh > reshow, "Explicit refresh did not consume pending work");
            drain();
            check(box.property("ksword_message_theme_revision").toULongLong() == explicitRefresh,
                  "Explicit refresh left a stale timer behind");
            box.hide();
        }
        check(paints.paints >= 2 && paints.allReady,
              "A message-box paint happened before shared defaults were prepared");
        // The static Qt API exercises the default application filter, without
        // a page opting into a KSword wrapper or preparing the dialog itself.
        for (bool dark : {false, true}) {
            KswordTheme::SetDarkModeEnabled(dark);
            for (bool longText : {false, true}) {
                const int beforeStatic = paints.paints;
                paints.closeOnFirstPaint = true;
                paints.staticStable = false;
                QTimer fallback;
                fallback.setSingleShot(true);
                QObject::connect(&fallback, &QTimer::timeout, &app, [] {
                    if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) box->accept();
                });
                fallback.start(2000); // Failure guard; normal closing follows the first real Paint.
                if (longText)
                    QMessageBox::warning(nullptr, QStringLiteral("Static warning"),
                        QString(200, QLatin1Char('X')), QMessageBox::Ok);
                else
                    QMessageBox::information(nullptr, QStringLiteral("Static information"),
                        QStringLiteral("Default message-box entry"), QMessageBox::Ok);
                fallback.stop();
                paints.closeOnFirstPaint = false;
                check(paints.paints > beforeStatic && paints.allReady,
                      "The static Qt message-box entry painted unprepared defaults");
                check(paints.staticStable, "The static Qt entry changed geometry after its first paint");
            }
        }
        // A style observer may synchronously destroy the target. Send the
        // boundary event directly so this checks our dispatcher lifetime guard.
        QPointer<QMessageBox> doomed = new QMessageBox(QMessageBox::Warning,
            QStringLiteral("Lifetime"), QStringLiteral("Owned fixture"), QMessageBox::Ok);
        DeleteDuringStyle deleter;
        doomed->installEventFilter(&deleter);
        QShowEvent show;
        QCoreApplication::sendEvent(doomed.data(), &show);
        check(doomed.isNull(), "The lifetime fixture did not exercise destruction");
        drain();
        check(true, "Queued cleanup survived target destruction");
        std::cout << "MESSAGE_BOX_FIRST_FRAME=PASS checks=" << checks
                  << " paints=" << paints.paints << '\n';
    } catch (const std::exception& error) {
        std::cerr << "MESSAGE_BOX_FIRST_FRAME=FAIL " << error.what() << '\n';
        return 1;
    }
}
'''


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qt-root", type=Path, default=Path("/usr"))
    parser.add_argument("--cxx", default=shutil.which("cl" if os.name == "nt" else "c++"))
    args = parser.parse_args()
    if not args.cxx:
        raise SystemExit("Native C++ compiler unavailable")
    includes = qt_include_root(args.qt_root)
    with tempfile.TemporaryDirectory(prefix="ksword-message-first-frame-") as temporary:
        work = Path(temporary)
        source = work / "test.cpp"
        source.write_text(production_source() + FIXTURE, encoding="utf-8")
        binary = work / ("test.exe" if os.name == "nt" else "test")
        include_paths = [includes, *(includes / module for module in ("QtCore", "QtGui", "QtWidgets"))]
        libraries = [args.qt_root / "lib", *sorted((args.qt_root / "lib").glob("*-linux-gnu"))]
        if os.name == "nt":
            command = [args.cxx, "/nologo", "/std:c++20", "/Zc:__cplusplus", "/utf-8", "/EHsc", "/MD", "/DNOMINMAX",
                       *(f"/I{path}" for path in include_paths), str(source), "/Fe:" + str(binary), "/link",
                       "/LIBPATH:" + str(args.qt_root / "lib"), "Qt6Widgets.lib", "Qt6Gui.lib", "Qt6Core.lib"]
        else:
            command = [args.cxx, "-std=c++20", "-fPIC", *(f"-I{path}" for path in include_paths)]
            command += [f"-L{path}" for path in libraries if path.is_dir()]
            command += [str(source), "-l:libQt6Widgets.so.6", "-l:libQt6Gui.so.6", "-l:libQt6Core.so.6", "-o", str(binary)]
        subprocess.run(command, cwd=work, check=True, timeout=120)
        runtime = work / "runtime"
        runtime.mkdir(mode=0o700)
        cache = work / "cache"
        cache.mkdir()
        environment = dict(os.environ, QT_QPA_PLATFORM="offscreen", XDG_RUNTIME_DIR=str(runtime),
                           XDG_CACHE_HOME=str(cache))
        if os.name == "nt":
            environment["PATH"] = str(args.qt_root / "bin") + os.pathsep + environment.get("PATH", "")
            environment["QT_PLUGIN_PATH"] = str(args.qt_root / "plugins")
        subprocess.run([str(binary)], check=True, env=environment, timeout=30)


if __name__ == "__main__":
    main()
