# KSword x64dbg backend and Tab plugin

## Source and package

The plugin ID is `x96dbg`. The runtime uses AMD64 `x64dbg.exe` only. The KSword
page contains controls and forwarded logs; x64dbg has an independent window.
The page uses the KSword theme snapshot and Consolas. It does not change the
debugger's window parent, fonts, colors or layout.

| Component | Baseline |
| --- | --- |
| x64dbg engine ABI and frontend | `de6fc34df7cb01edea28f40c8b5c49d37bc1c0fb` |
| Native TitanEngine | `21ef77f31fd42d17f785802c36b2ca4ca44c43d7` |
| DbgEng replay provider | `e452d24fa87754fea2fd6b5abbadc1929291ce72` |
| Runtime dependencies | `b6b5684eac7698c81f93232a998a896cfff80d56` |
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
    DbgEng/TitanEngine.dll   minidump/TTD provider and its runtime DLLs
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

| API surface | Live, HVM off | Live, HVM on | Minidump / TTD |
| --- | --- | --- | --- |
| Init, attach, Windows debug port, DebugLoop | Native TitanEngine | Native TitanEngine and owned state lifecycle | Checked DbgEng provider, synthetic events and handles |
| Software breakpoints | Native; stealth rejects visible patching | Native loop with Shadow variants in stealth | Provider logical code breakpoints where capability permits |
| Generic memory breakpoints | Native PAGE_GUARD; stealth rejects | PAGE_GUARD only with explicit normal-mode fallback | Provider logical data breakpoints where capability permits |
| Hardware execute breakpoints | Native DR | EPT execute or logged Shadow INT3 fallback; stealth prefers Shadow | Provider logical code breakpoint |
| Hardware write/read-write breakpoints | Native DR | Explicit `ERROR_NOT_SUPPORTED`; page watch is a different operation | Provider logical data breakpoint |
| GPR, flags, SIMD/XSTATE context | Native Windows context | Native Windows context with virtual DR overlay | Provider captured context; writes capability restricted |
| StepInto/StepOver and Run | Native callbacks / Windows transport | Same transport; owned state validated before Continue | Provider execution; immutable dump rejects execution |
| Query/allocate/protect/free memory | Native | Shared R0 backend; successful free retires affected bindings | Provider memory map; modifications capability restricted |
| Public Safe/Unsafe read and WriteSafe | Native filtering / raw semantics | Strict private-window original-view read; policy-aware write | Captured provider memory; no live-driver access |
| Open/close, paths, WOW64, thread IDs/priorities/times/cycles, remote creation/termination | Native real handles | Native real handles | Provider-owned synthetic identities and capability restrictions |
| Suspend/resume thread | Native | Shared backend with explicit configured Windows fallback | Provider result; no live thread operation |
| Session info and replay position/extent/seek/reverse operations | Native capabilities and unsupported reverse result | Same live capability boundary | Exact DbgEng session kind/capabilities and replay results |
| Versioned `KSwordDebuggerCall` | Explicit readiness/error and policy | Shared HVM controls and installed-binding diagnostics | Session query available; changed live HVM policy rejected |

The canonical export table has no optimistic success stubs. The PR's 27 added
exports are included in the 64-export ABI. `Provider.cpp` maintains the selected
dispatch table through debug-loop exit and frontend handle cleanup. Before
`InitReplayW`, the late-loaded DbgEng provider receives the same
`EngineCheckStructAlignment` startup handshake as a directly loaded engine.
Live launch/attach switches back after retiring owned state. Engine variables
and breakpoint defaults are propagated to both providers.

Replay capabilities are the underlying DbgEng provider's actual capabilities. HVM event history does
not become a replay timeline, a new live-stop event, or a substitute for a
Windows debug event. Native mode retains its data hardware breakpoints.

EPT data monitoring is page based. Generic byte-range hardware write/read-write
requests cannot be represented as exact stops, so HVM mode rejects them before
changing the native callback table. Explicit Watch/diagnostic operations remain
available through the versioned KSword API, with their actual page semantics.

## KSword-only frontend menus

The menu uses the engine loaded at startup. Changing the Settings selection does
not change its identity until restart. Native TitanEngine, DbgEng, GleeBug and
StaticEngine do not display KSword entries.

- **KSword → Engine capabilities and mechanisms**: current provider/session,
  capabilities, configured versus active HVM, owned binding/Shadow counts and
  fallback state.
- HVM, stealth preference, Shadow code patches, explicit data/native fallback,
  Windows context/suspend fallback, Shadow page budget, and owned-write restore.
  Menu checks read acknowledged state; policy mutation is disabled in replay,
  while running, or when installed bindings prevent a change.
- CPU **Breakpoint → KSword: installed mechanism**, plus the matching action in
  the breakpoint list. The execution action explicitly uses KSword policy.
- The breakpoint list **Type** column displays the installed mechanism: DR,
  EPT execution, Shadow INT3/long INT3/UD2, original-page software variant,
  PAGE_GUARD, or replay code/data. Upstream category and slot bookkeeping remain.

Optional `KSwordDebuggerCall` commands 8/9 are defined in
`DebuggerBackend/KswordDebuggerApi.h`, with version/size checked structures of
160 bytes (engine), 24 bytes (breakpoint query) and 56 bytes (result).
They add no required TitanEngine export or driver wire structure. A missing
optional export reports unsupported. Other adapters, including CE, need not
implement these Titan-specific queries.

Breakpoint details come from successfully installed owned records. Disabled,
failed or retired entries return `ERROR_NOT_FOUND`. Details report actual
mechanism, requested/effective coverage, access, frontend slot and per-binding
fallback error. Shadow byte arming is queried from tracked execution patches;
temporary removal at a held hit keeps the logical binding installed. DR/EPT
physical arming is not claimed when it was not queried. Ordinary reads retain
the original view. EPT data diagnostics state their 4096-byte granularity and
live diagnostics state that Windows debug transport remains present.

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
The PR #3974 migration, provider startup fix, menu UI evidence and TTD fixture
boundary are recorded in `ksword-x64dbg-pr3974-validation.md`.
