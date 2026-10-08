# KSword address navigation bridge

This x64dbg SDK plugin exposes a local, same-user named pipe for **Query** and
**Navigate**. The protocol is in `../NavigationProtocol.h`; clients verify the
pipe's server PID and retain the debugger process handle/creation time. The
server rechecks the target process object before and after GUI navigation and
acknowledges the actual selected address, PID, creation time and execution state.
Protocol v2 also requires a typed client receipt after the complete response is
read. The server waits for it with bounded I/O before disconnecting, preventing
unread response buffers from being discarded; v1 peers are rejected.

The bridge does not import a command executor or any attach, run, pause, detach,
breakpoint, context-write or memory-write API. All GUI calls run through
`GuiExecuteOnGuiThreadEx`. Timeout cancellation is atomically arbitrated before
the first GUI side effect, including after a slow target snapshot or CIP query.
If cancellation wins, no later navigation occurs. Once GUI navigation has been
committed, it cannot be undone safely: a timed-out caller receives
`ERROR_IO_PENDING` (result unconfirmed), and the launcher does not retry it. The
plugin refuses unloading while queued or executing DLL code remains. Pipe I/O
and GUI requests have bounded waits; the server cancels outstanding I/O when
unloading.

Only a launcher-created fresh, isolated session receives
`KSWORD_NAVIGATION_FRESH_ATTACH=1`. In that session the bridge enables the system
breakpoint pause and observes the official `CB_SYSTEMBREAKPOINT` then
`CB_PAUSEDEBUG` callbacks before reporting attachment readiness. Ordinary
installations opened by the user retain their settings. The coordinator waits
for a fresh attachment pause, rather than inferring successful attach from
`CreateProcess` or command enqueue success.

Build with `tools/Build-X64DbgNavigation.ps1 -Platform x64` (or `Win32`). The
HostX64 MSVC compiler is required even for the x32 plugin. Install
`KSwordNavigation.dp64` or `.dp32` into the matching debugger's `plugins` folder.
No third-party source is copied into this plugin; SDK ABI signatures are
dynamically resolved from already loaded debugger modules. Reference APIs:

- https://github.com/x64dbg/x64dbg/blob/development/src/bridge/bridgemain.h
- https://github.com/x64dbg/x64dbg/blob/development/src/dbg/_plugins.h
- https://help.x64dbg.com/en/latest/commands/gui/disasm.html
- https://help.x64dbg.com/en/latest/commands/gui/dump.html
- https://help.x64dbg.com/en/latest/commands/debug-control/AttachDebugger.html

Configure CMake with `-DKSWORD_NAVIGATION_TESTS=ON` for the x64 isolated fixture.
`NavigationTests` loads a fake SDK with no execution-control exports. It tests
real named-pipe exchange, wrong PID/creation time, malformed packets, exact
selected address, paused/running preservation, cancelled GUI work and unload
lifetime. Its synthetic target is its own process; it never performs a debug
attachment. A restricted execution sandbox may reject the pipe with Win32=5;
run this fixture in a normal local shell in that case. Passing the fixture does
not replace real x64dbg GUI or attach verification.
