# NPT02 capacity and leaf-group reuse candidate

This change follows the signed vGIF-fix measurements: 9,499 pool recycles and approximately 75.52% of sampled root software cost in NPT walking over a 27.99-second window. It does not claim every NPF came from capacity pressure.

## Implemented R0 behavior

- Separate general nested OS sizing (256 preallocated tables per root) from bounded probe sizing (64). Release walks the full owned ledger, including partial allocation failures.
- Increase exact source-word provenance from 512 to 4,096 and retain stable dependency IDs using a bounded reusable private bitset. External guard identities are not reused.
- When a new 4 KiB region needs a PT beneath an already existing PD, capacity pressure can disconnect one old PT and rebind its already allocated physical page to the empty PD slot. Upper-level tables and unrelated leaf groups survive. A bounded accessed-bit second-chance scan selects an eligible PT, while preserving strictly forward owned edges.
- Clear only the evicted group's leaves/dependencies, prune source words with no remaining dependents, advance the epoch, and discard the old resolved candidate. The next NPF performs a fresh walk. No data permissions are widened and no interrupted event is skipped.
- Update the owning session/shared cache's epoch after partial reclamation; otherwise the next virtual VMRUN would discard all surviving mappings as an epoch miss.
- Set FlushPending and retain the existing architecture-supported global-capable ASID flush/full-flush selection and real hardware-return acknowledgement. Untracked provenance, missing upper-level geometry and unsupported capacity cases keep the old whole-root reset fallback; corrupt ownership remains a fault.
- Account for local pools, identity tables, hardware watch clone/splits and shared pools in the original 64 MiB physical-table budget. Expanded shared roots fit a bounded registry count; saturation retains per-CPU fallback. For example, 32 local pools and 1,282 reserved identity/clone/split pages leave room for 26 shared 256-table roots, rather than allocating 64 oversized roots.

Existing capacity-recycle counters include partial group reuse; `nptCache.resets` still reports actual whole-cache reset decisions. Protocol layouts and metrics version remain unchanged. Hardware performance comparison should inspect NPF rate, reset/recycle relationship, populated shadow pages and sampled NPT walk cost for matched guest work.

## Offline validation and build

All 25 existing C targets and six Python tests executed successfully. The production shadow fixture passed 49,383 checks, including 8,209 consecutive group rebindings beyond two complete source-ID cursor rotations, preservation of unrelated PT/upper-level entries, accessed-bit second chance, source pruning, stale-candidate rejection, bounded source metadata and budget boundaries. These are software fixtures, not hardware TLB or performance evidence.

Standard MSVC/WDK Release x64 /WX compile/link succeeded. The x64 API validator reported Universal, INF/catalog generation completed without warnings, and the existing integer machine-code gate passed (238 instructions). Candidate SYS/PDB are in `artifacts/amd-perf-20261007-npt-reclaim` and are not signed or loaded.

R3 work remains independent: no GUI, language-pack, command-catalog or shared IOCTL structure changes; no main-program build. Driver output **and intermediate files** use this candidate's independent directory. The current service/VM were not loaded, started or reconfigured, and the preserved signed vGIF-fix candidate was not overwritten. Only HVM R0 sources, the independent HVM fixture and this topic's documentation/memory are included in the commit.

## Pending hardware discriminator

After candidate signing and a coordinated hardware window, run the same single-vCPU clone with `prepare-svm-accel/self-test/resident-svm-accel`; use the matching existing metrics-v11 CLI. Compare actual-QPC intervals and only coherent rows. First establish normal OS progress/desktop; then test eight vCPUs. Watch/shared-root mode remains a separate hardware experiment. A lower full-reset count alone does not prove a speedup or OS correctness.
