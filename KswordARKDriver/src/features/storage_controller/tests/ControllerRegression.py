"""Replay production controller C with bounded KMDF/port/DMA substitutes.

Requires native x64 MinGW GCC (Windows LLP64 ABI); never installs or loads a
driver and never accesses physical storage. Generated files stay in build logs.
"""
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[5]
SOURCE = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / ".codex-build-logs" / "controller-regression"
OUTPUT.mkdir(parents=True, exist_ok=True)


def function(path, name):
    source = path.read_text(encoding="utf-8-sig")
    match = re.search(r"^(?:static\s+)?(?:VOID|NTSTATUS|BOOLEAN|PVOID)\s+" +
                      re.escape(name) + r"\([^;{]*\{[\s\S]*?^\}", source, re.M)
    if not match:
        raise RuntimeError(f"Missing production routine: {path.name}:{name}")
    return f'\n#line {source[:match.start()].count(chr(10))+1} "{path.name}"\n{match[0]}\n'


header = (SOURCE / "controller.h").read_text(encoding="utf-8-sig")
context = header[header.index("#define KSCC_POOL_TAG"):header.index("EVT_WDF_DRIVER_DEVICE_ADD")]
context = re.sub(r"^WDF_DECLARE_CONTEXT_TYPE_WITH_NAME.*$", "", context, flags=re.M)
(OUTPUT / "controller_context_replay.h").write_text(context, encoding="utf-8")
api = header[header.index("NTSTATUS KsccAllocateDmaRegion("):]
(OUTPUT / "controller_api_replay.h").write_text(api, encoding="utf-8")

replay = ""
for name in ("KsccClearSessionState", "KsccAdvanceGeneration", "KsccValidateAtaGeometry",
             "KsccEvtSurpriseRemoval", "KsccMarkStopUnverified", "KsccResetIdentity",
             "KsccEvtD0Entry", "KsccEvtD0Exit"):
    replay += function(SOURCE / "device.c", name)
for name in ("KsccAllocatePool", "KsccEvtFileCleanup", "KsccRequestOwnerClosed", "KsccEvtContextCleanup"):
    replay += function(SOURCE / "driver.c", name)
for name in ("sha256.c", "controller_ioctl.c", "controller_transaction.c",
             "controller_audit.c", "controller_nvme.c", "controller_ide.c", "controller_ahci.c"):
    text = (SOURCE / name).read_text(encoding="utf-8-sig")
    if "WdfRequestGetRequestorProcessId" in text:
        raise RuntimeError(f"Requestor API is unavailable in the configured KMDF headers: {name}")
    replay += f'\n#line 1 "{name}"\n' + text.replace('#include "controller.h"', "", 1)
(OUTPUT / "controller_source_replay.h").write_text(replay, encoding="utf-8")

entry = ROOT / "KswordARKDriver" / "src" / "framework" / "driver_entry.c"
entry_source = entry.read_text(encoding="utf-8-sig")
state = entry_source[entry_source.index("typedef struct _KSWORD_ARK_CORE_LIFECYCLE"):
                     entry_source.index("/* An optional INF")]
(OUTPUT / "core_lifecycle_state_replay.h").write_text(state, encoding="utf-8")
core_replay = ""
for name in ("KswordARKDriverReadPnpProfile", "KswordARKDriverInitializeCoreLifecycle",
             "KswordARKDriverCoreEnterRequest", "KswordARKDriverCoreLeaveRequest",
             "KswordARKDriverCoreIsControllerDevice", "KswordARKDriverCoreAttachController",
             "KswordARKDriverCoreDetachController", "DriverEntry", "KswordARKDriverStartCore",
             "KswordARKDriverShutdownCore", "KswordARKDriverEvtDriverUnload",
             "KswordARKDriverEvtDriverContextCleanup"):
    core_replay += function(entry, name)
(OUTPUT / "core_lifecycle_replay.h").write_text(core_replay, encoding="utf-8")
# Global feature entry points are bounded substitutes; lifecycle logic is exact.
feature_names = sorted(set(re.findall(r"\b((?:KswordARK|KswordArk|KswRxpf)\w+)\s*\(", core_replay)))
special = {"KswordARKDriverReadPnpProfile", "KswordARKDriverInitializeCoreLifecycle",
           "KswordARKDriverCoreEnterRequest", "KswordARKDriverCoreLeaveRequest",
           "KswordARKDriverCoreIsControllerDevice", "KswordARKDriverCoreAttachController",
           "KswordARKDriverCoreDetachController", "KswordARKDriverStartCore",
           "KswordARKDriverShutdownCore", "KswordARKDriverEvtDriverUnload",
           "KswordARKDriverEvtDriverContextCleanup", "KswordARKAllocateNonPagedPool",
           "KswordARKDriverCreateControlDevice", "KswordARKDriverPublishControlDevice",
           "KswordArkStartupGetOsBuildNumber", "KswordArkStartupFailure",
           "KswordARKHvmEnableResidentLifecycle"}
feature_macros = []
for name in feature_names:
    if name not in special:
        action = "TestCoreUninitialize" if name.endswith("Uninitialize") or name.endswith("Shutdown") else "TestCoreInitialize"
        feature_macros.append(f'#define {name}(...) {action}("{name}")')
for name in sorted(set(re.findall(r"\bKswordArkStartStage\w+", core_replay))):
    feature_macros.append(f"#define {name} 0U")
(OUTPUT / "core_feature_stubs.h").write_text("\n".join(feature_macros), encoding="utf-8")

compiler = shutil.which("gcc")
if not compiler:
    sys.exit("Native x64 MinGW gcc is required for controller regression")
executable = OUTPUT / "controller_regression.exe"
subprocess.run([compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-Wno-multichar", "-Wno-unused-function", "-I", str(OUTPUT),
                "-I", str(ROOT), str(SOURCE / "tests" / "controller_regression.c"),
                "-o", str(executable)], check=True)
subprocess.run([str(executable)], check=True)
