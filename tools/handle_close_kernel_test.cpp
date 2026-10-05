// User-mode fixture for the unmodified business function from handle_close.c.
// Only its two kernel include lines are replaced by this fixture by the test runner.
#include <Windows.h>
#include <winternl.h>
#include "../shared/driver/KswordArkHandleIoctl.h"
#include <iostream>

#define NTKERNELAPI
#undef NTSYSAPI
#define NTSYSAPI
#define PASSIVE_LEVEL 0
#define UserMode 1
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_NOT_SUPPORTED ((NTSTATUS)0xC00000BBL)
#define STATUS_INVALID_DEVICE_STATE ((NTSTATUS)0xC0000184L)
#define STATUS_INVALID_CID ((NTSTATUS)0xC000000BL)
#define ULongToHandle(value) ((HANDLE)(ULONG_PTR)(value))
using PEPROCESS = PVOID;
using ULONG64 = unsigned long long;
using KSWORD_PS_SUSPEND_PROCESS_FN = NTSTATUS(NTAPI*)(PEPROCESS);
using KSWORD_PS_RESUME_PROCESS_FN = NTSTATUS(NTAPI*)(PEPROCESS);

namespace {
    constexpr NTSTATUS failed = static_cast<NTSTATUS>(0xC0000022L);
    int processToken = 0, objectToken = 0;
    int processRefs = 0, objectRefs = 0, exitLocks = 0, attachments = 0;
    unsigned suspendCalls = 0, resumeCalls = 0, closeCalls = 0, failures = 0;
    unsigned char irql = 0;
    bool available = true, raiseDuringReference = false;
    LONGLONG observedTime = 1234;
    NTSTATUS lookupStatus = 0, lockStatus = 0, suspendStatus = 0, referenceStatus = 0, closeStatus = 0, resumeStatus = 0;
    void check(bool condition, const char* label) {
        if (!condition) { std::cerr << "FAIL: " << label << '\n'; ++failures; }
    }
    void reset() {
        processRefs = objectRefs = exitLocks = attachments = 0;
        suspendCalls = resumeCalls = closeCalls = 0;
        irql = 0; available = true; raiseDuringReference = false; observedTime = 1234;
        lookupStatus = lockStatus = suspendStatus = referenceStatus = closeStatus = resumeStatus = 0;
    }
}

unsigned char KeGetCurrentIrql() { return irql; }
LONGLONG NTAPI PsGetProcessCreateTimeQuadPart(PEPROCESS) { return observedTime; }
NTSTATUS NTAPI PsLookupProcessByProcessId(HANDLE, PEPROCESS* process) {
    if (NT_SUCCESS(lookupStatus)) { *process = &processToken; ++processRefs; }
    return lookupStatus;
}
NTSTATUS NTAPI PsAcquireProcessExitSynchronization(PEPROCESS) {
    if (NT_SUCCESS(lockStatus)) { ++exitLocks; }
    return lockStatus;
}
VOID NTAPI PsReleaseProcessExitSynchronization(PEPROCESS) { --exitLocks; }
VOID KeStackAttachProcess(PVOID, PVOID) { ++attachments; }
VOID KeUnstackDetachProcess(PVOID) { --attachments; }
VOID ObDereferenceObject(PVOID object) { if (object == &processToken) { --processRefs; } else { --objectRefs; } }
NTSTATUS ObReferenceObjectByHandle(HANDLE, ULONG, PVOID, int mode, PVOID* object, PVOID) {
    check(mode == UserMode && attachments == 1, "reference resolves owner user-handle table");
    if (raiseDuringReference) { RaiseException(0xE1234567UL, 0, 0, nullptr); }
    if (NT_SUCCESS(referenceStatus)) { *object = &objectToken; ++objectRefs; }
    return referenceStatus;
}
NTSTATUS ObCloseHandle(HANDLE, int mode) {
    check(mode == UserMode && attachments == 1 && objectRefs == 1 && exitLocks == 1, "close uses pinned user object");
    ++closeCalls;
    return closeStatus;
}
NTSTATUS NTAPI suspend(PEPROCESS) { ++suspendCalls; return suspendStatus; }
NTSTATUS NTAPI resume(PEPROCESS) { check(attachments == 0, "detach before resume"); ++resumeCalls; return resumeStatus; }
KSWORD_PS_SUSPEND_PROCESS_FN KswordARKDriverResolvePsSuspendProcess() { return available ? suspend : nullptr; }
KSWORD_PS_RESUME_PROCESS_FN KswordARKDriverResolvePsResumeProcess() { return available ? resume : nullptr; }
NTSTATUS KswordARKDriverCloseHandle(const KSWORD_ARK_CLOSE_HANDLE_REQUEST*, NTSTATUS*);

int main() {
    KSWORD_ARK_CLOSE_HANDLE_REQUEST request{};
    request.processId = 123;
    request.handleValue = 0x100;
    request.expectedCreateTime100ns = 1234;
    request.expectedObjectAddress = reinterpret_cast<ULONG_PTR>(&objectToken);
    NTSTATUS recovered = failed;
    const auto run = [&]() {
        const NTSTATUS result = KswordARKDriverCloseHandle(&request, &recovered);
        check(processRefs == 0 && objectRefs == 0 && exitLocks == 0 && attachments == 0, "balanced refs/rundown/attachment");
        return result;
    };
    reset(); check(run() == 0 && recovered == 0 && closeCalls == 1 && resumeCalls == 1, "normal close and recovery");
    reset(); observedTime = 5678;
    check(run() == STATUS_INVALID_CID && suspendCalls == 0 && closeCalls == 0, "PID reuse rejected before suspend");
    reset(); request.expectedObjectAddress += 8;
    check(run() == STATUS_INVALID_HANDLE && closeCalls == 0 && resumeCalls == 1, "handle reuse rejected with recovery");
    request.expectedObjectAddress -= 8;
    reset(); suspendStatus = failed;
    check(run() == failed && closeCalls == 0 && resumeCalls == 0, "failed suspend does not resume unowned suspension");
    reset(); referenceStatus = failed;
    check(run() == failed && closeCalls == 0 && resumeCalls == 1, "reference failure resumes target");
    reset(); closeStatus = failed;
    check(run() == failed && resumeCalls == 1, "protected/denied close resumes target");
    reset(); resumeStatus = failed;
    check(run() == 0 && recovered == failed, "successful close preserves recovery failure separately");
    reset(); available = false;
    check(run() == STATUS_NOT_SUPPORTED && suspendCalls == 0, "missing recovery API never suspends target");
    reset(); lookupStatus = failed;
    check(run() == failed && suspendCalls == 0, "lookup failure leaves target untouched");
    reset(); lockStatus = failed;
    check(run() == failed && suspendCalls == 0, "exiting owner rejected");
    reset(); irql = 1;
    check(run() == STATUS_INVALID_DEVICE_STATE && suspendCalls == 0, "wrong IRQL rejected");
    reset(); raiseDuringReference = true;
    check(run() == static_cast<NTSTATUS>(0xE1234567UL) && closeCalls == 0 && resumeCalls == 1, "SEH path detaches and resumes");
    if (failures) { return 1; }
    std::cout << "Handle close kernel business regressions passed\n";
    return 0;
}
