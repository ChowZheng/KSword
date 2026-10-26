#!/usr/bin/env python3
"""Exercise production disk-monitor column controls with actual QtWidgets.

This small offscreen fixture verifies A/B visibility, customization, all-field
export visibility, and menu/view lifetime. It does not run the KSword GUI or
validate production theme painting, Windows IOCTLs, or ETW.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

REPOSITORY = Path(__file__).resolve().parents[1]
MARKER = '// KSWORD_PRODUCTION_COLUMN_PRESETS'


def qt_include_root(prefix: Path) -> Path:
    candidates = [prefix / 'include/qt6', prefix / 'include']
    candidates.extend(sorted((prefix / 'include').glob('*/qt6')))
    for candidate in candidates:
        if (candidate / 'QtWidgets/QApplication').is_file():
            return candidate
    raise SystemExit(f'Qt 6 development headers unavailable under {prefix}')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qt-root', type=Path, default=Path('/usr'),
                        help='Qt development prefix (for example /tmp/ksword-qt/usr)')
    parser.add_argument('--cxx', default=shutil.which('c++') or shutil.which('g++'),
                        help='Native C++ compiler; the fixture uses the installed Qt Linux runtime')
    args = parser.parse_args()
    if not args.cxx:
        raise SystemExit('Native C++ compiler unavailable')
    includes = qt_include_root(args.qt_root)
    production = (REPOSITORY / 'Ksword5.1/Ksword5.1/HardwareDock/DiskMonitorStoragePanel.cpp').read_text()
    start = production.index('namespace disk_monitor_ui\n')
    end = production.index('struct DiskMonitorStoragePerformanceState', start)
    helper = production[start:end]
    fixture = Path(__file__).with_suffix('.cpp').read_text()
    if fixture.count(MARKER) != 1:
        raise SystemExit('Fixture must contain exactly one production-helper marker')
    generated = fixture.replace(MARKER, helper)
    with tempfile.TemporaryDirectory(prefix='ksword-disk-column-ui-') as temporary:
        work = Path(temporary)
        source = work / 'test.cpp'
        binary = work / 'test'
        source.write_text(generated)
        command = [args.cxx, '-std=c++20', '-fPIC', f'-I{includes}']
        command.extend(f'-I{includes / module}' for module in ('QtCore', 'QtGui', 'QtWidgets'))
        # Link versioned runtime sonames; development .so symlinks are unnecessary.
        library_roots = [args.qt_root / 'lib']
        library_roots.extend(sorted((args.qt_root / 'lib').glob('*-linux-gnu')))
        command.extend(f'-L{path}' for path in library_roots if path.is_dir())
        command.extend([str(source), '-l:libQt6Widgets.so.6', '-l:libQt6Gui.so.6',
                        '-l:libQt6Core.so.6', '-o', str(binary)])
        subprocess.run(command, check=True, timeout=120)
        runtime = work / 'runtime'
        runtime.mkdir(mode=0o700)
        cache = work / 'cache'
        cache.mkdir()
        environment = dict(os.environ, QT_QPA_PLATFORM='offscreen',
                           XDG_RUNTIME_DIR=str(runtime), XDG_CACHE_HOME=str(cache))
        subprocess.run([str(binary)], check=True, timeout=30, env=environment)


if __name__ == '__main__':
    main()
