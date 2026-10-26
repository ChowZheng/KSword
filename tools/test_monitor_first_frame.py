#!/usr/bin/env python3
"""Check native splitter defaults and production monitor geometry callbacks.

This fixture uses real QtWidgets and the project's QPainter charts. Native
hardware sampling is replaced with counters; no device, driver or ETW is opened.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from disk_monitor_columns_test import qt_include_root
from test_memory_dock_layout_gate import extract_function_body, find_matching, scan_cpp

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / "Ksword5.1/Ksword5.1"


def generated_source(hardware: str | None = None, monitor: str | None = None) -> str:
    if hardware is None:
        hardware = (APP / "HardwareDock/HardwareDock.cpp").read_text(encoding="utf-8-sig")
    if monitor is None:
        monitor = (APP / "MonitorDock/MonitorPanelWidget.cpp").read_text(encoding="utf-8-sig")
    start = hardware.index("class UtilizationSidebarList final")
    brace = hardware.index("{", start)
    end = find_matching(scan_cpp(hardware, False), brace, "{", "}")
    if end < 0:
        raise SystemExit("Cannot locate the production sidebar size-hint class")
    sidebar = hardware[start:end + 2]
    replacements = {"@@SIDEBAR@@": sidebar,
                    "@@CHART_HEADER@@": str(ROOT / "shared/ui/KsPainterChart.h")}
    for owner, name, key, source in (
        ("HardwareDock", "showEvent", "HARDWARE_SHOW", hardware),
        ("HardwareDock", "scheduleUtilizationLayoutRefresh", "HARDWARE_SCHEDULE", hardware),
        ("MonitorPanelWidget", "showEvent", "MONITOR_SHOW", monitor),
        ("MonitorPanelWidget", "resizeEvent", "MONITOR_RESIZE", monitor),
        ("MonitorPanelWidget", "hideEvent", "MONITOR_HIDE", monitor),
        ("MonitorPanelWidget", "changeEvent", "MONITOR_CHANGE", monitor),
        ("MonitorPanelWidget", "adjustChartCellHeights", "MONITOR_ADJUST", monitor),
        ("MonitorPanelWidget", "updateCompactVisibility", "MONITOR_COMPACT", monitor),
    ):
        body = extract_function_body(source, rf"{owner}::{name}")
        if body is None:
            raise SystemExit(f"Cannot locate production callback {owner}::{name}")
        replacements[f"@@{key}@@"] = body
    result = FIXTURE
    for key, value in replacements.items():
        if result.count(key) != 1:
            raise SystemExit(f"Fixture must contain one marker: {key}")
        result = result.replace(key, value)
    return result


FIXTURE = r'''
#include <QApplication>
#include <QEventLoop>
#include <QGridLayout>
#include <QHideEvent>
#include <QLabel>
#include <QListWidget>
#include <QPointer>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include "@@CHART_HEADER@@"
namespace ks::i18n { QString contextText(const QString&, const QString& text) { return text; } }
static QString bytesPerSecondToText(double value) { return QString::number(value); }
static int checks = 0;
static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
    ++checks;
}
static void turn() { QCoreApplication::sendPostedEvents(); QCoreApplication::processEvents(); }
static void settle() { for (int i = 0; i < 4; ++i) turn(); }
static void waitPastOldCorrection() {
    QEventLoop loop;
    QTimer::singleShot(100, &loop, &QEventLoop::quit);
    loop.exec();
}
@@SIDEBAR@@
class HardwareDock : public QWidget {
public:
    enum class SamplingScope { HardwareDetails };
    int adjustments = 0, samplingStarts = 0;
    bool destroyDuringAdjust = false;
    bool m_utilizationLayoutRefreshPending = false;
    void startPerformanceSampling(SamplingScope) { ++samplingStarts; }
    void ensureCurrentHardwarePageInitialized() {}
    void refreshStaticHardwareTexts(bool) {}
    void adjustUtilizationChartHeights() { ++adjustments; if (destroyDuringAdjust) delete this; }
    void scheduleUtilizationLayoutRefresh() { @@HARDWARE_SCHEDULE@@ }
protected:
    void showEvent(QShowEvent* showEventPointer) override { @@HARDWARE_SHOW@@ }
};
class MonitorPanelWidget : public QWidget {
public:
    QVBoxLayout* m_rootLayout = new QVBoxLayout(this);
    QGridLayout* m_chartGridLayout = new QGridLayout;
    QLabel* m_compactSummaryLabel = new QLabel(this);
    QChartView* m_cpuChartView = nullptr;
    QChartView* m_memoryTrendChartView = nullptr;
    QChartView* m_diskChartView = nullptr;
    QChartView* m_networkChartView = nullptr;
    bool m_presentationExposed = false, m_compactTextOnly = false;
    bool m_chartThemeDirty = true, m_hasMetricSample = false;
    bool m_chartPresentationDirty = false;
    QString m_lastCompactSummaryText;
    struct Sample {
        double totalCpuUsage = 0;
        struct { double usedPercent = 0; } memory;
        double diskReadBytesPerSec = 0, diskWriteBytesPerSec = 0;
        double networkRxBytesPerSec = 0, networkTxBytesPerSec = 0;
    } m_latestPresentation;
    int adjustments = 0, themes = 0, reconciliations = 0;
    MonitorPanelWidget() {
        m_rootLayout->setContentsMargins(2, 2, 2, 2);
        m_rootLayout->setSpacing(2);
        m_rootLayout->addWidget(m_compactSummaryLabel, 1);
        m_chartGridLayout->setContentsMargins(0, 0, 0, 0);
        m_chartGridLayout->setHorizontalSpacing(2);
        m_chartGridLayout->setVerticalSpacing(2);
        m_rootLayout->addLayout(m_chartGridLayout, 1);
        const QList<QChartView**> chartTargets{&m_cpuChartView, &m_memoryTrendChartView,
            &m_diskChartView, &m_networkChartView};
        int index = 0;
        for (QChartView** slot : chartTargets) {
            *slot = new QChartView(new QChart, this);
            (*slot)->setMinimumHeight(0);
            (*slot)->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
            m_chartGridLayout->addWidget(*slot, index / 2, index % 2);
            ++index;
        }
        for (int i = 0; i < 2; ++i) {
            m_chartGridLayout->setRowStretch(i, 1);
            m_chartGridLayout->setColumnStretch(i, 1);
        }
    }
    void reconcileMetrics() { ++reconciliations; }
    void refreshVisiblePresentation(bool) {}
    void applyChartTextTheme() { ++themes; m_chartThemeDirty = false; }
    void updateCompactVisibility() { @@MONITOR_COMPACT@@ }
    void adjustChartCellHeights() { ++adjustments; @@MONITOR_ADJUST@@ }
    QList<QRect> chartGeometry() const {
        QList<QRect> result;
        for (QChartView* chart : {m_cpuChartView, m_memoryTrendChartView, m_diskChartView, m_networkChartView})
            result.append(chart->isVisible() ? chart->geometry() : QRect());
        return result;
    }
protected:
    void showEvent(QShowEvent* showEventPointer) override { @@MONITOR_SHOW@@ }
    void resizeEvent(QResizeEvent* resizeEventPointer) override { @@MONITOR_RESIZE@@ }
    void hideEvent(QHideEvent* hideEventPointer) override { @@MONITOR_HIDE@@ }
    void changeEvent(QEvent* eventPointer) override { @@MONITOR_CHANGE@@ }
};
class GeometryProbe final : public QObject {
public:
    QSplitter* splitter = nullptr;
    MonitorPanelWidget* monitor = nullptr;
    QList<int> firstSizes;
    QList<QRect> firstCharts;
    bool eventFilter(QObject* object, QEvent* event) override {
        if (event->type() == QEvent::Paint) {
            if (object == splitter && firstSizes.empty()) firstSizes = splitter->sizes();
            if (object == monitor && firstCharts.empty()) firstCharts = monitor->chartGeometry();
        }
        return false;
    }
};
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        for (int width : {580, 800, 1200, 1600}) for (bool hidden : {false, true}) {
            HardwareDock root;
            auto* outer = new QVBoxLayout(&root);
            auto* tabs = new QTabWidget(&root); outer->addWidget(tabs);
            auto* page = new QWidget; auto* layout = new QVBoxLayout(page);
            auto* body = new QSplitter(Qt::Horizontal, page); layout->addWidget(body);
            body->setChildrenCollapsible(false); body->setOpaqueResize(false); body->setHandleWidth(8);
            auto* left = new UtilizationSidebarList;
            left->setMinimumWidth(140); left->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
            body->addWidget(left);
            auto* right = new QStackedWidget; right->setMinimumWidth(360);
            right->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
            right->addWidget(new QWidget); body->addWidget(right);
            body->setStretchFactor(0, 0); body->setStretchFactor(1, 1);
            tabs->addTab(page, QStringLiteral("Performance")); tabs->addTab(new QWidget, QStringLiteral("Other"));
            if (hidden) tabs->setCurrentIndex(1);
            GeometryProbe probe; probe.splitter = body; body->installEventFilter(&probe);
            root.resize(width, 600); root.show(); settle();
            if (hidden) { tabs->setCurrentIndex(0); settle(); }
            const auto sizes = body->sizes();
            const int available = sizes.value(0) + sizes.value(1);
            const int expected = std::clamp(300, 140, std::max(140, available - 360));
            require(sizes.value(0) == expected, "Native initial sidebar width ignored the shared preferred width");
            require(probe.firstSizes == sizes, "Sidebar width changed after its first real Paint");
            waitPastOldCorrection();
            require(body->sizes() == sizes && root.adjustments == 0,
                    "An unchanged Show scheduled a default-width/chart correction");
            const int user = std::clamp(400, 140, std::max(140, available - 360));
            body->setSizes({user, available - user});
            const int retained = body->sizes().value(0);
            tabs->setCurrentIndex(1); tabs->setCurrentIndex(0); root.resize(width + 200, 700); settle();
            require(body->sizes().value(0) == retained, "A later native layout overwrote the user's splitter size");
            require(root.samplingStarts == 1, "Geometry preparation changed the sampling start path");
        }
        HardwareDock updates;
        for (int i = 0; i < 100; ++i) updates.scheduleUtilizationLayoutRefresh();
        require(updates.adjustments == 1, "One event turn repeated immediate device geometry work");
        settle(); require(updates.adjustments == 2, "The latest real viewport update did not get one queued pass");
        waitPastOldCorrection();
        require(updates.adjustments == 2, "An 80ms default geometry correction is still scheduled");
        QPointer<HardwareDock> doomed = new HardwareDock;
        doomed->destroyDuringAdjust = true; doomed->scheduleUtilizationLayoutRefresh();
        require(!doomed, "The lifetime fixture did not destroy the geometry owner"); settle();
        for (const QSize size : {QSize(600, 180), QSize(640, 320), QSize(900, 500), QSize(1400, 800)}) {
            MonitorPanelWidget monitor; GeometryProbe probe; probe.monitor = &monitor;
            monitor.installEventFilter(&probe); monitor.resize(size); monitor.show(); settle();
            require(!probe.firstCharts.empty() && probe.firstCharts == monitor.chartGeometry(),
                    "Monitor chart geometry changed after its first real Paint");
            const auto geometry = monitor.chartGeometry();
            const int passes = monitor.adjustments;
            waitPastOldCorrection();
            require(monitor.adjustments == passes && monitor.chartGeometry() == geometry,
                    "Monitor Show retained a fixed delayed geometry traversal");
            const int themes = monitor.themes;
            monitor.hide(); monitor.show(); settle(); waitPastOldCorrection();
            require(monitor.themes == themes, "An unchanged reshow traversed chart themes again");
            monitor.resize(size + QSize(100, 70)); settle();
            require(monitor.adjustments > passes, "Real Resize stopped maintaining chart geometry");
            require(monitor.reconciliations == 2, "The geometry change altered visible sampling reconciliation");
            monitor.hide();
            QFont changed = monitor.font(); changed.setPointSize(changed.pointSize() + 2);
            monitor.setFont(changed);
            monitor.show(); settle();
            require(monitor.themes == themes + 1, "A real hidden font change lost its chart-theme update");
        }
        std::cout << "MONITOR_FIRST_FRAME=PASS checks=" << checks << '\n';
    } catch (const std::exception& error) {
        std::cerr << "MONITOR_FIRST_FRAME=FAIL " << error.what() << '\n'; return 1;
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
    with tempfile.TemporaryDirectory(prefix="ksword-monitor-first-frame-") as temporary:
        work = Path(temporary); source = work / "test.cpp"
        binary = work / ("test.exe" if os.name == "nt" else "test")
        source.write_text(generated_source(), encoding="utf-8")
        include_paths = [includes, *(includes / module for module in ("QtCore", "QtGui", "QtWidgets"))]
        libraries = [args.qt_root / "lib", *sorted((args.qt_root / "lib").glob("*-linux-gnu"))]
        sources = [source, ROOT / "shared/ui/KsPainterChart.cpp"]
        if os.name == "nt":
            command = [args.cxx, "/nologo", "/std:c++20", "/Zc:__cplusplus", "/permissive-", "/utf-8", "/EHsc", "/MD", "/DNOMINMAX",
                       *(f"/I{path}" for path in include_paths), *(str(path) for path in sources), "/Fe:" + str(binary),
                       "/link", "/LIBPATH:" + str(args.qt_root / "lib"), "Qt6Widgets.lib", "Qt6Gui.lib", "Qt6Core.lib"]
        else:
            command = [args.cxx, "-std=c++20", "-fPIC", *(f"-I{path}" for path in include_paths)]
            command += [f"-L{path}" for path in libraries if path.is_dir()]
            command += [*(str(path) for path in sources), "-l:libQt6Widgets.so.6",
                        "-l:libQt6Gui.so.6", "-l:libQt6Core.so.6", "-o", str(binary)]
        subprocess.run(command, cwd=work, check=True, timeout=120)
        runtime = work / "runtime"; runtime.mkdir(mode=0o700)
        cache = work / "cache"; cache.mkdir()
        environment = dict(os.environ, QT_QPA_PLATFORM="offscreen", XDG_RUNTIME_DIR=str(runtime),
                           XDG_CACHE_HOME=str(cache))
        if os.name == "nt":
            environment["PATH"] = str(args.qt_root / "bin") + os.pathsep + environment.get("PATH", "")
            environment["QT_PLUGIN_PATH"] = str(args.qt_root / "plugins")
        subprocess.run([str(binary)], check=True, env=environment, timeout=30)


if __name__ == "__main__":
    main()
