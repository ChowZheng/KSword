#!/usr/bin/env python3
"""Run actual NTFS journal replay bodies with QtCore and deterministic Win32 I/O."""
import argparse
import pathlib
import shutil
import subprocess
import tempfile


def function(source, signature):
    start = source.index(signature)
    parameters = source.index("(", start)
    depth = 1
    end = parameters + 1
    while depth:
        depth += (source[end] == "(") - (source[end] == ")")
        end += 1
    body = source.index("{", end)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qt-root", type=pathlib.Path, default=pathlib.Path("/tmp/ksword-qt/usr"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[1]
    source_dir = root / "Ksword5.1/Ksword5.1/FileDock"
    source = (source_dir / "NtfsFastIndex.cpp").read_text(encoding="utf-8-sig")
    extracted = "\n\n".join(function(source, signature) for signature in
                              ("void insertRecord(", "bool replay(", "void finish("))
    include = args.qt_root / "include/x86_64-linux-gnu/qt6"
    library = pathlib.Path("/usr/lib/x86_64-linux-gnu/libQt6Core.so.6")
    if not include.is_dir() or not library.exists():
        raise SystemExit("Qt6Core development headers and runtime are required; set --qt-root")
    compiler = shutil.which("g++") or shutil.which("clang++")
    if not compiler:
        raise SystemExit("A C++17 compiler is required")
    with tempfile.TemporaryDirectory(prefix="ksword-usn-test-") as folder:
        folder = pathlib.Path(folder)
        (folder / "production_replay.inc").write_text(extracted, encoding="utf-8")
        (folder / "production_refresh.inc").write_text(
            function(source, "NtfsFastIndex::Snapshot NtfsFastIndex::refresh("), encoding="utf-8")
        executable = folder / "ntfs-journal-replay-test"
        command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-fPIC",
                   "-I", str(source_dir), "-I", str(folder), "-I", str(include),
                   "-I", str(include / "QtCore"), str(source_dir / "NtfsIndexRecord.cpp"),
                   str(root / "tools/ntfs_journal_replay_test.cpp"), str(library), "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
