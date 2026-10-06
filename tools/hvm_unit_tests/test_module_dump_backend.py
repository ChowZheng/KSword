"""Execute production DriverDock module-dump routing and commit gates.

ReadChunk, R0 framing validation, PE validation and ToFile are extracted
verbatim. Only Qt path objects, backend calls, loaded-module identity and the
file boundary are replaced. These host checks do not prove actual Windows
atomic rename, loaded-driver identity, HVM access or Qt GUI behavior.
Run: python tools/hvm_unit_tests/test_module_dump_backend.py
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import uuid

from test_memory_backend import QT_SHIM


ROOT = Path(__file__).resolve().parents[2]
PRODUCTION = ROOT / "Ksword5.1/Ksword5.1/DriverDock/DriverDock.Operation.cpp"
CLIENT_TYPES = ROOT / "Ksword5.1/Ksword5.1/ArkDriverClient/ArkDriverTypes.h"
FILE_HEADER = ROOT / "Ksword5.1/Ksword5.1/DriverDock/DriverDock.ModuleDumpFile.h"


def extract_block(source: str, declaration: str) -> str:
    masked = re.sub(
        r'R"(?P<raw_delimiter>[^ ()\\\t\r\n]{0,16})\(.*?\)(?P=raw_delimiter)"|'
        r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
        lambda match: " " * len(match.group()), source, flags=re.DOTALL,
    )
    match = re.search(declaration, masked)
    if match is None:
        raise ValueError(f"Production declaration not found: {declaration}")
    opening = masked.index("{", match.end())
    depth, cursor = 1, opening + 1
    while depth:
        depth += (masked[cursor] == "{") - (masked[cursor] == "}")
        cursor += 1
    if masked[cursor:cursor + 1] == ";":
        cursor += 1
    return source[match.start():cursor]


PRELUDE = r'''
#include "MemoryAccessBackend.h"
#include <cstddef>
#include <cstring>
#include <iostream>
#include <limits>
#define _KERNEL_MODE 1
#include "KswordArkMemoryIoctl.h"
constexpr unsigned long ERROR_SUCCESS = 0;
constexpr unsigned long ERROR_FILE_EXISTS = 80;
constexpr unsigned long ERROR_PATH_NOT_FOUND = 3;
unsigned long GetLastError() { return 123; }
static bool targetExists;
static bool directoryExists;
static bool unsafePath;
class QDir { public: bool exists() const { return directoryExists; } };
class QFileInfo {
    QString path;
public:
    explicit QFileInfo(const QString& value) : path(value) {}
    QString absoluteFilePath() const { return path; }
    QDir dir() const { return {}; }
    static bool exists(const QString&) { return targetExists; }
};
static bool driverOperationHasUnsafeWin32PathSyntax(const QString&, bool) { return unsafePath; }
static std::uint32_t imageSize;
static std::vector<std::uint8_t> image;
static constexpr std::uint64_t moduleBase = 0xFFFFF80000400000ULL;
static unsigned identityCalls;
static unsigned identityFailure;
static std::vector<std::uint32_t> identityExpectedSizes;
static unsigned fileCreates;
static unsigned fileWrites;
static unsigned fileCommits;
static unsigned fileDiscards;
static std::uint64_t commitSize;
static std::vector<std::uint8_t> writtenData;
'''


DRIVER_BOUNDARY = r'''
class DriverHandle { public: bool isValid() const { return true; } };
struct ReadCall { std::uint32_t pid; std::uint64_t address; std::uint32_t length; unsigned long flags; DriverHandle* handle; };
static std::vector<ReadCall> r0Calls;
static std::vector<VirtualMemoryReadResult> r0Script;
static VirtualMemoryReadResult CompleteRead(std::uint64_t address, std::uint32_t length) {
    VirtualMemoryReadResult result;
    result.io.ok = true;
    result.io.bytesReturned = offsetof(KSWORD_ARK_READ_VIRTUAL_MEMORY_RESPONSE, data) + length;
    result.version = KSWORD_ARK_MEMORY_PROTOCOL_VERSION;
    result.headerSize = offsetof(KSWORD_ARK_READ_VIRTUAL_MEMORY_RESPONSE, data);
    result.fieldFlags = KSWORD_ARK_MEMORY_FIELD_READ_DATA_PRESENT | KSWORD_ARK_MEMORY_FIELD_ADDRESS_KERNEL_RANGE;
    result.readStatus = KSWORD_ARK_MEMORY_READ_STATUS_OK;
    result.source = KSWORD_ARK_MEMORY_SOURCE_R0_MM_COPY_KERNEL_VIRTUAL;
    result.requestedBaseAddress = address;
    result.requestedBytes = result.bytesRead = length;
    result.maxBytesPerRequest = KSWORD_ARK_MEMORY_READ_MAX_BYTES;
    const auto offset = static_cast<std::size_t>(address - moduleBase);
    result.data.assign(image.begin() + offset, image.begin() + offset + length);
    return result;
}
class DriverClient {
public:
    DriverHandle open() const { return {}; }
    VirtualMemoryReadResult readVirtualMemory(std::uint32_t pid, std::uint64_t address,
        std::uint32_t length, unsigned long flags, DriverHandle* handle) const {
        r0Calls.push_back({pid, address, length, flags, handle});
        return r0Calls.size() <= r0Script.size() ? r0Script[r0Calls.size() - 1] : CompleteRead(address, length);
    }
};
} // namespace ksword::ark
namespace ksword::memory_backend {
struct HvmCall { MemoryAccessBackend backend; std::uint32_t pid; std::uint64_t address; std::uint64_t length; bool strict; };
static std::vector<HvmCall> hvmCalls;
static std::vector<AccessOutcome> hvmScript;
static AccessOutcome CompleteRead(std::uint64_t address, std::uint64_t length) {
    AccessOutcome result; result.ok = true; result.bytesDone = length;
    const auto offset = static_cast<std::size_t>(address - moduleBase);
    result.data = QByteArray(reinterpret_cast<const char*>(image.data() + offset), static_cast<qsizetype>(length));
    return result;
}
AccessOutcome readVirtual(MemoryAccessBackend backend, const DdmaSession&, std::uint32_t pid,
    std::uint64_t address, std::uint64_t length, bool strict) {
    hvmCalls.push_back({backend, pid, address, length, strict});
    return hvmCalls.size() <= hvmScript.size() ? hvmScript[hvmCalls.size() - 1] : CompleteRead(address, length);
}
} // namespace ksword::memory_backend
static bool driverModuleDumpQueryIdentity(const ksword::ark::DriverClient&, const QString&,
    std::uint64_t, std::uint32_t expectedSize, std::uint32_t& sizeOut, QString& detailOut) {
    ++identityCalls; identityExpectedSizes.push_back(expectedSize);
    if (identityCalls == identityFailure) { detailOut = QString("identity failed"); return false; }
    sizeOut = imageSize; return true;
}
'''


FILE_BOUNDARY = r'''
class DriverModuleDumpFile {
    bool created = false;
    bool committed = false;
public:
    ~DriverModuleDumpFile() { if (created && !committed) ++fileDiscards; }
    bool create(const QString&) { ++fileCreates; created = true; return true; }
    bool write(const std::uint8_t* bytes, std::size_t length) {
        ++fileWrites; writtenData.insert(writtenData.end(), bytes, bytes + length); return true;
    }
    bool commit(std::uint64_t size) { ++fileCommits; commitSize = size; committed = true; return true; }
    DriverModuleDumpFileError error() const { return DriverModuleDumpFileError::None; }
    unsigned long win32Error() const { return 0; }
    QString technicalDetail() const { return {}; }
};
'''


TESTS = r'''
static unsigned checks;
static unsigned failures;
static void Check(bool condition, const char* text) {
    ++checks; if (!condition) { ++failures; std::cerr << "FAIL: " << text << '\n'; }
}
template<class T> static void Put(std::size_t offset, T value) { std::memcpy(image.data() + offset, &value, sizeof(T)); }
static void Reset() {
    targetExists = unsafePath = false; directoryExists = true;
    identityCalls = identityFailure = fileCreates = fileWrites = fileCommits = fileDiscards = 0;
    commitSize = 0; writtenData.clear(); identityExpectedSizes.clear();
    ksword::ark::r0Calls.clear(); ksword::ark::r0Script.clear();
    ksword::memory_backend::hvmCalls.clear(); ksword::memory_backend::hvmScript.clear();
    imageSize = kDriverModuleDumpHeaderProbeBytes + KSWORD_ARK_MEMORY_READ_MAX_BYTES + 0x2000;
    image.assign(imageSize, 0xAB);
    Put<std::uint16_t>(0, 0x5A4D); Put<std::uint32_t>(0x3C, 0x80);
    Put<std::uint32_t>(0x80, 0x00004550); Put<std::uint16_t>(0x84, 0x8664);
    Put<std::uint16_t>(0x86, 1); Put<std::uint16_t>(0x94, 240);
    Put<std::uint16_t>(0x98, 0x20B); Put<std::uint32_t>(0xB8, 0x1000);
    Put<std::uint32_t>(0xD0, imageSize); Put<std::uint32_t>(0xD4, 0x200);
}
static void TestHvmChunkRouting() {
    using namespace ksword::memory_backend;
    ksword::ark::DriverClient client; ksword::ark::DriverHandle handle;
    Reset(); std::vector<std::uint8_t> bytes{1, 2, 3}; QString detail; bool zero = true;
    Check(driverModuleDumpReadChunk(client, handle, true, moduleBase + 0x1000, 17, bytes, detail, zero),
          "complete HVM chunk is accepted");
    Check(bytes == std::vector<std::uint8_t>(17, 0xAB) && !zero && ksword::ark::r0Calls.empty(),
          "HVM chunk exposes actual bytes and no R0 zero fill");
    Check(hvmCalls.size() == 1 && hvmCalls[0].backend == MemoryAccessBackend::Hvm && hvmCalls[0].strict &&
          hvmCalls[0].pid == 0 && hvmCalls[0].address == moduleBase + 0x1000 && hvmCalls[0].length == 17,
          "HVM module chunk always requests strict window, PID zero and exact address range");
    for (unsigned mutation = 0; mutation < 6; ++mutation) {
        Reset(); auto response = CompleteRead(moduleBase + 0x1000, 17);
        switch (mutation) {
        case 0: response.ok = false; response.failureText = QString("transport failed"); break;
        case 1: response.partial = true; break;
        case 2: response.bytesDone = 16; break;
        case 3: response.bytesDone = 18; break;
        case 4: response.data = QByteArray(16, 'x'); break;
        case 5: response.data = QByteArray(18, 'x'); break;
        }
        hvmScript.push_back(response); bytes.assign(3, 0xFF); detail = {}; zero = true;
        Check(!driverModuleDumpReadChunk(client, handle, true, moduleBase + 0x1000, 17, bytes, detail, zero),
              "HVM failure, partial and completion/data length mismatch are rejected");
        Check(bytes.empty() && !zero && !detail.isEmpty() && hvmCalls.size() == 1 && ksword::ark::r0Calls.empty(),
              "rejected HVM chunk clears stale output and never retries via R0");
        if (mutation == 0) Check(detail.value == "transport failed", "HVM transport failure retains diagnostic");
    }
}
static void TestR0ChunkRoutingAndValidation() {
    using namespace ksword::ark;
    DriverClient client; DriverHandle handle;
    Reset(); std::vector<std::uint8_t> bytes{1}; QString detail; bool zero = true;
    Check(driverModuleDumpReadChunk(client, handle, false, moduleBase + 0x1000, 17, bytes, detail, zero) &&
          bytes == std::vector<std::uint8_t>(17, 0xAB) && !zero,
          "R0 complete framed response remains accepted");
    Check(r0Calls.size() == 1 && r0Calls[0].handle == &handle && r0Calls[0].pid == 0 &&
          r0Calls[0].address == moduleBase + 0x1000 && r0Calls[0].length == 17 &&
          r0Calls[0].flags == (KSWORD_ARK_MEMORY_READ_FLAG_KERNEL_ADDRESS | KSWORD_ARK_MEMORY_READ_FLAG_ZERO_FILL_UNREADABLE) &&
          ksword::memory_backend::hvmCalls.empty(),
          "R0 path retains caller handle, kernel PID and existing zero-fill request flags");
    for (const auto status : {KSWORD_ARK_MEMORY_READ_STATUS_PARTIAL_COPY, KSWORD_ARK_MEMORY_READ_STATUS_ZERO_FILLED}) {
        Reset(); auto response = CompleteRead(moduleBase + 0x1000, 17);
        response.readStatus = status; response.copyStatus = -1;
        response.fieldFlags |= KSWORD_ARK_MEMORY_FIELD_PARTIAL_COPY | KSWORD_ARK_MEMORY_FIELD_ZERO_FILLED_UNREADABLE;
        response.data.assign(17, 0); r0Script.push_back(response); zero = false;
        Check(driverModuleDumpReadChunk(client, handle, false, moduleBase + 0x1000, 17, bytes, detail, zero) &&
              zero && bytes == std::vector<std::uint8_t>(17, 0),
              "R0 explicitly framed zero-filled partial/full unreadable responses stay supported");
    }
    for (unsigned mutation = 0; mutation < 7; ++mutation) {
        Reset(); auto response = CompleteRead(moduleBase + 0x1000, 17);
        switch (mutation) {
        case 0: response.io.ok = false; break;
        case 1: response.headerSize = 0; break;
        case 2: response.processId = 1; break;
        case 3: ++response.requestedBaseAddress; break;
        case 4: --response.bytesRead; break;
        case 5: response.data.pop_back(); break;
        case 6: response.readStatus = KSWORD_ARK_MEMORY_READ_STATUS_PARTIAL_COPY; break;
        }
        r0Script.push_back(response); bytes.assign(3, 0xFF); detail = {}; zero = true;
        Check(!driverModuleDumpReadChunk(client, handle, false, moduleBase + 0x1000, 17, bytes, detail, zero) &&
              bytes.empty() && !zero && !detail.isEmpty(),
              "R0 transport/framing/identity/count/unmarked partial failures clear output");
    }
}
static DriverModuleDumpResult Dump(bool hvm) {
    return driverModuleDumpToFile(QString("\\SystemRoot\\test.sys"), moduleBase, QString("C:/evidence/new.bin"), hvm);
}
static void TestCommitGates() {
    for (const bool hvm : {false, true}) {
        Reset(); auto result = Dump(hvm);
        Check(result.ok && result.error == DriverModuleDumpError::None && result.moduleSize == imageSize &&
              result.zeroFilledChunkCount == 0 && fileCommits == 1 && commitSize == imageSize && writtenData == image &&
              fileDiscards == 0,
              "validated module dump commits the exact authentic full image");
        Check(identityCalls == 2 && identityExpectedSizes == std::vector<std::uint32_t>({0, imageSize}) &&
              fileCreates == 1 && fileWrites == 3,
              "module identity is rechecked with authoritative size before commit");
        if (hvm) {
            const auto& calls = ksword::memory_backend::hvmCalls;
            Check(calls.size() == 3 && calls[0].strict && calls[1].strict && calls[2].strict &&
                  calls[0].pid == 0 && calls[1].pid == 0 && calls[2].pid == 0 && ksword::ark::r0Calls.empty(),
                  "every HVM image chunk uses strict kernel reads through commit");
        } else {
            const auto& calls = ksword::ark::r0Calls;
            Check(calls.size() == 3 && calls[0].handle != nullptr && calls[0].handle == calls[1].handle &&
                  calls[0].handle == calls[2].handle && ksword::memory_backend::hvmCalls.empty(),
                  "R0 module dump reuses one control handle across every chunk");
        }
        Reset(); identityFailure = 1; result = Dump(hvm);
        Check(!result.ok && result.error == DriverModuleDumpError::IdentityCheck && fileCreates == 0 && fileCommits == 0 &&
              ksword::ark::r0Calls.empty() && ksword::memory_backend::hvmCalls.empty(),
              "initial identity rejection prevents memory reads and any file creation");
        Reset(); identityFailure = 2; result = Dump(hvm);
        Check(!result.ok && result.error == DriverModuleDumpError::IdentityCheck && fileCommits == 0 &&
              fileWrites == 3 && fileDiscards == 1,
              "final identity rejection cancels complete temporary image without committing");
        Reset(); Put<std::uint16_t>(0, 0); result = Dump(hvm);
        Check(!result.ok && result.error == DriverModuleDumpError::PeValidation && fileCreates == 0 && fileCommits == 0,
              "invalid PE header cannot create or commit a module dump");
        Reset(); Put<std::uint32_t>(0xD0, imageSize - 0x1000); result = Dump(hvm);
        Check(!result.ok && result.error == DriverModuleDumpError::PeValidation && fileCreates == 0 && fileCommits == 0,
              "PE SizeOfImage disagreement with authoritative identity prevents commit");
        for (const unsigned failChunk : {1U, 2U}) {
            Reset();
            if (hvm) {
                auto good = ksword::memory_backend::CompleteRead(moduleBase, 0x10000);
                if (failChunk == 2) ksword::memory_backend::hvmScript.push_back(good);
                auto failed = good; failed.partial = true;
                ksword::memory_backend::hvmScript.push_back(failed);
            } else {
                auto good = ksword::ark::CompleteRead(moduleBase, 0x10000);
                if (failChunk == 2) ksword::ark::r0Script.push_back(good);
                auto failed = good; failed.io.ok = false; ksword::ark::r0Script.push_back(failed);
            }
            result = Dump(hvm);
            Check(!result.ok && result.error == DriverModuleDumpError::MemoryRead && fileCommits == 0 &&
                  identityCalls == 1 && fileWrites == failChunk - 1 && fileDiscards == failChunk - 1,
                  "header/body read rejection stops image construction and prevents commit");
        }
    }
    Reset();
    ksword::ark::r0Script.push_back(ksword::ark::CompleteRead(moduleBase, kDriverModuleDumpHeaderProbeBytes));
    auto zeroFilled = ksword::ark::CompleteRead(
        moduleBase + kDriverModuleDumpHeaderProbeBytes, KSWORD_ARK_MEMORY_READ_MAX_BYTES);
    zeroFilled.readStatus = KSWORD_ARK_MEMORY_READ_STATUS_PARTIAL_COPY;
    zeroFilled.copyStatus = -1;
    zeroFilled.fieldFlags |= KSWORD_ARK_MEMORY_FIELD_PARTIAL_COPY | KSWORD_ARK_MEMORY_FIELD_ZERO_FILLED_UNREADABLE;
    zeroFilled.data.assign(KSWORD_ARK_MEMORY_READ_MAX_BYTES, 0);
    ksword::ark::r0Script.push_back(zeroFilled);
    const auto result = Dump(false);
    auto expected = image;
    std::fill(expected.begin() + kDriverModuleDumpHeaderProbeBytes,
              expected.begin() + kDriverModuleDumpHeaderProbeBytes + KSWORD_ARK_MEMORY_READ_MAX_BYTES, 0);
    Check(result.ok && result.zeroFilledChunkCount == 1 && fileCommits == 1 && writtenData == expected,
          "R0 dump preserves and counts explicitly zero-filled body chunks through commit");
}
int main() {
    TestHvmChunkRouting(); TestR0ChunkRoutingAndValidation(); TestCommitGates();
    std::cout << "Production module dump backend: " << checks << " checks, " << failures << " failures\n";
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
    client_types = CLIENT_TYPES.read_text(encoding="utf-8-sig")
    file_header = FILE_HEADER.read_text(encoding="utf-8-sig")
    functions = "\n".join(extract_block(source, declaration) for declaration in (
        r"template\s*<typename\s+ValueType>\s+bool\s+driverModuleDumpReadValue\s*\(",
        r"bool\s+driverModuleDumpValidateRead\s*\(",
        r"bool\s+driverModuleDumpValidatePeImage\s*\(",
        r"bool\s+driverModuleDumpReadChunk\s*\(",
        r"DriverModuleDumpResult\s+driverModuleDumpToFile\s*\(",
    ))
    production_types = "\n".join(extract_block(source, declaration) for declaration in (
        r"enum\s+class\s+DriverModuleDumpError\s*:\s*std::uint32_t",
        r"struct\s+DriverModuleDumpResult\b",
    ))
    constants = "\n".join(re.findall(
        r"constexpr\s+std::uint32_t\s+kDriverModuleDump(?:HeaderProbeBytes|HardMaxBytes)\s*=.*?;", source,
    ))
    real_client_types = "\n".join(extract_block(client_types, rf"struct\s+{name}\b")
                                  for name in ("IoResult", "VirtualMemoryReadResult"))
    file_error = extract_block(file_header, r"enum\s+class\s+DriverModuleDumpFileError\s*:\s*std::uint32_t")
    args.work_dir.mkdir(parents=True, exist_ok=True)
    work_dir = args.work_dir.resolve()
    scratch = work_dir / f"module-dump-{uuid.uuid4().hex}"
    scratch.mkdir()
    try:
        qt = QT_SHIM.replace(
            "void clear() { value.clear(); }",
            "static QString fromLatin1(const char* text) { return QString(text); }\n"
            "    void prepend(const QString& other) { value = other.value + value; }\n"
            "    void clear() { value.clear(); }",
        )
        (scratch / "qt_shim.h").write_text(qt, encoding="utf-8")
        for name in ("QByteArray", "QString"):
            (scratch / name).write_text('#include "qt_shim.h"\n', encoding="utf-8")
        cpp_path = scratch / "production_module_dump.cpp"
        binary = scratch / ("module-dump.exe" if os.name == "nt" else "module-dump")
        cpp_path.write_text(
            PRELUDE + "\nnamespace ksword::ark {\n" + real_client_types + DRIVER_BOUNDARY +
            constants + "\n" + production_types + "\n" + file_error + FILE_BOUNDARY + functions + TESTS,
            encoding="utf-8",
        )
        subprocess.run([
            compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
            "-I", str(scratch), "-I", str(ROOT / "shared/driver"),
            "-I", str(ROOT / "Ksword5.1/Ksword5.1/MemoryDock"), str(cpp_path), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
    finally:
        if scratch.resolve().parent != work_dir:
            raise RuntimeError("Refusing to clean outside the test work directory")
        shutil.rmtree(scratch)


if __name__ == "__main__":
    main()
