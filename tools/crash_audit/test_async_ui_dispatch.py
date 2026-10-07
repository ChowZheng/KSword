#!/usr/bin/env python3
"""Exercise the production UI dispatcher and check its FileDock/ETW integration.

The Qt shim models queued cancellation and receiver destruction. No file action,
ETW session, driver request, or real GUI is run by this regression.
"""

from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
SOURCE_ROOT = ROOT / "Ksword5.1/Ksword5.1"


def function_body(source: str, name: str) -> str:
    start = source.index("void " + name + "(")
    found = re.search(r"\n(?:void|bool|QString|ks::file::\w+) \w+::", source[start + 5:])
    end = start + 5 + found.start() if found else len(source)
    return source[start:end]


def integration_checks() -> int:
    checks = 0
    file_source = (SOURCE_ROOT / "FileDock/FileDock.cpp").read_text(encoding="utf-8-sig")
    irp_source = (SOURCE_ROOT / "FileDock/FileDock.IrpBuilder.cpp").read_text(encoding="utf-8-sig")
    runtime = (SOURCE_ROOT / "MonitorDock/ProcessTraceMonitorWidget.Runtime.cpp").read_text(encoding="utf-8-sig")
    actions = (SOURCE_ROOT / "MonitorDock/ProcessTraceMonitorWidget.Actions.cpp").read_text(encoding="utf-8-sig")
    for source, name in [
        (file_source, "FileDock::requestAsyncManualReload"),
        (file_source, "FileDock::scanDeletedFilesForRecoveryAsync"),
        (file_source, "FileDock::recoverSelectedDeletedFilesAsync"),
        (file_source, "FileDock::takeOwnershipSelectedItems"),
        (irp_source, "FileDock::submitConstructedIrp"),
        (actions, "ProcessTraceMonitorWidget::refreshAvailableProcessListAsync"),
        (runtime, "ProcessTraceMonitorWidget::refreshTrackedProcessSnapshotAsync"),
    ]:
        body = function_body(source, name)
        worker = body[body.index("std::thread("):]
        prefix = worker[:worker.index("dispatcher->post(")]
        assert "dispatcher" in worker.split("]", 1)[0], name
        assert not re.search(r"(?:safeThis|guardThis)\s*(?:->|\.isNull\(|\.data\()", prefix), name
        assert "QMetaObject::invokeMethod" not in worker and "qApp" not in worker, name
        checks += 3
        if name.startswith("FileDock::"):
            assert re.search(r'\[progressPid\]\(\)\s*\{\s*kPro.set\(progressPid, "界面已关闭", 0, 100.0f\);', worker), name
            checks += 1
            assert 'if (!invokeOk)' not in worker, name
            checks += 1

    for name in ("transferSelectedItemsToOppositePanel", "deleteSelectedItemsWithMode"):
        body = function_body(file_source, "FileDock::" + name)
        assert "applicationGuard" not in body and "QMetaObject::invokeMethod" not in body, name
        assert "dispatcher->post(" in body, name
        assert re.search(r'\[progressPid\]\(\)\s*\{\s*kPro.set\(progressPid, "界面已关闭", 0, 100.0f\);', body), name
        checks += 3

    for path, class_name in [("FileDock/FileDock.cpp", "FileDock"),
        ("MonitorDock/ProcessTraceMonitorWidget.cpp", "ProcessTraceMonitorWidget")]:
        source = (SOURCE_ROOT / path).read_text(encoding="utf-8-sig")
        destructor = source[source.index(class_name + "::~" + class_name + "()") :]
        assert re.search(r"\{\s*m_uiDispatcher->close\(\);", destructor), class_name
        checks += 1
    assert "stopMonitoringInternal(true);" in (SOURCE_ROOT / "MonitorDock/ProcessTraceMonitorWidget.cpp").read_text(encoding="utf-8-sig")
    assert "m_captureThread->join();" in function_body(runtime, "ProcessTraceMonitorWidget::stopMonitoringInternal")
    checks += 2
    for path in ("Ksword5.1.vcxproj", "Ksword5.1.vcxproj.filters"):
        assert 'ClInclude Include="UI\\AsyncUiDispatcher.h"' in (SOURCE_ROOT / path).read_text(encoding="utf-8-sig")
        checks += 1
    return checks


def main() -> None:
    static_checks = integration_checks()
    compiler = shutil.which("g++")
    if compiler is None:
        raise SystemExit("g++ unavailable")
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
#include <thread>
#include <vector>
#include <functional>
const auto uiThread = std::this_thread::get_id();
std::atomic_int checks{0};
void check(bool value, const char* reason) {
    if (!value) throw std::runtime_error(reason);
    ++checks;
}
struct Gate {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool released = false;
    void block() {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        condition.notify_all();
        check(condition.wait_for(lock, std::chrono::seconds(5), [this] { return released; }), "post gate timed out");
    }
    void awaitEntered() {
        std::unique_lock<std::mutex> lock(mutex);
        check(condition.wait_for(lock, std::chrono::seconds(5), [this] { return entered; }), "entry gate timed out");
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        condition.notify_all();
    }
};
struct QObject {
    std::shared_ptr<int> life = std::make_shared<int>(1);
    virtual ~QObject() = default;
};
namespace Qt { constexpr int QueuedConnection = 1; }
struct QueuedTask { std::weak_ptr<int> receiver; std::function<void()> task; };
std::mutex queueMutex;
std::vector<QueuedTask> queue;
bool applicationAlive = true;
bool invokeSucceeds = true;
Gate* postGate = nullptr;
struct QMetaObject {
    static bool invokeMethod(QObject* receiver, std::function<void()> task, int) {
        check(applicationAlive, "worker borrowed application after shutdown");
        if (!invokeSucceeds) return false;
        if (postGate != nullptr) postGate->block();
        std::lock_guard<std::mutex> lock(queueMutex);
        queue.push_back({receiver->life, std::move(task)});
        return true;
    }
};
void drain() {
    check(std::this_thread::get_id() == uiThread, "UI queue drained off thread");
    std::vector<QueuedTask> pending;
    { std::lock_guard<std::mutex> lock(queueMutex); pending.swap(queue); }
    for (const auto& task : pending) if (!task.receiver.expired()) task.task();
}

__DISPATCHER__

std::promise<void>* destructorEntered = nullptr;
struct Widget : QObject {
    std::shared_ptr<ks::ui::AsyncUiDispatcher> dispatcher = std::make_shared<ks::ui::AsyncUiDispatcher>(this);
    ~Widget() override {
        if (destructorEntered != nullptr) destructorEntered->set_value();
        dispatcher->close();
    }
};

int main() {
    int delivered = 0;
    auto widget = std::make_unique<Widget>();
    auto dispatcher = widget->dispatcher;
    std::vector<int> rows{3, 5, 7};
    std::thread worker([&] {
        check(dispatcher->post([&, rows] {
            check(std::this_thread::get_id() == uiThread, "snapshot applied off UI thread");
            check(rows.size() == 3 && rows[2] == 7, "snapshot payload changed");
            ++delivered;
        }), "live snapshot was rejected");
    });
    worker.join();
    check(delivered == 0, "snapshot delivered synchronously");
    drain();
    check(delivered == 1, "live snapshot was lost");

    check(dispatcher->post([&] { ++delivered; }), "queued completion rejected");
    widget.reset();
    drain();
    check(delivered == 1, "queued task ran after widget destruction");
    applicationAlive = false;
    std::thread lateWorker([&] {
        check(!dispatcher->post([&] { ++delivered; }), "late completion accepted after application shutdown");
    });
    lateWorker.join();
    check(delivered == 1, "late task modified UI state");

    // Finite progress completes both when posting is rejected and when Qt
    // discards a completion already queued before the receiver was destroyed.
    applicationAlive = true;
    int progressCompleted = 0;
    auto progressWidget = std::make_unique<Widget>();
    auto progressDispatcher = progressWidget->dispatcher;
    check(progressDispatcher->post([&] { ++progressCompleted; }, [&] { ++progressCompleted; }),
        "live progress completion rejected");
    drain();
    check(progressCompleted == 1, "successful callback also ran cancellation cleanup");
    check(progressDispatcher->post([] {}, [&] { ++progressCompleted; }), "queued progress completion rejected");
    progressWidget.reset();
    drain();
    check(progressCompleted == 2, "destroyed receiver stranded an accepted progress task");
    check(!progressDispatcher->post([] {}, [&] { ++progressCompleted; }), "closed progress dispatcher accepted task");
    check(progressCompleted == 3, "rejected post stranded a finite progress task");

    auto closedWidget = std::make_unique<Widget>();
    auto closedDispatcher = closedWidget->dispatcher;
    int canceledOnce = 0;
    check(closedDispatcher->post([&] { ++delivered; }, [&] { ++canceledOnce; closedDispatcher->close(); }),
        "pre-close task was not accepted");
    closedDispatcher->close();
    drain();
    check(delivered == 1, "close allowed queued business callback while receiver was still alive");
    check(canceledOnce == 1, "closed queue cancellation did not happen once");
    closedWidget.reset();
    check(canceledOnce == 1, "receiver destruction duplicated canceled cleanup");
    check(!closedDispatcher->post([] {}, [&] { ++canceledOnce; closedDispatcher->close(); }),
        "post after close was accepted");
    check(canceledOnce == 2, "rejected post did not cancel once outside dispatcher lock");

    auto failedWidget = std::make_unique<Widget>();
    auto failedDispatcher = failedWidget->dispatcher;
    invokeSucceeds = false;
    check(!failedDispatcher->post([] {}, [&] { ++canceledOnce; failedDispatcher->close(); }),
        "Qt invoke failure reported success");
    check(canceledOnce == 3, "Qt invoke failure did not cancel once outside dispatcher lock");
    invokeSucceeds = true;

    // A post already holding the receiver lock finishes before close returns.
    applicationAlive = true;
    auto racingWidget = std::make_unique<Widget>();
    auto racingDispatcher = racingWidget->dispatcher;
    Gate gate;
    postGate = &gate;
    std::thread posting([&] {
        check(racingDispatcher->post([&] { ++delivered; }), "in-flight post rejected");
    });
    gate.awaitEntered();
    std::promise<void> began;
    destructorEntered = &began;
    auto enteredFuture = began.get_future();
    auto destroying = std::async(std::launch::async, [&] { racingWidget.reset(); });
    enteredFuture.wait();
    check(destroying.wait_for(std::chrono::milliseconds(25)) == std::future_status::timeout,
        "close returned while a worker still borrowed the receiver");
    gate.release();
    posting.join();
    destroying.get();
    destructorEntered = nullptr;
    postGate = nullptr;
    drain();
    check(delivered == 1, "racing queued task outlived receiver");
    check(!racingDispatcher->post([] {}), "closed dispatcher reopened");
    std::cout << "ASYNC_UI_LIFETIME_CHECKS=" << checks.load() << "\n";
}
'''.replace("__DISPATCHER__", dispatcher)
    work = ROOT / ".codex-tmp/crash-audit/async-ui-lifetime"
    work.mkdir(parents=True, exist_ok=True)
    cpp = work / "async_ui_lifetime.cpp"
    executable = work / "async_ui_lifetime.exe"
    cpp.write_text(harness, encoding="utf-8")
    subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread", "-static",
        str(cpp), "-o", str(executable)], check=True, timeout=60)
    result = subprocess.run([str(executable)], check=True, timeout=15, capture_output=True, text=True)
    print(result.stdout.strip())
    print(f"ASYNC_UI_INTEGRATION_CHECKS={static_checks}")


if __name__ == "__main__":
    main()
