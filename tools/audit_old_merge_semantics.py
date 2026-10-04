"""旧引用只读语义证据：只输出布尔值、计数、提交和非配置文件锚点。"""

from __future__ import annotations

import json
import re
import subprocess
from pathlib import Path


def git(*args: str) -> bytes:
    return subprocess.check_output(["git", *args])


def source(revision: str, path: str) -> str:
    value = git("show", f"{revision}:{path}")
    try:
        return value.decode("utf-8")
    except UnicodeDecodeError:
        return value.decode("gb18030", errors="replace")


def code_shape(value: str) -> str:
    # 只用于核对旧中文编码变化，不证明所有字符串语义相同。
    pattern = r'"(?:\\.|[^"\\])*"|/\*[\s\S]*?\*/|//[^\n]*'
    value = re.sub(pattern, lambda m: '""' if m[0].startswith('"') else "", value)
    return re.sub(r"\s+", "", value)


def main() -> None:
    monitor_path = "Main/GUIFunction/Monitor/MonitorMain.cpp"
    title_path = "Main/GUIFunction/Ksword5Title.cpp"
    process_path = "Main/GUIFunction/Process/ProcessInformation.cpp"
    merged_tree = git("rev-parse", "7f261257^{tree}").decode().strip()
    first_tree = git("rev-parse", "7f261257^1^{tree}").decode().strip()
    results: dict[str, object] = {
        "no_base_first_parent_tree_preserved": merged_tree == first_tree,
        "no_base_second_parent_readme_preserved": (
            git("rev-parse", "7f261257^2:README.md")
            == git("rev-parse", "7f261257:README.md")
        ),
        "process_detail_executable_shape_preserved": (
            code_shape(source("9cc6c917^1", process_path))
            == code_shape(source("9cc6c917", process_path))
        ),
        "legacy_monitor": {},
        "legacy_titlebar": {},
    }
    for revision in ["8658c055", "d1385ee5", "44efab1e", "54351aff",
                     "fde3ed69", "4ed1f92b", "7c097e14", "570d153f"]:
        monitor = source(revision, monitor_path)
        results["legacy_monitor"][revision] = {
            "process_start_event_present": '"Win32_ProcessStartTrace", false' in monitor,
            "process_stop_event_present": '"Win32_ProcessStopTrace", false' in monitor,
            "start_monitor_call_present": "g_eventMonitor.startMonitoring(selectedTypes)" in monitor,
            "generate_unique_session_call_present": "generateUniqueSessionName();" in monitor,
        }
        title = source(revision, title_path)
        results["legacy_titlebar"][revision] = {
            "request_admin_call_present": "RequestAdmin(" in title,
            "get_system_call_present": "GetSystem(" in title,
            "exit_flag_present": "Ksword_main_should_exit = 1" in title,
        }
    # 敏感 AI 文件只比较去除全部字面量和注释后的形状，绝不输出源码或配置值。
    ai_before = code_shape(source("d1385ee5^1", "AI/AiAPI.cpp"))
    ai_after = code_shape(source("d1385ee5", "AI/AiAPI.cpp"))
    results["legacy_ai_only_added_network_header"] = (
        ai_after.replace("#include<ws2tcpip.h>", "", 1) == ai_before
    )
    # 输出是提交摘要，限定一个不含配置的 UI 文件。
    kernel_path = "Ksword5.1/Ksword5.1/UI/KernelDisassemblyDialog.cpp"
    results["kernel_confirmation_history"] = git(
        "log", "--all", "--format=%h %s", "-SdangerousActionConfirmationsSuppressed",
        "--", kernel_path,
    ).decode("utf-8", errors="replace").splitlines()
    results["kernel_current_history"] = git(
        "log", "-6", "--format=%h %s", "--", kernel_path,
    ).decode("utf-8", errors="replace").splitlines()
    output = Path(".codex-build-logs/old-reference-semantic-check.json")
    output.write_text(json.dumps(results, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(results, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
