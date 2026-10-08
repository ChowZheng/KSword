"""Execute the production PFN scan/worker with mocked OS and Qt dependencies.

These fixtures validate lifecycle and failure behavior, not the real native ABI.
Raw JSON serialization is covered separately by the streaming auditor fixtures.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
from test_physical_page_mappings import ROOT, SOURCE, STRING, DATE, UUID

QT = r'''#pragma once
#include "QString"
#include <initializer_list>
#include <string>
#include <utility>
using qint64 = long long;
using qsizetype = long long;
class QByteArray {
    std::string data = "{}";
public:
    void append(char value) { data += value; }
    qsizetype size() const { return static_cast<qsizetype>(data.size()); }
    const char* constData() const { return data.data(); }
};
struct QJsonValue { template<class T> QJsonValue(const T&) {} };
struct QJsonArray {
    int count = 0;
    QJsonArray() = default;
    QJsonArray(std::initializer_list<QJsonValue> values) : count(static_cast<int>(values.size())) {}
    template<class T> void append(const T&) { ++count; }
    int size() const { return count; }
};
struct QJsonObject {
    QJsonObject() = default;
    QJsonObject(std::initializer_list<std::pair<QString, QJsonValue>>) {}
    template<class T> void insert(const QString&, const T&) {}
};
struct QJsonDocument {
    enum { Compact };
    explicit QJsonDocument(const QJsonObject&) {}
    QByteArray toJson(int) const { return {}; }
};
namespace QIODevice { enum { WriteOnly = 1, Truncate = 2 }; }
struct QFile {
    explicit QFile(const QString&) {}
    bool open(int) { return false; }
    bool isOpen() const { return false; }
    QString errorString() const { return QStringLiteral("mock disk unavailable"); }
    qint64 write(const char*, qint64) { return -1; }
    bool flush() { return false; }
    void close() {}
};
'''

MOCK = r'''#pragma once
#include <Windows.h>
#include <Psapi.h>
#include <array>
#include <atomic>
#include <cassert>
#include <cstring>
#include <map>
#include <new>
#include <vector>
constexpr unsigned KSWORD_ARK_PFN_MAX_OWNERS = 16384, KSWORD_ARK_PFN_MAX_PAGES = 4096;
struct KSWORD_ARK_PFN_RANGE { std::uint64_t firstPfn, pageCount; };
struct KSWORD_ARK_PFN_OWNER { std::uint64_t processKey; unsigned processId, sessionId; char imageName[16]; };
struct KSWORD_ARK_PFN_IDENTITY { std::uint64_t frame, pfn, backing; };
namespace mock {
enum Mode { Stable, BeforeOnly, AfterOnly, ConflictingOwner, Died, Denied, ChangedCreation, CompressionName, Oom };
inline Mode mode = Stable;
inline unsigned owners, pages, opened, closed;
inline std::atomic_bool* cancelled = nullptr;
inline void reset(Mode value) { mode = value; owners = pages = opened = closed = 0; cancelled = nullptr; }
}
inline HANDLE FakeOpenProcess(DWORD access, BOOL, DWORD pid) {
    assert(access == (PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE));
    if (mock::mode == mock::Denied) { return nullptr; }
    ++mock::opened; return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(pid));
}
inline BOOL FakeCloseHandle(HANDLE handle) { assert(handle); ++mock::closed; return TRUE; }
inline DWORD FakeGetProcessId(HANDLE handle) { return static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(handle)); }
inline DWORD FakeWaitForSingleObject(HANDLE, DWORD milliseconds) {
    assert(!milliseconds);
    return mock::mode == mock::Died && mock::owners == 2 ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}
inline BOOL FakeGetProcessTimes(HANDLE, FILETIME* created, FILETIME*, FILETIME*, FILETIME*) {
    *created = {mock::mode == mock::ChangedCreation && mock::owners == 2 ? 101UL : 100UL, 0}; return TRUE;
}
inline BOOL FakeGetProcessMemoryInfo(HANDLE, PROCESS_MEMORY_COUNTERS*, DWORD) { return FALSE; }
inline BOOL FakeGlobalMemoryStatusEx(MEMORYSTATUSEX* memory) { memory->ullTotalPhys = 8192ULL * 4096; memory->ullAvailPhys = 0; return TRUE; }
inline BOOL FakeGetPhysicallyInstalledSystemMemory(PULONGLONG kb) { *kb = 8192ULL * 4; return TRUE; }
inline BOOL FakeGetComputerNameW(wchar_t* host, DWORD* count) { host[0] = L'x'; host[1] = 0; *count = 1; return TRUE; }
inline void FakeGetNativeSystemInfo(SYSTEM_INFO* info) { *info = {}; info->wProcessorArchitecture = PROCESSOR_ARCHITECTURE_AMD64; info->dwPageSize = 4096; }
inline HMODULE FakeGetModuleHandleW(const wchar_t*) { return nullptr; }
inline void* FakeGetProcAddress(HMODULE, const char*) { return nullptr; }
#define OpenProcess FakeOpenProcess
#define CloseHandle FakeCloseHandle
#define GetProcessId FakeGetProcessId
#define GetProcessTimes FakeGetProcessTimes
#define WaitForSingleObject FakeWaitForSingleObject
#undef GetProcessMemoryInfo
#define GetProcessMemoryInfo FakeGetProcessMemoryInfo
#define GlobalMemoryStatusEx FakeGlobalMemoryStatusEx
#define GetPhysicallyInstalledSystemMemory FakeGetPhysicallyInstalledSystemMemory
#define GetComputerNameW FakeGetComputerNameW
#define GetNativeSystemInfo FakeGetNativeSystemInfo
#define GetModuleHandleW FakeGetModuleHandleW
#define GetProcAddress FakeGetProcAddress
namespace ksword::ark {
struct PfnNativeAttempt { unsigned long informationClass = 6, abiVersion = 1; long status = 0; bool invoked = true; };
struct PfnQueryTrace {
    unsigned nativeAttemptCount = 1;
    std::array<PfnNativeAttempt, 2> nativeAttempts{};
    unsigned long driverOperation = 0;
    long driverStatus = 0, driverTransportStatus = 0;
    bool driverAvailable = false, driverInvoked = false;
};
class PfnQueryClient {
public:
    std::uint64_t nativeBatches = 0, driverBatches = 0;
    PfnQueryTrace lastTrace() const { return {}; }
    long ranges(std::vector<KSWORD_ARK_PFN_RANGE>& result) { result = {{100,8192}}; ++nativeBatches; return 0; }
    long owners(std::vector<KSWORD_ARK_PFN_OWNER>& result) {
        ++mock::owners; ++nativeBatches; result.clear();
        if ((mock::mode == mock::BeforeOnly && mock::owners == 2) || (mock::mode == mock::AfterOnly && mock::owners == 1)) { return 0; }
        KSWORD_ARK_PFN_OWNER owner{}; owner.processKey = 0xaa;
        owner.processId = mock::mode == mock::ConflictingOwner && mock::owners == 2 ? 51 : 41;
        std::strcpy(owner.imageName, mock::mode == mock::CompressionName ? "MemCompression" : "worker.exe");
        result.push_back(owner); return 0;
    }
    long pages(std::uint64_t first, unsigned count, std::vector<KSWORD_ARK_PFN_IDENTITY>& result) {
        ++mock::pages;
        if (mock::mode == mock::Oom && mock::pages == 2) { throw std::bad_alloc(); }
        result.clear();
        for (unsigned i = 0; i < count; ++i) { result.push_back({(0xaaULL << 9) | (6ULL << 4),first+i,0x1000}); }
        ++nativeBatches; return 0;
    }
    long identities(const std::vector<std::uint64_t>&, std::vector<KSWORD_ARK_PFN_IDENTITY>&) { assert(false); return -1; }
};
}
'''

TEST = r'''
#include "PhysicalPageScan.cpp"
#include <iostream>
#include <cstdlib>
unsigned checks = 0;
void require(bool value, const char* message) { ++checks; if (!value) { std::cerr << message << '\n'; std::exit(1); } }
std::shared_ptr<ksword::pfn::Scan> run(mock::Mode mode, bool cancel = false) {
    mock::reset(mode);
    auto job = std::make_shared<ksword::pfn::ScanJob>(); job->cancel.store(cancel);
    ksword::pfn::collectPhysicalPages(job);
    require(job->done.load() && bool(job->result), "worker must publish");
    require(mock::opened == mock::closed, "lifetime leases must close even on resource failure");
    return job->result;
}
int main() {
    using namespace ksword::pfn;
    auto scan = run(mock::Stable);
    require(scan->complete && scan->accounting.expected == 8192 && scan->resolvedPrivatePages == 8192, "held stable identity resolves all observed private pages");
    require(scan->groups.size() == 1 && scan->groups[0].ownerLifetimeVerified && scan->groups[0].ownerCreateTime == 100, "group retains the lifetime proof");
    for (auto mode : {mock::BeforeOnly, mock::AfterOnly, mock::ConflictingOwner, mock::Died, mock::Denied, mock::ChangedCreation}) {
        scan = run(mode);
        require(scan->complete && scan->accounting.inUse(Use::Private) == 8192, "missing owner does not change a valid native classification");
        require(scan->resolvedPrivatePages == 0 && scan->unresolvedPrivatePages == 8192 && scan->ownerCoverage.resolved[0][6] == 0, "weak/recycled/exited identities stay unresolved");
    }
    scan = run(mock::CompressionName);
    require(scan->accounting.inUse(Use::Compression) == 0 && scan->accounting.inUse(Use::Private) == 8192, "ordinary process named MemCompression stays Private");
    require(scan->examples[0].size() == 256 && scan->groups[0].use == Use::Private, "name hints cannot remove native private evidence");
    scan = run(mock::Oom);
    require(scan->resourceFailure && !scan->complete && scan->accounting.expected == 8192 && scan->accounting.visited == 4096 && scan->accounting.notScanned() == 4096, "bad_alloc preserves known denominator and completed progress");
    require(!scan->auditSample.complete && scan->auditSample.unscannedPages == 4096 && scan->nativeBatches == 3, "failure preserves coverage and completed provider operations");
    require(scan->ownerCoverage.reconciles(scan->accounting) && scan->unresolvedPrivatePages == 4096, "failure consumer coverage partitions the retained native ledger");
    require(!scan->rawEvidenceFinalized && !scan->rawEvidenceComplete, "resource failure never finalizes evidence");
    scan = run(mock::Stable, true);
    require(scan->cancelled && !scan->complete && scan->accounting.notScanned() == 8192 && mock::opened == 0, "cancelled scan skips lifetime lease work and retains unscanned RAM");
    std::cout << "PHYSICAL_PAGE_SCAN_TESTS=PASS checks=" << checks << '\n';
}
'''


def main():
    compiler = shutil.which('g++') or shutil.which('clang++')
    if not compiler:
        raise SystemExit('G++ or Clang++ is required.')
    build_root = ROOT / '.codex-tmp/physical-page-scan'
    build_root.mkdir(parents=True, exist_ok=True)
    string = STRING.replace('    bool isEmpty()', '''    static QString fromWCharArray(const wchar_t* input) { return QString(std::wstring(input)); }
    static QString fromLatin1(const char* input, qsizetype length = -1) {
        std::string text(input, length < 0 ? std::char_traits<char>::length(input) : static_cast<std::size_t>(length));
        return QString(std::wstring(text.begin(), text.end()));
    }
    template<class T> static QString number(T input) { return QString(std::to_wstring(input)); }
    template<class... T> QString arg(const T&...) const { return *this; }
    std::string toStdString() const { return std::string(value.begin(), value.end()); }
    bool operator==(const QString& other) const { return value == other.value; }
    bool isEmpty()''').replace('class QString {', 'using qsizetype = long long;\nclass QString {')
    with tempfile.TemporaryDirectory(prefix='run-', dir=build_root) as temporary:
        build = Path(temporary)
        for name, data in {'QString': string, 'QDateTime': DATE.replace('    static QDateTime currentDateTime()', '    static QDateTime currentDateTimeUtc() { return {}; }\n    static QDateTime currentDateTime()'), 'QUuid': UUID, 'FakePfnClient.h': MOCK, 'QtMock.h': QT}.items():
            (build / name).write_text(data, encoding='utf-8')
        for name in ('QFile', 'QJsonArray', 'QJsonDocument', 'QJsonObject'):
            (build / name).write_text('#include "QtMock.h"\n', encoding='utf-8')
        header = (SOURCE / 'PhysicalPageScan.h').read_text(encoding='utf-8-sig').replace('../../../shared/evidence/PfnAccounting.h', (ROOT / 'shared/evidence/PfnAccounting.h').as_posix()).replace('../ArkDriverClient/ArkDriverPfn.h', 'FakePfnClient.h')
        (build / 'PhysicalPageScan.h').write_text(header, encoding='utf-8')
        production = (SOURCE / 'PhysicalPageScan.cpp').read_text(encoding='utf-8-sig').replace('../ArkDriverClient/ArkDriverPfn.h', 'FakePfnClient.h')
        (build / 'PhysicalPageScan.cpp').write_text(production, encoding='utf-8')
        (build / 'test.cpp').write_text(TEST, encoding='utf-8')
        exe = build / 'scan-tests.exe'
        subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-O2', '-I', str(build), str(build / 'test.cpp'), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=60)


if __name__ == '__main__':
    main()
