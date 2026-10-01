"""Archive current KSword CE, shared backend and driver sources inside the repo."""
from __future__ import annotations

import argparse
import hashlib
import json
import zipfile
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", default="dist/KSword-cheat-engine-source.zip")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = (root / args.output).resolve()
    roots = ["CheatEngineExecutablePlugin", "CheatEnginePlugin", "DebuggerBackend", "shared",
             "KswordARKDriver", "third_party/systeminformer_dyn",
             "Ksword5.1/Ksword5.1/ArkDriverClient", "Ksword5.1/Ksword5.1/ksword/string",
             "tools/debugger_vm_test"]
    singles = ["LICENSE", "docs/ksword-debugger-vm-validation.md",
               "tools/package_cheat_engine_plugin.ps1", "tools/export_cheat_engine_source.py"]
    if not output.is_relative_to(root) or output.suffix.lower() != ".zip":
        parser.error("The source archive must be a ZIP inside the repository")
    if any(output.is_relative_to(root / directory) for directory in roots):
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
    output.parent.mkdir(parents=True, exist_ok=True)
    manifest = []
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in sorted(files):
            name = path.relative_to(root).as_posix()
            data = path.read_bytes()
            archive.writestr(name, data)
            manifest.append({"path": name, "sha256": hashlib.sha256(data).hexdigest()})
        archive.writestr("source-manifest.json", json.dumps(manifest, indent=2) + "\n")
        archive.writestr("BUILD.md", """# Corresponding KSword CE source

This archive contains the current local KSword launcher, CE bridge, shared backend,
IOCTL protocols and driver source, including changes not yet published upstream.
Paths retain their repository layout and original license texts.

Build CheatEngineExecutablePlugin/KswordCheatEngineLauncher.vcxproj and
CheatEnginePlugin/KswordCheatEnginePlugin.vcxproj with Visual Studio 2022 MSBuild,
Release/x64. Use PreferredToolArchitecture=x64, PROCESSOR_ARCHITECTURE=AMD64 and
PROCESSOR_ARCHITEW6432=AMD64. The driver additionally requires the Windows WDK.
The guest policy fixture and its live-validation boundaries are retained under
tools/debugger_vm_test/ and docs/ksword-debugger-vm-validation.md.

Bundled Cheat Engine 7.6 binaries are the unmodified user-installed distribution;
this archive contains KSword's corresponding sources, not the complete CE source.
Original CE source: https://github.com/cheat-engine/cheat-engine
Original CE and dependency notices remain in the binary plugin payload.
""")
    print(f"SOURCE_ARCHIVE={output}")
    print(f"SOURCE_FILES={len(manifest)}")


if __name__ == "__main__":
    main()
