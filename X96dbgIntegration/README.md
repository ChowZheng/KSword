# x64dbg integration baseline

The KSword plugin ID is `x96dbg`; the executable is x64dbg for AMD64 only.
`PINNED_BASELINES.json` identifies the audited upstream source and the Qt SDK.

The upstream patch adds one engine selection, `DebugEngineKSword = 4`, through
the existing checked loader. It also adds the display name in Settings and
build information, and the same selection to the headless test runner. Failed
hardware and memory breakpoint installations roll back their newly created
frontend records, so the GUI cannot retain an enabled entry that suppresses a
later retry. A rejected enable-all hardware operation restores the failed
entry's disabled state and original slot metadata, preserving the engine error.
The generic context and debug-loop algorithms remain upstream implementations.

The baseline is PR #3974 commit `de6fc34df7cb01edea28f40c8b5c49d37bc1c0fb`,
including its minidump/TTD UI and session capability controls. The adapter selects
native TitanEngine for live sessions and the pinned DbgEng provider for replay.
KSword adds an engine menu and installed-mechanism diagnostics in CPU and
breakpoint menus. These appear only for the engine loaded at startup, not a
pending Settings selection. The breakpoint Type column displays the actual
installed mechanism without changing upstream software/hardware/memory grouping.

Canonical ABI declarations remain in `third_party/x64dbg_abi/`; adapter logic
is in `TitanEnginePlugin/`; the reusable driver backend is `DebuggerBackend/`.
The standalone frontend and KSword control/log page are connected by
`X96dbgExecutablePlugin/`.

See `docs/ksword-x64dbg-backend.md` for routing and build instructions, and
`docs/ksword-x64dbg-validation.md` for measured results and live HVM acceptance.
See `docs/ksword-x64dbg-api-review.md` for the PR #3974 interface review and
optional extensions needed to expose actual breakpoint mechanisms and views.
Current PR integration results are in `docs/ksword-x64dbg-pr3974-validation.md`.
