"""Run real bookmark add/render/async-refresh code with deterministic queues.

Production methods, bookmark fields and backend selection are extracted at run
time. Tiny Qt shims expose table cells and schedule worker/UI/menu callbacks
separately, allowing context changes at each asynchronous boundary. Memory
reads are scripted at the existing backend boundary; no UI, driver or disk is
required. Run: python tools/hvm_unit_tests/test_memory_bookmarks.py
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import uuid

from test_memory_backend import QT_SHIM as BASE_QT_SHIM
from test_memory_client import extract_struct


ROOT = Path(__file__).resolve().parents[2]
DIRECTORY = ROOT / "Ksword5.1/Ksword5.1/MemoryDock"
PRODUCTION = DIRECTORY / "MemoryDock.ViewBreakpointUtil.cpp"


def extract_method(source: str, name: str) -> str:
    masked = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"',
                    lambda match: " " * len(match.group()), source, flags=re.DOTALL)
    declaration = re.search(rf"[A-Za-z_]\w*(?:::\w+)*\s+{re.escape(name)}\s*\(", masked)
    if declaration is None:
        raise ValueError(f"Production method missing: {name}")
    cursor = masked.index("{", declaration.end()) + 1
    depth = 1
    while depth:
        depth += (masked[cursor] == "{") - (masked[cursor] == "}")
        cursor += 1
    return source[declaration.start():cursor]


def extract_button_body(source: str, button: str) -> str:
    marker = source.index(f"connect({button},")
    opening = source.index("[this]() {", marker) + len("[this]() ")
    cursor = opening + 1
    depth = 1
    while depth:
        depth += (source[cursor] == "{") - (source[cursor] == "}")
        cursor += 1
    return source[opening + 1:cursor - 1]


QT_SHIM = BASE_QT_SHIM.replace(
    "#include <vector>", "#include <vector>\n#include <sstream>\n#include <type_traits>",
).replace(
    "template<class... Args> QString arg(const Args&...) const { return *this; }",
    r'''template<class First, class... Rest> QString arg(const First& first, const Rest&...) const {
        QString result = *this;
        std::ostringstream formatted;
        if constexpr (std::is_same_v<std::decay_t<First>, QString>) formatted << first.value;
        else formatted << first;
        for (int number = 1; number < 100; ++number) {
            const auto token = "%" + std::to_string(number);
            const auto offset = result.value.find(token);
            if (offset != std::string::npos) {
                result.value.replace(offset, token.size(), formatted.str()); break;
            }
        }
        return result;
    }''',
).replace(
    "void clear() { value.clear(); }",
    "std::string toStdString() const { return value; }\n"
    "    bool operator==(const QString& other) const { return value == other.value; }\n"
    "    bool operator!=(const QString& other) const { return value != other.value; }\n"
    "    void clear() { value.clear(); }",
).replace(
    "const char* constData() const { return value.data(); }",
    "const char* constData() const { return value.data(); }\n"
    "    char* data() { return value.data(); }\n"
    "    void clear() { value.clear(); }\n"
    "    QByteArray left(qsizetype length) const { return QByteArray(constData(), length); }",
)

PRELUDE = r'''
#include "MemoryAccessBackend.h"
#include <array>
#include <atomic>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <utility>
using HANDLE = void*;
using qulonglong = unsigned long long;
static std::deque<std::function<void()>> workers, ui;
static std::map<std::string, std::function<void()>> menuCommits;
static bool menuOpen;
static int testTime = 1;
struct Alert { std::string title, message; bool ownerPresent; };
static std::vector<Alert> alerts;
static int qAppStorage;
static int* qApp = &qAppStorage;
namespace Qt {
    constexpr int QueuedConnection = 1;
    constexpr int UserRole = 256;
    constexpr int AscendingOrder = 0;
    constexpr int DescendingOrder = 1;
}
class QMessageBox {
public:
    template<class Owner> static void critical(Owner* owner, const QString& title, const QString& message) {
        alerts.push_back({title.toStdString(), message.toStdString(), owner != nullptr});
    }
};
class QThreadPool {
public:
    static QThreadPool* globalInstance() { static QThreadPool pool; return &pool; }
    template<class Fn> void start(Fn fn) { workers.emplace_back(std::move(fn)); }
};
class QMetaObject {
public:
    template<class Receiver, class Fn> static void invokeMethod(Receiver, Fn fn, int) {
        ui.emplace_back(std::move(fn));
    }
};
template<class T> class QPointer {
    T* pointer;
    std::weak_ptr<int> lifetime;
public:
    QPointer(T* value) : pointer(value), lifetime(value->lifetime) {}
    bool isNull() const { return lifetime.expired(); }
    T* data() const { return isNull() ? nullptr : pointer; }
    T* operator->() const { return data(); }
};
class QDateTime {
    int timestamp;
public:
    explicit QDateTime(int value) : timestamp(value) {}
    static QDateTime currentDateTime() { return QDateTime(testTime); }
    QString toString(const char*) const { return QString::fromStdString(std::to_string(timestamp)); }
};
class QComboBox {
    int selected = 0;
public:
    int currentIndex() const { return selected; }
    void setCurrentIndex(int value) { selected = value; }
};
class QVariant {
    qulonglong value = 0;
    bool valid = false;
public:
    QVariant() = default;
    QVariant(qulonglong input) : value(input), valid(true) {}
    qulonglong toULongLong(bool* converted = nullptr) const {
        if (converted != nullptr) *converted = valid;
        return value;
    }
};
class QTableWidgetItem {
    QVariant bookmarkId;
public:
    QString value, tooltip;
    explicit QTableWidgetItem(const QString& text) : value(text) {}
    void setToolTip(const QString& text) { tooltip = text; }
    void setData(int, QVariant value) { bookmarkId = value; }
    QVariant data(int) const { return bookmarkId; }
};
class QTableWidget {
    std::vector<std::array<std::unique_ptr<QTableWidgetItem>, 4>> rows;
    bool sorting = false;
    int sortColumn = 0, sortOrder = Qt::AscendingOrder;
    void applySort() {
        if (!sorting) return;
        const auto* previous = item(selected, 0);
        const qulonglong previousId = previous == nullptr ? 0 : previous->data(Qt::UserRole).toULongLong();
        std::sort(rows.begin(), rows.end(), [this](const auto& left, const auto& right) {
            const auto column = static_cast<std::size_t>(sortColumn);
            const auto first = left[column] ? left[column]->value.toStdString() : std::string();
            const auto second = right[column] ? right[column]->value.toStdString() : std::string();
            return sortOrder == Qt::AscendingOrder ? first < second : first > second;
        });
        for (std::size_t row = 0; row < rows.size(); ++row)
            if (rows[row][0] && rows[row][0]->data(Qt::UserRole).toULongLong() == previousId)
                selected = static_cast<int>(row);
    }
public:
    int selected = 0;
    unsigned mutationsWhileSorting = 0;
    int currentRow() const { return selected; }
    int rowCount() const { return static_cast<int>(rows.size()); }
    QTableWidgetItem* item(int row, int column) const {
        if (row < 0 || row >= rowCount() || column < 0 || column >= 4) return nullptr;
        return rows[static_cast<std::size_t>(row)][static_cast<std::size_t>(column)].get();
    }
    bool isSortingEnabled() const { return sorting; }
    void setSortingEnabled(bool enabled) { sorting = enabled; applySort(); }
    void sortItems(int column, int order) { sortColumn = column; sortOrder = order; sorting = true; applySort(); }
    void setCurrentCell(int row, int) { selected = row; }
    void setRowCount(int count) { rows.resize(static_cast<std::size_t>(count)); }
    void setItem(int row, int column, QTableWidgetItem* value) {
        rows.at(static_cast<std::size_t>(row)).at(static_cast<std::size_t>(column)).reset(value);
        if (sorting) ++mutationsWhileSorting;
        applySort();
    }
    std::string currentValue(int row = 0) const {
        return rows.at(static_cast<std::size_t>(row))[1]->value.toStdString();
    }
};
namespace ks::i18n { static QString sourceText(const QString& text) { return text; } }
namespace ks::ui {
template<class Owner, class Fn> bool DeferTableUiCommitIfContextMenuOpen(
    Owner*, const QString& key, std::initializer_list<QTableWidget*>, Fn fn) {
    if (!menuOpen) return false;
    menuCommits[key.toStdString()] = std::move(fn);
    return true;
}
}
struct kLogEvent {};
struct NullLog { template<class T> NullLog& operator<<(const T&) { return *this; } };
static NullLog info, dbg;
static constexpr char eol = '\n';
enum class SearchValueType { ByteArray };
static QString formatAddress(std::uint64_t address) { return QString::fromStdString(std::to_string(address)); }
static QString bytesToDisplayString(const QByteArray& bytes, SearchValueType) {
    return bytes.isEmpty() ? QString() : QString::fromStdString(std::string(bytes.constData(), static_cast<std::size_t>(bytes.size())));
}
namespace ksword::memory_backend {
struct ReadCall { MemoryAccessBackend backend; DdmaSession session; std::uint32_t pid; std::uint64_t address; std::uint64_t length; };
static std::vector<ReadCall> reads;
static std::deque<AccessOutcome> scriptedReads;
static std::uint64_t sessionGeneration;
static DdmaSession session;
const DdmaSession& currentDdmaSession() { return session; }
std::uint64_t ddmaSessionGeneration() { return sessionGeneration; }
QString backendDisplayName(MemoryAccessBackend backend) { return QString::fromStdString(std::to_string(static_cast<int>(backend))); }
AccessOutcome readVirtual(MemoryAccessBackend backend, const DdmaSession& saved,
    std::uint32_t pid, std::uint64_t address, std::uint64_t length) {
    reads.push_back({backend, saved, pid, address, length});
    if (!scriptedReads.empty()) {
        auto result = scriptedReads.front(); scriptedReads.pop_front(); return result;
    }
    AccessOutcome result; result.ok = true; result.bytesDone = length;
    result.data = QByteArray(static_cast<qsizetype>(length), static_cast<char>('A' + address % 20));
    return result;
}
}
class MemoryDock {
public:
    std::shared_ptr<int> lifetime = std::make_shared<int>(1);
    QTableWidget tableStorage;
    QComboBox comboStorage;
    QTableWidget* m_bookmarkTable = &tableStorage;
    QComboBox* m_bookmarkBackendCombo = &comboStorage;
    std::uint64_t lastJumpAddress = 0;
    void jumpToAddress(std::uint64_t address) { lastJumpAddress = address; }
'''

DECLARATIONS = r'''
    void addBookmarkByAddress(std::uint64_t, const QString&);
    void rebuildBookmarkTable();
    void refreshBookmarkValues();
    ksword::memory_backend::MemoryAccessBackend currentBookmarkBackend() const;
    void testRemoveSelectedBookmark();
    void testJumpSelectedBookmark();
};
'''

TESTS = r'''
using namespace ksword::memory_backend;
using State = MemoryDock::BookmarkValueState;
static unsigned checks, failures;
static void Check(bool condition, const char* label) {
    ++checks; if (!condition) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}
static void RunWorker() {
    if (workers.empty()) throw std::runtime_error("Expected a pending worker");
    auto task = std::move(workers.front()); workers.pop_front(); task();
}
static void RunUi() {
    if (ui.empty()) throw std::runtime_error("Expected a pending UI commit");
    auto task = std::move(ui.front()); ui.pop_front(); task();
}
static void Drain() {
    for (int iteration = 0; iteration < 100; ++iteration) {
        if (!workers.empty()) RunWorker();
        else if (!ui.empty()) RunUi();
        else return;
    }
    throw std::runtime_error("Refresh did not settle within 100 scheduled tasks");
}
static void CloseMenu() {
    menuOpen = false;
    auto commits = std::move(menuCommits); menuCommits.clear();
    for (auto& entry : commits) entry.second();
}
static void Reset() {
    if (!workers.empty() || !ui.empty() || !menuCommits.empty())
        throw std::runtime_error("Previous test left scheduled work");
    reads.clear(); scriptedReads.clear(); session = {}; sessionGeneration = 0; menuOpen = false; testTime = 1; alerts.clear();
}
static void Attach(MemoryDock& dock, MemoryAccessBackend backend = MemoryAccessBackend::Hvm) {
    dock.m_attachedPid = 77;
    dock.m_processAttachmentGeneration = 1;
    dock.comboStorage.setCurrentIndex(static_cast<int>(backend));
}
static AccessOutcome Outcome(const char* value, bool ok = true, bool partial = false) {
    AccessOutcome result; result.ok = ok; result.partial = partial;
    result.data = QByteArray(value, static_cast<qsizetype>(std::char_traits<char>::length(value)));
    result.bytesDone = static_cast<std::uint64_t>(result.data.size());
    return result;
}
static void TestSelectedBackendsAndFailures() {
    for (auto backend : {MemoryAccessBackend::UserMode, MemoryAccessBackend::StandardDriver,
                         MemoryAccessBackend::Hvm, MemoryAccessBackend::Ddma}) {
        Reset(); MemoryDock dock; Attach(dock, backend);
        session.diskIndex = 9; session.scratchLba = 123;
        scriptedReads.push_back(Outcome("CURRENT1"));
        dock.addBookmarkByAddress(0x1000, QStringLiteral("first"));
        session.diskIndex = 10; session.scratchLba = 456;
        Check(reads.empty() && workers.size() == 1, "adding bookmark performs no synchronous memory read");
        Drain();
        Check(reads.size() == 1 && reads[0].backend == backend && reads[0].pid == 77 && reads[0].length == 8,
              "initial read uses selected backend and exact PID/length without an R3 handle");
        Check(reads[0].session.diskIndex == 9 && reads[0].session.scratchLba == 123,
              "worker receives copied DDMA session");
        Check(dock.m_bookmarkCache[0].valueState == State::Ready && dock.tableStorage.currentValue() == "CURRENT1",
              "full read publishes current value");
        const auto id = dock.m_bookmarkCache[0].id;
        dock.addBookmarkByAddress(0x1000, QStringLiteral("new note"));
        Check(dock.m_bookmarkCache.size() == 1 && dock.m_bookmarkCache[0].id == id &&
              dock.m_bookmarkCache[0].noteText.toStdString() == "new note",
              "adding duplicate updates note without replacing stable bookmark identity");
        AccessOutcome failure; failure.failureText = QStringLiteral("read failed");
        scriptedReads.push_back(failure); dock.refreshBookmarkValues(); Drain();
        Check(dock.m_bookmarkCache[0].valueState == State::Failed &&
              dock.m_bookmarkCache[0].lastValueBytes.isEmpty() && dock.tableStorage.currentValue() != "CURRENT1",
              "failed read clears cached current bytes and visible old value");
        scriptedReads.push_back(Outcome("SHRT")); dock.refreshBookmarkValues(); Drain();
        Check(dock.m_bookmarkCache[0].valueState == State::Partial && dock.m_bookmarkCache[0].lastValueBytes.isEmpty(),
              "short read never publishes a current full value");
        scriptedReads.push_back(Outcome("EIGHTBYT", true, true)); dock.refreshBookmarkValues(); Drain();
        Check(dock.m_bookmarkCache[0].valueState == State::Partial && dock.m_bookmarkCache[0].lastValueBytes.isEmpty(),
              "partial status rejects even eight returned bytes");
        auto malformed = Outcome("EIGHTBYT"); malformed.bytesDone = 4;
        scriptedReads.push_back(malformed); dock.refreshBookmarkValues(); Drain();
        Check(dock.m_bookmarkCache[0].valueState == State::Partial && dock.m_bookmarkCache[0].lastValueBytes.isEmpty(),
              "byte accounting mismatch cannot become Ready");
        auto warning = Outcome("WARNBYTE"); warning.failureText = QStringLiteral("fallback warning"); warning.scratchDirty = true;
        scriptedReads.push_back(warning); dock.refreshBookmarkValues(); Drain();
        Check((backend == MemoryAccessBackend::Ddma
                   ? dock.m_bookmarkCache[0].valueState == State::Failed && alerts.size() == 1
                   : dock.m_bookmarkCache[0].valueState == State::Ready) && dock.m_bookmarkCache[0].scratchDirty &&
              dock.tableStorage.currentValue() != "WARNBYTE" && !dock.m_bookmarkCache[0].readDetail.isEmpty(),
              "usable read retains visible backend and dirty-scratch warnings");
        dock.m_attachedPid = 0; ++dock.m_processAttachmentGeneration;
        dock.refreshBookmarkValues();
        Check(dock.m_bookmarkCache[0].valueState == State::NoProcess && dock.m_bookmarkCache[0].lastValueBytes.isEmpty() && workers.empty(),
              "detaching clears current value without attempting reads");
    }
}
static void TestContextChangesAtCommit() {
    for (int change = 0; change < 5; ++change) {
        Reset(); MemoryDock dock; Attach(dock, change == 3 ? MemoryAccessBackend::Ddma : MemoryAccessBackend::Hvm);
        scriptedReads.push_back(Outcome("OLDVALUE")); dock.addBookmarkByAddress(0x1000, QStringLiteral("context"));
        RunWorker();
        if (change == 0) dock.m_attachedPid = 99;
        if (change == 1) ++dock.m_processAttachmentGeneration;
        if (change == 2) dock.comboStorage.setCurrentIndex(static_cast<int>(MemoryAccessBackend::StandardDriver));
        if (change == 3) { ++sessionGeneration; session.scratchLba = 999; }
        if (change == 4) {
            dock.comboStorage.setCurrentIndex(static_cast<int>(MemoryAccessBackend::StandardDriver));
            dock.refreshBookmarkValues();
            dock.comboStorage.setCurrentIndex(static_cast<int>(MemoryAccessBackend::Hvm));
            dock.refreshBookmarkValues();
        }
        RunUi();
        Check(dock.m_bookmarkCache[0].lastValueBytes.isEmpty() && dock.m_bookmarkCache[0].valueState != State::Ready,
              "PID/generation/backend/DDMA/ABA change rejects stale queued results");
        scriptedReads.push_back(Outcome("NEWVALUE")); Drain();
        Check(dock.m_bookmarkCache[0].valueState == State::Ready && dock.tableStorage.currentValue() == "NEWVALUE",
              "stale result schedules a fresh read in current context");
        Check(reads.size() == 2 && reads.back().pid == dock.m_attachedPid && reads.back().backend == dock.currentBookmarkBackend(),
              "replacement refresh uses current PID and backend");
    }
}
static void TestStableIdsAndPendingCoalescing() {
    Reset(); MemoryDock dock; Attach(dock);
    scriptedReads.push_back(Outcome("DELETED1")); dock.addBookmarkByAddress(0x1000, QStringLiteral("old"));
    const auto oldId = dock.m_bookmarkCache[0].id;
    dock.rebuildBookmarkTable();
    dock.testRemoveSelectedBookmark();
    dock.addBookmarkByAddress(0x1000, QStringLiteral("replacement"));
    Check(dock.m_bookmarkCache.size() == 1 && dock.m_bookmarkCache[0].id != oldId,
          "delete then re-add same address creates a new stable ID");
    RunWorker(); RunUi();
    Check(dock.m_bookmarkCache[0].lastValueBytes.isEmpty() && dock.m_bookmarkCache[0].valueState != State::Ready,
          "deleted bookmark result cannot populate same-address replacement");
    scriptedReads.push_back(Outcome("REPLACED")); Drain();
    Check(dock.tableStorage.currentValue() == "REPLACED" && reads.size() == 2,
          "replacement receives its own new sample");
    Reset(); MemoryDock coalesced; Attach(coalesced);
    coalesced.addBookmarkByAddress(0x2000, QStringLiteral("coalesced"));
    coalesced.refreshBookmarkValues(); coalesced.refreshBookmarkValues();
    Check(workers.size() == 1 && coalesced.m_bookmarkRefreshPending,
          "multiple refresh requests never queue parallel bookmark readers");
    Drain();
    Check(reads.size() == 2 && !coalesced.m_bookmarkRefreshPending && !coalesced.m_bookmarkRefreshInProgress,
          "in-flight refresh requests coalesce into one additional batch");
}
static void TestMenuDeferralAndLifetime() {
    Reset(); MemoryDock dock; Attach(dock);
    scriptedReads.push_back(Outcome("STALEMNU")); dock.addBookmarkByAddress(0x1000, QStringLiteral("menu"));
    RunWorker(); menuOpen = true; RunUi();
    Check(!menuCommits.empty() && dock.m_bookmarkCache[0].valueState != State::Ready,
          "menu defers committing read values");
    ++dock.m_processAttachmentGeneration; dock.refreshBookmarkValues();
    CloseMenu();
    Check(dock.m_bookmarkCache[0].lastValueBytes.isEmpty(),
          "deferred commit rechecks context when the menu actually closes");
    scriptedReads.push_back(Outcome("FRESHMNU")); Drain();
    Check(dock.tableStorage.currentValue() == "FRESHMNU", "fresh context fills table after menu closes");
    Reset();
    auto disposable = std::make_unique<MemoryDock>(); Attach(*disposable);
    disposable->addBookmarkByAddress(0x1000, QStringLiteral("destroyed"));
    disposable.reset(); Drain();
    Check(workers.empty() && ui.empty(), "destroyed dock safely rejects queued UI result");
}
static void TestReadTimestamp() {
    Reset(); MemoryDock dock; Attach(dock);
    testTime = 5; scriptedReads.push_back(Outcome("TIMESTMP"));
    dock.addBookmarkByAddress(0x1000, QStringLiteral("timestamp"));
    testTime = 10; RunWorker();
    testTime = 20; menuOpen = true; RunUi();
    testTime = 30; CloseMenu(); Drain();
    Check(dock.m_bookmarkCache[0].readTimeText.toStdString() == "10",
          "read timestamp records worker completion, not delayed UI/menu commit time");
    Check(dock.m_bookmarkCache[0].addTimeText.toStdString() == "5" &&
          dock.tableStorage.currentValue() == "TIMESTMP",
          "bookmark creation time and actual sample remain independent");
}
static void TestSortedTableIdentity() {
    Reset(); MemoryDock dock; Attach(dock);
    dock.addBookmarkByAddress(0x1000, QStringLiteral("first")); Drain();
    dock.addBookmarkByAddress(0x2000, QStringLiteral("second")); Drain();
    const auto firstId = dock.m_bookmarkCache[0].id;
    const auto secondId = dock.m_bookmarkCache[1].id;
    dock.tableStorage.sortItems(0, Qt::DescendingOrder);
    dock.tableStorage.setCurrentCell(0, 0);
    Check(dock.tableStorage.item(0, 0)->data(Qt::UserRole).toULongLong() == secondId,
          "sorted visible row carries its stable bookmark ID");
    dock.rebuildBookmarkTable();
    Check(dock.tableStorage.isSortingEnabled() && dock.tableStorage.mutationsWhileSorting == 0 &&
          dock.tableStorage.item(dock.tableStorage.currentRow(), 0)->data(Qt::UserRole).toULongLong() == secondId,
          "table rebuild disables sorting while filling and restores selection by ID");
    dock.testJumpSelectedBookmark();
    Check(dock.lastJumpAddress == 0x2000, "jump resolves sorted row ID instead of cache row index");
    dock.testRemoveSelectedBookmark();
    Check(dock.m_bookmarkCache.size() == 1 && dock.m_bookmarkCache[0].id == firstId &&
          dock.m_bookmarkCache[0].address == 0x1000,
          "delete resolves sorted row ID instead of deleting wrong cached bookmark");
}
static void TestDdmaFaultStopsAndRecovers() {
    Reset(); MemoryDock dock; Attach(dock, MemoryAccessBackend::Ddma);
    session.diskIndex = 42; session.scratchLba = 92837465;
    dock.addBookmarkByAddress(0x1000, QStringLiteral("first")); Drain();
    dock.addBookmarkByAddress(0x2000, QStringLiteral("second")); Drain();
    reads.clear();
    auto dirty = Outcome("DIRTYDIS"); dirty.scratchDirty = true;
    scriptedReads.push_back(dirty); scriptedReads.push_back(Outcome("DONTREAD"));
    dock.refreshBookmarkValues(); RunWorker();
    Check(reads.size() == 1 && scriptedReads.size() == 1,
          "dirty DDMA scratch stops later actual bookmark reads in same batch");
    RunUi();
    Check(alerts.size() == 1 && alerts[0].message.find("92837465") != std::string::npos &&
          alerts[0].message.find("42") != std::string::npos && dock.m_bookmarkDdmaReadBlocked,
          "disk restore fault reports exact snapshot disk/LBA and blocks the session");
    dock.refreshBookmarkValues(); dock.refreshBookmarkValues();
    Check(workers.empty() && reads.size() == 1 && alerts.size() == 1 &&
          dock.m_bookmarkCache[0].lastValueBytes.isEmpty() && dock.m_bookmarkCache[1].lastValueBytes.isEmpty(),
          "same faulty DDMA generation cannot keep reading or displaying current values");
    scriptedReads.clear(); scriptedReads.push_back(Outcome("FRESHONE")); scriptedReads.push_back(Outcome("FRESHTWO"));
    ++sessionGeneration; session.scratchLba = 19827364;
    dock.refreshBookmarkValues(); Drain();
    Check(reads.size() == 3 && alerts.size() == 1 &&
          dock.m_bookmarkCache[0].valueState == State::Ready && dock.m_bookmarkCache[1].valueState == State::Ready,
          "reconfigured DDMA generation resumes exact fresh reads");
}
static void TestDdmaFaultSurvivesStaleAndDeletion() {
    for (int action = 0; action < 2; ++action) {
        Reset(); MemoryDock dock; Attach(dock, MemoryAccessBackend::Ddma);
        session.diskIndex = 42; session.scratchLba = 92837465;
        auto dirty = Outcome("STALEDSK"); dirty.scratchDirty = true;
        scriptedReads.push_back(dirty); dock.addBookmarkByAddress(0x1000, QStringLiteral("fault"));
        dock.rebuildBookmarkTable(); RunWorker();
        session.diskIndex = 17; session.scratchLba = 19827364;
        if (action == 0) {
            dock.comboStorage.setCurrentIndex(static_cast<int>(MemoryAccessBackend::Hvm));
            dock.refreshBookmarkValues();
        } else dock.testRemoveSelectedBookmark();
        RunUi();
        Check(alerts.size() == 1 && alerts[0].message.find("92837465") != std::string::npos &&
              alerts[0].message.find("19827364") == std::string::npos,
              "stale backend result or deleted bookmark still reports original disk fault");
        if (action == 0) {
            Drain();
            Check(reads.size() == 2 && reads.back().backend == MemoryAccessBackend::Hvm,
                  "disk fault does not prevent switching to CPU memory backend");
        } else {
            dock.addBookmarkByAddress(0x1000, QStringLiteral("replacement"));
            Check(workers.empty() && reads.size() == 1 && dock.m_bookmarkCache[0].valueState == State::Failed,
                  "delete/re-add cannot bypass faulty DDMA generation block");
        }
    }
    Reset(); auto disposable = std::make_unique<MemoryDock>(); Attach(*disposable, MemoryAccessBackend::Ddma);
    auto dirty = Outcome("CLOSEDSK"); dirty.scratchDirty = true;
    scriptedReads.push_back(dirty); disposable->addBookmarkByAddress(0x1000, QStringLiteral("closing"));
    disposable.reset(); Drain();
    Check(alerts.size() == 1 && !alerts[0].ownerPresent,
          "closing the dock cannot erase an actual disk restore fault");
}
int main() {
    TestSelectedBackendsAndFailures(); TestContextChangesAtCommit();
    TestStableIdsAndPendingCoalescing(); TestMenuDeferralAndLifetime(); TestReadTimestamp();
    TestSortedTableIdentity(); TestDdmaFaultStopsAndRecovers(); TestDdmaFaultSurvivesStaleAndDeletion();
    std::cout << "Production bookmark refresh: " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
'''


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default=os.environ.get("CXX"))
    parser.add_argument("--source", type=Path, default=PRODUCTION)
    parser.add_argument("--work-dir", type=Path, default=ROOT / "work/hvm-memory-tests")
    args = parser.parse_args()
    compiler = args.compiler or shutil.which("g++") or shutil.which("clang++")
    if not compiler:
        parser.error("G++ or Clang++ is required")
    header = (DIRECTORY / "MemoryDock.h").read_text(encoding="utf-8-sig")
    source = args.source.read_text(encoding="utf-8-sig")
    build = (DIRECTORY / "MemoryDock.UiBuild.cpp").read_text(encoding="utf-8-sig")
    wiring = (DIRECTORY / "MemoryDock.UiWireAndStatus.cpp").read_text(encoding="utf-8-sig")
    state = re.search(r"enum class BookmarkValueState\s*\{[^}]*\}\s*;", header)
    if state is None:
        raise ValueError("Production bookmark states are missing")
    members = []
    for declaration in re.finditer(r"^[ \t]*[^\n;{}]*\bm_(?:bookmark(?:Cache|RefreshInProgress|RefreshPending|Context\w*|Ddma\w*)|nextBookmarkId|attachedPid|attachedProcessHandle|processAttachmentGeneration)\b[^;]*;", header, re.MULTILINE):
        members.append(declaration.group())
    models = state.group() + extract_struct(header, "BookmarkEntry") + "\n".join(members)
    methods = "\n".join(extract_method(source, f"MemoryDock::{name}") for name in (
        "addBookmarkByAddress", "rebuildBookmarkTable", "refreshBookmarkValues"))
    selection = "\n".join(extract_method(build, name) for name in (
        "backendFromComboIndex", "MemoryDock::currentBookmarkBackend"))
    handlers = "\n".join(
        f"void MemoryDock::{method}() {{\n" + extract_button_body(wiring, button) + "\n}\n"
        for method, button in (("testRemoveSelectedBookmark", "m_removeBookmarkButton"),
                               ("testJumpSelectedBookmark", "m_jumpBookmarkButton")))
    args.work_dir.mkdir(parents=True, exist_ok=True)
    work_dir = args.work_dir.resolve()
    scratch = work_dir / f"bookmarks-{uuid.uuid4().hex}"
    scratch.mkdir()
    try:
        (scratch / "qt_shim.h").write_text(QT_SHIM, encoding="utf-8")
        for name in ("QByteArray", "QString"):
            (scratch / name).write_text('#include "qt_shim.h"\n', encoding="utf-8")
        cpp = scratch / "production_bookmarks.cpp"
        binary = scratch / ("memory-bookmarks.exe" if os.name == "nt" else "memory-bookmarks")
        cpp.write_text(PRELUDE + models + DECLARATIONS + selection + methods + handlers + TESTS, encoding="utf-8")
        subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
                        "-I", str(scratch), "-I", str(DIRECTORY), str(cpp), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    finally:
        if scratch.resolve().parent != work_dir:
            raise RuntimeError("Refusing to clean outside the test work directory")
        shutil.rmtree(scratch)


if __name__ == "__main__":
    main()
