"""Run both production HUD snapshot workers against a thread-aware Qt adapter.

This does not create a GUI, query hardware, load a driver, or claim a Qt build.
The worker functions are extracted from production on each run; page access is
rejected on the real background thread, including attempted queued receivers.
"""

import os
from pathlib import Path
import re
import shutil
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "KswordHUD/HudPerformancePanel.cpp"


def function_text(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for position in range(opening, len(source)):
        depth += (source[position] == "{") - (source[position] == "}")
        if depth == 0:
            return source[start:position + 1]
    raise AssertionError("Unclosed production function")


SHIM = r'''
#include <atomic>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
static const auto uiThread = std::this_thread::get_id();
static void check(bool condition) {
    if (!condition) throw std::runtime_error("HUD worker lifetime invariant failed");
}
class QString {
public:
    std::string value;
    QString() = default;
    QString(const char* text) : value(text) {}
    explicit QString(std::string text) : value(std::move(text)) {}
    QString arg(const QString& text) const {
        auto formatted = value;
        const auto marker = formatted.find('%'); check(marker != std::string::npos);
        formatted.replace(marker, 2, text.value); return QString(std::move(formatted));
    }
};
#define QStringLiteral(text) QString(text)
class QObject {};
static QObject application;
static QObject* currentApplication = &application;
class QCoreApplication {
public: static QObject* instance() { return currentApplication; }
};
namespace Qt { enum ConnectionType { QueuedConnection }; }
static std::deque<std::function<void()>> uiQueue, workerQueue;
class QMetaObject {
public:
    template<class F> static bool invokeMethod(QObject* receiver, F callback, Qt::ConnectionType) {
        check(receiver == &application && std::this_thread::get_id() != uiThread);
        uiQueue.emplace_back(std::move(callback)); return true;
    }
};
class QThreadPool {
public:
    static QThreadPool* globalInstance() { static QThreadPool pool; return &pool; }
    template<class F> void start(F callback) { workerQueue.emplace_back(std::move(callback)); }
};
template<class T> class QPointer {
    T* pointer;
    std::shared_ptr<std::atomic_bool> alive;
public:
    explicit QPointer(T* value) : pointer(value), alive(value->alive) {}
    bool isNull() const { check(std::this_thread::get_id() == uiThread); return !alive->load(); }
    T* data() const { return isNull() ? nullptr : pointer; }
    T* operator->() const { check(!isNull()); return pointer; }
};
struct MemoryHardwareSummarySnapshot {
    int speedMhz = 3200, usedSlots = 2, totalSlots = 4;
    QString formFactorText = QStringLiteral("DIMM");
};
struct GpuHardwareSummarySnapshot {
    QString adapterNameText = QStringLiteral("GPU"), driverVersionText = QStringLiteral("version");
    QString driverDateText = QStringLiteral("date"), pnpDeviceIdText = QStringLiteral("device");
    double dedicatedMemoryGiB = 8;
};
static unsigned detailQueries, sensorQueries;
static MemoryHardwareSummarySnapshot queryMemoryHardwareSummarySnapshot() { ++detailQueries; return {}; }
static GpuHardwareSummarySnapshot queryGpuHardwareSummarySnapshot() { ++detailQueries; return {}; }
static QString queryCpuTemperatureText() { ++sensorQueries; return QStringLiteral("temperature"); }
static QString queryCpuVoltageText() { ++sensorQueries; return QStringLiteral("voltage"); }
class HudPerformancePanel : public QObject {
public:
    std::shared_ptr<std::atomic_bool> alive = std::make_shared<std::atomic_bool>(true);
    std::atomic_bool m_staticInfoRefreshing{false}, m_sensorRefreshing{false};
    int m_memorySpeedMhz = 0, m_memorySlotUsed = 0, m_memorySlotTotal = 0;
    QString m_memoryFormFactorText, m_gpuAdapterNameText, m_gpuDriverVersionText;
    QString m_gpuDriverDateText, m_gpuPnpDeviceIdText, m_cachedSensorText;
    double m_gpuDedicatedMemoryGiB = 0;
    ~HudPerformancePanel() { alive->store(false); }
    void requestAsyncStaticInfoRefresh();
    void requestAsyncSensorRefresh();
};
'''


CASES = r'''
static void reset() {
    check(workerQueue.empty() && uiQueue.empty());
    detailQueries = sensorQueries = 0; currentApplication = &application;
}
static void work() {
    check(!workerQueue.empty());
    auto callback = std::move(workerQueue.front()); workerQueue.pop_front();
    std::exception_ptr failure;
    std::thread worker([&] { try { callback(); } catch (...) { failure = std::current_exception(); } });
    worker.join(); if (failure) std::rethrow_exception(failure);
}
static void deliver() {
    check(!uiQueue.empty());
    auto callback = std::move(uiQueue.front()); uiQueue.pop_front(); callback();
}
static void requestBoth(HudPerformancePanel& panel) {
    panel.requestAsyncStaticInfoRefresh(); panel.requestAsyncSensorRefresh();
}
static int runCases() {
    // Both production workers defer writes to UI delivery and suppress duplicate requests.
    reset(); {
        HudPerformancePanel panel; requestBoth(panel); requestBoth(panel);
        check(workerQueue.size() == 2 && panel.m_staticInfoRefreshing.load() && panel.m_sensorRefreshing.load());
        work(); work();
        check(panel.m_memorySpeedMhz == 0 && panel.m_cachedSensorText.value.empty());
        deliver(); deliver();
        check(detailQueries == 2 && sensorQueries == 2);
        check(panel.m_memorySpeedMhz == 3200 && panel.m_memorySlotUsed == 2 && panel.m_memorySlotTotal == 4);
        check(panel.m_gpuDedicatedMemoryGiB == 8 && panel.m_cachedSensorText.value == "temperature|voltage");
        check(!panel.m_staticInfoRefreshing.load() && !panel.m_sensorRefreshing.load());
        requestBoth(panel); work(); work(); deliver(); deliver();
        check(detailQueries == 4 && sensorQueries == 4);
    }
    // Each pending worker remains safe if the page disappears before collection or UI delivery.
    for (bool beforeWorker : {true, false}) {
        reset(); auto panel = std::make_unique<HudPerformancePanel>(); requestBoth(*panel);
        if (beforeWorker) panel.reset();
        work(); work(); panel.reset(); deliver(); deliver();
        check(detailQueries == 2 && sensorQueries == 2);
    }
    // Application-less requests synchronously restore both in-flight flags.
    reset(); {
        HudPerformancePanel panel; currentApplication = nullptr; requestBoth(panel);
        check(workerQueue.empty() && !panel.m_staticInfoRefreshing.load() && !panel.m_sensorRefreshing.load());
    }
    // Dropping queued results during application teardown must not access the destroyed page.
    reset(); {
        auto panel = std::make_unique<HudPerformancePanel>(); requestBoth(*panel); work(); work();
        panel.reset(); uiQueue.clear(); check(workerQueue.empty() && uiQueue.empty());
    }
    std::cout << "PASS: 5 HUD production worker lifecycle scenarios\n"; return 0;
}
int main() {
    try { return runCases(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
'''


class HudAsyncLifetimeTests(unittest.TestCase):
    def production_functions(self):
        source = SOURCE.read_text(encoding="utf-8-sig")
        return "\n".join(function_text(source, f"void HudPerformancePanel::requestAsync{name}Refresh()")
                         for name in ("StaticInfo", "Sensor"))

    def replay(self, functions):
        compiler = shutil.which("g++")
        if compiler is None:
            self.fail("g++ is required for the production HUD lifecycle replay")
        scratch = ROOT / ".codex-tmp" / "hud-lifetime"
        scratch.mkdir(parents=True, exist_ok=True)
        source = scratch / "replay.cpp"
        executable = scratch / "replay.exe"
        source.write_text(SHIM + functions + CASES, encoding="utf-8")
        env = dict(os.environ, TEMP=str(scratch), TMP=str(scratch))
        env["PATH"] = str(Path(compiler).parent) + os.pathsep + env.get("PATH", "")
        subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread", str(source), "-o", str(executable)],
                       check=True, capture_output=True, text=True, env=env, timeout=60)
        return subprocess.run([str(executable)], capture_output=True, text=True, env=env, timeout=15)

    def test_production_worker_lifetimes(self):
        result = self.replay(self.production_functions())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("PASS: 5", result.stdout)
        print(result.stdout.strip())

    def test_replay_rejects_each_worker_using_the_page_receiver(self):
        for index in range(2):
            with self.subTest(worker=index):
                functions = self.production_functions()
                positions = list(re.finditer("            applicationContext,", functions))
                position = positions[index].start()
                mutation = functions[:position] + functions[position:].replace("            applicationContext,", "            safeThis.data(),", 1)
                # Keep the unused application capture from turning a runtime regression into a compile failure.
                mutation = mutation.replace("        const MemoryHardwareSummarySnapshot memorySummary =", "        (void)applicationContext;\n        const MemoryHardwareSummarySnapshot memorySummary =")
                mutation = mutation.replace("        const QString sensorText =", "        (void)applicationContext;\n        const QString sensorText =")
                result = self.replay(mutation)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn("HUD worker lifetime invariant failed", result.stderr)

    def test_live_sampling_is_drained_before_native_handles_close(self):
        source = SOURCE.read_text(encoding="utf-8-sig")
        body = function_text(source, "HudPerformancePanel::~HudPerformancePanel()")
        self.assertLess(body.index("m_refreshTimer->stop()"), body.index("m_liveSampleWatcher->waitForFinished()"))
        self.assertLess(body.index("m_liveSampleWatcher->waitForFinished()"), body.index("::PdhCloseQuery("))
        self.assertNotIn(".detach()", self.production_functions())


if __name__ == "__main__":
    unittest.main()
