#!/usr/bin/env python3
"""Reject CI output that omits QADS or retains a DLL from an older template."""

import argparse
import hashlib
from pathlib import Path
import re
import sys


def verify_runtime(runtime_dir: Path, repository_root: Path) -> str:
    project = repository_root / "Ksword5.1" / "Ksword5.1"
    reference = project / "lib" / "qtadvanceddocking.dll"
    runtime = runtime_dir / reference.name
    for path in (reference, runtime):
        if not path.is_file() or path.stat().st_size == 0:
            raise ValueError(f"QADS runtime is missing or empty: {path}")
    expected_hash = hashlib.sha256(reference.read_bytes()).hexdigest()
    actual_hash = hashlib.sha256(runtime.read_bytes()).hexdigest()
    if actual_hash != expected_hash:
        raise ValueError(
            f"QADS runtime differs from the release commit's library: {runtime}; "
            f"expected SHA256 {expected_hash}, got {actual_hash}"
        )
    header = (project / "include" / "ads" / "ads_version.h").read_text(
        encoding="utf-8"
    )
    parts = []
    for component in ("MAJOR", "MINOR", "PATCH"):
        match = re.search(rf"^#define ADS_VERSION_{component}\s+(\d+)\s*$", header, re.M)
        if not match:
            raise ValueError(f"QADS version header is missing ADS_VERSION_{component}")
        parts.append(match.group(1))
    return f"QADS {'.'.join(parts)} verified; SHA256={actual_hash}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runtime_dir", type=Path)
    parser.add_argument(
        "--repository-root", type=Path, default=Path(__file__).resolve().parents[1]
    )
    args = parser.parse_args()
    try:
        print(verify_runtime(args.runtime_dir, args.repository_root))
    except (OSError, ValueError) as error:
        print(f"QADS runtime verification failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
