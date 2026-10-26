#!/usr/bin/env python3
"""Build real NTFS/USN integration tests with native HostX64 MSVC and QtCore.

The caller provisions a dedicated NTFS volume with an active USN Journal and an
existing work directory on that volume. This runner never formats a volume or
creates/modifies a Journal. It cannot run on Linux or against the system volume.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qt-root", type=Path, required=True)
    parser.add_argument("--volume-root", "--volume", dest="volume", required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--cxx", default=shutil.which("cl"))
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("This integration test requires native Windows and a dedicated NTFS volume")
    compiler = Path(args.cxx).resolve() if args.cxx else None
    if not compiler or "/hostx64/x64/" not in compiler.as_posix().lower():
        parser.error("Run in the native MSVC HostX64 x64 developer environment")
    qt = args.qt_root.resolve()
    if not (qt / "include/QtCore/QCoreApplication").is_file() or not (qt / "lib/Qt6Core.lib").is_file():
        parser.error("The Qt root must contain native MSVC QtCore development files")
    if not args.work_dir.is_dir():
        parser.error("--work-dir must be an existing directory on the dedicated test volume")
    root = Path(__file__).resolve().parents[1]
    source = root / "Ksword5.1/Ksword5.1/FileDock"
    environment = dict(os.environ)
    environment["PATH"] = str(qt / "bin") + os.pathsep + environment.get("PATH", "")
    with tempfile.TemporaryDirectory(prefix="ksword-native-ntfs-build-") as temporary:
        work = Path(temporary)
        executable = work / "native_ntfs_index_tests.exe"
        command = [str(compiler), "/nologo", "/std:c++20", "/Zc:__cplusplus", "/permissive-",
                   "/utf-8", "/EHsc", "/MD", "/DNOMINMAX", "/DUNICODE", "/D_UNICODE", "/DQT_CORE_LIB",
                   "/I" + str(qt / "include"), "/I" + str(qt / "include/QtCore"), "/I" + str(source),
                   str(root / "tools/native_ntfs_index_tests.cpp"), str(source / "NtfsFastIndex.cpp"),
                   str(source / "NtfsIndexRecord.cpp"), "/Fe:" + str(executable), "/link",
                   "/LIBPATH:" + str(qt / "lib"), "Qt6Core.lib"]
        subprocess.run(command, cwd=work, env=environment, check=True, timeout=180)
        print("NATIVE_NTFS_INDEX_COMPILE=PASS HOSTX64=1 REAL_PRODUCTION_TUS=1", flush=True)
        subprocess.run([str(executable), args.volume, str(args.work_dir.resolve())],
                       cwd=work, env=environment, check=True, timeout=240)


if __name__ == "__main__":
    main()
