#!/usr/bin/env python3
"""Run the production registry reader/IOCTL with bounded two-query API mocks.

No registry or driver is opened. Pool reads are checked against the exact
allocation, including when a registry value grows between the size and data
queries. The real shared Windows ABI and METHOD_BUFFERED handler are retained.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import uuid

from test_driver_candidate_reference import ROOT, function


HARNESS = r"""
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#define _KERNEL_MODE 1
#include "driver/KswordArkRegistryIoctl.h"
#define _In_
#define _Out_
#define _Outptr_
#define _Out_writes_bytes_(x)
#define _Out_writes_bytes_to_(x,y)
#define VOID void
#define UNREFERENCED_PARAMETER(x) ((void)(x))
#define NT_SUCCESS(x) ((x) >= 0)
#define FIELD_OFFSET(type, field) offsetof(type, field)
using ULONG = unsigned long;
using USHORT = unsigned short;
using WCHAR = wchar_t;
using PWSTR = WCHAR*;
using PVOID = void*;
using HANDLE = void*;
using WDFDEVICE = void*;
using NTSTATUS = long;
constexpr NTSTATUS STATUS_SUCCESS = 0, STATUS_INVALID_PARAMETER = -1, STATUS_BUFFER_TOO_SMALL = -2;
constexpr NTSTATUS STATUS_REVISION_MISMATCH = -3, STATUS_OBJECT_NAME_NOT_FOUND = -4;
constexpr NTSTATUS STATUS_OBJECT_PATH_NOT_FOUND = -5, STATUS_INFO_LENGTH_MISMATCH = -6;
constexpr NTSTATUS STATUS_INTEGER_OVERFLOW = -7, STATUS_INSUFFICIENT_RESOURCES = -8;
constexpr NTSTATUS STATUS_BUFFER_OVERFLOW = -9, STATUS_UNSUCCESSFUL = -10, STATUS_ACCESS_DENIED = -11;
constexpr ULONG OBJ_KERNEL_HANDLE = 1, OBJ_CASE_INSENSITIVE = 2, KEY_QUERY_VALUE = 1;
constexpr ULONG KSWORD_ARK_REGISTRY_QUERY_TAG = 0x6752734BUL;
constexpr int NonPagedPoolNx = 0, KeyValuePartialInformation = 2;
static_assert(sizeof(ULONG) == 4 && sizeof(WCHAR) == 2, "The shared protocol requires the Windows ABI.");
struct UNICODE_STRING { USHORT Length = 0, MaximumLength = 0; PWSTR Buffer = nullptr; };
struct OBJECT_ATTRIBUTES { UNICODE_STRING* name = nullptr; };
struct KEY_VALUE_PARTIAL_INFORMATION { ULONG TitleIndex, Type, DataLength; unsigned char Data[1]; };
using PKEY_VALUE_PARTIAL_INFORMATION = KEY_VALUE_PARTIAL_INFORMATION*;
static_assert(offsetof(KEY_VALUE_PARTIAL_INFORMATION, Data) == 12 && sizeof(KEY_VALUE_PARTIAL_INFORMATION) == 16);
static unsigned char* pool;
static size_t poolBytes;
static unsigned allocations, frees, openHandles, queries, cases;
static bool failAllocation;
static ULONG firstDataBytes, secondDataBytes, reportedSecondLength, declaredSecondBytes;
static NTSTATUS firstStatus, secondStatus, openStatus;
constexpr NTSTATUS AUTOMATIC_STATUS = 99;
static void RtlZeroMemory(PVOID target, size_t bytes) { std::memset(target, 0, bytes); }
static void RtlCopyMemory(PVOID target, const void* source, size_t bytes) {
    const auto address = reinterpret_cast<uintptr_t>(source);
    const auto start = reinterpret_cast<uintptr_t>(pool);
    if (pool && address >= start && address < start + poolBytes) {
        assert(bytes <= poolBytes - (address - start));
    }
    std::memcpy(target, source, bytes);
}
static PVOID ExAllocatePoolWithTag(int kind, size_t bytes, ULONG tag) {
    assert(!pool && kind == NonPagedPoolNx && tag == KSWORD_ARK_REGISTRY_QUERY_TAG);
    if (failAllocation) return nullptr;
    ++allocations; poolBytes = bytes; pool = new unsigned char[bytes + 32];
    std::memset(pool, 0xA7, bytes + 32); return pool;
}
static void ExFreePoolWithTag(PVOID allocation, ULONG tag) {
    assert(allocation == pool && tag == KSWORD_ARK_REGISTRY_QUERY_TAG);
    for (size_t index = poolBytes; index < poolBytes + 32; ++index) assert(pool[index] == 0xA7);
    ++frees; delete[] pool; pool = nullptr;
}
static USHORT KswordARKRegistryBoundedWideLength(const WCHAR* text, USHORT maximum) {
    USHORT length = 0; while (length < maximum && text[length]) ++length; return length;
}
static NTSTATUS KswordARKRegistryBuildKernelPath(const WCHAR* text, UNICODE_STRING* result) {
    assert(text[0] == L'\\' && text[1] == L'R');
    result->Buffer = const_cast<PWSTR>(text); result->Length = 20; result->MaximumLength = 20;
    return STATUS_SUCCESS;
}
static void InitializeObjectAttributes(OBJECT_ATTRIBUTES* result, UNICODE_STRING* name, ULONG flags, PVOID, PVOID) {
    assert(flags == (OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE)); result->name = name;
}
static NTSTATUS ZwOpenKey(HANDLE* result, ULONG access, OBJECT_ATTRIBUTES* attributes) {
    assert(access == KEY_QUERY_VALUE && attributes->name->Length != 0);
    if (!NT_SUCCESS(openStatus)) return openStatus;
    ++openHandles; *result = reinterpret_cast<HANDLE>(uintptr_t(1)); return STATUS_SUCCESS;
}
static void ZwClose(HANDLE handle) { assert(handle && openHandles == 1); --openHandles; }
static unsigned char pattern(ULONG index) { return static_cast<unsigned char>(0x30 + index % 71); }
static NTSTATUS ZwQueryValueKey(HANDLE handle, UNICODE_STRING* value, int informationClass,
                               PVOID output, ULONG length, ULONG* resultLength) {
    assert(handle && openHandles == 1 && informationClass == KeyValuePartialInformation);
    assert(value->Length == 2 && value->Buffer[0] == L'v');
    ++queries;
    if (!output) {
        assert(queries == 1 && length == 0);
        *resultLength = firstDataBytes == ULONG(-1) ? 0 : 12 + firstDataBytes;
        return firstStatus;
    }
    assert(queries == 2 && output == pool && length == poolBytes && length >= 16);
    *resultLength = reportedSecondLength ? reportedSecondLength : 12 + secondDataBytes;
    const NTSTATUS status = secondStatus == AUTOMATIC_STATUS ?
        (12 + secondDataBytes <= length ? STATUS_SUCCESS : STATUS_BUFFER_OVERFLOW) : secondStatus;
    if (status != STATUS_SUCCESS && status != STATUS_BUFFER_OVERFLOW) return status;
    if (*resultLength < 12) return status;
    auto* information = static_cast<PKEY_VALUE_PARTIAL_INFORMATION>(output);
    information->Type = 3; information->DataLength = declaredSecondBytes == ULONG(-1) ? secondDataBytes : declaredSecondBytes;
    const ULONG readable = std::min(secondDataBytes, std::min(length - 12, *resultLength - 12));
    for (ULONG index = 0; index < readable; ++index) information->Data[index] = pattern(index);
    return status;
}
struct FakeRequest {
    std::vector<unsigned char> buffer = std::vector<unsigned char>(sizeof(KSWORD_ARK_READ_REGISTRY_VALUE_RESPONSE) + 32, 0xCD);
    size_t inputLength = sizeof(KSWORD_ARK_READ_REGISTRY_VALUE_REQUEST);
    size_t outputLength = sizeof(KSWORD_ARK_READ_REGISTRY_VALUE_RESPONSE);
};
using WDFREQUEST = FakeRequest*;
static NTSTATUS KswordARKRetrieveRequiredInputBuffer(WDFREQUEST request, size_t bytes, PVOID* output, size_t* actual) {
    *output = request->buffer.data(); *actual = request->inputLength;
    return bytes <= *actual ? STATUS_SUCCESS : STATUS_BUFFER_TOO_SMALL;
}
static NTSTATUS KswordARKRetrieveRequiredOutputBuffer(WDFREQUEST request, size_t bytes, PVOID* output, size_t* actual) {
    *output = request->buffer.data(); *actual = request->outputLength;
    return bytes <= *actual ? STATUS_SUCCESS : STATUS_BUFFER_TOO_SMALL;
}
static void KswordARKRegistryIoctlLog(WDFDEVICE, const char*, const char*, ...) {}
/*PRODUCTION*/
static void reset(ULONG initial, ULONG current) {
    assert(!pool && openHandles == 0 && allocations == frees);
    firstDataBytes = initial; secondDataBytes = current; reportedSecondLength = 0; declaredSecondBytes = ULONG(-1);
    failAllocation = false; queries = 0; firstStatus = STATUS_BUFFER_TOO_SMALL;
    secondStatus = AUTOMATIC_STATUS; openStatus = STATUS_SUCCESS;
}
static KSWORD_ARK_READ_REGISTRY_VALUE_REQUEST* prepare(FakeRequest& request, ULONG maximum = 0) {
    auto* input = reinterpret_cast<KSWORD_ARK_READ_REGISTRY_VALUE_REQUEST*>(request.buffer.data());
    std::memset(input, 0, sizeof(*input)); input->version = KSWORD_ARK_REGISTRY_PROTOCOL_VERSION;
    input->flags = KSWORD_ARK_REGISTRY_READ_FLAG_VALUE_NAME_PRESENT; input->maxDataBytes = maximum;
    input->keyPath[0] = L'\\'; input->keyPath[1] = L'R'; input->valueName[0] = L'v'; return input;
}
static KSWORD_ARK_READ_REGISTRY_VALUE_RESPONSE* invoke(FakeRequest& request, NTSTATUS expected = STATUS_SUCCESS) {
    size_t written = 999;
    assert(KswordARKRegistryIoctlReadValue(nullptr, &request, request.inputLength, request.outputLength, &written) == expected);
    assert(written == (expected == STATUS_SUCCESS ? sizeof(KSWORD_ARK_READ_REGISTRY_VALUE_RESPONSE) : 0));
    assert(!pool && openHandles == 0 && allocations == frees);
    for (size_t index = sizeof(KSWORD_ARK_READ_REGISTRY_VALUE_RESPONSE); index < request.buffer.size(); ++index)
        assert(request.buffer[index] == 0xCD);
    ++cases; return reinterpret_cast<KSWORD_ARK_READ_REGISTRY_VALUE_RESPONSE*>(request.buffer.data());
}
static void expect(ULONG initial, ULONG current, ULONG maximum, ULONG returned, bool truncated) {
    reset(initial, current); FakeRequest request; prepare(request, maximum);
    const auto* response = invoke(request);
    assert(response->requiredBytes == current && response->dataBytes == returned && response->valueType == 3);
    assert(response->status == (truncated ? KSWORD_ARK_REGISTRY_READ_STATUS_BUFFER_TOO_SMALL : KSWORD_ARK_REGISTRY_READ_STATUS_SUCCESS));
    assert(response->lastStatus == (truncated ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS));
    for (ULONG index = 0; index < returned; ++index) assert(response->data[index] == pattern(index));
    for (ULONG index = returned; index < KSWORD_ARK_REGISTRY_DATA_MAX_BYTES; ++index) assert(response->data[index] == 0);
}
int main() {
    expect(100, 100, 0, 100, false);
    expect(1, 100, 0, 4, true); // The original reader copies 100 bytes from a 16-byte allocation.
    expect(0, 5, 0, 4, true); // A header-only first query still allocates only the structure minimum.
    expect(0, 4, 0, 4, false);
    expect(100, 2, 0, 2, false);
    expect(0, 0, 0, 0, false);
    expect(100, 100, 1, 1, true);
    expect(12, 12, 0, 12, false); // Protocol maxDataBytes=0 selects the default, not zero capacity.
    expect(5000, 5000, ULONG(-1), KSWORD_ARK_REGISTRY_DATA_MAX_BYTES, true);
    expect(1, 5000, ULONG(-1), 4, true);
    reset(1, 100); secondStatus = STATUS_BUFFER_TOO_SMALL;
    { FakeRequest request; prepare(request); const auto* response = invoke(request);
      assert(response->requiredBytes == 100 && response->dataBytes == 0 && response->lastStatus == STATUS_BUFFER_TOO_SMALL); }
    reset(100, 100); secondStatus = STATUS_ACCESS_DENIED;
    { FakeRequest request; prepare(request); const auto* response = invoke(request);
      assert(response->status == KSWORD_ARK_REGISTRY_READ_STATUS_FAILED && response->lastStatus == STATUS_ACCESS_DENIED); }
    reset(100, 100); secondStatus = STATUS_OBJECT_NAME_NOT_FOUND;
    { FakeRequest request; prepare(request); const auto* response = invoke(request);
      assert(response->status == KSWORD_ARK_REGISTRY_READ_STATUS_NOT_FOUND); }
    reset(100, 100); failAllocation = true;
    { FakeRequest request; prepare(request); const auto* response = invoke(request);
      assert(response->lastStatus == STATUS_INSUFFICIENT_RESOURCES && queries == 1); }
    reset(ULONG(-1), 0); firstStatus = STATUS_OBJECT_NAME_NOT_FOUND;
    { FakeRequest request; prepare(request); const auto* response = invoke(request);
      assert(response->status == KSWORD_ARK_REGISTRY_READ_STATUS_NOT_FOUND && queries == 1); }
    for (const NTSTATUS status : {STATUS_SUCCESS, STATUS_BUFFER_OVERFLOW}) {
        reset(100, 100); secondStatus = status; reportedSecondLength = 8;
        FakeRequest request; prepare(request); const auto* response = invoke(request);
        assert(response->lastStatus == STATUS_INFO_LENGTH_MISMATCH && response->requiredBytes == 0 && response->dataBytes == 0);
    }
    reset(100, 4); declaredSecondBytes = 100;
    { FakeRequest request; prepare(request); const auto* response = invoke(request);
      assert(response->requiredBytes == 100 && response->dataBytes == 4 && response->lastStatus == STATUS_BUFFER_TOO_SMALL); }
    reset(1, 100); openStatus = STATUS_ACCESS_DENIED;
    { FakeRequest request; prepare(request); const auto* response = invoke(request);
      assert(response->lastStatus == STATUS_ACCESS_DENIED && queries == 0); }
    reset(1, 100);
    { FakeRequest request; prepare(request); --request.inputLength; invoke(request, STATUS_BUFFER_TOO_SMALL); assert(queries == 0); }
    reset(1, 100);
    { FakeRequest request; prepare(request); --request.outputLength; invoke(request, STATUS_BUFFER_TOO_SMALL); assert(queries == 0); }
    reset(1, 100);
    { FakeRequest request; prepare(request)->version = 99; const auto* response = invoke(request);
      assert(response->lastStatus == STATUS_REVISION_MISMATCH && queries == 0); }
    std::printf("registry-read-bounds regression: %u cases passed; every pool read and release checked\n", cases);
}
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++", help="C++ compiler path")
    parser.add_argument("--verify-unsafe-mutation", action="store_true",
                        help="Also require removing the physical data-window cap to fail the regression")
    args = parser.parse_args()
    compiler = shutil.which(args.cxx)
    if not compiler:
        raise SystemExit(f"C++ compiler unavailable: {args.cxx}")
    production = "\n\n".join([
        function("KswordARKDriver/src/features/registry/registry_query.c", "KswordARKRegistryPrepareResponse"),
        function("KswordARKDriver/src/features/registry/registry_query.c", "KswordARKDriverReadRegistryValue"),
        function("KswordARKDriver/src/features/registry/registry_ioctl.c", "KswordARKRegistryIoctlReadValue"),
    ])
    production = re.sub(r"(?m)^#pragma warning\([^\n]+\)\s*$", "", production)
    environment = os.environ.copy()
    environment["PATH"] = str(Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
    temporary_root = ROOT / ".codex-tmp" / "registry-read-tests"
    if not temporary_root.resolve().is_relative_to(ROOT.resolve()):
        raise SystemExit("Regression temporary directory must remain inside the repository.")
    temporary_root.mkdir(parents=True, exist_ok=True)
    temporary = temporary_root / f"case-{uuid.uuid4().hex}"
    temporary.mkdir()
    try:
        source = temporary / "regression.cpp"
        binary = temporary / ("regression.exe" if os.name == "nt" else "regression")
        source.write_text(HARNESS.replace("/*PRODUCTION*/", production), encoding="utf-8")
        subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "shared"),
                        str(source), "-o", str(binary)], check=True, env=environment)
        subprocess.run([str(binary)], check=True, env=environment)
        if args.verify_unsafe_mutation:
            unsafe_statement = "response->dataBytes = availableDataBytes;"
            if production.count(unsafe_statement) != 1:
                raise SystemExit("The bounded-copy mutation target changed; update the regression explicitly.")
            unsafe_production = production.replace(unsafe_statement, "/* Deliberately omit the pool-window cap. */")
            source.write_text(HARNESS.replace("/*PRODUCTION*/", unsafe_production), encoding="utf-8")
            subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "shared"),
                            str(source), "-o", str(binary)], check=True, env=environment)
            result = subprocess.run([str(binary)], capture_output=True, text=True, env=environment)
            if result.returncode == 0 or "bytes <= poolBytes" not in result.stderr:
                raise SystemExit("The unsafe mutation was not rejected at the pool-read boundary.")
            print("registry-read-bounds unsafe mutation: rejected at the exact pool-read boundary")
    finally:
        if temporary.resolve().is_relative_to(temporary_root.resolve()):
            shutil.rmtree(temporary)


if __name__ == "__main__":
    main()
