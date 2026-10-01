# KSword x64dbg validation

## Initial host validation on 2026-09-30

All builds and tests below ran inside the repository using AMD64 MSBuild and
HostX64 MSVC. No GUI automation or GUI launch was performed. No driver was
loaded, and HVM residency was not started for these tests.

| Check | Result | Evidence |
| --- | --- | --- |
| Pinned native TitanEngine Release/x64 | PASS | `.codex-build-logs/titan-native-cmake-x64.log` |
| Patched x64dbg frontend/core/headless Release/x64 | PASS | `.codex-build-logs/x96dbg-payload-build-final.log` |
| Production adapter DLL Release/x64 | PASS | `.codex-build-logs/titan-proxy-x64.log` |
| Canonical ABI and actual native DLL fixture | PASS | `TitanEnginePlugin/tests/validate_proxy.py`, 64 exact canonical declarations/exports |
| Control file session/revision and invalid-value handling | PASS | Production DLL fixture, no driver required |
| Shared backend native-session model | PASS | `.codex-build-logs/debugger-native-session-tests-build.log` |
| Memory rounding/overflow and module ownership rules | PASS | `TitanEnginePlugin/tests/LifetimeTests.cpp` |
| Launcher Release/x64 and headless protocol/argument checks | PASS | `.codex-build-logs/x96dbg-launcher-tests.log` |
| Existing CE bridge with updated shared backend | PASS | `.codex-build-logs/ce-shared-backend-native-session-build.log` |
| Actual x64dbg native debug-loop regressions through KSword DLL | 7/7 PASS | `.codex-build-logs/x64dbg-headless-results.log` |

The native DLL fixture covers real process handles, session structure size,
context alignment, allocation/query/read/write/protection/free, native LastError
preservation, replay unsupported results and rejection of a native DLL missing
a required export. These tests load the production DLL; they do not exercise
an actual EPT stop.

The backend model covers native ownership without a second attach/wait/continue,
identity/generation checks, owned retirement, external-resident preservation,
virtual DR programming and Windows-writer rollback. Modelled driver outcomes
are not evidence of a live VMX/EPT execution.

## Supplied toolkit milestone status

The supplied toolkit's first successful milestone requires one complete live
EPT execute-breakpoint session: the correct stopped thread/instruction,
register and memory inspection of that stop, a register edit used on resume,
exactly one StepInto, correct Continue, delete/Stop/Detach cleanup and a repeat
session without stale state. The ordinary native forwarding baseline is also
part of that milestone.

The initial 7/7 host regressions below were followed by the complete native
suite and live ShadowPage fallback acceptance in `KSword-HVM-Target`. Real
stops, context editing, exact StepInto, repeat hits, slot replacement, same-page
merge/delete, cleanup and the actual GUI Tabs passed. The original MTF-dependent
EPT/#DB route cannot run in this VM because its VMX capabilities omit MTF.
See [VM validation](ksword-debugger-vm-validation.md) for the exact verified
route and remaining capability boundaries.

## Actual headless regressions

Run against the compiled pinned host and production DLL:

```powershell
New-Item -ItemType Directory .deps/x64dbg-reference/bin/x64/KSword -Force
Copy-Item TitanEnginePlugin/x64/Release/TitanEngine.dll .deps/x64dbg-reference/bin/x64/KSword/TitanEngine.dll -Force
python .deps/x64dbg-reference/src/tests/run.py --engine KSword --arch x64 cmdline_init cmdline_attach attach_pause membp/hardware swbp_stale/step membp/range-write script_run_exit/multi-session --artifacts-dir .codex-build-logs/x64dbg-headless --keep-artifacts --timeout 40
```

| Test | Assertions | Result |
| --- | ---: | --- |
| `cmdline_init` | 36 | PASS |
| `cmdline_attach` | 2 | PASS |
| `attach_pause` | 1 | PASS |
| `membp/hardware` | 7 | PASS; native execute, write and read/write |
| `swbp_stale/step` | 5 | PASS; software breakpoint with stepping |
| `membp/range-write` | 11 | PASS; native memory breakpoint |
| `script_run_exit/multi-session` | 3 | PASS |

The runner suppresses target console windows. The test targets are controlled
local programs; this does not attach to unrelated user applications. The KSword
engine remains in native mode throughout these baseline tests.

## Original MTF-dependent route: acceptance on supporting hardware

The original HVM execution-stop slice uses the existing driver #DB transport.
It remains unverified on hardware exposing MTF. The VM has instead passed the
automatic ShadowPage fallback described in the separate live report. The
following checklist applies specifically to the original route:

1. Load the matching KSword driver using the normal supported workflow. Open
   the x96dbg control/log Tab and launch a controlled x64 debug target.
2. With the target actually paused and no native hardware breakpoints, enable
   HVM and check the matching acknowledgement. Install an execution hardware
   breakpoint at a known instruction, then Run.
3. Confirm a real held Windows `EXCEPTION_SINGLE_STEP`, the correct RIP/thread,
   and the normal x64dbg breakpoint UI. A history entry by itself does not pass.
4. Read GPR, flags, x87/SSE/AVX state and memory. Change a harmless GPR and verify
   it through the normal native context view. Confirm logical DR slots remain
   visible and physical hardware slots stay neutral.
5. StepInto once. Confirm exactly one instruction's effects, coherent context,
   and EPT rearm behavior. Run and hit the execution breakpoint again.
6. Delete/reinstall, replace a slot, remove all, exit an owned thread, free an
   owned mapped region, exit the process, Stop and Detach. Confirm owned leases
   retire and another frontend/resident remains intact.
7. Confirm native-to-HVM selection is rejected while native hardware bindings
   exist; disabling HVM is rejected while EPT bindings/programming remain.
   Confirm HVM write/read-write generic hardware requests report unsupported
   without leaving callback or DR state, while native mode still accepts them.
8. Exercise missing driver, incompatible driver/ABI, denied control, failed EPT
   installation/retirement and stale acknowledgement. Failure must be visible
   and must not continue with a half-installed execution stop.

The Tab's visible layout/theme, independent debugger windows, forwarded logs,
Consolas font, consecutive acknowledged mode switches and log-surface close
behavior were subsequently verified in the VM. The original MTF-dependent
steps above still require supporting hardware. No replay/time-travel support
is claimed for KSword HVM, and no x86 runtime is included.
