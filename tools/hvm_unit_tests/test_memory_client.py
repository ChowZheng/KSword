"""Compile and run the real DriverClient::hvmMemory with a fake IOCTL boundary.

The method, error classifier and result structs are extracted unchanged from
production files. Requests/responses use the shared protocol header. Invalid
payload pointers prove oversized writes are rejected before payload access.
Run: python tools/hvm_unit_tests/test_memory_client.py
Requires G++ or Clang++; no loaded driver or Qt is needed.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import uuid

from test_memory_transfer import extract_function


ROOT = Path(__file__).resolve().parents[2]
PRODUCTION = ROOT / "Ksword5.1/Ksword5.1/ArkDriverClient/ArkDriverHvm.cpp"
TYPES = PRODUCTION.with_name("ArkDriverTypes.h")


def extract_struct(source: str, name: str) -> str:
    match = re.search(rf"struct\s+{re.escape(name)}\s*\{{", source)
    if match is None:
        raise ValueError(f"Production struct missing: {name}")
    # These result structs contain fields and aggregate initializers only.
    cursor = source.index("{", match.start()) + 1
    depth = 1
    while depth:
        depth += (source[cursor] == "{") - (source[cursor] == "}")
        cursor += 1
    return source[match.start():source.index(";", cursor) + 1]


PRELUDE = r'''
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>
#define _KERNEL_MODE 1
#include "KswordArkHvmIoctl.h"
#define ERROR_SUCCESS 0UL
#define ERROR_INVALID_FUNCTION 1UL
#define ERROR_NOT_SUPPORTED 50UL
#define ERROR_INVALID_PARAMETER 87UL
namespace ksword::ark {
'''

BOUNDARY = r'''
struct DriverHandle {};
static unsigned ioCalls;
static bool failTransport;
static KSWORD_ARK_HVM_MEMORY_REQUEST captured;
static DriverHandle* capturedHandle;
static unsigned long capturedCode;
class DriverClient {
public:
    HvmMemoryResult hvmMemory(unsigned long, std::uint64_t, std::uint64_t,
        unsigned long, const unsigned char*, bool, bool, unsigned long,
        DriverHandle*) const;
    IoResult deviceIoControl(unsigned long code, void* input,
        unsigned long inputBytes, void* output, unsigned long outputBytes,
        DriverHandle* existingHandle) const {
        ++ioCalls;
        IoResult result;
        if (inputBytes != sizeof(captured) || outputBytes != sizeof(KSWORD_ARK_HVM_MEMORY_RESPONSE)) {
            result.win32Error = ERROR_INVALID_PARAMETER;
            return result;
        }
        captured = *static_cast<KSWORD_ARK_HVM_MEMORY_REQUEST*>(input);
        capturedHandle = existingHandle;
        capturedCode = code;
        if (failTransport) {
            result.win32Error = ERROR_NOT_SUPPORTED;
            return result;
        }
        auto& response = *static_cast<KSWORD_ARK_HVM_MEMORY_RESPONSE*>(output);
        response.version = KSWORD_ARK_HVM_MEMORY_PROTOCOL_VERSION;
        response.size = sizeof(response);
        response.status = KSWORD_ARK_HVM_MEMORY_STATUS_OK;
        response.bytesTransferred = captured.length;
        result.ok = true;
        result.bytesReturned = sizeof(response);
        return result;
    }
};
'''

TESTS = r'''
} // namespace ksword::ark
using namespace ksword::ark;
static unsigned checks;
static unsigned failures;
static void Check(bool condition, const char* label) {
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}
static void Reset() {
    ioCalls = 0; failTransport = false; captured = {};
    capturedHandle = nullptr; capturedCode = 0;
}
static void TestOversizedRequests() {
    const DriverClient client;
    // This pointer must never be dereferenced; absence of a crash is part of
    // the regression for oversized write requests.
    const auto* poison = reinterpret_cast<const unsigned char*>(std::uintptr_t(1));
    for (const auto operation : {
        KSWORD_ARK_HVM_MEMORY_OP_READ_PHYSICAL, KSWORD_ARK_HVM_MEMORY_OP_WRITE_PHYSICAL,
        KSWORD_ARK_HVM_MEMORY_OP_READ_VIRTUAL, KSWORD_ARK_HVM_MEMORY_OP_WRITE_VIRTUAL}) {
        for (const auto length : {KSWORD_ARK_HVM_MEMORY_MAX_BYTES + 1UL,
                                 (std::numeric_limits<unsigned long>::max)()}) {
            Reset();
            const auto result = client.hvmMemory(operation, 0xFEDCBA9876543210ULL,
                0x12345000ULL, length, poison, true, true, 77UL, nullptr);
            Check(!result.io.ok && result.io.win32Error == ERROR_INVALID_PARAMETER,
                  "oversized read/write reports invalid parameter");
            Check(result.response.status == KSWORD_ARK_HVM_MEMORY_STATUS_INVALID_REQUEST &&
                  result.response.bytesTransferred == 0UL,
                  "oversized request never reports a truncated transfer");
            Check(ioCalls == 0U, "oversized request rejected before any IOCTL");
            Check(!result.io.message.empty() && !result.unsupported,
                  "length rejection carries an actionable local failure");
        }
    }
}
static void TestMaximumRequests() {
    const DriverClient client;
    DriverHandle handle;
    std::vector<unsigned char> payload(KSWORD_ARK_HVM_MEMORY_MAX_BYTES);
    for (std::size_t index = 0; index < payload.size(); ++index)
        payload[index] = static_cast<unsigned char>(index % 251U);
    unsigned char zero[KSWORD_ARK_HVM_MEMORY_MAX_BYTES] = {};
    for (const auto operation : {
        KSWORD_ARK_HVM_MEMORY_OP_READ_PHYSICAL, KSWORD_ARK_HVM_MEMORY_OP_WRITE_PHYSICAL,
        KSWORD_ARK_HVM_MEMORY_OP_READ_VIRTUAL, KSWORD_ARK_HVM_MEMORY_OP_WRITE_VIRTUAL}) {
        const bool write = operation == KSWORD_ARK_HVM_MEMORY_OP_WRITE_PHYSICAL ||
                           operation == KSWORD_ARK_HVM_MEMORY_OP_WRITE_VIRTUAL;
        Reset();
        const auto result = client.hvmMemory(operation, 0xFEDCBA9876543210ULL,
            0x12345000ULL, KSWORD_ARK_HVM_MEMORY_MAX_BYTES, payload.data(),
            write, true, 77UL, &handle);
        Check(result.io.ok && result.response.bytesTransferred == KSWORD_ARK_HVM_MEMORY_MAX_BYTES &&
              ioCalls == 1U, "exact maximum transfer remains accepted");
        Check(captured.address == 0xFEDCBA9876543210ULL &&
              captured.directoryBase == 0x12345000ULL && captured.processId == 77UL &&
              captured.length == KSWORD_ARK_HVM_MEMORY_MAX_BYTES && captured.operation == operation,
              "legal request preserves full address, CR3, PID, operation and length");
        Check(capturedHandle == &handle && capturedCode == IOCTL_KSWORD_ARK_HVM_MEMORY,
              "legal request retains existing handle and exact IOCTL");
        Check(captured.version == KSWORD_ARK_HVM_MEMORY_PROTOCOL_VERSION &&
              captured.size == sizeof(captured) && captured.reserved0 == 0UL,
              "legal request uses current real protocol layout");
        Check((captured.flags & KSWORD_ARK_HVM_MEMORY_FLAG_UI_CONFIRMED) != 0UL &&
              ((captured.flags & KSWORD_ARK_HVM_MEMORY_FLAG_REQUIRE_WINDOW) != 0UL) == write &&
              captured.confirmationToken == KSWORD_ARK_HVM_MEMORY_CONFIRMATION_TOKEN,
              "confirmation and require-window flags preserved");
        Check(std::memcmp(captured.data, write ? payload.data() : zero, payload.size()) == 0,
              "only write operations copy the exact bounded payload");
    }
}
static void TestTranslationAndTransport() {
    const DriverClient client;
    const auto* poison = reinterpret_cast<const unsigned char*>(std::uintptr_t(1));
    Reset();
    auto result = client.hvmMemory(KSWORD_ARK_HVM_MEMORY_OP_TRANSLATE,
        0xFFFF800012345678ULL, 0xA123000ULL, 0UL, poison, false, true, 1234UL, nullptr);
    Check(result.io.ok && ioCalls == 1U && captured.length == 0UL,
          "zero-length translation remains accepted without payload access");
    Check(captured.address == 0xFFFF800012345678ULL && captured.directoryBase == 0xA123000ULL &&
          captured.processId == 1234UL && captured.operation == KSWORD_ARK_HVM_MEMORY_OP_TRANSLATE,
          "translation preserves address-space identity");
    Reset();
    result = client.hvmMemory(KSWORD_ARK_HVM_MEMORY_OP_QUERY_WINDOW,
        0ULL, 0ULL, 0UL, poison, false, false, 0UL, nullptr);
    Check(result.io.ok && ioCalls == 1U && captured.flags == 0UL,
          "zero-length availability query and unconfirmed flags unchanged");
    Reset(); failTransport = true;
    result = client.hvmMemory(KSWORD_ARK_HVM_MEMORY_OP_READ_PHYSICAL,
        0x1000ULL, 0ULL, 1UL, nullptr, false, true, 0UL, nullptr);
    Check(!result.io.ok && result.unsupported && result.io.win32Error == ERROR_NOT_SUPPORTED &&
          ioCalls == 1U, "actual error classifier preserves unsupported-driver handling");
}
int main() {
    TestOversizedRequests(); TestMaximumRequests(); TestTranslationAndTransport();
    std::cout << "Production HVM memory client: " << checks << " checks, " << failures << " failures\n";
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
    types = TYPES.read_text(encoding="utf-8-sig")
    definitions = "\n".join(extract_struct(types, name) for name in ("IoResult", "HvmMemoryResult"))
    implementation = "\n".join(extract_function(source, name) for name in (
        "isUnsupportedHvmError", "DriverClient::hvmMemory",
    ))
    args.work_dir.mkdir(parents=True, exist_ok=True)
    work_dir = args.work_dir.resolve()
    scratch = work_dir / f"client-{uuid.uuid4().hex}"
    scratch.mkdir()
    try:
        cpp_path = scratch / "production_client.cpp"
        binary = scratch / ("memory-client.exe" if os.name == "nt" else "memory-client")
        cpp_path.write_text(PRELUDE + definitions + BOUNDARY + implementation + TESTS, encoding="utf-8")
        subprocess.run([
            compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
            "-I", str(ROOT / "shared/driver"), str(cpp_path), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
    finally:
        if scratch.resolve().parent != work_dir:
            raise RuntimeError("Refusing to clean outside the test work directory")
        shutil.rmtree(scratch)


if __name__ == "__main__":
    main()
