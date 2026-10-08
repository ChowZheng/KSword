"""Run extracted production callback/dialog guards with deterministic lifetimes.

The C++ guards execute verbatim. Qt dialog/file implementations are not exercised;
their path ownership contract is checked here without launching WPR or a GUI.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
from test_physical_page_mappings import ROOT, SOURCE


def main():
    page = (SOURCE / 'PhysicalPageAttributionPage.cpp').read_text(encoding='utf-8-sig')
    first = page.index('    if (m_job && m_job->done.load()) {', page.index('void PhysicalPageAttributionPage::poll()'))
    end = page.index('    if (m_mappingJob &&', first)
    callback_block = page[first:end]
    trace = (SOURCE / 'MemoryConsumerEvidencePage.cpp').read_text(encoding='utf-8-sig')
    first = trace.index('    if (!m_capture.canStart())', trace.index('void MemoryConsumerEvidencePage::startTrace()'))
    end = trace.index('    m_output = output;', first)
    dialog_guard = trace[first:end]
    # Contract: reserve the UUID sidecar exclusively before the first command,
    # then persist solely to that path. No fixed ETL sibling may be overwritten.
    start = trace[trace.index('void MemoryConsumerEvidencePage::startTrace()'):trace.index('void MemoryConsumerEvidencePage::appendCommandOutput')]
    persist = trace[trace.index('void MemoryConsumerEvidencePage::persistTrace()'):trace.index('QJsonObject MemoryConsumerEvidencePage::evidence()')]
    assert 'm_output + QStringLiteral(".metadata-") + m_instance' in start
    assert start.index('QIODevice::NewOnly') < start.index('runTrace(')
    assert 'QSaveFile file(m_metadataOutput)' in persist
    assert 'QSaveFile file(m_output' not in persist
    source = r'''
#include "PoolTraceCapturePolicy.h"
#include <atomic>
#include <cassert>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
template<class T> class QPointer {
    std::weak_ptr<bool> alive;
public:
    explicit QPointer(T* object) : alive(object->alive) {}
    explicit operator bool() const { const auto state = alive.lock(); return state && *state; }
};
struct Counts { unsigned expected = 1, valid = 1; bool reconciles() const { return true; } };
struct Scan { Counts accounting; int auditSample = 7; bool resourceFailure = false; };
struct Job { std::atomic_bool done{true}; std::mutex mutex; std::shared_ptr<Scan> result = std::make_shared<Scan>(); };
static bool destroyed = false;
static unsigned rebuilds = 0, snapshots = 0;
struct PhysicalPageAttributionPage {
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    std::shared_ptr<Job> m_job = std::make_shared<Job>();
    std::shared_ptr<Scan> m_lastAttempt, m_scan, m_mappingScan;
    bool m_latestAttemptFailed = false;
    std::vector<int> m_auditHistory;
    std::function<void(const std::shared_ptr<Scan>&)> snapshotReady;
    ~PhysicalPageAttributionPage() { *alive = false; destroyed = true; }
    void rebuild() { assert(!destroyed); ++rebuilds; }
    void poll() { CALLBACK_BLOCK }
};
class QString : public std::string {
public:
    using std::string::string;
    bool isEmpty() const { return empty(); }
};
#define QStringLiteral(value) QString(value)
#define L(value) QString(value)
struct QFileDialog {
    static inline std::function<QString()> next;
    template<class... T> static QString getSaveFileName(T&&...) { return next(); }
};
struct MemoryConsumerEvidencePage {
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    ksword::pool_trace::CaptureState m_capture;
    QString m_output, m_instance;
    void startTrace() {
        DIALOG_GUARD
        m_output = output;
        m_capture = {};
        m_instance = "selected-instance";
        assert(m_capture.begin(ksword::pool_trace::Command::Help));
    }
};
int main() {
    auto* page = new PhysicalPageAttributionPage;
    std::shared_ptr<Scan> retained;
    page->snapshotReady = [page, &retained](const auto& snapshot) {
        delete page;
        // The callback and snapshot must both remain owned by locals after deletion.
        retained = snapshot;
        assert(retained && retained->accounting.valid == 1); ++snapshots;
    };
    page->poll();
    assert(destroyed && rebuilds == 0 && snapshots == 1 && retained);
    destroyed = false;
    page = new PhysicalPageAttributionPage;
    auto good = std::make_shared<Scan>(); page->m_scan = good;
    page->m_job->result->resourceFailure = true;
    page->poll();
    assert(page->m_latestAttemptFailed && page->m_scan == good && rebuilds == 1);
    delete page;
    using namespace ksword::pool_trace;
    MemoryConsumerEvidencePage trace;
    QFileDialog::next = [&trace] {
        trace.m_instance = "inner-instance";
        assert(trace.m_capture.begin(Command::Start));
        trace.m_capture.finish(Completion::Success);
        return QString("outer.etl");
    };
    trace.startTrace();
    assert(trace.m_capture.mayOwnSession && trace.m_capture.recording());
    assert(trace.m_instance == "inner-instance" && trace.m_output.empty());
    assert(trace.m_capture.shutdown() == Action::Stop);
    std::cout << "MEMORY_EVIDENCE_LIFETIMES_TESTS=PASS callback-deletion, failure-retention, dialog-reentry, exclusive-sidecar\n";
}
'''.replace('CALLBACK_BLOCK', callback_block).replace('DIALOG_GUARD', dialog_guard)
    compiler = shutil.which('g++') or shutil.which('clang++')
    if not compiler:
        raise SystemExit('G++ or Clang++ is required.')
    build_root = ROOT / '.codex-tmp/memory-evidence-lifetimes'
    build_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='run-', dir=build_root) as temporary:
        build = Path(temporary)
        (build / 'test.cpp').write_text(source, encoding='utf-8')
        exe = build / 'lifetime-tests.exe'
        subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-O2', '-I', str(ROOT / 'shared/evidence'), str(build / 'test.cpp'), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=60)


if __name__ == '__main__':
    main()
