# x64dbg PR #3974 integration and validation

Date: 2026-10-01. This record separates source/build checks, provider tests and
actual VM UI acceptance. It supplements earlier CE/x64dbg validation; the driver
and CE binaries were not replaced for this migration.

## Pinned inputs and implementation

| Input | Revision |
| --- | --- |
| x64dbg PR #3974 / `dbgeng` | `de6fc34df7cb01edea28f40c8b5c49d37bc1c0fb` |
| Native TitanEngine | `21ef77f31fd42d17f785802c36b2ca4ca44c43d7` |
| DbgEng provider | `e452d24fa87754fea2fd6b5abbadc1929291ce72` |
| Runtime deps | `b6b5684eac7698c81f93232a998a896cfff80d56` |

All 64 canonical declarations/exports, including the PR's 27 added exports,
remain required and typed. Live sessions use native Titan transport with shared
HVM adaptation; minidump/TTD use the checked DbgEng provider. Provider dispatch
remains selected through frontend teardown and synthetic handle closure.

The new API surfaces are covered as follows:

| Surface | Implementation |
| --- | --- |
| Unsafe read; query/allocate/free/protect | Native provider normally; shared backend for live HVM; captured provider memory in replay |
| `InitReplayW`, session info, position/extent/seek/run-back/step-back | Actual DbgEng provider results and capabilities; live reverse remains unsupported |
| Process/module paths, close, WOW64 | Current provider; replay identities never enter live Win32/R0 calls |
| Process termination and thread create/suspend/resume/terminate/ID/priority/times/cycles | Provider semantics; shared live-HVM suspend/resume and owned-state retirement |

Optional commands 8/9 expose current engine information and installed breakpoint
mechanisms. The frontend keeps upstream categories/slots while displaying the
actual binding in Type and details. HVM menus read acknowledged state and are
absent for other engines. Replay policy changes are disabled/rejected.

## Builds and automated checks

All builds used 64-bit MSBuild/MSVC, HostX64/x64 and the three required architecture
properties. Existing build directories were reused.

- Frontend/core/bridge, native Titan, DbgEng, headless runner and test binaries:
  build passed. Upstream warnings remain; this is not a zero-warning claim.
- Updated proxy and shared-backend model: build passed. Proxy final build has
  zero errors/warnings.
- Shared-backend model regression: passed, including context preservation,
  ownership rejection, Shadow writes/restore, original/data semantics and ABI
  pointer isolation.
- Proxy ABI/control tests: passed native forwarding, struct sizes, LastError,
  missing-export rejection, native binding policy guards, optional engine query,
  installed/native software mechanism and retired-binding `ERROR_NOT_FOUND`.
  These tests leave HVM disabled and do not prove live driver behavior.
- Native provider cases `script_run_exit/memory`, `multi-session`, and
  `swbp_stale/step`: passed, 11 assertions in total.
- `replay_minidump`: passed all 11 assertions after fixing provider startup,
  including repeat open/close, captured context/code/PEB, threads/maps and
  immutable execution rejection.
- `replay_ttd`: skipped as `fixture_unavailable`. No valid recording is installed;
  reverse execution, exact seek and recorded-exit navigation are implemented by
  delegation but are not runtime accepted here.

The first minidump run failed with an access violation in DbgEng's
`InitReplayW`/`OpenDumpFileWide`: its worker client had not been initialized.
The reference DbgEng case passed and established the adapter startup difference.
The adapter now invokes the provider's `EngineCheckStructAlignment` handshake
before `InitReplayW`, matching upstream bridge startup. The initial failure log
is retained separately from the passing rerun.

Evidence under `.codex-build-logs/debugger-vm/` includes
`pr3974-frontend-build.log`, `pr3974-titan-replay-fix-build.log`,
`pr3974-model-tests.log`, `pr3974-proxy-tests.log`,
`pr3974-adapter-regression.log`, `pr3974-dbgeng-reference.log` and
`pr3974-replay-fixed.log`. Headless provider checks are separate from UI acceptance.

## VM UI acceptance

VM: `KSword-HVM-Target`, Windows 11 build 22621.4317, two vCPUs, existing tested
driver. All frontend actions used PowerShell Direct to drive the interactive
desktop cursor and keyboard with foreground/window PID guards. No debug command
or backend script replaced these UI steps. File transfer, fixture generation,
hash verification and an elevated interactive launch task were prerequisites.

Frontend PID 3648, target `BreakpointDemoGUI-x64.exe` PID 10464:

| UI evidence | Result |
| --- | --- |
| `ui-0602` onward | New PR runtime launched, KSword-only menu available, actual policy checked |
| `ui-0644`–`ui-0645` | GUI execution request installed; Hardware group Type says **Shadow INT3**; details report slot 0, execute access, 1-byte coverage, unchanged original code, armed execution byte, fallback 0 |
| `ui-0647`–`ui-0650` | Run and click Increment in demo; actual stop delivered on main thread 5344 at `00007FF7472B1000` |
| `ui-0652` | Binding still installed; details now say execution bytes temporarily restored, logical binding retained |
| `ui-0654`–`ui-0656` | Delete, Run and Detach succeeded; target retained |
| `ui-0661`–`ui-0662` | Open `.dmp` via File menu; captured registers/code displayed; provider DbgEng, Minidump read only, capabilities `0x7`, configured HVM 1 / active HVM 0; HVM mutation controls disabled |
| `ui-0665`–`ui-0668` | UI Run did not execute immutable dump; Close completed |
| `ui-0672`–`ui-0674` | Attach live target again after dump cleanup; provider switched back to TitanEngine + KSword, live HVM active, zero owned bindings/Shadow pages |
| `ui-0686` | Select TitanEngine in Preferences, Save, close and relaunch; KSword top menu absent |
| `ui-0695`–`ui-0696` | Native CPU breakpoint submenu retains **Set Hardware on Execution**, with no KSword diagnostic action |
| `ui-0699`–`ui-0705` | Detach native session; restore fixture preference to KSword and close debugger, preserving the running target |

Backend log corroborates the hidden hit:

```text
ShadowPage INT3 removed: pid=10464 va=0x7FF7472B1000; physical execution view removed, native engine may retain a logical breakpoint
[TitanEngine] ShadowPage execution breakpoint hit: pid=10464 tid=5344 va=0x7FF7472B1000 slot=0 exception=0x80000003; actual=hidden INT3
```

Current UI binaries SHA256:

| Binary | SHA256 |
| --- | --- |
| x64dbg.dll | `F1F89E07E860E34C19D49EE2F3B13ACF0B95A4D102BE0B6FAE407B8530244DC4` |
| x64gui.dll | `14A36576A0B9C8619CFAEA8A60D423D367FCFE97949505504F1F8E0263A8F36C` |
| x64bridge.dll | `A1FC867F83428DFF3AEDFA06FD60C76D74706892A11CC7BCC0DA9687B55221E1` |
| KSword/TitanEngine.dll | `484CD3BE291379426594181D518AB4B23F75638DFA8B8F74695ABFE106C34050` |
| DbgEng/TitanEngine.dll | `73870C7C98CB6379F044D6CCEF6D1A124645D69B664252E11C0780030FC2CB70` |

## Boundaries

- VM lacks MTF for the original EPT/#DB execute path. Actual UI acceptance here
  is the Shadow INT3 path, not direct EPT execute acceptance.
- Strict HVM reads of some nonresident metadata returned error 299 /
  `STATUS_NOT_FOUND`; ordinary memory fallback was not performed. The upper
  address-space memory-query boundary also returned an explicit driver error.
  These results do not establish universal page availability.
- Live Windows debug transport remains visible. Hidden code bytes do not prove
  the debuggee cannot discover a debugger.
- Physical arming is reported only for tracked Shadow patches; the query does
  not synthesize physical DR/EPT state or provide a raw execution-view reader.
- This record verifies the listed added API paths and UI changes. It does not
  replace the earlier CE/native frontend suite or certify TTD without a trace.
