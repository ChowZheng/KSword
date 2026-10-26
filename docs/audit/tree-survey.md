# 树控件性能普查

普查对象：`E:\Projects\Ksword5.1\KswordARKLight\`（Win32 轻量外壳）全部 `WC_TREEVIEWW` 实例。
普查方式：全仓库 grep `WC_TREEVIEWW` / `CreateTreeView` / `TreeView_InsertItem` / `TVM_INSERTITEM` / `TVN_ITEMEXPANDING` / `TVN_ITEMEXPANDED` / `TVS_HASBUTTONS` / `SysTreeView32` / `ICC_TREEVIEW_CLASSES`，再逐个打开命中的创建点与插入点读代码。
性质：**只读审计，未修改任何源码。**

> 普查基线：`a787bdb3 refactor(kernel): 抽离内核值辅助函数到 KernelValueSupport.h/.cpp`（当前 HEAD）。
> 树相关演进链：`4c2c4d98` 新增 `Ui/LazyTreeView` → `969604fc` 注册表树迁到封装 → `a74eba1a` 硬件设备树迁到封装 → `a787bdb3` 抽出 `KernelValueSupport.h/.cpp`（`KernelPage.cpp` 因此重排约 500 行，本报告涉及内核树的行号均已在该基线重新定位）。
> 注意一：普查过程中 `RegistryView.cpp`、`HardwareView.cpp`、`Ui/LazyTreeView.*` 都被并行改写（两页先后迁到 `LazyTreeView`），本表记录的是**当前基线的状态**，不是初次 grep 时的状态。
> 注意二：`Ui/LazyTreeView` 的 `ChildSource`（同步取子节点的回调）与 `installChildren` 是 `a74eba1a` 引入的，与"树是内存模型投影"的页面形态完全吻合。

### 本报告的引用校验方式

全文 **133 处唯一 `文件:行` 引用**均经脚本全量校验（非抽样），七道检查：

1. **存在性 + 越界**：文件存在，且行号区间未超出文件实际行数 —— 133/133 通过。
2. **锚点行非噪声**：引用的**起始行不得是空行 / 纯注释 / 孤括号**（这类行号存在性校验查不出，但语义已失效）。命中 4 条，**全部是有意引用契约注释**，且上下文已明写，不会被误读：`KernelPage.cpp:5396-5397`（"明文写着"的删除禁令）、`Ui/LazyTreeView.h:32-35`（"头文件注释原文"）、`Ui/LazyTreeView.h:37-48`（"四条硬规则"）、`Ui/TreeViewUtil.h:15`（数量核实表里说明"第 4 处匹配是注释"）。
3. **内容语义指向**：引用的那一行/那一段确实包含该条所述的符号或语句。
4. **旧行号残留扫描**：审计期间经历 6 个新提交与队友并行改动导致的行号漂移，均已重定位并复核。
5. **函数归属比对**：把「`函数名`（`文件:行`）」形态的断言与「该行号落在哪个函数体内」逐条比对 —— 存在性、越界、非噪声、语义四道都查不出"行号落在邻近的另一个函数里"。
6. **定义处 vs 调用点**：该行号是**函数的定义行**，还是**函数体内的某条语句 / 某次调用**？
7. **类型 / 常量 / 类 / 成员**：该引用**根本不是函数**（Win32 宏、`class`/`struct` 定义、`constexpr` 常量、成员声明、Win32 结构体字段）——必须显式写明它不是函数，否则读者按函数去找会扑空。

**第 5 道检查抓到 2 处真实错误**（不悄悄改掉，记在这里供复核者看到"这里曾经错在哪、现在指哪"）：

| 引用 | 原写的归属 | 实际归属 | 状态 |
|---|---|---|---|
| `KernelPage.cpp:5840` | `ApplyCachedValueProjection` | `RequestResultSort`（定义于 `:5785`） | 已改 |
| `KernelPage.cpp:4841-4843` | `ApplyCachedValueProjection` | `RenderResult`（定义于 `:4840`） | 已改 |

两处是**完全同型**：行号在文件范围内、不是噪声、该行内容语义也对得上（都是 `ScopedListViewRedrawLock` 构造），只是落在邻近的另一个函数里。**前三道检查 100% 通过。**

6. **定义处 vs 调用点**：`函数名`（`文件:行`）默认读作**定义处**；若该行其实是**调用点**，读者按定义去找就会找不到。这一道与第 5 道**完全正交**——第 5 道查"行号落在哪个函数"，第 6 道查"行号是该函数的定义行还是函数体内的某条语句/某次调用"。

**第 6 道检查抓到 1 处真实错误 + 25 处形态歧义**：

| 引用 | 原写法 | 实际 | 处理 |
|---|---|---|---|
| `KernelPage.cpp:7073` | `RenderCallbackLocalModel`（`:7073`） | **调用点**（该行就是 `RenderCallbackLocalModel();`），定义在 `:7232` | 改为「**定义于** `:7232`，**调用点在 `:7073`**」 |
| `Ui/LazyTreeView.cpp:34` | `LazyTreeView::create`（`:34`） | 定义在 `:32`，`:34` 是体内转调 `CreateTreeView` 的语句 | 改为「**定义于** `:32`，转调语句在 `:34`」 |
| `Ui/LazyTreeView.cpp:276` | `LazyTreeView::reset`（`:276`） | 定义在 `:272`，`:276` 是体内 `ClearTreeView(hwnd_)` 调用 | 改为「**定义于** `:272`，调用语句在 `:276`」 |
| `Ui/LazyTreeView.cpp:459-464` / `:465-470` | `handleNotify`（同上） | 定义在 `:453`，两处分别是 `TVN_ITEMEXPANDINGW` / `TVN_DELETEITEMW` 分支 | 改为「**定义于** `:453`，分支在 `:459-464` / `:465-470`」 |
| `HardwareView.cpp:384-394` | `PopulateTree`（同上） | 定义在 `:370`，`:384-394` 是筛选态展开所有根的代码段 | 改为「**定义于** `:370`，代码段在 `:384-394`」 |
| `KernelPage.cpp:4089` / `:4841-4843` / `:5234` / `:5840` | 各重绘锁取锁语句 | 分别是 `RestoreReportListRows`（`:4082`）/ `RenderResult`（`:4840`）/ `ApplyCachedValueProjection`（`:5191`）/ `RequestResultSort`（`:5785`）的**体内语句** | 统一改为「**定义于** `:NNNN`，取锁语句在 `:NNNN`」 |
| 12 处调用点表格 / §6.5 对照表 | `函数名` `:NNNN` | 均为**定义行**，但形态与"调用点"混用，读者无从判断 | 统一加「**定义于**」前缀 |

> **本报告的引用约定**（与相邻审计一致）：
> - 指**定义**：写「**定义于** `:NNNN`」；
> - 指**调用**：写「**调用点** `:NNNN`」；
> - 指**函数体内的某条语句**：写「**定义于** `:NNNN`，……语句在 `:NNNN`」；
> - **裸行号**（无函数名）只用于**语句行**（如 `::SetTimer(...)` 所在行、控件创建语句），且上下文必须自明；
> - 指**类型 / 常量 / 类 / 成员**：必须**显式写明它不是函数**（见下方第 7 类的说明）。
>
> **第 5 类引用：类型 / 常量 / 类 / 成员**
>
> `类名`（`文件:行`）这个形态默认读作**函数定义**。但树侧报告里有大量**根本不是函数**的引用，读者按函数去找会扑空。已逐条标注：
>
> | 引用 | 实际 | 标注 |
> |---|---|---|
> | `ScopedWindowRedrawLock`（`Ui/ListViewUtil.h:40`）×3 处 | 类定义 | 「**类定义**」 |
> | `ScopedListViewRedrawLock`（`Ui/ListViewUtil.h:57`） | 类定义 | 「**类定义**」 |
> | `KernelObjectNamespaceTreeNodeState`（`KernelPage.h:26`） | 结构体定义 | 「**结构体定义**」 |
> | `ObjectTreeProjectionCommit`（`KernelPage.h:320`） | 结构体 | 「**结构体**」 |
> | `TreeRow`（`HardwareView.cpp:252` 附近） | 类型 | 「**类型**」 |
> | `itemByKey_`（`Ui/LazyTreeView.h:186`）/ `refreshTask`（`HardwareView.cpp:91`）/ `childSource_`（`Ui/LazyTreeView.cpp:432`） | 成员声明 | 「**成员声明**」/「**成员**」 |
> | `lParam`（`Ui/LazyTreeView.cpp:131`） | Win32 结构体字段 | 「**Win32 结构体字段**」 |
> | `TVM_INSERTITEM` / `TVIF_CHILDREN` / `WM_TIMER` / `EN_CHANGE` | Win32 宏 | 「**Win32 宏**」 |
>
> 共 **14 处**（去重后），全部已显式标注，不与函数引用混用。


> 实现要点（避免复现同类误判）：
> - 顶层定义正则要**排除关键字开头**（`if`/`for`/`while`/`switch`/`catch`/`return`/`else`/`do`），否则 `if (xxx) {` 会被当成函数定义；
> - **正则要允许返回类型前缀**（`HTREEITEM LazyTreeView::insertBatch(...)`、`std::vector<...> void Foo(`）——不收紧的话顶层定义一个都匹配不上（`Ui/LazyTreeView.cpp` 里大量函数是这种形态）；
> - 函数区间 = **本定义行 → 下一个顶层定义行 − 1**，不能按"行首非缩进"切（函数体内部有顶格写的语句，按缩进切会把区间切碎）；
> - 归属判定用「报告写的函数名 == 实际 owner 名 **或** owner 的末段 == 报告写的名」，避免 `Class::Method` 与 `Method` 两种写法互相不认；
> - **只对强断言形态生效**（`// Class::Method  (File.cpp:NNN)` 代码块头注释、`` `Class::Method`（`File.cpp:NNN`）`` 函数名带 `::` 且紧邻行号）。把 `setRows`/`push_back`/`std::move` 这类**API 调用名**也当成归属断言会产生大量误报（本报告首版即如此，29 条全误报）。
> - **宏常量需白名单**：`EN_CHANGE` / `WM_COMMAND` / `TVM_INSERTITEM` / `TVIF_CHILDREN` / `WM_TIMER` 是 Win32 宏，不是函数，不参与归属比对。
> - **需排除"元说明"形态**：本节上方表格里「原写作「`X`（`File.cpp:NNN`）」」是在**举例说明旧写法**，不是断言，不应被当成归属不匹配。
> - **成员变量名不是函数**：`propertyList_` / `summaryList_` / `callbackGroupList_` 这类 `HWND` 成员出现在「`名字` `:NNNN`」形态里时是**控件创建语句**，不参与函数归属比对。**判定成员不能只靠 `name.endswith("_")`** —— `refreshTask` / `childSource_` 这类 camelCase 成员同样要排除；且要核**该行是不是声明处**，本报告曾把 `refreshTask` 标成 `HardwareView.cpp:549`（实为 `BeginDeviceRefresh` 的定义行），真正的成员声明在 `:91`。
> - **无文件名的引用需由上下文推断文件**：表格内 `` `函数名` `:NNNN` `` 形态没有文件名，脚本需从同一行最近出现的 `file:NNN` 推断，否则会整批漏检（本报告脚本首版即栽在这里）。
> - **数量与行号一样会失效，引用数量必须重跑 grep，不能凭印象**：本报告此前只校验行号、把数量当固定事实。已逐条重跑核实（见下表）。**基线声明本身也是"数量类事实"** —— 本报告的基线原写 `a74eba1a`，而 HEAD 已到 `a787bdb3`（该提交重排 `KernelPage.cpp` 约 500 行），同样已修正。
>
> **本报告的数量类陈述全量核实结果**（59 处数量类陈述，逐条重跑 grep）：
>
> | 数量陈述 | 核实 |
> |---|---|
> | 4 个 `WC_TREEVIEWW` 实例 | ✓ `KernelPage.cpp:2158` / `KernelPage.cpp:5351` / `TreeViewUtil.cpp:14` + `LazyTreeView` 经 `CreateTreeView` 转调 |
> | 3 处 `TVM_INSERTITEM` 代码站点 | ✓ `KernelPage.cpp:5425` / `LazyTreeView.cpp:134` / `TreeViewUtil.cpp:59`（第 4 处匹配是 `TreeViewUtil.h:15` 的注释） |
> | 内核树 `SW_HIDE` 10 处 | ✓ |
> | 针对该树的 `SW_SHOW` 0 处 | ✓（全文件 157 处 `SW_SHOW` 均不作用于该树） |
> | `MoveWindow(objectNamespaceTree_, ...)` 3 处 | ✓ |
> | 全仓库 17 处重绘锁用法 | ✓ |
> | `RenderCallbackLocalModel` 12 处调用点 | ✓（14 处匹配 = 1 定义 + 1 注释 + 12 调用） |
> | 内核页 4 处批量重建套了重绘锁 | ✓ `:4089` / `:4841-4843` / `:5234` / `:5840` |
> | UI 线程 key 拼接 2 次 | ✓（同一处代码 `KernelPage.cpp:5150` 被选中项与首可见项两个场景调用） |
> | 审计期间 6 个新提交 | ✓ `a44cbf6f..HEAD` 恰为 6 个 |
> | 14 处非函数实体标注 | ✓（脚本去重后统计；此前我按"标注词出现次数"估成 19，**数量未去重**，已修正） |
>
> ⚠️ **行号会漂移，锚点必须能自验证**：本报告的关键条目均带「下一个函数起于 `:NNNN`」这类锚点（如 `PumpObjectTreeProjection` 为 `:5378-5438`，下一函数 `RebuildObjectNamespaceTreeFromCurrentRows` 起于 `:5440`），可在代码变化后自行重新定位。**若某条引用的起始行变成空行或与描述不符，即为失效，请按函数名重新定位而不是照抄行号。**



## 一句话结论

ARKLight 一共只有 **4 个真正的树控件实例**，分布在 3 个文件：

- **注册表树**（`RegistryView.cpp`）：已迁到 `LazyTreeView`，有批量上限 + 单父上限，**判定 OK**。
- **硬件设备树**（`HardwareView.cpp`）：同样已迁到 `LazyTreeView`，**插入路径本身达标**；残留风险在**筛选重建路径**（整树单消息递归删除 + 筛选态展开所有根），**判定风险**。
- **内核对象命名空间树**（`KernelPage.cpp`）：**同一个 HWND 被复用两次创建**，插入是**定时器分片 + 每片 4ms 预算**，**判定 OK**；但该树**永久隐藏**，迁移收益为负。
- **`Ui/LazyTreeView`** 本身是通用封装，现在**注册表页与硬件页都在用**。

**严重（无上限且同步全量插入）= 0。** 全仓库已无"一次性把整棵子树插进去"的写法。

## 汇总表

| # | 文件:行 | 用途 | 数据来源 | 插入时机 | 上限 | 懒加载 | 让出消息 | 判定 |
|---|---------|------|----------|----------|------|--------|----------|------|
| 1 | `Features/Registry/RegistryView.cpp:1215` | 注册表键树 | `EnumerateRegistrySubKeyNames`（后台 `treeChildrenTask` 线程，`RegistryView.cpp:419`） | 展开时（`TVN_ITEMEXPANDINGW` → `LazyTreeView::onExpanding` → `RequestTreeChildren` → 回调 `ReplaceTreeNodeChildren`） | **有**：`kMaxRowsPerParent = 5000`（单父）；批量 `kBatchSize = 400` | 有 | **有**（分批 + `WM_SETREDRAW(FALSE)`） | **OK** |
| 2 | `Features/Hardware/HardwareView.cpp:715` | 设备管理器树 | `ChildRowsOf`（`HardwareView.cpp:266`，**UI 线程同步**，但设备模型已在内存里） | 展开时（`onExpanding` → `childSource_` → `installChildren` → `insertBatch` ≤400 行）；筛选重建时另走 `setRoots` | **有**：`kMaxRowsPerParent = 5000`（单父）；批量 `kBatchSize = 400` | 有 | **有**（分批 + `WM_SETREDRAW(FALSE)`） | **风险**（残留在筛选重建路径，见 §2） |
| 3 | `Features/Kernel/KernelPage.cpp:2157` | 内核对象命名空间树（初始实例） | `KernelValueProjectionBuilder`（后台 `cachedProjectionTask_` 线程，`KernelPage.cpp:5183`） | 查询完成后投影重建 | **有**：`kMaxDirectoryRows = 2500`（后端行数上限） | 无（整棵树投影） | **有**（16ms 定时器 + 每次 ≤4ms + ≤128 工作单元） | **OK** |
| 4 | `Features/Kernel/KernelPage.cpp:5351` | 内核树（按 featureId 保留的实例） | 同上（`objectNamespaceTrees_[featureId]` 缓存） | 同上 | 同上 | 同上 | 同上 | **OK** |
| 5 | `Ui/LazyTreeView.h:56` / `Ui/LazyTreeView.cpp:32` | 通用惰性树封装（**非独立控件实例**，`create()` 内部转调 `CreateTreeView`） | 由调用方提供 | 由调用方调 `setRoots` / `setChildren` | **有**：`kBatchSize=400`、`kMaxRowsPerParent=5000` | 有 | 有 | **OK** |
| 6 | `Ui/TreeViewUtil.cpp:7` | 通用建树 helper（`CreateTreeView`） | 无 | 无 | 无 | 无 | 无 | **OK**（只是 helper，不是树实例） |
| 7 | `Features/Kernel/KernelPage.cpp:2157` 与 `5351` | **同一语义的两处创建**，第二处只在切换 featureId 时按需补建 | — | — | — | — | — | **附注**（同一 HWND 复用 + 永久隐藏，见 §5.4 / §5.5） |
| 8 | `Ui/LazyTreeView.cpp:272` → `Ui/TreeViewUtil.cpp:66` | **跨树共性**：`reset()` 用 `TreeView_DeleteItem(TVI_ROOT)` 在单条消息里递归删掉整棵已展开的树 | — | 树重建时（`setRoots` → `reset()`） | **无**（已展开节点数无上限） | — | **无** | **风险**（见 §6.1） |

> 表里第 3、4、7 行是同一个语义实体的三个面：第 3 行是"首次创建"，第 4 行是"按 featureId 补建"，第 7 行标注二者共用同一个成员指针。

### 明确排除（grep 命中但**不是树控件**）

| 位置 | 实际是什么 |
|---|---|
| `Features/Process/ProcessView.cpp`（`kTreeIndentPixels` / `DrawDottedTreeGuides`） | ListView 里的**画出来的缩进虚线**，不是 TreeView。表格本体是 `LVS_OWNERDATA` 虚拟表 |
| `Features/Process/ProcessModel.cpp:appendTreeRows` | 进程父子关系的**行分组**，最终仍落在虚拟 ListView 里 |
| `Features/Window/WindowView.cpp` / `Features/WindowTools/*` / `Features/Handle/HandlePage.cpp` / `Features/Startup/StartupView.cpp` / `Features/Service/ServiceView.cpp` / `Features/File/FileView.cpp` / `Features/File/FileFeature.cpp` | 全部是 `WC_LISTVIEWW` + `LVS_OWNERDATA` 虚拟表，**没有任何树控件**。grep `WC_TREEVIEWW` 在这些文件里 0 命中 |
| `Features/Driver/*` / `Features/Privilege/*` / `Features/Memory/*` / `Features/Network/*` / `Features/Misc/*` / `Features/SysTools/*` / `Features/Monitor/*` / `Features/HardwareStats/*` / `Features/NetTools/*` / `Features/ProcessDetail/*` | 同上，`WC_TREEVIEWW` 0 命中 |
| `Ui/EntityNavigation.cpp` | 只是 `WM_APP+104` 的跨页导航消息转发，与树无关 |
| `Ui/Controls.cpp`（`ICC_TREEVIEW_CLASSES`） | `InitCommonControlsEx` 的类注册标志，不是树实例 |
| `Features/Kernel/KernelPage.cpp:2390` / `:2410` / `:2435` / `:2475`（内核回调拦截面板的 4 个列表） | 全部 `WC_LISTVIEWW` + `LVS_REPORT`，**无 `LVS_OWNERDATA`**、**无树**。`callbackGroupList_`、每类型的 `ruleList`、`callbackBypassList_`、`callbackFileMonitorList_`。该面板的重建路径（`PopulateCallbackInterceptPanel` `:7052` → `RenderCallbackLocalModel` `:7232`）是 ListView 口径，归 UI 线程拷贝审计，**不属于本报告**。附带一条事实（同样非树）：该路径**是内核页唯一没有重绘抑制的批量插入路径**，且 `RenderCallbackLocalModel` 有 **12 处调用点**、全是规则增删改的用户交互，详见 §6.5 |

---

## 逐个说明

### 1. 注册表树（RegistryView.cpp）— 判定 OK

- **创建点**：`Features/Registry/RegistryView.cpp:1215`
  ```cpp
  const bool treeCreated = state.tree.create(state.hwnd, kTreeId, 0, 0, 0, 0);
  ```
  `state.tree` 是 `Ksword::Ui::LazyTreeView`（`RegistryView.cpp:130` 声明），`create()`（`Ui/LazyTreeView.cpp:32`）内部转调 `Ui/TreeViewUtil.cpp:7` 的 `CreateTreeView`，真正 `CreateWindowExW(WC_TREEVIEWW, ...)` 在 `Ui/TreeViewUtil.cpp:12`。

- **数据来源**：`EnumerateRegistrySubKeyNames(path, mode, &statusText)`，跑在 `treeChildrenTask`（`AsyncSnapshotTask<RegistryTreeChildrenSnapshot>`）的工作线程上，`RegistryView.cpp:413-419`：
  ```cpp
  state.treeChildrenTask->request(
      [path, mode, query, useRegex] {
          RegistryTreeChildrenSnapshot snapshot{};
          snapshot.path = path;
          snapshot.mode = mode;
          snapshot.query = query;
          snapshot.subKeys = EnumerateRegistrySubKeyNames(path, mode, &snapshot.statusText);
  ```
  **枚举本身在后台线程**，UI 线程不阻塞。

- **插入时机**：展开时。`TVN_ITEMEXPANDINGW` 由 `LazyTreeView::onExpanding` 自己消费（`Ui/LazyTreeView.cpp:407`），页面只提供回调（`RegistryView.cpp:1212-1214`）：
  ```cpp
  state.tree.setExpansionCallback([&state](HTREEITEM item, const std::wstring& key) {
      return RequestTreeChildren(state, item, key);
  });
  ```
  数据落地后 `ReplaceTreeNodeChildren`（`RegistryView.cpp:348-366`）把子键名转成 `TreeRow` 列表交给 `state.tree.setChildren(item, std::move(children))`（`RegistryView.cpp:364`）。**页面不再直接碰 `TreeView_InsertItem`。**

- **上限**：**有**。`Ui/LazyTreeView.h:56-59`
  ```cpp
  static constexpr int kBatchSize = 400;
  static constexpr int kMaxRowsPerParent = 5000;
  ```
  触到 5000 时插一行截断提示（`Ui/LazyTreeView.cpp:166-181`），文本形如「… 已截断：只显示前 N / 共 M 项，请用筛选框缩小范围」，**不是静默丢弃**。

- **懒加载**：**有**。子节点只在用户展开时枚举；未展开的节点不占用任何枚举开销。

- **让出消息**：**有（分批）**，但要分清两件事：
  - **分批是真的让出**：`LazyTreeView::insertBatch`（`Ui/LazyTreeView.cpp:202`）单次最多插 `kBatchSize=400` 行（`Ui/LazyTreeView.cpp:213-215`），剩下的挂在末尾「载入更多」合成行上，**用户点它才续插下一批** —— 每一批之间消息泵真的转了。
  - **重绘抑制不是让出**：`beginUpdate()/endUpdate()`（`Ui/LazyTreeView.cpp:57-76`）只是 `WM_SETREDRAW(FALSE)` + 末尾一次 `InvalidateRect`。它压掉的是**绘制**，**不压条目记账** —— 每次 `TVM_INSERTITEM`（**Win32 宏**，`Ui/LazyTreeView.cpp:134` 的 `TVM_INSERTITEMW` 调用）内部的控件侧记账（维护项链表、重算可见项、滚动条范围）照样同步发生。

  ```cpp
  while (parentNode.cursor < parentNode.pending.size()
         && parentNode.shown < kMaxRowsPerParent
         && added < kBatchSize) {
  ```

- **额外确认（曾存在、现已修掉的两条）**：
  - 树筛选框的 **每键全树重建**：现在 `EN_CHANGE`（**Win32 宏**）只记 query 并起 250ms 防抖定时器（`RegistryView.cpp:1400-1406`，`kTreeFilterDebounceMs = 250`，`RegistryView.cpp:50`），真正重建推迟到 `WM_TIMER`（`RegistryView.cpp:1377-1383`）。
  - **递归全树扫描找节点**：现在走 `state.tree.findItem(path)`，`LazyTreeView` 内部维护 `itemByKey_` 哈希索引（**成员声明** `Ui/LazyTreeView.h:186`），O(1)。

- **判 OK 依据**：上限、批量、重绘抑制、懒加载、哈希索引、防抖，六项齐备。
- **残留**：重建路径的整树删除，见 §6.1（与硬件页同一条共性问题）。

---

### 2. 硬件设备树（HardwareView.cpp）— 判定 风险

> 初次普查时这一行是"裸 TreeView + 展开时一次插完该层全部子节点"。`a74eba1a` 已把它迁到 `LazyTreeView`，**插入路径的问题已经解决**；下面按当前基线重写。

- **创建点**：`Features/Hardware/HardwareView.cpp:715`
  ```cpp
  const bool treeReady = state->tree.create(hwnd, kTreeId, 0, 0, 100, 100);
  ```
  `state->tree` 是 `Ksword::Ui::LazyTreeView`（`HardwareView.cpp:82`），同样走 `Ui/TreeViewUtil.cpp:7` 的 `CreateTreeView`，样式与注册表树一致。

- **数据来源**：**UI 线程同步**，但数据本来就在内存里。`HardwareView.cpp:712-714`：
  ```cpp
  state->tree.setChildSource([state](HTREEITEM, const std::wstring& key) {
      return ChildRowsOf(*state, static_cast<int>(std::wcstol(key.c_str(), nullptr, 10)));
  });
  ```
  `ChildRowsOf`（`HardwareView.cpp:266-280`）从 `state.model` 取该节点的 `childIndices`，逐个翻成 `TreeRow`（**类型**，`MakeDeviceRow` **定义于** `HardwareView.cpp:252`）。
  底层设备枚举（`EnumerateDeviceManagerTree`）本身跑在后台 `refreshTask` 线程（`refreshTask` 是 `HardwareViewState` 的**成员**，声明于 `HardwareView.cpp:91`；后台任务在 `BeginDeviceRefresh`（**定义于** `HardwareView.cpp:549`）里发起），SetupAPI 循环硬上界 `for (DWORD ordinal = 0;ordinal<100000;++ordinal)`（`shared/usermode/backend/hardware/HardwareEnumerator.cpp:353`），触顶置 `result.limited = true`（同文件 `:368`）。

- **插入时机**：展开时，`LazyTreeView::onExpanding`（`Ui/LazyTreeView.cpp:407`）走 `childSource_` 分支（**成员** `childSource_`，分支语句在 `Ui/LazyTreeView.cpp:432-438`）：
  ```cpp
  if (childSource_) {
      installChildren(item, *node, childSource_(item, node->key), false, false);
      return FALSE;
  }
  ```
  `installChildren`（`Ui/LazyTreeView.cpp:296`）→ `insertBatch`（`:202`），**单次展开最多插 400 行**，其余挂「载入更多」行，展开本身在同一条消息里正常走完（不取消、不往返、无空白分支）。

- **上限**：**有**（同 §1：`kMaxRowsPerParent = 5000` / `kBatchSize = 400`）。

- **懒加载**：**有**，且按层。

- **让出消息**：**有（分批）**。同 §1：单次展开插 ≤400 行，其余挂「载入更多」行由用户点开；但 `WM_SETREDRAW(FALSE)` 只压绘制、不压记账（§1「让出消息」条目已展开说明）。

- **已修掉的两条旧问题**（`a74eba1a` 提交说明里列的就是这两条）：
  - `InsertTreeNode` / `PopulateTreeItemChildren` 已删除，展开不再"一次插完该层全部子节点"。
  - `FindTreeItemByDeviceIndex`（递归全树扫描 + 每节点一次 `TVM_GETITEM`）已删除。`EnsureTreeItemVisible`（`HardwareView.cpp:436`）改为逐层 `TreeView_Expand` + `findItem`（**方法**，定义于 `Ui/LazyTreeView.cpp:353`）命中（调用点 `HardwareView.cpp:455-467`），**O(1) 哈希查找**。

- **判定 风险的三个理由（全部落在筛选重建路径上）**：
  1. **筛选无防抖**：`WM_COMMAND` 的 `EN_CHANGE`（**Win32 宏**，分支判断在 `HardwareView.cpp:760`）**直接调用** `RequestHardwareFilter`（定义于 `HardwareView.cpp:496`，调用点 `:761`），没有注册表页那样的 250ms 定时器（对比 `RegistryView.cpp:1400-1406` 的 `::SetTimer(state->hwnd, kTreeFilterTimerId, kTreeFilterDebounceMs, nullptr)` 在 `:1404`）。
  2. **筛选落地 = 整树重建**：`ApplyHardwareFilter`（`HardwareView.cpp:471`）→ `PopulateTree`（`:477`）→ `state->tree.setRoots(...)`（`:383`）→ `reset()` → `TreeView_DeleteItem(TVI_ROOT)`，**单条消息内递归删掉整棵已展开的树**，再重插根。已展开节点数没有上限（见 §6.1）。
  3. **筛选态展开所有根**：`PopulateTree`（**定义于** `HardwareView.cpp:370`，其中展开所有根的代码段在 `:384-394`）：
     ```cpp
     if (!state->filterQuery.empty()) {
         const HWND treeHwnd = state->tree.hwnd();
         HTREEITEM root = TreeView_GetRoot(treeHwnd);
         while (root) {
             const HTREEITEM next = TreeView_GetNextSibling(treeHwnd, root);
             TreeView_Expand(treeHwnd, root, TVE_EXPAND);
             root = next;
         }
     }
     ```
     每个根的展开都会同步取子节点并插 ≤400 行。根数（顶层设备类别）没有全局上限，于是**单条消息内的插入量 = 根数 × 400**，这正是"无全局上限的同步批量插入"。

- **这条路径的耗时是同步且不可让出的（重要）**：`setChildSource` 的插入发生在 **`TVN_ITEMEXPANDINGW` 通知内部** —— `handleNotify`（**定义于** `Ui/LazyTreeView.cpp:453`，其中处理 `TVN_ITEMEXPANDINGW` 的分支在 `:459-464`）把 `TVN_ITEMEXPANDINGW` 转给 `onExpanding`（**定义于** `:407`），`onExpanding` 的 `childSource_` 分支（`:432-438`）当场调 `installChildren(item, *node, childSource_(item, node->key), false, false)`，插入完成才 `return FALSE` 放行展开。
  所以：
  - 全部记账（`TVM_INSERTITEM` 的控件侧开销）**发生在同一条消息、同一个展开通知里**，`WM_SETREDRAW(FALSE)` **只遮住了绘制观感，不遮记账耗时**；
  - `installChildren` 传的 `expand=false`（`Ui/LazyTreeView.cpp:296-323` 的最后一个参数）避免了二次嵌套展开，但**没有把插入挪出这条消息**；
  - 结论：**H3 的插入是同步不可让出的**。分批机制在这里帮不上忙 —— `kBatchSize` 只限制"一个父节点一次插多少"，而筛选态是**多个父节点各插一批**，全部挤在同一条消息里。

- **缓解因素**（所以是风险不是严重）：筛选请求走后台线程 + `AsyncSnapshotTask` 的**请求合并**（新请求覆盖旧请求），连续按键不会堆积成 N 次重建，最多落地一次；且筛选态只展开**第一层**，更深层仍然是懒的。

- **判 OK 依据不成立的原因**：单父上限与批量上限都只作用在"一个父节点"上，**筛选重建这条路径整体不在这两个上限的覆盖范围内**；且插入落在展开通知内，同步不可让出。

---

### 3. 内核对象命名空间树（KernelPage.cpp）— 判定 OK

- **创建点（两处）**：
  - `Features/Kernel/KernelPage.cpp:2157-2168`（`KernelPage::Create` 里，建页面时先建一个）：
    ```cpp
    objectNamespaceTree_ = ::CreateWindowExW(WS_EX_CLIENTEDGE,
        WC_TREEVIEWW,
        L"",
        WS_CHILD | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS,
        0, 0, 0, 0, hwnd_, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    ```
  - `Features/Kernel/KernelPage.cpp:5351-5353`（`SelectObjectNamespaceTree` 里，切换到某个 featureId 时按需补建）：
    ```cpp
    retained.hwnd = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
        WS_CHILD | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS,
        0, 0, 0, 0, hwnd_, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    ```

- **数据来源**：`KernelValueProjectionBuilder(input).build(requestId)`，跑在后台 `cachedProjectionTask_`（`AsyncSnapshotTask<std::shared_ptr<KernelValueProjection>>`）线程（`KernelPage.cpp:5183`）。树节点数据在后台投影成 `projection->treeNodes`（`Features/Kernel/KernelProjectionValues.inl` 的 `RebuildObjectNamespaceTreeFromCurrentRows`）。

- **插入时机**：**查询完成后投影重建**。不是懒加载——整棵三层树一次性投影出来。

- **上限**：**有**。后端行数上限 `kMaxDirectoryRows = 2500`（`shared/usermode/backend/kernel/ObjectDirectory.h:13`）：
  ```cpp
  constexpr std::size_t kMaxDirectoryRows = 2500;
  ```
  `RecursiveDirectoryOptions` 的 `maxDepth=4, maxRows=2500, maxScannedRows=10000`（同文件 `:16`）；单目录枚举 `DirectoryQueryOptions::maxEntries=100000, maxDurationMs=8000`（`shared/usermode/backend/kernel/ObjectNamespace.h:167`）。触顶时明确提示"目录递归达到显示上限 2500 行，已截断"。

- **懒加载**：**无**。整棵树投影。

- **让出消息**：**有，而且是全部树里做得最扎实的**。`KernelPage::PumpObjectTreeProjection`（`KernelPage.cpp:5378`）由 16ms 定时器驱动（`kTimerObjectTreeProjection = 51091`，`KernelPage.cpp:93`），每片最多 4ms 墙钟 + 128 工作单元：
  ```cpp
  while(commit->next<projection->treeNodes.size() && work+2<=128 && ::GetTickCount64()-started<4) {
      const auto index=commit->next;const auto& node=projection->treeNodes[index];
      TVINSERTSTRUCTW insert{};insert.hInsertAfter=TVI_LAST;
      ...
      const auto item=TreeView_InsertItem(objectNamespaceTree_,&insert);++work;
  ```
  旧树的删除也走同样的分片（`KernelPage.cpp:5398-5408`，逐叶删除，`work+4<=128` 且 <4ms），注释明说：
  ```cpp
  // Delete only leaves. A single TVM_DELETEITEM must never recursively
  // erase an unbounded old subtree in one UI callback.
  ```

- **判 OK 依据**：后端有 2500 行硬上限 + 明确截断提示；插入和删除都做了 4ms 分片，不会出现一次性长阻塞。

- **重要结论：这棵树永久隐藏，迁移收益为负**。全文件核过：`::ShowWindow(objectNamespaceTree_, SW_HIDE)` 精确 **10 处**（`KernelPage.cpp:2644 / 2738 / 2786 / 2841 / 2887 / 2934 / 2979 / 3036 / 3082 / 5341`），针对该树的 `SW_SHOW` **0 处**；创建时样式**没有 `WS_VISIBLE`**；`::MoveWindow(objectNamespaceTree_, 0, objectTop, 0, 0, TRUE)` **3 处**（`:2758 / 3121 / 3126`），即宽高被压成 0×0。内核页真正给用户看的是 `resultList_`（`resultList_` 才是可见的 "tree-table surface"）。
  也就是说这棵树目前是**后台数据模型的可视化镜像，但用户看不到**——它仍在承担全部插入开销。因此：
  - **迁移到 `LazyTreeView` 的收益是负的**：它本来就不阻塞（4ms 分片），迁移只改变节点记录归属，不产生性能收益；
  - **正确动作是评估停建**（连同 `objectNamespaceTrees_` 缓存一起下线），而不是"为了统一而统一"。

---

### 4. Ui/LazyTreeView（通用封装）— 判定 OK

- **文件**：`Ui/LazyTreeView.h`（193 行）/ `Ui/LazyTreeView.cpp`（519 行）
- **创建点**：`Ui/LazyTreeView.cpp:32` `create()` → 内部调 `Ui/TreeViewUtil.cpp:7` 的 `CreateTreeView`。**它本身不是一个独立的树实例**，是给页面用的封装。

- **它解决的问题**（头文件注释原文，`Ui/LazyTreeView.h:32-35`）：
  > "It exists because a plain TreeView has no owner-data mode: every node is a real window item, so a page that inserts a whole snapshot in one loop blocks the UI thread for the duration of that loop."

- **四条硬规则**（`Ui/LazyTreeView.h:37-48`）：
  1. 插入分批：`kBatchSize = 400`（`.h:56`），余量挂「载入更多」行。
  2. 单父上限：`kMaxRowsPerParent = 5000`（`.h:59`），触顶插截断行，**不静默丢数据**。
  3. 记录归控件所有、按 `HTREEITEM` 索引、`key` 另有哈希索引（`.h:186`）→ 查找 O(1)、删除不漏。
  4. 每批插入包在 `WM_SETREDRAW(FALSE)` + 末尾一次 `InvalidateRect` 里（`.cpp:57-76`）。

- **两种取数模式**（`.h:66` 与 `.h:78`）：
  - `ExpansionCallback`（异步后端）：返回 true 表示请求已受理，控件保持折叠，等 `setChildren()` 落地再展开——"so the user never stares at an empty branch"。注册表页用这个。
  - `ChildSource`（内存模型投影）：展开那一刻同步取子节点，第一批就地插入，同一条消息里让展开正常走完（`.cpp:432-438`）。硬件页用这个。

- **上限**：有（400 / 5000）。
- **懒加载**：有。
- **让出消息**：有（分批 + 重绘抑制）。
- **判 OK 依据**：机制完备。

- **接入现状**：
  | 页面 | 是否接入 LazyTreeView |
  |---|---|
  | `Features/Registry/RegistryView.cpp` | **是**（`969604fc`） |
  | `Features/Hardware/HardwareView.cpp` | **是**（`a74eba1a`） |
  | `Features/Kernel/KernelPage.cpp` | **否**（用的是自己写的 4ms 分片泵） |

  `Ui/LazyTreeView.cpp` 已列进 `KswordARKLight.vcxproj`（第 243 行 ClCompile、第 473 行 ClInclude）。

- **一条必须记住的迁移约束**：封装把 `lParam`（**Win32 结构体字段**）**固定为 0**（赋值语句在 `Ui/LazyTreeView.cpp:131`），节点数据由控件按 `HTREEITEM` 记录并用 `userDataOf()` 取（`Ui/LazyTreeView.cpp:368`）。所以**迁移不是"换个控件"，而是"换取数链路"**：
  - 硬件页已经改完：`SelectedDeviceIndex` 走 `state->tree.userDataOf(selected)`（`HardwareView.cpp:223`），选中回调走 `state->tree.userDataOf(change->itemNew.hItem)`（`HardwareView.cpp:796`），`lParam` 不再承载数据。
  - 内核页仍靠 `lParam` 取数：`SelectObjectNamespaceTreeItem(changed->itemNew.lParam)`（`KernelPage.cpp:1944`）、`TreeView_GetItem` 取 `value.lParam`（`KernelPage.cpp:5150`）、插入时 `lParam=node.state.get()`（`KernelPage.cpp:5424`）。**这棵树要迁，必须先改 `userDataOf()` 链路。**
  - 内核树 `lParam` 承载的 payload 是 `KernelObjectNamespaceTreeNodeState`（**结构体定义** `KernelPage.h:26`），其稳定 key 由 `KernelTreeNodeKey` 生成（声明 `KernelValueSupport.h:224`，实现 `KernelValueSupport.cpp:451`，把 `nodeKind`/`nodeType`/`nodePath` 拼成小写串）。迁到 `LazyTreeView` 时，这套 payload + key 生成逻辑要整体搬到 `TreeRow::userData` / `TreeRow::key` 上 —— **这就是"换取数链路"具体要搬的东西**。

---

### 5. 其它核查项

#### 5.1 数量上限关键字全量搜索结果

搜索 `kMaxDisplayRows` / `5000` / `truncat` / `截断` / `上限` / `limit` / `Limit`，在 ARKLight 内命中的**树相关**上限只有这些：

| 上限常量 | 值 | 位置 | 作用对象 |
|---|---|---|---|
| `kMaxDisplayRows` | 5000 | `Ui/ListViewUtil.h:15` | **ListView** 行数上限（不是树） |
| `LazyTreeView::kBatchSize` | 400 | `Ui/LazyTreeView.h:56` | 树单次插入批大小（注册表树 + 硬件树共用） |
| `LazyTreeView::kMaxRowsPerParent` | 5000 | `Ui/LazyTreeView.h:59` | 树单父节点子节点上限（注册表树 + 硬件树共用） |
| `kMaxDirectoryRows` | 2500 | `shared/usermode/backend/kernel/ObjectDirectory.h:13` | 内核对象目录递归行数上限（喂给内核树） |
| `kMaxScannedRows` | 10000 | `shared/usermode/backend/kernel/ObjectDirectory.h:16` | 同上，扫描预算 |
| `DirectoryQueryOptions::maxEntries` | 100000 | `shared/usermode/backend/kernel/ObjectNamespace.h:167` | 内核命名空间单目录枚举上限（另有 `maxDurationMs=8000`） |

**"插空子节点换箭头"的做法全仓库不存在**：注册表树与硬件树走 `LazyTreeView`，用 `TVIF_CHILDREN`（**Win32 宏**，标志设置在 `Ui/LazyTreeView.cpp:121-126`）只给箭头不建占位子项；内核树是全量投影，插入 `TVIF_TEXT|TVIF_PARAM`（`KernelPage.cpp:5424`）且子节点本来就是真节点。没有历史包袱。

#### 5.2 让出消息关键字全量搜索结果

在 5 个树相关文件（`RegistryView.cpp` / `HardwareView.cpp` / `KernelPage.cpp` / `LazyTreeView.cpp` / `TreeViewUtil.cpp`）里搜 `PeekMessage` / `PostMessage` / `MsgWaitForMultipleObjects`：**0 命中**。

让出消息在这个项目里不是靠 `PeekMessage` 实现的，而是靠两种机制：
1. **分批 + 合成行**（`LazyTreeView`）：一次插 400 行，其余靠用户点「载入更多」推进。
2. **定时器分片**（`KernelPage::PumpObjectTreeProjection`）：16ms 定时器，每片 ≤4ms 墙钟 + ≤128 工作单元。

**必须区分"让出消息"与"重绘抑制"**——这两件事在本项目里常被混为一谈：

| 机制 | 压掉什么 | 没压掉什么 |
|---|---|---|
| `WM_SETREDRAW(FALSE)`（`LazyTreeView::beginUpdate/endUpdate`，`Ui/LazyTreeView.cpp:57-76`） | **绘制**：不做逐项重绘，末尾一次 `InvalidateRect` | **条目记账**：每次 `TVM_INSERTITEM`（**Win32 宏**，`Ui/LazyTreeView.cpp:134` 的 `TVM_INSERTITEMW` 调用）内部的控件侧开销（维护项链表、重算可见项、滚动条范围）照常同步发生 |
| 分批 + 合成行 | 一次消息内的插入**量** | 若多条目被塞进同一批（如硬件页筛选态展开所有根），这一批内部仍是同步的 |
| 定时器分片（内核树） | 每片 ≤4ms 墙钟，片间消息泵真的转 | — |

所以准确的说法是：**"有重绘抑制"不等于"有让出消息"**。硬件页筛选态那条路径（§6.2）就是"绘制被抑制、记账仍同步、且同步不可让出"的典型。

**一处必须说清的例外**：内核树的**插入/删除循环本身没有重绘抑制**。`PumpObjectTreeProjection`（`KernelPage.cpp:5378-5438`，下一个函数 `RebuildObjectNamespaceTreeFromCurrentRows` 起于 `:5440`）内含**真正的树插入**：

```cpp
// KernelPage.cpp:5425
const auto item=TreeView_InsertItem(objectNamespaceTree_,&insert);++work;
```

但该函数全范围内 `ScopedWindowRedrawLock` / `ScopedListViewRedrawLock` / `WM_SETREDRAW` **0 命中**。全仓库**唯一**对树控件取重绘锁的地方是 `KernelPage.cpp:4844` 的 `ScopedWindowRedrawLock objectTreeLock(objectNamespaceTree_)`——而它在 `RenderResult`（`:4840-4988`，下一个函数 `AddDynamicResultColumns` 起于 `:4990`）里，**该函数体内不操作这棵树**：整个 `:4840-4988` 区间内 `objectNamespaceTree_` **只出现 `:4844` 这一行**，属空转。

**即：树控件的重绘抑制事实上是「零覆盖」**——内核树的插入路径本身不取锁，而唯一那把取锁的并不在插入路径上。

这不影响内核树 OK 的判定，原因是**它本来就不可见**：`SW_HIDE` 10 处 / `SW_SHOW` 0 处 / 创建时无 `WS_VISIBLE` / `MoveWindow(...,0,0)` 3 处（§3 末尾）。隐藏控件不绘制，重绘抑制对它没有意义。**但结论要写全**：内核树的分片泵解决的是"记账阻塞"，不是"重绘闪烁"——后者因不可见而本来就不存在。

#### 5.3 `Ui/TreeViewUtil` 的四个 helper 现状

| helper | 位置 | 调用方 |
|---|---|---|
| `CreateTreeView` | `Ui/TreeViewUtil.cpp:7` | `LazyTreeView::create`（**定义于** `Ui/LazyTreeView.cpp:32`，其中转调 `CreateTreeView` 的语句在 `:34`）——注册表页与硬件页都经它 |
| `InsertTreeItem` | `Ui/TreeViewUtil.cpp:31` | **无调用方**（`grep -rn "InsertTreeItem"` 只命中定义与头文件注释）。它还是**旧接口**：`TVIF_TEXT \| TVIF_PARAM` 之外没有 `TVIF_CHILDREN`，用它插的节点拿不到展开箭头 |
| `ClearTreeView` | `Ui/TreeViewUtil.cpp:62` | `LazyTreeView::reset`（**定义于** `Ui/LazyTreeView.cpp:272`，其中 `ClearTreeView(hwnd_)` 调用语句在 `:276`）——**这是 §6.1 风险的来源** |
| `SetTreeViewImageList` | `Ui/TreeViewUtil.cpp:70` | **无调用方** |

#### 5.4 内核树的两处创建点

`KernelPage::SelectObjectNamespaceTree`（`KernelPage.cpp:5332` 起）里，`objectNamespaceTree_` 被**清空再重建**，按 featureId 各保留一份：
```cpp
auto& retained = objectNamespaceTrees_[featureId];
if (!retained.hwnd) {
    retained.hwnd = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"", ...);
    ...
}
objectNamespaceTree_ = retained.hwnd;
```
`objectNamespaceTrees_` 是 `std::unordered_map<KernelFeatureId, ObjectNamespaceTreeCache>`（`KernelPage.h:308`，结构体定义 `:303`）。因此**同一个语义最多会有 2 个同时存活的 TreeView HWND**（`ObjectNamespaceOverview` + `ObjectDirectoryRecursive` 各一个）。

由于两者都被 `MoveWindow(...,0,0)` 缩到 0×0 且 `SW_HIDE`，且每个 HWND 都配一份独立的 `ObjectTreeProjectionCommit`（**结构体**，`KernelPage.h:320`，`std::array<std::shared_ptr<ObjectTreeProjectionCommit>,2> treeProjectionCommits_`），**切换 featureId 时要各自维护一棵隐藏树的投影状态**。这不是性能 bug，但属于"同一份数据两套 UI 状态"的维护负担。

#### 5.5 内核树的"不可见"是结论，不是附带事实

见 §3 末尾。补充一条与 §4 联动的事实：**内核树不能直接迁到 `LazyTreeView`**，因为 `LazyTreeView::reset()` 走 `ClearTreeView` → `TreeView_DeleteItem(TVI_ROOT)`（单条消息递归删整棵树），正好撞上 `KernelPage.cpp:5396-5397` 那段注释明令禁止的事。内核树要"统一"，得先给封装补一条分片删除路径，而不是反过来。

#### 5.6 树控件样式对比

| 位置 | 样式 |
|---|---|
| `Ui/TreeViewUtil.cpp:10-11` | `TVS_HASBUTTONS \| TVS_HASLINES \| TVS_LINESATROOT \| TVS_SHOWSELALWAYS` + `WS_CLIPSIBLINGS`（注册表树与硬件树共用） |
| `Features/Kernel/KernelPage.cpp:2160` / `5850` | 同上但**少了 `WS_CLIPSIBLINGS`**，且**没有 `WS_VISIBLE`**（永远隐藏） |

三者都没有 `TVS_SORT`（避免每次插入的 O(log N) 排序开销，是好事），也都没有 `TVS_NOHSCROLL`（横向滚动条会按最长文本反复重算，插入多时成本被放大）。

---

### 6. 跨树的共性残留问题

#### 6.1 `reset()` 单条消息递归删整棵树

`LazyTreeView::reset()`（`Ui/LazyTreeView.cpp:272-281`）：

```cpp
void LazyTreeView::reset()
{
    if (hwnd_) {
        beginUpdate();
        ClearTreeView(hwnd_);          // → TreeViewUtil.cpp:66
        endUpdate();
    }
    nodes_.clear();
    itemByKey_.clear();
}
```

`ClearTreeView`（`Ui/TreeViewUtil.cpp:62-68`）做的是：

```cpp
void ClearTreeView(HWND treeView) {
    if (treeView) {
        TreeView_DeleteItem(treeView, TVI_ROOT);   // 一次删掉整棵树
    }
}
```

**问题**：`TVI_ROOT` 的删除会**递归清掉整棵已展开子树**，全部在一条 UI 消息里完成。已展开的节点数**没有上限**——每个父节点可以插到 `kMaxRowsPerParent = 5000`，展开的层数也没有限制。删除时每个节点还会触发一条 `TVN_DELETEITEMW`，`LazyTreeView::handleNotify`（**定义于** `Ui/LazyTreeView.cpp:453`，处理 `TVN_DELETEITEMW` 的分支在 `:465-470`）逐条 `eraseRecord`，即 **N 次哈希擦除也在同一条消息里**。

**当前命中者**：
- 注册表树：`RebuildRegistryTree`（`RegistryView.cpp:705`）→ `FillTreeRoots`（`:334`）→ `state.tree.setRoots(...)`（`:345`）→ `reset()`。触发点：树筛选防抖定时器（`:1380`）、模式切换（`:731`、`:1438`）、页面创建（`:1277`）。
- 硬件树：`ApplyHardwareFilter`（`HardwareView.cpp:471`）→ `PopulateTree`（`:477`）→ `state->tree.setRoots(...)`（`:383`）→ `reset()`。触发点：**筛选框每次 `EN_CHANGE`**（**Win32 宏**，调用点 `HardwareView.cpp:761`，分支判断在 `:760`），无防抖。

**与内核页自定的规矩对比**：`KernelPage.cpp:5396-5397` 明文写着 "A single TVM_DELETEITEM must never recursively erase an unbounded old subtree in one UI callback."，内核树因此专门做了逐叶 4ms 分片删除。**`LazyTreeView` 目前没有对应的分片删除路径**，这是把内核树迁进封装时会踩的坑（§5.5），同时也是注册表树/硬件树现在就在承受的形态。

**事实层面能确定的量级**：一次 `TVM_DELETEITEM(TVI_ROOT)` 的成本随已展开节点数线性增长；设备树浅（3–5 层）、注册表树单父封顶 5000，所以实际节点数通常在百到千级，**不是灾难级，但也不是免费**。硬件页因为筛选无防抖，这条路径被触发的频率最高。

#### 6.2 筛选态展开所有根 ⇒ 单条消息内插入量 = 根数 × 400

见 §2 理由 3。`kBatchSize` 与 `kMaxRowsPerParent` 都是**单父**上限，筛选态把**所有根**都展开一遍，于是总量失去全局上限。

**同步不可让出**：这条路径上的插入全部落在 `TVN_ITEMEXPANDINGW` 通知内部（`handleNotify` **定义于** `Ui/LazyTreeView.cpp:453`、分支 `:459-464` → `onExpanding` **定义于** `:407`、`childSource_` 分支 `:432-438` → `installChildren(..., expand=false)`）。分批机制在这里帮不上忙 —— `kBatchSize` 只限制"一个父节点一次插多少"，而这里是**多个父节点各插一批，全挤在同一条消息里**。`WM_SETREDRAW(FALSE)` 只压绘制，**记账仍同步**（§1「让出消息」条目已展开说明）。

#### 6.3 `ReplaceTreeNodeChildren` 的构造循环跑的是 N，不是 400

**这一条纠正一个容易读错的地方**：`kBatchSize = 400` 约束的是**插入**（`LazyTreeView::insertBatch`，`Ui/LazyTreeView.cpp:202`），**不约束 `TreeRow` 的构造**。

`RegistryView.cpp:352-360`（UI 线程，deliver 回调内）：

```cpp
std::vector<Ksword::Ui::TreeRow> children;
children.reserve(snapshot.subKeys.size());          // 353: 一次性 reserve 全部 N
for (const std::wstring& subKey : snapshot.subKeys) {   // 354: 遍历全部 N，不是前 400
    Ksword::Ui::TreeRow row;
    row.text = subKey;                              // 356: 一次 wstring 拷贝
    row.key = DisplayPathFromRootAndChild(snapshot.path, subKey);  // 357: 一次路径拼接 + 分配
    row.hasChildren = true;
    children.push_back(std::move(row));              // 359
}
state.tree.setChildren(item, std::move(children));  // 364
```

所以在 UI 线程上真实发生的是：

| 工作 | 次数 | 每次成本 |
|---|---|---|
| `wstring` 拷贝（`row.text`） | **N** | 一次分配 + 拷贝 |
| 路径拼接（`row.key`） | **N** | `rootText + L"\\" + childName`（`RegistryView.cpp:226-231`）→ 一次分配 + 拷贝 |
| `TreeRow` 构造 + `push_back` | **N** | 一次 `TreeRow` 构造（两个 `wstring` 成员） |
| `reserve` 一次性分配 | 1 | N × sizeof(TreeRow) |
| **`TVM_INSERTITEM`（插入）** | **≤400/批** | 由 `kBatchSize` 约束 |

**N 没有上限**：后端枚举完全不 clamp（`shared/usermode/backend/registry/RegistryBrowse.cpp:75`，`for (DWORD index = 0; index < subKeyCount; ++index)`），`subKeyCount` 直接来自 `RegQueryInfoKeyW`。而 `snapshot.subKeys` 是在**后台线程**构造的（`RegistryView.cpp:419`），整份列表通过 `AsyncSnapshotTask` 交给 UI 线程。

**与改造前的对比**（这是判断"是否仍是问题"的关键）：
- 改造前：每个子键 **2 次 `TVM_INSERTITEM`**（本体 + 占位子节点）+ 2 次堆分配 + N 次路径拼接，**且无 `WM_SETREDRAW` 抑制**。按 RegistryPerf-flash 在 `docs/audit/registry-expand-perf.md:60-67` 的推算量级，每次未抑制重绘的 `TVM_INSERTITEM` 约 20–100 µs，`HKCR`（10000+ 子键）就是 **0.4–2 s**。
- 改造后：**0 次** `TVM_INSERTITEM` 在这个循环里（插入被推迟到 `insertBatch`，且 ≤400/批 + 有重绘抑制）。循环里剩下的是 N 次字符串分配与拷贝，量级在**毫秒级**（每次 `wstring` 拼接约百纳秒级），比改造前低 2–3 个数量级。

**结论**：这仍是 UI 线程上的 O(N) 工作，**不是** O(400)，但已是可接受的量级——**不构成风险判定**，也不改变注册表树 OK 的结论。真正的收益来自"插入从 2N 次降到 ≤400 次/批"，而不是来自"字符串构造被限制到 400"。

**一个附带事实（内存侧，非线程侧）**：`LazyTreeView::installChildren`（`Ui/LazyTreeView.cpp:296-301`）把整份 `children` 收进 `node.pending`，只有被插进控件的行才在 `insertBatch` 里 `row.text.clear(); row.text.shrink_to_fit();`（`Ui/LazyTreeView.cpp:227-230`）释放。**未显示的行**（超出 400 且未点「载入更多」）连同其 `text`/`key` 一起驻留在 `pending` 里，量级是 O(未显示行数)。这与 `kMaxRowsPerParent = 5000` 的截断上限一致，不构成无界增长。

#### 6.4 内核树的载荷构造在 UI 线程，但不在树的插入路径上

内核树有一个**树控件特有的**拷贝点，不属于 `TreeView_InsertItem`，但确实落在 UI 线程上，且**规模随树节点数 N 线性增长**——因此单列出来，避免被误认为"树侧已无残留"。

**载荷模型**：树节点的 `lParam` 存的是 `KernelObjectNamespaceTreeNodeState*`（裸指针），所有权由 `KernelProjectionValues.inl:2735` 的 `owners` map 持有（`shared_ptr`）：

```cpp
std::unordered_map<KernelObjectNamespaceTreeNodeState*,std::shared_ptr<KernelObjectNamespaceTreeNodeState>> owners;
```

`state` 本身在**后台 builder 线程**构造（`KernelProjectionValues.inl:2763-2771`，`make_shared` + 5 个 `wstring` 成员 move 进结构），`owners.emplace(raw,std::move(state))` 把 `shared_ptr` 存进 map，`treeNodes` 里再存一份 `shared_ptr` 副本（`:2780`）。

**UI 线程上的 key 拼接**（`KernelPage.cpp:5149-5151`，`RequestCachedValueProjection` 内）：

```cpp
const auto key=[&](HTREEITEM item){TVITEMW value{};value.mask=TVIF_PARAM;value.hItem=item;
    return item && TreeView_GetItem(objectNamespaceTree_,&value) && value.lParam
        ? KernelTreeNodeKey(*reinterpret_cast<const KernelObjectNamespaceTreeNodeState*>(value.lParam))
        : std::wstring{};};
input->treeSelectedKey=key(TreeView_GetSelection(objectNamespaceTree_));
input->treeTopKey=key(TreeView_GetFirstVisible(objectNamespaceTree_));
```

`KernelTreeNodeKey`（`KernelValueSupport.cpp:451-452`）做的是 **3 段 `wstring` 拼接 + 一次 `LowerInvariantKey` 拷贝**（`KernelValueSupport.cpp:368`，按值传参再 `towlower` 变换）：

```cpp
return LowerInvariantKey(state.nodeKind+L"\n"+state.nodeType+L"\n"+state.nodePath);
```

**规模判定：不是热点。** 这个 lambda 在 UI 线程上**只调用 2 次**（选中项 + 首可见项），不是 N 次。N 次的 key 构造发生在**后台 builder 线程**（`KernelProjectionBuilderCore.cpp:73`，遍历 `result_->treeNodes` 逐节点算 key 以匹配选中/顶端 key），代价落在后台线程。

**因此**：内核树侧真正的 UI 线程残留只有 **2 次 key 拼接**，量级可忽略；树的插入/删除都做了 4ms 分片（§3），载荷构造在后台线程。**内核树判定 OK 不变**，本条只是把"载荷拷贝"这条线也核完，避免遗漏。

#### 6.5 回调拦截面板：内核页唯一没有重绘抑制的批量插入路径（**非树控件**，记此仅为避免被误当作树侧遗漏）

本条不属于树控件判定（该面板 4 个列表全是 `WC_LISTVIEWW`，见"明确排除"表），但它是在树控件普查过程中核到的**同一类形态**（UI 线程逐项重建控件 + 无上限 + 无重绘抑制），且与相邻审计的 #31 同源，故单独记录事实。**改造建议归 UI 线程拷贝审计报告。**

**机制**：`ScopedListViewRedrawLock`（**类定义** `Ui/ListViewUtil.h:57`）对 ListView 与 Header 各持一把 `ScopedWindowRedrawLock`（**类定义** `Ui/ListViewUtil.h:40`），即构造时 `WM_SETREDRAW(FALSE)`、析构时恢复并 invalidate 一次。内核页其他 4 处批量重建路径都套了它：

| 路径 | 重绘锁 |
|---|---|
| `RestoreReportListRows`（**定义于** `KernelPage.cpp:4082`，取锁语句在 `:4089`） | 有 |
| `RenderResult`（**定义于** `KernelPage.cpp:4840`，三把列表锁的取锁语句在 `:4841-4843`） | 有（3 个列表各一把） |
| `ApplyCachedValueProjection`（**定义于** `KernelPage.cpp:5191`，取锁语句在 `:5234`） | 有 |
| `RequestResultSort`（**定义于** `KernelPage.cpp:5785`）的排序应用回调（取锁语句在 `:5840`） | 有 |
| **回调拦截面板**（`PopulateCallbackInterceptPanel` **定义于** `:7052` / `RenderCallbackLocalModel` **定义于** `:7232`） | **无** |

**两点比"仅缺一把锁"更严重的细节**：

1. **`RenderCallbackLocalModel` 本身就是第二个无锁批量插入路径**，不只是"没加锁"。它内部自己做 `ListView_DeleteAllItems(callbackGroupList_)`（`:7238`）+ 逐组 `ListView_DeleteAllItems(ruleList)`（`:7240-7242`）+ 逐行 `AddListRow`（`:7243` 起）。`PopulateCallbackInterceptPanel` 另有一轮 `DeleteAllItems` ×4（`:7063-7071`），两轮都不在锁内。

2. **触发频次远高于"刷新时一次"**。`RenderCallbackLocalModel` 有 **12 处调用点**，全部是用户交互或导入操作：

   | 调用点 | 触发动作 | 所属函数 |
   |---|---|---|
   | `:7073` | 面板填充 | `PopulateCallbackInterceptPanel`（**定义于** `:7052`） |
   | `:7514` | 新增规则组 | `OnCallbackAddGroup`（**定义于** `:7500`） |
   | `:7532` | 删除规则组 | `OnCallbackRemoveGroup`（**定义于** `:7518`） |
   | `:7547` | 重命名规则组 | `OnCallbackRenameGroup`（**定义于** `:7536`） |
   | `:7576` | 上下移动规则组 | `OnCallbackMoveGroup`（**定义于** `:7554`） |
   | `:7588` | 启用/禁用规则组 | `OnCallbackToggleGroupEnabled`（**定义于** `:7580`） |
   | `:7629` | 新增规则 | `OnCallbackAddRule`（**定义于** `:7596`） |
   | `:7646` | 删除规则 | `OnCallbackRemoveRule`（**定义于** `:7633`） |
   | `:7677` | 上下移动规则 | `OnCallbackMoveRule`（**定义于** `:7650`） |
   | `:7694` | 启用/禁用规则 | `OnCallbackToggleRuleEnabled`（**定义于** `:7681`） |
   | `:7813` | 粘贴规则文本 | `OnCallbackPasteRuleText`（**定义于** `:7741`） |
   | `:8157` | 导入配置文件 | `ApplyCallbackFileIoResult`（**定义于** `:8126`） |

   即**用户点一下「新增规则」就触发一次整表删除 + 全量重绘**。

3. **行数放大 2 倍**：每条规则插 **2 行**——一条规则行（`:7257`）+ 一条「发起程序匹配 / 目标程序匹配 / 备注」说明行（`:7269`）。N 条规则 → **2N 行插入**，每行 10–11 列，即每次 20N–22N 次 `ListView_SetItemText` 跨进程消息，每次都触发重绘。

4. **无数量上限**：`callbackRules_` 无任何 cap——`push_back` 见 `:7628`（新增）与 `:7812`（粘贴）；导入路径 `LoadCallbackLocalConfig`（`:8506`）用 `while (std::getline(input, line))`（`:8517`）读到文件结束，逐条 `rules.push_back`（`:8598`），同样无 cap。

**修复时的嵌套约束（重要事实，非改法建议）**：`ScopedWindowRedrawLock`（**类定义** `Ui/ListViewUtil.h:40`）**不可重入**。其构造无条件发 `WM_SETREDRAW(FALSE)`（`Ui/ListViewUtil.cpp:14`），析构无条件发 `WM_SETREDRAW(TRUE)`（`:24`）——`WM_SETREDRAW` 是**布尔开关，不是计数器**。

而 `PopulateCallbackInterceptPanel`（**定义于** `:7052`）**内部调用** `RenderCallbackLocalModel`（**定义于** `:7232`，**调用点在 `:7073`**），两者作用在**同一批 HWND** 上（`callbackGroupList_` `:2390`、`callbackRuleLists_` 各 `:2410` 创建的独立 `WC_LISTVIEWW`、`:2435`、`:2475`）。因此若在两处入口**各加一把**锁，内层析构会把外层的抑制提前解除，外层后续的 `DeleteAllItems` + 逐行插入照样逐行重绘。

已核实**全仓库 17 处锁用法均无同句柄嵌套**，故该风险**只在修复时才会产生**（是"修复引入的新风险"，不是既有缺陷——两者性质完全不同，对后来者判断优先级很重要）。17 处分布：`FileView.cpp:482`、`HandlePage.cpp:896` / `:943`、`KernelPage.cpp:1892` / `:1900` / `:3700` / `:4089` / `:4841-4844` / `:5234` / `:5840`（`RequestResultSort`）、`NetToolsConnectionView.cpp:304`、`RegistryView.cpp:642`、`WindowView.cpp:1215`、`TextFindSupport.cpp:478`。

三个最像嵌套的已逐个排除：
- **`HandlePage.cpp:896` 与 `:943`**——同一个函数 `PopulateDetail`（`:879`）、同一个控件 `detailList_`，但 `:943` 位于 `state_->detailTask->request(...)`（`:905`）的**异步 deliver 回调**内（lambda 起点 `:928`），是任务完成后**另一个时刻**的独立取锁，`:896` 的锁那时早已析构。**不是嵌套。**
- **`KernelPage.cpp:4841-4844`**——`:4841`/`:4842`/`:4843` 是三把 ListView 锁作用于三个不同 HWND（`resultList_` 创建于 `:2169`、`propertyList_` `:2186`、`summaryList_` `:2215`），`:4844` 是 `ScopedWindowRedrawLock objectTreeLock(objectNamespaceTree_)` 作用于**树控件**，也不是同一句柄。**不构成同句柄嵌套。**（另见 §5.2 末尾：`:4844` 这把树锁在其函数体内是空转的。）
- **`KernelPage.cpp:1892` 与 `:1900`**——同一函数内两个**互斥 if 分支**（`header->hwndFrom == primaryTab_` 与 `== secondaryTab_`）各自的独立取锁，各自单层作用域。**不是嵌套。**

**一条由此推出的边界事实**：现有 17 处**全部是单句柄、单层作用域**（逐处扫描该锁之后 45 行内的锁数量，除 `:1892` 覆盖到 `:1900`、`:4841` 覆盖到 `:4844` 这两组外，其余每处均为单锁，而那两组也是不同句柄或互斥分支）。因此若把 `ScopedWindowRedrawLock` 改成引用计数语义，**这 17 处的行为完全不变**（进 1 出 1、计数 1→0、归零才恢复，与现状等价）。此条仅作事实记录——改法与风险评估归 `docs/audit/ui-thread-copy-hotspots.md` 的 #31 条目第 5 条。

**与树侧判定的关系**：**无**。这条不改变任何树的判定，登记在此只为两件事——(a) 它是"UI 线程逐项重建控件 + 无上限 + 无重绘抑制"这一形态在**内核页**的残留，形态与树侧的 §6.1 同源，说明该形态不止存在于树控件；(b) 避免后续读者按"内核页已做对"的印象漏掉它。

**改造建议见 `docs/audit/ui-thread-copy-hotspots.md` 的 #31 条目第 5、6 条**（该报告负责改法，本节只写事实与约束）。

---

## 判定汇总与建议关注点

| 判定 | 数量 | 对象 |
|---|---|---|
| **严重**（无上限 + 同步全量插入） | 0 | — |
| **风险** | 1 个树实例 + 1 条共性机制 | 硬件设备树 `HardwareView.cpp:715`（残留：筛选重建路径）；`LazyTreeView::reset()` 整树单消息递归删除 `Ui/LazyTreeView.cpp:272`（同时命中注册表树与硬件树） |
| **OK** | 3 个树实例 + 2 个封装 | 注册表树、内核树（×2 创建点，同一语义）、`LazyTreeView` 封装、`TreeViewUtil` helper |

### 还需关注的两处

- **硬件设备树的筛选重建路径**（`HardwareView.cpp:471-494` → `:370-396`）：
  1. `EN_CHANGE`（**Win32 宏**）没有防抖（对比注册表页 `RegistryView.cpp:1400-1406` 的 250ms 定时器）；
  2. 筛选态展开所有根（`:384-394`），单条消息内插入量 = 根数 × 400，无全局上限；且插入落在 `TVN_ITEMEXPANDINGW` 内部，**同步不可让出**（§6.2）。
- **`LazyTreeView::reset()` 的整树递归删除**（`Ui/LazyTreeView.cpp:272` → `Ui/TreeViewUtil.cpp:66`）：注册表树与硬件树的每次树重建都走这里，与内核页自己定的规矩相反。

**另有一处非树控件但同形态的残留**（§6.5，不计入上表判定）：内核回调拦截面板缺重绘抑制 + 12 处交互触发点 + 2N 行放大 + 无行数上限。改造建议归 UI 线程拷贝审计报告，树侧仅记录事实。

### 四处需要纠正的常见误读

**其一：`kBatchSize = 400` 兜底了注册表树的展开成本** —— **只对了一半**。插入确实被限制在 ≤400 行/批，但 `ReplaceTreeNodeChildren` 的 `TreeRow` 构造循环跑的是**全部子键数 N**（§6.3）。所幸这部分是 N 次字符串分配（毫秒级），而非改造前的 2N 次 `TVM_INSERTITEM`（`HKCR` 达 0.4–2 s），所以量级可接受、判定仍为 OK。**真正的收益来自"插入从 2N 降到 ≤400/批"，不是来自"字符串构造被限制到 400"。**

**其二：有 `WM_SETREDRAW` 抑制 = 有让出消息** —— **不成立**。`WM_SETREDRAW(FALSE)` 只压**绘制**，不压 `TVM_INSERTITEM` 的**条目记账**；而 `LazyTreeView` 真正让出消息的是**分批 + 合成行**（用户点「载入更多」）。硬件页筛选态那条路径是"绘制被抑制、记账仍同步、同步不可让出"的典型（§5.2、§6.2）。

**其三：内核页的批量列表重建都做了重绘抑制** —— **不成立**。内核页有 4 处批量重建套了 `ScopedListViewRedrawLock`（`:4089` `RestoreReportListRows`、`:4841-4843` `RenderResult`、`:5234` `ApplyCachedValueProjection`、`:5840` `RequestResultSort` 的排序应用回调），但**回调拦截面板是唯一遗漏**（§6.5），且它有 12 处调用点全是用户交互——点一下「新增规则」就整表重绘一次。别被"内核页已做对"的印象误导。

**其四：给回调面板补一把重绘锁就能解决** —— **会踩坑**。`ScopedWindowRedrawLock`（**类定义** `Ui/ListViewUtil.h:40`）**不可重入**：构造无条件 `WM_SETREDRAW(FALSE)`（`Ui/ListViewUtil.cpp:14`）、析构无条件 `WM_SETREDRAW(TRUE)`（`:24`），而 `WM_SETREDRAW` 是**布尔开关不是计数器**。`PopulateCallbackInterceptPanel`（**定义于** `:7052`）内部调用 `RenderCallbackLocalModel`（**定义于** `:7232`，**调用点在 `:7073`**），两者作用在同一批 HWND 上——**两处各加一把锁，内层析构会把外层的抑制提前解掉**，净效果几乎为零。已核实全仓库 17 处现有锁用法**均无同句柄嵌套**，所以这是修复时才会引入的新风险，不是既有缺陷。详见 §6.5 的"修复时的嵌套约束"。

### 已经做对的，别回退

- **`LazyTreeView` 这套封装本身**：`kBatchSize=400` / `kMaxRowsPerParent=5000` / `WM_SETREDRAW` 抑制 / `key` 哈希索引 / `TVIF_CHILDREN` 不插占位子项 / 截断提示行，是全仓库树里最完整的形态。两个可见树（注册表、硬件）都已接入。**注意两处区分**：`kBatchSize` 约束的是**插入**不是 `TreeRow` 的**构造**（§6.3）；`WM_SETREDRAW` 是**重绘抑制**不是**让出消息**（§5.2）——读代码时别把两者混为一谈。
- **内核树的 4ms / 128 工作单元分片泵**（插入和删除都分片，`KernelPage.cpp:5378-5438`）是唯一真正做到"不让 UI 线程被长任务钉住"的实现。**但正因为它做得对，迁移到 `LazyTreeView` 没有收益**——正确动作是评估停建这棵永久隐藏的树（§3 末尾、§5.5）。
- 注册表树的 250ms 筛选防抖 + `findItem` 哈希索引，是全仓库树里最完整的"交互不卡"形态。
- **全仓库唯一对树控件取重绘锁的地方是 `KernelPage.cpp:4844`（`objectTreeLock(objectNamespaceTree_)`），而它在 `RenderResult` 内空转**——函数体（`:4840-4988`，下一个函数 `AddDynamicResultColumns` 起于 `:4990`）只有这一行提到 `objectNamespaceTree_`（整个区间内该符号**仅此一次**），不操作这棵树。换句话说，**树控件的重绘抑制事实上是「零覆盖」**：内核树的插入路径本身不取锁（`PumpObjectTreeProjection`，`KernelPage.cpp:5378-5438`，内含真正的树插入 `TreeView_InsertItem`（`:5425`），但全函数 `WM_SETREDRAW` / `ScopedWindowRedrawLock` **0 命中**）；而全仓库唯一对树取锁的地方就是这把空转的 `:4844`。这**不构成缺陷**（树永久隐藏，隐藏控件不绘制，重绘抑制对它本无意义），但**别把它当成"内核页连树都做了重绘抑制"的证据**（§5.2 末尾）。

---

## 与相邻审计的边界（避免重复排期）

- **UI 线程快照拷贝审计**（`docs/audit/ui-thread-copy-hotspots.md`，SnapshotCopySurvey-bunny）：树控件的**插入路径**结论以本报告为准。注册表树与硬件树现在都不在 UI 线程直接调 `TreeView_InsertItem`；拷贝维度的残留集中在 `RegistryView.cpp:352-360`（`ReplaceTreeNodeChildren` 在 UI 线程把每个子键名拷成 `TreeRow`，含一次 `DisplayPathFromRootAndChild` 路径拼接），这属于拷贝问题，不在本报告判定范围内。**量级见 §6.3**——该循环跑的是**全部子键数 N**，不是 `kBatchSize=400`。
  - 树侧对拷贝审计的补充：**内核回调拦截面板（该报告 #31）不含任何树控件**（`KernelPage.cpp:2390` / `:2410` / `:2435` / `:2475` 四个列表全是 `WC_LISTVIEWW` + `LVS_REPORT`，无 `LVS_OWNERDATA`、无树），纯 ListView 口径，**树侧无对应条目**。树侧的 UI 线程残留只有 §6.4 那 2 次 key 拼接，量级可忽略。
  - **该报告 #31 的补充事实（§6.5）**：回调面板是**内核页唯一没有重绘抑制的批量插入路径**（其他 4 处都套了 `ScopedListViewRedrawLock`），且 `RenderCallbackLocalModel` 有 **12 处调用点**、全部是规则增删改的用户交互（新增/删除/重命名/移动/启禁规则组与规则、粘贴规则文本、导入配置）——即**每次交互都触发一次整表删除 + 全量重绘**，不是"刷新时一次"。每条规则插 2 行 → N 条放大成 2N 行 × 10–11 列。`callbackRules_` 无上限。
- **注册表展开性能实测**（`docs/audit/registry-expand-perf.md`，RegistryPerf-flash）：该报告针对的是 `969604fc` **之前**的旧注册表树实现，结论已被当前基线取代。
- **迁移可行性与退化风险**（`docs/audit/tree-sites-inventory.md`，TreeSites-bunny）：该报告是"能不能迁"的视角，本报告是"现在有没有性能问题"的视角。§4 的 `lParam=0` 约束、§5.5 的"内核树不能直接迁"两处与该报告的结论一致。
