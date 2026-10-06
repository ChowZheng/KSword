"""Audit only the reviewed theme/entry recovery files, retaining full-workspace diagnostics."""

from pathlib import Path
import i18n_language_pack as language_audit


# 本次恢复涉及的源码清单；工作台并行改动不属于这里的成功结论。
RECOVERY_FILES = {
    "MainWindow.cpp", "theme.h",
    "HardwareDock/HardwareDock.cpp", "HardwareDock/HardwareDock.h",
    "HardwareDock/HardwareDock.Theme.cpp",
    "UI/SvgThemeIconManager.cpp", "UI/SvgThemeIconManager.h",
    "UI/ThemeAccentIcon.cpp", "UI/ThemeAccentIcon.h",
    "UI/ThemeControlGlyphs.cpp", "UI/ThemeControlGlyphs.h",
    "UI/DockThemeIcons.cpp", "UI/DockThemeIcons.h",
    "UI/PerformanceChartTheme.cpp", "UI/PerformanceChartTheme.h",
    "UI/ThemeColorRemap.cpp", "UI/ThemeColorRemap.h", "UI/GlobalUiBaseStyle.cpp",
    "Framework/LogDockWidget.cpp", "KernelDock/KernelDock.cpp",
    "MonitorDock/MonitorDock.cpp", "MonitorDock/ProcessTraceMonitorWidget.Ui.cpp",
    "ProcessDock/ProcessDock.cpp", "ProcessDock/ProcessDetailWindow.h",
    "ProcessDock/ProcessDetailWindow.BaseAndUi.cpp",
    "ProcessDock/ProcessDetailWindow.ActionAndUtil.cpp",
    "ProcessDock/ProcessDetailWindow.ExtendedActions.cpp",
    "StartupDock/StartupDock.Helpers.cpp", "ServerDock/ServiceDock.Helpers.cpp",
}


def main() -> int:
    """复用生产审计规则，仅过滤源码归属；语言键配对及占位符规则仍完整执行。"""
    source_root = Path(__file__).resolve().parent.parent / "Ksword5.1" / "Ksword5.1"
    semantic_references: dict = {}
    extracted = language_audit.extract_source_strings(source_root, semantic_references)
    scoped_strings = {
        text: entry for text, entry in extracted.items()
        if any(location.path in RECOVERY_FILES for location in entry.occurrences)
    }
    scoped_references = {
        key: occurrences for key, occurrences in semantic_references.items()
        if any(location.path in RECOVERY_FILES for location in occurrences)
    }
    zh_pack = language_audit.load_pack(source_root / "languages" / "zh-CN.json")
    en_pack = language_audit.load_pack(source_root / "languages" / "en-US.json")
    scoped_errors = language_audit.audit(scoped_strings, zh_pack, en_pack, scoped_references)
    workspace_errors = language_audit.audit(extracted, zh_pack, en_pack, semantic_references)
    print(f"RECOVERY_I18N_FILES={len(RECOVERY_FILES)}")
    print(f"RECOVERY_I18N_STRINGS={len(scoped_strings)}")
    print(f"RECOVERY_I18N_ERRORS={len(scoped_errors)}")
    print(f"FULL_WORKSPACE_I18N_ERRORS={len(workspace_errors)}")
    for error in scoped_errors:
        print(error)
    return 1 if scoped_errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
