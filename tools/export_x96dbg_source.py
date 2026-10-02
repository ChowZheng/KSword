"""Export the source used by the x64 debugger plugin, inside this repository."""
from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import zipfile
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", default="dist/KSword-x96dbg-source.zip")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = (root / args.output).resolve()
    if not output.is_relative_to(root) or output.suffix.lower() != ".zip":
        parser.error("The source archive must be a ZIP inside the repository")
    roots = [
        "TitanEnginePlugin", "X96dbgExecutablePlugin", "X96dbgIntegration",
        "DebuggerBackend", "third_party/x64dbg_abi", "third_party/x64dbg_runtime_licenses",
        "KswordARKDriver", "third_party/systeminformer_dyn",
        "shared", "Ksword5.1/Ksword5.1/ArkDriverClient", "Ksword5.1/Ksword5.1/ksword/string",
        "tools/debugger_vm_test",
    ]
    singles = [
        "LICENSE", "docs/ksword-x64dbg-backend.md", "docs/ksword-x64dbg-validation.md",
        "tools/Build-X96dbgPayload.ps1", "tools/package_x96dbg_plugin.ps1",
        "tools/export_x96dbg_source.py", "docs/ksword-debugger-vm-validation.md",
        "docs/ksword-x64dbg-api-review.md", "docs/ksword-x64dbg-pr3974-validation.md",
    ]
    if any(output.is_relative_to(root / name) for name in roots):
        parser.error("The source archive cannot replace or include its source inputs")
    excluded = {"x64", "x86", "obj", "bin", ".git", "__pycache__"}
    files: set[Path] = set()
    for name in roots:
        directory = root / name
        if not directory.is_dir():
            raise FileNotFoundError(directory)
        for path in directory.rglob("*"):
            if path.is_symlink():
                raise ValueError(f"Source symlink is not accepted: {path}")
            if path.is_file() and not excluded.intersection(path.relative_to(directory).parts):
                files.add(path)
    for name in singles:
        path = root / name
        if not path.is_file():
            raise FileNotFoundError(path)
        files.add(path)
    pins = json.loads((root / "X96dbgIntegration/PINNED_BASELINES.json").read_text(encoding="utf-8-sig"))
    upstream = [
        ("x64dbg", root / ".deps/x64dbg-reference", pins["x64dbg"]),
        ("TitanEngine", root / ".deps/x64dbg-reference/src/third_party/TitanEngine", pins["nativeTitanEngine"]),
        ("DbgEng", root / ".deps/x64dbg-reference/src/third_party/DbgEng", pins["dbgEng"]),
    ]
    source_manifest = []
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in sorted(files):
            name = path.relative_to(root).as_posix()
            data = path.read_bytes()
            archive.writestr(name, data)
            source_manifest.append({"path": name, "sha256": hashlib.sha256(data).hexdigest()})
        for name, checkout, revision in upstream:
            actual = subprocess.check_output(["git", "-C", str(checkout), "rev-parse", "HEAD"], text=True).strip()
            if actual != revision:
                raise ValueError(f"{name} source differs from the pinned revision")
            data = subprocess.check_output(["git", "-C", str(checkout), "archive", "--format=zip", revision])
            entry = f"upstream/{name}-{revision}.zip"
            archive.writestr(entry, data)
            source_manifest.append({"path": entry, "sha256": hashlib.sha256(data).hexdigest()})
        submodules = subprocess.check_output(
            ["git", "-C", str(root / ".deps/x64dbg-reference"), "submodule", "status", "--recursive"],
            text=True,
        )
        archive.writestr("upstream/submodule-revisions.txt", submodules)
        archive.writestr("source-manifest.json", json.dumps(source_manifest, indent=2) + "\n")
        archive.writestr("BUILD.md", """# Source archive

The top-level paths retain their KSword repository layout. The proxy and launcher
projects build directly with Visual Studio 2022 MSBuild, Release/x64. All three
HostX64 properties documented in docs/ksword-x64dbg-backend.md are required.
The shared protocol and current debugger/HVM driver sources are included under
shared/ and KswordARKDriver/, with the original System Informer license retained.
Building the driver additionally requires the matching Windows WDK.

upstream/ contains unmodified pinned x64dbg, native TitanEngine and DbgEng source ZIPs.
Apply X96dbgIntegration/patches/x64dbg-ksword-engine.patch to the x64dbg source.
Submodule revisions are recorded in upstream/submodule-revisions.txt; repository
URLs are retained in the original .gitmodules and CMake source.
tools/Build-X96dbgPayload.ps1 -PrepareDependencies retrieves the pinned complete
checkout and the verified unmodified Qt 5.12.12 SDK inside .deps/.

Original Qt and runtime dependency license texts and source links are under
third_party/x64dbg_runtime_licenses/. Native TitanEngine and x64dbg retain their
original licenses in the upstream ZIPs and in the binary plugin package.
""")
    print(f"SOURCE_ARCHIVE={output}")
    print(f"SOURCE_FILES={len(source_manifest)}")


if __name__ == "__main__":
    main()
