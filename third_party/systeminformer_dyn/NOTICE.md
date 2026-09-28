# System Informer DynData Notice

Imported for Ksword ARK Phase 0 DynData matching.

- Upstream project: System Informer
- Upstream URL: https://github.com/winsiderss/systeminformer
- License: MIT License, copied in LICENSE.txt
- Third-party licensing note: this package is governed by the license noted above and is not automatically subject to the Ksword project `LICENSE`.
- Upstream copyright: Copyright (c) 2022 Winsider Seminars & Solutions, Inc.
- Imported files: kphdyn.xml, kphdyn.c, kphdyn.h
- Local import target: third_party/systeminformer_dyn
- Import date: 2026-04-30
- Source commit: unavailable in the local snapshot (no .git metadata present)

Ksword uses the generated DynData table and a small type wrapper only. It does
not import KPH IOCTL protocols, communication layers, object/reference systems,
session-token verification, or tracing infrastructure.

`PfnNative.h` additionally contains a minimal x64 subset of PHNT's read-only
Superfetch/PFN identity ABI, adapted from `ntpfapi.h` and `ntmmapi.h` on
2026-09-28. Source: https://github.com/winsiderss/phnt . The same upstream MIT
copyright and license in `LICENSE.txt` apply. This subset does not import PFN
setters or assume the private in-memory `_MMPFN` layout.
