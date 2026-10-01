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
engine adapter, and the checkbox changes only after a matching acknowledgement.

## Engine selection

Each launch creates `sessions/UUID/x64dbg.ini` with `[Engine] DebugEngine=4`.
The debugger starts using its existing `-userdir` option, so user preferences
outside this isolated session are preserved. The narrow x64dbg source patch
adds `DebugEngineKSword=4` and resolves it through the existing checked loader
to `KSword/TitanEngine.dll`. An unpatched debugger cannot use this package.
`-p PID` is x64dbg's existing attach option.

## Control protocol

The child debugger receives:

- `KSWORD_DEBUGGER_LOG_FILE`: session UTF-8 log path.
- `KSWORD_DEBUGGER_CONTROL_FILE`: log path followed by `.control`.
- `KSWORD_DEBUGGER_STATE_FILE`: log path followed by `.state`.
- `KSWORD_DEBUGGER_SESSION_ID`: UUID without braces.

Atomic control files contain `SESSION REVISION HVM_SELECTED` followed by a
newline. State files contain `SESSION REVISION ERROR HVM_SELECTED DRIVER_READY
RESIDENT_ACTIVE EPT_AVAILABLE` followed by a newline. Flags are `0` or `1`.
The page ignores another session or an older revision. It displays failures
without changing the selected backend; a five-second missing acknowledgement
leaves the previous selection visible and permits a new request. Runtime
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
