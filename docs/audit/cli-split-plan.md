# KswordCLI.cpp 拆分方案（8854 行 → 12 模块）

调研时间：2026-10-24 之后（基于 `dc282eb1`）
目标：满足项目规定「单个 .cpp 不超过 1000 行」，禁止 .inc/.inl 堆叠。

## 事实基线

| 项目 | 值 |
|---|---|
| 路径 | `KswordCLI/KswordCLI.cpp` |
| 行数 | 8854（代码 7624 / 注释 883 / 空行 347） |
| 对应 .h | **不存在**（只有 8 个 `R3*Shared.h` + `ArkDriverExtended.h` + `CommandRegistry.h`） |
| 匿名 namespace | 第 72-8813 行是**唯一一个**，内部 201 个顶层实体 |
| 匿名 namespace 之外 | 只有 `wmain`（8820-8854） |
| 对外导出符号 | **零**（除 wmain）。全仓库无其他 TU 引用 `dispatchCommand` / `commandProcessFamily` 等 |
| 最大单函数 | `commandMemoryFamily` 489 行；其次 `commandKernelFamily` 452 行 |
| .inc/.inl | 0 个（已符合规定） |

关键判断：8854 行**不是**一个巨型函数造成的，而是「28 个家族 × 平均 150 行 + 60 多个打印/构包子函数」。
每个家族已经把打印/构包下沉成 `printXxx`/`buildXxx`/`queryXxx`，家族本体只剩扁平 `if (sub == L"...")` 链。
**前人已经拆过一轮，本次是纯搬运，不需要重构函数体。**

## 3 个必须共享的可变全局（拆分的唯一硬性架构动作）

| 变量 | 定义行 | 写入 | 读取 | 说明 |
|---|---|---|---|---|
| `lastIoctlError` (DWORD) | 81 | 261 | 1451、1463、8829 | |
| `lastOpenError` (DWORD) | 82 | 225 | 1451、1462、8828 | |
| `responseFailureRc` (int) | 83 | 766、789 | 1451、8826 | |

`dispatchNested`（1443-1470）用 `RestoreErrors` 保存/恢复这 3 个值实现嵌套隔离。
**拆成多个 TU 后必须是同一实例**（`extern` + 单一定义），否则 `wmain` 退出码映射与嵌套隔离**静默失效——编译不报错**。

除这 3 个之外：无 mutex、无缓存、无线程局部、无 atomic。
`ensureHelpRegistry`（1474）的函数内 magic static 是唯一非幂等初始化点（C++11 线程安全）。
→ **不需要 context 对象**。只需要「具名命名空间 + 3 个 extern 全局 + 工具函数声明」的头。

## 12 模块方案（全部 < 1000 行）

| # | 文件名 | 职责 | 需搬运行区间 | 净行 |
|---|---|---|---|---|
| 1 | `CliCore.cpp/.h` | 常量、`IoctlResult`、`DriverHandle`、传输族、GUID/时间工具、3 个全局 | 1-70, 72-168, 185-209, 215-264, 751-803, 809-897, 903-1002, 1004-1040, 1049-1087, 1093-1125, 1641-1652, 1684-1724, 1730-1735, 1913-1930, 5258-5288 | 701 |
| 2 | `CliArgs.cpp/.h` | `NamedArgs` + 9 个选项读取 + 文本/数值工具 + 宽字符拷贝族 | 170-180, 270-455, 461-508, 514-607, 613-745, 1658-1678, 5166-5252 | 596 |
| 3 | `CliFrontEnd.cpp/.h` | 帮助元数据表、帮助渲染、`validateCommandOptions`、`ensureHelpRegistry`、`dispatchNested`、`dispatchCommand`、`wmain` | 1131-1149, 1151-1373, 1379-1441, 1443-1470, 1472-1551, 1557-1632, 8732-8854 | 612 |
| 4 | `CliProcess.cpp/.h` | process 家族 + 进程/线程详情、字段采样、注入构包 | 2084-2613, 2619-2901 | 813 |
| 5 | `CliMemoryFile.cpp/.h` | memory 家族、file 家族、卷/BitLocker/MountMgr/存储审计 | 2909-2929, 2935-3423, 3431-3655, 4986-5160 | 910 |
| 6 | `CliNetwork.cpp/.h` | network 家族、网络审计/端点/WFP/NDIS、AFD/NSI 降级 | 3661-4436, 7103-7190 | 864 |
| 7 | `CliKernel.cpp/.h` | kernel 家族、驱动卸载诊断、driver 家族、log 家族 | 1937-2077, 5309-5760, 7294-7498 | 798 |
| 8 | `CliCallbackDyn.cpp/.h` | callback/dyn/capability 家族 + DynData v4 查询 | 1758-1909, 5294-5303, 5766-5938, 5944-6130, 6136-6354 | 741 |
| 9 | `CliObjectFamilies.cpp/.h` | thread/handle/alpc/section/wsl 家族 | 6360-6472, 6478-6729 | 365 |
| 10 | `CliRegistry.cpp/.h` | registry/redirect/mutation + trust/safety/preflight | 6735-6886, 6892-7097, 7799-7952 | 512 |
| 11 | `CliDeviceMisc.cpp/.h` | win32k 查询、设备审计、安全视图、keyboard/hardware/hwid/window/misc | 4442-4845, 4851-4980, 7196-7288, 7504-7523, 7530-7793 | 911 |
| 12 | `CliDdmaObjectType.cpp/.h` | DDMA 自检 + R0 对象类型过程枚举 | 7964-8051, 8055-8393, 8399-8726 | 755 |

合计 8578 + 276 段间注释/空行 = 8854，无遗漏无重复。

头依赖方向：`CliCore.h` ← `CliArgs.h` ← `CliFrontEnd.h`；其余 9 个模块只依赖 Core + Args。
跨模块复用仅 2 处：`printModuleIdentity`（dyn + kernel）、驱动卸载诊断 3 函数（同模块内）。

## 拆分后可删除的「寄生虫」前置声明

- 185-209（25 行）整块前置声明 → 由 .h 取代
- 1737-1739（3 行）第二个前置声明块（`fixedAnsiWide`/`copyOptionalWideOption`/`printModuleIdentity` 因定义在 5166/5199/5294 才需要）
- 1443 `int dispatchCommand(...);`、1444 `bool isUnsupportedTransportError(DWORD);`
- `#pragma comment(lib, ...)` 只保留在 `CliNetwork.cpp`

## 实施硬约束（回归风险）

1. **匿名命名空间是最大的坑。** 未提升的符号会每个 TU 各生成一份且互不链接。必须先做「匿名 → `namespace ks::cli::legacy` + 头声明」的机械平移，尤其是 3 个全局变量。
2. **`kCommandHelps`（190 条，1182-1373）与 R3 层耦合，不能动。**
   `CommandRegistry.cpp:113` 的逻辑：`addCommand` 时若已存在同 path 且旧条目 `run` 为空，新条目被打上 `legacyDefault=true`；
   `dispatchR3`（`CommandRegistry.cpp:162-183`）对 `legacyDefault` 且**无 `--backend`** 时返回 `nullopt`，回落到 legacy handler。
   即 **`kCommandHelps` 的 190 条 = 「哪些命令默认走 legacy」的唯一声明**。
   若帮助表被裁、被拆散、或 `ensureHelpRegistry` 里 `addCommand` 注册顺序被打乱（必须先注册 legacy 帮助条目，再调所有 `register*`），
   默认行为会**静默切到 R3 实现，输出格式全变**。**最高优先级回归点。**
3. **`#ifndef KSWORD_CLI_LEGACY_FIXTURE`（1481）原样保留** —— 测试 fixture 用它编译「只注册 legacy 帮助、不注册 R3」模式。
4. **输出文本是契约**：`Test-KswordCLIReadOnly.ps1`（32KB）按文本断言。
   拆分必须零输出变化，包括 `std::wcout` 的 `std::hex`/`std::dec` 状态残留顺序（很多分支不重置 `std::dec`，是既有行为，**不要顺手修**）。
5. **`wmain` 留在瘦身后的 `KswordCLI.cpp`**（~40 行），避免工程入口名变化。
6. 工程文件：`KswordCLI.vcxproj` 加 12 个 ClCompile + 12 个 ClInclude；`.filters` 新增 `源文件\LegacyCli` 分组。
7. 注释语言混用（中文「处理：/返回：」与英文「Inputs/Processing/Returns」），按目标文件既有风格就近统一，**不要全局改写**（污染 diff）。

## 确实需要提子函数的 2 处（其余整体搬走）

- `commandMemoryFamily` 2935-3423（489 行）：13 个自包含分支，分支间只共享 `args`/`io`/`sub`，可机械提为 `memQueryVa(args, io)` 等静态函数。
- `commandKernelFamily` 5309-5760（452 行）：13 个分支，最大 `query-driver-object`（5431-5502，72 行）。
  注意 `force-unload-driver`（5545-5579）用到 1937-2044 的 3 个卸载诊断函数 → 必须与 kernel 家族同模块。
- `runDdmaSelfTest` 8055-8270（216 行）：线性流程无 switch，按「能力检查 / 目标盘选择 / 逐盘读校验」三阶段切。
- `commandR0ObjectTypeProcedures` 8506-8726（221 行）：一个 160 行分页循环（8533-8692），把单行渲染提为 `printObjectTypeProcedureRow`，布局判定+退出码提为 `finalizeObjectTypeProcedureScan`，8 个跨迭代状态收进 `ObjectTypeScanState` 局部 struct。

## 非 CLI 职责（后续下沉，本次不做）

| 位置 | 内容 | 应归属 |
|---|---|---|
| 4224-4361 | `GetExtendedTcpTable`/`GetExtendedUdpTable` 直接枚举 AFD 降级 | `shared/usermode/backend/network/`（已有 `Connections.cpp`） |
| 4367-4436 | `commandNetworkNsiFallback` | 网络后端 / 或删（现只打印 `status=degraded`） |
| 8533-8692 | 64 页分页 + 布局自校验 | `shared/usermode/backend/kernel/ObjectTypes.cpp` |
| 8055-8270 | DDMA 自检流程 | 独立诊断模块 |
| 2510-2613 | `buildDllInjectPayload` + `resolveLoadLibraryW` | 进程注入后端（`ProcessControls.cpp` 已有同类） |
| 3765-3812 / 3985-4168 | 网络审计响应头手写二进制布局校验 | 与 `shared/usermode/backend/system/IoctlDecoder.cpp` 重复 |
| 1182-1373 | 190 条帮助元数据 | 与 `CommandRegistry` 的 `Command` 元数据重复，长期合并 |

命名建议：新文件带 `Legacy` 前缀（`CliLegacyProcess.cpp` 等），因为 `R3*.cpp` 已用新架构实现同一批域，
将来 `--backend r3` 全面接管时可整组删除。
