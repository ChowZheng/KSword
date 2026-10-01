# KSword TitanEngine proxy (AMD64)

`KSword/TitanEngine.dll` implements the 64 canonical engine exports in the
pinned x64dbg PR 3974 ABI. Its forwarding dependency is the original, matching
`TitanEngine.dll` in the parent directory. Module identity, AMD64 format and all
64 exports are checked before dispatch; a KSword proxy cannot be its own native
dependency. The host checked loader is retained where x64dbg provides it.
Both DLLs stay pinned while callbacks or the log control worker can execute.
`DllMain` only records the module handle.

The default is native forwarding. Starting x64dbg, querying its session and
using ordinary debugging requires no KSword driver. Default memory Safe/Unsafe retain
native filtering/raw semantics; allocation, protection, pause, stepping,
software and memory breakpoints retain native behavior. Replay exports retain
the native unsupported result; no HVM or reverse-execution capability bits are
invented.

## HVM execution breakpoints

The opt-in HVM path routes **hardware execution** breakpoints through the shared
debugger backend. Hardware data breakpoints are rejected in HVM mode because
the current EPT data primitive covers a 4 KiB page rather than Titan's hardware
byte range. Native mode supports all normal hardware types. The raw Watch API
remains available for explicit page coverage and first-touch evidence.

Only the native DLL's six import slots are adapted: `GetThreadContext`,
`SetThreadContext`, `WaitForDebugEvent`, `ContinueDebugEvent`,
`ReadProcessMemory`, and `WriteProcessMemory`. These are
process-local imports, not global or remote detours. Native Titan retains its
callback table and one debug-event thread. Guest #DB injection from HVM_DEBUG
becomes a real Windows debug event; there is no polling callback or fabricated
Windows event.

In normal mode with fallback allowed, if the EPT execution protocol is unavailable, the shared backend uses
pinned ShadowPage INT3 with EPTP switching; MTF is not required for this fallback.
Titan's own software-breakpoint loop owns the real Windows #BP stop, temporary
restore, TF step and rearm. Internal breakpoint reads see logical INT3 bytes;
public Safe/Unsafe reads retain native semantics and expose the original bytes.
Hardware slot replacement and multiple breakpoints on one page remain transactional.
The fallback does not extend generic hardware data breakpoint support. Disabling
fallback rejects this transition and publishes the actual error and path.

## Policy options

`KSWORD_DEBUGGER_OPTIONS` is a versioned 48-byte common-backend structure, not
an adapter-only preference. `GET_OPTIONS` (4), `SET_OPTIONS` (5), and
`QUERY_POLICY` (6) work without opening a driver. A failed `SET_OPTIONS` returns
the confirmed options in its output. The 72-byte policy status reports actual
native/EPT/Shadow path, owned bindings/pages, change eligibility and fallback
count/last error. `RESTORE_SHADOW_WRITES` (7) removes owned memory patches;
`address=bytes=0` restores all, while retaining breakpoint masks.

| Option | Normal default | Meaning |
| --- | --- | --- |
| mode | normal | Normal retains the existing EPT-first path; stealth selects ShadowPage execution |
| shadowMemoryWrites | off | Hidden execution-page memory patches; original code bytes remain readable |
| allowFallback | on | Permit explicitly logged supported fallback, including ordinary data writes |
| logFallback | on, required | Critical fallback logging cannot be disabled |
| nativeContextFallback | on | Permit logged native context fallback when the backend supports it |
| nativeSuspendFallback | on | Permit logged native suspend fallback when the backend supports it |
| maxShadowPages | 32 | Bound owned ShadowPage memory patches to 1–32 pages |

Selecting stealth in the Tab starts with Shadow writes on and fallback off.
Allowing fallback explicitly permits ordinary **data** writes; it never permits
visible original-code INT3 or hardware DR fallback for stealth breakpoints.
Executable patches require HVM, and mixed executable/data write ranges are
rejected. The policy-aware `MemoryWriteSafe` route classifies original protection
before native Titan temporarily promotes a page to RWX. Native PAGE_GUARD memory
breakpoints are rejected in stealth; normal HVM logs this native fallback and
requires `allowFallback=1`. Stealth does not hide the Windows debug-event channel
or debug port and does not guarantee an undetectable debugger. Failed hidden
installation logs source, error, and rejected actual path.

Windows reads supply the complete context of that held event. The shared
backend overlays DR0–DR3/DR7, preserving DR6; writes clone the full Windows
context with `InitializeContext`/`CopyContext`, including XSTATE, and replace
only debug fields for EPT programming. Rollback writes only the debug slice.
The native TF/delete/rearm sequence owns the original instruction's transition
and native single-step callback. A short pending record handles Titan's
DR7-before-address programming; continuation is refused until it is complete.
Step completion releases the policy lock before calling the frontend's pause
callback, which may wait for Run while another thread reads stopped context.

Mode selection, hardware installation and context adaptation share one policy
lock. Selection changes require an idle debugger or an actual held native
event, and existing software, memory, hardware and backend Shadow bindings must
be removed before changing mode. Identical requests remain idempotent. User
software and memory one-shot callbacks retire the matching adapter binding before calling
the frontend. These callbacks release adapter locks before entering x64dbg.
The control worker uses a copied held event rather than reading native global
event data while the debug loop runs. Session generation and retained process
and thread handles reject stale identities. Exit cleanup happens after the
real EXIT_PROCESS event is continued. Stop, detach and both launch/attach debug
loop exits retire this backend's leases. Successful native memory frees retire
overlapping owned EPT hardware bindings, including the rounded pages of an
unaligned decommit; failed frees preserve them. Module unload retires only
private HVM bindings with the matching PID and captured allocation base. Logical
breakpoints on unmapped/replaced pages must be reinstalled.

## Extra exports and log Tab

- `KSwordTitanInitialize()` validates/initializes the native adapter outside the
  loader lock and returns a Win32 error.
- `KSwordTitanControl(DWORD enabled, KSWORD_DEBUGGER_BACKEND_STATUS*)` changes
  the HVM selection and returns the actual state plus a Win32 error.
- `KSwordDebuggerCall(KSWORD_DEBUGGER_CALL*)` exposes the common versioned ABI,
  including all existing HVM protocol commands and their ownership guards.

The standalone launcher sets `KSWORD_DEBUGGER_LOG_FILE`,
`KSWORD_DEBUGGER_CONTROL_FILE`, `KSWORD_DEBUGGER_STATE_FILE` and
`KSWORD_DEBUGGER_SESSION_ID`. The idle worker consumes
the legacy `<session> <revision> <requestedHvm>` (preserving existing options),
or appends `v2 <mode> <shadowWrites> <allowFallback> <contextFallback>
<suspendFallback> <maxPages>`. It acknowledges the legacy seven-field prefix
`<session> <revision> <error> <actualHvm> <driverReady> <residentActive> <eptProtocol>`
followed by `v2`, the six confirmed options, then `<actualPath>
<activeBreakpoints> <shadowWritePages> <canChange> <fallbackCount> <lastFallbackError>`.
Session IDs and monotonic revisions reject stale requests. Native off-mode
acknowledgments do not open a driver. HVM errors remain visible; selection is
never silently accepted on a partial failure. Control/state readers allow
read/write/delete sharing and close before dispatch or waiting so the next
atomic file replacement cannot be blocked by a polling reader. A 500 ms actual
state heartbeat updates eligibility and path after native pause/binding events.

## Build and checks

Build `KswordTitanEngine.vcxproj` with 64-bit MSBuild and all three required
architecture properties. The output is `x64/Release/TitanEngine.dll`.
`generate_forwarders.py` regenerates typed wrappers directly from the canonical
header. New source files appear in the project and filters.

```powershell
python third_party/x64dbg_abi/check_titanengine_exports.py TitanEnginePlugin/x64/Release/TitanEngine.dll --definition third_party/x64dbg_abi/TitanEngine.def --canonical-header third_party/x64dbg_abi/TitanEngine.h --adapter-header third_party/x64dbg_abi/TitanEngine.h
python TitanEnginePlugin/tests/validate_proxy.py --proxy TitanEnginePlugin/x64/Release/TitanEngine.dll --native .deps/titanengine-reference/build-x64/Release/TitanEngine.dll --fixture-root .codex-build-logs/titan-proxy-fixture
```

The headless test covers actual native ABI/session/context-size, handles,
memory, protection, success/failure LastError, unsupported replay, idle control
acknowledgments, v1/v2 options, validation, failure rollback, real native software
binding protection, stealth visible-breakpoint rejection, and rejection of an
incomplete native export table. `tests/LifetimeTests.vcxproj` also builds the pure
headless `ControlProtocolTests.cpp` cases, requiring neither GUI nor driver. These checks do
not activate HVM or prove live EPT hits. The VM's full native regressions, real
ShadowPage fallback stops and actual control/log Tabs are recorded in
`docs/ksword-debugger-vm-validation.md`.

The proxy intentionally exposes only the selected canonical engine ABI plus
the three KSword exports. Third-party plugins importing historical Titan APIs
outside those 64 names are not automatically compatible. x86, WOW64 HVM,
generic EPT data breakpoints and reverse execution are not claimed.
