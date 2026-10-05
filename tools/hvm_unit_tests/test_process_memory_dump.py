"""Exercise the real process KMDUMP writer with scripted memory and file I/O.

The production function is extracted verbatim. Win32 process/query calls, the
existing memory facade and QSaveFile are replaced at their external boundaries.
This validates format, identity and failure/commit control flow without Qt or a
loaded driver. Run: python tools/hvm_unit_tests/test_process_memory_dump.py
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import uuid

from test_memory_backend import QT_SHIM
from test_memory_bookmarks import extract_method


ROOT = Path(__file__).resolve().parents[2]
PRODUCTION = ROOT / "Ksword5.1/Ksword5.1/MemoryDock/MemoryDock.ProcessRegion.cpp"

PRELUDE = r'''
#include "MemoryAccessBackend.h"
#include <cstring>
#include <deque>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#define _KERNEL_MODE 1
#include "KswordArkMemoryIoctl.h"
using HANDLE = void*;
using LPCVOID = const void*;
using SIZE_T = std::size_t;
using BOOL = int;
using DWORD = unsigned long;
using qint64 = std::int64_t;
constexpr int FALSE = 0;
constexpr DWORD ERROR_INVALID_PARAMETER = 87;
constexpr unsigned WAIT_TIMEOUT = 258;
constexpr unsigned MEM_COMMIT = 0x1000;
struct FILETIME { std::uint32_t dwLowDateTime{}, dwHighDateTime{}; };
union ULARGE_INTEGER {
    struct { std::uint32_t LowPart, HighPart; };
    std::uint64_t QuadPart;
};
struct SYSTEM_INFO { void* lpMinimumApplicationAddress; void* lpMaximumApplicationAddress; };
struct MEMORY_BASIC_INFORMATION {
    void* BaseAddress{}; SIZE_T RegionSize{}; std::uint32_t Protect{}, State{}, Type{};
};
static std::uint32_t processPid = 77;
static std::uint64_t processCreation = 0x123456789ULL;
static bool processAlive = true, timesAvailable = true;
static bool processWow64, wow64QueryAvailable = true;
static DWORD queryError = 5;
static SIZE_T shortQueryResponse;
static unsigned identityChecks, exitOnIdentityCheck, queryCalls;
static std::uint64_t maximumAddress = 0x4000;
static std::deque<MEMORY_BASIC_INFORMATION> queryScript;
static unsigned GetProcessId(HANDLE) { return processPid; }
static unsigned WaitForSingleObject(HANDLE, unsigned) {
    ++identityChecks;
    if (exitOnIdentityCheck && identityChecks == exitOnIdentityCheck) processAlive = false;
    return processAlive ? WAIT_TIMEOUT : 0;
}
static int GetProcessTimes(HANDLE, FILETIME* creation, FILETIME*, FILETIME*, FILETIME*) {
    creation->dwLowDateTime = static_cast<std::uint32_t>(processCreation);
    creation->dwHighDateTime = static_cast<std::uint32_t>(processCreation >> 32U);
    return timesAvailable;
}
static BOOL IsWow64Process(HANDLE, BOOL* value) {
    *value = processWow64; return wow64QueryAvailable;
}
static void GetSystemTimeAsFileTime(FILETIME* value) { value->dwLowDateTime = 1234; }
static void GetSystemInfo(SYSTEM_INFO* value) {
    value->lpMinimumApplicationAddress = reinterpret_cast<void*>(0x1000);
    value->lpMaximumApplicationAddress = reinterpret_cast<void*>(maximumAddress);
}
static SIZE_T VirtualQueryEx(HANDLE, LPCVOID, MEMORY_BASIC_INFORMATION* value, SIZE_T size) {
    ++queryCalls;
    if (shortQueryResponse && queryScript.empty()) return shortQueryResponse;
    if (queryScript.empty()) return 0;
    *value = queryScript.front(); queryScript.pop_front(); return size;
}
static DWORD GetLastError() { return queryError; }
static unsigned toDwordPid(std::uint32_t pid) { return pid; }
static bool isReadableProtect(std::uint32_t protect) { return protect == 4; }
struct Progress {
    void set(int, const char*, int, float) {}
} kPro;
namespace QIODevice { constexpr int WriteOnly = 1; }
static std::vector<char> destination, pending;
static unsigned writeCalls, seekCalls, commitCalls;
static unsigned failWrite, failSeek;
static bool failOpen, failCommit, fallbackDisabled;
class QSaveFile {
    qint64 offset = 0;
public:
    explicit QSaveFile(const QString&) {}
    void setDirectWriteFallback(bool enabled) { fallbackDisabled = !enabled; }
    bool open(int) { pending.clear(); return !failOpen; }
    qint64 pos() const { return offset; }
    QString errorString() const { return QStringLiteral("fixture I/O failure"); }
    qint64 write(const char* data, qint64 length) {
        ++writeCalls;
        if (failWrite && failWrite == writeCalls) return length ? length - 1 : -1;
        if (offset < 0 || length < 0) return -1;
        const auto end = static_cast<std::size_t>(offset + length);
        if (pending.size() < end) pending.resize(end);
        std::memcpy(pending.data() + offset, data, static_cast<std::size_t>(length));
        offset += length; return length;
    }
    bool seek(qint64 value) {
        ++seekCalls;
        if (value < 0 || (failSeek && failSeek == seekCalls)) return false;
        offset = value; return true;
    }
    bool commit() {
        ++commitCalls;
        if (failCommit) return false;
        destination = pending; return true;
    }
};
namespace ksword::memory_backend {
struct ReadCall { MemoryAccessBackend backend; std::uint32_t pid; std::uint64_t address, length; bool strict; };
static std::vector<ReadCall> reads;
static std::deque<AccessOutcome> readScript;
static bool exitDuringRead, changeIdentityDuringRead;
AccessOutcome readVirtual(MemoryAccessBackend backend, const DdmaSession&, std::uint32_t pid,
    std::uint64_t address, std::uint64_t length, bool strict) {
    reads.push_back({backend, pid, address, length, strict});
    if (backend == MemoryAccessBackend::StandardDriver && length > KSWORD_ARK_MEMORY_READ_MAX_BYTES) {
        AccessOutcome rejected; rejected.failureText = QStringLiteral("driver read exceeds protocol limit"); return rejected;
    }
    if (exitDuringRead) processAlive = false;
    if (changeIdentityDuringRead) ++processCreation;
    if (!readScript.empty()) {
        auto result = readScript.front(); readScript.pop_front(); return result;
    }
    AccessOutcome result; result.ok = true; result.bytesDone = length;
    result.data = QByteArray(static_cast<qsizetype>(length), static_cast<char>(reads.size()));
    return result;
}
}
class MemoryDock {
public:
    static bool dumpProcessMemoryToFile(std::uint32_t, const std::shared_ptr<void>&,
        std::uint64_t, ksword::memory_backend::MemoryAccessBackend, const QString&,
        int, QString&);
};
'''

TESTS = r'''
using namespace ksword::memory_backend;
static unsigned checks, failures;
static void Check(bool condition, const char* label) {
    ++checks; if (!condition) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}
static MEMORY_BASIC_INFORMATION Region(std::uint64_t base, std::uint64_t length, bool readable = true) {
    MEMORY_BASIC_INFORMATION region; region.BaseAddress = reinterpret_cast<void*>(base);
    region.RegionSize = length; region.Protect = readable ? 4 : 0;
    region.State = MEM_COMMIT; region.Type = 0x20000; return region;
}
static void Reset() {
    processPid = 77; processCreation = 0x123456789ULL; processAlive = timesAvailable = true;
    processWow64 = false; wow64QueryAvailable = true; queryError = 5; shortQueryResponse = 0;
    identityChecks = exitOnIdentityCheck = queryCalls = 0; maximumAddress = 0x4000;
    queryScript = {Region(0x1000, 0x1000), Region(0x2000, 0x1000, false), Region(0x3000, 0x1000)};
    destination = {'o', 'l', 'd'}; pending.clear();
    writeCalls = seekCalls = commitCalls = failWrite = failSeek = 0;
    failOpen = failCommit = fallbackDisabled = false;
    reads.clear(); readScript.clear(); exitDuringRead = changeIdentityDuringRead = false;
}
static bool Dump(QString& error, MemoryAccessBackend backend = MemoryAccessBackend::Hvm,
    std::uint64_t creationTime = 0x123456789ULL) {
    const std::shared_ptr<void> handle(reinterpret_cast<void*>(1), [](void*) {});
    return MemoryDock::dumpProcessMemoryToFile(77, handle, creationTime, backend,
        QStringLiteral("target.kmdump"), 123, error);
}
static void CheckUncommitted(const QString& error, const char* label) {
    Check(destination == std::vector<char>({'o', 'l', 'd'}), label);
    Check(!error.isEmpty(), "failure carries a reason");
}
template<class T> static T At(std::size_t offset) {
    T value{}; std::memcpy(&value, destination.data() + offset, sizeof(value)); return value;
}
struct FileHeader {
    char magic[8]; std::uint32_t version, pid; std::uint64_t timestamp; std::uint32_t regions, reserved;
};
struct RegionHeader {
    std::uint64_t base, declared, dumped; std::uint32_t protect, state, type, reserved;
};
static void TestCompleteFormatAndBackend() {
    for (const auto backend : {MemoryAccessBackend::UserMode, MemoryAccessBackend::StandardDriver, MemoryAccessBackend::Hvm}) {
        Reset(); QString error; Check(Dump(error, backend), "complete dump succeeds for selected backend");
        Check(error.isEmpty() && commitCalls == 1 && fallbackDisabled, "complete dump commits once with direct file fallback disabled");
        Check(reads.size() == 2 && reads[0].backend == backend && reads[1].backend == backend,
              "every readable region uses the captured backend");
        Check(reads[0].strict && reads[1].strict && reads[0].pid == 77 && reads[0].address == 0x1000,
              "HVM reads are strict and preserve process address identity");
        const auto file = At<FileHeader>(0);
        Check(std::string(file.magic) == "KMDUMP1" && file.version == 1 && file.pid == 77
              && file.regions == 2 && file.reserved == 0, "KMDUMP1 header layout and region count preserved");
        const auto first = At<RegionHeader>(sizeof(FileHeader));
        const auto secondOffset = sizeof(FileHeader) + sizeof(RegionHeader) + 0x1000;
        const auto second = At<RegionHeader>(secondOffset);
        Check(first.base == 0x1000 && first.declared == 0x1000 && first.dumped == 0x1000
              && second.base == 0x3000 && second.dumped == 0x1000, "unreadable regions have no phantom record");
        Check(destination.size() == sizeof(FileHeader) + 2 * sizeof(RegionHeader) + 0x2000
              && destination[sizeof(FileHeader) + sizeof(RegionHeader)] == 1
              && destination[secondOffset + sizeof(RegionHeader)] == 2, "exact bytes and boundaries exported without zero fill");
    }
    for (const auto backend : {MemoryAccessBackend::UserMode, MemoryAccessBackend::StandardDriver, MemoryAccessBackend::Hvm}) {
        Reset(); maximumAddress = 0x1000 + KSWORD_ARK_MEMORY_READ_MAX_BYTES * 4 + 17;
        queryScript = {Region(0x1000, KSWORD_ARK_MEMORY_READ_MAX_BYTES * 4 + 17)};
        QString error; Check(Dump(error, backend), "multi-chunk region succeeds within each backend limit");
        Check(reads.size() == 5 && reads[0].length == KSWORD_ARK_MEMORY_READ_MAX_BYTES && reads[4].length == 17
              && reads[4].address == 0x1000 + KSWORD_ARK_MEMORY_READ_MAX_BYTES * 4,
              "chunk cursor covers exact contiguous range within R0 protocol limit");
    }
}
static void TestReadFailures() {
    for (int mode = 0; mode < 5; ++mode) {
        Reset(); AccessOutcome failure;
        if (mode == 1) { failure.ok = true; failure.partial = true; failure.bytesDone = 3; failure.data = QByteArray(3, 'p'); }
        if (mode == 2) { failure.ok = true; }
        if (mode == 3) { failure.ok = true; failure.bytesDone = 4096; failure.data = QByteArray(3, 'p'); }
        if (mode == 4) { failure.ok = true; failure.bytesDone = 3; failure.data = QByteArray(4096, 'p'); }
        readScript.push_back(failure); QString error;
        Check(!Dump(error), "failed partial zero or inconsistent read cannot succeed");
        CheckUncommitted(error, "bad read does not replace existing file");
        Check(commitCalls == 0 && reads.size() == 1 && reads[0].strict, "bad strict read stops before further regions or commit");
    }
    Reset(); AccessOutcome prefix; prefix.ok = true; prefix.bytesDone = 4096; prefix.data = QByteArray(4096, 'p');
    readScript.push_back(prefix); readScript.push_back({}); QString error;
    Check(!Dump(error), "later region failure cannot convert prefix into complete dump");
    CheckUncommitted(error, "later failure leaves destination untouched");
}
static void TestWow64AddressSpaceEnd() {
    const auto boundaryScript = [](std::uint64_t boundary = 0x100000000ULL) {
        processWow64 = true; queryError = ERROR_INVALID_PARAMETER;
        maximumAddress = 0x100010000ULL;
        queryScript = {Region(0x1000, 0x1000), Region(0x2000, boundary - 0x2000, false)};
    };
    for (const auto backend : {MemoryAccessBackend::UserMode, MemoryAccessBackend::StandardDriver, MemoryAccessBackend::Hvm}) {
        for (const std::uint64_t boundary : {0x7FFF0000ULL, 0x80000000ULL, 0xFFFF0000ULL, 0x100000000ULL}) {
            Reset(); boundaryScript(boundary); QString error;
            Check(Dump(error, backend), "confirmed WOW64 2 or 4 GiB end, with final reserved 64 KiB, completes for every backend");
            Check(error.isEmpty() && commitCalls == 1 && queryCalls == 3 && reads.size() == 1,
                  "WOW64 end publishes complete readable prefix after continuous low-address traversal");
            Check(At<FileHeader>(0).regions == 1, "WOW64 end writes the exact completed region count");
        }
    }
    for (int mode = 0; mode < 6; ++mode) {
        Reset(); boundaryScript();
        if (mode == 0) processWow64 = false;
        if (mode == 1) wow64QueryAvailable = false;
        if (mode == 2) queryError = 5;
        if (mode == 3) queryScript = {Region(0x1000, 0x1000)};
        if (mode == 4) shortQueryResponse = sizeof(MEMORY_BASIC_INFORMATION) - 1;
        if (mode == 5) boundaryScript(0x7FFE0000ULL);
        QString error;
        Check(!Dump(error), "native unknown other-error low-address or short query failure cannot become normal end");
        CheckUncommitted(error, "unproven address-space end leaves existing dump untouched");
        Check(commitCalls == 0, "unproven address-space end never commits");
    }
    Reset(); boundaryScript(); exitOnIdentityCheck = 7; QString error;
    Check(!Dump(error), "WOW64 end still rechecks live process identity before commit");
    CheckUncommitted(error, "process exit at WOW64 end cannot publish stale memory");
    Check(commitCalls == 0, "stale target at normal WOW64 end never commits");
    Reset(); boundaryScript();
    queryError = 5;
    queryScript.push_back(Region(0x100000000ULL, 0x10000));
    QString highMemoryError;
    Check(Dump(highMemoryError) && reads.size() == 2 && reads[1].address == 0x100000000ULL,
          "successful native WOW64 mappings above 4 GiB remain included");
}
static void TestIdentityFailures() {
    for (int mode = 0; mode < 7; ++mode) {
        Reset();
        if (mode == 0) processAlive = false;
        if (mode == 1) processPid = 88;
        if (mode == 2) ++processCreation;
        if (mode == 3) timesAvailable = false;
        if (mode == 4) exitDuringRead = true;
        if (mode == 5) changeIdentityDuringRead = true;
        if (mode == 6) exitOnIdentityCheck = 9; // final check just before file-header commit
        QString error; Check(!Dump(error), "stale or exited target is refused");
        CheckUncommitted(error, "target identity change does not publish dump");
        Check(commitCalls == 0, "identity failure never reaches commit");
        if (mode < 4) Check(reads.empty(), "initial identity failure performs no target reads");
    }
    Reset(); QString error;
    const std::shared_ptr<void> empty;
    Check(!MemoryDock::dumpProcessMemoryToFile(77, empty, processCreation, MemoryAccessBackend::Hvm,
        QStringLiteral("target"), 123, error), "missing target lease is refused");
}
static void TestQueryAndFileFailures() {
    for (int mode = 0; mode < 5; ++mode) {
        Reset();
        if (mode == 0) queryScript.clear();
        if (mode == 1) queryScript = {Region(0x1000, 0)};
        if (mode == 2) queryScript = {Region(0x2000, 4096)};
        if (mode == 3) queryScript = {Region(0, 100)};
        if (mode == 4) queryScript = {Region(0x1000, (std::numeric_limits<std::uint64_t>::max)())};
        QString error; Check(!Dump(error), "failed malformed or incomplete query cannot report success");
        CheckUncommitted(error, "query failure preserves target file");
    }
    Reset(); queryScript = {Region(0x1000, 0x3000, false)}; QString error;
    Check(!Dump(error) && reads.empty() && commitCalls == 0, "zero-readable-region dump is never published");
    for (unsigned failedWrite = 1; failedWrite <= 8; ++failedWrite) {
        Reset(); failWrite = failedWrite; QString writeError;
        Check(!Dump(writeError), "every header data or header-rewrite failure aborts");
        CheckUncommitted(writeError, "short file write preserves existing destination");
        Check(commitCalls == 0, "short write never commits");
    }
    for (unsigned failedSeek = 1; failedSeek <= 5; ++failedSeek) {
        Reset(); failSeek = failedSeek; QString seekError;
        Check(!Dump(seekError), "every seek failure aborts");
        CheckUncommitted(seekError, "seek failure preserves destination");
    }
    Reset(); failOpen = true; QString openError;
    Check(!Dump(openError) && reads.empty(), "open failure performs no memory reads");
    CheckUncommitted(openError, "open failure preserves existing output");
    Reset(); failCommit = true; QString commitError;
    Check(!Dump(commitError) && commitCalls == 1, "failed atomic commit is not success");
    CheckUncommitted(commitError, "commit failure preserves old destination");
    Reset(); QString backendError;
    Check(!Dump(backendError, MemoryAccessBackend::Ddma) && reads.empty() && writeCalls == 0,
          "DDMA export is rejected before memory or file I/O");
}
int main() {
    TestCompleteFormatAndBackend(); TestReadFailures(); TestWow64AddressSpaceEnd(); TestIdentityFailures(); TestQueryAndFileFailures();
    std::cout << "Production process memory dump: " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
'''


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=PRODUCTION)
    parser.add_argument("--compiler", default=os.environ.get("CXX"))
    parser.add_argument("--work-dir", type=Path, default=ROOT / "work/hvm-memory-tests")
    args = parser.parse_args()
    compiler = args.compiler or shutil.which("g++") or shutil.which("clang++")
    if not compiler:
        parser.error("G++ or Clang++ is required")
    source = args.source.read_text(encoding="utf-8-sig")
    method = extract_method(source, "MemoryDock::dumpProcessMemoryToFile")
    args.work_dir.mkdir(parents=True, exist_ok=True)
    work_dir = args.work_dir.resolve()
    scratch = work_dir / f"process-dump-{uuid.uuid4().hex}"
    scratch.mkdir()
    try:
        (scratch / "qt_shim.h").write_text(QT_SHIM, encoding="utf-8")
        for name in ("QByteArray", "QString"):
            (scratch / name).write_text('#include "qt_shim.h"\n', encoding="utf-8")
        cpp_path = scratch / "production_process_dump.cpp"
        binary = scratch / ("process-dump.exe" if os.name == "nt" else "process-dump")
        cpp_path.write_text(PRELUDE + method + TESTS, encoding="utf-8")
        subprocess.run([
            compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
            "-I", str(scratch), "-I", str(PRODUCTION.parent), "-I", str(ROOT / "shared/driver"),
            str(cpp_path), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
    finally:
        if scratch.resolve().parent != work_dir:
            raise RuntimeError("Refusing to clean outside the test work directory")
        shutil.rmtree(scratch)


if __name__ == "__main__":
    main()
