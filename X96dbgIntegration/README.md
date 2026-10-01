# x64dbg integration baseline

The KSword plugin ID is `x96dbg`; the executable is x64dbg for AMD64 only.
`PINNED_BASELINES.json` identifies the audited upstream source and the Qt SDK.

The upstream patch adds one engine selection, `DebugEngineKSword = 4`, through
the existing checked loader. It also adds the display name in Settings and
build information, and the same selection to the headless test runner. It does
not modify the generic debugger's context, breakpoint or debug-loop algorithms.

Canonical ABI declarations remain in `third_party/x64dbg_abi/`; adapter logic
is in `TitanEnginePlugin/`; the reusable driver backend is `DebuggerBackend/`.
The standalone frontend and KSword control/log page are connected by
`X96dbgExecutablePlugin/`.

See `docs/ksword-x64dbg-backend.md` for routing and build instructions, and
`docs/ksword-x64dbg-validation.md` for measured results and live HVM acceptance.
