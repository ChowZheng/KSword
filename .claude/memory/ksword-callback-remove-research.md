# Callback external-unregistration research (2026-10-01)

Source evidence collected by the research sub-agent. Reference checkouts were fetched into a temporary research directory; they are not project dependencies.

## Registry: sufficiently supported to implement a candidate path

Actual ARK source: OpenArk's original upstream URL currently returns Repository not found. The public fork `onepeanut/open-ark` retains actual driver code:
- Commit: `7b8578517161ce59c310766d830116db53c362de`.
- Layout: https://github.com/onepeanut/open-ark/blob/7b8578517161ce59c310766d830116db53c362de/src/OpenArkDrv/knotify/notify-lib.cpp#L51
- Unregistration: https://github.com/onepeanut/open-ark/blob/7b8578517161ce59c310766d830116db53c362de/src/OpenArkDrv/knotify/notify-lib.cpp#L558
- `RemoveRegistryNotify`: traverses CallbackListHead, uses `CONTAINING_RECORD(entry, CM_CALLBACK_CONTEXT_BLOCKEX, ListEntry)`; Function comparison at line 577; passes `ctx->Cookie` to `CmUnRegisterCallback` at line 578.
- x64 offsets: LIST_ENTRY at 0x00, two ULONG state fields at 0x10/0x14, Cookie (LARGE_INTEGER value) at 0x18, CallerContext at 0x20, Function at 0x28, Altitude at 0x30.
- Upstream shortcomings: matches Function alone, ignores NTSTATUS, returns TRUE immediately. Do not import those success semantics.

Independent active project: WinObjEx64:
- Commit: `f6c36be713b06bf8d4756fd7108acb73b55eb3d9`.
- Structure: https://github.com/hfiref0x/WinObjEx64/blob/f6c36be713b06bf8d4756fd7108acb73b55eb3d9/Source/Shared/ntos/ntos.h#L5631
- Direct node read and Cookie display: https://github.com/hfiref0x/WinObjEx64/blob/f6c36be713b06bf8d4756fd7108acb73b55eb3d9/Source/WinObjEx64/extras/extrasCallbacks.c#L3885
- `DumpCmCallbacks` has no build-dependent structure branch; it reads ListEntry.Flink directly as the structure base.
- `FindCmCallbackHead` at line 2058 uses build branches <= Win11 25H2 versus newer only for different instruction patterns locating the list head. It does not change Cookie offset. This is development evidence across versions, not proof of every supported kernel ABI.

Native binary verification:
- File: C:/Windows/System32/ntoskrnl.exe
- Version: 10.0.19041.7725
- SHA256: b3de5c162d582e0e3cbb27977fd82c2d8212d18fbc3f70bb8bf7e0c4a5925960
- CmUnRegisterCallback RVA 0x869430. Input LARGE_INTEGER is the RCX value (mov rbx,rcx at 0x869443), not a pointer to LARGE_INTEGER.
- At 0x869497 r8d=0; iterator helper at RVA 0x6c50e4 returns current node minus r8d, proving list-node address equals structure base here.
- At 0x8694bc compares [rax+0x18] to input Cookie.
- At 0x869757 callback is [rdi+0x28]; at 0x869765 context is [rdi+0x20].

Recommended calibration within KSword:
- `registry_callback.c:361` calls CmRegisterCallbackEx with Function=KswordArkRegistryCallback, Context=runtime, output=&runtime->RegistryCookie.
- Find a unique live list node whose qwords at 0x18/0x20/0x28 equal the known returned Cookie, runtime pointer, and callback pointer respectively. This establishes the layout with a registration the driver owns.
- Preconditions: own registration succeeded and remains live; runtime Cookie nonzero; successful memory read and unique self match in the same list head used for external enumeration.
- If no self row or ambiguous match, keep external candidate removal unavailable with the concrete calibration reason. An old nonzero Cookie is not itself proof of a live registration.
- Publish the actual external Cookie in registrationAddress, node in rawStorageValue, use EX identity revalidation, preserve API status, re-enumerate after success. Cookie is a token value, so do not require it to be a kernel virtual address or aligned pointer.
- This calibration supports a candidate path, not a blanket verified guarantee. Externally removing a callback may break its owning driver, consistent with the user's requested ARK operation.

Author technical article: https://bbs.kanxue.com/thread-287914.htm (2025-08-06, X66iaM). States verification on Win7 7601 and Win10 19H1; presents the same structure and CmUnRegisterCallback(cookie). Useful corroboration, not modern exhaustive validation.
Official ABI: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-cmunregistercallback ; returns STATUS_INVALID_PARAMETER for unknown Cookie; must not be called from inside RegistryCallback; IRQL <= APC_LEVEL.

## Image Verification: call the Se wrapper

Native binary verifies a genuine CallbackObject registration handle:
- SeRegisterImageVerificationCallback RVA 0x7d64d0.
- At 0x7d64eb loads global CallbackObject; 0x7d64f8 calls ExRegisterCallback RVA 0x37dbd0 with callback/context; 0x7d650e writes its returned RAX directly to caller output.
- SeUnregisterImageVerificationCallback RVA 0x91b7c0: lock dec global registration counter, then call ExUnregisterCallback RVA 0x380530 with original RCX.
- Thus CallbackObject's CALLBACK_REGISTRATION base is the correct handle, but unregistering this subtype directly through ExUnregisterCallback skips the Se registration counter. Use SeUnregisterImageVerificationCallback for this subtype.
- Caller-facing signature is VOID NTAPI SeUnregisterImageVerificationCallback(PVOID Registration), consistent with wrapper behavior on this exact build. Resolve dynamically; do not claim unchecked all-build ABI support.

## EMP: current callback node is not the provider HANDLE

WinObjEx64 structure is EMP_CALLBACK_DB_RECORD at ntos.h:5821; DumpEmpCallbackListHead at extrasCallbacks.c:5351 reads this callback record through its singly linked `List` field. It is not an EmProviderRegister provider-registration block.
- Actual exported unregister symbol is `EmProviderDeregister`, not `EmpProviderDeregister` (the latter does not exist in the inspected native export table).
- On native kernel EmProviderDeregister RVA 0x889880 returns VOID and treats RCX as provider block. It reads arrays/counts at +0x08/+0x10, +0x18/+0x20, +0x28/+0x30, and frees that block. Passing a callback-record base would dereference unrelated layout.
- Native provider block's +0x28 array contains pointers to callback records; count +0x30; callback record CallbackFunc at +0x10 and reference count at +0x18, matching WinObjEx64's callback record shape. But current callback-only traversal has no inverse mapping to provider handle.
- Do not enable external EMP deregistration until enumeration recovers a real provider registration and establishes its relation to this callback record.
- Author's original 2012 article: https://redplait.blogspot.com/2012/09/emproviderregisterempproviderregister.html ; indexed excerpt confirms EmProviderRegister(DriverObject, Entries, EntryCount, Callbacks, CallbackCount, outHandle). Full article fetch was unavailable through browser; treat excerpt as secondary corroboration of native disassembly, not proof of current ABI.

## ETW: recover ETW_REG_ENTRY, not ETW_GUID_ENTRY

Primary original reverse-engineering analysis: https://www.geoffchappell.com/studies/windows/km/ntoskrnl/api/etw/register/register.htm . Explains separate ETW_GUID_ENTRY (provider) and ETW_REG_ENTRY (one registration); from Windows 8 REGHANDLE is the address of ETW_REG_ENTRY. Explicitly limits implementation discussion to original Win10 and warns about later silo changes.
- Native EtwUnregister RVA 0x74f800 dereferences supplied RCX directly at +0x62, +0x28, +0x20, then unlinks list fields at +0/+0x10. Confirms REGHANDLE is a registration-record pointer on 19041.7725.
- Current enumeration has no reliable ETW_REG_ENTRY recovery/provider-reglist identity. Keep unsupported until that chain is established. Never substitute a provider GUID/hash bucket node/function pointer as REGHANDLE.

## PnP and power corroboration

WinObjEx64 DEVICE_CLASS_NOTIFY_ENTRY at ntos.h:5875 holds the modern common header: category+0x10, session id+0x14, session handle+0x18, callback+0x20, context+0x28, DriverObject+0x30, refcount+0x38, Unregistered+0x3a, Lock+0x40, EntryLock+0x48, subtype data+0x50.
- Native IoRegisterPlugPlayNotification RVA 0x70f3d0 allocates target-device subtype base in RDI, links RDI into the device notification list, then at 0x70f513 writes RDI to caller NotificationEntry output. Node base is the actual handle.
- IoUnregisterPlugPlayNotificationEx RVA0x7906d0 passes original RCX to internal unregister at0x37e660 (dl=1); internal reads handle+0x40 mutex and writes handle+0x3a Unregistered. Legacy IoUnregisterPlugPlayNotification invokes same helper with dl=0.
- Some very old private headers have callback at +0x18 and no session fields. Do not use them as modern ABI proof. Scope by supported build and validate exact header/routine; no need to require current module attribution merely to identify the public handle.

WinObjEx64 power setting structure explicitly warns its V2 tail is incomplete/incorrect for latest versions. Use verified leading fields/tag/handle semantics rather than copying the tail:
https://github.com/hfiref0x/WinObjEx64/blob/f6c36be713b06bf8d4756fd7108acb73b55eb3d9/Source/Shared/ntos/ntos.h#L5724

CodeMachine article supplies public/undocumented ABI prototypes for Priority and Coalescing:
https://codemachine.com/articles/kernel_callback_functions.html
- VOID IoUnregisterPriorityCallback(PDRIVER_OBJECT DriverObject)
- VOID PoUnregisterCoalescingCallback(PVOID Handle)
- The handle/driver identity still needs actual enumerated-node proof, as parent agent's native disassembly has already confirmed.
