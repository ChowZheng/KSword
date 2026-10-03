"""Run production HVM page-walk/transfer code with fake physical memory.

This compiles the exact current functions from hvm_memory.c, including its
request dispatcher, with the real shared protocol. Only kernel primitives,
process lookup and the physical access boundary are replaced. No driver is
loaded and these checks do not prove WDK compilation or private-window safety.

Run: python tools/hvm_unit_tests/test_memory_transfer.py
Requires GCC or Clang; generated files live temporarily under work/.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import uuid


ROOT = Path(__file__).resolve().parents[2]
PRODUCTION = ROOT / "KswordARKDriver/src/features/hvm/hvm_memory.c"


def extract_function(source: str, name: str) -> str:
    # Mask comments/strings without shifting offsets before counting braces.
    masked = re.sub(
        r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"',
        lambda match: " " * len(match.group()),
        source,
        flags=re.DOTALL,
    )
    declaration = re.search(
        rf"(?:static\s+)?(?:NTSTATUS|BOOLEAN|AccessOutcome|HvmMemoryResult|QString|bool|void)\s+{re.escape(name)}\s*\(",
        masked,
    )
    if declaration is None:
        raise ValueError(f"Production function not found: {name}")
    opening = masked.index("{", declaration.end())
    depth = 1
    cursor = opening + 1
    while depth:
        if masked[cursor] == "{":
            depth += 1
        elif masked[cursor] == "}":
            depth -= 1
        cursor += 1
    return source[declaration.start():cursor]


PRELUDE = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define _KERNEL_MODE 1
#ifndef _M_AMD64
#define _M_AMD64 1
#endif
#include "KswordArkHvmIoctl.h"
#define _In_
#define _Out_
#define _Out_opt_
#define _Inout_updates_bytes_(length)
typedef unsigned long long ULONGLONG;
typedef unsigned long ULONG;
typedef unsigned char UCHAR;
typedef unsigned char BOOLEAN;
typedef unsigned char KIRQL;
typedef int32_t NTSTATUS;
#define TRUE ((BOOLEAN)1)
#define FALSE ((BOOLEAN)0)
#define PASSIVE_LEVEL 0
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000D)
#define STATUS_NOT_SUPPORTED ((NTSTATUS)0xC00000BB)
#define STATUS_NOT_FOUND ((NTSTATUS)0xC0000225)
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xC0000022)
#define STATUS_PARTIAL_COPY ((NTSTATUS)0x8000000D)
#define NT_SUCCESS(status) ((NTSTATUS)(status) >= 0)
#define RtlZeroMemory(target, length) memset((target), 0, (length))
#define RtlCopyMemory(target, source, length) memcpy((target), (source), (length))
#define KSW_HVM_MEMORY_PAGE_BYTES 0x1000ULL
#define KSW_HVM_MEMORY_PHYSICAL_MAX 0x000FFFFFFFFFFFFFULL
#define KSW_HVM_MEMORY_ENTRY_FRAME_MASK 0x000FFFFFFFFFF000ULL
#define KSW_HVM_MEMORY_ENTRY_PRESENT 0x1ULL
#define KSW_HVM_MEMORY_ENTRY_LARGE 0x80ULL
typedef struct { BOOLEAN Ready; int Lock; } KSW_HVM_MEMORY_WINDOW;
static KSW_HVM_MEMORY_WINDOW g_KswordHvmMemory;
static UCHAR g_memory[0x20000];
static ULONGLONG g_failureFrame;
static NTSTATUS g_copyFailure;
static unsigned g_copyCalls;
static unsigned g_entryReads;
static unsigned g_resolveCalls;
static unsigned g_checks;
static unsigned g_failures;
static ULONGLONG g_copyAddress[8];
static ULONG g_copyLength[8];
static void KeAcquireSpinLock(int* lock, KIRQL* irql)
{ (void)lock; *irql = 0; }
static void KeReleaseSpinLock(int* lock, KIRQL irql)
{ (void)lock; (void)irql; }
static ULONGLONG __readcr3(void) { return 0x1000ULL; }
static NTSTATUS KswordARKHvmMemoryResolveProcessDirectoryBase(
    ULONG processId, ULONGLONG* directoryBase)
{
    ++g_resolveCalls;
    *directoryBase = processId == 77UL ? 0x1000ULL : 0ULL;
    return processId == 77UL ? STATUS_SUCCESS : STATUS_NOT_FOUND;
}
static NTSTATUS KswordARKHvmMemoryReadEntry(
    ULONGLONG address, ULONGLONG* value)
{
    ++g_entryReads;
    if (address > sizeof(g_memory) - sizeof(*value)) {
        *value = 0ULL;
        return STATUS_NOT_FOUND;
    }
    memcpy(value, g_memory + (size_t)address, sizeof(*value));
    return STATUS_SUCCESS;
}
static NTSTATUS FakePhysicalCopy(
    ULONGLONG address, UCHAR* buffer, ULONG length, BOOLEAN isWrite)
{
    if (g_copyCalls < 8U) {
        g_copyAddress[g_copyCalls] = address;
        g_copyLength[g_copyCalls] = length;
    }
    ++g_copyCalls;
    if ((address & ~0xFFFULL) == g_failureFrame) {
        return g_copyFailure;
    }
    if (address > sizeof(g_memory) || length > sizeof(g_memory) - address) {
        return STATUS_ACCESS_DENIED;
    }
    if (isWrite) {
        memcpy(g_memory + (size_t)address, buffer, length);
    } else {
        memcpy(buffer, g_memory + (size_t)address, length);
    }
    return STATUS_SUCCESS;
}
static NTSTATUS KswordARKHvmMemoryCopyThroughWindow(
    ULONGLONG address, UCHAR* buffer, ULONG length, BOOLEAN isWrite)
{ return FakePhysicalCopy(address, buffer, length, isWrite); }
static NTSTATUS KswordARKHvmMemoryCopyFallback(
    ULONGLONG address, UCHAR* buffer, ULONG length)
{ return FakePhysicalCopy(address, buffer, length, FALSE); }
'''


TESTS = r'''
static void Check(int condition, const char* label)
{
    ++g_checks;
    if (!condition) {
        ++g_failures;
        fprintf(stderr, "FAIL: %s\n", label);
    }
}
static void Entry(ULONGLONG address, ULONGLONG value)
{ memcpy(g_memory + (size_t)address, &value, sizeof(value)); }
static void Map(ULONGLONG va, ULONGLONG frame)
{
    Entry(0x1000ULL + ((va >> 39) & 511ULL) * 8ULL, 0x2001ULL);
    Entry(0x2000ULL + ((va >> 30) & 511ULL) * 8ULL, 0x3001ULL);
    Entry(0x3000ULL + ((va >> 21) & 511ULL) * 8ULL, 0x4001ULL);
    Entry(0x4000ULL + ((va >> 12) & 511ULL) * 8ULL, frame | 1ULL);
}
static void Reset(void)
{
    size_t index;
    memset(g_memory, 0, sizeof(g_memory));
    memset(g_copyAddress, 0, sizeof(g_copyAddress));
    memset(g_copyLength, 0, sizeof(g_copyLength));
    for (index = 0; index < 4096U; ++index) {
        g_memory[0x8000U + index] = (UCHAR)(0x20U + index % 63U);
        g_memory[0x9000U + index] = 0xCCU;
        g_memory[0xC000U + index] = (UCHAR)(0x80U + index % 63U);
    }
    Map(0x7000ULL, 0x8000ULL);
    Map(0x8000ULL, 0xC000ULL);
    g_KswordHvmMemory.Ready = TRUE;
    g_failureFrame = ~0ULL;
    g_copyFailure = STATUS_ACCESS_DENIED;
    g_copyCalls = g_entryReads = g_resolveCalls = 0U;
}
static KSWORD_ARK_HVM_MEMORY_REQUEST Request(
    ULONG operation, ULONGLONG address, ULONG length)
{
    KSWORD_ARK_HVM_MEMORY_REQUEST request;
    memset(&request, 0, sizeof(request));
    request.version = KSWORD_ARK_HVM_MEMORY_PROTOCOL_VERSION;
    request.size = sizeof(request);
    request.operation = operation;
    request.flags = KSWORD_ARK_HVM_MEMORY_FLAG_UI_CONFIRMED;
    request.confirmationToken = KSWORD_ARK_HVM_MEMORY_CONFIRMATION_TOKEN;
    request.address = address;
    request.directoryBase = 0x1000ULL;
    request.length = length;
    memset(request.data, 0x5A, sizeof(request.data));
    return request;
}
static KSWORD_ARK_HVM_MEMORY_RESPONSE Execute(
    const KSWORD_ARK_HVM_MEMORY_REQUEST* request)
{
    KSWORD_ARK_HVM_MEMORY_RESPONSE response;
    Check(KswordARKHvmMemoryExecute(request, &response) == STATUS_SUCCESS,
          "dispatcher preserves protocol-level completion");
    return response;
}
static void TestNoncontiguousRead(void)
{
    KSWORD_ARK_HVM_MEMORY_REQUEST request;
    KSWORD_ARK_HVM_MEMORY_RESPONSE response;
    Reset();
    request = Request(KSWORD_ARK_HVM_MEMORY_OP_READ_VIRTUAL, 0x7FF0ULL, 32UL);
    response = Execute(&request);
    Check(response.status == KSWORD_ARK_HVM_MEMORY_STATUS_OK, "two-page read succeeds");
    Check(response.bytesTransferred == 32UL, "two-page read completes exact length");
    Check(response.physicalAddress == 0x8FF0ULL, "response preserves initial PA");
    Check(response.usedDirectWindow == 1U, "two-page read records window path");
    Check(memcmp(response.data, g_memory + 0x8FF0, 16U) == 0, "unaligned first fragment");
    Check(memcmp(response.data + 16, g_memory + 0xC000, 16U) == 0,
          "next VA page reads its own noncontiguous frame");
    Check(g_copyCalls == 2U && g_copyAddress[1] == 0xC000ULL,
          "two physical fragments use independent translations");
    Check(g_copyLength[0] == 16UL && g_copyLength[1] == 16UL, "split honors offset");
    Check(g_entryReads == 8U, "production walker runs for both pages");
}
static void TestNoncontiguousWrite(void)
{
    KSWORD_ARK_HVM_MEMORY_REQUEST request;
    KSWORD_ARK_HVM_MEMORY_RESPONSE response;
    UCHAR empty[KSWORD_ARK_HVM_MEMORY_MAX_BYTES] = {0};
    UCHAR expected[32];
    memset(expected, 0x5A, sizeof(expected));
    Reset();
    request = Request(KSWORD_ARK_HVM_MEMORY_OP_WRITE_VIRTUAL, 0x7FF0ULL, 32UL);
    response = Execute(&request);
    Check(response.status == KSWORD_ARK_HVM_MEMORY_STATUS_OK, "two-page write succeeds");
    Check(response.bytesTransferred == 32UL, "two-page write length");
    Check(response.physicalAddress == 0x8FF0ULL, "write retains initial PA");
    Check(memcmp(g_memory + 0x8FF0, expected, 16U) == 0, "first frame receives prefix");
    Check(memcmp(g_memory + 0xC000, expected + 16, 16U) == 0,
          "mapped second frame receives suffix");
    Check(g_memory[0x9000] == 0xCCU && g_memory[0x900F] == 0xCCU,
          "adjacent unrelated physical frame remains untouched");
    Check(g_memory[0x8FEF] != 0x5AU && g_memory[0xC010] != 0x5AU,
          "bytes outside requested interval remain untouched");
    Check(memcmp(response.data, empty, sizeof(empty)) == 0, "successful write hides payload");
}
static void TestMissingPageAndCopyFailure(void)
{
    KSWORD_ARK_HVM_MEMORY_REQUEST request;
    KSWORD_ARK_HVM_MEMORY_RESPONSE response;
    Reset();
    Entry(0x4000ULL + 8ULL * 8ULL, 0ULL);
    request = Request(KSWORD_ARK_HVM_MEMORY_OP_READ_VIRTUAL, 0x7FF0ULL, 32UL);
    response = Execute(&request);
    Check(response.status == KSWORD_ARK_HVM_MEMORY_STATUS_PARTIAL,
          "second unmapped page reports partial read");
    Check(response.ntStatus == STATUS_NOT_FOUND && response.bytesTransferred == 16UL,
          "partial read preserves walk failure and completed prefix");
    Check(response.physicalAddress == 0x8FF0ULL && response.usedDirectWindow == 1U,
          "partial read retains initial PA and window evidence");
    Check(g_copyCalls == 1U, "missing second page is never copied");
    Check(memcmp(response.data, g_memory + 0x8FF0, 16U) == 0, "partial read prefix valid");
    request.operation = KSWORD_ARK_HVM_MEMORY_OP_WRITE_VIRTUAL;
    response = Execute(&request);
    Check(response.status == KSWORD_ARK_HVM_MEMORY_STATUS_PARTIAL &&
          response.bytesTransferred == 16UL, "missing page exposes partial write");
    Check(g_memory[0x8FF0] == 0x5AU && g_memory[0xC000] == 0x80U &&
          g_memory[0x9000] == 0xCCU, "partial write touches only mapped prefix");
    Reset();
    Entry(0x4000ULL + 7ULL * 8ULL, 0ULL);
    request = Request(KSWORD_ARK_HVM_MEMORY_OP_READ_VIRTUAL, 0x7FF0ULL, 32UL);
    response = Execute(&request);
    Check(response.status == KSWORD_ARK_HVM_MEMORY_STATUS_TRANSLATION_FAILED &&
          response.bytesTransferred == 0UL && g_copyCalls == 0U,
          "first missing page retains translation-failed status and performs no copy");
    Reset();
    g_failureFrame = 0xC000ULL;
    response = Execute(&request);
    Check(response.status == KSWORD_ARK_HVM_MEMORY_STATUS_PARTIAL &&
          response.ntStatus == STATUS_ACCESS_DENIED && response.bytesTransferred == 16UL,
          "second physical access failure preserves partial prefix");
    g_copyFailure = STATUS_PARTIAL_COPY;
    response = Execute(&request);
    Check(response.status == KSWORD_ARK_HVM_MEMORY_STATUS_PARTIAL &&
          response.ntStatus == STATUS_PARTIAL_COPY && response.bytesTransferred == 16UL,
          "short physical fragment does not count uncompleted bytes");
}
static void TestSinglePageAndPhysical(void)
{
    KSWORD_ARK_HVM_MEMORY_REQUEST request;
    KSWORD_ARK_HVM_MEMORY_RESPONSE response;
    Reset();
    request = Request(KSWORD_ARK_HVM_MEMORY_OP_READ_VIRTUAL, 0x7103ULL, 64UL);
    response = Execute(&request);
    Check(response.status == 0UL && response.bytesTransferred == 64UL &&
          g_copyCalls == 1U && memcmp(response.data, g_memory + 0x8103, 64U) == 0,
          "unaligned single-page read unchanged");
    request = Request(KSWORD_ARK_HVM_MEMORY_OP_READ_VIRTUAL, 0x7000ULL,
                      KSWORD_ARK_HVM_MEMORY_MAX_BYTES);
    response = Execute(&request);
    Check(response.bytesTransferred == KSWORD_ARK_HVM_MEMORY_MAX_BYTES &&
          memcmp(response.data, g_memory + 0x8000, sizeof(response.data)) == 0,
          "aligned maximum-sized read unchanged");
    Reset();
    request = Request(KSWORD_ARK_HVM_MEMORY_OP_READ_PHYSICAL, 0x8FF0ULL, 32UL);
    response = Execute(&request);
    Check(response.status == 0UL && response.bytesTransferred == 32UL &&
          g_entryReads == 0U && g_copyCalls == 2U && g_copyAddress[1] == 0x9000ULL,
          "physical read retains contiguous splitting without a page walk");
    Check(memcmp(response.data, g_memory + 0x8FF0, 32U) == 0,
          "physical read follows contiguous physical bytes");
    request.operation = KSWORD_ARK_HVM_MEMORY_OP_WRITE_PHYSICAL;
    response = Execute(&request);
    Check(response.status == 0UL && g_memory[0x8FF0] == 0x5AU &&
          g_memory[0x9000] == 0x5AU && g_memory[0xC000] == 0x80U,
          "physical write retains contiguous semantics");
    request.address = KSW_HVM_MEMORY_PHYSICAL_MAX - 8ULL;
    response = Execute(&request);
    Check(response.status == KSWORD_ARK_HVM_MEMORY_STATUS_ADDRESS_INVALID,
          "physical encoding-limit rejection unchanged");
}
static void TestWindowFallbackAndHierarchy(void)
{
    KSWORD_ARK_HVM_MEMORY_REQUEST request;
    KSWORD_ARK_HVM_MEMORY_RESPONSE response;
    Reset();
    g_KswordHvmMemory.Ready = FALSE;
    request = Request(KSWORD_ARK_HVM_MEMORY_OP_READ_VIRTUAL, 0x7FF0ULL, 32UL);
    response = Execute(&request);
    Check(response.status == 0UL && response.usedDirectWindow == 0U &&
          response.windowReady == 0U && memcmp(response.data + 16, g_memory + 0xC000, 16U) == 0,
          "fallback still reads each noncontiguous virtual mapping");
    request.flags |= KSWORD_ARK_HVM_MEMORY_FLAG_REQUIRE_WINDOW;
    g_copyCalls = g_entryReads = 0U;
    response = Execute(&request);
    Check(response.status == KSWORD_ARK_HVM_MEMORY_STATUS_WINDOW_UNAVAILABLE &&
          g_copyCalls == 0U && g_entryReads == 0U, "requireWindow rejects before walk or access");
    request.flags &= ~KSWORD_ARK_HVM_MEMORY_FLAG_REQUIRE_WINDOW;
    request.operation = KSWORD_ARK_HVM_MEMORY_OP_WRITE_VIRTUAL;
    response = Execute(&request);
    Check(response.status == KSWORD_ARK_HVM_MEMORY_STATUS_ACCESS_FAILED &&
          response.ntStatus == STATUS_NOT_SUPPORTED && response.bytesTransferred == 0UL &&
          g_memory[0x8FF0] != 0x5AU, "writes never fall back");
    Reset();
    request = Request(KSWORD_ARK_HVM_MEMORY_OP_READ_VIRTUAL, 0x7FF0ULL, 32UL);
    request.processId = 77UL;
    request.directoryBase = 0x5000ULL;
    response = Execute(&request);
    Check(response.status == 0UL && g_resolveCalls == 1U && g_entryReads == 8U,
          "one PID resolution supplies the same hierarchy to both pages");
    request.processId = 88UL;
    response = Execute(&request);
    Check(response.status == KSWORD_ARK_HVM_MEMORY_STATUS_PROCESS_LOOKUP_FAILED,
          "PID lookup failure unchanged");
    request.processId = 0UL;
    response = Execute(&request);
    Check(response.status == KSWORD_ARK_HVM_MEMORY_STATUS_TRANSLATION_FAILED,
          "custom CR3 remains authoritative without a PID");
    request.directoryBase = 0ULL;
    response = Execute(&request);
    Check(response.status == 0UL, "zero PID/CR3 still uses current hierarchy");
}
static void TestBoundaries(void)
{
    KSWORD_ARK_HVM_MEMORY_REQUEST request;
    KSWORD_ARK_HVM_MEMORY_RESPONSE response;
    Reset();
    request = Request(KSWORD_ARK_HVM_MEMORY_OP_READ_VIRTUAL, ~0ULL - 8ULL, 16UL);
    response = Execute(&request);
    Check(response.status == KSWORD_ARK_HVM_MEMORY_STATUS_ADDRESS_INVALID &&
          response.ntStatus == STATUS_INVALID_PARAMETER && g_entryReads == 0U &&
          g_copyCalls == 0U, "wrapped virtual range rejected before any access");
    request.operation = KSWORD_ARK_HVM_MEMORY_OP_WRITE_VIRTUAL;
    response = Execute(&request);
    Check(response.status == KSWORD_ARK_HVM_MEMORY_STATUS_ADDRESS_INVALID &&
          g_copyCalls == 0U, "wrapped virtual write leaves memory untouched");
    request = Request(KSWORD_ARK_HVM_MEMORY_OP_READ_VIRTUAL, ~0ULL - 31ULL, 32UL);
    Map(request.address, 0x8000ULL);
    response = Execute(&request);
    Check(response.status == 0UL && response.bytesTransferred == 32UL &&
          response.physicalAddress == 0x8FE0ULL, "span ending exactly at ULLONG_MAX is valid");
    Reset();
    request = Request(KSWORD_ARK_HVM_MEMORY_OP_READ_VIRTUAL, 0x7000ULL, 0UL);
    response = Execute(&request);
    Check(response.status == KSWORD_ARK_HVM_MEMORY_STATUS_ADDRESS_INVALID &&
          g_copyCalls == 0U, "zero-length transfer rejected");
    request.operation = KSWORD_ARK_HVM_MEMORY_OP_TRANSLATE;
    response = Execute(&request);
    Check(response.status == 0UL && response.physicalAddress == 0x8000ULL &&
          response.bytesTransferred == 0UL && g_copyCalls == 0U,
          "translation-only zero length remains valid");
    Reset();
    Entry(0x3000ULL, KSW_HVM_MEMORY_ENTRY_PRESENT | KSW_HVM_MEMORY_ENTRY_LARGE);
    request = Request(KSWORD_ARK_HVM_MEMORY_OP_READ_VIRTUAL, 0x7FF0ULL, 32UL);
    response = Execute(&request);
    Check(response.status == 0UL && response.physicalAddress == 0x7FF0ULL &&
          g_entryReads == 6U && g_copyAddress[1] == 0x8000ULL,
          "large-page mappings remain correct across a 4-KiB fragment boundary");
    Reset();
    request = Request(KSWORD_ARK_HVM_MEMORY_OP_READ_VIRTUAL, 0x7FFFULL,
                      KSWORD_ARK_HVM_MEMORY_MAX_BYTES);
    response = Execute(&request);
    Check(response.status == 0UL && response.bytesTransferred == sizeof(response.data) &&
          g_copyLength[0] == 1UL &&
          memcmp(response.data + 1, g_memory + 0xC000, sizeof(response.data) - 1U) == 0,
          "maximum request at the final byte of a page splits one plus remainder");
}
int main(void)
{
    TestNoncontiguousRead();
    TestNoncontiguousWrite();
    TestMissingPageAndCopyFailure();
    TestSinglePageAndPhysical();
    TestWindowFallbackAndHierarchy();
    TestBoundaries();
    printf("HVM production memory transfer: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0U ? EXIT_SUCCESS : EXIT_FAILURE;
}
'''


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=PRODUCTION)
    parser.add_argument("--compiler", default=os.environ.get("CC"))
    parser.add_argument("--work-dir", type=Path, default=ROOT / "work/hvm-memory-tests")
    args = parser.parse_args()
    compiler = args.compiler or shutil.which("gcc") or shutil.which("clang")
    if not compiler:
        parser.error("GCC or Clang is required (--compiler or CC can specify its path)")
    source = args.source.read_text(encoding="utf-8-sig")
    functions = [
        extract_function(source, name)
        for name in (
            "KswordARKHvmMemoryIsPhysicalRangeValid",
            "KswordARKHvmMemoryTranslate",
            "KswordARKHvmMemoryCopyRange",
            "KswordARKHvmMemoryExecute",
        )
    ]
    args.work_dir.mkdir(parents=True, exist_ok=True)
    work_dir = args.work_dir.resolve()
    # mkdir inherits the workspace ACL; mkdtemp's 0700 ACL can exclude Windows
    # sandbox processes from their own newly created directory.
    scratch = work_dir / f"run-{uuid.uuid4().hex}"
    scratch.mkdir()
    try:
        c_path = scratch / "production_memory_transfer.c"
        binary = scratch / ("memory-transfer.exe" if os.name == "nt" else "memory-transfer")
        c_path.write_text(PRELUDE + "\n".join(functions) + TESTS, encoding="utf-8")
        subprocess.run(
            [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
             "-I", str(ROOT / "shared/driver"), str(c_path), "-o", str(binary)],
            check=True,
        )
        subprocess.run([str(binary)], check=True)
    finally:
        if scratch.resolve().parent != work_dir:
            raise RuntimeError("Refusing to clean a path outside the test work directory")
        shutil.rmtree(scratch)


if __name__ == "__main__":
    main()
