# 进程列表 R0 字段显示

- 保护状态列只显示原始字节；`Runtime pattern`、PDB 等字段来源保留在悬停提示中，用于诊断偏移来源。
- PPL 列按 `shared/driver/KswordArkProcessIoctl.h` 的 PS_PROTECTION 位域解码：Type 位 0–2、Audit 位 3、Signer 位 4–7。Type=0 显示 `-`，PPL/PP 显示 signer 名称和完整原始代码（如 `WinTCB(0x61)` / `WinTCB(0x62)`）；未知类型或 signer 保留 `Unknown(代码)`，不能误报无保护。缺失 PROTECTION_PRESENT 时继续显示不可用。
- HandleTable 可用时只显示地址或 `null`，字段来源放在提示中；没有可用偏移仍显示不可用，不把未采集字段当作空指针。
- 显示入口在 `ProcessDock.cpp` 的 `formatColumnText` 与 `processTableData`。新增提示同时定点补充双语包的 `context_translations` 和 `source_translations`，两者缺一会使 i18n 审计失败。
