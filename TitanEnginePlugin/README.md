# KSword TitanEngine proxy (AMD64)

`KSword/TitanEngine.dll` implements the 64 canonical engine exports in the
pinned x64dbg PR 3974 ABI. Its forwarding dependency is the original, matching
`TitanEngine.dll` in the parent directory. Module identity, AMD64 format and all
64 exports are checked before dispatch; a KSword proxy cannot be its own native
dependency. The host checked loader is retained where x64dbg provides it.
Both DLLs stay pinned while callbacks or the log control worker can execute.
`DllMain` only records the module handle.

The default is native forwarding. Starting x64dbg, querying its session and
using ordinary debugging requires no KSword driver. Memory Safe/Unsafe retain
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

If the normal EPT execution protocol is unavailable, the shared backend uses
pinned ShadowPage INT3 with EPTP switching; MTF is not required for this fallback.
Titan's own software-breakpoint loop owns the real Windows #BP stop, temporary
restore, TF step and rearm. Internal breakpoint reads see logical INT3 bytes;
public Safe/Unsafe reads retain native semantics and expose the original bytes.
Hardware slot replacement and multiple breakpoints on one page remain transactional.
The fallback does not extend generic hardware data breakpoint support.

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
event, and existing hardware breakpoints must be removed before changing mode.
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
`<session> <revision> <requestedHvm>` and acknowledges
`<session> <revision> <error> <actualHvm> <driverReady> <residentActive> <eptProtocol>`.
Session IDs and monotonic revisions reject stale requests. Native off-mode
acknowledgments do not open a driver. HVM errors remain visible; selection is
never silently accepted on a partial failure. Control/state readers allow
read/write/delete sharing and close before dispatch or waiting so the next
atomic file replacement cannot be blocked by a polling reader.

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
acknowledgments and rejection of an incomplete native export table. It does
not activate HVM or prove live EPT hits. The VM's full native regressions, real
ShadowPage fallback stops and actual control/log Tabs are recorded in
`docs/ksword-debugger-vm-validation.md`.

The proxy intentionally exposes only the selected canonical engine ABI plus
the three KSword exports. Third-party plugins importing historical Titan APIs
outside those 64 names are not automatically compatible. x86, WOW64 HVM,
generic EPT data breakpoints and reverse execution are not claimed.
