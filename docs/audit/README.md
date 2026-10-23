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
| `status-overlap.md` | 状态文字被控件遮挡的清单与边界阈值 | 逐文件读布局常量 + 矩形求交 |

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
