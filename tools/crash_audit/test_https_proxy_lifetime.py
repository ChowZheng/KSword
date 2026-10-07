#!/usr/bin/env python3
"""Compile the production HTTPS ownership/dispatch code with deterministic Qt shims.

No network listener, certificate store, or PowerShell process is started. This
checks lifetime ordering; it does not replace compilation with the real Qt SDK.
"""

from pathlib import Path
import argparse
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "Ksword5.1/Ksword5.1/NetworkDock/HttpsProxyService.cpp"


def extract_between(source: str, start: str, end: str) -> str:
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default="g++")
    args = parser.parse_args()
    compiler = shutil.which(args.compiler)
    if compiler is None:
        raise SystemExit(f"C++ compiler unavailable: {args.compiler}")

    source = SOURCE.read_text(encoding="utf-8-sig")
    certificate_class = extract_between(
        source, "    class HttpsProxyCertificateStore final", "    namespace\n"
    )
    dispatcher_class = extract_between(
        source, "    class HttpsProxyUiDispatcher final", "    // 会话证书工作"
    )
    registry = extract_between(
        source, "        class ProxySessionRegistry final", "        class ProxyServer final"
    )
    callbacks = extract_between(
        source, "        auto hostCertLoader =", "        auto sessionIdProvider ="
    )
    thread_owner = extract_between(
        source, "                const auto sessionThreadOwner =", "                QThread* sessionThread ="
    )

    harness = r'''
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using QString = std::string;
using QByteArray = std::string;
struct QSslCertificate {};
struct QSslKey {};
constexpr int kSessionStopFirstWaitMs = 3;
constexpr int kSessionStopFinalWaitMs = 5;
const auto uiThread = std::this_thread::get_id();
std::atomic_int checks{0};
void check(bool value, const char* reason) {
    if (!value) throw std::runtime_error(reason);
    ++checks;
}

struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool release = false;
    void block() {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [this] { return release; });
    }
    void awaitEntered() {
        std::unique_lock<std::mutex> lock(mutex);
        check(changed.wait_for(lock, std::chrono::seconds(5), [this] { return entered; }), "gate timeout");
    }
    void unblock() {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
        changed.notify_all();
    }
};
Gate certificateGate;
std::atomic_int certificatesFinished{0};

class QThread {
public:
    static std::atomic_int destroyed;
    static std::atomic_int unsafeDeletes;
    std::atomic_bool running{true};
    bool timedOut = false;
    Gate* waitGate = nullptr;
    ~QThread() { ++destroyed; }
    static QThread* currentThread() { return nullptr; }
    void requestInterruption() {}
    void quit() {}
    bool wait(int) {
        if (waitGate != nullptr) waitGate->block();
        return !timedOut;
    }
    void deleteLater() {
        if (running.load()) ++unsafeDeletes;
        delete this;
    }
};
std::atomic_int QThread::destroyed{0};
std::atomic_int QThread::unsafeDeletes{0};

class QObject {
public:
    std::shared_ptr<int> alive = std::make_shared<int>(0);
    virtual ~QObject() = default;
};
class QCoreApplication {
public:
    static bool available;
    static QCoreApplication* instance() {
        check(available, "late worker accessed destroyed application");
        static QCoreApplication app;
        return &app;
    }
    std::mutex mutex;
    std::vector<std::pair<std::weak_ptr<int>, std::function<void()>>> queue;
    void drain() {
        check(std::this_thread::get_id() == uiThread, "UI queue drained off thread");
        std::vector<std::pair<std::weak_ptr<int>, std::function<void()>>> pending;
        { std::lock_guard<std::mutex> lock(mutex); pending.swap(queue); }
        for (const auto& task : pending) if (!task.first.expired()) task.second();
    }
};
bool QCoreApplication::available = true;
namespace Qt { constexpr int QueuedConnection = 1; }
struct QMetaObject {
    template<class Task> static void invokeMethod(QObject* receiver, Task task, int) {
        auto* app = QCoreApplication::instance();
        std::lock_guard<std::mutex> lock(app->mutex);
        app->queue.emplace_back(receiver->alive, std::move(task));
    }
};
template<class T> class QPointer {
    std::weak_ptr<T> pointer;
public:
    QPointer(const std::shared_ptr<T>& value) : pointer(value) {}
    bool isNull() const { return pointer.expired(); }
    T* operator->() const { return pointer.lock().get(); }
};
struct HttpsProxyParsedEntry { int value = 0; };

__DISPATCHER_CLASS__

__CERTIFICATE_CLASS__

HttpsProxyCertificateStore::HttpsProxyCertificateStore() = default;
bool HttpsProxyCertificateStore::loadHostCertificateBundle(
    const QString&, QSslCertificate*, QSslKey*, QString*) {
    std::lock_guard<std::recursive_mutex> lock(m_certificateMutex);
    certificateGate.block();
    markRootCertificatePrepared();
    ++certificatesFinished;
    return true;
}

__REGISTRY__

std::shared_ptr<QThread> makeThreadOwner() {
__THREAD_OWNER__
    return sessionThreadOwner;
}

struct HttpsMitmProxyService : QObject, std::enable_shared_from_this<HttpsMitmProxyService> {
    std::shared_ptr<HttpsProxyCertificateStore> m_certificateStore = std::make_shared<HttpsProxyCertificateStore>();
    std::shared_ptr<HttpsProxyUiDispatcher> m_uiDispatcher = std::make_shared<HttpsProxyUiDispatcher>(this);
    ~HttpsMitmProxyService() override { m_uiDispatcher->close(); }
    static std::atomic_int deliveries;
    void emitParsedEntry(const HttpsProxyParsedEntry&) {
        check(std::this_thread::get_id() == uiThread, "parsed callback accessed service off UI thread");
        ++deliveries;
    }
    void emitStatus(const QString&) {
        check(std::this_thread::get_id() == uiThread, "status callback accessed service off UI thread");
        ++deliveries;
    }
    auto makeCallbacks() {
        const QPointer<HttpsMitmProxyService> safeThis(shared_from_this());
__CALLBACKS__
        return std::make_tuple(hostCertLoader, parsedEmitter, statusEmitter);
    }
};
std::atomic_int HttpsMitmProxyService::deliveries{0};

int main() {
    // A blocked certificate loader retains its lock and atomic after service destruction.
    std::function<bool(const QString&, QSslCertificate*, QSslKey*, QString*)> loader;
    auto service = std::make_shared<HttpsMitmProxyService>();
    std::weak_ptr<HttpsProxyCertificateStore> certificateStore = service->m_certificateStore;
    { auto callbacks = service->makeCallbacks(); loader = std::get<0>(callbacks); }
    std::thread certificateWorker([&] { check(loader("example.test", nullptr, nullptr, nullptr), "loader failed"); });
    certificateGate.awaitEntered();
    service.reset();
    check(!certificateStore.expired(), "service destruction freed active certificate store");
    certificateGate.unblock();
    certificateWorker.join();
    check(certificatesFinished == 1, "late certificate task did not complete");
    loader = {};
    check(certificateStore.expired(), "certificate store leaked after final task release");

    // finished/remove races with stopAndWait: the wait snapshot must own QThread.
    auto registry = std::make_shared<ProxySessionRegistry>();
    auto thread = makeThreadOwner();
    Gate waitGate;
    thread->waitGate = &waitGate;
    QThread* raw = thread.get();
    std::weak_ptr<QThread> weakThread = thread;
    check(registry->add(thread), "initial registry add rejected");
    thread.reset();
    std::thread stopWorker([registry] { registry->stopAndWait(); });
    waitGate.awaitEntered();
    raw->running.store(false);
    registry->remove(raw);
    check(!weakThread.expired(), "finished freed QThread used by stop snapshot");
    waitGate.unblock();
    stopWorker.join();
    check(weakThread.expired(), "completed QThread retained after snapshot release");

    // Both waits time out: the registry retains the active thread until finished.
    auto lateRegistry = std::make_shared<ProxySessionRegistry>();
    auto lateThread = makeThreadOwner();
    lateThread->timedOut = true;
    auto* lateRaw = lateThread.get();
    std::weak_ptr<QThread> lateWeak = lateThread;
    check(lateRegistry->add(lateThread), "late registry add rejected");
    lateThread.reset();
    lateRegistry->stopAndWait();
    check(!lateWeak.expired(), "timed out QThread destroyed while running");
    lateRaw->running.store(false);
    lateRegistry->remove(lateRaw);
    check(lateWeak.expired(), "late finished QThread did not release");
    check(QThread::unsafeDeletes == 0, "running QThread scheduled for destruction");
    check(QThread::destroyed == 2, "thread cleanup count mismatch");

    // Background emitters only enqueue; UI checks the guard after the service dies.
    auto liveService = std::make_shared<HttpsMitmProxyService>();
    auto liveCallbacks = liveService->makeCallbacks();
    std::thread emitter([&] {
        std::get<1>(liveCallbacks)(HttpsProxyParsedEntry{});
        std::get<2>(liveCallbacks)("sample status");
    });
    emitter.join();
    check(HttpsMitmProxyService::deliveries == 0, "callbacks delivered before UI queue drain");
    QCoreApplication::instance()->drain();
    check(HttpsMitmProxyService::deliveries == 2, "live UI callbacks lost");
    std::get<1>(liveCallbacks)(HttpsProxyParsedEntry{});
    std::get<2>(liveCallbacks)("late status");
    liveService.reset();
    QCoreApplication::instance()->drain();
    check(HttpsMitmProxyService::deliveries == 2, "late callback touched destroyed service");
    QCoreApplication::available = false;
    // Late session emitters must work even after the QApplication has disappeared.
    std::thread lateEmitter([&] {
        std::get<1>(liveCallbacks)(HttpsProxyParsedEntry{});
        std::get<2>(liveCallbacks)("status after app destruction");
    });
    lateEmitter.join();
    check(HttpsMitmProxyService::deliveries == 2, "callback delivered after application destruction");
    std::cout << "HTTPS_LIFETIME_CHECKS=" << checks.load() << "\n";
}
'''
    for marker, code in {
        "__CERTIFICATE_CLASS__": certificate_class,
        "__DISPATCHER_CLASS__": dispatcher_class,
        "__REGISTRY__": registry,
        "__THREAD_OWNER__": thread_owner,
        "__CALLBACKS__": callbacks,
    }.items():
        harness = harness.replace(marker, code)

    temp_root = ROOT / ".codex-tmp/crash-audit"
    temp_root.mkdir(parents=True, exist_ok=True)
    temp = temp_root / "https-lifetime"
    temp.mkdir(parents=True, exist_ok=True)
    cpp = temp / "https_lifetime.cpp"
    executable = temp / "https_lifetime.exe"
    cpp.write_text(harness, encoding="utf-8")
    subprocess.run(
        [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread", "-static", str(cpp), "-o", str(executable)],
        check=True, timeout=60,
    )
    result = subprocess.run([str(executable)], check=True, timeout=15, capture_output=True, text=True)
    print(result.stdout.strip())


if __name__ == "__main__":
    main()
