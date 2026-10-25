#!/usr/bin/env python3
"""Compatibility entry for the production-C hotkey rule regression suite.

The old Python matcher and generated per-version driver profiles were retired.
Keep this command usable for existing scripts while running the same offline
PE corpus and negative cases as tools/test_hotkey_runtime_rules.py.
"""

from __future__ import annotations

from pathlib import Path
import subprocess
import sys


def main(argv: list[str] | None = None) -> int:
    suite = Path(__file__).resolve().with_name("test_hotkey_runtime_rules.py")
    command = [sys.executable, str(suite), *(sys.argv[1:] if argv is None else argv)]
    return subprocess.call(command, cwd=suite.parent.parent)


if __name__ == "__main__":
    sys.exit(main())
