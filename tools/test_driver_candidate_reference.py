#!/usr/bin/env python3
"""Compile the production candidate-reference functions against safe API mocks.

This offline regression never loads the driver. Invalid candidate addresses are
opaque sentinels: the mocked object-manager API rejects any access unless an
ID lookup has already supplied a reference to that exact address.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import uuid


ROOT = Path(__file__).resolve().parents[1]


def function(source_path: str, name: str) -> str:
    text = (ROOT / source_path).read_text(encoding="utf-8-sig")
    signature = re.search(rf"(?m)^(?:static\s+)?(?:NTSTATUS|VOID|BOOLEAN|LONG|ULONG)\s+{re.escape(name)}\(", text)
    if signature is None:
        raise ValueError(f"Production function unavailable: {name}")
    start = signature.start()
    body_start = text.index("\n{", signature.end()) + 1
    depth = 0
    for index in range(body_start, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start:index + 1]
    raise ValueError(f"Unterminated production function: {name}")


HARNESS = r"""
#include <cassert>
#include <cstdint>
#include <cstdio>
#define _In_
#define _In_opt_
#define _Out_
using NTSTATUS = int;
using BOOLEAN = bool;
using PVOID = void*;
using HANDLE = void*;
using ULONG_PTR = uintptr_t;
using PEPROCESS = void*;
using PETHREAD = void*;
struct Type {};
using POBJECT_TYPE = Type*;
constexpr BOOLEAN TRUE = true, FALSE = false;
constexpr NTSTATUS STATUS_SUCCESS = 0, STATUS_INVALID_PARAMETER = -1;
constexpr NTSTATUS STATUS_INVALID_DEVICE_STATE = -2, STATUS_OBJECT_TYPE_MISMATCH = -3;
constexpr NTSTATUS STATUS_DATA_ERROR = -4, STATUS_REVISION_MISMATCH = -5;
constexpr NTSTATUS STATUS_INVALID_CID = -6;
constexpr int APC_LEVEL = 1;
#define NT_SUCCESS(status) ((status) >= 0)
static Type processType, threadType, otherType;
static POBJECT_TYPE processTypePointer = &processType, threadTypePointer = &threadType;
static POBJECT_TYPE* PsProcessType = &processTypePointer;
static POBJECT_TYPE* PsThreadType = &threadTypePointer;
static PVOID lookupObject, lastLookupObject;
static POBJECT_TYPE actualType;
static NTSTATUS lookupStatus;
static int currentIrql, lookupCalls, typeCalls, dereferenceCalls, ownedReferences, cases;
static HANDLE lastLookupId;
static int KeGetCurrentIrql() { return currentIrql; }
static NTSTATUS lookup(HANDLE id, PVOID* out) {
    ++lookupCalls;
    lastLookupId = id;
    *out = NT_SUCCESS(lookupStatus) ? lookupObject : nullptr;
    if (*out != nullptr) { ++ownedReferences; lastLookupObject = *out; }
    return lookupStatus;
}
static NTSTATUS PsLookupProcessByProcessId(HANDLE id, PEPROCESS* out) { return lookup(id, out); }
static NTSTATUS PsLookupThreadByThreadId(HANDLE id, PETHREAD* out) { return lookup(id, out); }
static POBJECT_TYPE ObGetObjectType(PVOID object) {
    assert(ownedReferences > 0 && object == lastLookupObject);
    ++typeCalls;
    return actualType;
}
static void ObDereferenceObject(PVOID object) {
    assert(ownedReferences > 0 && object == lastLookupObject);
    --ownedReferences;
    ++dereferenceCalls;
}
static BOOLEAN KswordARKCrossViewPointerAligned(ULONG_PTR address) {
    return address != 0 && (address & 7U) == 0;
}
/*PRODUCTION*/
using Wrapper = NTSTATUS(*)(PVOID, HANDLE, POBJECT_TYPE, BOOLEAN*, BOOLEAN*);
static PVOID candidate = reinterpret_cast<PVOID>(uintptr_t(0xFFFF900000008000ULL));
static HANDLE candidateId = reinterpret_cast<HANDLE>(uintptr_t(88));
static void reset(POBJECT_TYPE type) {
    lookupObject = candidate;
    lastLookupObject = nullptr;
    actualType = type;
    lookupStatus = STATUS_SUCCESS;
    currentIrql = lookupCalls = typeCalls = dereferenceCalls = ownedReferences = 0;
    lastLookupId = nullptr;
}
static void expect(Wrapper wrapper, PVOID object, HANDLE id, POBJECT_TYPE type,
                   NTSTATUS status, int expectedLookups, int expectedTypes, int expectedDerefs) {
    BOOLEAN typeMatched = true, referenced = true;
    assert(wrapper(object, id, type, &typeMatched, &referenced) == status);
    assert(typeMatched == NT_SUCCESS(status) && referenced == NT_SUCCESS(status));
    assert(lookupCalls == expectedLookups && typeCalls == expectedTypes);
    assert(dereferenceCalls == expectedDerefs);
    if (NT_SUCCESS(status)) {
        assert(lastLookupId == id && ownedReferences == 1);
        ObDereferenceObject(object);
    }
    assert(ownedReferences == 0);
    ++cases;
}
static void run(Wrapper wrapper, POBJECT_TYPE type) {
    reset(type);
    expect(wrapper, candidate, candidateId, type, STATUS_SUCCESS, 1, 1, 0);
    reset(type); lookupStatus = STATUS_INVALID_CID;
    expect(wrapper, candidate, candidateId, type, STATUS_INVALID_CID, 1, 0, 0);
    reset(type); lookupObject = reinterpret_cast<PVOID>(uintptr_t(0xFFFF900000009000ULL));
    expect(wrapper, candidate, candidateId, type, STATUS_REVISION_MISMATCH, 1, 0, 1);
    reset(type); actualType = &otherType;
    expect(wrapper, candidate, candidateId, type, STATUS_OBJECT_TYPE_MISMATCH, 1, 1, 1);
    reset(type); currentIrql = APC_LEVEL + 1;
    expect(wrapper, candidate, candidateId, type, STATUS_INVALID_DEVICE_STATE, 0, 0, 0);
    reset(type);
    expect(wrapper, nullptr, candidateId, type, STATUS_INVALID_PARAMETER, 0, 0, 0);
    reset(type);
    expect(wrapper, candidate, nullptr, type, STATUS_INVALID_PARAMETER, 0, 0, 0);
    reset(type);
    expect(wrapper, candidate, candidateId, nullptr, STATUS_INVALID_PARAMETER, 0, 0, 0);
    reset(type);
    expect(wrapper, reinterpret_cast<PVOID>(uintptr_t(3)), candidateId, type, STATUS_INVALID_PARAMETER, 0, 0, 0);
    reset(type); lookupObject = nullptr;
    expect(wrapper, candidate, candidateId, type, STATUS_DATA_ERROR, 1, 0, 0);
    reset(type);
    expect(wrapper, candidate, candidateId, &otherType, STATUS_OBJECT_TYPE_MISMATCH, 0, 0, 0);
    reset(type); BOOLEAN out = true;
    assert(wrapper(candidate, candidateId, type, nullptr, &out) == STATUS_INVALID_PARAMETER);
    assert(lookupCalls == 0 && typeCalls == 0); ++cases;
    reset(type);
    assert(wrapper(candidate, candidateId, type, &out, nullptr) == STATUS_INVALID_PARAMETER);
    assert(lookupCalls == 0 && typeCalls == 0); ++cases;
}
int main() {
    run(KswordARKCrossViewTryReferenceTypedObject, &processType);
    run(KswordARKCrossViewTryReferenceTypedObject, &threadType);
    run(KswordARKThreadCrossViewTryReferenceTypedObject, &threadType);
    std::printf("candidate-reference regression: %d cases passed\n", cases);
}
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++", help="C++ compiler path")
    args = parser.parse_args()
    cxx = shutil.which(args.cxx)
    if not cxx:
        raise SystemExit(f"C++ compiler unavailable: {args.cxx}")
    production = "\n\n".join([
        function("KswordARKDriver/src/features/kernel/object_header_fallback.c", "KswordARKObjectHeaderReferenceObjectSafe"),
        function("KswordARKDriver/src/features/process/process_crossview.c", "KswordARKCrossViewTryReferenceTypedObject"),
        function("KswordARKDriver/src/features/thread/thread_crossview.c", "KswordARKThreadCrossViewTryReferenceTypedObject"),
    ])
    environment = os.environ.copy()
    environment["PATH"] = str(Path(cxx).parent) + os.pathsep + environment.get("PATH", "")
    temporary_root = ROOT / ".codex-tmp" / "candidate-reference-tests"
    if not temporary_root.resolve().is_relative_to(ROOT.resolve()):
        raise SystemExit("Regression temporary directory must remain inside the repository.")
    temporary_root.mkdir(parents=True, exist_ok=True)
    temporary = temporary_root / f"case-{uuid.uuid4().hex}"
    temporary.mkdir()
    try:
        source = temporary / "regression.cpp"
        binary = temporary / ("regression.exe" if os.name == "nt" else "regression")
        source.write_text(HARNESS.replace("/*PRODUCTION*/", production), encoding="utf-8")
        subprocess.run([cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(binary)],
                       check=True, env=environment)
        subprocess.run([str(binary)], check=True, env=environment)
    finally:
        if temporary.resolve().is_relative_to(temporary_root.resolve()):
            shutil.rmtree(temporary)


if __name__ == "__main__":
    main()
