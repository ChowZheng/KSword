#!/usr/bin/env python3
"""Exercise the exact disk-monitor section helper with real Qt/theme queues.

The complete helper is extracted unchanged. StandardWidgetTheme, FlatButtonTheme,
MainSurfaceStyle, PrimaryPageStyle, ThemeBinding and GlobalUiBaseStyle are real
translation units; unused non-button roles and Linux's unused Win32 progress
animation query are adapted. This tests the section UI, not monitor sampling,
ETW, volume counters, table-action capabilities or the full DiskMonitorPage.
--source-ref selects only the helper's Git revision for a behavioral negative
control; shared themes remain current so the old title-button defect is isolated.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from disk_monitor_columns_test import qt_include_root
from test_table_style_lifetime import block


ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / "Ksword5.1/Ksword5.1"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qt-root", "--qt-dir", dest="qt", type=Path,
                        default=Path(os.environ.get("KSWORD_QT_DIR", "/usr")))
    parser.add_argument("--cxx", default=shutil.which("cl" if os.name == "nt" else "c++"))
    parser.add_argument("--source-ref", help="Git revision of the complete section helper")
    parser.add_argument("--output-directory", type=Path, help="Save actual section captures")
    args = parser.parse_args()
    if not args.cxx:
        parser.error("A native C++ compiler is required")
    qt = args.qt.resolve()
    include = qt_include_root(qt)
    if not (include / "QtTest/QTest").is_file():
        parser.error("Qt 6 Test development files are required")
    compiler = shutil.which(args.cxx) or args.cxx
    if os.name == "nt" and "/hostx64/x64/" not in Path(compiler).resolve().as_posix().lower():
        parser.error("Windows tests require the MSVC HostX64 x64 developer environment")
    page = APP / "HardwareDock/DiskMonitorPage.cpp"
    source = (subprocess.check_output(["git", "show", f"{args.source_ref}:{page.relative_to(ROOT).as_posix()}"],
                                    cwd=ROOT).decode("utf-8-sig") if args.source_ref else page.read_text(encoding="utf-8-sig"))
    helper = block(source, "    QWidget* createDiskMonitorSection(")
    if "QLabel** titleLabelOut" in helper:
        adapter = """QWidget* createFixtureSection(QWidget* parent, QLabel** title, const QString& text, QWidget* content)
{ return createDiskMonitorSection(parent, title, text, content); }"""
    elif "QToolButton** toggleButtonOut" in helper:
        # Preserve old helper bytes/signature; the adapter merely allows the
        # same fixture to expose its old title-button contract as a failure.
        adapter = """QWidget* createFixtureSection(QWidget* parent, QLabel** title, const QString& text, QWidget* content)
{ QToolButton* oldTitle = nullptr; *title = nullptr; return createDiskMonitorSection(parent, &oldTitle, text, content); }"""
    else:
        raise SystemExit("Unrecognized production disk section signature")
    fixture = Path(__file__).with_name("disk_monitor_layout_ui_tests.cpp").read_text(encoding="utf-8")
    for marker, replacement in (("// PRODUCTION_DISK_MONITOR_SECTION", helper),
                                ("// PRODUCTION_SECTION_ADAPTER", adapter)):
        if fixture.count(marker) != 1:
            raise SystemExit(f"Expected exactly one fixture marker {marker}")
        fixture = fixture.replace(marker, replacement)
    with tempfile.TemporaryDirectory(prefix="ksword-disk-monitor-layout-") as temporary:
        work = Path(temporary)
        cpp = work / "DiskMonitorLayoutTests.cpp"
        cpp.write_text(fixture, encoding="utf-8")
        sources = [cpp]
        digest = hashlib.sha256(helper.encode("utf-8"))
        for filename in ("StandardWidgetTheme.cpp", "FlatButtonTheme.cpp", "MainSurfaceStyle.cpp",
                         "PrimaryPageStyle.cpp", "ThemeBinding.cpp", "GlobalUiBaseStyle.cpp", "FlowLayout.cpp"):
            original = (APP / "UI" / filename).read_text(encoding="utf-8-sig")
            digest.update(original.encode("utf-8"))
            adapted = original
            if filename in ("StandardWidgetTheme.cpp", "MainSurfaceStyle.cpp", "PrimaryPageStyle.cpp"):
                marker = '#include "../Framework.h"'
                if adapted.count(marker) != 1:
                    raise SystemExit(f"Expected one Windows umbrella include in {filename}")
                adapted = adapted.replace(marker, "")
            if filename == "StandardWidgetTheme.cpp":
                adapted = ("#include <Windows.h>\n" if os.name == "nt" else
                           "#include <QWidget>\nusing BOOL = int; constexpr BOOL TRUE = 1;\n"
                           "constexpr int SPI_GETCLIENTAREAANIMATION = 0;\n"
                           "inline bool SystemParametersInfoW(int, int, BOOL* value, int) { *value = 0; return true; }\n") + adapted
            destination = work / filename
            destination.write_text(adapted, encoding="utf-8")
            sources.append(destination)
        palette = block((APP / "MainWindow.cpp").read_text(encoding="utf-8-sig"),
                        "    QPalette buildApplicationAppearancePalette(const QPalette& sourcePalette)")
        (work / "DiskMonitorPalette.inc").write_text(palette, encoding="utf-8")
        digest.update(palette.encode("utf-8"))
        print(f"DISK_MONITOR_LAYOUT_SOURCE_SHA256={digest.hexdigest()}", flush=True)
        includes = [APP, APP / "UI", work, include]
        includes.extend(include / module for module in ("QtCore", "QtGui", "QtWidgets", "QtTest"))
        libraries = [qt / "lib", *sorted((qt / "lib").glob("*-linux-gnu"))]
        executable = work / ("disk-monitor-layout.exe" if os.name == "nt" else "disk-monitor-layout")
        if os.name == "nt":
            command = [compiler, "/nologo", "/std:c++20", "/Zc:__cplusplus", "/permissive-", "/utf-8",
                       "/EHsc", "/MD", "/DNOMINMAX", "/DWIN32_LEAN_AND_MEAN", "/DUNICODE", "/D_UNICODE",
                       "/DQT_WIDGETS_LIB", "/DQT_GUI_LIB"]
            command.extend("/I" + str(path) for path in includes)
            command.extend(str(path) for path in sources)
            command.extend(["/Fe:" + str(executable), "/link", "/LIBPATH:" + str(qt / "lib"),
                            "Qt6Widgets.lib", "Qt6Gui.lib", "Qt6Core.lib", "Qt6Test.lib", "User32.lib"])
        else:
            command = [compiler, "-std=c++20", "-fPIC", "-DQT_WIDGETS_LIB", "-DQT_GUI_LIB"]
            command.extend("-I" + str(path) for path in includes)
            command.extend("-L" + str(path) for path in libraries if path.is_dir())
            command.extend(str(path) for path in sources)
            command.extend(["-l:libQt6Widgets.so.6", "-l:libQt6Gui.so.6", "-l:libQt6Core.so.6",
                            "-l:libQt6Test.so.6", "-o", str(executable)])
        subprocess.run(command, cwd=work, check=True, timeout=180)
        runtime, cache = work / "runtime", work / "cache"
        runtime.mkdir(mode=0o700)
        cache.mkdir()
        environment = dict(os.environ, QT_QPA_PLATFORM="offscreen", QTEST_MOUSEEVENT_DELAY="0",
                           QTEST_KEYEVENT_DELAY="0", XDG_RUNTIME_DIR=str(runtime), XDG_CACHE_HOME=str(cache))
        if os.name == "nt":
            environment["PATH"] = str(qt / "bin") + os.pathsep + environment.get("PATH", "")
            environment["QT_PLUGIN_PATH"] = str(qt / "plugins")
        else:
            environment["LD_LIBRARY_PATH"] = os.pathsep.join(str(path) for path in libraries if path.is_dir()) \
                + os.pathsep + environment.get("LD_LIBRARY_PATH", "")
        parameters = [str(executable)]
        if args.output_directory: parameters.append(str(args.output_directory.resolve()))
        result = subprocess.run(parameters, env=environment, check=False, timeout=60)
        raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
