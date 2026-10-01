# Guest policy regression

Run inside the prepared Windows x64 VM, with the current guest driver already
loaded. `ROOT` must contain `DebugTarget.exe`, the production native
`x64dbg/bin/x64/TitanEngine.dll`, and the production adapter
`x64dbg/bin/x64/KSword/TitanEngine.dll` with its dependencies.

```powershell
python tools\debugger_vm_test\policy_live.py C:\KSwordDebuggerTest --run-in-vm
```

The script does not install or load a driver. It calls the real Titan adapter and
shared C ABI. Each phase has a 65-second subprocess deadline and creates only its
own `DebugTarget` process. A kill-on-close Windows job retires that process when
the phase exits or times out. A failed phase stops the suite for investigation.

Artifacts are written to `ROOT/policy-regression/`: one stdout log, backend log,
target metadata and JSON result per phase, plus the combined `result.json`.
Success requires exit code 0, all seven phase results, and `FINAL PASS policy regression`.

Coverage:

- Offline default/options/query/restore calls keep the driver handle closed;
  successful and rejected settings return complete, actual ACKs.
- An opt-in execution-view edit changes `inc rax` to `dec rax`: the target returns
  `0xff`, while Titan and ordinary Windows reads retain the original code. Range
  restore returns execution to `0x101`.
- Oversized, cross-page, mixed executable/data, and page-budget requests are
  rejected before mutation. A previously installed code patch remains effective.
- Two separate targets execute the same `MEM_IMAGE` probe. Initially shared
  image backing must reject Shadow writes without changing either target. The
  fixture explicitly creates Windows COW-private backing with an identical-byte
  write, verifies the first page remains `MEM_IMAGE` with `Shared=0`, and then
  patches only its execution view. It returns `0x102`, both original mappings
  retain unchanged bytes, the peer still returns `0x101`, and restore returns
  the first target to `0x101`. This setup requires rebuilding `DebugTarget.exe`
  from the current Release/x64 project; the production backend does not
  automatically perform the fixture's same-byte COW write into a running target.
  The fixture disables incremental linking so its published probe points at the
  real isolated code section, rather than an incremental-link jump-table thunk.
- Ordinary RW data uses explicitly allowed fallback, changes the target's actual
  read result, and records both route and completed transfer. Disabling fallback
  rejects the write with zero transfer. Both normal and stealth policies are checked.
- Native normal, HVM normal, and HVM stealth breakpoints report their actual
  path and stop at the exact instruction through the real native debug loop.
  HVM normal checks the advertised EPT capability: unavailable EPT must report the
  logged Shadow fallback. Stealth requires Shadow and rejects HVM-off DR fallback.
- Active memory patches and frontend breakpoint bindings reject changed options
  with `ERROR_BUSY`, retain the actual options ACK, and allow identical requests.
- A stealth code patch and hidden INT3 share one page; restoring memory masks
  keeps the logical breakpoint hidden across its native one-instruction retry.
  A second real hit proves rearming after restore, then retiring it permits normal execution.

Executable page protection is not proof of code intent. The script edits a known
instruction in its own fixture. Execution-view patches do not change ordinary
data reads and are unsuitable for numeric freezes in RX/RWX data. This fixture
does not claim debug-port hiding, general debugger compatibility, or live proof
of injected rollback failures; those failure branches have separate backend models.

## Actual CE native data watchpoint

`ce_data_live.lua` runs in the disposable CE payload with the production bridge.
The launching fixture sets `KSWORD_TEST_ROOT` and a fresh
`KSWORD_DEBUGGER_LOG_FILE`, owns `DebugTarget`, and prepares a foreign standard
HVM resident before CE starts. The Lua fixture makes no HVM lifecycle calls.
It selects normal policy with explicit fallback and Shadow memory writes off,
then installs one eight-byte native write watchpoint at a safely held pause.

The callback requires the real Windows single-step event, matching physical DR
slot/DR6 evidence, post-store RIP and `RAX=0x101`. After removal, another safe
pause proves physical DR addresses and enables are cleared and the backend ACK
reports zero bindings. A second actual target store must complete without another
data callback. CE's address list includes inactive records awaiting its deletion
countdown, so retained list entries are recorded rather than treated as active
hardware ownership. Foreign HVM generation, full processor residency, zero
Shadow views/EPT rules, and `ownsResident=false` remain mandatory throughout.
The fixture has a 60-second deadline; the launcher must also bound and retire its
own CE/target processes. Success ends with `FINAL PASS CE native data watchpoint`.
