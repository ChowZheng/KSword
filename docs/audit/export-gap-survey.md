# 导出能力缺口普查

范围：`KswordARKLight/` 全部功能页（`Features/` 21 个目录 + `App/`），只读排查，未改源码、未编译。

基线：`Ui/ExportUtil.h` / `Ui/ExportUtil.cpp` 已提供 6 个导出原语。本文回答三个问题：**谁在用**、**谁没用**、**谁自己又写了一遍**。

行号均为本次 `grep -n` 实测，命令附在 §7，可重跑复核。

---

## 0. 结论摘要

| 指标 | 数量 |
|---|---|
| 共享导出原语（`Ui/ExportUtil`） | 6 个函数 + 2 个类型（1 个 TSV 构建、1 个剪贴板、2 个存盘、2 个"选路径 / 写盘"拆分） |
| 走 `SaveUtf8TextFileWithDialog` 的文件导出入口 | **24 处调用点** |
| 走 `SaveBinaryFileWithDialog` 的二进制导出 | **1 处** |
| 走 `ChooseTextExportDestination` + `WriteUtf8TextToDestination` 的异步导出 | **1 处** |
| 完全手写 `GetSaveFileNameW` + 自己拼 TSV 的导出 | **3 处**（全在 `KernelPage.cpp`） |
| **文件导出入口合计** | **29 处** |
| 走共享 `Ui::CopyTextToClipboard` 的剪贴板点 | **18 处** |
| **手写 `OpenClipboard` 复制的点** | **22 处 / 21 个文件** |
| **有表格或详情文本、但没有任何文件导出的页面** | **13 个** + 1 处共享菜单 |
| **连行级复制都没有的表** | **3 个**（`FileView` 文件列表、`HotkeyKeyboard` 热键表与键盘布局表） |
| 手写 TSV / 文本拼接函数 | **39 个**（35 个 `XxxAsText` 形态 + `ExportText` + `CopyListCell/Row/All`） |
| `AppendTsvRow` 副本 | **5 份**（4 个同名函数 + 1 个 lambda） |
| `SanitizeTsvCell` 副本 | **2 份** |

三个最关键的事实：

1. **只有 `KernelPage` 能导出「全部行」**（`KernelPage.cpp:11091` `ExportAllRowsTsv`，菜单 `:9037` "导出全部 TSV"）。其余 28 处**一律只导出当前可见（筛选后）行或单条详情**，用户把筛选器一收就再也拿不到全量数据。而 `ExportUtil` 里根本没有「全量」这一档 API——这不是页面偷懒，是基建缺位。
2. **内核导出的编码和其它页面不一致**。`ExportUtil` 统一 UTF-8 + BOM（`ExportUtil.cpp:118` `ToUtf8WithBom`），但 `KernelPage.cpp:904` `WriteWholeFileText` 写的是 **UTF-16LE + BOM**。同一个产品导出的 `.tsv`，用 Excel 打开表现不同。
3. **`KernelPage` 的导出完全不进证据会话**。`grep GlobalEvidenceSession Features/Kernel/*.cpp` 零命中；而 `ExportUtil.cpp:180` / `:231` / `:287` 每次成功都会 `record()`。也就是说 SSDT / 内核钩子 / 驱动对象这些**最需要留证**的导出，在「证据会话」里查不到。

---

## 1. 基建现状：`Ui/ExportUtil`

### 1.1 对外符号（定义处）

| 符号 | 声明 | 定义 | 语义 |
|---|---|---|---|
| `SaveTextFileResult` | `ExportUtil.h:15` | — | `Saved` / `Cancelled` / `Failed` 三态，把「用户取消」和「写失败」分开 |
| `TextExportDestination` | `ExportUtil.h:23` | — | 纯 UI 线程产物（`MAX_PATH` 路径），可安全交给工作线程写盘 |
| `ChooseTextExportDestination` | `ExportUtil.h:26` | `ExportUtil.cpp:153` | 只弹保存框，不写盘 |
| `WriteUtf8TextToDestination` | `ExportUtil.h:30` | `ExportUtil.cpp:162` | 写盘 + 记证据，无对话框 |
| `BuildVisibleVirtualListTsv` | `ExportUtil.h:37` | `ExportUtil.cpp:186` | 表头 + **当前 `visibleIndexes()` 行**，转义后拼 TSV |
| `CopyTextToClipboard` | `ExportUtil.h:43` | `ExportUtil.cpp:203` | `CF_UNICODETEXT` + 记证据 |
| `SaveUtf8TextFileWithDialog` | `ExportUtil.h:51` | `ExportUtil.cpp:235` | `ChooseSavePath` + `WriteUtf8TextToDestination` 的同步组合 |
| `SaveBinaryFileWithDialog` | `ExportUtil.h:64` | `ExportUtil.cpp:259` | 原始字节，证据里只记描述不记 payload |

### 1.2 内部实现

| 符号 | 行 | 说明 |
|---|---|---|
| `SanitizeTsvCell` | `ExportUtil.cpp:19` | 把 `\t` `\r` `\n` 换成空格。**全项目另有 1 份同功能副本**（见 §5.3） |
| `AppendTsvRow` | `ExportUtil.cpp:28` | 全项目 5 份拼行实现之一（见 §5.2） |
| `WriteAll` | `ExportUtil.cpp:38` | 分块 `WriteFile` |
| `ChooseSavePath` | `ExportUtil.cpp:66` | `OPENFILENAMEW` + `OFN_OVERWRITEPROMPT \| OFN_PATHMUSTEXIST \| OFN_NOCHANGEDIR`；`GetSaveFileNameW` 在 `:85` |
| `WriteExportFile` | `ExportUtil.cpp:95` | `CreateFileW` + `FILE_FLAG_SEQUENTIAL_SCAN` |
| `ToUtf8WithBom` | `ExportUtil.cpp:118` | **UTF-8 + BOM**；空文本返回空字节（不产生只有 BOM 的空文件） |

### 1.3 基建缺口（结构性问题）

- **没有「全量」API**。只有 `BuildVisibleVirtualListTsv`，没有 `BuildVirtualListTsv(list, titles, scope)` 这种带 `scope = Visible | All` 的版本。→ 24 个导出点全部只能导可见行，是基建限制而非页面选择。
- **没有针对原生 ListView（`WC_LISTVIEW`，owner-data 或普通）的 TSV 构建器**。这类表格只能靠调用方自己 `ListView_GetItemText` 循环，导致 `ProcessDetailPage.cpp:1164`、`AuditTable.cpp:195`、`KernelPage.cpp:11118` 各写一遍。
- **没有「导出全量 + 导出可见」成对产出的便捷封装**，导致每个页面各自实现 `RowsAsText(state, bool visibleRows)` 这个布尔开关——全项目 20+ 份。
- **没有 `SaveUtf8TextFileWithDialog` 的异步版本**，只有 `MainWindow.cpp:1441` 自己把 `Choose*` / `Write*` 拆成两半跑后台。其余 24 处都是 UI 线程同步写盘。

---

## 2. 现有导出点全清单

### 2.1 走 `SaveUtf8TextFileWithDialog` 的文本文件导出（24 处）

格式列含义：**范围** = `可见`（跟随筛选）/ `全量`（忽略筛选）/ `详情`（单条记录）。

| # | 文件:行 | 用户入口 | 导出内容 | 范围 |
|---|---|---|---|---|
| 1 | `Features/Driver/DriverFeature.cpp:227` | 顶栏按钮"导出 TSV"（`:254`） | 按当前 tab 分派 `ExportDriver{Overview,Object,Unloaded,Thread}ViewTsv`（`:203/:207/:211/:213`） | 可见 |
| 2 | `Features/Handle/HandlePage.cpp:790` | 右键"导出当前可见句柄 TSV"（`:767`） | `VisibleHandleRowsAsTsv`（`:380`，12 列固定表头） | 可见 |
| 3 | `Features/HardwareStats/BusDeviceView.cpp:596` | 按钮"导出 TSV"（`:496`） | `ExportBusDeviceViewTsv`（`:746`） | 可见 |
| 4 | `Features/HardwareStats/DiskActivityView.cpp:460` | 按钮"导出 TSV"（`:335`） | `ExportDiskActivityViewTsv`（`:588`） | 可见 |
| 5 | `Features/HardwareStats/PerformanceView.cpp:467` | 按钮"导出 TSV"（`:344`） | `ExportPerformanceViewTsv`（`:597`） | 可见 |
| 6 | `Features/HardwareStats/UsbTopologyView.cpp:594` | 按钮"导出 TSV"（`:500`） | `ExportUsbTopologyViewTsv`（`:727`） | 可见 |
| 7 | `Features/Memory/DriverMemoryView.cpp:1119` | 菜单"导出快照报告 (.txt)"（`:1212`） | `BuildMemorySnapshotTextReport`（调用在 `:1121`）单条快照报告 | 详情 |
| 8 | `Features/Monitor/CallbackMonitorView.cpp:756` | 按钮"导出"（`:638`） | `ExportText`（`:587`）**表头硬编码** | 可见 |
| 9 | `Features/Monitor/EtwMonitorView.cpp:1044` | 按钮"导出..."（`:402`）+ 菜单"导出可见结果..."（`:1124`） | 可见事件行 | 可见 |
| 10 | `Features/NetTools/NetToolsConnectionView.cpp:393` | 按钮"导出 TSV"（`:620`） | `BuildVisibleVirtualListTsv`（`:386`） | 可见 |
| 11 | `Features/NetTools/NetToolsDiagnosticView.cpp:251` | 按钮"导出"（`:331`） | 诊断报告全文（纯文本，非表格） | 详情 |
| 12 | `Features/NetTools/NetToolsFirewallView.cpp:386` | 按钮"导出 TSV"（`:586`） | `BuildVisibleVirtualListTsv`（`:379`） | 可见 |
| 13 | `Features/Network/NetworkView.cpp:406` | 按钮"导出 TSV"（`:444`） | `BuildVisibleVirtualListTsv`（`:155`） | 可见 |
| 14 | `Features/Process/ProcessView.cpp:1218` | 菜单"导出可见结果为 TSV..."（`:2342`） | `VisibleRowsAsTsv`（`:1185`）→ `AuditCommon::BuildTsv`（`:1204`） | 可见 |
| 15 | `Features/Registry/RegistrySearchView.cpp:448` | 按钮"导出 TSV"（`:560`）+ 菜单"导出可见 TSV"（`:482`） | 可见搜索结果行 | 可见 |
| 16 | `Features/Registry/RegistryView.cpp:1098` | 菜单"导出当前可见值 TSV"（`:1144`） | `BuildVisibleVirtualListTsv`（`:1090`，4 列） | 可见 |
| 17 | `Features/Service/ServiceView.cpp:702` | 菜单"导出可见服务 TSV…"（`:838`） | `VisibleRowsAsTsv`（`:661`） | 可见 |
| 18 | `Features/Service/ServiceView.cpp:732` | 菜单"导出所选服务详情 TSV…"（`:839`） | `DetailAsText`（`:671`）单条服务属性 | 详情 |
| 19 | `Features/Window/WindowView.cpp:332` | 按钮"导出 TSV"（`:1857`） | 可见窗口行 | 可见 |
| 20 | `Features/Window/WindowView.cpp:483` | 菜单"导出当前详情为 TSV"（`:1481`/`:1681`） | 当前窗口 / Win32k 详情 | 详情 |
| 21 | `Features/WindowTools/WindowToolsCaptureView.cpp:282` | 按钮"导出 TSV"（`:474`） | `BuildVisibleVirtualListTsv`（`:274`） | 可见 |
| 22 | `Features/WindowTools/WindowToolsClipboardView.cpp:417` | 按钮"导出 TSV"（`:610`） | `BuildVisibleVirtualListTsv`（`:409`） | 可见 |
| 23 | `Features/WindowTools/WindowToolsHierarchyView.cpp:437` | 按钮"导出 TSV"（`:552`） | `BuildVisibleVirtualListTsv`（`:429`） | 可见 |
| 24 | `Features/WindowTools/WindowToolsHotkeyView.cpp:286` | 按钮"导出 TSV"（`:375`） | `BuildVisibleVirtualListTsv`（`:278`） | 可见 |

> 统计：**20 处「可见」**（含 #8 表头写死）、**4 处「详情」**（#7、#11、#18、#20）、**0 处「全量」**。

### 2.2 二进制导出（1 处）

| 文件:行 | 入口 | 内容 |
|---|---|---|
| `Features/Memory/DriverMemoryView.cpp:1145` | 菜单"导出原始字节 (.bin)"（`:1213`） | `snapshot->bytes`，证据里只记 `BuildSnapshotMetadataText`（`:1102`）不记 payload |

### 2.3 异步导出（1 处，范式参考）

| 文件:行 | 入口 | 实现 |
|---|---|---|
| `App/MainWindow.cpp:1441` | 证据会话菜单 4 项（`:665`–`:668`） | UI 线程 `ChooseTextExportDestination`（`:1441`）→ 后台 `WriteUtf8TextToDestination`（`:1469`）→ 回 UI 报状态（`:1473`–`:1485`）。**这是唯一一个把对话框与磁盘 I/O 分离的实现**，应作为其它页面的模板 |

### 2.4 完全手写的导出（`GetSaveFileNameW` + 自拼 TSV，3 处）

| 文件:行 | 入口 | 内容 | 问题 |
|---|---|---|---|
| `Features/Kernel/KernelPage.cpp:8224` | 按钮"导出配置"（`:2371`） | `SerializeCallbackLocalConfig()`（`:8227`） | 不记证据；不走 `ExportUtil` |
| `Features/Kernel/KernelPage.cpp:8247` | 按钮"导出"（`:2453`）/ 菜单（`:8435`） | `callbackFileMonitorList_` 全部行（`:8251`–`:8263`） | **无表头行**；不记证据；编码 UTF-16 |
| `Features/Kernel/KernelPage.cpp:11111` | 菜单"导出全部 TSV"（`:9037`/`:9276`/`:9332`/`:9457`/`:9506`） | `resultList_` 全部行（`:11127`–`:11139`，`VisibleCellText` + `CurrentCopyColumnIndices`） | 不记证据；编码 UTF-16；`ExportAllRowsTsv` 名字叫「全部」但过滤后列表里已无被筛掉的行 |

写入路径：`KernelPage.cpp:8034` `StartCallbackFileIo` → `WriteWholeFileText`（`:904`，**UTF-16LE + BOM**，`:916`–`:918`）。

> `Features/File/FileActions.cpp:296` 也调了 `GetSaveFileNameW`，但那是**重命名对话框**（`PromptRenameTarget`，`:281`），不是数据导出，**不计入导出点**。

### 2.5 剪贴板导出

#### 2.5.1 走共享 `Ui::CopyTextToClipboard`（18 处）

| 文件:行 | 性质 |
|---|---|
| `Features/AuditCommon/AuditFormatting.cpp:98` | 薄封装 `CopyTextToClipboard` |
| `Features/Driver/DriverActions.cpp:157` | 薄封装（`DriverActions::CopyTextToClipboard`） |
| `Features/Handle/HandlePage.cpp:345` | 薄封装 |
| `Features/Memory/DriverMemoryView.cpp:370` | 薄封装 |
| `Features/Monitor/EtwMonitorView.cpp:191` | 薄封装 |
| `Features/NetTools/NetToolsConnectionView.cpp:129` | 薄封装 |
| `Features/Network/NetworkView.cpp:138` | 薄封装 |
| `Features/Process/ProcessView.cpp:1656` | 薄封装 |
| `Features/ProcessDetail/ProcessDetailPage.cpp:1116` | 薄封装（`ProcessDetailPage::CopyText`） |
| `Features/ProcessDetail/ProcessDetailPage.EvidencePeb.cpp:127` | 薄封装 |
| `Features/ProcessDetail/ProcessDetailPage.Module.cpp:148` | 薄封装 |
| `Features/Service/ServiceView.cpp:148` | 薄封装 |
| `Features/Window/WindowView.cpp:502` | 薄封装 |
| `Features/Driver/DriverThreadView.cpp:395` | 直接调用 |
| `Features/SysTools/IoctlDecoderView.cpp:163` | 直接调用 |
| `Features/Registry/RegistrySearchView.cpp:421` | 直接调用（复制单元格） |
| `Features/Registry/RegistrySearchView.cpp:429` | 直接调用（复制行） |
| `Features/Registry/RegistrySearchView.cpp:436` | 直接调用（复制可见结果） |

#### 2.5.2 手写 `OpenClipboard` + `SetClipboardData`（22 处）

全部**不记证据会话**，且与 §1.2 的 `CopyTextToClipboard` 行为不等价。

| # | 文件:行 | 所在函数 |
|---|---|---|
| 1 | `Features/Driver/DriverUnloadedView.cpp:250` | `CopyText`（`:249`） |
| 2 | `Features/File/FileActions.cpp:873` | `FileActions::copyTextToClipboard` |
| 3 | `Features/Hardware/HardwareHwidDispatchView.cpp:163` | `WriteClipboardText` |
| 4 | `Features/Hardware/HardwareView.cpp:133` | `WriteClipboardText`（`:132`） |
| 5 | `Features/HardwareStats/BusDeviceView.cpp:116` | `WriteClipboardText` |
| 6 | `Features/HardwareStats/DiskActivityView.cpp:80` | `WriteClipboardText` |
| 7 | `Features/HardwareStats/PerformanceView.cpp:81` | `WriteClipboardText` |
| 8 | `Features/HardwareStats/UsbTopologyView.cpp:113` | `WriteClipboardText` |
| 9 | `Features/Kernel/KernelPage.cpp:420` | 复制单元格 |
| 10 | `Features/Kernel/KernelPage.cpp:445` | 复制行/整表 |
| 11 | `Features/Memory/DriverMemoryView.cpp:377` | `CopyTextToClipboard` 本地版 |
| 12 | `Features/Misc/MiscFeature.cpp:567` | `WriteClipboardText`（`:566`） |
| 13 | `Features/NetTools/NetToolsDiagnosticView.cpp:89` | `WriteClipboardText` |
| 14 | `Features/NetTools/NetToolsFirewallView.cpp:118` | `WriteClipboardText` |
| 15 | `Features/Privilege/PrivilegeView.cpp:121` | `CopyText`（`:120`） |
| 16 | `Features/Process/ProcessActions.cpp:81` | `WriteClipboardText` |
| 17 | `Features/Registry/RegistryActions.cpp:369` | `WriteClipboardText` |
| 18 | `Features/Startup/StartupView.cpp:136` | `CopyText`（`:135`） |
| 19 | `Features/SysTools/ContextMenuView.cpp:98` | `CopyText`（`:97`） |
| 20 | `Features/SysTools/EventLogView.cpp:94` | `CopyText`（`:93`） |
| 21 | `Features/SysTools/FileHolderView.cpp:96` | `CopyText`（`:95`） |
| 22 | `Features/SysTools/SystemTimeView.cpp:231` | `CopyText` |

> `Features/WindowTools/WindowToolsClipboardView.cpp` 里的 `OpenClipboard` 是**读**剪贴板（该页功能本身），不是导出，已排除。

---

## 3. 缺口清单

判定口径：**「有表格或详情文本」+「没有任何文件导出入口」**才计入。只有剪贴板算「部分缺口」，单独标注。

### 3.1 完全没有文件导出，且有复制（部分缺口）

| # | 表格/详情创建点 | 数据来源 | 现有能力 | 建议形态 | 优先级 |
|---|---|---|---|---|---|
| G1 | `Features/Hardware/HardwareView.cpp:728` 设备树（`LazyTreeView`）+ `:716` 属性详情表 | `HardwareModel` | 只复制实例 ID（`:647`）；详情表复制三档（`DetailRowsAsText` `:169`，入口 `:675`–`:678`） | 树 → 全量 TSV（设备树天然全量）；详情表 → 可见 TSV | P2 |
| G2 | `Features/Privilege/PrivilegeView.cpp:514` 特权列表 + `:529` 特权详情 | `PrivilegeModel` | 复制行/可见行/详情（`RowsAsText` `:388`、`DetailAsText` `:414`，入口 `:435`–`:437`） | 列表 → 可见 TSV；详情 → 详情 TSV | P2 |
| G3 | `Features/Startup/StartupView.cpp:728` 启动项列表 + `:724` 启动项详情 | `StartupLightEnumerator` | 复制单元格/行/可见行/详情（`SelectedRowsAsText` `:455`，入口 `:489`/`:503`/`:684`/`:688`） | 列表 → 可见 TSV；详情 → 详情 TSV | **P1**（自启动项是持久化证据） |
| G4 | `Features/SysTools/ContextMenuView.cpp:610` 右键菜单项表 | Shell 扩展枚举 | 复制选中行/可见行（`RowsAsText` `:408`，入口 `:523`–`:524`） | 可见 TSV | P2 |
| G5 | `Features/SysTools/EventLogView.cpp:529` 事件日志表 | 事件日志 | 复制行/可见行/完整描述（`RowsAsText` `:352`，入口 `:409`–`:411`） | 可见 TSV + 全量 TSV | **P1**（日志天然是证据） |
| G6 | `Features/SysTools/FileHolderView.cpp:502` 文件占用表 | Restart Manager | 复制行/可见行（`RowsAsText` `:317`，入口 `:410`–`:411`） | 可见 TSV | **P1**（"谁占着这个文件"是排障输出） |
| G7 | `Features/Misc/MiscFeature.cpp:829` 杂项审计表 | `MiscAuditRow` | 复制单元格/行/可见结果（`BuildRowsTsv` `:542`，入口 `:786`–`:788`） | 可见 TSV | P3 |
| G8 | `Features/ProcessDetail/ProcessDetailPage.cpp:1218` 通用表格右键菜单（覆盖热键/键盘/令牌等所有 `listColumnCounts_` 表） | 各 tab | 复制单元格/行/全部（`CopyListCell` `:1147`、`CopyListRow` `:1150`、`CopyListAll` `:1164`） | 全部表格统一挂"导出为 TSV…" | **P1**（进程详情是核心取证面） |
| G9 | `Features/ProcessDetail/ProcessDetailPage.Module.cpp:656` 模块虚表 | `ProcessDetailCollector` | 复制单元格/行（`:868`–`:869`） | 可见 TSV | **P1**（DLL 侧加载） |
| G10 | `Features/ProcessDetail/ProcessDetailPage.Thread.cpp:420` 线程虚表 | 同上 | 复制单元格/行/全部 + 线程详情（`:624`–`:628`） | 可见 TSV | **P1** |
| G11 | `Features/ProcessDetail/ProcessDetailPage.Token.cpp:214` 令牌详情文本 | Token IOCTL | 复制文本 | 纯文本详情 | **P1**（权限提升取证） |
| G12 | `Features/ProcessDetail/ProcessDetailPage.EvidencePeb.cpp:146` PEB 证据面板 | PEB 读 | 复制（`:127`） | 纯文本详情 | **P1** |
| G13 | `Features/ProcessDetail/ProcessDetailPage.DetailPlugin.cpp:184`/`:205` 路径与命令行 | 进程详情 | 复制文本 | 纯文本详情 | P2 |
| G14 | `Features/Driver/DriverDebugOutputView.cpp:577` 调试输出表右键菜单 | 驱动 `DbgPrint` 流 | 复制单元格/行/可见结果（`:585`–`:589`，`BuildVisibleTsv` `:234`） | 可见 TSV；顶栏 `DriverFeature.cpp:217` 明确拒绝（"当前页不支持顶栏 TSV 导出"） | **P1**（2000 行环形缓冲 `:46`，不导就丢） |
| G15 | `Features/AuditCommon/AuditTable.cpp:221` **共享**右键菜单（`ShowAuditTableContextMenu`） | 所有 `AuditTable` 使用者 | 复制单元格/整行/全部 TSV（`:251`–`:255`） | 菜单加一项"导出为 TSV…"，一次覆盖 `HardwareHwidDispatchView.cpp:711`/`:725`、`FileFeature.cpp:1333`、`ProcessView`、`ServiceView` | **P1**（一处改动，多点受益） |

### 3.2 完全没有复制、也没有导出的表（最严重的缺口）

| # | 创建点 | 数据来源 | 现状 |
|---|---|---|---|
| G16 | `Features/File/FileView.cpp:284` **文件浏览主表**（`WC_LISTVIEWW` + owner-data `:483`） | `FileSystemEnumerator` | 只有按路径的 CopyPath / CopyKernelModePath 等动作（`FileActions.h:17`–`:19`）；**整表既不能复制也不能导出**。上下文菜单只有列显隐（`:400`）与文件动作（`:745`） |
| G17 | `Features/ProcessDetail/ProcessDetailPage.HotkeyKeyboard.cpp:422` 热键注册表 | 注册热键枚举 | 无复制、无导出、无菜单（该文件 `AppendMenuW` 零命中） |
| G18 | `Features/ProcessDetail/ProcessDetailPage.HotkeyKeyboard.cpp:448` 键盘布局表 | 同上 | 同上 |

### 3.3 纯文本报告页：只有复制按钮，没有导出

| # | 文件:行 | 内容 | 现有 | 建议 |
|---|---|---|---|---|
| G19 | `Features/SysTools/SystemTimeView.cpp:339`（按钮）/ `:469`（复制） | `state->reportText` 系统时间报告 | 只有"复制报告"按钮 | 纯文本详情导出（`.txt`），P2 |
| G20 | `Features/SysTools/IoctlDecoderView.cpp:117`（按钮）/ `:163`（复制） | `state->reportText` IOCTL 解码报告 | 只有"复制结果"按钮 | 纯文本详情导出（`.txt`），P2 |

### 3.4 全局性缺口（不是某个页面）

| # | 缺口 | 证据 |
|---|---|---|
| G21 | **没有「导出全量（忽略筛选）」** | §2.1 的 24 处全部是可见行；`ExportUtil.h:37` 注释明确写了「deliberately follows the active filter rather than exporting hidden rows」。用户筛完想看全量，只能清空筛选重跑一次采集——对实时数据（ETW、回调监控、磁盘活动）等于**永远拿不到** |
| G22 | **内核导出的编码与全项目不一致** | `KernelPage.cpp:904`/`:916` UTF-16LE vs `ExportUtil.cpp:118` UTF-8+BOM |
| G23 | **内核导出不进证据会话** | `grep -rn "GlobalEvidenceSession\|EvidenceSession" Features/Kernel/*.cpp` → 0 命中 |
| G24 | **文件监控事件导出的 TSV 无表头** | `KernelPage.cpp:8253`–`:8263` 直接从 row 0 写数据，没有 header 行 |
| G25 | **`EtwFilterDialog.cpp:141` 供应商勾选表无导出** | 预设表，几十行，主观判定为**不需要**（仅记录，不建议补） |

---

## 4. 优先级排序（前 8）

排序依据两条：① **数据难以重建**（一次性、随时间流逝、进程退出即消失）；② **用户会不会拿它当证据**离开这个程序。

| 名次 | 缺口 | 为什么排这里 |
|---|---|---|
| 1 | **G15 共享审计表菜单加文件导出**（`AuditTable.cpp:221`） | 不是"一个页面"，是**一处改动覆盖全部 `AuditTable` 使用者**。这些页面（HWID 状态、文件审计、进程、服务）已经有"复制全部 TSV"，说明数据早就成形，只差一个落盘入口。投入产出比最高 |
| 2 | **G21 全量导出基建**（`ExportUtil` 补 `scope` 参数） | 24 个页面同时受益；对实时流（ETW / 回调 / 磁盘活动 / 调试输出）这是**唯一**能拿到完整数据集的方式，且部分数据过了就没了 |
| 3 | **G14 驱动调试输出导出**（`DriverDebugOutputView.cpp:577`） | 2000 行环形缓冲（`:46`），满了自动丢；且是驱动 `DbgPrint`，**无法重放**。现在顶栏还说"不支持"（`DriverFeature.cpp:217`） |
| 4 | **G16 文件浏览表复制/导出**（`FileView.cpp:284`） | 唯一一个连复制都没有的主数据表；文件列表是"这个目录里当时有什么"的现场快照，重建依赖文件系统状态 |
| 5 | **G8/G9/G10 进程详情模块表与线程表导出** | DLL 侧加载与线程归属是核心取证结论；`ProcessDetail` 有 4 个表格和 5 处详情文本，**一个导出都没有**，是覆盖最差的整块功能 |
| 6 | **G11/G12 令牌与 PEB 详情导出** | 提权与注入取证的关键单点证据；现在只能剪贴板，用户粘到别处就散了 |
| 7 | **G5/G6 事件日志与文件占用导出** | 两者都是"事后不可重现"（日志滚动、占用随时释放）；且代码里 TSV 构建器已存在，只差落盘 |
| 8 | **G3 启动项导出** | 自启动项是持久化证据，用户会想"存一份现在的启动项列表去对比"；`SelectedRowsAsText` 已就绪 |

> 明确排在最后的：G1（硬件树）、G2（特权）、G4（右键菜单项）、G7（杂项）、G13（路径/命令行）、G19/G20（两处报告）——都是**随时可重新枚举**的配置类数据。

---

## 5. 重复实现清理清单（重点）

用户要的是「各处都加上」，但**不能变成各处各写一套**。下面是现有重复的完整账。

### 5.1 剪贴板写入：22 份 → 1 份

见 §2.5.2 全表。22 处手写 `OpenClipboard` / `GlobalAlloc` / `SetClipboardData` / `CloseClipboard` 四步曲，与 `ExportUtil.cpp:203` 相比**少了证据会话记录**（`ExportUtil.cpp:231`）。全部改为 `Ksword::Ui::CopyTextToClipboard(owner, text, L"<来源名>")`。

其中 8 处是名字几乎一样的薄封装（`CopyText` / `WriteClipboardText`），可以一并删除：

`DriverUnloadedView.cpp:249`、`FileActions.cpp:872`、`HardwareView.cpp:132`、`MiscFeature.cpp:566`、`PrivilegeView.cpp:120`、`StartupView.cpp:135`、`ContextMenuView.cpp:97`、`EventLogView.cpp:93`、`FileHolderView.cpp:95`、`SystemTimeView.cpp:230`。

### 5.2 拼 TSV 行：5 份 → 1 份

| 副本 | 差异 |
|---|---|
| `Ui/ExportUtil.cpp:28` | 调 `SanitizeTsvCell`，**唯一转义过的** |
| `Features/AuditCommon/AuditFormatting.cpp:78`（`appendRow` lambda） | 调本文件的 `SanitizeTsvCell`，转义过 |
| `Features/Driver/DriverActions.cpp:19` | 不转义 |
| `Features/Handle/HandlePage.cpp:348` | 不转义 |
| `Features/Window/WindowView.cpp:282` | 不转义，多一个 `columnCount` 截断参数 |

→ 保留 `Ui::ExportUtil` 一份，导出成公开的 `AppendTsvRow(std::wstring&, const std::vector<std::wstring>&)`。

### 5.3 `SanitizeTsvCell`：2 份 → 1 份

`Ui/ExportUtil.cpp:19`（按值传参）与 `Features/AuditCommon/AuditFormatting.cpp:64`（按 const 引用传参），**函数体和语义完全相同**。`AuditFormatting.h:34` 还把它导出了。合并后 `AuditFormatting.h` 的声明删掉即可。

### 5.4 「可见 / 选中 / 全部」TSV 构建：39 份 → 2 个 API

这是最大的一块。其中 35 个是 `XxxAsText(state, bool)` 形态，布尔含义**每个文件都不一样**：

| 函数 | 文件:行 | 布尔语义 | 带表头 |
|---|---|---|---|
| `RowsAsTsv` | `HandlePage.cpp:360` | `allVisible`（true=全部可见，false=选中） | 否 |
| `VisibleHandleRowsAsTsv` | `HandlePage.cpp:380` | 无参数，恒为可见 | 是（表头写死 `:381`） |
| `RowsAsText` | `DriverUnloadedView.cpp:277` | `visibleRows` | 是 |
| `BuildVisibleTsv` | `DriverDebugOutputView.cpp:234` | **`selectedOnly`（true=仅选中，与其它相反）** | 否 |
| `DetailRowsAsText` | `HardwareView.cpp:169` | `allRows`（原生 ListView 遍历） | 否 |
| `RowsAsText` | `BusDeviceView.cpp:205` | `visibleRows` | 是 |
| `DetailAsText` | `BusDeviceView.cpp:231` | — | 是 |
| `RowsAsText` | `DiskActivityView.cpp:117` | `visibleRows` | 是 |
| `RowsAsText` | `PerformanceView.cpp:118` | `visibleRows` | 是 |
| `RowsAsText` | `UsbTopologyView.cpp:207` | `visibleRows` | 是 |
| `DetailAsText` | `UsbTopologyView.cpp:233` | — | 是 |
| `BuildRowsTsv` | `MiscFeature.cpp:542` | 只接受行数组 | 是（表头另写） |
| `RowsAsText` | `NetToolsConnectionView.cpp:479` | `visibleRows` | 是 |
| `DetailAsText` | `NetToolsConnectionView.cpp:505` | — | 是 |
| `RowsAsText` | `NetToolsFirewallView.cpp:402` | `visibleRows` | 是 |
| `DetailAsText` | `NetToolsFirewallView.cpp:428` | — | 是 |
| `SelectedRowsText` | `NetworkView.cpp:330` | `allVisible` | 否（可见集走 `BuildVisibleVirtualListTsv` `:155`） |
| `RowsAsText` | `PrivilegeView.cpp:388` | `visibleRows` | 是 |
| `DetailAsText` | `PrivilegeView.cpp:414` | — | 是 |
| `SelectedRowsAsText` | `ProcessView.cpp:1140` | `allColumns`（**不是行范围，是列范围**） | 否 |
| `VisibleRowsAsText` | `ProcessView.cpp:1163` | 无参数 | 否 |
| `VisibleRowsAsTsv` | `ProcessView.cpp:1185` | 无参数 | 是（`AuditCommon::BuildTsv`） |
| `RegistryRowsAsText` | `RegistryView.cpp:1041` | `allVisible` | 否 |
| `RowsAsText` | `ServiceView.cpp:636` | `visibleRows` | 否 |
| `VisibleRowsAsTsv` | `ServiceView.cpp:661` | 无参数 | **用字符串拼接补表头**（`:666`–`:668`） |
| `DetailAsText` | `ServiceView.cpp:671` | — | 是 |
| `SelectedRowsAsText` | `StartupView.cpp:455` | `visibleRows` | 否 |
| `RowsAsText` | `ContextMenuView.cpp:408` | `allVisible` | 是 |
| `RowsAsText` | `EventLogView.cpp:352` | `allVisible` | 是 |
| `RowsAsText` | `FileHolderView.cpp:317` | `allVisible` | 是 |
| `RowsAsTsv` | `WindowToolsCommon.cpp:80` | `visibleRows` + `columnCount` | 否 |
| `ReportText` | `WindowToolsHierarchyView.cpp:417` | 诊断报告 | 是 |
| `BuildAuditTableTsv` | `AuditTable.cpp:195` | 读原生 ListView 全部行 | 是 |
| `CopyListCell` / `CopyListRow` / `CopyListAll` | `ProcessDetailPage.cpp:1147` / `:1150` / `:1164`（注释头 `:1164`–`:1191`） | 原生 ListView，单元格 / 单行 / 全部 | 仅 `CopyListAll` 带表头（`:1171`–`:1181`） |
| `ExportText` | `CallbackMonitorView.cpp:587` | 无参数 | **表头硬编码 `:588`** |
| `BuildTsv` | `AuditFormatting.cpp:74` | `(headers, rows)` | 是 |

建议在 `Ui/ExportUtil` 收敛为两个 API：

```
enum class RowScope { Visible, All, Selected };
std::wstring BuildVirtualListTsv(const std::vector<std::wstring>& titles,
                                 const VirtualListView& list, RowScope scope);
std::wstring BuildNativeListTsv(HWND listView, RowScope scope);   // 读表头 + 单元格
```

调用方一律删掉本地 `RowsAsText(state, bool)`，改传 `RowScope`。**这一条同时解决 §3.4 的 G21（全量导出）**——`RowScope::All` 就是现在缺的那一档。

### 5.5 保存对话框手写：3 份 → 1 份

`KernelPage.cpp:8224` / `:8247` / `:11111` 各自拼了一遍 `OPENFILENAMEW`（`lStructSize` / `lpstrFilter` / `nMaxFile` / `Flags` / `lpstrDefExt`）。统一改走 `ChooseTextExportDestination`（`ExportUtil.cpp:153`）+ `WriteUtf8TextToDestination`（`:162`），或直接用 `SaveUtf8TextFileWithDialog`（`:235`）。

### 5.6 编码不一致：2 套 → 1 套

`KernelPage.cpp:904`–`:918` 写 UTF-16LE + BOM；`ExportUtil.cpp:118` 写 UTF-8 + BOM。统一到 UTF-8（`ExportUtil` 方向），否则用户拿到的 `.tsv` 有一半 Excel 会识别成乱码。

### 5.7 表头与列定义脱节

`CallbackMonitorView.cpp:588` 把表头写死成 `L"序号\t时间\t类别\t操作\t来源PID\t目标PID\t结果\t路径"`，而同一文件 `:672` 的列定义是 `序号 / 时间 / 类别 / 操作 / 来源 PID / 目标 PID / 结果 / 路径`。**导出的表头与界面上看到的表头不一致**（`来源PID` vs `来源 PID`，`目标PID` vs `目标 PID`）。改成从 `ListView_GetHeader` 读，或复用 `:672` 的同一个数组。

同类问题：`HandlePage.cpp:381`（12 列手抄一份）、`ServiceView.cpp:666`–`:668`（`BuildTsv({headers}, {}) + rows` 先拼表头再拼数据，两个 `\r\n` 语义容易错位）。

### 5.8 薄封装 `CopyXxxText`：8 个 → 0 个

`DriverActions::CopyTextToClipboard`（`DriverActions.cpp:156`）、`AuditFormatting.cpp:97`、`HandlePage.cpp:344`、`DriverMemoryView.cpp:369`、`EtwMonitorView.cpp:190`、`NetToolsConnectionView.cpp:129`、`NetworkView.cpp:137`、`ServiceView.cpp:148`、`WindowView.cpp:502`、`ProcessDetailPage::CopyText`（`ProcessDetailPage.cpp:1113`）——全部只是给 `Ui::CopyTextToClipboard` 补一个固定的 `source` 字符串。调用点直接传字符串即可。

### 5.9 前向声明重复

`DriverObjectView.cpp:393` 与 `:551` 对 `BuildObjectTsv` 声明了两次；`DriverOverviewView.cpp:440` 与 `:531` 对 `BuildOverviewTsv` 声明了两次。清理时可一并去掉。

---

## 6. 建议的落地顺序（不含实施）

1. **扩基建**：`Ui/ExportUtil` 增加 `RowScope` + `BuildVirtualListTsv` / `BuildNativeListTsv`，并把 `AppendTsvRow` / `SanitizeTsvCell` 收成唯一实现（§5.2、§5.3、§5.4）。这一步不做，后面每加一个页面就多一份重复。
2. **改共享菜单**：`AuditTable.cpp:221` 加"导出为 TSV…"（G15），顺手把 `CopyListAll` 那套原生 ListView 遍历换成 `BuildNativeListTsv`。
3. **改内核**：`KernelPage` 三处手写导出切到 `ExportUtil`，统一 UTF-8，补证据会话记录，补文件监控表头（G22–G24）。**注意 `KernelPage.cpp` 正在做火焰图采集，改之前先确认窗口期。**
4. **补高优页面**：G14（调试输出）→ G16（文件浏览）→ G8/G9/G10（进程详情）→ G11/G12（令牌/PEB）。
5. **批量清理**：22 处手写剪贴板一次性替换（§5.1），纯机械改动，可独立成一个提交。
6. **补剩余页面**：G3、G5、G6，再 G1、G2、G4、G7、G13、G19、G20。

---

## 7. 复核方式

```bash
cd KswordARKLight

# 共享原语定义
grep -rn "SaveUtf8TextFileWithDialog\|SaveBinaryFileWithDialog\|BuildVisibleVirtualListTsv\|\
CopyTextToClipboard\|ChooseTextExportDestination\|WriteUtf8TextToDestination" --include=*.cpp --include=*.h .

# 24 个 SaveUtf8 调用点（排除定义/声明/注释）
grep -rn "Ksword::Ui::SaveUtf8TextFileWithDialog(" --include=*.cpp Features App | wc -l   # → 24

# 22 处手写剪贴板
grep -rn "OpenClipboard" --include=*.cpp --include=*.h . | grep -v Ui/ExportUtil

# 5 份拼行 / 2 份转义
grep -rn "void AppendTsvRow\|appendRow = \[" --include=*.cpp .                     # → 5
grep -rn "std::wstring SanitizeTsvCell" --include=*.cpp .                          # → 2

# 有表格但无 SaveUtf8
grep -rl "WC_LISTVIEW\|VirtualListView" --include=*.cpp Features App | sort > /tmp/t.txt
grep -rl "SaveUtf8TextFileWithDialog" --include=*.cpp Features App | sort > /tmp/s.txt
comm -23 /tmp/t.txt /tmp/s.txt     # → 19 个文件

# 内核不进证据会话
grep -rn "GlobalEvidenceSession\|EvidenceSession" Features/Kernel/*.cpp    # → 0
```

**计数口径说明**：`comm -23` 得到 19 个「有表格但没走 `SaveUtf8`」的文件。扣除 `KernelPage.cpp`（有手写导出，归 §2.4）、`WindowToolsCommon.cpp`（共享工具，非页面）、`EtwFilterDialog.cpp`（预设对话框，判定不需导出）、以及 4 个通过 `DriverFeature.cpp:227` 顶栏按钮间接导出的 Driver 视图（`DriverObjectView` / `DriverOverviewView` / `DriverThreadView` / `DriverUnloadedView`），得 **12 个文件**；再加两个不以 `WC_LISTVIEW` / `VirtualListView` 字面量建表、但持有纯文本报告的 `SysTools/SystemTimeView.cpp`、`SysTools/IoctlDecoderView.cpp`，合计 **13 个页面**（`ProcessDetailPage` 的 6 个 tab 文件按 1 个页面计）。另有 **1 处共享菜单** `AuditCommon/AuditTable.cpp:221` 没有文件导出，修一处即覆盖 `HardwareHwidDispatchView.cpp:711`/`:725` 等全部调用方。

---

## 8. 已知的文档偏差

`docs/audit/list-virtualization-survey.md` §2 对 `File/FileView.cpp:285` 的描述是"导出走 `state.entries`，不受可见集影响"。**该描述与代码不符**：`FileView.cpp` 全文 `Export` / `SaveUtf8` / `OpenClipboard` 零命中，该表既无复制也无导出（见 G16）。推测是早期规划遗留。建议修正该文档，避免后续按它做设计。
