"""Obtain the other MSVC target environment without changing the host architecture."""
import os
from pathlib import Path
import subprocess


def target_environment(architecture):
    installation = os.environ.get("VSINSTALLDIR")
    if not installation:
        raise RuntimeError("run regressions from a HostX64 MSVC developer environment")
    setup = Path(installation) / "Common7/Tools/VsDevCmd.bat"
    command = f'"{setup}" -arch={architecture} -host_arch=x64 >nul && set'
    output = subprocess.check_output('cmd /d /s /c "' + command + '"', text=True)
    environment = os.environ.copy()
    for line in output.splitlines():
        if "=" in line and not line.startswith("="):
            key, value = line.split("=", 1)
            environment[key.upper()] = value
    return environment
