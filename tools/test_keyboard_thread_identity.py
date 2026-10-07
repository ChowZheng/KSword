#!/usr/bin/env python3
"""Exercise production hotkey identity code without dereferencing raw candidates.

The real ID-based reference helper and DynData state layout are extracted too.
Mocked public thread accessors require an owned reference to the exact address.
This test never loads the driver or reads live kernel memory.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import uuid

from test_driver_candidate_reference import ROOT, function


HARNESS = r"""
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#define _KERNEL_MODE 1
#include "driver/KswordArkDynDataIoctl.h"
#define _In_
#define _Out_
#define VOID void
#define NT_SUCCESS(x) ((x) >= 0)
using ULONG = unsigned long;
using ULONG64 = unsigned long long;
using ULONG_PTR = uintptr_t;
using SIZE_T = size_t;
using BOOLEAN = unsigned char;
using WCHAR = wchar_t;
using PCSTR = const char*;
using PVOID = void*;
using HANDLE = void*;
using PEPROCESS = void*;
using PETHREAD = void*;
using NTSTATUS = long;
constexpr BOOLEAN TRUE = 1, FALSE = 0;
constexpr ULONG MAXULONG = 0xFFFFFFFFUL, KSW_HOOK_SCAN_TAG = 0x6848734BUL;
constexpr ULONG_PTR MAXULONG_PTR = UINTPTR_MAX;
constexpr NTSTATUS STATUS_SUCCESS = 0, STATUS_INVALID_PARAMETER = -1;
constexpr NTSTATUS STATUS_INVALID_DEVICE_STATE = -2, STATUS_OBJECT_TYPE_MISMATCH = -3;
constexpr NTSTATUS STATUS_DATA_ERROR = -4, STATUS_REVISION_MISMATCH = -5, STATUS_INVALID_CID = -6;
constexpr int APC_LEVEL = 1;
static_assert(sizeof(ULONG) == 4 && sizeof(WCHAR) == 2 && sizeof(PVOID) == 8, "Windows x64 ABI required.");
/*STATE_TYPES*/
struct Type {};
using POBJECT_TYPE = Type*;
static Type processType, threadType, otherType;
static POBJECT_TYPE processTypePointer = &processType, threadTypePointer = &threadType;
static POBJECT_TYPE* PsProcessType = &processTypePointer;
static POBJECT_TYPE* PsThreadType = &threadTypePointer;
static unsigned cases, reads, lookups, accessors, typeCalls, dereferences, references, allocations, frees, snapshots;
static int currentIrql;
static PVOID lookupObject, lastLookupObject, pool;
static NTSTATUS lookupStatus;
static POBJECT_TYPE actualType;
static bool failAllocation;
static ULONG snapshotCidOffset;
static std::unordered_map<ULONG_PTR, ULONG_PTR> memory;
static constexpr ULONG_PTR threadInfo = 0xFFFF900000007000ULL, candidate = 0xFFFF900000008000ULL;
static constexpr ULONG cidOffset = 0x4C8UL;
static constexpr ULONG_PTR candidateTid = 88;
static int KeGetCurrentIrql() { return currentIrql; }
static bool KswordARKKeyboardReadPointer(ULONG_PTR address, ULONG_PTR* output) {
    assert(currentIrql <= APC_LEVEL); ++reads; *output = 0;
    const auto found = memory.find(address);
    if (found == memory.end()) return false;
    *output = found->second; return true;
}
static NTSTATUS PsLookupProcessByProcessId(HANDLE, PEPROCESS*) { assert(false); return STATUS_INVALID_CID; }
static NTSTATUS PsLookupThreadByThreadId(HANDLE id, PETHREAD* output) {
    assert(currentIrql <= APC_LEVEL && uintptr_t(id) == candidateTid); ++lookups;
    *output = NT_SUCCESS(lookupStatus) ? lookupObject : nullptr;
    if (*output) { ++references; lastLookupObject = *output; }
    return lookupStatus;
}
static POBJECT_TYPE ObGetObjectType(PVOID object) {
    assert(references == 1 && object == lastLookupObject); ++typeCalls; return actualType;
}
static void ObDereferenceObject(PVOID object) {
    assert(references == 1 && object == lastLookupObject); ++dereferences; --references;
}
static HANDLE PsGetThreadProcessId(PETHREAD object) {
    assert(references == 1 && object == lastLookupObject); ++accessors; return reinterpret_cast<HANDLE>(uintptr_t(44));
}
static HANDLE PsGetThreadId(PETHREAD object) {
    assert(references == 1 && object == lastLookupObject); ++accessors; return reinterpret_cast<HANDLE>(candidateTid);
}
static ULONG HandleToULong(HANDLE handle) { return static_cast<ULONG>(uintptr_t(handle)); }
static PVOID KswordARKAllocateNonPagedPool(SIZE_T bytes, ULONG tag) {
    assert(!pool && currentIrql <= APC_LEVEL && bytes == sizeof(KSW_DYN_STATE) && tag == KSW_HOOK_SCAN_TAG);
    if (failAllocation) return nullptr;
    ++allocations; pool = new unsigned char[bytes]; return pool;
}
static void ExFreePoolWithTag(PVOID allocation, ULONG tag) {
    assert(allocation == pool && tag == KSW_HOOK_SCAN_TAG); ++frees;
    delete[] static_cast<unsigned char*>(pool); pool = nullptr;
}
static void KswordARKDynDataSnapshot(KSW_DYN_STATE* output) {
    assert(output == pool && currentIrql <= APC_LEVEL); ++snapshots;
    std::memset(output, 0, sizeof(*output)); output->Kernel.EtCid = snapshotCidOffset;
}
/*PRODUCTION*/
static void reset() {
    assert(references == 0 && !pool && allocations == frees);
    memory.clear(); memory[threadInfo] = candidate; memory[candidate + cidOffset + sizeof(PVOID)] = candidateTid;
    reads = lookups = accessors = typeCalls = dereferences = 0; currentIrql = 0;
    lookupObject = reinterpret_cast<PVOID>(candidate); lastLookupObject = nullptr; lookupStatus = STATUS_SUCCESS;
    actualType = &threadType; threadTypePointer = &threadType; PsThreadType = &threadTypePointer;
    snapshotCidOffset = cidOffset; failAllocation = false;
}
static void expect(ULONG offset, ULONG_PTR expectedAddress, unsigned expectedReads,
                   unsigned expectedLookups, unsigned expectedAccessors, unsigned expectedDereferences) {
    ULONG_PTR address = ULONG_PTR(-1); ULONG pid = 999, tid = 999;
    KswordARKKeyboardFillHotkeyThreadIdentity(threadInfo, offset, &address, &pid, &tid);
    assert(address == expectedAddress && reads == expectedReads && lookups == expectedLookups);
    assert(accessors == expectedAccessors && dereferences == expectedDereferences && references == 0);
    assert(pid == (expectedAccessors ? 44 : 0) && tid == (expectedAccessors ? candidateTid : 0)); ++cases;
}
int main() {
    reset(); expect(cidOffset, candidate, 2, 1, 2, 1);
    reset(); lookupStatus = STATUS_INVALID_CID; expect(cidOffset, candidate, 2, 1, 0, 0); // Exit before lookup.
    reset(); lookupObject = reinterpret_cast<PVOID>(candidate + 0x1000); expect(cidOffset, candidate, 2, 1, 0, 1); // Recycled TID.
    reset(); actualType = &otherType; expect(cidOffset, candidate, 2, 1, 0, 1);
    reset(); lookupObject = nullptr; expect(cidOffset, candidate, 2, 1, 0, 0);
    for (const ULONG offset : {0UL, MAXULONG, 0xFFFFUL}) { reset(); expect(offset, candidate, 1, 0, 0, 0); }
    reset(); memory.erase(candidate + cidOffset + sizeof(PVOID)); expect(cidOffset, candidate, 2, 0, 0, 0);
    reset(); memory[candidate + cidOffset + sizeof(PVOID)] = 0; expect(cidOffset, candidate, 2, 0, 0, 0);
    reset(); memory.erase(threadInfo); expect(cidOffset, 0, 1, 0, 0, 0);
    reset(); memory[threadInfo] = 7; expect(cidOffset, 0, 1, 0, 0, 0);
    reset(); memory[threadInfo] = MAXULONG_PTR - 3; expect(cidOffset, MAXULONG_PTR - 3, 1, 0, 0, 0);
    reset(); memory[threadInfo] = MAXULONG_PTR - 40; expect(cidOffset, MAXULONG_PTR - 40, 1, 0, 0, 0);
    reset(); currentIrql = APC_LEVEL + 1; expect(cidOffset, 0, 0, 0, 0, 0);
    reset(); currentIrql = APC_LEVEL; expect(cidOffset, candidate, 2, 1, 2, 1);
    reset(); PsThreadType = nullptr; expect(cidOffset, candidate, 1, 0, 0, 0);
    reset(); threadTypePointer = nullptr; expect(cidOffset, candidate, 1, 0, 0, 0);
    reset(); KswordARKKeyboardFillHotkeyThreadIdentity(threadInfo, cidOffset, nullptr, nullptr, nullptr);
    assert(references == 0 && dereferences == 1 && accessors == 0); ++cases;
    reset(); ULONG pid = 999; KswordARKKeyboardFillHotkeyThreadIdentity(0, cidOffset, nullptr, &pid, nullptr);
    assert(pid == 0 && reads == 0 && references == 0); ++cases;
    reset(); assert(KswordARKKeyboardSnapshotThreadCidOffset() == cidOffset);
    assert(!pool && allocations == frees && snapshots == 1); ++cases;
    reset(); failAllocation = true; const unsigned before = snapshots;
    assert(KswordARKKeyboardSnapshotThreadCidOffset() == MAXULONG && snapshots == before && !pool); ++cases;
    reset(); currentIrql = APC_LEVEL + 1; const unsigned prior = allocations;
    assert(KswordARKKeyboardSnapshotThreadCidOffset() == MAXULONG && allocations == prior && !pool); ++cases;
    std::printf("keyboard-thread-identity regression: %u cases passed; DynData=%zu bytes stays in pool\n", cases, sizeof(KSW_DYN_STATE));
}
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++", help="C++ compiler path")
    args = parser.parse_args()
    compiler = shutil.which(args.cxx)
    if not compiler:
        raise SystemExit(f"C++ compiler unavailable: {args.cxx}")
    query_path = "KswordARKDriver/src/features/keyboard/keyboard_query.c"
    source_text = (ROOT / query_path).read_text(encoding="utf-8-sig")
    # Require the offset captured once at entry to be propagated to the real row and identity callers.
    assert source_text.count("threadCidOffset = KswordARKKeyboardSnapshotThreadCidOffset();") == 1
    assert "ThreadCidOffset, // 候选线程只通过这份偏移安全读取 ID。" in source_text
    assert "threadCidOffset, // 每个热键使用同一查询快照的 Cid 偏移。" in source_text
    state_header = (ROOT / "KswordARKDriver/include/ark/ark_dyndata.h").read_text(encoding="utf-8-sig")
    state_types = state_header[state_header.index("typedef struct _KSW_DYN_KERNEL_OFFSETS"):
                               state_header.index("} KSW_DYN_STATE, *PKSW_DYN_STATE;") + len("} KSW_DYN_STATE, *PKSW_DYN_STATE;")]
    production = "\n\n".join([
        function(query_path, "KswordARKKeyboardLooksLikeKernelPointer"),
        function("KswordARKDriver/src/features/kernel/object_header_fallback.c", "KswordARKObjectHeaderReferenceObjectSafe"),
        function(query_path, "KswordARKKeyboardSnapshotThreadCidOffset"),
        function(query_path, "KswordARKKeyboardFillHotkeyThreadIdentity"),
    ])
    environment = os.environ.copy()
    environment["PATH"] = str(Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
    temporary_root = ROOT / ".codex-tmp" / "keyboard-identity-tests"
    if not temporary_root.resolve().is_relative_to(ROOT.resolve()):
        raise SystemExit("Regression temporary directory must remain inside the repository.")
    temporary_root.mkdir(parents=True, exist_ok=True)
    temporary = temporary_root / f"case-{uuid.uuid4().hex}"
    temporary.mkdir()
    try:
        source = temporary / "regression.cpp"
        binary = temporary / ("regression.exe" if os.name == "nt" else "regression")
        source.write_text(HARNESS.replace("/*STATE_TYPES*/", state_types).replace("/*PRODUCTION*/", production), encoding="utf-8")
        subprocess.run([compiler, "-std=c++17", "-D_M_AMD64", "-Wall", "-Wextra", "-Werror", "-fno-inline", "-O1",
                        "-fstack-usage", "-Wframe-larger-than=1024", "-I", str(ROOT / "shared"),
                        str(source), "-o", str(binary)], check=True, env=environment)
        subprocess.run([str(binary)], check=True, env=environment)
        usage_files = list(temporary.glob("*.su"))
        if not usage_files:
            raise SystemExit("Compiler did not emit the required stack-usage evidence.")
        usages = "\n".join(path.read_text(encoding="utf-8") for path in usage_files)
        for name in ("KswordARKKeyboardSnapshotThreadCidOffset", "KswordARKKeyboardFillHotkeyThreadIdentity"):
            entry = next((line for line in usages.splitlines() if name in line), None)
            if not entry:
                raise SystemExit(f"Missing stack-usage evidence: {name}")
            print(entry)
    finally:
        if temporary.resolve().is_relative_to(temporary_root.resolve()):
            shutil.rmtree(temporary)


if __name__ == "__main__":
    main()
