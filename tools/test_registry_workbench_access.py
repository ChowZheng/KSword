#!/usr/bin/env python3
"""Build and run access-service recording mocks with a supplied Qt MinGW SDK."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import uuid

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qt", default=os.environ.get("QTDIR"), required=not os.environ.get("QTDIR"))
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    compiler = shutil.which(args.cxx)
    if not compiler:
        raise SystemExit(f"C++ compiler unavailable: {args.cxx}")
    qt = Path(args.qt).resolve()
    if not (qt / "include/QtCore/QString").is_file():
        raise SystemExit("Qt MinGW SDK with headers and Qt6Core import library required.")
    environment = os.environ.copy()
    environment["PATH"] = str(qt / "bin") + os.pathsep + str(Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
    temporary_root = ROOT / "work/registry-access-tests"
    temporary_root.mkdir(parents=True, exist_ok=True)
    temporary = temporary_root / f"case-{uuid.uuid4().hex}"
    temporary.mkdir()
    try:
        binary = temporary / "access-tests.exe"
        subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
            "-isystem", str(qt / "include"), "-isystem", str(qt / "include/QtCore"),
            "-I", str(ROOT / "Ksword5.1/Ksword5.1"), str(ROOT / "tools/registry_workbench_access_tests.cpp"),
            "-L", str(qt / "lib"), "-lQt6Core", "-ladvapi32", "-o", str(binary)], check=True, env=environment)
        subprocess.run([str(binary)], check=True, env=environment)
    finally:
        if temporary.resolve().is_relative_to(temporary_root.resolve()):
            shutil.rmtree(temporary)


if __name__ == "__main__":
    main()
