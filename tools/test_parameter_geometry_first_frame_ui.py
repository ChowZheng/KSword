#!/usr/bin/env python3
"""Compare first actual Paint vs settled geometry in two exact production classes.

Only ParameterColumns and ParameterChoiceWrap are extracted, unchanged, from
ParameterWorkspace.cpp. All widgets, layouts, event dispatch and mouse input are
real Qt. No business page, theme-binding queue or Windows main GUI is claimed.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_memory_dock_layout_gate import scan_cpp, find_matching


def extract_class(source, name):
    start = source.index('class ' + name + ' final')
    masked = scan_cpp(source, False)
    opening = masked.index('{', start)
    end = find_matching(masked, opening, '{', '}')
    if end < 0 or source[end + 1] != ';': raise ValueError('Incomplete production class ' + name)
    return source[start:end + 2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qt-root', type=Path, default=Path('/usr'))
    parser.add_argument('--cxx', default=shutil.which('cl' if os.name == 'nt' else 'c++'))
    parser.add_argument('--source-ref', help='Optional Git revision of ParameterWorkspace.cpp')
    args = parser.parse_args()
    roots = [args.qt_root / 'include', args.qt_root / 'include/qt6', *list((args.qt_root / 'include').glob('*/qt6'))]
    include = next((p for p in roots if (p / 'QtWidgets/QApplication').is_file()), None)
    if not args.cxx or not include: parser.error('Native C++ compiler and Qt6 Widgets/Test development files are required')
    root = Path(__file__).resolve().parents[1]
    relative = 'Ksword5.1/Ksword5.1/UI/ParameterWorkspace.cpp'
    source = subprocess.check_output(['git', 'show', f'{args.source_ref}:{relative}'], cwd=root).decode() if args.source_ref else (root / relative).read_text(encoding='utf-8')
    classes = '\n\n'.join(extract_class(source, name) for name in ('ParameterColumns', 'ParameterChoiceWrap'))
    print('PARAMETER_CLASSES_SHA256=' + hashlib.sha256(classes.encode()).hexdigest(), flush=True)
    fixture = (root / 'tools/parameter_geometry_first_frame_ui_tests.cpp').read_text(encoding='utf-8')
    marker = '// KSWORD_PARAMETER_GEOMETRY_CLASSES'
    if fixture.count(marker) != 1: raise ValueError('Expected one class insertion marker')
    with tempfile.TemporaryDirectory(prefix='ksword-parameter-geometry-') as temporary:
        work = Path(temporary); generated = work / 'fixture.cpp'; generated.write_text(fixture.replace(marker, classes), encoding='utf-8')
        binary = work / ('test.exe' if os.name == 'nt' else 'test')
        paths = [include, *(include / module for module in ('QtCore', 'QtGui', 'QtWidgets', 'QtTest'))]
        if os.name == 'nt':
            command = [args.cxx, '/nologo', '/std:c++20', '/Zc:__cplusplus', '/utf-8', '/EHsc', '/MD', '/DNOMINMAX',
                       '/DQT_WIDGETS_LIB', '/DQT_GUI_LIB', *('/I' + str(p) for p in paths), str(generated), '/Fe:' + str(binary),
                       '/link', '/LIBPATH:' + str(args.qt_root / 'lib'), 'Qt6Widgets.lib', 'Qt6Gui.lib', 'Qt6Core.lib', 'Qt6Test.lib']
        else:
            libraries = [args.qt_root / 'lib', *list((args.qt_root / 'lib').glob('*-linux-gnu'))]
            command = [args.cxx, '-std=c++20', '-fPIC', '-DQT_WIDGETS_LIB', '-DQT_GUI_LIB', *('-I' + str(p) for p in paths), *('-L' + str(p) for p in libraries), str(generated), '-l:libQt6Widgets.so.6', '-l:libQt6Gui.so.6', '-l:libQt6Core.so.6', '-l:libQt6Test.so.6', '-o', str(binary)]
        subprocess.run(command, cwd=work, check=True, timeout=120)
        runtime = work / 'runtime'; runtime.mkdir(mode=0o700)
        cache = work / 'cache'; cache.mkdir()
        environment = dict(os.environ, QT_QPA_PLATFORM='offscreen', XDG_RUNTIME_DIR=str(runtime), XDG_CACHE_HOME=str(cache), QTEST_MOUSEEVENT_DELAY='0')
        if os.name == 'nt':
            environment['PATH'] = str(args.qt_root / 'bin') + os.pathsep + environment.get('PATH', '')
            environment['QT_PLUGIN_PATH'] = str(args.qt_root / 'plugins')
        else:
            environment['LD_LIBRARY_PATH'] = os.pathsep.join(str(p) for p in libraries) + os.pathsep + environment.get('LD_LIBRARY_PATH', '')
        subprocess.run([str(binary)], env=environment, check=True, timeout=60)


if __name__ == '__main__': main()
