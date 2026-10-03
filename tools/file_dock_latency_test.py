"""Compile and run the actual FileDock models in a bounded Qt offscreen harness.

Extracts production classes without linking the rest of the application. Only
theme/i18n and the blocking reparse query are substituted; Qt models, sorting,
queued callbacks and object lifetimes are real. Requires the x64 MSVC/Qt SDKs.
"""

import argparse
import ctypes
import os
from pathlib import Path
import subprocess
import shutil
import tempfile


def between(source, start, end):
    return source[source.index(start):source.index(end, source.index(start))]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qt-dir', type=Path, default=Path('D:/Software/Qt/6.9.3/msvc2022_64'))
    parser.add_argument('--vcvars', type=Path, default=Path('D:/Software/VS/VC/Auxiliary/Build/vcvars64.bat'))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = (root / 'Ksword5.1/Ksword5.1/FileDock/FileDock.cpp').read_text(encoding='utf-8')
    header = (root / 'Ksword5.1/Ksword5.1/FileDock/ManualFileSystemParser.h').read_text(encoding='utf-8')
    classes = '\n'.join([
        between(source, '    class AsyncReparseMarkerCache', '    // ExplorerFileSortProxyModel'),
        between(source, '    class ExplorerFileSortProxyModel', '    // buildDriverNtPath'),
        between(source, '    enum class ManualModelColumn', '    // manualFsTypeToText'),
        between(source, '    class ManualDirectoryModel', '    // buildSuspiciousNameSet'),
    ])
    entry = between(header, '    struct ManualDirectoryEntry', '    // MftScanDiagnostics')
    harness = r'''
#include <QtWidgets>
#include <QtCore>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <thread>
#include <vector>
namespace ks::i18n { QString displayText(const QString& s) { return s; } }
namespace KswordTheme {
enum class AccentRole { Orange };
QString AccentHex(AccentRole) { return QStringLiteral("#ff8800"); }
}
std::atomic<int> probes{0};
std::atomic<int> probesOnUi{0};
QString reparseKindMarkerForPath(const QString& path) {
    ++probes;
    if (QThread::currentThread() == qApp->thread()) ++probesOnUi;
    if (path.contains(QStringLiteral("slow")))
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    return path.contains(QStringLiteral("marked")) ? QStringLiteral("JUNCTION") : QString();
}
'''
    harness += '\nnamespace ks::file {\n' + entry + '\n}\n' + classes
    harness += r'''
struct kLogEvent {};
struct NullLog { template<class T> NullLog& operator<<(const T&) { return *this; } } dbg;
constexpr int eol = 0;
class FileDock : public QObject {
public:
    struct FilePanelWidgets {
        QTreeView* fileView = nullptr;
        QAbstractTableModel* manualModel = nullptr;
        QSortFilterProxyModel* manualProxyModel = nullptr;
        QFileSystemModel* fsModel = nullptr;
        QSortFilterProxyModel* proxyModel = nullptr;
        QLabel* pathStatusLabel = nullptr;
        QLabel* selectionStatusLabel = nullptr;
        QLabel* diskStatusLabel = nullptr;
        QString currentPath;
        QString panelNameText;
        QString lastStatusLogSignature;
        int statusRequestSerial = 0;
        bool statusQueryInProgress = false;
        bool manualMode = true;
    };
    FilePanelWidgets m_leftPanel, m_rightPanel;
    bool currentModeIsManual(const FilePanelWidgets& panel) const { return panel.manualMode; }
    QString currentIndexPath(const FilePanelWidgets& panel) const;
    std::vector<QString> selectedPaths(const FilePanelWidgets& panel) const;
    void updatePanelStatus(FilePanelWidgets& panel);
    static QString formatSizeText(std::uint64_t size);
};
'''
    harness += between(source, 'void FileDock::updatePanelStatus(', 'void FileDock::selectPendingPath(')
    harness += between(source, 'QString FileDock::currentIndexPath(', 'QString FileDock::formatSizeText(')
    harness += source[source.index('QString FileDock::formatSizeText('):]
    harness += r'''
void require(bool condition, const char* message) {
    if (!condition) { std::cerr << message << std::endl; std::exit(1); }
}
template<class F> bool until(F ready, int timeout = 5000) {
    QElapsedTimer timer; timer.start();
    while (!ready() && timer.elapsed() < timeout) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return ready();
}
std::shared_ptr<ManualDirectoryModel::Entries> entries(int count, const QString& prefix) {
    auto result = std::make_shared<ManualDirectoryModel::Entries>();
    result->reserve(count);
    for (int i = count; i > 0; --i) {
        ks::file::ManualDirectoryEntry entry;
        entry.name = QStringLiteral("item%1").arg(i);
        entry.absolutePath = prefix + QLatin1Char('/') + entry.name;
        entry.sizeBytes = static_cast<std::uint64_t>(count - i + 1);
        entry.modifiedTime = QDateTime::fromSecsSinceEpoch(i);
        entry.typeText = QStringLiteral("file");
        result->push_back(std::move(entry));
    }
    return result;
}
int main(int argc, char** argv) {
    std::cout << "starting QApplication" << std::endl;
    QApplication app(argc, argv);
    std::cout << "checking large snapshot" << std::endl;
    auto format = [](std::uint64_t size) { return QString::number(size); };
    ManualDirectoryModel model(format);
    ExplorerFileSortProxyModel proxy;
    proxy.setSourceModel(&model);
    auto many = entries(100000, QStringLiteral("synthetic"));
    QElapsedTimer timing; timing.start();
    model.setSnapshot(many, {QStringLiteral("item2")});
    const auto publishMs = timing.elapsed();
    require(model.rowCount() == 100000 && model.columnCount() == 6, "snapshot lost rows/columns");
    require(publishMs < 500, "snapshot publication blocked UI");
    timing.restart();
    proxy.sort(0);
    const auto sortMs = timing.elapsed();
    std::cout << "publish_ms=" << publishMs << " sort_ms=" << sortMs << std::endl;
    require(proxy.index(0, 0).data().toString() == QStringLiteral("item1"), "natural sorting failed");
    require(proxy.index(1, 0).data().toString() == QStringLiteral("item2"), "natural numeric sorting failed");
    require(proxy.index(1, 0).data(Qt::BackgroundRole).isValid(), "suspicious highlight lost");
    require(!proxy.index(1, 0).data(Qt::ToolTipRole).toString().isEmpty(), "suspicious tooltip lost");
    proxy.sort(1);
    require(proxy.index(0, 1).data(Qt::UserRole).toULongLong() == 1, "numeric size sorting failed");
    proxy.sort(3);
    require(proxy.index(0, 3).data(Qt::UserRole).toDateTime().toSecsSinceEpoch() == 1,
        "timestamp sorting failed");
    require(probes == 0, "sorting queried reparse metadata");
    auto withDirectory = entries(3, QStringLiteral("synthetic"));
    (*withDirectory)[0].isDirectory = true;
    model.setSnapshot(withDirectory);
    proxy.sort(0);
    require(proxy.index(0, 0).data(Qt::UserRole + 1).toBool(), "directory priority lost");
    proxy.setFilterFixedString(QStringLiteral("item2"));
    require(proxy.rowCount() == 1, "name filtering failed");
    proxy.setFilterFixedString(QString());

    std::cout << "checking large selection" << std::endl;
    model.setSnapshot(many);
    FileDock dock;
    QTreeView view; view.setUniformRowHeights(true); view.setModel(&proxy);
    view.setSelectionMode(QAbstractItemView::ExtendedSelection);
    view.setSelectionBehavior(QAbstractItemView::SelectRows);
    view.selectAll();
    QLabel pathLabel, selectionLabel, diskLabel;
    auto& panel = dock.m_leftPanel;
    panel.fileView = &view; panel.manualModel = &model; panel.manualProxyModel = &proxy;
    panel.pathStatusLabel = &pathLabel; panel.selectionStatusLabel = &selectionLabel;
    panel.diskStatusLabel = &diskLabel; panel.currentPath = QDir::currentPath();
    timing.restart();
    const auto selected = dock.selectedPaths(panel);
    const auto selectMs = timing.elapsed();
    require(selected.size() == 100000 && selectMs < 1000, "large selection lost paths or stalled UI");
    timing.restart();
    dock.updatePanelStatus(panel);
    const auto statusMs = timing.elapsed();
    require(statusMs < 1000, "selection status stalled UI");
    require(selectionLabel.text().contains(QStringLiteral("100000")) &&
        selectionLabel.text().contains(FileDock::formatSizeText(5000050000ULL)), "cached size total incorrect");
    view.clearSelection();
    view.setCurrentIndex(proxy.index(0, 0));
    dock.updatePanelStatus(panel);
    require(until([&]() { return !panel.statusQueryInProgress; }), "status query never completed");
    require(selectionLabel.text().startsWith(QStringLiteral("选中: 1 ")), "stale selection status overwrote new count");
    std::cout << "selection_ms=" << selectMs << " status_ms=" << statusMs << std::endl;

    ManualDirectoryModel slow(format);
    std::cout << "checking async queries" << std::endl;
    slow.setSnapshot(entries(1, QStringLiteral("slow-marked")));
    int heartbeats = 0;
    QTimer heartbeat; heartbeat.setInterval(1);
    QObject::connect(&heartbeat, &QTimer::timeout, [&]() { ++heartbeats; });
    heartbeat.start();
    timing.restart();
    for (int i = 0; i < 100; ++i) slow.index(0, 2).data();
    require(timing.elapsed() < 50, "display data blocked on reparse IO");
    require(until([&]() { return slow.index(0, 2).data().toString().contains(QStringLiteral("JUNCTION")); }),
        "async marker never arrived");
    require(heartbeats > 20, "slow query stalled event loop");
    const int cachedProbes = probes;
    for (int i = 0; i < 100; ++i) slow.index(0, 2).data();
    QCoreApplication::processEvents();
    require(probes == cachedProbes, "positive marker cache did not deduplicate queries");

    ManualDirectoryModel negative(format);
    negative.setSnapshot(entries(1, QStringLiteral("plain")));
    negative.index(0, 2).data();
    require(until([&]() { return probes > cachedProbes; }), "negative probe never started");
    QThreadPool::globalInstance()->waitForDone(); QCoreApplication::processEvents();
    const int negativeProbes = probes;
    for (int i = 0; i < 100; ++i) negative.index(0, 2).data();
    QCoreApplication::processEvents();
    require(probes == negativeProbes, "negative cache did not deduplicate queries");

    slow.setSnapshot(entries(1, QStringLiteral("slow-marked-old")));
    slow.index(0, 2).data();
    require(until([&]() { return probes > negativeProbes; }), "stale query never started");
    slow.setSnapshot(entries(1, QStringLiteral("new-plain")));
    slow.index(0, 2).data();
    QThreadPool::globalInstance()->waitForDone(); QCoreApplication::processEvents();
    QThreadPool::globalInstance()->waitForDone(); QCoreApplication::processEvents();
    require(slow.index(0, 2).data().toString() == QStringLiteral("file"), "old marker overwrote new snapshot");
    auto closed = std::make_unique<ManualDirectoryModel>(format);
    closed->setSnapshot(entries(1, QStringLiteral("slow-marked-close")));
    const int beforeClose = probes;
    closed->index(0, 2).data();
    require(until([&]() { return probes > beforeClose; }), "close query never started");
    closed.reset();
    QThreadPool::globalInstance()->waitForDone(); QCoreApplication::processEvents();

    QTemporaryDir directory;
    std::cout << "checking filesystem cache" << std::endl;
    require(directory.isValid(), "temporary directory creation failed");
    for (int i = 1; i <= 1500; ++i) {
        QFile file(directory.filePath(QStringLiteral("file%1.txt").arg(i)));
        require(file.open(QIODevice::WriteOnly), "fixture creation failed");
        file.write(QByteArray(i % 127 + 1, 'x'));
    }
    ReparseAwareFileSystemModel fs;
    fs.setFilter(QDir::AllEntries | QDir::NoDotAndDotDot);
    bool loaded = false;
    QObject::connect(&fs, &QFileSystemModel::directoryLoaded, [&](const QString& path) {
        if (QDir(path) == QDir(directory.path())) loaded = true;
    });
    const QModelIndex sourceRoot = fs.setRootPath(directory.path());
    require(until([&]() { return loaded && fs.rowCount(sourceRoot) == 1500; }), "filesystem model did not load");
    ExplorerFileSortProxyModel fsProxy;
    fsProxy.setSourceModel(&fs);
    const int beforeFsSort = probes;
    for (int column = 0; column < 4; ++column) fsProxy.sort(column);
    require(probes == beforeFsSort, "filesystem sort triggered reparse IO");
    fsProxy.sort(0);
    const QModelIndex proxyRoot = fsProxy.mapFromSource(sourceRoot);
    require(fsProxy.index(1, 0, proxyRoot).data(QFileSystemModel::FileNameRole).toString() ==
        QStringLiteral("file2.txt"), "filesystem natural sorting failed");
    fs.index(0, 0, sourceRoot).data();
    require(until([&]() { return probes > beforeFsSort; }), "filesystem display did not queue probe");
    QThreadPool::globalInstance()->waitForDone(); QCoreApplication::processEvents();
    const int afterFsProbe = probes;
    fs.index(0, 0, sourceRoot).data();
    QCoreApplication::processEvents();
    require(probes == afterFsProbe, "filesystem negative cache failed");
    require(probesOnUi == 0, "reparse IO ran on the UI thread");
    std::cout << "PASS rows=100000 publish_ms=" << publishMs << " sort_ms=" << sortMs
        << " heartbeats=" << heartbeats << " ui_probes=" << probesOnUi << std::endl;
    QThreadPool::globalInstance()->waitForDone(); QCoreApplication::processEvents();
}
'''
    with tempfile.TemporaryDirectory(prefix='ksword-filedock-test-') as temp:
        out = Path(temp)
        cpp = out / 'test.cpp'
        exe = out / 'test.exe'
        cpp.write_text(harness, encoding='utf-8')
        build = out / 'build.cmd'
        qt = args.qt_dir.resolve()
        build.write_text(
            f'@echo off\ncall "{args.vcvars.resolve()}" >nul\n'
            'if errorlevel 1 exit /b %errorlevel%\n'
            'cl /nologo /std:c++17 /permissive- /Zc:__cplusplus /utf-8 /EHsc /MD /W4 /WX /O2 /DNOMINMAX '
            f'/external:W0 /external:I"{qt / "include"}" '
            f'/external:I"{qt / "include/QtCore"}" /external:I"{qt / "include/QtGui"}" '
            f'/external:I"{qt / "include/QtWidgets"}" "{cpp}" '
            f'/Fo"{out / "test.obj"}" /Fe"{exe}" /link /LIBPATH:"{qt / "lib"}" '
            'Qt6Core.lib Qt6Gui.lib Qt6Widgets.lib\nif errorlevel 1 exit /b %errorlevel%\n'
            f'"{qt / "bin/windeployqt.exe"}" --no-translations --no-system-d3d-compiler '
            f'--no-opengl-sw --compiler-runtime "{exe}"\nexit /b %errorlevel%\n', encoding='utf-8')
        subprocess.run(['cmd.exe', '/d', '/c', str(build)], check=True, cwd=root,
                       stdout=subprocess.DEVNULL)
        shutil.copy2(qt / 'plugins/platforms/qoffscreen.dll', out / 'platforms/qoffscreen.dll')
        for dll in (root / 'Ksword5.1/x64/Release').glob('*140*.dll'):
            shutil.copy2(dll, out / dll.name)
        environment = dict(os.environ, QT_QPA_PLATFORM='offscreen')
        environment['PATH'] = os.pathsep.join([
            str(qt / 'bin'), str(root / 'Ksword5.1/x64/Release'), environment['PATH']])
        previous = ctypes.windll.kernel32.SetErrorMode(3)
        try:
            subprocess.run([str(exe)], check=True, env=environment, timeout=30)
        finally:
            ctypes.windll.kernel32.SetErrorMode(previous)


if __name__ == '__main__':
    main()
