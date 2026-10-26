# 内核 Dock 挂载对照（Qt vs ARKLight）

真值来源：`Ksword5.1/Ksword5.1/KernelDock/KernelNavigationCatalog.h`（id / parent / 标题树）
+ `KernelDock/KernelDock.cpp:611-830`（`addTab` 实际调用顺序）。

ARKLight 侧：`KswordARKLight/Features/Kernel/KernelPageLayout.cpp`
的 `kOriginalTopLevelTabs` 与 `SecondaryTabsForPrimary()`。

## 1. Qt 内核 Dock 结构（12 个顶层分组）

| 顺序 | 路由 id | 标题 | 二级页 |
|---|---|---|---|
| 1 | `object` | 对象命名空间 | 总览 / 目录递归 / BaseNamedObjects / 符号链接 / 设备与驱动 / IoTimer / IOCTL 派遣表 / ObjectType 方法指针 / 对象类型 |
| 2 | `atom` | 原子表遍历 | — |
| 3 | `misc` | 杂项 | 历史NtQuery / 代码完整性 / VBS·HVCI / HVCI 绕过 / RXPF 模拟 / 知识中心 |
| 4 | `io` | I/O管理 | SSDT / ShadowSSDT / IDT / GDT / IOCTLS / DriverStartIo |
| 5 | `audit` | 内核审计与回调 | Inline Hook / IAT·EAT / 回调遍历 / 驱动回调 / 统一审计 / Shadow 比较 |
| 6 | `hal` | HAL | HalDispatchTable / HalPrivateDispatchTable / HalAcpiDispatchTable / HalSubComponents |
| 7 | `wdf` | WDF | WDF 函数 / WDF 回调 |
| 8 | `slat` | SLAT/IOMMU | EPT·NPT 交叉视图 / IOMMU·DMAR·IVRS |
| 9 | `timers` | 定时器/DPC | — |
| 10 | `workers` | 内核工作队列线程 | — |
| 11 | `cid` | CID表 | — |
| 12 | `ipc` | IPC | NamedPipe / ALPC / 通信对象 |

补充事实：
- Qt 的 `audit` 下 `audit.unified`（统一审计）与 `audit.inline`（Inline Hook）是**兄弟**，
  不存在两个重复的 Hook 顶层页。
- `SSDT` / `ShadowSSDT` **只**挂在 `io` 下，没有任何第二处挂载。
- `VT-x / EPT` 已经移出内核 Dock，comment 见 `KernelDock.cpp:717`：
  "VT-x/EPT 页已移到顶层「虚拟化」Dock"。
- `PDB Profile` / `驱动状态` / `DynData 总览` 属于 DriverDock（`selfDriverCatalog()`）。

## 2. ARKLight 重构前（18 个扁平顶层页，无层级）

```
动态偏移 / 对象命名空间 / IPC / 原子表遍历 / 历史NtQuery / SSDT遍历 / SSSDT解析 /
Inline Hook 审计 / IAT-EAT 钩子检测 / Hook审计 / 驱动回调 / 回调遍历 /
内核可执行内存 / CrossView / 定时器DPC / IOCTL 派遣表 / 内核修改审计 / 键盘
```

问题：

| 现象 | 说明 |
|---|---|
| 重复挂载点 | `Inline Hook 审计` 与 `Hook审计` 成了两个同级顶层页，Qt 里是父子关系 |
| 重复挂载点 | `SSDT遍历`/`SSSDT解析` 是顶层页，又被 `Hook审计` 组当子页挂了一遍 |
| 归属错误 | `命名管道`/`通信端点` 挂在对象命名空间下，Qt 归 IPC |
| 重复挂载点 | `动态偏移` 组重复挂了驱动 Dock 已有的 DynData / 驱动状态 / Capability |
| 结构偏差 | 18 个顶层页全平铺，Qt 是两层；顶层顺序也与 Qt 的 addTab 顺序无关 |

## 3. ARKLight 重构后（11 个 Qt 分组 + 3 个补充分组）

| 顺序 | 顶层标题 | primary feature | 二级页 |
|---|---|---|---|
| 1 | 对象命名空间 | ObjectNamespaceOverview | 总览 / 目录递归 / BaseNamedObjects / 符号链接 / 设备与驱动 / IOCTL 派遣表 / ObjectType 方法指针 / 对象类型 |
| 2 | 原子表遍历 | AtomTable | — |
| 3 | 杂项 | NtQueryLegacy | 历史 NtQuery / 内核可执行内存 / 内核内存证据 / 内核修改审计 / PiDDB 缓存 / 网络逐包捕获 |
| 4 | I/O管理 | Ssdt | SSDT / ShadowSSDT |
| 5 | 内核审计与回调 | HookAuditSummary | Inline Hook / IAT·EAT / 回调遍历 / 驱动回调 / 统一审计 / Minifilter 放行 PID |
| 6 | 平台审计 | PlatformAudit | — |
| 7 | SLAT/IOMMU | SlatIommuAudit | — |
| 8 | 定时器/DPC | KernelTimerDpc | — |
| 9 | 内核工作队列 | WorkQueueThreads | — |
| 10 | CID表 | CidTableSummary | CID 表 / 进程 CrossView / 线程 CrossView |
| 11 | IPC | IpcSummary | IPC 汇总 / 命名管道 / 通信端点 |
| 12 | PDB Profile | PdbProfileStatus | —（补充分组：Qt 在 DriverDock） |
| 13 | 虚拟化 | HvmStatus | VT-x·EPT 状态 / VM-exit 事件（补充分组：Qt 在虚拟化 Dock） |
| 14 | 键盘 | KeyboardHotkeys | 热键 / 钩子（补充分组：Qt 无对应内核页） |

## 4. 仍存在的实现缺口（非回归，改表前就缺）

Qt 有、ARKLight **没有实现**的页面：

- `object.iotimer` IoTimer
- `io.idt` / `io.gdt` / `io.ioctls` / `io.startio`
- `misc.integrity` 代码完整性、`misc.vbs` VBS·HVCI、`misc.hvci` HVCI 绕过、
  `misc.rxpf` RXPF 模拟、`misc.knowledge` 知识中心
- `hal.*` 4 页（ARKLight 只有单个 `PlatformAudit`）
- `wdf.*` 2 页
- `audit.shadow` Shadow 比较
- `ipc.alpc` ALPC

ARKLight 有、`KernelFeatureId` 已定义但**任何 Dock 都没挂**的页面（改表前后一致，
属于既有失联，需要补挂）：

| feature | 标题 | 建议归属 |
|---|---|---|
| `DriverDispatchTable` | 驱动派遣表 | 驱动 Dock |
| `DriverImageFields` | 驱动镜像字段 | 驱动 Dock |
| `DriverCommunication` | 驱动通信端点 | 驱动 Dock |
| `SystemTimeState` | 系统计时状态 | 硬件 Dock |
| `I8042Audit` | 键鼠端点审计 | 硬件 Dock |
| `RawDiskSectors` | 物理磁盘扇区 | 硬件 Dock |
