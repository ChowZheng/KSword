# KSword x64dbg backend and Tab plugin

## Source and package

The plugin ID is `x96dbg`. The runtime uses AMD64 `x64dbg.exe` only. The KSword
page contains controls and forwarded logs; x64dbg has an independent window.
The page uses the KSword theme snapshot and Consolas. It does not change the
debugger's window parent, fonts, colors or layout.

| Component | Baseline |
| --- | --- |
| x64dbg engine ABI and frontend | `f107330b6563da3c38d60a3ad6e629057b7cf5d0` |
| Native TitanEngine | `21ef77f31fd42d17f785802c36b2ca4ca44c43d7` |
| Canonical engine exports | 64, exact typed declarations |
| Qt SDK | Qt 5.12.12 msvc2017_64, archive SHA256 in `X96dbgIntegration/PINNED_BASELINES.json` |

The core patch in `X96dbgIntegration/patches/` adds `DebugEngineKSword = 4` and
`KSword\\TitanEngine.dll` to the existing checked loader. Settings, build
information and the headless runner share that selection. Loader protection
remains enabled. Each launcher session writes its own `[Engine] DebugEngine=4`
in `sessions/UUID/x64dbg.ini`, and invokes x64dbg's existing `-userdir` argument.
Attaching uses its existing `-p PID` option.

```text
x96dbg/
  KswordX96dbgLauncher.exe
  plugin.json
  payload/x64dbg/
    x64dbg.exe
    x64bridge.dll
    x64dbg.dll
    x64gui.dll
    TitanEngine.dll          native engine, canonical pinned ABI
    KSword/TitanEngine.dll   KSword adapter
    platforms/qwindows.dll
  licenses/
  KSword-x96dbg-source.zip
  payload-manifest.json
```

The adapter finds the native DLL by an absolute path one directory above its
own `KSword/` directory. It validates AMD64 and every canonical symbol, rejects
recursive loading, and pins both DLLs for process lifetime. Initialization
occurs on an explicit/canonical API call after DLL loading. `DllMain` only
records the module handle. A missing driver does not prevent native debugging.

## Routing policy

| API surface | Native mode | HVM mode |
| --- | --- | --- |
| Init, attach, Windows debug port, DebugLoop | Native TitanEngine | Native TitanEngine |
| Software breakpoints | Native | Native loop; hidden INT3 when ShadowPage fallback is selected |
| Generic memory breakpoints | Native | Native |
| Hardware execute breakpoints | Native DR | KSword EPT execute; automatic ShadowPage INT3 fallback if unavailable |
| Hardware write/read-write breakpoints | Native DR | Explicit `ERROR_NOT_SUPPORTED` |
| GPR, flags, SIMD/XSTATE context | Native Windows context | Native Windows context with virtual DR overlay |
| StepInto/StepOver and Run | Native callbacks and Windows transport | Same transport; owned EPT state validated before Continue |
| Query/read/write/allocate/protect/free memory | Native | Native; successful free retires affected owned EPT bindings |
| Thread/process handles, suspend/resume, termination | Native real handles | Native real handles; owned state retired on lifecycle transitions |
| Session info and replay APIs | Exact native result/capabilities | Exact native result/capabilities |
| Versioned `KSwordDebuggerCall` | Driver API, explicit readiness/error | Shared HVM control, memory, policy, view, events, nested and debug APIs |

The canonical export table has no optimistic success stubs. Replay capabilities
are the underlying native engine's actual capabilities. HVM event history does
not become a replay timeline, a new live-stop event, or a substitute for a
Windows debug event. Native mode retains its data hardware breakpoints.

EPT data monitoring is page based. Generic byte-range hardware write/read-write
requests cannot be represented as exact stops, so HVM mode rejects them before
changing the native callback table. Explicit Watch/diagnostic operations remain
available through the versioned KSword API, with their actual page semantics.

## Real stop and context transport

The adapter changes six import slots inside its loaded native TitanEngine
module: `GetThreadContext`, `SetThreadContext`, `WaitForDebugEvent` and
`ContinueDebugEvent`, `ReadProcessMemory` and `WriteProcessMemory`. They are local
imports in that module. All six slots must exist at the pinned baseline; initialization
fails on an incompatible native DLL, with rollback of any modified slots.

The implemented EPT execution path uses the existing driver to check user CPL, thread,
TEB, DR7 and RIP, sets guest DR6 and injects guest #DB. Windows delivers the real
`EXCEPTION_SINGLE_STEP` event through the native debug port. Native TitanEngine
holds that event and dispatches its normal hardware-breakpoint callback on its
debug thread. There is no polling thread calling frontend breakpoint handlers.
The VM lacks MTF, so this original #DB lane remains unsupported there. Its
automatic ShadowPage fallback was exercised with real #BP events, exact RIP,
full context editing, one instruction StepInto, repeat hits and cleanup. See
`ksword-debugger-vm-validation.md` for the verified route and its limits.

ShadowPage uses EPTP switching and driver-owned pinned target pages. Original
memory reads retain the original code; native breakpoint bookkeeping sees a
logical INT3 overlay. The native software-breakpoint loop temporarily restores
the hit byte and rearms after a real TF step. The frontend's hardware slot
callbacks are delivered by that same debug-event thread. Multiple offsets on
one page merge, and deleting one offset preserves the others.

The native context writer uses `InitializeContext`/`CopyContext` to retain the
complete requested Windows context, including XSTATE buffers. Only DR0-DR3 and
DR7 are virtualized; real Windows DR6 is preserved for #DB dispatch. Physical
DR0-DR3 addresses remain neutral while logical DR7 enables remain available to
the driver's thread/lifetime guard.
Native TitanEngine programs DR7 before the address; that short sequence is
staged per thread and cannot be continued or switched away while incomplete.
Failure to install an EPT rule causes native callback-table rollback and an
explicit failure. Failed retirement prevents unsafe continuation.

`DebuggerBackend` borrows the native session; it never attaches a second debug
port or calls its own Wait/Continue for that session. It retains process/thread
object identities and a session generation. Stale events, reused IDs and a
conflicting frontend owner are rejected. Process exit is retired after the
held Windows exit event is continued. Detach, Stop, owned thread exit and
module unload and affected memory free retire the adapter's recorded automatic
breakpoint IDs. Decommit ranges cover the actual Windows page rounding, and a
module unload retires bindings with the retained matching allocation base.
Automatic preparation rejects an already prepared/resident external session;
retirement tears down preparation only when this backend owns it. Raw rules
created through the versioned KSword API remain their caller's responsibility
and must not be mixed into an automatic adapter-owned preparation.

## Control and logs

The Tab responds immediately with the `ksword-plugin/1` `tab_ready` event,
then launches the debugger asynchronously. Startup failures remain visible in
the page and are forwarded to PluginHost. Closing the page closes its own
launcher surface; it does not terminate the independent debugger or target.

The launcher passes the following environment variables to the debugger:

| Name | Purpose |
| --- | --- |
| `KSWORD_DEBUGGER_LOG_FILE` | UTF-8 engine/backend log |
| `KSWORD_DEBUGGER_CONTROL_FILE` | Atomic HVM selection request |
| `KSWORD_DEBUGGER_STATE_FILE` | Atomic acknowledgement |
| `KSWORD_DEBUGGER_SESSION_ID` | Session UUID |

Request: `SESSION REVISION HVM_SELECTED\n`.
Acknowledgement: `SESSION REVISION ERROR HVM_SELECTED DRIVER_READY RESIDENT_ACTIVE EPT_AVAILABLE\n`.

The page accepts only the matching session and revision. The checkbox reflects
the acknowledged selection. Driver absence or rejected control remains an
explicit error. A missing acknowledgement times out after five seconds and
retains the previous displayed choice. File readers share read/write/delete
access and close before dispatch/wait, allowing consecutive atomic replacements.
Existing native hardware breakpoints
must be removed before selecting HVM. Active EPT bindings must be removed
before selecting native mode. Engine-selection success alone does not prove
that residency or an execution rule is active.
An active native target must be paused at a held Windows debug event before
changing the selection; an accepted asynchronous pause request is insufficient.

## Extending to another debugger

Keep driver policy in `DebuggerBackend`, wire structures in `shared/driver`,
and device access in `ArkDriverClient`. A new adapter supplies frontend-owned
real process/thread identities, observes its actual pending debug events, and
uses the native writer transaction for virtual debug registers. It must not
reuse CE attachment ownership for an existing native session. The versioned
`KSwordDebuggerCall` seam and session log/control contract can be shared without
copying x64dbg GUI branches into another debugger.

## Build and package

Run from the repository root. Use the configured 64-bit MSBuild, never a Win32
build host. Every MSBuild invocation requires all three architecture properties:

```powershell
$msbuild='C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe'
$hostArgs=@('/p:PreferredToolArchitecture=x64','/p:PROCESSOR_ARCHITECTURE=AMD64','/p:PROCESSOR_ARCHITEW6432=AMD64')
& $msbuild TitanEnginePlugin/KswordTitanEngine.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64 @hostArgs /m:1 /v:minimal
& $msbuild X96dbgExecutablePlugin/KswordX96dbgLauncher.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64 @hostArgs /m:1 /v:minimal
& tools/Build-X96dbgPayload.ps1 -PrepareDependencies -BuildTests
python tools/export_x96dbg_source.py
& tools/package_x96dbg_plugin.ps1 -X64dbgDirectory .deps/x64dbg-reference/bin/x64 -QtLicenseDirectory third_party/x64dbg_runtime_licenses
```

The payload builder verifies source revisions and the Qt archive hash, checks
HostX64 resolution, and applies the narrow source patch idempotently. Build
directories remain under `.deps/`. Packaging verifies AMD64 binaries, nonzero
canonical export implementations and the actual bridge engine marker, retains
original licenses/source, and creates hashes for every package file. Test
targets and build-only PDB/LIB/EXP/ILK files are excluded from the runtime.

Install by copying the resulting `x96dbg` directory into KSword's `plugin/`
directory. The supplied plugin ZIP has `plugin.json` at its root; extract it
into `plugin/x96dbg/` beside KSword's executable. Live HVM acceptance and current
verification limits are listed in `ksword-x64dbg-validation.md`.
