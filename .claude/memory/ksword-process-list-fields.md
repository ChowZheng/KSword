# 进程列表 R0 字段显示

- 保护状态列只显示原始字节；`Runtime pattern`、PDB 等字段来源保留在悬停提示中，用于诊断偏移来源。
- PPL 列按 `shared/driver/KswordArkProcessIoctl.h` 的 PS_PROTECTION 位域解码：Type 位 0–2、Audit 位 3、Signer 位 4–7。Type=0 显示 `-`，PPL/PP 显示 signer 名称和完整原始代码（如 `WinTCB(0x61)` / `WinTCB(0x62)`）；未知类型或 signer 保留 `Unknown(代码)`，不能误报无保护。保护状态、PPL、HandleTable、SectionObject 未采集时均显示 `-`，字段来源保留在悬停提示中。
- HandleTable、SectionObject 只显示实际地址，移除 `Available:` 与来源括号；字段不可用或地址为空都显示 `-`，字段来源放在提示中。
- 显示入口在 `ProcessDock.cpp` 的 `formatColumnText` 与 `processTableData`。新增提示同时定点补充双语包的 `context_translations` 和 `source_translations`，两者缺一会使 i18n 审计失败。
