# 进程列表 R0 字段显示

- 保护状态列只显示原始字节；`Runtime pattern`、PDB 等字段来源保留在悬停提示中，用于诊断偏移来源。
- PPL 列按 `shared/driver/KswordArkProcessIoctl.h` 的 PS_PROTECTION 位域解码：Type 位 0–2、Audit 位 3、Signer 位 4–7。Type=0 显示 `-`，PPL/PP 显示 signer 名称和完整原始代码（如 `WinTCB(0x61)` / `WinTCB(0x62)`）；未知类型或 signer 保留 `Unknown(代码)`，不能误报无保护。保护状态、PPL、HandleTable、SectionObject 未采集时均显示 `-`，字段来源保留在悬停提示中。
- HandleTable、SectionObject 只显示实际地址，移除 `Available:` 与来源括号；字段不可用或地址为空都显示 `-`，字段来源放在提示中。
- 显示入口在 `ProcessDock.cpp` 的 `formatColumnText` 与 `processTableData`。新增提示同时定点补充双语包的 `context_translations` 和 `source_translations`，两者缺一会使 i18n 审计失败。

## SectionObject 实机核查（2026-10-03）

- 本机内核 `10.0.19041.7725`，RSDS `63950D24-7362-0017-E1FC-214544590A77/1`，PE SHA256 `b3de5c162d582e0e3cbb27977fd82c2d8212d18fbc3f70bb8bf7e0c4a5925960`。精确 PDB 与导出访问器均给出 `_EPROCESS.SectionObject=0x518`。
- 已加载服务使用 `Ksword5.1/x64/Ksword-nice/KswordARK.sys`，该运行目录的旧压缩配置包只有 2405 项且没有当前内核匹配项。驱动现场的 `EpSectionObject` 为 `0xFFFFFFFF` / Unavailable，进程枚举因此未读取该字段。已备份并同步新版 2406 项包，原有 2405 项逐项完全一致。
- `process_resolver.c` 的运行时回退在驱动初始化期间取 `PsGetCurrentProcess()` 校验候选对象；加载阶段的 System 样本在 `0x518` 处实测为 0，普通进程则为非零。只用该样本校验会把正确偏移拒绝为不可用。此回退代码尚未修改；不能把“全不可用”认定为所有进程都没有 SectionObject。
- 通过现有 `dyn apply-profile` 接口只合入精确字段 item 2 后，驱动回读为 `0x518` / PDB profile，独立 PowerShell 枚举返回有效 Section 指针；前 30 条记录中 25 条非零。System、Secure System、Registry 等可能真实为空。此验证没有写 EPROCESS、替换或重新装载驱动。
- 进程列表指针可读不代表完整 Section 映射枚举可用：`section query-process` 仍受 `_SECTION.ControlArea` 与映射链布局依赖约束，本次仍仅由公共文件对象路径获得 ControlArea。
- 现场日志与旧配置备份位于 `.codex-build-logs/process-columns-20261002/`。管理员只读诊断可用隐藏窗口的独立 PowerShell 进程执行 CLI 并落盘；普通会话的 Win32 5 仅代表访问被拒，不能据此推断驱动功能失败。
