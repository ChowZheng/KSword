#!/usr/bin/env python3
"""Exercise automatic button focus with the real shared theme installation.

Links production StandardWidgetTheme, FlatButtonTheme, MainSurfaceStyle,
ThemeBinding and GlobalUiBaseStyle, preserving the real registration/queue.
The main-window appearance palette is extracted unchanged. Non-button roles
are inert adapters; Linux also adapts the unused Win32 progress-animation query.
This is an isolated QtWidgets integration test, not the Windows KSword main GUI.
Use --source-ref d628861b to compile a successful negative-control executable
whose actual mouse/automatic-focus behavior must fail the same assertions.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_table_style_lifetime import block


ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / "Ksword5.1/Ksword5.1"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qt-root", "--qt-dir", dest="qt", type=Path,
                        default=Path(os.environ.get("KSWORD_QT_DIR", "/usr")))
    parser.add_argument("--cxx", default=shutil.which("cl" if os.name == "nt" else "c++"))
    parser.add_argument("--source-ref", help="Compile the production files from a Git revision")
    parser.add_argument("--output-directory", type=Path, help="Save actual widget captures")
    args = parser.parse_args()
    if not args.cxx:
        parser.error("A native C++ compiler is required")
    qt = args.qt.resolve()
    candidates = [qt / "include", qt / "include/qt6"]
    candidates.extend(sorted((qt / "include").glob("*/qt6")))
    include = next((path for path in candidates if (path / "QtWidgets/QApplication").is_file()), None)
    if not include or not (include / "QtTest/QTest").is_file():
        parser.error("Qt 6 Widgets and Test development files are required")
    compiler = shutil.which(args.cxx) or args.cxx
    if os.name == "nt" and "/hostx64/x64/" not in Path(compiler).resolve().as_posix().lower():
        parser.error("Windows integration tests require the MSVC HostX64 x64 developer environment")

    def production(relative: str) -> str:
        path = APP / relative
        if args.source_ref:
            return subprocess.check_output(
                ["git", "show", f"{args.source_ref}:{path.relative_to(ROOT).as_posix()}"],
                cwd=ROOT).decode("utf-8-sig")
        return path.read_text(encoding="utf-8-sig")

    with tempfile.TemporaryDirectory(prefix="ksword-button-theme-integration-") as temporary:
        work = Path(temporary)
        sources = [Path(__file__).with_name("button_theme_integration_tests.cpp")]
        digest = hashlib.sha256()
        for filename in ("StandardWidgetTheme.cpp", "FlatButtonTheme.cpp", "MainSurfaceStyle.cpp",
                         "ThemeBinding.cpp", "GlobalUiBaseStyle.cpp"):
            source = production("UI/" + filename)
            digest.update(source.encode("utf-8"))
            if filename in ("StandardWidgetTheme.cpp", "MainSurfaceStyle.cpp"):
                marker = '#include "../Framework.h"'
                if source.count(marker) != 1:
                    raise SystemExit(f"Expected one Windows umbrella include in {filename}")
                source = source.replace(marker, "")
            if filename == "StandardWidgetTheme.cpp":
                if os.name == "nt":
                    source = "#include <Windows.h>\n" + source
                else:
                    source = ("#include <QWidget>\nusing BOOL = int; constexpr BOOL TRUE = 1;\n"
                              "constexpr int SPI_GETCLIENTAREAANIMATION = 0;\n"
                              "inline bool SystemParametersInfoW(int, int, BOOL* value, int) "
                              "{ *value = 0; return true; }\n" + source)
            destination = work / filename
            destination.write_text(source, encoding="utf-8")
            sources.append(destination)
        palette = block(production("MainWindow.cpp"),
                        "    QPalette buildApplicationAppearancePalette(const QPalette& sourcePalette)")
        (work / "ButtonIntegrationPalette.inc").write_text(palette, encoding="utf-8")
        digest.update(palette.encode("utf-8"))
        print(f"BUTTON_THEME_INTEGRATION_SOURCE_SHA256={digest.hexdigest()}", flush=True)
        includes = [APP, APP / "UI", work, include]
        includes.extend(include / module for module in ("QtCore", "QtGui", "QtWidgets", "QtTest"))
        libraries = [qt / "lib"]
        libraries.extend(sorted((qt / "lib").glob("*-linux-gnu")))
        executable = work / ("button-theme-integration.exe" if os.name == "nt" else "button-theme-integration")
        if os.name == "nt":
            command = [compiler, "/nologo", "/std:c++20", "/Zc:__cplusplus", "/permissive-",
                       "/utf-8", "/EHsc", "/MD", "/DNOMINMAX", "/DWIN32_LEAN_AND_MEAN",
                       "/DUNICODE", "/D_UNICODE", "/DQT_WIDGETS_LIB", "/DQT_GUI_LIB"]
            command.extend("/I" + str(path) for path in includes)
            command.extend(str(path) for path in sources)
            command.extend(["/Fe:" + str(executable), "/link", "/LIBPATH:" + str(qt / "lib")])
            command.extend(module + ".lib" for module in ("Qt6Widgets", "Qt6Gui", "Qt6Core", "Qt6Test"))
            command.append("User32.lib")  # Real StandardProgressSurface SystemParametersInfoW.
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
        if args.output_directory:
            parameters.append(str(args.output_directory.resolve()))
        result = subprocess.run(parameters, env=environment, check=False, timeout=60)
        raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
