#!/usr/bin/env python3
"""Verify real ToolbarMetrics geometry at the first Paint and after the queue drains."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qt-root', type=Path, default=Path('/usr'))
    parser.add_argument('--cxx', default=shutil.which('cl' if os.name == 'nt' else 'c++'))
    parser.add_argument('--source-ref', help='Optional Git revision of ToolbarMetrics.h for baseline rejection')
    args = parser.parse_args()
    roots = [args.qt_root / 'include', args.qt_root / 'include/qt6', *list((args.qt_root / 'include').glob('*/qt6'))]
    include = next((path for path in roots if (path / 'QtWidgets/QApplication').is_file()), None)
    if not args.cxx or not include: parser.error('Native C++ compiler and Qt6 Widgets development files are required')
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='ksword-toolbar-first-frame-') as temporary:
        work = Path(temporary)
        binary = work / ('toolbar.exe' if os.name == 'nt' else 'toolbar')
        paths = [include, *(include / module for module in ('QtCore', 'QtGui', 'QtWidgets')), root / 'Ksword5.1/Ksword5.1']
        if args.source_ref:
            baseline = work / 'baseline'
            header = baseline / 'UI/ToolbarMetrics.h'
            header.parent.mkdir(parents=True)
            header.write_bytes(subprocess.check_output(['git', 'show', f'{args.source_ref}:Ksword5.1/Ksword5.1/UI/ToolbarMetrics.h'], cwd=root))
            paths.insert(0, baseline)
        source = root / 'tools/toolbar_first_frame_ui_tests.cpp'
        if os.name == 'nt':
            command = [args.cxx, '/nologo', '/std:c++20', '/Zc:__cplusplus', '/utf-8', '/EHsc', '/MD', '/DNOMINMAX',
                       *('/I' + str(path) for path in paths), str(source), '/Fe:' + str(binary), '/link',
                       '/LIBPATH:' + str(args.qt_root / 'lib'), 'Qt6Widgets.lib', 'Qt6Gui.lib', 'Qt6Core.lib']
        else:
            command = [args.cxx, '-std=c++20', '-fPIC', *('-I' + str(path) for path in paths), str(source), '-l:libQt6Widgets.so.6', '-l:libQt6Gui.so.6', '-l:libQt6Core.so.6', '-o', str(binary)]
        subprocess.run(command, cwd=work, check=True, timeout=120)
        runtime = work / 'runtime'; runtime.mkdir(mode=0o700)
        cache = work / 'cache'; cache.mkdir()
        environment = dict(os.environ, QT_QPA_PLATFORM='offscreen', XDG_RUNTIME_DIR=str(runtime), XDG_CACHE_HOME=str(cache))
        if os.name == 'nt':
            environment['PATH'] = str(args.qt_root / 'bin') + os.pathsep + environment.get('PATH', '')
            environment['QT_PLUGIN_PATH'] = str(args.qt_root / 'plugins')
        subprocess.run([str(binary)], env=environment, check=True, timeout=30)


if __name__ == '__main__': main()
