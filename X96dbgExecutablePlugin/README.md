# x96dbg KSword Tab plugin

The plugin ID is `x96dbg`; the debugger payload is the 64-bit `x64dbg.exe`.
The KSword Tab contains a control and log surface. x64dbg remains in its own
top-level window with its own fonts, colors and window layout.

`KswordX96dbgLauncher.exe --ksword-plugin tab -- --parent-hwnd HWND --host-pid PID`
creates a direct `WS_CHILD` window in the KSword host and returns the
`ksword-plugin/1` `tab_ready` event before starting the debugger. Payload errors
are displayed in the page and forwarded as log events. The launcher owns only
its own surface and observer handles; closing the Tab does not stop or detach
the debugger or its target.

The process context action uses `--ksword-plugin attach -- --pid PID`.
`info` and `check` perform no GUI launch. Native TitanEngine forwarding is
allowed when the KSword driver is absent. An HVM selection is a request to the
engine adapter, and all controls change only after a matching actual-state acknowledgement.

## HVM policy controls

The Tab offers normal/stealth HVM, ShadowPage memory writes, allowed fallback,
native context/suspend fallback, and a 1–32 Shadow page limit. The normal defaults
preserve the existing automatic fallback behavior. Selecting stealth requests
Shadow writes on and fallback off; enabling fallback explicitly still cannot
permit visible original-code INT3 or hardware debug registers for stealth
breakpoints. Ordinary data-write fallback is permitted when selected and is
always logged. Unsupported native PAGE_GUARD memory breakpoints are refused in
stealth. Stealth prefers a hidden execution view; the Windows debug-event channel
remains active, and the mode does not guarantee complete invisibility.

The status reports the actual native/EPT/ShadowPage route, active binding and
Shadow page counts, fallback count and last error. Controls are disabled while
awaiting an ACK, while the target is running, or while active breakpoints/Shadow
writes prevent changing policy. Pause and remove or restore those bindings
before changing options. Rejection retains the actual selection and options.
Only successfully confirmed options are saved in `x96dbg-options.ini` beside the
launcher. HVM activation is never saved: each new session starts off and applies
the saved policy through the common backend, even when no driver is present.

## Engine selection

Each launch creates `sessions/UUID/x64dbg.ini` with `[Engine] DebugEngine=4`.
The debugger starts using its existing `-userdir` option, so user preferences
outside this isolated session are preserved. The narrow x64dbg source patch
adds `DebugEngineKSword=4` and resolves it through the existing checked loader
to `KSword/TitanEngine.dll`. An unpatched debugger cannot use this package.
`-p PID` is x64dbg's existing attach option.

The frontend is pinned to PR #3974's `dbgeng` branch. Its File/Open accepts
minidump and TTD artifacts through the bundled DbgEng provider, while live
launch/attach uses native TitanEngine with optional KSword HVM routing. Only
the loaded KSword engine shows the KSword menu and installed-breakpoint details.
The Type column reports the real DR/EPT/Shadow/software/PAGE_GUARD/replay
mechanism rather than inferring it from the frontend hardware category.
HVM preferences are inactive in replay and preserved for the next live session.

## Control protocol

The child debugger receives:

- `KSWORD_DEBUGGER_LOG_FILE`: session UTF-8 log path.
- `KSWORD_DEBUGGER_CONTROL_FILE`: log path followed by `.control`.
- `KSWORD_DEBUGGER_STATE_FILE`: log path followed by `.state`.
- `KSWORD_DEBUGGER_SESSION_ID`: UUID without braces.

Atomic control files retain `SESSION REVISION HVM_SELECTED`, followed by
`v2 MODE SHADOW_WRITES ALLOW_FALLBACK CONTEXT_FALLBACK SUSPEND_FALLBACK MAX_PAGES`
and a newline. Legacy three-field requests remain supported and preserve the
currently configured options. State files retain `SESSION REVISION ERROR
HVM_SELECTED DRIVER_READY RESIDENT_ACTIVE EPT_AVAILABLE`, then append `v2`,
the six confirmed options, and `ACTUAL_PATH ACTIVE_BREAKPOINTS SHADOW_WRITE_PAGES
CAN_CHANGE FALLBACK_COUNT LAST_FALLBACK_ERROR`. Flags are `0` or `1`; mode is
`0=normal, 1=stealth`, path is `0=native, 1=EPT, 2=ShadowPage`.
The page ignores another session or an older revision. It displays failures
without changing the selected backend; a five-second missing acknowledgement
leaves the last actual values visible and permits a new request only if the
last actual state allowed changes. A matching failure ACK includes the actual
options after rollback, and a 500 ms heartbeat refreshes pause/binding eligibility.
Unknown versions, out-of-range values, stale sessions and trailing fields never
partially apply options. A legacy backend ACK does not enable unsupported controls. Runtime
engine support and actual live HVM validation are described in the backend
documentation shipped with the package.

Theme colors and language are the startup snapshots supplied by `PluginHost`.
The embedded surface uses Consolas at 10 pt. Reopen the Tab to refresh its
language or theme snapshot.

## Build and package

Build `KswordX96dbgLauncher.vcxproj` with 64-bit MSBuild, `Platform=x64`, and all
three HostX64 architecture properties required by the repository AGENTS file.
Run `tools/package_x96dbg_plugin.ps1` with the patched x64 debugger directory,
the built proxy and the canonical native engine. Only AMD64 executables/DLLs
are accepted. The package script validates all 64 canonical engine export
names without loading either DLL and retains the native engine under
`payload/x64dbg/TitanEngine.dll`. The proxy is under `payload/x64dbg/KSword/`.
The resulting `plugin/x96dbg/` directory is a local runtime payload and is
excluded from the source repository. The distributable ZIP has `plugin.json`
at its root; extract it into `plugin/x96dbg/` beside the KSword executable.
