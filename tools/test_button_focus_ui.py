#!/usr/bin/env python3
"""Check real shared-button Qt painting with window mouse input and keyboard focus.

Links production FlatButtonTheme.cpp, MainSurfaceStyle.cpp, both Hex glyph TUs
and theme.h; only the
ThemeBinding scheduler is a fixture adapter. The Windows umbrella include is
removed from a temporary MainSurfaceStyle copy, preserving its implementation.
This is an isolated QtWidgets test, not the KSword GUI or native Windows theme.
"""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qt-root', type=Path, default=Path('/usr'))
    parser.add_argument('--cxx', default=shutil.which('cl' if os.name == 'nt' else 'c++'))
    parser.add_argument('--source-ref', help='Optional Git revision of the four production TUs, for baseline rejection')
    parser.add_argument('--output-directory', type=Path, help='Save actual mouse/keyboard widget captures')
    args = parser.parse_args()
    if not args.cxx:
        parser.error('A native C++ compiler is required (MSVC on Windows, c++ on Linux)')
    roots = [args.qt_root / 'include', args.qt_root / 'include/qt6']
    roots.extend((args.qt_root / 'include').glob('*/qt6'))
    includes = next((p for p in roots if (p / 'QtWidgets/QApplication').is_file()), None)
    if not includes or not (includes / 'QtTest/QTest').is_file():
        parser.error('Qt6 Widgets and Test development files are required; pass --qt-root')
    root = Path(__file__).resolve().parents[1]
    app = root / 'Ksword5.1/Ksword5.1'
    with tempfile.TemporaryDirectory(prefix='ksword-button-focus-') as temporary:
        work = Path(temporary)
        production = []
        for relative in ('UI/FlatButtonTheme.cpp', 'UI/MainSurfaceStyle.cpp',
                         'UI/MemoryWorkbench/HexInspectorWidgets.cpp', 'UI/MemoryWorkbench/HexViewWidgets.cpp'):
            original = app / relative
            if args.source_ref:
                source = subprocess.check_output(['git', 'show', f'{args.source_ref}:{original.relative_to(root).as_posix()}'], cwd=root).decode('utf-8')
            else:
                source = original.read_text(encoding='utf-8')
            if relative == 'UI/MainSurfaceStyle.cpp':
                marker = '#include "../Framework.h"'
                if source.count(marker) != 1:
                    raise SystemExit('Expected one removable Windows umbrella include')
                source = source.replace(marker, '')
            if args.source_ref or relative == 'UI/MainSurfaceStyle.cpp':
                path = work / original.name
                path.write_text(source, encoding='utf-8')
            else:
                path = original
            production.append(str(path))
        binary = work / ('button_focus.exe' if os.name == 'nt' else 'button_focus')
        include_paths = [includes, *(includes / module for module in ('QtCore', 'QtGui', 'QtWidgets', 'QtTest')), app, app / 'UI', app / 'UI/MemoryWorkbench']
        sources = [str(root / 'tools/button_focus_ui_tests.cpp'), *production]
        if os.name == 'nt':
            command = [args.cxx, '/nologo', '/std:c++20', '/Zc:__cplusplus', '/permissive-', '/utf-8', '/EHsc', '/MD',
                       '/DNOMINMAX', '/DUNICODE', '/D_UNICODE', '/DQT_WIDGETS_LIB', '/DQT_GUI_LIB']
            command.extend('/I' + str(p) for p in include_paths)
            command.extend([*sources, '/Fe:' + str(binary), '/link', '/LIBPATH:' + str(args.qt_root / 'lib')])
            command.extend(name + '.lib' for name in ('Qt6Widgets', 'Qt6Gui', 'Qt6Core', 'Qt6Test'))
        else:
            command = [args.cxx, '-std=c++20', '-fPIC', '-DQT_WIDGETS_LIB', '-DQT_GUI_LIB']
            command.extend('-I' + str(p) for p in include_paths)
            libraries = [args.qt_root / 'lib', *sorted((args.qt_root / 'lib').glob('*-linux-gnu'))]
            command.extend('-L' + str(p) for p in libraries if p.is_dir())
            command.extend([*sources, '-l:libQt6Widgets.so.6', '-l:libQt6Gui.so.6', '-l:libQt6Core.so.6', '-l:libQt6Test.so.6', '-o', str(binary)])
        subprocess.run(command, cwd=work, check=True, timeout=120)
        runtime = work / 'runtime'; runtime.mkdir(mode=0o700)
        cache = work / 'cache'; cache.mkdir()
        environment = dict(os.environ, QT_QPA_PLATFORM='offscreen', QTEST_MOUSEEVENT_DELAY='0', QTEST_KEYEVENT_DELAY='0',
                           XDG_RUNTIME_DIR=str(runtime), XDG_CACHE_HOME=str(cache))
        if os.name == 'nt':
            environment['PATH'] = str(args.qt_root / 'bin') + os.pathsep + environment.get('PATH', '')
            environment['QT_PLUGIN_PATH'] = str(args.qt_root / 'plugins')
        else:
            environment['LD_LIBRARY_PATH'] = os.pathsep.join(str(p) for p in libraries if p.is_dir()) + os.pathsep + environment.get('LD_LIBRARY_PATH', '')
        parameters = [str(binary)]
        if args.output_directory: parameters.append(str(args.output_directory.resolve()))
        subprocess.run(parameters, env=environment, check=True, timeout=60)


if __name__ == '__main__':
    main()
