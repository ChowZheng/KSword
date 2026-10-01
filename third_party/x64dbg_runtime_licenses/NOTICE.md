# x64dbg runtime dependency notices

These files retain original license and copyright text for the x64dbg runtime
dependencies. The component list follows the pinned x64dbg source's
`docs/licenses.md`. License texts are not modified by KSword. Their origin URLs
and the hashes of the collected texts are recorded in `provenance.json`.

Qt 5.12.12 comes from the upstream x64dbg dependency archive identified in
`X96dbgIntegration/PINNED_BASELINES.json`. The Qt license files and bundled
third-party notices come from the corresponding `v5.12.12` Qt source tags.
The unmodified Qt sources are available at
<https://download.qt.io/archive/qt/5.12/5.12.12/single/> and
<https://github.com/qt/qtbase/tree/v5.12.12>.
Qt remains dynamically linked; KSword does not modify these Qt DLLs.

The native TitanEngine, GleeBug, DbgEng adapter and distorm original license
texts are copied directly from the pinned upstream checkout by the package
script. The original x64dbg and TitanEngine source archives are included in
`KSword-x96dbg-source.zip`, alongside the local KSword adapter sources and patch.

Other component source locations:

- asmjit: <https://github.com/asmjit/asmjit>
- asmtk: <https://github.com/asmjit/asmtk>
- XEDParse: <https://github.com/x64dbg/XEDParse>
- jansson: <https://github.com/x64dbg/jansson>
- Zydis: <https://github.com/zyantific/zydis>
- lz4: <https://github.com/x64dbg/lz4>
- Scylla: <https://github.com/NtQuery/Scylla>
- ldconvert: <https://github.com/x64dbg/ldconvert>
- LLVM demangling: <https://github.com/llvm/llvm-project>
- DeviceNameResolver: <https://github.com/x64dbg/DeviceNameResolver>

The upstream DeviceNameResolver checkout has no standalone license file; no
license text is invented for it here. Microsoft compiler, symbol/debugging and
D3D compiler components are retained unmodified from the pinned x64dbg runtime
dependency bundle, under their original Microsoft component terms. This notice
does not relicense third-party binaries under the KSword license.
