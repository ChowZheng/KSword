"""Run each API Monitor regression in the x64 MSVC developer environment."""
from pathlib import Path
import subprocess
import sys

tests = sorted(Path(__file__).resolve().parent.glob("test_*.py"))
for test in tests:
    print(f"REGRESSION_START={test.name}", flush=True)
    subprocess.run([sys.executable, str(test)], check=True, timeout=300)
    print(f"REGRESSION_PASS={test.name}", flush=True)
print(f"REGRESSION_RESULT=SUCCESS TEST_COUNT={len(tests)}", flush=True)
