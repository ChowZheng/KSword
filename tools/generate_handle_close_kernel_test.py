"""Compile the real close business function against a deterministic Windows API fixture."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
source = (root / "KswordARKDriver/src/features/handle/handle_close.c").read_text(encoding="utf-8")
for include in ('#include "ark/ark_driver.h"', '#include "../../platform/process_resolver.h"'):
    assert source.count(include) == 1, f"Unexpected production include: {include}"
    source = source.replace(include, "// Kernel headers replaced by the user-mode API fixture.")
fixture = (root / "tools/handle_close_kernel_test.cpp").as_posix()
Path(sys.argv[1]).write_text(f'#include "{fixture}"\n' + source, encoding="utf-8")
