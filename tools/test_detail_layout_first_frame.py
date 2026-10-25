#!/usr/bin/env python3
"""Measure natural first-paint layout using unmodified DetailLayoutHost methods.

The host declaration, binding TU, compatibility TU and lifecycle/layout methods
come from production sources. Editor contents, row expansion and translation are
thin adapters: this fixture measures Qt layout timing, not editor/data behavior.
No render(), repaint(), forced UpdateRequest or nested processEvents is used
while creating or showing the measured UI.
"""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def methods(source: str, name: str) -> list[str]:
    pattern = re.compile(r"(?m)^(?:[\w:*<>]+\s+)?ks::ui::DetailLayoutHost::" + re.escape(name) + r"\s*\(")
    result = []
    for match in pattern.finditer(source):
        opening = source.index('{', match.end())
        depth = 0
        tokens = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/|[{}]', re.S)
        for token in tokens.finditer(source, opening):
            value = token.group()
            if value == '{':
                depth += 1
            elif value == '}':
                depth -= 1
                if not depth:
                    result.append(source[match.start():token.end()])
                    break
        else:
            raise RuntimeError(f'Unterminated production method {name}')
    if not result:
        raise RuntimeError(f'Production method {name} not found')
    return result


ADAPTERS = r'''#pragma once
#include <QWidget>
#include <QSet>
#include <memory>
struct CodeEditorReadOnlyViewState;
class CodeEditorWidget : public QWidget {
    Q_OBJECT
public:
    using QWidget::QWidget;
signals:
    void contentChanged(const QString&);
};
namespace ks::ui {
struct FieldDocument {};
struct StructuredFieldViewState;
class StructuredFieldView : public QWidget {
    Q_OBJECT
public:
    enum class Presentation { Sections, Tree };
    using QWidget::QWidget;
signals:
    void documentChanged();
};
}
'''

BUSINESS_ADAPTERS = r'''
namespace ks::ui {
void DetailLayoutHost::handleViewClicked(const QPersistentModelIndex&) {}
void DetailLayoutHost::handleDetailChanged(const QString&) {}
void DetailLayoutHost::handleFieldDocumentChanged() {}
void DetailLayoutHost::restoreEmbeddedEntryLayout(const EmbeddedEntry&) {}
void DetailLayoutHost::installEmbeddedRowDelegate() {}
void DetailLayoutHost::restoreEmbeddedRowDelegate() {}
void DetailLayoutHost::flushEmbeddedEditorGeometries() {}
}
'''


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qt-root', type=Path, default=Path('/usr'))
    parser.add_argument('--cxx', default=shutil.which('cl' if os.name == 'nt' else 'c++'))
    parser.add_argument('--source-ref', help='Read all measured production files from this Git revision')
    parser.add_argument('--sanitize-address', action='store_true', help='Instrument the measured lifecycle for memory errors')
    parser.add_argument('--work-directory', type=Path, help='Keep generated sources and executable here for debugging')
    args = parser.parse_args()
    if args.sanitize_address and os.name == 'nt':
        parser.error('--sanitize-address currently requires the Linux GNU/Clang build path')
    if not args.cxx:
        parser.error('A C++20 compiler is required')
    roots = [args.qt_root / 'include', args.qt_root / 'include/qt6']
    roots.extend((args.qt_root / 'include').glob('*/qt6'))
    includes = next((p for p in roots if (p / 'QtWidgets/QApplication').is_file()), None)
    moc_candidates = [args.qt_root / 'bin/moc.exe', args.qt_root / 'lib/qt6/libexec/moc', args.qt_root / 'libexec/moc']
    moc = next((p for p in moc_candidates if p.is_file()), None)
    if not includes or not moc:
        parser.error('Qt6 Widgets development files and moc are required; pass --qt-root')
    root = Path(__file__).resolve().parents[1]
    ui = root / 'Ksword5.1/Ksword5.1/UI'
    def production_file(name: str) -> str:
        if args.source_ref:
            return subprocess.check_output(['git', 'show', args.source_ref + ':Ksword5.1/Ksword5.1/UI/' + name], cwd=root).decode('utf-8-sig')
        return (ui / name).read_text(encoding='utf-8-sig')
    if args.work_directory:
        args.work_directory.mkdir(parents=True, exist_ok=False)
    workspace = nullcontext(str(args.work_directory.resolve())) if args.work_directory else tempfile.TemporaryDirectory(prefix='ksword-detail-first-frame-')
    with workspace as directory:
        work = Path(directory)
        header = production_file('DetailLayoutHost.h')
        assert header.count('#include "StructuredFieldView.h"') == 1
        (work / 'DetailLayoutHost.h').write_text(header.replace('#include "StructuredFieldView.h"', '#include "EditorAdapters.h"'), encoding='utf-8')
        (work / 'EditorAdapters.h').write_text(ADAPTERS, encoding='utf-8')
        (work / 'CodeEditorWidget.h').write_text('#include "EditorAdapters.h"\n', encoding='utf-8')
        lifecycle = production_file('DetailLayoutHost.cpp')
        selected = ['DetailLayoutHost', '~DetailLayoutHost', 'initializeHostUi',
                    'scheduleHostUiInitialization', 'ensureManagedSplitter',
                    'clearEmbeddedDetails', 'updateEmbeddedEditorGeometries',
                    'refreshEmbeddedIndicators', 'scheduleEmbeddedIndicatorRefresh',
                    'destroyFloatingWindow', 'eventFilter']
        if 'DetailLayoutHost::prepareHostUi(' in lifecycle:
            selected.append('prepareHostUi')
        bodies = [body for name in selected for body in methods(lifecycle, name)]
        assert len(methods(lifecycle, 'DetailLayoutHost')) == 4
        production = '''#include "DetailLayoutHost.h"
#include <QAbstractItemView>
#include <QAbstractItemModel>
#include <QApplication>
#include <QBoxLayout>
#include <QDialog>
#include <QEvent>
#include <QScrollBar>
#include <QScopeGuard>
#include <QSplitter>
#include <QTableView>
#include <QTableWidget>
#include <QThreadPool>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <algorithm>
#include <utility>
namespace ks::i18n { inline QString text(const QString&, const QString& fallback) { return fallback; } }
'''
        production += '\n\n'.join(bodies) + '\n' + BUSINESS_ADAPTERS
        for filename in ('DetailLayoutHost.Binding.cpp', 'DetailLayoutHost.Compatibility.cpp'):
            content = production_file(filename)
            # Only includes are adapted; every production function body is intact.
            content = re.sub(r'^#include[^\n]*\n', '', content, flags=re.M)
            production += '\n' + content
        (work / 'ProductionMethods.cpp').write_text(production, encoding='utf-8')
        libraries = [args.qt_root / 'lib', *sorted((args.qt_root / 'lib').glob('*-linux-gnu'))]
        environment = dict(os.environ)
        environment['LD_LIBRARY_PATH'] = os.pathsep.join(str(p) for p in libraries if p.is_dir()) + os.pathsep + environment.get('LD_LIBRARY_PATH', '')
        subprocess.run([str(moc), str(work / 'EditorAdapters.h'), '-o', str(work / 'moc_EditorAdapters.cpp')], env=environment, check=True, timeout=30)
        include_paths = [work, ui, includes, *(includes / name for name in ('QtCore', 'QtGui', 'QtWidgets'))]
        sources = [root / 'tools/detail_layout_first_frame_tests.cpp', work / 'ProductionMethods.cpp', work / 'moc_EditorAdapters.cpp']
        executable = work / ('detail_first_frame.exe' if os.name == 'nt' else 'detail_first_frame')
        if os.name == 'nt':
            command = [args.cxx, '/nologo', '/std:c++20', '/Zc:__cplusplus', '/utf-8', '/EHsc', '/MD', '/DNOMINMAX',
                       *('/I' + str(p) for p in include_paths), *(str(p) for p in sources), '/Fe:' + str(executable),
                       '/link', '/LIBPATH:' + str(args.qt_root / 'lib'), 'Qt6Widgets.lib', 'Qt6Gui.lib', 'Qt6Core.lib']
        else:
            command = [args.cxx, '-std=c++20', '-fPIC', '-O1', '-g', *('-I' + str(p) for p in include_paths)]
            if args.sanitize_address:
                command += ['-g', '-fsanitize=address', '-fno-omit-frame-pointer']
                environment['ASAN_OPTIONS'] = 'detect_leaks=0'
            command += ['-L' + str(p) for p in libraries if p.is_dir()]
            command += [*(str(p) for p in sources), '-l:libQt6Widgets.so.6', '-l:libQt6Gui.so.6', '-l:libQt6Core.so.6', '-o', str(executable)]
        subprocess.run(command, cwd=work, env=environment, check=True, timeout=120)
        runtime = work / 'runtime'; runtime.mkdir(mode=0o700)
        cache = work / 'cache'; cache.mkdir()
        environment.update(QT_QPA_PLATFORM='offscreen', XDG_RUNTIME_DIR=str(runtime), XDG_CACHE_HOME=str(cache))
        if os.name == 'nt':
            environment['PATH'] = str(args.qt_root / 'bin') + os.pathsep + environment.get('PATH', '')
            environment['QT_PLUGIN_PATH'] = str(args.qt_root / 'plugins')
        result = subprocess.run([str(executable)], cwd=work, env=environment,
                                capture_output=True, text=True, timeout=60)
        print(result.stdout, end='')
        # Qt's offscreen plugin reports its expected window-manager limitation
        # for every size-hint propagation; retain all other diagnostics.
        diagnostics = '\n'.join(line for line in result.stderr.splitlines()
                                if line != 'This plugin does not support propagateSizeHints()')
        if diagnostics:
            print(diagnostics)
        result.check_returncode()


if __name__ == '__main__':
    main()
