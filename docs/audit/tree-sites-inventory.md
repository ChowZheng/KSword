# TreeView 控件站点盘点（`Ksword::Ui::LazyTreeView` 迁移清单）

审计对象：`E:\Projects\Ksword5.1\KswordARKLight`（纯 Win32 / C++17 / MSVC）
性质：**只读审计，未修改任何源码**。本文件是本次唯一的产出。
行号基线：`a787bdb3 refactor(kernel): 抽离内核值辅助函数到 KernelValueSupport.h/.cpp`（工作区干净，除 `docs/audit/*.md` 未跟踪）。
配套阅读：`docs/audit/tree-survey.md`（同轮性能普查，393 行），本文只做**迁移视角**，两份结论一致处不重复论证。

> **v2 更新说明（重要）**：本文 v1 基线是 `a44cbf6f`。此后落地了 `a74eba1a hardware: 设备树迁到 LazyTreeView`、`a787bdb3` 内核页抽离 `KernelValueSupport`（`KernelPage.cpp` 由 11861 行降到 11363 行，树相关行号整体 **-500 左右**），并且 `LazyTreeView` 新增了 `setChildSource`（同步内存数据源）。v1 中"硬件树是唯一待迁站点""同步数据源与展开契约冲突"两条**已经过期**，本版已改。v1 的硬件页行号（727/278/432/810）全部作废。

---

## 1. 结论

1. **全仓 `WC_TREEVIEWW` 实例仍是 4 个、3 个文件，但迁移状态变了**：注册表树（`RegistryView.cpp`，`969604fc` 迁完）、硬件设备树（`HardwareView.cpp`，`a74eba1a` 迁完）、内核对象命名空间树（`KernelPage.cpp`，两个创建点，**未迁且不建议迁**）。
2. **"插空子节点换展开箭头"全仓不存在**：两个已迁页面走 `TVIF_CHILDREN`（`LazyTreeView.cpp:121-126`），内核树是全量投影、子节点本来就是真节点。`LazyTreeView` 的这条规则没有存量包袱。
3. **迁移收益已从"硬件页"转移到"`LazyTreeView::reset()`"**：两个已迁页面每次重建都走 `reset()` → `ClearTreeView` → `TreeView_DeleteItem(hwnd, TVI_ROOT)`（`Ui/LazyTreeView.cpp:272-281` + `Ui/TreeViewUtil.cpp:62-68`），单条消息内递归删除整棵已展开树，删除期间每条 `TVN_DELETEITEMW` 还要同步擦一次哈希。这是目前**唯一还在实时承受**的树相关长阻塞形态。
4. **内核对象命名空间树不该迁**：已做 16ms / 4ms 分片插入与逐叶分片删除，迁过去会把删除退化成 `TVI_ROOT` 一次性递归；而且**这棵树在布局里始终隐藏**（创建无 `WS_VISIBLE`，10 处 `SW_HIDE`、0 处 `SW_SHOW`，3 处 `MoveWindow(...,0,0)`），零用户收益。
5. **文件/驱动/句柄/进程页没有树**：`Features/File*`、`Features/Driver*`、`Features/Handle*`、`Features/Process*`、`Features/HardwareStats*`、`Features/Window*` 全是 `WC_LISTVIEWW`；无 `Features/Device*` 目录。

---

## 2. 检索口径与全仓命中

| 模式 | 命中（排除 docs） |
|---|---|
| `CreateTreeView` | `Ui/TreeViewUtil.{h,cpp}`、`Ui/LazyTreeView.cpp:34` |
| `WC_TREEVIEWW` | `Ui/TreeViewUtil.cpp:14`、`Ui/LazyTreeView.{h,cpp}`、`Features/Kernel/KernelPage.cpp:2158,5352`、`Features/Hardware` 已无（随 `a74eba1a` 删除） |
| `"SysTreeView32"` | **0 命中** |
| `CreateWindowExW.*TREEVIEW` | 仅 `Ui/TreeViewUtil.cpp:12` + `KernelPage.cpp:2157,5351` |
| `HTREEITEM\|TreeView_`（文件计数） | `RegistryView.cpp`、`HardwareView.cpp`、`KernelPage.cpp`、`Ui/LazyTreeView.cpp`、`Ui/TreeViewUtil.cpp` —— **无第 4 个业务文件** |
| `TVN_ITEMEXPAND\|TVN_SELCHANGED\|TVN_DELETEITEM` | `HardwareView.cpp:789,793`、`KernelPage.cpp:1941`、`RegistryView.cpp:1323,1365`、`Ui/LazyTreeView.cpp:460-470` |

明确排除：`KswordSetup/KswordGUI/KNavigation.h:132` 的 `KTreeView` 是 **FLTK `Fl_Group` 自绘**，属安装器进程；`Ui/Controls.cpp:44` `ICC_TREEVIEW_CLASSES` 只是 `InitCommonControlsEx` 标志；`Features/Process/ProcessView.cpp` 的 `kTreeIndentPixels`/`DrawDottedTreeGuides` 是虚拟列表里画出来的缩进虚线；`Ui/TreeViewUtil.cpp:31 InsertTreeItem` / `:70 SetTreeViewImageList` 仍无人调用（死代码）。

---

## 3. 站点总表

| # | 站点 | 创建点 | 单父节点数量级 | 占位子节点 | `WM_SETREDRAW` | 节点数据 | 查找节点 | 展开来源 | 状态 |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 硬件设备管理器树 | `HardwareView.cpp:715`（`LazyTreeView::create`） | 根 5–40；单父典型 10–150；最坏 10³；枚举硬上界 100000 | 无（`hasChildren`→`TVIF_CHILDREN`，`MakeDeviceRow:260`） | **有**（控件内 `beginUpdate/endUpdate`） | `key` = 设备索引字符串（`246-248`），`userData` = 索引（`259`），模型持有；控件按 `HTREEITEM` 记记录 | `findItem()` O(1)（`455/463`） | **同步内存源**（`setChildSource:712-714`），插入在控件内分批 | **已迁移（a74eba1a）** |
| 2 | 内核对象命名空间树（Overview） | `KernelPage.cpp:2157` | 根 15+；单目录 `maxEntries=100000`/8s；最坏 10⁴ | 无（全量投影） | 泵内无；仅 `4844` 外层 lock | `lParam` = 裸指针（`5424`），投影 `shared_ptr` 持有 | 无扫描；选中/置顶只读 2 项（`5150-5151`），恢复 O(1)（`5435`） | 模型异步 + 16ms/4ms 分片 | 未迁，**不建议迁** |
| 3 | 内核对象命名空间树（Recursive，按 featureId 保留） | `KernelPage.cpp:5351` | `maxRows=2500, maxDepth=4` | 无 | 同上 | 同上 | 同上 | 同上 | 同上 |
| 4 | 注册表树 | `RegistryView.cpp:1215` | 子键数后端无上限 → 命中 5000 截断 | 无（注释 `341`） | **有** | 控件自有 map + key 索引 | `findItem()` O(1)（`448/485/495`） | 异步（`treeChildrenTask:413`）+ 分批 | **已迁移（969604fc）** |

---

## 4. 逐站点明细

### 4.1 硬件设备树（`Features/Hardware/HardwareView.cpp`）— 已迁 `LazyTreeView`

| 项 | 位置 | 说明 |
|---|---|---|
| 控件成员 | `82` | `Ksword::Ui::LazyTreeView tree`（不再是裸 `HWND`） |
| 创建 | `715` | `state->tree.create(hwnd, kTreeId, 0, 0, 100, 100)` |
| 子节点来源 | `712-714` | `setChildSource`：`ChildRowsOf(*state, wcstol(key))`；**同步**，因为设备模型早在内存里（不用后台往返，也不用 `setChildren` 二次调用） |
| 行构造 | `252-262` `MakeDeviceRow` | `text=CompactDeviceName`、`key=DeviceTreeKey(index)`、`userData=index`、`hasChildren=HasVisibleChildren` |
| 子行收集 | `266-280` `ChildRowsOf` | 遍历 `childIndices`，按 `IsVisibleDevice` 过滤 |
| 整树重建 | `370-396` `PopulateTree` | 组根 `TreeRow` → `setRoots`（383）→ 若筛选非空则**遍历所有根逐个 `TreeView_Expand`**（384-394）→ `SelectFirstRoot`（395） |
| 选中恢复 | `433-469` `EnsureTreeItemVisible` | 用 `findItem(DeviceTreeKey(...))` 逐层命中；不再有 `TVM_GETITEM` 全子节点扫描 |
| 取选中数据 | `214-224` `SelectedDeviceIndex` | `tree.selected()` + `tree.userDataOf()`（注释已写明"树项不再携带 lParam"） |
| `WM_NOTIFY` | `784-799` | 先 `handleNotify`（789）整包消费展开/删除/合成行选中；再处理 `TVN_SELCHANGEDW`（793-798），带 `silentSelection()` + `isNotice()` 双过滤 + `userDataOf` |
| 右键 | `800-805`（NM_RCLICK）、`814` 起（WM_CONTEXTMENU） | 用 `tree.hwnd()` 做 `HitTest`/`GetWindowRect` |
| 数量级 | 后端 `shared/usermode/backend/hardware/HardwareEnumerator.cpp:353` | `ordinal<100000` 硬上界；真实设备 150–400；`RebuildHierarchy`（`293-330`）产生根 5–40 个 |
| 数据所有权 | — | 无堆节点对象，删除不漏；`key` 用索引字符串，**只在同一份快照内唯一**，快照一换整树重建（`246-248` 注释已声明该约定） |
| 已消失的旧热点 | — | `FindTreeItemByDeviceIndex`（旧 432-448 递归全树）、`InsertTreeNode` 递归、`PopulateTreeItemChildren` 同步整层插入、`TreeView_DeleteAllItems` + `WM_SETREDRAW` 手写段，全部随迁移删除 |

**迁移后残留的三个问题（这是本文件当前的核心输出）**：

- **H1 每次筛选 `reset()` = 一次 `TVI_ROOT` 递归删除**：`ApplyHardwareFilter`（`471-494`）→ `PopulateTree`（`370`）→ `setRoots`（`383`）→ `reset()`（`LazyTreeView.cpp:272-281`）→ `ClearTreeView`（`TreeViewUtil.cpp:62-68`）→ `TreeView_DeleteItem(hwnd, TVI_ROOT)`。已展开的子树越大，这一条消息越长；删除过程中每条 `TVN_DELETEITEMW` 还会同步 `eraseRecord`。
- **H2 筛选无防抖**：`EN_CHANGE`（`760-762`）直接 `RequestHardwareFilter`；全文件 `SetTimer` 0 命中。对比注册表页 250ms 防抖（`RegistryView.cpp:50 kTreeFilterDebounceMs` + `:1404 SetTimer`）。缓解：筛选本身走后台 `AsyncSnapshotTask`，连续按键会被请求合并，不会真的重建 N 次。
- **H3 筛选态展开所有根，单条消息插入量 = 根数 × 400**：`384-394` 对每个根调 `TreeView_Expand`，每个根触发一次 `childSource` → `installChildren` → 一次 `insertBatch`（≤ `kBatchSize=400`）。`kBatchSize` 与 `kMaxRowsPerParent=5000` 都是**单父**上限，这条路径的整体插入量不在这两个上限的覆盖范围内。根数通常 5–40，即单条消息最坏 2k–16k 行插入（已在 `WM_SETREDRAW(FALSE)` 内，绘制成本被抑制，但条目记账仍是同步的）。

### 4.2 内核对象命名空间树（`Features/Kernel/KernelPage.cpp`）— 未迁

| 项 | 位置 | 说明 |
|---|---|---|
| 创建 #1 | `2157-2168` | `KernelPage::Create`，样式**无 `WS_VISIBLE`** |
| 创建 #2 | `5351-5357` | `SelectObjectNamespaceTree` 内按 featureId 补建，缓存进 `objectNamespaceTrees_` |
| 停放/切换 | `5332-5364` | 当前树 `SW_HIDE` + 投影存入 `parked.projection`；目标树挂回 + `SetTimer(16ms)` |
| 重建入口 | `5440-5442` → `RequestCachedValueProjection` → 后台 `cachedProjectionTask_` | 树模型在工作线程投影成扁平 `treeNodes`（`KernelProjectionValues.inl:2727-2925`） |
| 插入循环 | `5420-5430` | 定时器泵，预算 `work+2<=128 && GetTickCount64()-started<4`；`lParam = node.state.get()`（5424） |
| 删除循环 | `5398-5408` | 逐叶删除（`GetChild` 下潜 → `GetParent` 回退 → `DeleteItem`），预算 `work+4<=128` 且 <4ms；注释明令"一次 `TVM_DELETEITEM` 不得递归删无界旧子树" |
| 清空点 | 仅上面逐叶删除 | 无 `DeleteAllItems`、无 `TVI_ROOT` |
| `TVN_*` | `1941-1945` 仅 `TVN_SELCHANGEDW` | `SelectObjectNamespaceTreeItem(changed->itemNew.lParam)`（1944） |
| 节点状态 | `KernelPage.h:26 struct KernelObjectNamespaceTreeNodeState`；key 在 `KernelValueSupport.h:224` / `.cpp:451` | `lParam` 是裸指针，所有权在投影 `shared_ptr`（`KernelProjectionValues.h:36`） |
| 生命周期 | `Pump` 内 `commit->retired` → `ApplyCachedValueProjection` → `objectNamespaceProjection_=projection`（约 5404-5416） | 先删完旧项再换投影，顺序正确；若将来先 retire 再删项，`lParam` 立即悬垂 |
| 数量级 | `ObjectNamespace.h:167` `maxEntries=100000,maxDurationMs=8000`；`ObjectDirectory.h:13,16` `kMaxDirectoryRows=2500,maxDepth=4,maxScannedRows=10000` | Overview 单目录可达 10³–10⁴；Recursive 树节点约 3×10³ |
| 可见性 | 创建无 `WS_VISIBLE`；`SW_HIDE` 10 处（2644/2738/2786/2841/2887/2934/2979/3036/3082/5341）；`SW_SHOW` 0 处；`MoveWindow(...,0,0)` 3 处 | **用户从未见过这棵树**，但插入/删除/投影开销全额支付 |

### 4.3 注册表树（`RegistryView.cpp`）— 已迁，模板

| 项 | 位置 |
|---|---|
| 创建 / 回调 | `1215` / `1212-1214` `setExpansionCallback` → `RequestTreeChildren`（`392-468`） |
| 落地 / 通知 / 清空 | `345` `setRoots`、`364` `setChildren`、`1323` `handleNotify`、`1484` `reset()` |
| 防抖 | `50` `kTreeFilterDebounceMs=250`、`1404` `SetTimer` |
| 截断 | 子键后端无上限 → `kMaxRowsPerParent=5000` 会真实命中并显示截断行 |

---

## 5. 收益与待办排序（v2）

| 排名 | 事项 | 收益 | 依据 |
|---|---|---|---|
| 1 | 给 `LazyTreeView::reset()` 补**分片删除** | **高（唯一还在实时承受的树长阻塞）** | 注册表页 + 硬件页每次筛选重建都走 `TVI_ROOT` 单消息递归删；已展开节点数无上限（单父 5000、层数不限）。对齐内核页 `5397-5398` 注释的规矩 |
| 2 | 硬件页筛选加 200–250ms 防抖 | 中 | `760-762` 无防抖；虽有后台请求合并，但每次落地仍触发 H1 + H3 |
| 3 | 硬件页筛选态"展开所有根"改为只展开命中的根 | 中 | `384-394` 单消息最坏 根数 × 400 行插入 |
| 4 | 内核树：**评估停建**（连同 `objectNamespaceTrees_` 缓存） | 中（省 CPU/内存，无 UX 变化） | 树不可见，用户看的是 `resultList_` |
| 5 | 内核树迁 `LazyTreeView` | **负** | 丢分片删除、丢 `nativeTreeHandles` O(1) 恢复、key 可能碰撞、5000 截断改变数据完整性 |

---

## 6. 迁移风险

### 6.1 `LazyTreeView` 能力边界（v2 现状）

| 能力 | 现状 | 影响 |
|---|---|---|
| 同步内存数据源 | **已支持**（`setChildSource`，`LazyTreeView.h:78,94`；`onExpanding:432-437` 在 `TVN_ITEMEXPANDINGW` 内直接装子行并让本次展开继续） | v1 的"同步源必须 PostMessage"结论已作废 |
| 异步后端数据源 | 支持（`setExpansionCallback` + `setChildren`，节点保持折叠直到数据落地） | 注册表页形态 |
| 分片删除 | **不支持**：`reset()` 就是 `TVI_ROOT` 一删了之（6.2 R1） | 两个已迁页面都在承受 |
| 图标/图像列表 | 不支持（`insertOne` 只发 `TVIF_TEXT\|TVIF_PARAM\|TVIF_CHILDREN`，`lParam=0`） | 硬件树未来加图标要先扩控件 |
| 多列/表头 | 不支持 | 与 `docs/audit/arklight-hardware-columns-diff.md:124,129` 的"设备管理补 6 列"待办冲突：接了本封装，补列必须自绘或换控件 |
| 复选框 | 不支持（未消费 `TVN_ITEMCHANGED`） | 硬件页若要"只看问题设备"勾选会撞墙 |
| 按文本排序 | 不支持（按入参顺序；无 `TVS_SORT`/`SortChildrenCB`） | 硬件页靠模型层排序（`HardwareEnumerator.cpp:326-329`），迁移后须保持"调用方排序"约定 |
| 单节点文本增量更新 | 不支持（无 `setItemText`），只能整层 `setChildren`/`clearChildren`/整树 `reset` | 状态文字变化只能重建 |
| 展开事件外传 | 不支持：`handleNotify` 吞掉**所有** `TVN_ITEMEXPANDINGW`（`LazyTreeView.cpp:460-464`） | 页面拿不到"用户刚展开了谁"；需要该事件就得改控件 |
| 拖放 | 不支持（不消费 `TVN_BEGINDRAG`） | 当前无站点需要 |
| 多选 | 不支持 | 当前无站点需要 |
| 全局 key 唯一 | **无校验**：`itemByKey_` 是单张扁平 map（`LazyTreeView.h:186`），撞 key 时 `insertOne` 静默覆盖（`LazyTreeView.cpp:143-145`） | 注册表 key=路径唯一 ✔；硬件 key=索引同快照唯一 ✔；内核 key=`kind\n type\n path` 小写（`KernelValueSupport.cpp:451`）跨作用域可能撞 |

### 6.2 已在两个已迁页面上出现的风险

- **R1 `reset()` 的单消息递归删除**（最高优先级）：`LazyTreeView.cpp:272-281` → `TreeViewUtil.cpp:62-68` → `TreeView_DeleteItem(TVI_ROOT)`。命中路径：注册表 `RebuildRegistryTree:705` → `FillTreeRoots:334` → `setRoots:345`；硬件 `ApplyHardwareFilter:471` → `PopulateTree:370` → `setRoots:383`。删除时每条 `TVN_DELETEITEMW` 还同步 `eraseRecord`。展开越深、单父越宽，这条消息越久。
- **R2 筛选态整层展开**：硬件 `384-394`，`kBatchSize` 是单父上限，这条路径是"根数 × 400"。
- **R3 筛选无防抖**：硬件 `760-762`。
- **R4 合成行必须过滤**：`LazyTreeView::setChildren` 的"载入更多/已截断"行可被选中。硬件已按 `RegistryView.cpp:1367` 的模式加了 `silentSelection()` + `isNotice()`（`795`）；任何新接页面都必须照做，否则会拿合成行去查数据。
- **R5 `lParam` 恒 0**：`LazyTreeView.cpp:131`。两个已迁页面已改走 `userDataOf`（硬件 `223/796`）。**内核页若迁，必须先改 `1944` 与 `5150` 两处 `lParam` 取数。**
- **R6 空文本行无兜底**：`insertOne` 不检查 `text` 为空；`CompactDeviceName` 理论可产出空串。建议调用方给 `<未知设备>`。

### 6.3 内核树专属风险（不建议迁，列出以备决策）

- **K1 删除分片退化**：迁过去 `reset()` 就是 `TVI_ROOT` 递归删，`KernelPage.cpp:5397-5398` 的注释明令禁止的正是这件事。
- **K2 无收益**：树不可见（4.2 末行），只增记录表开销。
- **K3 两棵 parked HWND**：`objectNamespaceTrees_`（`KernelPage.h:291-296`）+ `treeProjectionCommits_[2]`（`308`）；`LazyTreeView` 只有 `attach()`，没有"保留 HWND/展开状态换数据集"的接口，须两个实例且切页全量重插。
- **K4 key 可能碰撞**：见 6.1 末行；撞了会静默覆盖，表现为"点 A 选中 B"。
- **K5 置顶恢复无封装**：现用 `nativeTreeHandles[top-1]` + `SelectSetFirstVisible`（`5435-5436`）；`selectByKey` 只保证选中 + `EnsureVisible`，置顶要手写。
- **K6 5000 截断改变数据完整性**：Overview 单目录 `maxEntries=100000`，`\Sessions\*\BaseNamedObjects`、`\RPC Control` 超 5000 完全可能；迁后从"完整投影"变"截断行"，是功能退化。
- **K7 合成行必须过滤**：现 `1941-1945` 无条件 `SelectObjectNamespaceTreeItem(lParam)`，迁后必须先判 `isNotice/silentSelection`。

### 6.4 剩余待办 checklist（按 5. 的排序）

1. `LazyTreeView` 增加分片删除（例如 `resetChunked()` 或 `reset()` 内部按 4ms/128 单元推进），让注册表页与硬件页复用；这是唯一需要改封装本身的一项。
2. 硬件页 `EN_CHANGE` 加 250ms 防抖，触发点与注册表页保持一致（`RegistryView.cpp:50/1404`）。
3. 硬件页筛选态只展开"含命中项"的根，避免"根数 × 400"。
4. 若要给硬件树补 Qt 那 6 列：先决策"自绘单列树"还是"换控件"，不要在 `LazyTreeView` 上叠列模型。
5. 内核树：把"是否继续建隐藏树"提给产品；若继续保留，不要再叠加迁移。

---

## 附录 A：三站点决策速查

| 维度 | 硬件树 | 内核树 | 注册表树 |
|---|---|---|---|
| 迁移状态 | 已完成（a74eba1a） | 未迁，建议不迁 | 已完成（969604fc） |
| 剩余风险 | `reset()` 递归删、无防抖、筛选全根展开 | 隐藏开销；迁移会退化 | `reset()` 递归删 |
| 用户可见行为变化 | 已发生（>400 出"载入更多"、>5000 截断） | 迁移才会发生（截断 + 删除阻塞） | 已接受 |
| 需要新加封装能力 | 分片删除（共用） | 分片删除 + 数据集热替换 | 分片删除（共用） |

## 附录 B：与 `docs/audit/tree-survey.md` 的关系

- `tree-survey.md`（重写版，393 行）：性能普查主线，按"上限/懒加载/让出消息"判定；已把内核树隐藏升级为结论，并新增 §6.1 专门写 `reset()` 的 `TVI_ROOT` 问题。
- 本文件：迁移可行性主线，回答"还剩哪些站点、迁了会退化什么、封装本身缺什么"。
- 两处交叉引用已对齐：内核树"收益为负"、"`reset()` 分片删除"、"硬件页筛选无防抖 + 全根展开"。行号基线不同（本文件 `a787bdb3`，`tree-survey.md` 为同期当前值），引用时以符号名为准。
