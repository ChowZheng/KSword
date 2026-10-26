#!/usr/bin/env python3
"""Run the shared-control input and first-paint regressions with native Qt.

These isolated component tests use production UI code and offscreen QtWidgets.
They do not launch KSword, load its driver or inspect another process.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys


TESTS = (
    ("button focus", "test_button_focus_ui.py", False),
    ("button theme integration", "test_button_theme_integration_ui.py", False),
    ("toolbar defaults", "test_toolbar_first_frame_ui.py", False),
    ("parameter geometry", "test_parameter_geometry_first_frame_ui.py", False),
    ("secondary forms", "secondary_layout_first_frame_test.py", False),
    ("theme preparation", "test_theme_first_frame_ui.py", True),
    ("message boxes", "test_message_box_first_frame.py", False),
    ("detail layouts", "test_detail_layout_first_frame.py", False),
    ("monitor defaults", "test_monitor_first_frame.py", False),
    ("table defaults", "test_table_default_first_frame_ui.py", True),
    ("companion defaults", "test_companion_ui_defaults.py", False),
    ("disk monitor layout", "test_disk_monitor_layout_ui.py", False),
    ("MFT search page", "test_ntfs_search_page_ui.py", False),
)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qt-dir", "--qt-root", dest="qt", type=Path, required=True)
    parser.add_argument("--msbuild", type=Path)
    args = parser.parse_args()
    if os.name == "nt":
        compiler = shutil.which("cl")
        if not compiler or "/hostx64/x64/" not in Path(compiler).as_posix().lower():
            parser.error("Run in the MSVC HostX64 x64 developer environment")
        if not args.msbuild:
            parser.error("Windows regressions require --msbuild with 64-bit MSBuild")
    root = Path(__file__).resolve().parent
    for label, filename, uses_msbuild in TESTS:
        print(f"SHARED_UI_BEGIN={label}", flush=True)
        command = [sys.executable, "-B", str(root / filename),
                   "--qt-dir" if uses_msbuild else "--qt-root", str(args.qt.resolve())]
        if uses_msbuild and args.msbuild:
            command += ["--msbuild", str(args.msbuild.resolve())]
        result = subprocess.run(command, cwd=root.parent, text=True, encoding="utf-8",
                                errors="replace", stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=300)
        if result.returncode:
            print(result.stdout, end="", flush=True)
            raise SystemExit(result.returncode)
        summaries = [line for line in result.stdout.splitlines()
                     if "CHECKS=" in line or "FIRST_FRAME=PASS" in line or "THEME_FIRST_FRAME_PASS" in line
                     or "FIRST_FRAME_UI=PASS" in line or line.startswith("TABLE_DEFAULT_")
                     or line.startswith("NTFS_SEARCH_PAGE_UI_PASS")]
        print("\n".join(summaries) if summaries else f"SHARED_UI_PASS={label}", flush=True)
    print("SHARED_UI_REGRESSIONS=PASS", flush=True)


if __name__ == "__main__":
    main()
