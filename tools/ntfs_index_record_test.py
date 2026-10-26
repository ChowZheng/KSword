#!/usr/bin/env python3
"""Compile and test the production NTFS record parser without Qt or Windows."""
import argparse
import pathlib
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[1]
    source = root / "Ksword5.1/Ksword5.1/FileDock"
    compiler = shutil.which("g++") or shutil.which("clang++")
    if not compiler:
        raise SystemExit("A C++17 compiler (g++/clang++) is required")
    with tempfile.TemporaryDirectory(prefix="ksword-ntfs-test-") as folder:
        executable = pathlib.Path(folder) / "ntfs-index-record-test"
        command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-I", str(source), str(source / "NtfsIndexRecord.cpp"),
                   str(root / "tools/ntfs_index_record_test.cpp"), "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
