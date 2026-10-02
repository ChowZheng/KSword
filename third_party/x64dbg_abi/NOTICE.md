# Canonical x64dbg engine ABI

`TitanEngine.h`, `TitanEngine.def`, `check_titanengine_exports.py` and `LICENSE`
are copied without semantic changes from x64dbg commit
`de6fc34df7cb01edea28f40c8b5c49d37bc1c0fb` (PR 3974).
Upstream: https://github.com/x64dbg/x64dbg/tree/de6fc34df7cb01edea28f40c8b5c49d37bc1c0fb

The manifest and signature inventory are supplied by the user's
`KSword_x64dbg_agent_package.zip`; they describe the same 64 canonical exports.
The actual upstream header is the ABI authority. The native TitanEngine
forwarding dependency is pinned separately at
`21ef77f31fd42d17f785802c36b2ca4ca44c43d7`.

The proxy is released only for AMD64. No x86 ABI or WOW64 HVM support is claimed.
