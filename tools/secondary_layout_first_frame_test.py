#!/usr/bin/env python3
"""Run actual secondary/settings layout helpers with QtWidgets before timer drain.

Only the Windows umbrella header is omitted from portable TU copies. Geometry,
controllers and event paths are production code. Theme-binding/flat-color and tab
animation adapters are tiny fixture substitutes, so this is not KSword GUI or
Windows visual acceptance.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
UI = ROOT / 'Ksword5.1/Ksword5.1/UI'
SOURCES = ('SecondaryPageLayout.cpp', 'SettingsPresentation.cpp')


def qt_include_root(prefix: Path) -> Path:
    candidates = [prefix / 'include/qt6', prefix / 'include']
    candidates.extend(sorted((prefix / 'include').glob('*/qt6')))
    for candidate in candidates:
        if (candidate / 'QtWidgets/QApplication').is_file():
            return candidate
    raise SystemExit(f'Qt 6 development headers unavailable under {prefix}')


def run_fixture(args, work: Path, baseline: bool = False) -> subprocess.CompletedProcess:
    includes = qt_include_root(args.qt_root)
    paths = []
    for filename in SOURCES:
        if baseline:
            source = subprocess.check_output(
                ['git', 'show', f'{args.baseline_ref}:Ksword5.1/Ksword5.1/UI/{filename}'],
                cwd=ROOT).decode('utf-8')
        else:
            source = (UI / filename).read_text(encoding='utf-8')
        # Actual helper translation units; no copied geometry algorithm.
        source = '#include <QEvent>\n#include <QResizeEvent>\n#include <QShowEvent>\n#include <QVariant>\n' + source.replace('#include "../Framework.h"', '')
        path = work / filename
        path.write_text(source, encoding='utf-8')
        paths.append(path)
    binary = work / ('secondary-layout-tests.exe' if os.name == 'nt' else 'secondary-layout-tests')
    fixture = Path(__file__).with_suffix('.cpp')
    modules = ('QtCore', 'QtGui', 'QtWidgets')
    if Path(args.cxx).name.lower() in ('cl', 'cl.exe'):
        command = [args.cxx, '/nologo', '/std:c++17', '/Zc:__cplusplus', '/utf-8', '/EHsc', '/MD', '/W3', '/WX', '/wd4100',
                   '/DUNICODE', '/D_UNICODE', '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN',
                   f'/I{UI}', f'/I{includes}']
        command.extend(f'/I{includes / module}' for module in modules)
        command.extend([str(fixture), *(str(path) for path in paths), f'/Fe:{binary}',
                        '/link', f'/LIBPATH:{args.qt_root / "lib"}',
                        'Qt6Widgets.lib', 'Qt6Gui.lib', 'Qt6Core.lib'])
    else:
        command = [args.cxx, '-std=c++17', '-fPIC', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                   f'-I{UI}', f'-I{includes}']
        command.extend(f'-I{includes / module}' for module in modules)
        library_roots = [args.qt_root / 'lib']
        library_roots.extend(sorted((args.qt_root / 'lib').glob('*-linux-gnu')))
        command.extend(f'-L{path}' for path in library_roots if path.is_dir())
        command.extend([str(fixture), *(str(path) for path in paths),
                        '-l:libQt6Widgets.so.6', '-l:libQt6Gui.so.6',
                        '-l:libQt6Core.so.6', '-o', str(binary)])
    subprocess.run(command, cwd=work, check=True, timeout=180)
    runtime = work / 'runtime'
    runtime.mkdir(mode=0o700)
    cache = work / 'cache'
    cache.mkdir()
    environment = dict(os.environ, QT_QPA_PLATFORM='offscreen',
                       XDG_RUNTIME_DIR=str(runtime), XDG_CACHE_HOME=str(cache))
    if os.name == 'nt':
        environment['PATH'] = str(args.qt_root / 'bin') + os.pathsep + environment.get('PATH', '')
        environment['QT_PLUGIN_PATH'] = str(args.qt_root / 'plugins')
    return subprocess.run([str(binary)], check=False, timeout=45, env=environment,
                          text=True, capture_output=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qt-root', type=Path, default=Path(os.environ.get('QT_ROOT_DIR', '/usr')))
    parser.add_argument('--cxx', default=shutil.which('cl') or shutil.which('c++') or shutil.which('g++'))
    parser.add_argument('--baseline-ref', help='Also prove the same first-Paint checks reject this Git revision')
    args = parser.parse_args()
    if not args.cxx:
        raise SystemExit('Native C++ compiler unavailable')
    with tempfile.TemporaryDirectory(prefix='ksword-secondary-first-frame-') as temporary:
        work = Path(temporary)
        current = work / 'current'
        current.mkdir()
        result = run_fixture(args, current)
        print(result.stdout, end='')
        if result.returncode:
            print(result.stderr, end='')
            raise SystemExit(result.returncode)
        if args.baseline_ref:
            baseline = work / 'baseline'
            baseline.mkdir()
            result = run_fixture(args, baseline, baseline=True)
            print(result.stdout, end='')
            print(result.stderr, end='')
            if result.returncode != 1 or 'FAILURES=' not in result.stdout or 'FAILURES=0' in result.stdout:
                raise SystemExit('Negative control did not report an expected geometry-test failure')
            print(f'NEGATIVE_CONTROL_REJECTED={args.baseline_ref}')


if __name__ == '__main__':
    main()
