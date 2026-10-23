# 内核 / 硬件 UI 对齐 Qt 审计

本轮工作是把 ARKLight 的内核页、硬件页按 Ksword5.1(Qt) 主程序的挂载结构与表格内容对齐。
本目录是审计过程产物，供复核与接手使用。

## 文件

| 文件 | 内容 | 真值来源 |
|---|---|---|
| `kernel-mount-map.md` | 内核 Dock 顶层/二级挂载对照，重构前后差异 | `KernelDock/KernelNavigationCatalog.h` + `KernelDock.cpp:611-830` |
| `qt-hardware-map.md` | 硬件 Dock 16 个顶层页及其列名 | `HardwareDock/` + `zHwPages` |
| `qt-kernel-columns-A.md` | object / io / atom / misc 组逐页列名 | 各 `*Tab.cpp` 的 `setHorizontalHeaderLabels` |
| `qt-kernel-columns-B.md` | audit / hal / wdf / slat / timers / workers / cid / ipc 组逐页列名 | 同上 |
| `arklight-hardware-columns-diff.md` | ARKLight 硬件页现有列 vs Qt 真值的逐页差异 | `Features/Hardware*` 的 `InsertColumn` |
| `status-overlap.md` | 状态文字被控件遮挡的清单与边界阈值 | 逐文件读布局常量 + 矩形求交 |
| `boundary-overlap-verify.md` | 矮窗口下详情/编辑列压状态行的核实与改法 | 同上，带算式复核 |

## 更正记录

### 2026-10-23 `d3e60987` 提交信息里的一处错误

提交信息写的是「Qt 这一页表头是英文硬编码，中文界面下显示成中英混杂」，
并把 NamedPipe 六列译成了中文。**前半句不准确**：

`KernelNamedPipeTab.cpp:211` 用的是 `ks::ui::FlatTableModel` 的 ColumnSpec，
表头确实走 `i18n::displayText`，而 `displayText` 在 zh-CN（产品源语言基线）
下直接 `return sourceText`，所以 Qt 在中文界面**原样显示这六个英文词**，
并不是「部分翻译、部分不翻译」。`zh-CN.json` 里存在的
`"Pipe Name" -> "管道名称"` 映射不参与这条路径。

结论不变但理由要改：Light 是纯中文产品（产品约定不引 i18n），
这一页译成中文是有意偏离 Qt 的原始显示，不是「跟 Qt 保持一致」。
后续若有人质疑为什么和 Qt 不一样，答案在这里。

### 同类偏离（有意不跟 Qt）

Qt 有几处把裸标识符直接当表头显示给用户，属于 Qt 侧缺陷，Light 不跟：

| 页面 | Qt 显示 | Light 显示 |
|---|---|---|
| `object.named` BaseNamedObjects | `scope / directoryPath / objectName / objectType / fullPath / symbolicTarget / statusText` | 作用域 / 目录路径 / 对象名称 / 对象类型 / 完整路径 / 符号链接目标 / 状态 |
| `object.links` 符号链接 | `sourceDirectory / linkName / fullPath / targetPath / dosCandidate / statusText` | 来源目录 / 对象类型 / 链接名称 / 完整路径 / 目标路径 / 状态 |
| `ipc.pipe` 命名管道 | `Pipe Name / NT Path / Attributes / LastWriteTime / Status / Source` | 管道名称 / NT 路径 / 属性 / 最后写入时间 / 状态 / 来源 |

## 数据缺口（不是 UI 能补的）

对列时发现一批「Qt 列存在但 Light 协议没有逐行字段」的情况，这些列**没有加**，
因为加了就是空列：

- 回调列表：公司 / 文件版本 / 文件描述（facade 只产 Altitude）
- 定时器 DPC：模块（facade 行里没有模块归属）
- 对象类型：访问掩码（别名指向 `ValidAccess`，facade 未产出该键）
- 平台审计：厂商 / 表地址 / 签名策略 / 备注
- 工作队列线程：线程状态 / 等待原因 / 参数 / 模块基址 / 模块路径 / R0 状态 / 管理保护
- CID 表：Qt 是逐条 14 列，Light 的 `CidTableSummary` 只出 PspCidTable 摘要行，数据模型不同
- 统一审计：Qt 跑真实扫描并聚合风险行，Light 的 `HookAuditSummary` 只汇总审计入口可用性

前 5 条是加字段就能补的；后 2 条要改数据层，不是列的问题。

## 硬件列对齐的结论（详见 `arklight-hardware-columns-diff.md`）

硬件这一侧和内核不一样：内核的列大多能靠「补 canonical + 别名」补上，硬件这
几页的差距**基本是数据层缺口，不是表头问题**，所以本轮只改了挂载结构，没强行
补列：

| 页面 | Qt | ARKLight | 结论 |
|---|---|---|---|
| 处理器 | 逐逻辑处理器 7 列（利用率/频率/温度/电压） | 单行 CPUID 快照 4 列 | 不是同一张表。要真对齐得接 per-CPU 性能计数器，属功能开发 |
| 磁盘活动 | 3 张表共 28 列（进程级读/写 B/s、I/O 优先级、响应时间） | 单表 10 列 | Qt 有进程级 I/O 归属，Light 没有这层数据 |
| 设备管理 | 6 列 QTreeWidget | 无表头的 Win32 TreeView | **可补**：给 TreeView 加列头即可，是纯 UI 活 |
| 底层硬件检查 | 24 列，含 CR0/CR4/EFER/MSR_LSTAR/SYSENTER_EIP/IDTR/GDTR 等 | 8 列 | 行由 `AppendDriverIntegrityRows` 产出，那些寄存器字段协议里根本没有 |
| i8042prt 审计 | 12 列 | 动态列（按行 key 生成） | 可给定固定列序，但要先确认每个 key 都有值 |
| 性能监控 | 左列表 + QStackedWidget 详情 | 单表 | 结构不同，未动 |

「设备管理补列头」是这份清单里唯一纯 UI、风险低、收益明确的项，留着待办。
