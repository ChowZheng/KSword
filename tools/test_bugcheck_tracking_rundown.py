#!/usr/bin/env python3
"""Exercise production BugCheck writer rundown without loading a driver."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import uuid

from test_driver_candidate_reference import ROOT, function


HARNESS = r"""
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <thread>
#define _In_
#define _In_opt_
#define _Inout_opt_
#define VOID void
using LONG = int32_t;
using ULONG = uint32_t;
using ULONG64 = uint64_t;
using ULONG_PTR = uintptr_t;
using USHORT = uint16_t;
using CHAR = char;
using BOOLEAN = bool;
using NTSTATUS = int;
using PVOID = void*;
using HANDLE = void*;
using PEPROCESS = void*;
using PUNICODE_STRING = void*;
using PPS_CREATE_NOTIFY_INFO = void*;
constexpr bool TRUE = true, FALSE = false;
constexpr int PASSIVE_LEVEL = 0, APC_LEVEL = 1;
constexpr NTSTATUS STATUS_NOT_FOUND = -1, STATUS_SUCCESS = 0;
constexpr ULONG MAXULONG = UINT32_MAX;
constexpr int KSWORD_ARK_BUGCHECK_MODULE_NAME_CHARS = 64;
constexpr int KSWORD_BUGCHECK_EVIDENCE_STACK = 8;
#define RTL_NUMBER_OF(x) (sizeof(x) / sizeof((x)[0]))
#define UNREFERENCED_PARAMETER(x) ((void)(x))
#undef __try
#define __try try
#define __except(x) catch (...)
static int GetExceptionCode() { return -99; }
struct IMAGE_INFO { bool SystemModeImage; PVOID ImageBase; size_t ImageSize; };
using PIMAGE_INFO = IMAGE_INFO*;
struct EX_RUNDOWN_REF {
    std::mutex lock;
    std::condition_variable changed;
    bool initialized = false, closed = false;
    unsigned references = 0;
};
static EX_RUNDOWN_REF g_KswordArkBugcheckTrackingRundown;
static volatile LONG g_KswordArkBugcheckTrackingInitialized = 0;
static volatile LONG g_KswordArkBugcheckTrackingAccepting = 0;
static struct { volatile LONG TrackingReady = 0; } g_KswordArkBugcheckState;
static thread_local int irql = 0;
static volatile LONG gStackWriter = 0, gStackSequence = 0;
static struct {
    ULONG Count = 0;
    NTSTATUS Status = STATUS_NOT_FOUND;
    ULONG64 ThreadId = 0, Time = 0, Frames[KSWORD_BUGCHECK_EVIDENCE_STACK] = {};
} gOperationStack;
static std::atomic<int> moduleWrites{0}, imageWrites{0}, processWrites{0}, captures{0};
static bool validName = true;
static std::function<void()> beforeAcquire, afterAcquire, publisherHook, captureHook;
static std::mutex phaseLock;
static std::condition_variable phaseChanged;
static bool waitEntered = false;
static int KeGetCurrentIrql() { return irql; }
static LONG InterlockedExchange(volatile LONG* value, LONG newValue) {
    return __atomic_exchange_n(value, newValue, __ATOMIC_SEQ_CST);
}
static LONG InterlockedCompareExchange(volatile LONG* value, LONG newValue, LONG comparand) {
    __atomic_compare_exchange_n(value, &comparand, newValue, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return comparand;
}
static LONG InterlockedIncrement(volatile LONG* value) { return __atomic_add_fetch(value, 1, __ATOMIC_SEQ_CST); }
static void KeMemoryBarrier() { std::atomic_thread_fence(std::memory_order_seq_cst); }
static void ExInitializeRundownProtection(EX_RUNDOWN_REF* ref) {
    std::lock_guard<std::mutex> guard(ref->lock);
    assert(!ref->initialized); ref->initialized = true;
}
static void ExWaitForRundownProtectionRelease(EX_RUNDOWN_REF* ref) {
    std::unique_lock<std::mutex> guard(ref->lock);
    assert(ref->initialized); ref->closed = true;
    { std::lock_guard<std::mutex> phase(phaseLock); waitEntered = true; }
    phaseChanged.notify_all();
    ref->changed.wait(guard, [&] { return ref->references == 0; });
}
static void ExReInitializeRundownProtection(EX_RUNDOWN_REF* ref) {
    std::lock_guard<std::mutex> guard(ref->lock);
    assert(ref->initialized && ref->closed && ref->references == 0);
    ref->closed = false;
}
static BOOLEAN ExAcquireRundownProtection(EX_RUNDOWN_REF* ref) {
    if (beforeAcquire) beforeAcquire();
    {
        std::lock_guard<std::mutex> guard(ref->lock);
        assert(ref->initialized);
        if (ref->closed) return false;
        ++ref->references;
    }
    if (afterAcquire) afterAcquire();
    return true;
}
static void ExReleaseRundownProtection(EX_RUNDOWN_REF* ref) {
    std::lock_guard<std::mutex> guard(ref->lock);
    assert(ref->references != 0); --ref->references; ref->changed.notify_all();
}
static void assertProtected() {
    std::lock_guard<std::mutex> guard(g_KswordArkBugcheckTrackingRundown.lock);
    assert(g_KswordArkBugcheckTrackingRundown.references != 0);
}
static BOOLEAN KswordARKBugcheckCopyUnicodeBaseNameA(PUNICODE_STRING, CHAR* name, ULONG) {
    assertProtected(); name[0] = 'x'; name[1] = '\0'; return validName;
}
static void KswordARKBugcheckPublishModule(ULONG_PTR, ULONG, const CHAR*) {
    assertProtected(); ++moduleWrites; if (publisherHook) publisherHook();
}
static void KswordARKBugcheckEvidenceImage(ULONG_PTR, ULONG, PUNICODE_STRING) {
    assertProtected(); ++imageWrites;
}
static void KswordARKBugcheckPublishProcess(PEPROCESS, HANDLE, BOOLEAN) {
    assertProtected(); ++processWrites; if (publisherHook) publisherHook();
}
static USHORT RtlCaptureStackBackTrace(ULONG, ULONG, PVOID* frames, PVOID) {
    assertProtected(); ++captures; if (captureHook) captureHook(); frames[0] = reinterpret_cast<PVOID>(uintptr_t(55)); return 1;
}
static PVOID PsGetCurrentThreadId() { return reinterpret_cast<PVOID>(uintptr_t(22)); }
static ULONG64 KeQueryInterruptTime() { return 99; }
/*PRODUCTION*/
static void assertDrained() {
    std::lock_guard<std::mutex> guard(g_KswordArkBugcheckTrackingRundown.lock);
    assert(g_KswordArkBugcheckTrackingRundown.closed && g_KswordArkBugcheckTrackingRundown.references == 0);
}
static void concurrentStop(std::function<void()> writer, std::function<void()>& hook) {
    bool writerEntered = false, resumeWriter = false;
    std::atomic<bool> stopReturned{false};
    { std::lock_guard<std::mutex> phase(phaseLock); waitEntered = false; }
    hook = [&] {
        std::unique_lock<std::mutex> phase(phaseLock);
        writerEntered = true; phaseChanged.notify_all();
        phaseChanged.wait(phase, [&] { return resumeWriter; });
    };
    std::thread active(writer);
    { std::unique_lock<std::mutex> phase(phaseLock); phaseChanged.wait(phase, [&] { return writerEntered; }); }
    std::thread stopping([&] { KswordARKBugcheckTrackingStop(); stopReturned = true; });
    {
        std::unique_lock<std::mutex> phase(phaseLock);
        phaseChanged.wait(phase, [&] { return waitEntered; });
        assert(!stopReturned && g_KswordArkBugcheckTrackingAccepting == 0);
        resumeWriter = true;
    }
    phaseChanged.notify_all(); active.join(); stopping.join(); hook = nullptr;
    assert(stopReturned); assertDrained();
}
int main() {
    assert(!KswordARKBugcheckTrackingAcquire());
    KswordARKBugcheckContextOperation(); assert(captures == 0);
    IMAGE_INFO image{true, reinterpret_cast<PVOID>(uintptr_t(4096)), 4096};
    KswordARKBugcheckTrackLoadedImage(nullptr, nullptr, &image); assert(moduleWrites == 0);
    KswordARKBugcheckTrackProcess(nullptr, nullptr, nullptr); assert(processWrites == 0);
    KswordARKBugcheckTrackingInitialize(); assertDrained();
    assert(!KswordARKBugcheckTrackingAcquire());
    KswordARKBugcheckTrackingStart();
    KswordARKBugcheckTrackLoadedImage(nullptr, nullptr, &image); assert(moduleWrites == 1 && imageWrites == 1);
    KswordARKBugcheckTrackProcess(nullptr, nullptr, nullptr); assert(processWrites == 1);
    KswordARKBugcheckContextOperation(); assert(captures == 1 && gOperationStack.Count == 1);
    validName = false; KswordARKBugcheckTrackLoadedImage(nullptr, nullptr, &image); assert(moduleWrites == 1); validName = true;
    gStackWriter = 1; KswordARKBugcheckContextOperation(); assert(captures == 1); gStackWriter = 0;
    captureHook = [] { throw 1; }; KswordARKBugcheckContextOperation(); captureHook = nullptr;
    assert(gOperationStack.Status == -99 && gOperationStack.Count == 0 && gStackWriter == 0);
    irql = APC_LEVEL + 1; assert(!KswordARKBugcheckTrackingAcquire()); irql = 0;
    beforeAcquire = [] { KswordARKBugcheckTrackingStop(); };
    assert(!KswordARKBugcheckTrackingAcquire()); beforeAcquire = nullptr; assertDrained();
    KswordARKBugcheckTrackingStart();
    afterAcquire = [] { InterlockedExchange(&g_KswordArkBugcheckTrackingAccepting, 0); };
    assert(!KswordARKBugcheckTrackingAcquire()); afterAcquire = nullptr;
    KswordARKBugcheckTrackingStop(); assertDrained();
    KswordARKBugcheckTrackingStart();
    beforeAcquire = [] { KswordARKBugcheckTrackingStop(); assertDrained(); KswordARKBugcheckTrackingStart(); };
    assert(KswordARKBugcheckTrackingAcquire()); beforeAcquire = nullptr; KswordARKBugcheckTrackingRelease();
    for (int round = 0; round < 20; ++round) {
        concurrentStop([&] { KswordARKBugcheckTrackLoadedImage(nullptr, nullptr, &image); }, publisherHook);
        KswordARKBugcheckTrackingStart();
        concurrentStop([&] { KswordARKBugcheckTrackProcess(nullptr, nullptr, nullptr); }, publisherHook);
        KswordARKBugcheckTrackingStart();
        concurrentStop([] { KswordARKBugcheckContextOperation(); }, captureHook);
        assert(gStackWriter == 0); KswordARKBugcheckTrackingStart();
    }
    KswordARKBugcheckTrackingStop(); assertDrained();
    KswordARKBugcheckTrackingStop(); assertDrained();
    const int written = moduleWrites;
    KswordARKBugcheckTrackLoadedImage(nullptr, nullptr, &image); assert(moduleWrites == written);
    std::puts("bugcheck tracking regression: lifecycle, stale admission, 60 concurrent drains passed");
}
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    compiler = shutil.which(args.cxx)
    if not compiler:
        raise SystemExit(f"C++ compiler unavailable: {args.cxx}")
    control = "KswordARKDriver/src/features/bugcheck/bugcheck_control.c"
    runtime = "KswordARKDriver/src/features/bugcheck/bugcheck_runtime.c"
    context = "KswordARKDriver/src/features/bugcheck/bugcheck_context.c"
    initialization = function(runtime, "KswordARKBugcheckInitialize")
    assert initialization.index("KswordARKBugcheckTrackingStop();") < initialization.index("RtlZeroMemory(&g_KswordArkBugcheckState")
    assert initialization.index("KswordARKBugcheckEvidenceInitialize") < initialization.index("KswordARKBugcheckTrackingStart();")
    teardown = function(runtime, "KswordARKBugcheckUninitialize")
    assert teardown.index("KswordARKBugcheckTrackingStop();") < teardown.index("KeDeregisterBugCheckReasonCallback")
    control_initialization = function(control, "KswordARKBugcheckControlInitialize")
    assert control_initialization.index("KswordARKBugcheckTrackingInitialize();") < control_initialization.index("InterlockedExchange(&g_KswordArkBugcheckControlReady, 1L)")
    production = "\n\n".join([
        *(function(control, f"KswordARKBugcheckTracking{name}") for name in ["Initialize", "Start", "Stop", "Acquire", "Release"]),
        function(runtime, "KswordARKBugcheckTrackLoadedImage"),
        function(runtime, "KswordARKBugcheckTrackProcess"),
        function(context, "KswordARKBugcheckContextOperation"),
    ])
    environment = os.environ.copy()
    environment["PATH"] = str(Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
    temporary_root = ROOT / ".codex-tmp" / "bugcheck-tracking-tests"
    if not temporary_root.resolve().is_relative_to(ROOT.resolve()):
        raise SystemExit("Regression scratch path must remain inside the repository.")
    temporary = temporary_root / f"case-{uuid.uuid4().hex}"
    temporary.mkdir(parents=True)
    try:
        source = temporary / "regression.cpp"
        executable = temporary / ("regression.exe" if os.name == "nt" else "regression")
        source.write_text(HARNESS.replace("/*PRODUCTION*/", production), encoding="utf-8")
        subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread", str(source), "-o", str(executable)],
                       check=True, env=environment)
        subprocess.run([str(executable)], check=True, env=environment, timeout=30)
    finally:
        if temporary.resolve().is_relative_to(temporary_root.resolve()):
            shutil.rmtree(temporary)


if __name__ == "__main__":
    main()
