"""Compile and run the actual FileDock models in a bounded Qt offscreen harness.

Extracts production classes without linking the rest of the application. Only
theme/i18n and reparse IO are substituted. Presentation queries use the actual
Shell/MIME implementation with injected delays/failures for lifetime tests.
Qt models, sorting and callbacks are real. Requires the x64 MSVC/Qt SDKs.
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
    parser.add_argument('--system32', action='store_true', help='Measure visible System32 loading and event-loop stalls')
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
    classes = classes.replace('FilePresentation queryFilePresentation(', 'FilePresentation realQueryFilePresentation(')
    classes = classes.replace('    // 只为被视图请求的项补充展示信息', r'''
FilePresentation queryFilePresentation(const QString& path) {
    ++presentationQueries;
    if (QThread::currentThread() == qApp->thread()) ++presentationOnUi;
    if (path.contains(QStringLiteral("slow-presentation")))
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    if (path.contains(QStringLiteral("failed-presentation"))) return {};
    return realQueryFilePresentation(path);
}
    // 只为被视图请求的项补充展示信息''')
    classes = classes.replace('// QSortFilterProxyModel::lessThan 接收的是源模型索引', '++compareCalls;\n            // QSortFilterProxyModel::lessThan 接收的是源模型索引')
    entry = between(header, '    struct ManualDirectoryEntry', '    // MftScanDiagnostics')
    harness = r'''
#include <QtWidgets>
#include <QtCore>
#include <Windows.h>
#include <Shellapi.h>
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
class TimedApplication : public QApplication {
public:
    using QApplication::QApplication;
    bool measure = false;
    bool notify(QObject* receiver, QEvent* event) override {
        QElapsedTimer timing; timing.start();
        const QByteArray receiverClass = receiver->metaObject()->className();
        const int type = int(event->type());
        const bool result = QApplication::notify(receiver, event);
        if (measure && timing.elapsed() >= 20)
            std::cout << "SLOW_EVENT type=" << type << " receiver="
                << receiverClass.constData() << " ms=" << timing.elapsed() << std::endl;
        return result;
    }
};
std::atomic<int> probes{0};
std::atomic<int> compareCalls{0};
std::atomic<int> presentationQueries{0};
std::atomic<int> presentationOnUi{0};
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
void checkRefreshScroll() {
    std::cout << "checking refresh viewport preservation" << std::endl;
    const QString directory = QStringLiteral("scroll-fixture");
    QString current = directory;
    auto format = [](std::uint64_t size) { return QString::number(size); };
    ManualDirectoryModel model(format);
    ExplorerFileSortProxyModel proxy; proxy.setSourceModel(&model); proxy.sort(0);
    auto original = entries(5000, directory);
    model.setSnapshot(original);
    QTreeView tree;
    tree.setUniformRowHeights(true);
    tree.setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    tree.setModel(&proxy); tree.resize(500, 320); tree.show();
    require(until([&]() { return tree.verticalScrollBar()->maximum() > 1000; }), "tree did not lay out");
    tree.scrollTo(proxy.index(1800, 0), QAbstractItemView::PositionAtTop);
    tree.verticalScrollBar()->setValue(tree.verticalScrollBar()->value() + 7);
    tree.horizontalScrollBar()->setValue(50);
    const int horizontal = tree.horizontalScrollBar()->value();
    const QModelIndex oldAnchor = tree.indexAt(QPoint(8, 0));
    const QString anchorPath = oldAnchor.data(Qt::UserRole).toString();
    const int anchorTop = tree.visualRect(oldAnchor).top();
    require(!anchorPath.isEmpty(), "tree anchor not captured");
    auto begin = [&](QAbstractItemView& view) {
        auto* state = new FileRefreshScrollState(&view, directory, [&]() { return current == directory; });
        state->waitForModel(&model);
    };
    auto restored = [&](QAbstractItemView& view) {
        return until([&]() {
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            return !view.findChild<QObject*>(QStringLiteral("file_refresh_scroll_state"), Qt::FindDirectChildrenOnly);
        });
    };
    auto inserted = std::make_shared<ManualDirectoryModel::Entries>(*original);
    for (int i = 0; i < 20; ++i) {
        ks::file::ManualDirectoryEntry entry;
        entry.name = QStringLiteral("ahead%1").arg(i);
        entry.absolutePath = directory + QLatin1Char('/') + entry.name;
        inserted->push_back(entry);
    }
    begin(tree);
    model.setSnapshot(inserted);
    proxy.sort(0);
    require(restored(tree), "tree restoration did not finish");
    const QModelIndex newAnchor = tree.indexAt(QPoint(8, 0));
    require(newAnchor.data(Qt::UserRole).toString() == anchorPath && tree.visualRect(newAnchor).top() == anchorTop,
        "inserting earlier rows shifted the visible file or pixel offset");
    require(tree.horizontalScrollBar()->value() == horizontal, "horizontal scroll position was lost");

    const int beforeDeletion = tree.verticalScrollBar()->value();
    begin(tree);
    auto deleted = std::make_shared<ManualDirectoryModel::Entries>(*inserted);
    deleted->erase(std::remove_if(deleted->begin(), deleted->end(),
        [&](const auto& entry) { return entry.absolutePath == anchorPath; }), deleted->end());
    model.setSnapshot(deleted);
    require(restored(tree) && tree.verticalScrollBar()->value() == beforeDeletion,
        "deleted anchor jumped to the table head");
    begin(tree);
    model.setSnapshot(entries(60, directory));
    require(restored(tree) && tree.verticalScrollBar()->value() == tree.verticalScrollBar()->maximum(),
        "shrinking rows did not clamp the old scroll position");

    model.setSnapshot(original);
    tree.scrollTo(proxy.index(1800, 0), QAbstractItemView::PositionAtTop);
    const int beforeRepeat = tree.verticalScrollBar()->value();
    begin(tree);
    model.setSnapshot({});
    begin(tree);
    model.setSnapshot(original);
    require(restored(tree) && tree.verticalScrollBar()->value() == beforeRepeat,
        "repeated refresh saved the temporarily empty viewport");
    begin(tree);
    model.setSnapshot(original);
    QKeyEvent home(QEvent::KeyPress, Qt::Key_Home, Qt::NoModifier);
    QApplication::sendEvent(&tree, &home);
    tree.scrollToTop();
    require(restored(tree) && tree.verticalScrollBar()->value() == 0,
        "pending restoration overrode user input");
    tree.scrollTo(proxy.index(1800, 0), QAbstractItemView::PositionAtTop);
    begin(tree);
    current = QStringLiteral("other-directory");
    model.setSnapshot(entries(5000, current));
    tree.scrollToTop();
    require(restored(tree) && tree.verticalScrollBar()->value() == 0,
        "old directory scroll position leaked into navigation");
    current = directory;

    QTreeView detached;
    auto detachedProxy = std::make_unique<ExplorerFileSortProxyModel>();
    detachedProxy->setSourceModel(&model);
    detached.setModel(detachedProxy.get()); detached.show();
    begin(detached);
    model.setSnapshot(original);
    detachedProxy.reset();
    require(restored(detached), "destroyed model left a pending restoration");
    auto closedView = std::make_unique<QTreeView>();
    closedView->setModel(&proxy); closedView->show();
    begin(*closedView);
    model.setSnapshot(original);
    closedView.reset();
    QCoreApplication::processEvents();

    for (const auto mode : {QListView::ListMode, QListView::IconMode}) {
        model.setSnapshot(original);
        QListView list;
        list.setModel(&proxy); list.setViewMode(mode);
        list.setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        list.setUniformItemSizes(true); list.setLayoutMode(QListView::Batched); list.setBatchSize(128);
        if (mode == QListView::IconMode) { list.setGridSize(QSize(128, 96)); list.setWrapping(true); }
        list.resize(520, 320); list.show();
        require(until([&]() { return !list.visualRect(proxy.index(4999, 0)).isEmpty(); }), "batched fixture did not lay out");
        list.scrollTo(proxy.index(2200, 0), QAbstractItemView::PositionAtTop);
        QCoreApplication::processEvents();
        const int oldValue = list.verticalScrollBar()->value();
        begin(list);
        model.setSnapshot({});
        model.setSnapshot(original);
        require(restored(list) && std::abs(list.verticalScrollBar()->value() - oldValue) <= 1,
            "batched list/icon refresh lost its scroll position");
    }
}
int main(int argc, char** argv) {
    std::cout << "starting QApplication" << std::endl;
    TimedApplication app(argc, argv);
    if (app.arguments().contains(QStringLiteral("--system32"))) {
        std::cout << "checking visible System32 load" << std::endl;
        ReparseAwareFileSystemModel realFs;
        realFs.setReadOnly(false);
        realFs.setResolveSymlinks(true);
        realFs.setFilter(QDir::AllEntries | QDir::NoDotAndDotDot);
        ExplorerFileSortProxyModel realProxy;
        realProxy.setSourceModel(&realFs);
        QTreeView realView;
        realView.setUniformRowHeights(true);
        realView.setModel(&realProxy);
        realView.setSortingEnabled(true);
        realView.header()->setStretchLastSection(false);
        realView.header()->setSectionResizeMode(0, QHeaderView::Stretch);
        realView.resize(800, 600);
        realView.show();
        // 测量已经打开的文件面板导航，排除 Qt 第一次创建/绘制窗口的启动成本。
        QElapsedTimer warmup; warmup.start();
        until([&]() { return warmup.elapsed() >= 100; });
        const QString path = QDir::fromNativeSeparators(qEnvironmentVariable("SystemRoot")) + QStringLiteral("/System32");
        bool done = false;
        QObject::connect(&realFs, &QFileSystemModel::directoryLoaded, [&](const QString& loadedPath) {
            if (QDir(loadedPath) == QDir(path)) {
                realView.sortByColumn(0, Qt::AscendingOrder);
                done = true;
            }
        });
        qint64 maxGap = 0;
        QElapsedTimer lastBeat; lastBeat.start();
        QTimer loopProbe;
        loopProbe.setInterval(1);
        QObject::connect(&loopProbe, &QTimer::timeout, [&]() {
            maxGap = std::max(maxGap, lastBeat.restart());
        });
        loopProbe.start();
        app.measure = true;
        compareCalls = 0;
        const QModelIndex realRoot = realFs.setRootPath(path);
        realView.setRootIndex(realProxy.mapFromSource(realRoot));
        realView.sortByColumn(0, Qt::AscendingOrder);
        require(until([&]() { return done; }, 30000), "System32 load never completed");
        QElapsedTimer settle; settle.start();
        until([&]() { return settle.elapsed() >= 1500; });
        std::cout << "checking visible System32 refresh" << std::endl;
        realView.scrollTo(realProxy.index(2500, 0, realView.rootIndex()), QAbstractItemView::PositionAtTop);
        const int previousScroll = realView.verticalScrollBar()->value();
        ReparseAwareFileSystemModel refreshedFs;
        refreshedFs.setReadOnly(false);
        refreshedFs.setFilter(QDir::AllEntries | QDir::NoDotAndDotDot);
        auto* scrollState = new FileRefreshScrollState(&realView, path, []() { return true; });
        scrollState->waitForModel(&refreshedFs);
        realProxy.setSourceModel(&refreshedFs);
        const QModelIndex refreshedRoot = refreshedFs.setRootPath(path);
        realView.setRootIndex(realProxy.mapFromSource(refreshedRoot));
        require(until([&]() {
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            return !realView.findChild<QObject*>(QStringLiteral("file_refresh_scroll_state"), Qt::FindDirectChildrenOnly);
        }, 30000), "System32 refresh scroll restoration never completed");
        require(realView.verticalScrollBar()->value() == previousScroll,
            "recreating the System32 filesystem model lost the viewport");
        require(realFs.rowCount(realRoot) > 1000, "System32 fixture was unexpectedly small");
        require(maxGap < 100, "System32 load exceeded the 100ms UI stall budget");
        require(presentationOnUi == 0, "file presentation IO ran on the UI thread");
        app.measure = false;
        std::cout << "SYSTEM32 rows=" << realFs.rowCount(realRoot) << " max_ui_gap_ms=" << maxGap
            << " comparisons=" << compareCalls << std::endl;
    }
    QThreadPool::globalInstance()->waitForDone(); QCoreApplication::processEvents();
    probes = 0;
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
    for (const QString& name : {QStringLiteral("slow-presentation.txt"), QStringLiteral("failed-presentation.txt")}) {
        QFile file(directory.filePath(name));
        require(file.open(QIODevice::WriteOnly), "presentation fixture creation failed");
        file.write("text fixture");
    }
    ReparseAwareFileSystemModel fs;
    fs.setFilter(QDir::AllEntries | QDir::NoDotAndDotDot);
    bool loaded = false;
    QObject::connect(&fs, &QFileSystemModel::directoryLoaded, [&](const QString& path) {
        if (QDir(path) == QDir(directory.path())) loaded = true;
    });
    const QModelIndex sourceRoot = fs.setRootPath(directory.path());
    require(until([&]() { return loaded && fs.rowCount(sourceRoot) == 1502; }), "filesystem model did not load");
    ExplorerFileSortProxyModel fsProxy;
    fsProxy.setSourceModel(&fs);
    const int beforeFsSort = probes;
    const int beforeFsPresentation = presentationQueries;
    for (int column = 0; column < 4; ++column) fsProxy.sort(column);
    require(probes == beforeFsSort, "filesystem sort triggered reparse IO");
    require(presentationQueries == beforeFsPresentation, "filesystem sorting queued Shell/MIME queries");
    fsProxy.sort(0);
    const QModelIndex proxyRoot = fsProxy.mapFromSource(sourceRoot);
    require(fsProxy.index(2, 0, proxyRoot).data(QFileSystemModel::FileNameRole).toString() ==
        QStringLiteral("file2.txt"), "filesystem natural sorting failed");
    fs.index(0, 0, sourceRoot).data();
    require(until([&]() { return probes > beforeFsSort; }), "filesystem display did not queue probe");
    QThreadPool::globalInstance()->waitForDone(); QCoreApplication::processEvents();
    const int afterFsProbe = probes;
    fs.index(0, 0, sourceRoot).data();
    QCoreApplication::processEvents();
    require(probes == afterFsProbe, "filesystem negative cache failed");
    require(probesOnUi == 0, "reparse IO ran on the UI thread");
    std::cout << "checking async Shell/MIME presentation" << std::endl;
    const QString slowPath = directory.filePath(QStringLiteral("slow-presentation.txt"));
    const QModelIndex slowIndex = fs.index(slowPath);
    AsyncFilePresentationCache presentation(&fs);
    const int beforePresentation = presentationQueries;
    const int beforePresentationBeats = heartbeats;
    timing.restart();
    for (int i = 0; i < 100; ++i)
        presentation.value(slowPath, slowIndex, Qt::DecorationRole);
    require(timing.elapsed() < 50, "presentation data blocked on Shell/MIME IO");
    require(until([&]() { return presentation.value(slowPath, slowIndex, Qt::DecorationRole).isValid(); }),
        "native file icon did not arrive");
    require(heartbeats > beforePresentationBeats + 20, "slow presentation stalled UI");
    require(presentationQueries == beforePresentation + 1, "presentation queries were not deduplicated");
    const QMimeType expectedType = QMimeDatabase().mimeTypeForFile(slowPath);
    require(presentation.value(slowPath, slowIndex, Qt::DisplayRole).toString() == expectedType.comment(),
        "detailed MIME type was not preserved");
    const QString failedPath = directory.filePath(QStringLiteral("failed-presentation.txt"));
    const QModelIndex failedIndex = fs.index(failedPath);
    presentation.value(failedPath, failedIndex, Qt::DecorationRole);
    require(until([&]() { return presentationQueries > beforePresentation + 1; }), "failed query never started");
    QThreadPool::globalInstance()->waitForDone(); QCoreApplication::processEvents();
    const int afterFailure = presentationQueries;
    for (int i = 0; i < 100; ++i)
        presentation.value(failedPath, failedIndex, Qt::DecorationRole);
    QCoreApplication::processEvents();
    require(presentationQueries == afterFailure, "failed presentation was not cached");
    presentation.reset();
    presentation.value(slowPath, slowIndex, Qt::DecorationRole);
    require(until([&]() { return presentationQueries > afterFailure; }), "stale presentation never started");
    presentation.reset();
    QThreadPool::globalInstance()->waitForDone(); QCoreApplication::processEvents();
    require(!presentation.value(slowPath, slowIndex, Qt::DecorationRole).isValid(),
        "stale presentation overwrote reset cache");
    require(until([&]() { return presentation.value(slowPath, slowIndex, Qt::DecorationRole).isValid(); }),
        "reset presentation never retried");
    auto closedPresentation = std::make_unique<AsyncFilePresentationCache>(&fs);
    const int beforePresentationClose = presentationQueries;
    closedPresentation->value(slowPath, slowIndex, Qt::DecorationRole);
    require(until([&]() { return presentationQueries > beforePresentationClose; }), "close presentation never started");
    closedPresentation.reset();
    QThreadPool::globalInstance()->waitForDone(); QCoreApplication::processEvents();
    require(presentationOnUi == 0, "Shell/MIME IO ran on the UI thread");
    checkRefreshScroll();
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
            'Qt6Core.lib Qt6Gui.lib Qt6Widgets.lib Shell32.lib Ole32.lib User32.lib\nif errorlevel 1 exit /b %errorlevel%\n'
            f'"{qt / "bin/windeployqt.exe"}" --no-translations --no-system-d3d-compiler '
            f'--no-opengl-sw --compiler-runtime "{exe}"\nexit /b %errorlevel%\n', encoding='utf-8')
        compilation = subprocess.run(['cmd.exe', '/d', '/c', str(build)], cwd=root,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if compilation.returncode:
            print(compilation.stdout)
        compilation.check_returncode()
        shutil.copy2(qt / 'plugins/platforms/qoffscreen.dll', out / 'platforms/qoffscreen.dll')
        for dll in (root / 'Ksword5.1/x64/Release').glob('*140*.dll'):
            shutil.copy2(dll, out / dll.name)
        environment = dict(os.environ, QT_QPA_PLATFORM='offscreen')
        environment['PATH'] = os.pathsep.join([
            str(qt / 'bin'), str(root / 'Ksword5.1/x64/Release'), environment['PATH']])
        previous = ctypes.windll.kernel32.SetErrorMode(3)
        try:
            subprocess.run([str(exe)] + (['--system32'] if args.system32 else []),
                           check=True, env=environment, timeout=90 if args.system32 else 30)
        finally:
            ctypes.windll.kernel32.SetErrorMode(previous)


if __name__ == '__main__':
    main()
