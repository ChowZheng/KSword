#include <ntddk.h>
#include <ntimage.h>
#include <basetsd.h>
#include <stdio.h>
#include <string.h>
#include "../KswordARKDriver/src/features/bugcheck/bugcheck_evidence.h"

// Redirect only operating-system entry points. WDK structures and production logic stay real.
static KIRQL gReplayIrql = PASSIVE_LEVEL;
static ULONG64 gReplayTime = 1234567ULL;
static ULONG_PTR gReplayThread = 0x1234;
static ULONG_PTR gReplayProcess = 0x5678;
static ULONG gReplayCaptureCalls;
static ULONG gReplayCaptureSkip;
static ULONG gReplayCaptureLimit;
static USHORT gReplayFrames = 16;
static BOOLEAN gReplayRaise;
static ULONG gChecks;
static ULONG gFailures;

__declspec(dllimport) VOID __stdcall RaiseException(ULONG, ULONG, ULONG, const ULONG_PTR*);
static KIRQL ReplayIrql(VOID) { return gReplayIrql; }
static ULONG64 ReplayTime(VOID) { return gReplayTime; }
static HANDLE ReplayThread(VOID) { return (HANDLE)gReplayThread; }
static HANDLE ReplayProcess(VOID) { return (HANDLE)gReplayProcess; }
static USHORT NTAPI ReplayCapture(ULONG Skip, ULONG Limit, PVOID* Frames, PULONG Hash)
{
    ULONG index;
    gReplayCaptureCalls += 1;
    gReplayCaptureSkip = Skip;
    gReplayCaptureLimit = Limit;
    if (Hash != NULL) *Hash = 0;
    if (gReplayRaise) RaiseException((ULONG)STATUS_ACCESS_VIOLATION, 0, 0, NULL);
    for (index = 0; index < Limit && index < gReplayFrames; ++index)
        Frames[index] = (PVOID)(ULONG_PTR)(0xFFFF800000001000ULL + (ULONG64)index * 0x100ULL);
    return gReplayFrames;
}

#undef KeQueryInterruptTime
#define KeQueryInterruptTime ReplayTime
#define KeGetCurrentIrql ReplayIrql
#define PsGetCurrentThreadId ReplayThread
#define PsGetCurrentProcessId ReplayProcess
#define RtlCaptureStackBackTrace ReplayCapture
// The production declaration describes an import; this unit supplies the OS mock.
#undef NTSYSAPI
#define NTSYSAPI
#include "../KswordARKDriver/src/features/bugcheck/bugcheck_context.c"
#include "../KswordARKDriver/src/features/bugcheck/bugcheck_trace.c"

#define EXPECT(Expression) Check((Expression) ? TRUE : FALSE, #Expression, __LINE__)
#define REPLAY_CONTEXT_OFFSET 0x348UL
#define REPLAY_PREFIX_BYTES (REPLAY_CONTEXT_OFFSET + FIELD_OFFSET(CONTEXT, Rip) + sizeof(ULONG64))
#define REPLAY_ALL_REGISTERS ((1UL << KSWORD_BUGCHECK_EVIDENCE_REGISTERS) - 1UL)
static VOID Check(BOOLEAN Passed, PCSTR Expression, ULONG Line)
{
    gChecks += 1;
    if (!Passed) {
        gFailures += 1;
        printf("FAIL line=%lu: %s\n", Line, Expression);
    }
}

static VOID MakeDump(UCHAR* Bytes, ULONG Flags, ULONG64* Expected)
{
    CONTEXT context;
    ULONG machine = IMAGE_FILE_MACHINE_AMD64;
    RtlZeroMemory(&context, sizeof(context));
    RtlZeroMemory(Bytes, REPLAY_PREFIX_BYTES);
    context.ContextFlags = Flags;
    context.Rax = 0; // A valid zero must still have an availability bit.
    context.Rbx = 0x2222222222222222ULL;
    context.Rcx = 0x3333333333333333ULL;
    context.Rdx = 0x4444444444444444ULL;
    context.Rsi = 0x5555555555555555ULL;
    context.Rdi = 0x6666666666666666ULL;
    context.Rbp = 0x7777777777777777ULL;
    context.Rsp = 0x8888888888888888ULL;
    context.R8 = 0x9999999999999999ULL;
    context.R9 = 0xAAAAAAAAAAAAAAAAULL;
    context.R10 = 0xBBBBBBBBBBBBBBBBULL;
    context.R11 = 0xCCCCCCCCCCCCCCCCULL;
    context.R12 = 0xDDDDDDDDDDDDDDDDULL;
    context.R13 = 0xEEEEEEEEEEEEEEEEULL;
    context.R14 = 0xFFFFFFFFFFFFFFFFULL;
    context.R15 = 0x1111222233334444ULL;
    context.Rip = 0xFFFFF800AABBCCDDULL;
    context.EFlags = 0x202;
    memcpy(Bytes, "PAGEDU64", 8);
    memcpy(Bytes + 0x30, &machine, sizeof(machine));
    memcpy(Bytes + REPLAY_CONTEXT_OFFSET, &context, REPLAY_PREFIX_BYTES - REPLAY_CONTEXT_OFFSET);
    Expected[0] = context.Rax; Expected[1] = context.Rbx;
    Expected[2] = context.Rcx; Expected[3] = context.Rdx;
    Expected[4] = context.Rsi; Expected[5] = context.Rdi;
    Expected[6] = context.Rbp; Expected[7] = context.Rsp;
    Expected[8] = context.R8; Expected[9] = context.R9;
    Expected[10] = context.R10; Expected[11] = context.R11;
    Expected[12] = context.R12; Expected[13] = context.R13;
    Expected[14] = context.R14; Expected[15] = context.R15;
    Expected[16] = context.Rip; Expected[17] = context.EFlags;
}

static VOID DumpPart(const UCHAR* Bytes, ULONG Position, ULONG Length, LONGLONG Offset, KBUGCHECK_DUMP_IO_TYPE Type)
{
    KBUGCHECK_DUMP_IO io;
    RtlZeroMemory(&io, sizeof(io));
    io.Offset = (ULONG64)Offset;
    io.Buffer = (PVOID)(Bytes + Position);
    io.BufferLength = Length;
    io.Type = Type;
    KswordARKBugcheckEvidenceDumpIo(&io);
}

static KSWORD_BUGCHECK_EVIDENCE ContextSnapshot(ULONG_PTR Thread)
{
    KSWORD_BUGCHECK_EVIDENCE evidence;
    RtlZeroMemory(&evidence, sizeof(evidence));
    evidence.ThreadId = Thread;
    KswordARKBugcheckContextSnapshot(&evidence);
    return evidence;
}

static VOID CheckRegisters(const KSWORD_BUGCHECK_EVIDENCE* Evidence, ULONG Mask, const ULONG64* Expected)
{
    ULONG index;
    EXPECT(Evidence->Context.RegisterMask == Mask);
    EXPECT(Evidence->Context.ContextSource == (Mask != 0 ? 1UL : 0UL));
    for (index = 0; index < KSWORD_BUGCHECK_EVIDENCE_REGISTERS; ++index)
        EXPECT(Evidence->Context.Registers[index] == ((Mask & (1UL << index)) != 0 ? Expected[index] : 0ULL));
}

static VOID TestDumpContext(VOID)
{
    UCHAR bytes[REPLAY_PREFIX_BYTES];
    ULONG64 expected[KSWORD_BUGCHECK_EVIDENCE_REGISTERS];
    KSWORD_BUGCHECK_EVIDENCE evidence;
    ULONG position;
    ULONG length;
    ULONG flags;
    ULONG hole = REPLAY_CONTEXT_OFFSET + FIELD_OFFSET(CONTEXT, Rbx) + 3UL;
    ULONG controlMask = (1UL << 7) | (1UL << 16) | (1UL << 17);
    printf("CASE dump: PAGE/DU64 fragments, holes, flags, generations\n");
    MakeDump(bytes, CONTEXT_CONTROL | CONTEXT_INTEGER, expected);
    KswordARKBugcheckContextReset();
    evidence = ContextSnapshot(gReplayThread);
    EXPECT(evidence.Context.ContextStatus == STATUS_NOT_FOUND);
    CheckRegisters(&evidence, 0, expected);

    // Start with an explicit position, then split fields into arbitrary sequential chunks.
    DumpPart(bytes, 0, 3, 0, KbDumpIoHeader);
    position = 3;
    while (position < REPLAY_PREFIX_BYTES) {
        length = (position % 19UL) + 1UL;
        if (length > REPLAY_PREFIX_BYTES - position) length = REPLAY_PREFIX_BYTES - position;
        DumpPart(bytes, position, length, -1LL, KbDumpIoHeader);
        position += length;
    }
    evidence = ContextSnapshot(gReplayThread);
    EXPECT(evidence.Context.ContextStatus == STATUS_SUCCESS);
    CheckRegisters(&evidence, REPLAY_ALL_REGISTERS, expected);

    KswordARKBugcheckContextReset();
    DumpPart(bytes, 0, hole, 0, KbDumpIoHeader);
    DumpPart(bytes, hole + 1UL, (ULONG)REPLAY_PREFIX_BYTES - hole - 1UL, (LONGLONG)hole + 1LL, KbDumpIoHeader);
    evidence = ContextSnapshot(gReplayThread);
    CheckRegisters(&evidence, REPLAY_ALL_REGISTERS & ~(1UL << 1), expected);
    DumpPart(bytes, hole, 1, hole, KbDumpIoHeader);
    evidence = ContextSnapshot(gReplayThread);
    CheckRegisters(&evidence, REPLAY_ALL_REGISTERS, expected);

    // Missing a required signature byte never permits context interpretation.
    KswordARKBugcheckContextReset();
    DumpPart(bytes, 0, 3, 0, KbDumpIoHeader);
    DumpPart(bytes, 4, (ULONG)REPLAY_PREFIX_BYTES - 4UL, 4, KbDumpIoHeader);
    evidence = ContextSnapshot(gReplayThread);
    EXPECT(evidence.Context.ContextStatus == STATUS_NOT_FOUND);
    CheckRegisters(&evidence, 0, expected);

    MakeDump(bytes, CONTEXT_CONTROL, expected);
    KswordARKBugcheckContextReset();
    DumpPart(bytes, 0, (ULONG)REPLAY_PREFIX_BYTES, 0, KbDumpIoHeader);
    evidence = ContextSnapshot(gReplayThread);
    CheckRegisters(&evidence, controlMask, expected);
    MakeDump(bytes, CONTEXT_INTEGER, expected);
    DumpPart(bytes, 0, (ULONG)REPLAY_PREFIX_BYTES, 0, KbDumpIoHeader);
    evidence = ContextSnapshot(gReplayThread);
    CheckRegisters(&evidence, REPLAY_ALL_REGISTERS & ~controlMask, expected);

    // Updating flags must revoke registers from groups no longer declared valid.
    MakeDump(bytes, CONTEXT_CONTROL | CONTEXT_INTEGER, expected);
    DumpPart(bytes, 0, (ULONG)REPLAY_PREFIX_BYTES, 0, KbDumpIoHeader);
    flags = CONTEXT_CONTROL;
    memcpy(bytes + REPLAY_CONTEXT_OFFSET + FIELD_OFFSET(CONTEXT, ContextFlags), &flags, sizeof(flags));
    DumpPart(bytes, REPLAY_CONTEXT_OFFSET + FIELD_OFFSET(CONTEXT, ContextFlags), sizeof(flags),
        REPLAY_CONTEXT_OFFSET + FIELD_OFFSET(CONTEXT, ContextFlags), KbDumpIoHeader);
    evidence = ContextSnapshot(gReplayThread);
    CheckRegisters(&evidence, controlMask, expected);
    flags = 0;
    memcpy(bytes + REPLAY_CONTEXT_OFFSET + FIELD_OFFSET(CONTEXT, ContextFlags), &flags, sizeof(flags));
    DumpPart(bytes, REPLAY_CONTEXT_OFFSET + FIELD_OFFSET(CONTEXT, ContextFlags), sizeof(flags),
        REPLAY_CONTEXT_OFFSET + FIELD_OFFSET(CONTEXT, ContextFlags), KbDumpIoHeader);
    evidence = ContextSnapshot(gReplayThread);
    EXPECT(evidence.Context.ContextStatus == STATUS_NOT_SUPPORTED);
    CheckRegisters(&evidence, 0, expected);

    MakeDump(bytes, CONTEXT_CONTROL | CONTEXT_INTEGER, expected);
    DumpPart(bytes, 0, (ULONG)REPLAY_PREFIX_BYTES, 0, KbDumpIoHeader);
    memcpy(bytes + 4, "NOPE", 4);
    DumpPart(bytes, 4, 4, 4, KbDumpIoHeader);
    evidence = ContextSnapshot(gReplayThread);
    EXPECT(evidence.Context.ContextStatus == STATUS_INVALID_IMAGE_FORMAT);
    CheckRegisters(&evidence, 0, expected);
    MakeDump(bytes, CONTEXT_CONTROL | CONTEXT_INTEGER, expected);
    bytes[0x30] = 0x4C; bytes[0x31] = 0x01; // x86 is rejected by the AMD64 decoder.
    DumpPart(bytes, 0, (ULONG)REPLAY_PREFIX_BYTES, 0, KbDumpIoHeader);
    evidence = ContextSnapshot(gReplayThread);
    EXPECT(evidence.Context.ContextStatus == STATUS_INVALID_IMAGE_FORMAT);
    CheckRegisters(&evidence, 0, expected);
    MakeDump(bytes, 0x00010003UL, expected); // Valid-looking groups for a different architecture.
    DumpPart(bytes, 0, (ULONG)REPLAY_PREFIX_BYTES, 0, KbDumpIoHeader);
    evidence = ContextSnapshot(gReplayThread);
    EXPECT(evidence.Context.ContextStatus == STATUS_NOT_SUPPORTED);
    CheckRegisters(&evidence, 0, expected);

    // An offset-zero fragment begins a new generation, with no previous tail reuse.
    MakeDump(bytes, CONTEXT_CONTROL | CONTEXT_INTEGER, expected);
    DumpPart(bytes, 0, (ULONG)REPLAY_PREFIX_BYTES, 0, KbDumpIoHeader);
    DumpPart(bytes, 0, 8, 0, KbDumpIoHeader);
    evidence = ContextSnapshot(gReplayThread);
    EXPECT(evidence.Context.ContextStatus == STATUS_NOT_FOUND);
    CheckRegisters(&evidence, 0, expected);
    DumpPart(bytes, 8, (ULONG)REPLAY_PREFIX_BYTES - 8UL, -1LL, KbDumpIoHeader);
    evidence = ContextSnapshot(gReplayThread);
    CheckRegisters(&evidence, REPLAY_ALL_REGISTERS, expected);

    KswordARKBugcheckContextReset();
    DumpPart(bytes, 0, (ULONG)REPLAY_PREFIX_BYTES, -2LL, KbDumpIoHeader);
    DumpPart(bytes, 0, (ULONG)REPLAY_PREFIX_BYTES, 0, KbDumpIoBody);
    DumpPart(bytes, 0, 0, 0, KbDumpIoHeader);
    KswordARKBugcheckEvidenceDumpIo(NULL);
    evidence = ContextSnapshot(gReplayThread);
    EXPECT(evidence.Context.ContextStatus == STATUS_NOT_FOUND);
    CheckRegisters(&evidence, 0, expected);
    InterlockedExchange(&gContextWriter, 1);
    DumpPart(bytes, 0, (ULONG)REPLAY_PREFIX_BYTES, 0, KbDumpIoHeader);
    InterlockedExchange(&gContextWriter, 0);
    evidence = ContextSnapshot(gReplayThread);
    CheckRegisters(&evidence, 0, expected);
    InterlockedExchange(&gContextSequence, 1);
    evidence = ContextSnapshot(gReplayThread);
    EXPECT(evidence.Context.ContextStatus == STATUS_RETRY);
    EXPECT((evidence.Flags & KSWORD_CONTEXT_RACE_FLAG) != 0);
    CheckRegisters(&evidence, 0, expected);
    InterlockedExchange(&gContextSequence, 0);
}

static VOID TestOperationStack(VOID)
{
    KSWORD_BUGCHECK_EVIDENCE evidence;
    ULONG index;
    ULONG calls;
    printf("CASE stack: 16 real-cache frames, thread/time, zero/SEH failure, HIGH_LEVEL\n");
    KswordARKBugcheckContextReset();
    gReplayIrql = PASSIVE_LEVEL; gReplayFrames = 16; gReplayRaise = FALSE;
    gReplayTime = 456789ULL;
    KswordARKBugcheckContextOperation();
    evidence = ContextSnapshot(gReplayThread);
    EXPECT(gReplayCaptureSkip == 1);
    EXPECT(gReplayCaptureLimit == KSWORD_BUGCHECK_EVIDENCE_STACK);
    EXPECT(evidence.Context.StackStatus == STATUS_SUCCESS);
    EXPECT(evidence.Context.StackCount == 16);
    EXPECT(evidence.Context.StackSource == 4);
    EXPECT(evidence.Context.StackThreadId == gReplayThread);
    EXPECT(evidence.Context.StackSampleTime == gReplayTime);
    for (index = 0; index < 16; ++index)
        EXPECT(evidence.Context.Stack[index] == 0xFFFF800000001000ULL + (ULONG64)index * 0x100ULL);
    evidence = ContextSnapshot(gReplayThread + 1);
    EXPECT(evidence.Context.StackSource == 3);
    calls = gReplayCaptureCalls;
    gReplayIrql = HIGH_LEVEL; gReplayTime += 99;
    KswordARKBugcheckContextOperation();
    evidence = ContextSnapshot(gReplayThread);
    EXPECT(gReplayCaptureCalls == calls);
    EXPECT(evidence.Context.StackCount == 16);
    EXPECT(evidence.Context.StackSampleTime == 456789ULL);
    KswordARKBugcheckContextReset();
    KswordARKBugcheckContextOperation();
    evidence = ContextSnapshot(gReplayThread);
    EXPECT(gReplayCaptureCalls == calls);
    EXPECT(evidence.Context.StackCount == 0);
    EXPECT(evidence.Context.StackStatus == STATUS_NOT_FOUND);
    gReplayIrql = PASSIVE_LEVEL;
    gReplayFrames = 0;
    KswordARKBugcheckContextOperation();
    evidence = ContextSnapshot(gReplayThread);
    EXPECT(evidence.Context.StackStatus == STATUS_NOT_FOUND);
    EXPECT(evidence.Context.StackSource == 0);
    EXPECT(evidence.Context.StackCount == 0);
    EXPECT(evidence.Context.StackThreadId == gReplayThread);
    EXPECT(evidence.Context.StackSampleTime == gReplayTime);
    gReplayRaise = TRUE;
    KswordARKBugcheckContextOperation();
    evidence = ContextSnapshot(gReplayThread);
    EXPECT(evidence.Context.StackStatus == STATUS_ACCESS_VIOLATION);
    EXPECT(evidence.Context.StackSource == 0);
    EXPECT(evidence.Context.StackCount == 0);
    gReplayRaise = FALSE; gReplayFrames = 16;
    calls = gReplayCaptureCalls;
    InterlockedExchange(&gStackWriter, 1);
    KswordARKBugcheckContextOperation();
    InterlockedExchange(&gStackWriter, 0);
    EXPECT(gReplayCaptureCalls == calls);
    InterlockedExchange(&gStackSequence, 1);
    evidence = ContextSnapshot(gReplayThread);
    EXPECT(evidence.Context.StackStatus == STATUS_RETRY);
    EXPECT(evidence.Context.StackCount == 0);
    EXPECT((evidence.Flags & KSWORD_CONTEXT_RACE_FLAG) != 0);
    InterlockedExchange(&gStackSequence, 0);
}

static KSWORD_BUGCHECK_EVIDENCE TraceSnapshot(VOID)
{
    KSWORD_BUGCHECK_EVIDENCE evidence;
    RtlZeroMemory(&evidence, sizeof(evidence));
    evidence.Flags = KSWORD_BUGCHECK_EVIDENCE_VALID;
    KswordARKBugcheckTraceSnapshot(&evidence);
    return evidence;
}

static VOID TestTrace(VOID)
{
    KSWORD_BUGCHECK_EVIDENCE evidence;
    ULONG index;
    ULONG64 latest;
    KSWORD_BUGCHECK_TRACE_SLOT* slot;
    CHAR raw[80];
    printf("CASE trace: six-slot overwrite, bounded text, status, disabled, try-lock/torn slot\n");
    evidence = TraceSnapshot();
    EXPECT(evidence.EventCount == 0);
    EXPECT(evidence.EventsDropped == 0);
    EXPECT((evidence.Flags & KSWORD_BUGCHECK_EVIDENCE_TRACE_PRESENT) != 0);
    EXPECT((evidence.Flags & KSWORD_BUGCHECK_EVIDENCE_DBG_CAPTURE_DISABLED) != 0);
    gReplayIrql = HIGH_LEVEL;
    for (index = 0; index < 8; ++index) {
        gReplayTime = 1000ULL + index;
        KswordARKBugcheckTraceRecord(KSWORD_BUGCHECK_TRACE_KIND_IOCTL_END, 0x100UL + index, STATUS_ACCESS_DENIED, "END", 3);
    }
    evidence = TraceSnapshot();
    EXPECT(evidence.EventCount == 6);
    EXPECT(evidence.EventsOverwritten == 2);
    EXPECT(evidence.EventsDiscarded == 0);
    EXPECT(evidence.EventsDropped == 2);
    EXPECT((evidence.Flags & KSWORD_BUGCHECK_EVIDENCE_TRACE_OVERWRITTEN) != 0);
    EXPECT((evidence.Flags & KSWORD_BUGCHECK_EVIDENCE_VALID) != 0);
    for (index = 0; index < 6; ++index) {
        EXPECT(evidence.Events[index].Sequence == 3ULL + index);
        EXPECT(evidence.Events[index].Code == 0x102UL + index);
        EXPECT(evidence.Events[index].Status == STATUS_ACCESS_DENIED);
        EXPECT(evidence.Events[index].Time == 1002ULL + index);
        EXPECT(evidence.Events[index].ProcessId == gReplayProcess);
        EXPECT(evidence.Events[index].ThreadId == gReplayThread);
        EXPECT(evidence.Events[index].Flags == KSWORD_BUGCHECK_TRACE_EVENT_CONTEXT_VALID);
        EXPECT(strcmp(evidence.Events[index].Text, "END") == 0);
    }
    memset(raw, 'X', sizeof(raw));
    KswordARKBugcheckTraceRecord(KSWORD_BUGCHECK_TRACE_KIND_IOCTL_BEGIN, 9, STATUS_PENDING, raw, sizeof(raw));
    evidence = TraceSnapshot();
    EXPECT(evidence.Events[5].Text[63] == '\0');
    EXPECT(strlen(evidence.Events[5].Text) == 63);
    EXPECT((evidence.Events[5].Flags & KSWORD_BUGCHECK_TRACE_EVENT_TEXT_TRUNCATED) != 0);
    EXPECT((evidence.Events[5].Flags & KSWORD_BUGCHECK_TRACE_EVENT_STATUS_UNAVAILABLE) != 0);
    EXPECT(evidence.Events[5].Status == STATUS_PENDING);
    memcpy(raw, "ABC", 3);
    KswordARKBugcheckTraceRecord(KSWORD_BUGCHECK_TRACE_KIND_DRIVER_LOG, 2, STATUS_NOT_SUPPORTED, raw, 3);
    evidence = TraceSnapshot();
    EXPECT(strcmp(evidence.Events[5].Text, "ABC") == 0);
    EXPECT((evidence.Events[5].Flags & KSWORD_BUGCHECK_TRACE_EVENT_TEXT_TRUNCATED) == 0);
    EXPECT((evidence.Events[5].Flags & KSWORD_BUGCHECK_TRACE_EVENT_STATUS_UNAVAILABLE) != 0);
    KswordARKBugcheckTraceRecord(KSWORD_BUGCHECK_TRACE_KIND_DBGPRINT, 0x33, (NTSTATUS)4, NULL, 0);
    evidence = TraceSnapshot();
    EXPECT((evidence.Flags & KSWORD_BUGCHECK_EVIDENCE_DBG_CAPTURE_ACTIVE) != 0);
    EXPECT((evidence.Flags & KSWORD_BUGCHECK_EVIDENCE_DBG_CAPTURE_DISABLED) == 0);
    EXPECT(evidence.Events[5].Text[0] == '\0');
    EXPECT(evidence.Events[5].Status == (NTSTATUS)4);
    EXPECT((evidence.Events[5].Flags & KSWORD_BUGCHECK_TRACE_EVENT_STATUS_UNAVAILABLE) == 0);
    KswordARKBugcheckTraceRecord(KSWORD_BUGCHECK_TRACE_KIND_DBGPRINT_STATE, 0, STATUS_SUCCESS, NULL, 0);
    evidence = TraceSnapshot();
    EXPECT((evidence.Flags & KSWORD_BUGCHECK_EVIDENCE_DBG_CAPTURE_DISABLED) != 0);
    EXPECT((evidence.Flags & KSWORD_BUGCHECK_EVIDENCE_DBG_CAPTURE_ACTIVE) == 0);
    latest = evidence.Events[5].Sequence;
    InterlockedExchange(&g_KswordTraceWriter, 1);
    KswordARKBugcheckTraceRecord(KSWORD_BUGCHECK_TRACE_KIND_IOCTL_END, 0xBAD, STATUS_SUCCESS, NULL, 0);
    evidence = TraceSnapshot();
    EXPECT(evidence.EventCount == 6);
    EXPECT(evidence.EventsDiscarded == 1);
    EXPECT(evidence.EventsDropped == evidence.EventsOverwritten + 1ULL);
    EXPECT(evidence.Events[5].Sequence == latest);
    EXPECT((evidence.Flags & KSWORD_BUGCHECK_EVIDENCE_TRACE_DROPPED) != 0);
    EXPECT((evidence.Flags & KSWORD_BUGCHECK_EVIDENCE_TRACE_RACE) != 0);
    InterlockedExchange(&g_KswordTraceWriter, 0);
    slot = &g_KswordTraceSlots[(ULONG)((latest - 1ULL) % KSWORD_BUGCHECK_EVIDENCE_EVENTS)];
    InterlockedExchange64(&slot->CommitSequence, -((LONG64)latest));
    evidence = TraceSnapshot();
    EXPECT(evidence.EventCount == 5);
    EXPECT((evidence.Flags & KSWORD_BUGCHECK_EVIDENCE_TRACE_RACE) != 0);
    EXPECT(evidence.Events[5].Sequence == 0);
    EXPECT(evidence.Events[4].Sequence == latest - 1ULL);
    InterlockedExchange64(&slot->CommitSequence, (LONG64)latest);
    evidence = TraceSnapshot();
    EXPECT(evidence.EventCount == 6);
    EXPECT((evidence.Flags & KSWORD_BUGCHECK_EVIDENCE_TRACE_RACE) == 0);
    EXPECT((evidence.Flags & KSWORD_BUGCHECK_EVIDENCE_DBG_CAPTURE_DISABLED) != 0);
    gReplayIrql = PASSIVE_LEVEL;
}

int main(VOID)
{
    TestDumpContext();
    TestOperationStack();
    TestTrace();
    printf("BUGCHECK_CONTEXT_REPLAY checks=%lu failures=%lu\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
