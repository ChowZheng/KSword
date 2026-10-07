#!/usr/bin/env python3
"""Run the production RegistryDock thread/stop code against a blocking backend shim.

No registry key, .reg import/export, Windows process, or real GUI is touched.
"""
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
SOURCE_ROOT = ROOT / "Ksword5.1/Ksword5.1"


def balanced_end(text: str, start: int, opening: str, closing: str) -> int:
    depth, state, index = 0, "code", start
    while index < len(text):
        char, pair = text[index], text[index:index + 2]
        if state == "line":
            if char == "\n": state = "code"
        elif state == "block":
            if pair == "*/": state = "code"; index += 1
        elif state in ('"', "'"):
            if char == "\\": index += 1
            elif char == state: state = "code"
        elif pair == "//": state = "line"; index += 1
        elif pair == "/*": state = "block"; index += 1
        elif char in ('"', "'"): state = char
        elif char == opening: depth += 1
        elif char == closing:
            depth -= 1
            if depth == 0: return index + 1
        index += 1
    raise ValueError("Unbalanced production block")


def method(source: str, signature: str) -> str:
    begin = source.index(signature)
    opening = source.index("{", begin)
    return source[begin:balanced_end(source, opening, "{", "}")]


def main() -> None:
    source = (SOURCE_ROOT / "RegistryDock/RegistryDock.cpp").read_text(encoding="utf-8-sig")
    start = method(source, "void RegistryDock::startSearchAsync()")
    prefix = start[start.index("{") + 1:start.index("    const QString keyword")]
    worker_begin = start.index("    m_searchThread = std::make_unique<std::thread>(")
    worker_paren = start.index("(", worker_begin)
    worker = start[worker_begin:balanced_end(start, worker_paren, "(", ")") + 1]
    stop = method(source, "void RegistryDock::stopSearch(bool waitForThread)")
    destructor = method(source, "RegistryDock::~RegistryDock()")
    assert "std::move(m_searchThread)" not in stop and ".detach()" not in stop
    assert "m_searchThread->join();" in prefix
    assert re.search(r"\{\s*m_uiDispatcher->close\(\);", destructor)
    for name in ("exportCurrentKeyAsync", "importRegFileAsync"):
        body = method(source, "void RegistryDock::" + name + "()")
        assert "QMetaObject::invokeMethod" not in body and "qApp" not in body
        assert "dispatcher->post" in body and 'kPro.set(progressPid, "界面已关闭", 0, 100.0f)' in body
    dispatcher = (SOURCE_ROOT / "UI/AsyncUiDispatcher.h").read_text(encoding="utf-8-sig")
    dispatcher = re.sub(r'^#(?:pragma once|include <QtCore/[^>]+>)\n', '', dispatcher, flags=re.MULTILINE)
    harness = r'''
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <functional>
std::atomic_int checks{0};
void check(bool value, const char* reason) {
    if (!value) throw std::runtime_error(reason);
    ++checks;
}
struct QString {
    std::string value;
    QString(const char* text = "") : value(text) {}
    QString arg(std::size_t) const { return *this; }
};
#define QStringLiteral(value) QString(value)
struct kLogEvent {};
struct Log { template<class T> Log& operator<<(const T&) { return *this; } } info, dbg;
constexpr int eol = 0;
struct Progress {
    std::atomic_int completed{0};
    void set(int, const char*, int, float value) { if (value >= 100) ++completed; }
} kPro;
struct QObject {
    std::shared_ptr<int> life = std::make_shared<int>(1);
    virtual ~QObject() = default;
};
template<class T> struct QPointer {
    T* value;
    std::weak_ptr<int> life;
    QPointer(T* object) : value(object), life(object->life) {}
    bool operator==(std::nullptr_t) const { return life.expired(); }
    bool operator!=(std::nullptr_t) const { return !life.expired(); }
    T* operator->() const { return value; }
};
namespace Qt { constexpr int QueuedConnection = 1; }
struct Queued { std::weak_ptr<int> receiver; std::function<void()> task; };
std::mutex queueMutex;
std::condition_variable queueCondition;
std::vector<Queued> queue;
struct QMetaObject {
    static bool invokeMethod(QObject* receiver, std::function<void()> task, int) {
        std::lock_guard<std::mutex> lock(queueMutex);
        queue.push_back({receiver->life, std::move(task)});
        queueCondition.notify_all();
        return true;
    }
};
void awaitQueued() {
    std::unique_lock<std::mutex> lock(queueMutex);
    check(queueCondition.wait_for(lock, std::chrono::seconds(5), [] { return !queue.empty(); }), "completion queue timeout");
}
void drain() {
    std::vector<Queued> pending;
    { std::lock_guard<std::mutex> lock(queueMutex); pending.swap(queue); }
    for (const auto& item : pending) if (!item.receiver.expired()) item.task();
}
__DISPATCHER__

struct Button { bool enabled = true; void setEnabled(bool value) { enabled = value; } };
struct Timer { void stop() {} };
struct SearchGate {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool released = false;
    void block() {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        condition.notify_all();
        check(condition.wait_for(lock, std::chrono::seconds(5), [this] { return released; }), "backend gate timeout");
    }
    void awaitEntered() {
        std::unique_lock<std::mutex> lock(mutex);
        check(condition.wait_for(lock, std::chrono::seconds(5), [this] { return entered; }), "backend did not enter");
    }
    void release() { std::lock_guard<std::mutex> lock(mutex); released = true; condition.notify_all(); }
};
using HKEY = void*;
struct RegistryDock : QObject {
    struct SearchOptions {};
    std::atomic_bool m_searchRunning{false};
    std::atomic_bool m_searchStopFlag{false};
    std::unique_ptr<std::thread> m_searchThread;
    std::shared_ptr<ks::ui::AsyncUiDispatcher> m_uiDispatcher = std::make_shared<ks::ui::AsyncUiDispatcher>(this);
    Button searchButton, stopButton;
    Timer timer;
    Button* m_searchButton = &searchButton;
    Button* m_stopSearchButton = &stopButton;
    Timer* m_searchFlushTimer = &timer;
    int m_progressPid = 17;
    int committed = 0;
    QString lastStatus;
    std::atomic_bool membersAlive{true};
    SearchGate* backendGate = nullptr;
    ~RegistryDock() override;
    void startSearchAsync();
    void stopSearch(bool waitForThread);
    void flushPendingSearchRows() { check(membersAlive.load(), "completion accessed destroyed page"); ++committed; }
    void updateStatusBar(const QString& text) { lastStatus = text; }
    void searchRegistryRecursive(HKEY, const QString&, const QString&, SearchOptions, std::size_t* scanned, std::size_t* hits) {
        if (backendGate != nullptr) backendGate->block();
        check(membersAlive.load(), "backend accessed page after destructor returned");
        *scanned = 64;
        *hits = 2;
    }
};
void RegistryDock::startSearchAsync() {
__START_PREFIX__
    HKEY root = nullptr;
    QString subPath("fixture"), keyword("query");
    SearchOptions options;
    m_searchRunning.store(true);
    m_searchStopFlag.store(false);
    QPointer<RegistryDock> guardThis(this);
    const auto dispatcher = m_uiDispatcher;
    const int progressPid = m_progressPid;
__WORKER__
}
__STOP__
__DESTRUCTOR__

int main() {
    // Natural completion leaves a joinable handle; a second normal search must
    // reclaim it before assigning another std::thread (old code terminated).
    auto page = std::make_unique<RegistryDock>();
    page->startSearchAsync();
    awaitQueued();
    drain();
    check(!page->m_searchRunning.load(), "first search did not finish");
    check(page->m_searchThread->joinable(), "fixture did not retain completed thread handle");
    page->startSearchAsync();
    awaitQueued();
    drain();
    check(page->committed == 2, "second search completion lost");
    check(kPro.completed == 2, "natural completion progress mismatch");
    page.reset();

    // Interactive cancellation must not move the only join handle away.
    // Destruction blocks until the actual backend member accesses have ended.
    SearchGate gate;
    auto cancelPage = std::make_unique<RegistryDock>();
    cancelPage->backendGate = &gate;
    cancelPage->startSearchAsync();
    gate.awaitEntered();
    cancelPage->stopSearch(false);
    check(cancelPage->m_searchThread != nullptr && cancelPage->m_searchThread->joinable(), "interactive stop lost destructor join handle");
    check(cancelPage->m_searchRunning.load(), "stop published idle while backend still owned page members");
    auto destroying = std::async(std::launch::async, [&] { cancelPage.reset(); });
    check(destroying.wait_for(std::chrono::milliseconds(25)) == std::future_status::timeout,
        "page destructor returned before recursive backend exited");
    gate.release();
    destroying.get();
    drain();
    check(kPro.completed == 3, "page close stranded search completion progress");

    // A normal cancellation keeps the existing stopped UI state/message.
    SearchGate stoppedGate;
    auto stoppedPage = std::make_unique<RegistryDock>();
    stoppedPage->backendGate = &stoppedGate;
    stoppedPage->startSearchAsync();
    stoppedGate.awaitEntered();
    stoppedPage->stopSearch(false);
    stoppedGate.release();
    awaitQueued();
    drain();
    check(!stoppedPage->m_searchRunning.load(), "canceled search never reached idle");
    check(stoppedPage->lastStatus.value == "状态: 搜索已停止", "cancellation status changed");
    check(kPro.completed == 4, "cancel progress did not finish once");
    stoppedPage.reset();
    std::cout << "REGISTRY_SEARCH_LIFETIME_CHECKS=" << checks.load() << "\n";
}
'''
    for marker, value in {"__DISPATCHER__": dispatcher, "__START_PREFIX__": prefix,
        "__WORKER__": worker, "__STOP__": stop, "__DESTRUCTOR__": destructor}.items():
        harness = harness.replace(marker, value)
    compiler = shutil.which("g++")
    if compiler is None: raise SystemExit("g++ unavailable")
    work = ROOT / ".codex-tmp/crash-audit/registry-search"
    work.mkdir(parents=True, exist_ok=True)
    cpp, executable = work / "registry_search.cpp", work / "registry_search.exe"
    cpp.write_text(harness, encoding="utf-8")
    subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread", "-static",
        str(cpp), "-o", str(executable)], check=True, timeout=60)
    result = subprocess.run([str(executable)], check=True, timeout=15, capture_output=True, text=True)
    print(result.stdout.strip())


if __name__ == "__main__": main()
