#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
#define _KERNEL_MODE 1
#include "KswordArkCallbackIoctl.h"
#ifndef _In_
#define _In_
#endif
#ifndef _Inout_
#define _Inout_
#endif
#ifndef _In_z_
#define _In_z_
#endif
#ifndef _Out_
#define _Out_
#endif
#define NTAPI
#define VOID void
#define TRUE 1
#define FALSE 0
#define PASSIVE_LEVEL 0
#define NT_SUCCESS(s) ((s) >= 0)
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000DL)
#define STATUS_NOT_FOUND ((NTSTATUS)0xC0000225L)
#define STATUS_NOT_SUPPORTED ((NTSTATUS)0xC00000BBL)
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xC0000001L)
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xC0000022L)
#define STATUS_PROCEDURE_NOT_FOUND ((NTSTATUS)0xC000007AL)
#define STATUS_RETRY ((NTSTATUS)0xC000022DL)
#define IO_TYPE_DRIVER 4
#define KSWORD_ARK_CALLBACK_REGISTERED_REGISTRY 1UL
typedef long NTSTATUS;
typedef unsigned long ULONG;
typedef unsigned long long ULONG64;
typedef long long LONGLONG;
typedef long LONG;
typedef size_t SIZE_T;
typedef struct { LONGLONG QuadPart; } LARGE_INTEGER;
typedef struct { short Type; unsigned short Size; } DRIVER_OBJECT;
typedef struct _LIST_ENTRY { struct _LIST_ENTRY *Flink, *Blink; } LIST_ENTRY;
typedef struct { LARGE_INTEGER RegistryCookie; ULONG RegisteredCallbacksMask; } KSWORD_ARK_CALLBACK_RUNTIME;
typedef uintptr_t ULONG_PTR;
typedef int BOOLEAN;
typedef void *PVOID;
typedef const wchar_t *PCWSTR;
typedef PVOID PKBUGCHECK_CALLBACK_RECORD;
typedef PVOID PKBUGCHECK_REASON_CALLBACK_RECORD;
typedef PVOID PDRIVER_OBJECT;
typedef PVOID PDEVICE_OBJECT;
typedef PVOID PDRIVER_FS_NOTIFICATION;
typedef PVOID PSE_LOGON_SESSION_TERMINATED_ROUTINE;
typedef PVOID PSE_LOGON_SESSION_TERMINATED_ROUTINE_EX;
typedef PVOID PDEBUG_PRINT_CALLBACK;
#define RtlZeroMemory(p,n) memset(p,0,n)
#include <string.h>
static KSWORD_ARK_CALLBACK_RUNTIME fixture_runtime;
static KSWORD_ARK_CALLBACK_RUNTIME* KswordArkCallbackGetRuntime(void) { return &fixture_runtime; }
static LONG InterlockedAnd(volatile LONG *p, LONG bits) { LONG old=*p; *p &= bits; return old; }
NTSTATUS KswordArkRegistryCallback(PVOID a, PVOID b, PVOID c) { (void)a; (void)b; (void)c; return 0; }
static uintptr_t memory_begin, memory_end, driver_begin;
static int KswordARKRuntimeReadMemory(const void *from, void *to, size_t size) {
    uintptr_t value=(uintptr_t)from;
    if ((value >= memory_begin && value <= memory_end && size <= memory_end-value) ||
        (value==driver_begin && size==sizeof(DRIVER_OBJECT))) { memcpy(to,from,size); return 1; }
    return 0;
}
#define KswordArkCallbackEnumReadMemory KswordARKRuntimeReadMemory
void KswordArkCallbackRegistryRemoved(ULONG64 cookie);

static int calls, enum_calls, irql, present_before, present_after;
static ULONG fixture_fields, fixture_subtype;
static ULONG64 current_registration, current_context;
static PVOID seen_registration, seen_context, seen_callback;
static NTSTATUS api_result, enum_before_result, enum_after_result;
static int KeGetCurrentIrql(void) { return irql; }
static NTSTATUS RtlStringCbPrintfW(wchar_t *out, size_t size, PCWSTR format, ...) {
    (void)format;
    if (size) out[0] = L'D';
    return 0;
}
static NTSTATUS KswordArkCallbackEnumRevalidateRemoveRequest(
    const KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_EX_REQUEST *request,
    BOOLEAN require_generation, BOOLEAN match_identity, BOOLEAN *present,
    ULONG *out_fields, ULONG64 *registration, ULONG64 *generation,
    ULONG64 *context, ULONG *out_subtype) {
    (void)request; (void)generation;
    assert(!require_generation);
    assert(match_identity == (enum_calls == 0));
    *present = enum_calls == 0 ? present_before : present_after;
    if (out_fields) *out_fields = fixture_fields;
    if (registration) *registration = current_registration;
    if (context) *context = current_context;
    if (out_subtype) *out_subtype = fixture_subtype;
    return enum_calls++ == 0 ? enum_before_result : enum_after_result;
}
static NTSTATUS record_call(PVOID registration) {
    ++calls; seen_registration = registration; return api_result;
}
static BOOLEAN KeDeregisterBugCheckCallback(PVOID p) { return NT_SUCCESS(record_call(p)); }
static BOOLEAN KeDeregisterBugCheckReasonCallback(PVOID p) { return NT_SUCCESS(record_call(p)); }
static NTSTATUS KeDeregisterNmiCallback(PVOID p) { return record_call(p); }
static VOID ExUnregisterCallback(PVOID p) { (void)record_call(p); }
static VOID SeUnregisterImageVerificationCallback(PVOID p) { (void)record_call(p); }
static VOID IoUnregisterShutdownNotification(PVOID p) { (void)record_call(p); }
static VOID IoUnregisterFsRegistrationChange(PVOID object, PVOID callback) {
    seen_context = object; seen_callback = callback; (void)record_call(NULL);
}
static NTSTATUS SeUnregisterLogonSessionTerminatedRoutine(PVOID p) {
    seen_callback = p; return record_call(NULL);
}
static NTSTATUS SeUnregisterLogonSessionTerminatedRoutineEx(PVOID p, PVOID context) {
    seen_callback=p; seen_context=context; return record_call(NULL);
}
static NTSTATUS CmUnRegisterCallback(LARGE_INTEGER cookie) { return record_call((PVOID)(ULONG_PTR)cookie.QuadPart); }
static NTSTATUS DbgSetDebugPrintCallback(PVOID callback, BOOLEAN enable) { assert(!enable); seen_callback=callback; return record_call(NULL); }
static VOID coalescing_unregister(PVOID p) { (void)record_call(p); }
static VOID priority_unregister(PDRIVER_OBJECT object) { seen_context=object; (void)record_call(NULL); }
static int export_available=1;
static PVOID KswordArkCallbackExtendedGetSystemRoutine(PCWSTR name) {
    if (!export_available) return NULL;
    return wcscmp(name,L"PoUnregisterCoalescingCallback")==0 ? (PVOID)coalescing_unregister : (PVOID)priority_unregister;
}
static NTSTATUS PoUnregisterPowerSettingCallback(PVOID p) { return record_call(p); }
static NTSTATUS IoUnregisterPlugPlayNotificationEx(PVOID p) { return record_call(p); }
#include "production.c"

static KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_EX_REQUEST request;
static KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_EX_RESPONSE response;
static void reset(ULONG callback_class) {
    request = (KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_EX_REQUEST){0};
    response = (KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_EX_RESPONSE){0};
    request.callbackClass = callback_class;
    request.callbackAddress = 0xFFFF800012340000ULL;
    request.registrationAddress = 0xFFFF800045670000ULL;
    request.identityHash = 1;
    request.flags = KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_FLAG_REQUIRE_REVALIDATION;
    calls = enum_calls = irql = present_after = 0; present_before = 1;
    fixture_fields = KSWORD_ARK_CALLBACK_ENUM_FIELD_REMOVABLE_CANDIDATE;
    fixture_subtype = KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_LOGON_LEGACY;
    current_registration = request.registrationAddress;
    current_context = 0xFFFF800078900000ULL;
    api_result = enum_before_result = enum_after_result = STATUS_SUCCESS;
    seen_registration = seen_context = seen_callback = NULL;
}
int main(void) {
    const ULONG classes[] = {5, 10, 11, 12, 13, 14, 9, 15, 16, 18, 19, 20, 21, 23};
    for (size_t i = 0; i < sizeof(classes)/sizeof(classes[0]); ++i) {
        reset(classes[i]);
        if (classes[i] == 5 || classes[i] == 18 || classes[i] == 19 || classes[i] == 23) fixture_fields |= KSWORD_ARK_CALLBACK_ENUM_FIELD_HANDLE;
        assert(KswordArkCallbackRemoveExtendedPublic(&request, &response) == STATUS_SUCCESS);
        assert(calls == 1 && enum_calls == 2);
        if (classes[i] == 13) {
            assert((ULONG64)(uintptr_t)seen_context == current_context);
            assert((ULONG64)(uintptr_t)seen_callback == request.callbackAddress);
        } else if (classes[i] == 20) {
            assert((ULONG64)(uintptr_t)seen_context == current_context);
        } else if (classes[i] != 14 && classes[i] != 21) {
            assert((ULONG64)(uintptr_t)seen_registration == current_registration);
        }
    }
    reset(16); api_result = STATUS_ACCESS_DENIED;
    assert(KswordArkCallbackRemoveExtendedPublic(&request, &response) == STATUS_ACCESS_DENIED);
    assert(calls == 1 && enum_calls == 1);
    reset(10); api_result = STATUS_ACCESS_DENIED;
    assert(KswordArkCallbackRemoveExtendedPublic(&request, &response) == STATUS_NOT_FOUND);
    reset(12); present_after = 1;
    assert(KswordArkCallbackRemoveExtendedPublic(&request, &response) == STATUS_UNSUCCESSFUL);
    reset(9); enum_after_result = STATUS_ACCESS_DENIED;
    assert(KswordArkCallbackRemoveExtendedPublic(&request, &response) == STATUS_ACCESS_DENIED);
    assert(calls == 1);
    reset(16); present_before = 0;
    assert(KswordArkCallbackRemoveExtendedPublic(&request, &response) == STATUS_NOT_FOUND && !calls);
    reset(16); enum_before_result = STATUS_ACCESS_DENIED;
    assert(KswordArkCallbackRemoveExtendedPublic(&request, &response) == STATUS_ACCESS_DENIED && !calls);
    reset(16); request.identityHash = 0;
    assert(KswordArkCallbackRemoveExtendedPublic(&request, &response) == STATUS_INVALID_PARAMETER && !calls);
    reset(16); irql = 2;
    assert(KswordArkCallbackRemoveExtendedPublic(&request, &response) == STATUS_INVALID_PARAMETER && !calls);
    reset(18);
    assert(KswordArkCallbackRemoveExtendedPublic(&request, &response) == STATUS_NOT_SUPPORTED && !calls);
    reset(23); fixture_fields |= KSWORD_ARK_CALLBACK_ENUM_FIELD_HANDLE; fixture_fields &= ~KSWORD_ARK_CALLBACK_ENUM_FIELD_REMOVABLE_CANDIDATE;
    assert(KswordArkCallbackRemoveExtendedPublic(&request, &response) == STATUS_NOT_SUPPORTED && !calls);
    reset(14); fixture_subtype = KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_LOGON_EX;
    assert(KswordArkCallbackRemoveExtendedPublic(&request, &response) == STATUS_SUCCESS && calls==1);
    assert((ULONG64)(uintptr_t)seen_context==current_context && (ULONG64)(uintptr_t)seen_callback==request.callbackAddress);
    reset(14); fixture_subtype = KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_LOGON_EX; current_context=0;
    assert(KswordArkCallbackRemoveExtendedPublic(&request, &response) == STATUS_SUCCESS);
    reset(19); fixture_fields |= KSWORD_ARK_CALLBACK_ENUM_FIELD_HANDLE; export_available=0;
    assert(KswordArkCallbackRemoveExtendedPublic(&request, &response) == STATUS_PROCEDURE_NOT_FOUND && !calls);
    export_available=1;
    reset(5); fixture_fields |= KSWORD_ARK_CALLBACK_ENUM_FIELD_HANDLE; current_registration=37; // Cookie is an unaligned token, not a kernel address.
    assert(KswordArkCallbackRemoveExtendedPublic(&request, &response) == STATUS_SUCCESS && (uintptr_t)seen_registration==37);

    reset(10);
    KSWORD_ARK_CALLBACK_ENUM_ENTRY row = {0};
    row.status = KSWORD_ARK_CALLBACK_ENUM_STATUS_OK;
    row.callbackClass = 10; row.callbackAddress = request.callbackAddress;
    row.registrationAddress = request.registrationAddress; row.identityHash = 2;
    assert(KswordArkCallbackEnumRemoveRequestMatchesEntry(&request, &row, TRUE));
    row.trustFlags=KSWORD_ARK_CALLBACK_TRUST_OWNER_MODULE_RESOLVED;
    assert(KswordArkCallbackEnumRemoveRequestMatchesEntry(&request, &row, TRUE));
    row.rawStorageValue=99; // A changed storage node must not match merely because display metadata changed.
    assert(!KswordArkCallbackEnumRemoveRequestMatchesEntry(&request, &row, TRUE));
    row.rawStorageValue=0;
    assert(KswordArkCallbackEnumRemoveRequestMatchesEntry(&request, &row, FALSE));
    row.status = KSWORD_ARK_CALLBACK_ENUM_STATUS_NOT_REGISTERED;
    assert(!KswordArkCallbackEnumRemoveRequestMatchesEntry(&request, &row, FALSE));
    row.status = KSWORD_ARK_CALLBACK_ENUM_STATUS_OK; row.registrationAddress += 8;
    assert(!KswordArkCallbackEnumRemoveRequestMatchesEntry(&request, &row, FALSE));

    /* Exercise actual prefix decoding; wrong tags, self handles and objects must remain read-only. */
    ULONG64 storage[14]={0}, callback=0, context=0, registration=0; ULONG fields=0;
    DRIVER_OBJECT driver={IO_TYPE_DRIVER,sizeof(DRIVER_OBJECT)};
    memory_begin=(uintptr_t)storage; memory_end=memory_begin+sizeof(storage); driver_begin=(uintptr_t)&driver;
    storage[2]=0x74655350UL; storage[10]=123; storage[11]=456;
    assert(KswordArkSpecialDecodeIdentity(18,memory_begin,&callback,&context,&registration,&fields) && callback==123 && context==456 && registration==memory_begin);
    storage[2]=0; assert(!KswordArkSpecialDecodeIdentity(18,memory_begin,&callback,&context,&registration,&fields));
    storage[2]=memory_begin; storage[3]=123; storage[5]=456; storage[8]=789;
    assert(KswordArkSpecialDecodeIdentity(19,memory_begin+0x30,&callback,&context,&registration,&fields) && registration==memory_begin && callback==123);
    storage[2]++; assert(!KswordArkSpecialDecodeIdentity(19,memory_begin+0x30,&callback,&context,&registration,&fields));
    storage[2]=memory_begin; storage[4]=driver_begin;
    assert(KswordArkSpecialDecodeIdentity(20,memory_begin,&callback,&context,&registration,&fields) && context==driver_begin);
    driver.Type=0; assert(!KswordArkSpecialDecodeIdentity(20,memory_begin,&callback,&context,&registration,&fields)); driver.Type=IO_TYPE_DRIVER;
    storage[2]=1; storage[4]=123; storage[5]=456; storage[6]=driver_begin; storage[7]=1;
    assert(KswordArkSpecialDecodeIdentity(23,memory_begin,&callback,&context,&registration,&fields));
    storage[7]=1ULL<<16; assert(!KswordArkSpecialDecodeIdentity(23,memory_begin,&callback,&context,&registration,&fields));
    storage[2]=123;
    assert(KswordArkSpecialDecodeIdentity(21,memory_begin+0x18,&callback,&context,&registration,&fields) && callback==123);

    /* Calibrate Registry using a live self triple, and reject stale cookies or altered ABI. */
    ULONG64 registry_memory[8]={0};
    LIST_ENTRY *head=(LIST_ENTRY*)registry_memory, *node=(LIST_ENTRY*)(registry_memory+2);
    memory_begin=(uintptr_t)registry_memory; memory_end=memory_begin+sizeof(registry_memory);
    head->Flink=head->Blink=node; node->Flink=node->Blink=head;
    fixture_runtime.RegistryCookie.QuadPart=37; fixture_runtime.RegisteredCallbacksMask=1;
    registry_memory[5]=37; registry_memory[6]=(ULONG64)(uintptr_t)&fixture_runtime; registry_memory[7]=(ULONG64)(uintptr_t)KswordArkRegistryCallback;
    assert(KswordArkCallbackRegistryLayoutValidated(memory_begin));
    registry_memory[5]++; assert(!KswordArkCallbackRegistryLayoutValidated(memory_begin)); registry_memory[5]--;
    fixture_runtime.RegisteredCallbacksMask=0; assert(!KswordArkCallbackRegistryLayoutValidated(memory_begin)); fixture_runtime.RegisteredCallbacksMask=1;
    KswordArkCallbackRegistryRemoved(37); assert(fixture_runtime.RegistryCookie.QuadPart==0 && fixture_runtime.RegisteredCallbacksMask==0);
    puts("Callback removal argument, failure, confirmation, and identity tests passed.");
    return 0;
}
