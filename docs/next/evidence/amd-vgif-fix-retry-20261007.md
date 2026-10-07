# Physical GIF correction: signed host retry

Driver source commit: `189e3445`; candidate `artifacts/amd-perf-20261007-vgif-fix`.
Signed SYS SHA256: `5640b39a8d5a8d36c689d0401399f15906dbf24e24b901b3d5b8d0ff32888ec5`.
SYS/PDB RSDS: `20387c97-4039-43e5-88fd-b169677f89d5`, age 1. Metrics v11.

## Correction and validation

CLGI/STGI interception remains enabled even on vGIF-capable hardware; closed software GIF always retains the physical INTR masking and NMI interception contract. Hardware vGIF is no longer selected. VLS and private clean groups remain enabled in `resident-svm-accel`; WATCH, shared-root mode and the integer fast bridge remain disabled for this retry.

Standard MSVC/WDK x64 Release /WX compilation and link succeeded; API validator reported Universal, INF/catalog processing completed without warnings, and the integer machine-code gate passed (238 instructions). All 25 C fixture targets and six Python tests executed successfully. New fixtures exercise production interrupt preparation with a hardware-vGIF request, coordinator CLGI/closed entry/STGI/open entry, and acceleration preserving CLGI/STGI intercepts. The new provenance fixture originally lacked required 4 KiB alignment; that fixture allocation was corrected before the complete test run passed.

Actual signed kernel load, preparation, serial self-test 32/32, and resident commit 32/32 succeeded. VMware cold-started the single-vCPU/8192 MiB clone, reached EFI Windows Boot Manager/runtime, and the user observed the Windows logo/spinner. The original immediate host lock was not reproduced during this observation; this does not establish absence of future host faults or successful desktop boot.

## Retained hardware evidence

Raw directory: `artifacts/amd-perf-20261007-vgif-fix-live`.

- Initial asynchronous start: `run-20261007-125031-153Z`; startup-003 already shows VLS entries, real CLGI/STGI exits and L2 hardware exits. The first sampling script incorrectly treated one busy optimization record as a global failure; raw records were retained and read-only collection resumed without restarting the VM.
- `startup-capture-20261007-125313-767Z`: 15 raw/combined samples at two-second intervals. Final summary serialization failed, but status/metrics and VMware log copies had already been written. Root independently analyzed samples 0/7/14 using actual QPC intervals of 14.0123935 and 13.9786059 seconds, with all 32 hotspot and timing rows comparable.
- `three-sample-20261007-125828-421Z`: three independent samples around local 20:58:29, 20:58:40 and 20:58:52. All status records remain 32 prepared/self-tested/resident, ACTIVE and lastStatus=0. L2 NPF increased by 4,223,276, VMRUN by 42,613, CLGI/STGI by 42,615 each.
- `three-sample-20261007-130013-777Z`: another three samples; L2 NPF increased by 4,095,760, VMRUN/CLGI/STGI by 48,688 each. No CLI timeout occurred. Collection then stopped, retaining the VM, driver and explicitly elevated administrator worker.

vGIF selection/entry counts remain zero; requested flags are `0xB0005`, watchState=0. Optimization and flight snapshots have only 31/32 coherent/valid rows at some capture instants, with different busy CPUs. No latch was observed in readable flight records; unreadable rows are not evidence of no fault. Whole-set optimization totals cannot be subtracted when the valid CPU set changes. Hotspot/timing coverage was independently complete in the selected 0/7/14 analysis.

## Performance finding for the next change

The 27.9909994-second analyzed window contains 5,843,319 exits, including 4,964,126 L2 NPFs. Sampled root software cycles total 1,012,227,900; NPT walking accounts for 764,479,225 (75.52%), while NPT12 source synchronization accounts for 41,965,075 (4.15%). These are sampled software costs, not pure hardware VMEXIT time or a matched-workload speedup.

The same window has 59,742 NPT cache lookups, 49,301 hits, 10,441 resets and **9,499 pool recycles**. The current general path still uses the original 64-table probe-sized pool (`hvm_svm_nested_runtime.h`). When installation returns FULL, `hvm_svm_nested_execute.c` resets the entire shadow tree, invalidates its epoch and rewalks on subsequent faults. This is an observed contributor to excessive walks, not proof that every NPF has this cause. Real VLS selections occurred and VMLOAD/VMSAVE exits were absent from the compared hotspot window; continuing to focus only on eliminating those exits would miss the measured bottleneck.

Next isolate the per-root table capacity and whole-root recycle policy: separate bounded-probe sizing from general execution sizing, account for all pools within the existing 64 MiB table budget, and replace full resets under ordinary capacity pressure with an owned leaf-group reclamation policy plus mandatory hardware flush. Preserve fresh source validation and event retry; do not raise page permissions or suppress guest-required invalidation. Shared-root/write-watch mode still needs its own hardware test after the current single-core OS reaches the desktop.

Complete L2 desktop boot, eight-vCPU execution, stop/teardown and native unload have not been accepted in this retry. The last user screen report was logo/spinner; the user then left to bathe. No VM reboot or further mode change was performed while away.
