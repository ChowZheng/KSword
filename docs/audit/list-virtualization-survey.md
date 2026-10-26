# 列表虚拟化普查

范围：`KswordARKLight/` 下全部 ListView，共 58 个创建点（其中 `KernelPage.cpp:2908` 在循环里创建 7 个规则表，实际控件约 64 个）。只读排查，未改源码。

状态：初版普查后代码有连续几笔相关提交，行号按 `dc282eb1` 重新校准。影响本文结论的有两处——①`setRows(*ptr)` 的 18 处深拷贝已修完（`45ca93b0` 17 处 + `969604fc` 1 处）；②**原本列为风险的证据会话列表已由 `dc282eb1` 改造成 owner-data + 后台物化**，§37 因此从「风险」改为「已修复的参考范式」。**§38 仍未修，是当前唯一一处风险**。

## 总览

- 走 `VirtualListView` 封装的列表：**31 个**（29 个文件，`ProcessDetailPage` 独占 2 个）
- 手写 `LVS_OWNERDATA`（未用封装但本身已虚拟化）：**6 个**
- 仍是全量 `ListView_InsertItem` 的列表：**21 个控件创建点**
  - 其中「主数据表」性质：4 个（1 个风险 + 3 个有上限/固定行数）
  - 其中「属性详情面板」性质：10 个（每行一个键值对，天然小）
  - 其中 KernelPage 辅助表：5 个创建点（11 个控件）
  - 其中 HandlePage 详情：1 个、EtwFilterDialog 供应商预设：1 个
- 有明确显示上限的列表：**8 处**
  - `Ui/ListViewUtil.h:15` `kMaxDisplayRows = 5000`（全局默认上限）
  - `Ui/EvidenceSessionView.cpp:210` 复用同一上限，超限在状态栏写「仅显示前 X 项 / 共 Y 项」（`:251-253`）
  - `Features/ProcessDetail/ProcessDetailPage.cpp:1067` 手写同一上限的截断+状态提示
  - `Features/Monitor/EtwMonitorView.cpp:44` `kMaxEventRows = 5000`（环形缓冲）
  - `Features/Monitor/CallbackMonitorView.cpp:41` `kMaxLocalRecords = 10000`
  - `Features/Driver/DriverDebugOutputView.cpp:46` `kMaximumRecords = 2000`
  - `Features/Monitor/EtwMonitorView.cpp:45` `kMaxEventsPerFlush = 200`（每 tick 处理量，非总量）
  - `Features/Hardware/HardwareHwidDispatchView.cpp:372` 固定 `KSWORD_ARK_HWID_DISPATCH_ENTRY_COUNT` 行

| # | 文件:行 | 用途 | owner-data | 插入方式 | 刷新线程 | 上限 | 判定 |
|---|---------|------|-----------|----------|----------|------|------|
| 1 | `Features/Process/ProcessView.cpp:515` | 进程列表（主表） | 是（原生） | `SetItemCountEx` + 自绘 `LVN_GETDISPINFO`；快照走 `ProcessModel::setSharedRows` (`:1777`) | 后台 prep 任务 + UI 线程 16ms 分片提交 (`:1059-1107`) | 无（owner-data 不需要） | OK(已虚拟化) |
| 2 | `Features/File/FileView.cpp:285` | 文件浏览 | 是（原生） | `SetItemCountEx` (`:483`) + 手写 `LVN_GETDISPINFOW` (`:788`) | 后台 `refreshTask`(`:593`)/`filterTask`(`:534`) | 无 | OK(已虚拟化) |
| 3 | `Features/File/FileFeature.cpp:1616` | 文件审计表 | 是（原生） | `SetItemCountEx` (`:1225`) | 后台 | 无 | OK(已虚拟化) |
| 4 | `Features/Monitor/EtwMonitorView.cpp:410` | ETW 事件流 | 是（原生） | `SetItemCountEx` (`:643`, `:839` 清空) | UI 定时器从后台 inbox 取，单次 ≤200 (`:592`) | 5000 (`:44`) | OK(已虚拟化) |
| 5 | `Features/Monitor/CallbackMonitorView.cpp:656` | 回调监控 | 是（原生） | `SetItemCountEx` (`:213`) | 后台采集 + UI 刷新 | 10000 (`:41`) | OK(已虚拟化) |
| 6 | `Features/Kernel/KernelPage.cpp:2667` | 内核结果主表 | 是（原生） | `SetItemCountEx` (`:4550`, `:11698` 清空) | 后台 prep (`:6335-6336`) | 无 | OK(已虚拟化) |
| 7 | `Features/Window/WindowView.cpp:255` | 窗口列表 | 是（封装） | `setSharedRows` (`:1221`) | 后台枚举 + UI 提交 | 无 | OK |
| 8 | `Features/Handle/HandlePage.cpp:92` | 句柄列表 | 是（封装） | `setSharedRows(state_->filterRows)` (`:690`) | 后台 | 无 | OK |
| 9 | `Features/Registry/RegistryView.cpp:133` | 注册表视图 | 是（封装） | `setSharedRows(rows)` (`:644`) | 后台 | 无 | OK |
| 10 | `Features/Registry/RegistrySearchView.cpp:73` | 注册表搜索结果 | 是（封装） | `setSharedRows` (`:324`) | 后台 | 无 | OK |
| 11 | `Features/Network/NetworkView.cpp:79` | 网络连接 | 是（封装） | `setSharedRows` (`:180`) | 后台 | 无 | OK |
| 12 | `Features/Service/ServiceView.cpp:107` | 服务列表 | 是（封装） | `setSharedRows(filterRows)` (`:430`) | 后台 | 无 | OK |
| 13 | `Features/Privilege/PrivilegeView.cpp:85` | 特权列表 | 是（封装） | `setSharedRows(filterRows)` (`:283`) | 后台 | 无 | OK |
| 14 | `Features/Startup/StartupView.cpp:91` | 启动项 | 是（封装） | `setSharedRows(filterRows)` (`:336`) | 后台 | 无 | OK |
| 15 | `Features/NetTools/NetToolsConnectionView.cpp:93` | 连接表 | 是（封装） | `setSharedRows(filterRows)` (`:305`) | 后台 | 无 | OK |
| 16 | `Features/NetTools/NetToolsFirewallView.cpp:83` | 防火墙规则 | 是（封装） | `setSharedRows(filterRows)` (`:296`) | 后台 | 无 | OK |
| 17 | `Features/Driver/DriverObjectView.cpp:54` | 驱动对象 | 是（封装） | `setSharedRows(filterRows)` (`:536`) | 后台 | 无 | OK |
| 18 | `Features/Driver/DriverOverviewView.cpp:57` | 驱动概览 | 是（封装） | `setSharedRows(filterRows)` (`:587`) | 后台 | 无 | OK |
| 19 | `Features/Driver/DriverThreadView.cpp:107` | 驱动线程 | 是（封装） | `setRows(std::move)` (`:300`) | 后台 + UI | 无 | OK |
| 20 | `Features/Driver/DriverUnloadedView.cpp:90` | 已卸载驱动 | 是（封装） | `setSharedRows(filterRows)` (`:382`) | 后台 | 无 | OK |
| 21 | `Features/Driver/DriverDebugOutputView.cpp:103` | 内核调试输出 | 是（封装） | `SetItemCountEx` (`:276`) | 后台 drain (`:357`) | 2000 (`:46`) | OK |
| 22 | `Features/Memory/DriverMemoryView.cpp:148` | 驱动内存历史 | 是（封装） | `setSharedRows(rows)` (`:329`) | 后台 | 无 | OK |
| 23 | `Features/WindowTools/WindowToolsClipboardView.cpp:90` | 剪贴板格式 | 是（封装） | `setSharedRows` (`:367`) | 后台 | 无 | OK |
| 24 | `Features/WindowTools/WindowToolsCaptureView.cpp:79` | 窗口捕获 | 是（封装） | `setSharedRows(shared)` (`:220`) | 后台 | 无 | OK |
| 25 | `Features/WindowTools/WindowToolsHierarchyView.cpp:136` | 窗口层级 | 是（封装） | `setSharedRows(shared)` (`:374`) | 后台 | 无 | OK |
| 26 | `Features/WindowTools/WindowToolsHotkeyView.cpp:93` | 热键占用 | 是（封装） | `setSharedRows(shared)` (`:179`) | 后台探测 | 无 | OK |
| 27 | `Features/SysTools/ContextMenuView.cpp:84` | 右键菜单项 | 是（封装） | `setSharedRows(filterRows)` (`:292`) | 后台 | 无 | OK |
| 28 | `Features/SysTools/EventLogView.cpp:82` | 事件日志 | 是（封装） | `setSharedRows(filterRows)` (`:270`) | 后台 | 无 | OK |
| 29 | `Features/SysTools/FileHolderView.cpp:83` | 文件占用 | 是（封装） | `setSharedRows(filterRows)` (`:216`) | 后台 | 无 | OK |
| 30 | `Features/HardwareStats/BusDeviceView.cpp:77` | 总线设备 | 是（封装） | `setSharedRows` (`:351`) | 后台 | 无 | OK |
| 31 | `Features/HardwareStats/UsbTopologyView.cpp:75` | USB 拓扑 | 是（封装） | `setSharedRows` (`:355`) | 后台 | 无 | OK |
| 32 | `Features/HardwareStats/DiskActivityView.cpp:66` | 磁盘活动 | 是（封装） | `setRows(std::move)` (`:205`) | 后台 + UI | 无 | OK |
| 33 | `Features/HardwareStats/PerformanceView.cpp:66` | 性能计数器 | 是（封装） | `setRows(std::move)` (`:209`) | 后台 + UI | 无 | OK |
| 34 | `Features/Misc/MiscFeature.cpp:122` | 杂项表 | 是（封装） | `setSharedRows(filterRows)` (`:535`) | 后台 | 无 | OK |
| 35 | `Features/ProcessDetail/ProcessDetailPage.h:480` | 线程列表 | 是（封装） | `setSharedRows` (`ProcessDetailPage.Thread.cpp:448/481/543`) | 后台 | 无 | OK |
| 36 | `Features/ProcessDetail/ProcessDetailPage.h:481` | 模块列表 | 是（封装） | `setSharedRows` (`ProcessDetailPage.Module.cpp:670/698/796`) | 后台 | 无 | OK |
| 37 | `Ui/EvidenceSessionView.cpp:431` | 证据会话列表 | 是（封装，`dc282eb1`） | `setSharedRows` (`:246`)，行表在后台 `BuildSnapshotRows`(`:206`) 建好 | 后台 `snapshotTask`(`:278/280`)，UI 只装索引 | 5000 (`:210`，状态栏提示 `:251-253`) | OK(已虚拟化) |
| 38 | `Features/Kernel/KernelPage.cpp:2973` | 文件系统事件列表 | **否** | 全量 `DeleteAllItems` (`:7569`) + 逐行 `AddListRow` (`:7653`) | **UI 线程**（`PopulateCallbackInterceptPanel`，`:1769`/`:5441` 调用） | **无** | **风险** |
| 39 | `Features/ProcessDetail/ProcessDetailPage.cpp:983` | 热键 / 键盘列表 | 否 | 全量 `DeleteAllItems` (`:1052`) + `AddListRow` (`:1090`) | UI 线程 | 5000 (`:1067`，超限写状态栏 `:1077`) | OK(有上限) |
| 40 | `Features/Hardware/HardwareHwidDispatchView.cpp:386` | HWID 状态表 | 否 | `ReplaceAuditTableRows` → `ReplaceListViewRowsCapped` | UI 线程（每次刷新重建） | 5000 + 固定 8 行 (`:372`) | OK(有上限) |
| 41 | `Features/AuditCommon/AuditTable.cpp:88` | 通用只读审计表 helper | 否 | `ReplaceListViewRowsCapped` (`:145`) | 调用方决定 | 5000 (`ListViewUtil.h:15`) | OK(有上限) |
| 42 | `Features/Monitor/EtwFilterDialog.cpp:145` | ETW 供应商勾选 | 否 | 全量 `DeleteAllItems` (`:235`) + `InsertItem` (`:242`) | UI 线程 | 无（数据是预设表，几十行） | OK(小数据量) |
| 43 | `Features/Handle/HandlePage.cpp:548` | 句柄属性详情 | 否 | `InsertListViewTextRow` (`:331`) | UI 线程 | 无（每行一属性，<50） | OK(小数据量) |
| 44 | `Features/Privilege/PrivilegeView.cpp:529` | 特权详情 | 否 | `SetDetailText` → `InsertItem` (`:114`) | UI 线程 | 无（键值对） | OK(小数据量) |
| 45 | `Features/Service/ServiceView.cpp:1002` | 服务详情 | 否 | 同上 (`:141`) | UI 线程 | 无 | OK(小数据量) |
| 46 | `Features/HardwareStats/BusDeviceView.cpp:530` | 设备详情 | 否 | 同上 (`:109`) | UI 线程 | 无 | OK(小数据量) |
| 47 | `Features/HardwareStats/UsbTopologyView.cpp:529` | USB 节点详情 | 否 | 同上 (`:106`) | UI 线程 | 无 | OK(小数据量) |
| 48 | `Features/NetTools/NetToolsConnectionView.cpp:658` | 连接详情 | 否 | 同上 (`:122`) | UI 线程 | 无 | OK(小数据量) |
| 49 | `Features/NetTools/NetToolsFirewallView.cpp:623` | 规则详情 | 否 | 同上 (`:111`) | UI 线程 | 无 | OK(小数据量) |
| 50 | `Features/WindowTools/WindowToolsClipboardView.cpp:633` | 剪贴板详情 | 否 | 同上 (`:159`) | UI 线程 | 无 | OK(小数据量) |
| 51 | `Features/Window/WindowView.cpp:1868` | 窗口属性详情 | 否 | `SetListText` → `InsertItem` (`:392`) | UI 线程 | 无 | OK(小数据量) |
| 52 | `Features/Startup/StartupView.cpp:724` | 启动项详情 | 否 | `SetListText` → `InsertItem` (`:120`) | UI 线程 | 无 | OK(小数据量) |
| 53 | `Features/Hardware/HardwareView.cpp:716` | 硬件属性详情 | 否 | `SetListText` → `InsertItem` (`:116`) | UI 线程 | 无 | OK(小数据量) |
| 54 | `Features/Kernel/KernelPage.cpp:2684` | 内核对象属性 | 否 | 全量重建 (`:5737`, `:11714`) | UI 线程 | 无（单对象属性） | OK(小数据量) |
| 55 | `Features/Kernel/KernelPage.cpp:2713` | 内核对象摘要 | 否 | 全量重建 (`:5738`, `:9142`) | UI 线程 | 无 | OK(小数据量) |
| 56 | `Features/Kernel/KernelPage.cpp:2888` | 回调规则组 | 否 | 全量重建 (`:7561`) | UI 线程 | 无（组数少） | OK(小数据量) |
| 57 | `Features/Kernel/KernelPage.cpp:2908` | 回调规则（7 个页签各一） | 否 | 全量重建 (`:7563`, `:7739`) | UI 线程 | 无（规则数受驱动配置限制） | OK(小数据量) |
| 58 | `Features/Kernel/KernelPage.cpp:2933` | Minifilter PID 放行 | 否 | 全量重建 (`:7566`) | UI 线程 | 无（PID 白名单短） | OK(小数据量) |

## 逐个说明

### 1. `Features/Process/ProcessView.cpp:515` 进程列表 — OK（参考实现）

原生 `LVS_OWNERDATA`（`:515`），没有用 `VirtualListView` 封装，但把封装里的思路做到了极致：

- 数据侧 `ProcessModel::setSharedRows`（`:1777`）共享 `ProcessSnapshotRow` 快照指针，不深拷贝。
- 呈现准备放在后台，`nativeCommit` 在 UI 线程按 16ms/128 条分片提交（`:1059`、`:1090`），刷新过程不冻结消息循环。
- 计数只用 `SetItemCountEx(count, LVSICF_NOINVALIDATEALL|LVSICF_NOSCROLL)`（`:1079`），从不逐行 `InsertItem`。
- 过滤在后台任务里算（`:1124` → `FilterRowIndexes` `:1126`），带 generation 校验丢弃过期结果。

这是「后台组装 + 虚拟化 + 分片提交」的标杆，其余页面可以照抄。

### 2. `Features/File/FileView.cpp:285` 文件浏览 — OK

`WC_LISTVIEWW` + `LVS_OWNERDATA`（`:285`），手写 `LVN_GETDISPINFOW`（`:788`），行数据是 `state.entries` 全量快照 + `visibleIndexes` 索引（`:478`）。刷新 `:593`、过滤 `:534`、清空 `:569` 都在后台任务里完成，UI 线程只做 `SetItemCountEx`（`:483`）。导出走 `state.entries`，不受可见集影响。

### 3. `Features/File/FileFeature.cpp:1616` 文件审计表 — OK

`CreateReportListView(..., LVS_OWNERDATA)`（`:1616`），`SetItemCountEx(state.rows->size())`（`:1225`），快照是 `shared_ptr<const vector>`。

### 4. `Features/Monitor/EtwMonitorView.cpp:410` ETW 事件流 — OK

owner-data（`:410`），`kMaxEventRows = 5000` 环形裁剪（`:771-776` 同时修正 `visibleEventIndexes_` 与 `firstEventSequence_`），每次 tick 最多消费 200 条（`:592`），避免一次刷新把 UI 打断。清空走 `SetItemCountEx(0)`（`:839`）。

### 5. `Features/Monitor/CallbackMonitorView.cpp:656` 回调监控 — OK

owner-data（`:656`），本地环形缓冲 `kMaxLocalRecords = 10000`（`:41`，`:573` 弹出），`SetItemCountEx` 更新计数（`:213`）。

### 6. `Features/Kernel/KernelPage.cpp:2667` 内核结果主表 — OK

owner-data（`:2670` 的样式行），行来自 `currentRows_`，`SetItemCountEx`（`:4550`）/ 清空 `SetItemCountEx(0)`（`:11698`）。`currentRows_` 由后台结果安装（`:6335-6336`、`:5370`），`RetireBackgroundValue` 处理旧快照延迟释放（`:4538`、`:5610`）。列重建只动表头（`:4594` 起），不重插行。

### 7-36. 走 `VirtualListView` 封装的 30 个列表 — OK

（第 31 个封装列表是 §37 的证据会话，已单列说明。）

统一特征：`LVS_OWNERDATA` 由 `VirtualListView::create` 添加（`Ui/VirtualListView.cpp:25`），文本按需 `LVN_GETDISPINFO`，过滤调用静态 `FilterRowIndexes`（可在工作线程），`NM_CUSTOMDRAW` 走 `handleNotify`。数据组装全部在 `AsyncSnapshotTask` 之类的后台任务里，UI 线程只装索引。

这些页面**全部**已走共享快照快路径（`setSharedRows`），UI 线程没有整表拷贝（见 §已修复）。

### 37. `Ui/EvidenceSessionView.cpp:431` 证据会话列表 — OK（已修复，可作范式）

初版普查时这是两处风险之一：无 owner-data、UI 线程 `DeleteAllItems` + 整表 `InsertListViewTextRow`、无上限。**`dc282eb1`「证据列表改为 owner-data 虚拟表并按 5000 行封顶」已完整修复**，且修法值得抄：

- 控件换成 `VirtualListView`（`:431` `state.list.create(...)`），插入成本归零。
- **行表构建整个搬进后台**：`BuildSnapshotRows`（`:206`）在 `snapshotTask` 的 Work 里跑（`:278`），`sharedSnapshot()` 也在 Work 里取——原来那次外层容器拷贝（N 次原子递增）现在不发生在 UI 线程。
- `TimestampText` 与 2 次 `to_wstring` 全部在 Work 里做（`:218-225`），UI 线程只收一份冻结的行表。
- 交接零拷贝：`state.list.setSharedRows(std::move(result.rows))`（`:246`）。
- 上限在 Work 里就生效（`:210`），超限时状态栏写「仅显示前 X 项 / 共 Y 项」（`:251-253`），不误导。
- `itemData` 存的是**快照下标**，`ModelIndexForPosition`（`:109`）经 `visibleIndexes()` 两步映射回到快照项；`EvidenceSnapshotRows`（`:49`）把行表与快照打包一起传，注释写明「The two travel together so a row index can never address a different evidence item than the one the row was built from」。**后续不要拆开这两个容器**——拆开就会引入「行指向另一个证据项」的错误。

这条正好印证前述判断的**完整形态**：只做 owner-data 而不搬构建，UI 线程仍会留下 N 行 `VirtualListRow` 构造 + 5 次字符串格式化；`dc282eb1` 是「虚拟化 + 后台物化」两件事一起做，才真正把 UI 线程成本清空。

其余候选页若要照此改造，可对照这四处：控件换封装、构建进 Work、`setSharedRows` 交接、上限与提示成对出现。

在虚拟化改造排期之前可以先止血：给插入循环加 `kMaxDisplayRows` 截断，提示拼进已有的 `SetStatus`（`EvidenceSessionView.cpp:176-178`）——参见 `Ui/ListViewUtil.h:108` / `:116`。

### 38. `Features/Kernel/KernelPage.cpp:2973` 文件系统事件列表 — **风险**

- 创建：`CreateWindowExW(WC_LISTVIEWW, LVS_REPORT | LVS_SHOWSELALWAYS)`（`:2973`），无 owner-data。
- 插入：`PopulateCallbackInterceptPanel`（`:7550`）先 `DeleteAllItems`（`:7569`），再遍历 `currentRows_` 的 `FileMonitor` / `FileEvent` 段逐行 `AddListRow`（`:7653-7665`）。
- 线程：`PopulateCallbackInterceptPanel` 在 UI 线程执行，调用点是 `WM_COMMAND` 分支（`:1769`，勾选"仅显示 Oplock / FSCTL" 等）和缓存投影安装路径（`:5441`）。
- 上限：**没有任何截断**。行数完全取决于驱动侧 drain 出来的事件量，而这个列表正是「持续拉事件」的场景（`启动 FSCTL 监控` / `拉取事件`），是全文件里最可能真正跑出大行数的非虚拟列表。
- 额外的中间层开销：取单元格用的是 `rowValue`（`:7583-7592`），它对每个别名做线性 `_wcsicmp` 扫列**并按值返回 `std::wstring`**。FileMonitor 分支每行调 11 次 `rowValue` 产 11 个格子（`:7653-7665`），单行就是数百次字符串比较；这个开销与「逐格跨进程插入」叠加在同一线程上。

同函数里的规则/组/放行列表受配置规模限制，不需要虚拟化；文件事件表则必须要么改 owner-data，要么至少套上 `kMaxDisplayRows` 截断——用现成的 `ReplaceListViewRowsCapped`（`Ui/ListViewUtil.h:108`，实现 `ListViewUtil.cpp:159`）+ `RowCapNotice`（`Ui/ListViewUtil.h:116`），并**必须把提示显示出来**，否则截断会让用户误以为「数据只有这么多」，比卡顿更糟（这条设计意图写在 `Ui/ListViewUtil.h:106-107`）。

### 39. `Features/ProcessDetail/ProcessDetailPage.cpp:983` 热键 / 键盘列表 — OK(有上限)

`AddList` 用普通 `LVS_REPORT`（`:983`）。`AddListRow` 在插入前判断 `row >= kMaxDisplayRows`（`:1067`），超限直接丢弃并在状态栏追加「已达显示上限 5000 行」（`:1077`），注释明确写了「只是显示上限，后端 vector 保持完整，复制/详情仍用完整索引」（`:1064-1066`）。这是「不虚拟化时的正确兜底写法」。

注意：同页真正的大表（线程 `ProcessDetailPage.Thread.cpp:420`、模块 `ProcessDetailPage.Module.cpp:656`）已经通过 `AddVirtualList` 走封装。

### 40-41. `HardwareHwidDispatchView` / `AuditTable` — OK(有上限)

`HardwareHwidDispatchView.cpp:386` 走 `ReplaceAuditTableRows`，行数固定（`KSWORD_ARK_HWID_DISPATCH_ENTRY_COUNT`，`:372-384`），拿 `RowCapNotice` 附到状态栏（`:389-393`）。`AuditTable.cpp:145` 是通用包装，直接转发到 `ReplaceListViewRowsCapped`，默认 5000。

### 42. `Features/Monitor/EtwFilterDialog.cpp:145` ETW 供应商勾选 — OK(小数据量)

`LVS_REPORT | LVS_SINGLESEL`（`:145`），`loadStateToControls` 全量重插（`:235-246`），但数据源是固定的供应商预设数组（`state_.providers`），量级几十行，且带 `LVS_EX_CHECKBOXES`，不适合 owner-data（勾选状态要落在模型里）。

### 43-58. 详情面板与 KernelPage 辅助表 — OK(小数据量)

这些 `detailList_` / `propertyList_` / `summaryList_` 都是「一条记录展开成 N 个键值对」，N 由协议字段数决定（十几到几十行），单次选中变化才重建一次。10 个页面用的是完全相同的 `SetDetailText` / `SetListText` 写法（`column == 0` 时 `InsertItem`，否则 `SetItemText`），见 `ServiceView.cpp:134-145`、`PrivilegeView.cpp:107`、`BusDeviceView.cpp:102`、`UsbTopologyView.cpp:99`、`NetToolsConnectionView.cpp:115`、`NetToolsFirewallView.cpp:104`、`WindowToolsClipboardView.cpp:152`、`WindowView.cpp:380-393`、`StartupView.cpp:113-124`、`HardwareView.cpp:109-119`。

`KernelPage` 的 5 个辅助表（`:2684`、`:2713`、`:2888`、`:2908`、`:2933`）由 `RestoreReportListRows`（`:4580-4592`）在换页签时整表恢复，数据是缓存的小表，量级可控。

## 已修复：`setRows(*shared)` 的整表深拷贝（本轮普查后已清零）

初版普查时发现 18 处「明明持有共享指针，却解引用后传给按值接收的 `setRows`」，等于在 UI 线程把整表 `VirtualListRow`（含全部 `std::wstring` 单元格）深拷贝一次。**这些已全部修复，当前 `setRows(*ptr)` 全项目 0 处**：

- `45ca93b0`「列表共享快照清扫，17 处 `setRows(*ptr)` 改为 `setSharedRows(ptr)`」——覆盖 17 处
- `969604fc`「registry: 树迁到 LazyTreeView」——连带覆盖 `RegistryView` 那 1 处（该行现在位于 `Features/Registry/RegistryView.cpp:644`）

复核结果：`grep -rn "setRows(\*"` 在 `KswordARKLight` 源码中已无命中，`setSharedRows(` 调用点 32 处（含 31 个封装页 + `ProcessModel`；其中 `EvidenceSessionView` 的 1 处由 `dc282eb1` 新增）。

值得记录的修复边界（`45ca93b0` 的做法，后续同类改动应照此把握）：

- **不改** `setRows(std::move(rows))`——真右值，`setRows` 内部直接 move 进 `make_shared`，与 `setSharedRows` 同价，没必要改；现存的 `DriverThreadView.cpp:300`、`DiskActivityView.cpp:205`、`PerformanceView.cpp:209` 属于这一类。
- **不改** `setRows({})`——清空，没有拷贝成本。
- **必查**交接后的不变异性：`VirtualListView.h:47-48` 要求「caller must never mutate the vector after hand-off」。提交里逐处核查了 17 个交接点没有 `push_back`/`clear`/`resize`/`erase`/`operator[]` 赋值。这条约定是这类改造的隐式风险——只改调用点不查写入，会把「多一次拷贝」换成「数据竞争或悬垂」。

因此本文表格里各页的「插入方式」列也随之更新：原先标 `setRows(*x)` 的 18 行现在都是 `setSharedRows(x)`，UI 线程不再有这次整表拷贝。

## 交叉引用

- **拷贝热点的展开分析**在 `docs/audit/ui-thread-copy-hotspots.md`（34 条）。本文 §37 对应其 **#30**（已由 `dc282eb1` 修复），§38 对应其 **#31**（未修），两侧 file:line 已互相对齐；那 18 处 `setRows(*ptr)` 是两边独立统计、逐行齐平的同一批点，且已由 `45ca93b0` + `969604fc` 修复清零。
- **改造排期建议**：§38 与该报告的 #31/#32/#34 归为同一批——「无 owner-data 的 ListView 改走 `VirtualListView`」。`VirtualListView` 无需新增能力，`setSharedRows` 快路径已在 31 个封装页全部用上，§37 给出了一份可直接照抄的完整改造样本。
- **树控件不在本文范围**：注册表树已由 `969604fc` 迁到 `LazyTreeView`，`RegistryView.cpp:348` 的 `ReplaceTreeNodeChildren` 现在只是组装 `TreeRow` 后调 `state.tree.setChildren(...)`（`:364`），分批/「载入更多」/单父节点上限都由控件负责（`Ui/LazyTreeView.h`），本文不对树侧下结论；树控件普查由 TreeSurvey-bunny 出，`docs/audit/registry-expand-perf.md` 由 RegistryPerf-flash 产出。

## 结论

1. 虚拟化覆盖率已经很高：37 个主数据表里 37 个走 owner-data（31 个封装 + 6 个手写），没有「主表还在全量 InsertItem」的遗漏，唯一的例外见下一条。
2. **当前只剩一处风险**：`Features/Kernel/KernelPage.cpp:2973` 文件系统事件表——无上限 + 无 owner-data + UI 线程 `DeleteAllItems` 重建（`:7569`/`:7653`，调用点 `:1769`/`:5441`）。原先并列的另一处 `Ui/EvidenceSessionView.cpp` 已由 `dc282eb1` 修复，见 §37。
3. 其余非虚拟列表都是详情面板 / 固定预设 / 已加 5000 截断，维持现状即可。
4. `setRows(*shared)` 那 18 处整表深拷贝已在 `45ca93b0`（17 处）与 `969604fc`（1 处）修复清零，本文只留作记录，不再是待办。
5. §37 的修法（owner-data + 后台物化 + `setSharedRows` 交接 + 上限与提示成对）可以直接作为 §38 的改造模板。


