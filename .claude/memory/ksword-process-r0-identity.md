---
name: ksword-process-r0-identity
description: R0-only 进程危险动作的创建时间来源、驱动对象身份校验与旧协议兼容约束
metadata:
  type: project
---

# R0-only 进程身份校验

进程表的“仅内核枚举可见”记录可能无法通过 `OpenProcess` 得到同一对象。Rootkit 还可能篡改
`UniqueProcessId`、活动链表或用户态查询路径，因此不能用 R3 `OpenProcess + GetProcessTimes`
替代驱动对目标 `EPROCESS` 的身份判断。

## 当前链路

- `KSWORD_ARK_PROCESS_ENTRY` v3 返回 `creationTime100ns`，值来自枚举时已引用对象的
  `PsGetProcessCreateTimeQuadPart`。无法引用的 CID 弱证据行返回 0。
- `ProcessDock` 对有真实 R0 创建时间的 kernel-only 行直接使用该时间建立 identity；旧驱动或
  弱证据行才使用 `KernelOnlyCreationTimeSeed + PID` 维持稳定显示键。
- R0 结束进程与 `BreakOnTermination/APC` 特殊标志请求携带
  `expectedCreateTime100ns`。驱动先按 CID/ActiveProcessLinks/PID 解析并引用 `EPROCESS`，再比较
  `PsGetProcessCreateTimeQuadPart`，不匹配返回 `STATUS_INVALID_CID` 且不执行写操作。
- 普通 R3 可见进程继续持有经 `GetProcessTimes` 验证的 Win32 查询句柄，并同时接受驱动侧校验。
  kernel-only 目标跳过 Win32 句柄校验，只使用驱动侧对象校验。
- 新驱动按请求字段偏移接受旧版终止/特殊标志固定前缀。旧请求缺失的创建时间按 0 处理。

## 约束

- 2026-10-02：按评估要求，常规 R0 进程结束与普通线程结束中的低 PID（0–4）及
  `PsInitialSystemProcess` 拒绝代码已注释保留；中央 safety 的 Idle/System 判断仅对
  `PROCESS_TERMINATE` 豁免。创建时间、线程归属、请求结构与确认策略仍须校验。
  通用 `KswordARKValidateUserPid` 供其他动作使用，不能全局关闭；HVM 的 PID 0/4
  保护按用户要求保留。取消驱动特判不代表内核终止必然成功，尤其 PID 0 仍可能无法解析。

- 新增按 PID/CID 定位的 R0 危险动作时，优先把期望创建时间放入 `shared/driver/` 请求结构，
  并在驱动解析出的对象上校验。
- 不要把 kernel-only 行的合成 identity 时间传给驱动。合成值只用于 UI 缓存键。
- 不要全局关闭 `ProcessDock::dispatchProcessActionTargetsInParallel` 的 R3 identity hold。
  普通 R3 变更动作只对明确标记为 `isKernelOnly` 的目标跳过。
- 独立的“R0 结束进程”列表动作不依赖 R3 `OpenProcess`：R3 可见目标须携带真实
  `creationTime100ns`，由驱动在已引用的 `EPROCESS` 上校验；缺失时拒绝下发。
  R3 动作及其组合链仍保留原有 identity hold，不能为解决受保护进程问题全局跳过。

## 进程详情的逐方法结束

- 主程序进程列表的“高级结束进程（逐方法）”、主程序详情页和 Light 详情页的 R3 条目共用
  `shared/ProcessTerminateMethods.h`，目前为 14 项。组合链也从该表执行，新增方法时只改这一处。
- 两个详情页执行单项 R3 方法前，都用捕获的创建时间验证目标并持有查询句柄，避免 PID 复用；
  R0 驱动结束单列在表外，仍走各自已有的驱动调用与身份保护。
- 详情页下拉框不包含 HVM 与 DMA：HVM 需要地址和常驻状态编排，DMA 需要另行指定写入地址。

## 2026-10-02 结束方法接入

- 用户先后取消 Ctrl+C 和 Ctrl+Break；R3 共享方法表仍为原有 14 项，未新增控制台信号或辅助进程入口。
- R0 顺序为清 PP/PPL → ZwTerminateProcess → PspTerminateProcess → Normal Kernel APC → 逐线程终止 → 清零可写用户内存。Psp 只消费精确 PE/RSDS 匹配的 v4 可选 item 1401（GlobalRva/core），检查可执行节和已核对的四参数入口序列（EPROCESS/排除线程/NTSTATUS/flags），未知 ABI 返回不支持；不猜 CALL 或复用其他内核 RVA；老配置缺失该项会继续后续方法。生成器和运行时 PDB 解析器均提供此项。
- 普通进程 APC 在目标线程自身上下文调用 ZwTerminateProcess(NtCurrentProcess())；与系统线程 APC 共用注册/取消/卸载排空，另持 EPROCESS 引用并核对上下文。未导出 PsGetNextProcessThread 时用 500ms 有界 CID 查询与精确 EPROCESS 归属校验回退。排队成功不等于目标退出。
- R3-only 组合动作必须以 includeR0Fallback 守住最后实际驱动调用，不能只守住回退前的存在性检查。

- 本机配套发布矩阵仅补充了当前内核的一条 item 1401：先校验 PE SHA256、RSDS GUID/Age、PDB DBI Age 与四参数入口序列，再压缩并回读；2406 个矩阵条目均保留。其他内核需重新生成/应用匹配 PDB 配置。
