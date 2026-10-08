#!/usr/bin/env python3
"""Standalone Windows Qt verification; does not replace the MSVC production build."""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import shutil
import subprocess

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qt-dir', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--compiler', default='g++')
    parser.add_argument('--incremental', action='store_true', help='Reuse objects newer than their source files in the same output directory.')
    parser.add_argument('--force-source', action='append', default=[], help='Recompile this source basename even with --incremental (repeatable).')
    arguments = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = arguments.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    qt = arguments.qt_dir.resolve()
    source = root / 'Ksword5.1/Ksword5.1'
    files = [root / 'tools/privilege_workbench_tests.cpp'] + [source / 'PrivilegeDock' / name for name in
        ('PrivilegeAccountPages.cpp', 'PrivilegeTokenPages.cpp', 'PrivilegeAccessPage.cpp',
         'PrivilegeSnapshotPage.cpp', 'PrivilegeSnapshotModel.cpp')]
    files += [source / name for name in ('Internationalization/LanguageManager.cpp',
        'UI/ThemeStatusRole.cpp', 'UI/ThemeControlGlyphs.cpp', 'ksword/process/process_run_as.cpp')]
    flags = ['-std=c++23', '-Wall', '-Wextra', '-Werror', '-Wno-unknown-pragmas',
        '-DNOMINMAX', '-DUNICODE', '-D_UNICODE', '-O1']
    for include in ('include', 'include/QtCore', 'include/QtGui', 'include/QtWidgets'):
        flags += ['-isystem', str(qt / include)]
    objects = []
    for file in files:
        obj = output / (file.stem + '.o')
        if (not arguments.incremental or file.name in arguments.force_source
                or not obj.exists() or obj.stat().st_mtime < file.stat().st_mtime):
            subprocess.run([arguments.compiler, *flags, '-c', str(file), '-o', str(obj)], check=True, timeout=120)
        objects.append(str(obj))
    executable = output / 'privilege_workbench_tests.exe'
    subprocess.run([arguments.compiler, *objects, '-L' + str(qt / 'lib'), '-o', str(executable),
        '-lQt6Widgets', '-lQt6Gui', '-lQt6Core', '-ladvapi32', '-lnetapi32', '-lsecur32',
        '-lshell32', '-luserenv', '-lauthz'], check=True, timeout=120)
    language_dir = output / 'languages'
    language_dir.mkdir(exist_ok=True)
    for language in ('en-US', 'zh-CN'):
        shutil.copy2(source / 'languages' / (language + '.json'), language_dir)
    screenshots = output / 'screenshots'
    screenshots.mkdir(exist_ok=True)
    environment = dict(os.environ, QT_QPA_PLATFORM='offscreen', QT_PLUGIN_PATH=str(qt / 'plugins'))
    environment['PATH'] = str(qt / 'bin') + os.pathsep + environment['PATH']
    subprocess.run([str(executable), str(screenshots)], check=True, timeout=120, env=environment)

if __name__ == '__main__':
    main()
