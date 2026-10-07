#!/usr/bin/env python3
"""Compile the production SectionObject fallback and profile-preserving branch."""

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
#include <initializer_list>
#include <cstring>
#include <cwchar>
#define VOID void
#define _In_
#define _Out_
#define _Inout_
#define _In_z_
#define _In_reads_bytes_(x)
#undef __try
#define __try try
#define __except(x) catch (...)
using LONG = int32_t;
using ULONG = uint32_t;
using BOOLEAN = bool;
using UCHAR = uint8_t;
using SIZE_T = size_t;
using PEPROCESS = void*;
using PVOID = void*;
struct UNICODE_STRING { const wchar_t* Buffer; };
using KSWORD_PROCESS_PROTECTION_ACCESSOR_FN = UCHAR(*)(PEPROCESS);
using KSWORD_PROCESS_SIGNATURE_ACCESSOR_FN = UCHAR(*)(PEPROCESS, UCHAR*);
constexpr BOOLEAN TRUE = true, FALSE = false;
constexpr ULONG KSW_DYN_OFFSET_UNAVAILABLE = 0xFFFFFFFFU;
constexpr ULONG KSW_DYN_FIELD_SOURCE_UNAVAILABLE = 0U;
constexpr ULONG KSW_DYN_FIELD_SOURCE_RUNTIME_PATTERN = 6U;
struct KSW_DYN_STATE {
    struct { ULONG EpSectionObject; } Kernel;
    struct { ULONG EpSectionObject; } KernelSources;
};
static PEPROCESS process = reinterpret_cast<PEPROCESS>(uintptr_t(0xFFFF900000001000ULL));
static UCHAR signatureCode[32], protectionCode[32];
static UCHAR signatureField = 1, sectionField = 2, protectionField = 3;
static bool denyCode, denySignature, denySection, denyProtection, throwAccessor;
static unsigned accessorCalls;
static UCHAR signatureAccessor(PEPROCESS object, UCHAR* section) {
    assert(object == process); ++accessorCalls;
    if (throwAccessor) throw 1;
    *section = 2; return 1;
}
static UCHAR protectionAccessor(PEPROCESS object) {
    assert(object == process); ++accessorCalls;
    if (throwAccessor) throw 1;
    return 3;
}
static PEPROCESS PsGetCurrentProcess() { return process; }
static void RtlInitUnicodeString(UNICODE_STRING* name, const wchar_t* value) { name->Buffer = value; }
static PVOID MmGetSystemRoutineAddress(const UNICODE_STRING* name) {
    return std::wcscmp(name->Buffer, L"PsGetProcessSignatureLevel") == 0
        ? reinterpret_cast<PVOID>(signatureAccessor) : reinterpret_cast<PVOID>(protectionAccessor);
}
static void RtlZeroMemory(PVOID address, SIZE_T size) { std::memset(address, 0, size); }
static void RtlCopyMemory(PVOID destination, const void* source, SIZE_T size) { std::memcpy(destination, source, size); }
static BOOLEAN KswordARKRuntimeReadMemory(const void* address, PVOID destination, SIZE_T size) {
    if (address == reinterpret_cast<PVOID>(signatureAccessor) || address == reinterpret_cast<PVOID>(protectionAccessor)) {
        assert(size == 32);
        if (denyCode) return false;
        std::memcpy(destination, address == reinterpret_cast<PVOID>(signatureAccessor) ? signatureCode : protectionCode, size);
        return true;
    }
    assert(size == 1);
    const uintptr_t offset = reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(process);
    if (offset == 0x300) { if (denySignature) return false; *static_cast<UCHAR*>(destination) = signatureField; }
    else if (offset == 0x301) { if (denySection) return false; *static_cast<UCHAR*>(destination) = sectionField; }
    else if (offset == 0x303) { if (denyProtection) return false; *static_cast<UCHAR*>(destination) = protectionField; }
    else assert(false);
    return true;
}
static void resetAccessors() {
    denyCode = denySignature = denySection = denyProtection = throwAccessor = false;
    accessorCalls = 0; signatureField = 1; sectionField = 2; protectionField = 3;
    std::memset(signatureCode, 0, sizeof(signatureCode));
    std::memset(protectionCode, 0, sizeof(protectionCode));
    signatureCode[0] = signatureCode[8] = protectionCode[0] = 0x8A;
    signatureCode[1] = signatureCode[9] = protectionCode[1] = 0x81;
    LONG offset = 0x300; std::memcpy(signatureCode + 2, &offset, sizeof(offset)); signatureCode[6] = 0xC3;
    offset = 0x301; std::memcpy(signatureCode + 10, &offset, sizeof(offset)); signatureCode[14] = 0x88; signatureCode[15] = 0x02;
    offset = 0x303; std::memcpy(protectionCode + 2, &offset, sizeof(offset)); protectionCode[6] = 0xC3;
}
/*PRODUCTION*/
static unsigned resolverCalls;
static LONG instrumentedResolver() {
    ++resolverCalls;
    return KswordARKDriverResolveProcessSectionObjectOffset();
}
#define KswordARKDriverResolveProcessSectionObjectOffset instrumentedResolver
static void apply(KSW_DYN_STATE* State) {
/*CALLER*/
}
#undef KswordARKDriverResolveProcessSectionObjectOffset
int main() {
    assert(KswordARKDriverResolveProcessSectionObjectOffset() == -1);
    unsigned cases = 1;
    for (const ULONG offset : {0U, 0x310U, 0x400U}) {
        KSW_DYN_STATE state{{offset}, {7U}};
        resolverCalls = 0; apply(&state);
        assert(state.Kernel.EpSectionObject == offset);
        assert(state.KernelSources.EpSectionObject == 7U);
        assert(resolverCalls == 0); ++cases;
    }
    for (const ULONG offset : {KSW_DYN_OFFSET_UNAVAILABLE, 0x0000FFFFU}) {
        KSW_DYN_STATE state{{offset}, {KSW_DYN_FIELD_SOURCE_UNAVAILABLE}};
        resolverCalls = 0; apply(&state);
        assert(state.Kernel.EpSectionObject == offset);
        assert(state.KernelSources.EpSectionObject == KSW_DYN_FIELD_SOURCE_UNAVAILABLE);
        assert(resolverCalls == 1); ++cases;
    }
    ULONG valid = 0x310U, source = 7U;
    assert(KswordARKDynDataStoreRuntimeOffset(-1, &valid, &source));
    assert(valid == 0x310U && source == 7U); ++cases;
    assert(!KswordARKDynDataStoreRuntimeOffset(-1, nullptr, &source)); ++cases;
    assert(!KswordARKDynDataStoreRuntimeOffset(-1, &valid, nullptr)); ++cases;
    LONG signatureOffset = -1, sectionOffset = -1;
    resetAccessors();
    assert(KswordARKDriverResolveProcessSignatureOffsets(&signatureOffset, &sectionOffset));
    assert(signatureOffset == 0x300 && sectionOffset == 0x301); ++cases;
    assert(KswordARKDriverResolveProcessProtectionOffset() == 0x303); ++cases;
    resetAccessors(); denyCode = true;
    assert(!KswordARKDriverResolveProcessSignatureOffsets(&signatureOffset, &sectionOffset));
    assert(signatureOffset == -1 && sectionOffset == -1 && accessorCalls == 0); ++cases;
    assert(KswordARKDriverResolveProcessProtectionOffset() == -1 && accessorCalls == 0); ++cases;
    resetAccessors(); denySignature = true;
    assert(!KswordARKDriverResolveProcessSignatureOffsets(&signatureOffset, &sectionOffset));
    assert(signatureOffset == -1 && sectionOffset == -1); ++cases;
    resetAccessors(); denySection = true;
    assert(!KswordARKDriverResolveProcessSignatureOffsets(&signatureOffset, &sectionOffset));
    assert(signatureOffset == -1 && sectionOffset == -1); ++cases;
    resetAccessors(); signatureField = 9;
    assert(!KswordARKDriverResolveProcessSignatureOffsets(&signatureOffset, &sectionOffset)); ++cases;
    resetAccessors(); sectionField = 9;
    assert(!KswordARKDriverResolveProcessSignatureOffsets(&signatureOffset, &sectionOffset)); ++cases;
    resetAccessors(); denyProtection = true;
    assert(KswordARKDriverResolveProcessProtectionOffset() == -1); ++cases;
    resetAccessors(); protectionField = 9;
    assert(KswordARKDriverResolveProcessProtectionOffset() == -1); ++cases;
    resetAccessors(); throwAccessor = true;
    assert(!KswordARKDriverResolveProcessSignatureOffsets(&signatureOffset, &sectionOffset)); ++cases;
    assert(KswordARKDriverResolveProcessProtectionOffset() == -1); ++cases;
    std::printf("section-object fallback regression: %u cases passed\n", cases);
}
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    compiler = shutil.which(args.cxx)
    if not compiler:
        raise SystemExit(f"C++ compiler unavailable: {args.cxx}")
    loader = "KswordARKDriver/src/features/dyndata/dyndata_loader.c"
    resolver = function("KswordARKDriver/src/platform/process_resolver.c", "KswordARKDriverResolveProcessSectionObjectOffset")
    # The unsafe APIs must not be dependencies of the compiled production body.
    body = resolver[resolver.index("\n{"):]
    assert "ObGetObjectType(" not in body and "RtlCopyMemory(" not in body
    text = (ROOT / loader).read_text(encoding="utf-8-sig")
    start = text.index("    if (!KswordARKDynDataOffsetPresent(State->Kernel.EpSectionObject))")
    end = text.index("\n    }", start) + len("\n    }")
    caller = text[start:end]
    production = "\n\n".join([
        resolver,
        function(loader, "KswordARKDynDataOffsetPresent"),
        function(loader, "KswordARKDynDataStoreSourcedOffset"),
        function(loader, "KswordARKDynDataStoreRuntimeOffset"),
        function("KswordARKDriver/src/platform/process_resolver.c", "KswordARKDriverDecodeReturnedUcharOffset"),
        function("KswordARKDriver/src/platform/process_resolver.c", "KswordARKDriverResolveProcessSignatureOffsets"),
        function("KswordARKDriver/src/platform/process_resolver.c", "KswordARKDriverResolveProcessProtectionOffset"),
    ])
    environment = os.environ.copy()
    environment["PATH"] = str(Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
    temporary_root = ROOT / ".codex-tmp" / "sectionobject-tests"
    if not temporary_root.resolve().is_relative_to(ROOT.resolve()):
        raise SystemExit("Regression scratch path must remain inside the repository.")
    temporary = temporary_root / f"case-{uuid.uuid4().hex}"
    temporary.mkdir(parents=True)
    try:
        source = temporary / "regression.cpp"
        executable = temporary / ("regression.exe" if os.name == "nt" else "regression")
        source.write_text(HARNESS.replace("/*PRODUCTION*/", production).replace("/*CALLER*/", caller), encoding="utf-8")
        subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(executable)],
                       check=True, env=environment)
        subprocess.run([str(executable)], check=True, env=environment)
    finally:
        if temporary.resolve().is_relative_to(temporary_root.resolve()):
            shutil.rmtree(temporary)


if __name__ == "__main__":
    main()
