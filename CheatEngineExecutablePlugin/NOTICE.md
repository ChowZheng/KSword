# KSword Cheat Engine executable plugin

The launcher and `KswordCheatEnginePlugin.dll` are KSword project components.
Those KSword components are distributed under GNU GPL version 3 only; the
package retains the corresponding `LICENSE.txt`.

The packaged `payload/Cheat Engine/` directory is copied from the user's
locally installed Cheat Engine 7.6 distribution. Cheat Engine remains the work
of its original authors. Its official source is available at:

https://github.com/cheat-engine/cheat-engine

Original third-party notices shipped with Cheat Engine, including
`libiptlicense.txt` and `tcclib/COPYING`, are retained in the payload.

The package intentionally omits Cheat Engine's DBK/DBVM kernel payloads and
32-bit CE launchers. The CE adapter routes memory, thread context, suspension,
allocation and protection through the reusable `DebuggerBackend` and
`ArkDriverClient`. HVM mode adds strict private-window memory access and EPT
debugger stops. Windows still supplies debug-event transport and remote-thread
creation. See `DEBUGGER_BACKEND.md` for capability and granularity limits.
