#!/usr/bin/env python3
"""Exercise Taskbar input and HUD first-paint defaults with actual QtWidgets.

The Taskbar entry class and animation predicate are extracted without changes.
HUD uses its real header, page constructors, geometry callbacks and QPainter
charts. Only hardware discovery/sampling is replaced; no device is opened.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from disk_monitor_columns_test import qt_include_root
from test_memory_dock_layout_gate import extract_function_body, find_matching, scan_cpp

ROOT = Path(__file__).resolve().parents[1]


def extract_class(source: str, name: str) -> str:
    start = source.index(f"class {name} final")
    brace = source.index("{", start)
    end = find_matching(scan_cpp(source, False), brace, "{", "}")
    if end < 0:
        raise SystemExit(f"Cannot find production class {name}")
    return source[start:end + 2]


def generated_source(taskbar: str, hud: str) -> str:
    def body(source: str, name: str) -> str:
        result = extract_function_body(source, name)
        if result is None:
            raise SystemExit(f"Cannot find production function {name}")
        return result

    replacements = {
        "@@ENTRY@@": extract_class(taskbar, "PageEntry"),
        "@@RAINBOW@@": body(taskbar, r"QColor\s+rainbow"),
        "@@SPARKLINE@@": extract_class(hud, "CpuCoreSparklineWidget"),
        "@@TICK@@": re.search(r"if \(entry->[^\n]+\) entry->update\(\);", taskbar)[0],
        "@@TRANSPARENT@@": body(hud, r"void\s+appendTransparentBackgroundStyle"),
        "@@CONFIGURE_CHART@@": body(hud, r"void\s+configureTransparentChart"),
        "@@CREATE_CHART@@": body(hud, r"QChartView\*\s+createNoFrameChartView"),
    }
    functions = []
    for name, parameters in (
        ("initializeUi", ""), ("initializeSidebarCards", ""),
        ("initializeCpuPage", ""), ("initializeMemoryPage", ""),
        ("initializeDiskPage", ""), ("initializeNetworkPage", ""),
        ("initializeGpuPage", ""), ("initializeCoreCharts", ""),
        ("syncSidebarSelection", "const int selectedRowIndex"),
        ("resizeEvent", "QResizeEvent* resizeEventPointer"),
        ("showEvent", "QShowEvent* showEventPointer"),
        ("hideEvent", "QHideEvent* hideEventPointer"),
        ("refreshCorePresentation", "const double progress, const bool refreshLabels"),
        ("adjustChartHeights", ""),
    ):
        function_body = body(hud, rf"HudPerformancePanel::{name}")
        if name == "initializeCoreCharts":
            discovery = "const DWORD logicalProcessorCount = std::max<DWORD>(1, ::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));"
            if function_body.count(discovery) != 1:
                raise SystemExit("Expected one platform core-count query")
            function_body = function_body.replace(discovery,
                "const int logicalProcessorCount = std::max(1, fixtureCoreCount);")
        if name == "adjustChartHeights":
            function_body = "\n++adjustments[this];\n" + function_body
        functions.append(f"void HudPerformancePanel::{name}({parameters}) {{{function_body}}}")
    if "HudPerformancePanel::eventFilter" in hud:
        functions.append("bool HudPerformancePanel::eventFilter(QObject* watched, QEvent* event) {" +
                         body(hud, r"HudPerformancePanel::eventFilter") + "}")
    replacements["@@HUD_FUNCTIONS@@"] = "\n".join(functions)
    result = FIXTURE
    for marker, value in replacements.items():
        if result.count(marker) != 1:
            raise SystemExit(f"Expected one fixture marker {marker}")
        result = result.replace(marker, value)
    return result


FIXTURE = r'''
#include <QApplication>
#include <QFocusEvent>
#include <QGridLayout>
#include <QHideEvent>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScopeGuard>
#include <QShowEvent>
#include <QStackedWidget>
#include <QStyleFactory>
#include <QTimer>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QWindow>
#include <QtTest/QTest>
#include <cmath>
#include <cstdio>
#include <vector>
#include <algorithm>
#include "HudPerformancePanel.h"
#include "PerformanceNavCard.h"
#include "HudColors.h"
#include "KsPainterChart.h"
static int checks = 0, failures = 0;
static QHash<const HudPerformancePanel*, int> adjustments;
static int fixtureCoreCount = 8;
static void check(bool ok, const char* message) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
static void flush() { for (int i = 0; i < 6; ++i) QApplication::processEvents(); }
static void waitForOldCorrection() { QTest::qWait(110); flush(); }
static QColor rainbow(qreal hue, int alpha = 255) { @@RAINBOW@@ }
@@ENTRY@@
static void tick(PageEntry* entry) { @@TICK@@ }
static void move(QWidget& target, QPoint point) {
    QTest::mouseMove(target.window()->windowHandle(), target.mapTo(target.window(), point), 1);
    flush();
}
static void click(PageEntry& entry) {
    QTest::mouseClick(entry.window()->windowHandle(), Qt::LeftButton, Qt::NoModifier,
        entry.mapTo(entry.window(), entry.rect().center()), 1);
    flush();
}
static QColor painted(PageEntry& entry) {
    return entry.window()->grab().toImage().pixelColor(entry.mapTo(entry.window(), QPoint(150, 12)));
}
class PaintCounter : public QObject {
public:
    QObject* target = nullptr;
    int paints = 0;
    bool eventFilter(QObject* object, QEvent* event) override {
        if (object == target && event->type() == QEvent::Paint) ++paints;
        return false;
    }
};
static void taskbarInput() {
    QWidget host;
    host.setAutoFillBackground(true);
    QPalette palette = host.palette(); palette.setColor(QPalette::Window, QColor(13, 18, 28));
    host.setPalette(palette); host.setFocusPolicy(Qt::StrongFocus); host.resize(560, 180);
    PageEntry entry(QStringLiteral("Entry"), QStringLiteral("Root / Entry"), 2, &host);
    entry.setGeometry(12, 12, 220, 25);
    QLineEdit previous(&host); previous.setGeometry(12, 85, 200, 30);
    QWidget::setTabOrder(&previous, &entry);
    PaintCounter counter; counter.target = &entry; entry.installEventFilter(&counter);
    host.show(); host.activateWindow(); flush(); host.setFocus(Qt::OtherFocusReason);
    move(host, QPoint(500, 150));
    const QColor normal = painted(entry);
    move(entry, entry.rect().center());
    check(entry.underMouse() && painted(entry) != normal, "Taskbar hover did not use actual input/paint");
    int clicks = 0;
    QObject::connect(&entry, &QPushButton::clicked, &host, [&] { ++clicks; });
    click(entry); move(host, QPoint(500, 150));
    check(entry.hasFocus() && !entry.underMouse() && clicks == 1, "Taskbar mouse repro lost focus or activation");
    check(painted(entry) == normal, "Taskbar mouse focus retained hover fill");
    flush(); const int idlePaints = counter.paints;
    for (int i = 0; i < 5; ++i) { tick(&entry); flush(); }
    check(counter.paints == idlePaints, "Taskbar animation repainted idle mouse focus");
    previous.setFocus(Qt::OtherFocusReason);
    QTest::keyClick(&previous, Qt::Key_Tab, Qt::NoModifier, 1); flush();
    check(entry.hasFocus() && painted(entry) != normal, "Taskbar Tab lost visible keyboard focus");
    const int keyboardPaints = counter.paints; tick(&entry); flush();
    check(counter.paints > keyboardPaints, "Taskbar keyboard cue stopped animating");
    click(entry); move(host, QPoint(500, 150));
    check(entry.hasFocus() && painted(entry) == normal, "Taskbar same-focus keyboard-to-mouse retained fill");
    QTest::keyClick(&entry, Qt::Key_Space, Qt::NoModifier, 1); flush();
    check(clicks == 3 && painted(entry) != normal, "Taskbar same-focus mouse-to-keyboard lost cue or Space");
    click(entry); move(host, QPoint(500, 150));
    check(entry.hasFocus() && painted(entry) == normal, "Taskbar second same-focus mouse transition retained fill");
    entry.current = true; const QColor selected = painted(entry);
    check(selected != normal, "Taskbar current route lost its selected cue");
    const int currentPaints = counter.paints; tick(&entry); flush();
    check(counter.paints > currentPaints, "Taskbar current route stopped animating");
    entry.current = false; entry.setEnabled(false);
    check(painted(entry) == normal, "Taskbar disabled mouse-focused entry retained highlight");
}
static QColor textPrimaryColor() { return QColor(242, 246, 252); }
@@SPARKLINE@@
static void appendTransparentBackgroundStyle(QWidget* widgetPointer) { @@TRANSPARENT@@ }
static void configureTransparentChart(QChart* chartPointer) { @@CONFIGURE_CHART@@ }
static QChartView* createNoFrameChartView(QChart* chartPointer, QWidget* parentWidget) { @@CREATE_CHART@@ }
HudPerformancePanel::HudPerformancePanel(QWidget* parent) : QWidget(parent) {
    appendTransparentBackgroundStyle(this);
    initializeUi();
    m_coreSampleAnimation = new QVariantAnimation(this);
}
HudPerformancePanel::~HudPerformancePanel() { adjustments.remove(this); }
void HudPerformancePanel::refreshVisiblePresentation(bool) {}
@@HUD_FUNCTIONS@@
struct Geometry {
    QRect target, page, viewport, host;
    int minimum = 0, maximum = 0;
    bool operator==(const Geometry& other) const {
        return target == other.target && page == other.page && viewport == other.viewport
            && host == other.host && minimum == other.minimum && maximum == other.maximum;
    }
};
static Geometry geometry(HudPerformancePanel& panel, QWidget* target) {
    return {target->geometry(), panel.m_detailStack->currentWidget()->geometry(),
        panel.m_coreChartScrollArea->viewport()->geometry(), panel.m_coreChartHostWidget->geometry(),
        target->minimumHeight(), target->maximumHeight()};
}
class FirstPaint : public QObject {
public:
    HudPerformancePanel* panel = nullptr;
    QWidget* target = nullptr;
    Geometry first;
    bool seen = false;
    bool eventFilter(QObject* object, QEvent* event) override {
        if (object == target && event->type() == QEvent::Paint && !seen) {
            first = geometry(*panel, target); seen = true;
        }
        return false;
    }
};
static QWidget* targetFor(HudPerformancePanel& panel, int page) {
    if (page == 0) return panel.m_coreChartEntries.front().chartWidget;
    if (page == 1) return panel.m_memoryChartView;
    if (page == 2) return panel.m_diskChartView;
    if (page == 3) return panel.m_networkChartView;
    return panel.m_gpuEngineCharts.front().chartView;
}
static void compareFirst(FirstPaint& probe, const char* phase) {
    const Geometry settled = geometry(*probe.panel, probe.target);
    if (probe.seen && !(probe.first == settled))
        std::fprintf(stderr, "HUD %s target=%d,%d %dx%d->%d,%d %dx%d viewportH=%d->%d hostH=%d->%d\n",
            phase, probe.first.target.x(), probe.first.target.y(), probe.first.target.width(), probe.first.target.height(),
            settled.target.x(), settled.target.y(), settled.target.width(), settled.target.height(),
            probe.first.viewport.height(), settled.viewport.height(), probe.first.host.height(), settled.host.height());
    check(probe.seen, "HUD target never received an actual Paint");
    check(probe.first == settled, "HUD first Paint differed from settled geometry");
}
static void hudGeometry() {
    for (int cores : {8, 36}) for (QSize size : {QSize(760, 460), QSize(1400, 900)}) {
        fixtureCoreCount = cores;
        HudPerformancePanel panel;
        panel.resize(size);
        FirstPaint probe; probe.panel = &panel;
        qApp->installEventFilter(&probe);
        for (int page = 0; page < 5; ++page) {
            panel.hide(); panel.syncSidebarSelection(page);
            probe.target = targetFor(panel, page); probe.seen = false;
            panel.show(); flush();
            const int settledWork = adjustments.value(&panel);
            waitForOldCorrection();
            compareFirst(probe, "show");
            check(adjustments.value(&panel) == settledWork, "HUD retained fixed delayed geometry work after Show");
            probe.seen = false;
            panel.resize(size + QSize(130, 90)); flush(); waitForOldCorrection();
            compareFirst(probe, "resize");
            panel.resize(size); flush();
        }
        qApp->removeEventFilter(&probe);
    }
}
class ResizeTail : public QObject {
public:
    int events = 0;
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::Resize) ++events;
        return false;
    }
};
class ResizeKiller : public QObject {
public:
    QPointer<HudPerformancePanel> panel;
    QWidget* page = nullptr;
    QWidget* survivor = nullptr;
    bool armed = true;
    bool eventFilter(QObject*, QEvent* event) override {
        if (armed && event->type() == QEvent::Resize) {
            armed = false;
            page->setParent(survivor);
            delete panel.data();
        }
        return false;
    }
};
static void hudLifetime() {
    QWidget survivor;
    QPointer<HudPerformancePanel> panel = new HudPerformancePanel;
    panel->resize(1000, 600); panel->syncSidebarSelection(1); panel->show(); flush(); waitForOldCorrection();
    QPointer<QWidget> page = panel->m_memoryPage;
    QChartView* chart = panel->m_memoryChartView;
    chart->setMinimumHeight(0); chart->setMaximumHeight(QWIDGETSIZE_MAX);
    chart->resize(chart->width(), 1);
    ResizeTail tail;
    page->removeEventFilter(panel);
    page->installEventFilter(&tail);
    page->installEventFilter(panel);
    ResizeKiller killer; killer.panel = panel; killer.page = page; killer.survivor = &survivor;
    chart->installEventFilter(&killer);
    const int before = tail.events;
    QResizeEvent resize(page->size(), QSize(1, 1));
    QCoreApplication::sendEvent(page, &resize);
    check(!killer.armed && panel.isNull(), "HUD real constraint Resize did not exercise synchronous panel destruction");
    check(page && tail.events > before, "HUD swallowed the surviving page's original Resize after panel destruction");
    if (panel) delete panel.data();
}
class TargetKiller : public QObject {
public:
    bool armed = true;
    bool eventFilter(QObject* target, QEvent* event) override {
        if (armed && event->type() == QEvent::Resize) {
            // Qt keeps using the receiver inside setMinimumSize after Resize;
            // its supported lifetime boundary is deferred deletion.
            armed = false; target->deleteLater();
        }
        return false;
    }
};
static void hudTargetLifetime() {
    auto* panel = new HudPerformancePanel;
    panel->resize(1000, 600); panel->show(); flush(); waitForOldCorrection();
    QPointer<QWidget> container = panel->m_coreChartEntries.front().containerWidget;
    QPointer<QWidget> chart = panel->m_coreChartEntries.front().chartWidget;
    container->setMinimumHeight(0); container->setMaximumHeight(QWIDGETSIZE_MAX);
    container->resize(container->width(), 1);
    TargetKiller killer; container->installEventFilter(&killer);
    QResizeEvent resize(panel->m_coreChartScrollArea->viewport()->size(), QSize(1, 1));
    QCoreApplication::sendEvent(panel->m_coreChartScrollArea->viewport(), &resize);
    check(!killer.armed && container && chart,
          "HUD real CPU-container constraint did not preserve its receiver through the setter");
    QCoreApplication::sendPostedEvents(container, QEvent::DeferredDelete);
    check(!container && !chart, "HUD CPU-container deferred deletion lost its child ownership");
    delete panel;
}
int main(int argc, char** argv) {
    QApplication app(argc, argv); app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    taskbarInput(); hudGeometry(); hudLifetime(); hudTargetLifetime();
    std::printf("COMPANION_UI_DEFAULTS=%s CHECKS=%d FAILURES=%d\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
'''


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qt-root", type=Path, default=Path("/usr"))
    parser.add_argument("--cxx", default=shutil.which("cl" if os.name == "nt" else "c++"))
    parser.add_argument("--source-ref", help="Optional Git revision for baseline rejection")
    args = parser.parse_args()
    if not args.cxx:
        parser.error("A native C++ compiler is required")
    includes = qt_include_root(args.qt_root)
    if not includes or not (includes / "QtTest/QTest").is_file():
        parser.error("Qt6 Widgets and Test headers are required; pass --qt-root")
    def read(relative: str) -> str:
        if args.source_ref:
            return subprocess.check_output(["git", "show", f"{args.source_ref}:{relative}"], cwd=ROOT).decode("utf-8")
        return (ROOT / relative).read_text(encoding="utf-8-sig")
    taskbar = read("Taskbar/PageOverviewWindow.cpp")
    hud = read("KswordHUD/HudPerformancePanel.cpp")
    with tempfile.TemporaryDirectory(prefix="ksword-companion-ui-") as temporary:
        work = Path(temporary)
        header = read("KswordHUD/HudPerformancePanel.h").replace("private:", "public:")
        (work / "HudPerformancePanel.h").write_text(header, encoding="utf-8")
        source = work / "fixture.cpp"
        source.write_text(generated_source(taskbar, hud), encoding="utf-8")
        binary = work / ("companion_ui.exe" if os.name == "nt" else "companion_ui")
        include_paths = [work, includes, *(includes / module for module in ("QtCore", "QtGui", "QtWidgets", "QtTest")),
                         ROOT / "KswordHUD", ROOT / "shared/ui"]
        sources = [source, ROOT / "shared/ui/KsPainterChart.cpp", ROOT / "KswordHUD/PerformanceNavCard.cpp"]
        if os.name == "nt":
            command = [args.cxx, "/nologo", "/std:c++20", "/Zc:__cplusplus", "/utf-8", "/EHsc", "/MD", "/DNOMINMAX", "/DQT_WIDGETS_LIB", "/DQT_GUI_LIB",
                       *(f"/I{p}" for p in include_paths), *(str(p) for p in sources), "/Fe:" + str(binary),
                       "/link", "/LIBPATH:" + str(args.qt_root / "lib"), "Qt6Widgets.lib", "Qt6Gui.lib", "Qt6Core.lib", "Qt6Test.lib"]
        else:
            command = [args.cxx, "-std=c++20", "-fPIC", "-DQT_WIDGETS_LIB", "-DQT_GUI_LIB", *(f"-I{p}" for p in include_paths)]
            libraries = [args.qt_root / "lib", *sorted((args.qt_root / "lib").glob("*-linux-gnu"))]
            command += [*(f"-L{p}" for p in libraries if p.is_dir()), *(str(p) for p in sources),
                        "-l:libQt6Widgets.so.6", "-l:libQt6Gui.so.6", "-l:libQt6Core.so.6", "-l:libQt6Test.so.6", "-o", str(binary)]
        subprocess.run(command, cwd=work, check=True, timeout=120)
        runtime = work / "runtime"; runtime.mkdir(mode=0o700)
        cache = work / "cache"; cache.mkdir()
        environment = dict(os.environ, QT_QPA_PLATFORM="offscreen", XDG_RUNTIME_DIR=str(runtime),
                           XDG_CACHE_HOME=str(cache), QTEST_MOUSEEVENT_DELAY="0", QTEST_KEYEVENT_DELAY="0")
        if os.name == "nt":
            environment["PATH"] = str(args.qt_root / "bin") + os.pathsep + environment.get("PATH", "")
            environment["QT_PLUGIN_PATH"] = str(args.qt_root / "plugins")
        else:
            environment["LD_LIBRARY_PATH"] = os.pathsep.join(str(p) for p in libraries if p.is_dir()) + os.pathsep + environment.get("LD_LIBRARY_PATH", "")
        subprocess.run([str(binary)], check=True, env=environment, timeout=60)


if __name__ == "__main__":
    main()
