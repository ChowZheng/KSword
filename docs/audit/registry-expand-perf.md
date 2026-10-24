# 注册表树展开卡顿剖析

分析对象：`KswordARKLight/Features/Registry/RegistryView.cpp`（1661 行）
配套：`RegistryActions.cpp`、`shared/usermode/backend/registry/RegistryBrowse.cpp`、`Ui/TreeViewUtil.cpp`、`Ui/AsyncTask.h`
性质：只读审计，未修改源码。

## 结论（按影响排序）

1. **`ReplaceTreeNodeChildren` 在 UI 线程一次插完整个子键列表，且每个节点插 2 次、堆分配 2 次** —— `RegistryView.cpp:481-508`（插入循环 503-506）+ `InsertTreeNode` `357-395`
   主线程工作量：`O(N)`，N = 该键的直接子键数。实测代价 = **2N 次 `TVM_INSERTITEM` + 2N 次 `new RegistryTreeNodeData` + N 次全路径字符串拼接**。`hasChildren` 在 505 行硬编码为 `true`，所以每个子键额外多一个占位子节点，成本直接翻倍。
   无 `WM_SETREDRAW(FALSE)`、无分批、无让出消息（全文件仅在 ListView 路径 780 行用了 `ScopedListViewRedrawLock`，TreeView 一次都没有）。

2. **树筛选框每敲一个字符就 `RebuildRegistryTree` 全树重建** —— `RegistryView.cpp:1521-1525` → `RebuildRegistryTree` `842-854`
   主线程工作量：`O(K)` 节点析构（K = 当前已物化的全部节点，含占位节点，即 ≈2×可见节点）+ 重新插入 5 个根 + `SelectPathInTree` 里再触发一次 HKLM 根节点的后台枚举，结果回来后又走第 1 条的 `O(N)` 全量插入。**逐字符 O(K) 重建，且路径上没有任何防抖。**

3. **`EnumerateRegistrySubKeyNames` 完全没有数量上限** —— `RegistryBrowse.cpp:75-82`（WinAPI）、`RegistryActions.cpp:191-193`（R0）
   循环上限就是 `RegQueryInfoKeyW` 返回的真实 `subKeyCount`，没有 `kMaxDisplayRows`、没有 `truncate`、没有 `5000`。

4. **`FindTreeItemByPath` 递归全树扫描，每个节点一次 `TreeView_GetItem`，无缓存** —— `RegistryView.cpp:465-479`
   调用点 3 处：`523`（`ReleaseTreeChildrenRequest`）、`550`（`EnsureTreeChildrenLoaded`）、`599`（子节点任务回调）。
   每次 `O(N)` 次 `TVM_GETITEM`（每次都是跨窗口 `SendMessage`）。第 550 行的调用发生在**每次展开**的入口，第 599 行发生在**每次展开结果落地**时。

5. **`PopulateList` 每次快照全量深拷贝行，无上限** —— `RegistryView.cpp:781` `state.list.setRows(*rows)`
   `VirtualListView::setRows(std::vector<VirtualListRow>)` 是**按值传参**（`VirtualListView.cpp:41`），所以这里把整份行表再复制一遍，每行 4 个 `wstring`。键下值多时是导航（不是展开）的次级卡点。

## 逐点证据

### 1. ReplaceTreeNodeChildren 全量插入

```cpp
// RegistryView.cpp:491-506
HTREEITEM child = TreeView_GetChild(state.tree, item);
while (child) {
    HTREEITEM next = TreeView_GetNextSibling(state.tree, child);
    if (HTREEITEM grandChild = TreeView_GetChild(state.tree, child)) {
        ClearTreeNodeDataRecursive(state.tree, grandChild);
    }
    if (RegistryTreeNodeData* childData = TreeItemData(state.tree, child)) {
        delete childData;
    }
    TreeView_DeleteItem(state.tree, child);
    child = next;
}
for (const std::wstring& subKey : snapshot.subKeys) {
    const std::wstring childPath = DisplayPathFromRootAndChild(snapshot.path, subKey);
    InsertTreeNode(state.tree, item, subKey, childPath, true);   // <- 505: hasChildren 恒为 true
}
```

代价分解（N = `snapshot.subKeys.size()`）：

- **删除段 `491-502`：通常 O(1)，不是主要成本。** `childrenLoaded`（490 行）一旦置 true，`EnsureTreeChildrenLoaded` 的守卫（546 行）就不会再为该路径发请求，所以重复进入这个删除循环的常规场景只有"旧内容是 1 个占位节点"。最坏情况（子树已被展开过再走到这里）才是 `O(子树节点数)`，但正常路径到不了。**这一点与直觉相反，值得记录：卡顿不来自删除，来自插入。**
- **插入段 `503-506`：2N 次插入。** 每个子键：
  - `DisplayPathFromRootAndChild`（240-245）做一次 `parentPath + L"\\" + subKey` 拼接 → N 次堆分配，且每个子节点都持有一份完整路径副本（内存 O(N × 路径长度)）。
  - `InsertTreeNode` 362 行 `new RegistryTreeNodeData()` → N 次堆分配。
  - `TreeView_InsertItem`（374）→ N 次 `TVM_INSERTITEM`。
  - 因为 `hasChildren == true`，380-393 再 `new` 一个占位节点并再发一次 `TVM_INSERTITEM` → **又 N 次堆分配 + N 次插入**。
- **无重绘抑制。** 父节点此时已经是展开状态（`TVE_EXPAND` 在枚举发起前就发生了，见 `611-651`），所以每个新插入项立刻进入可见集合，控件每次插入都要更新可见项列表、重算滚动条范围、`InvalidateRect`。全文件没有任何 `WM_SETREDRAW` / `ScopedWindowRedrawLock` 作用于 `state.tree`（grep 结果：唯一一处 redraw lock 在 780 行的 ListView）。

推算主线程耗时（无 profiler，按"每次未抑制重绘的 `TVM_INSERTITEM` 20–100 µs"量级推算）：

| 场景 | 子键数 N | 插入次数 2N | 推算阻塞 |
|---|---|---|---|
| `HKLM\SYSTEM\CurrentControlSet\Services` | ~500–900 | 1000–1800 | 20–180 ms |
| `HKLM\SOFTWARE` | ~500–1500 | 1000–3000 | 20–300 ms |
| `HKLM\SOFTWARE\Microsoft` | ~2000+ | 4000+ | 80–400 ms |
| `HKCR` / `HKLM\SOFTWARE\Classes`（合并视图） | 10000+ | 20000+ | 0.4–2 s |

### 2. RebuildRegistryTree 每次按键重建

```cpp
// RegistryView.cpp:842-854
void RebuildRegistryTree(RegistryViewState& state) {
    if (!state.tree) { return; }
    ClearRegistryTree(state.tree);                                    // 849
    for (const RegistryRootNode& rootNode : kRootNodes) {
        InsertTreeNode(state.tree, TVI_ROOT, rootNode.displayText, rootNode.pathText, true);
    }
    SelectPathInTree(state, CurrentPath(state));                      // 853
}
```

调用点（grep 全量）：

| 行 | 触发 | 频率 |
|---|---|---|
| 871 | `PrepareModeForExternalNavigation` | 外部跳转时 |
| 1408 | `WM_CREATE` | 一次 |
| **1523** | **树筛选框 `EN_CHANGE`** | **每个按键** |
| 1557 | 模式下拉 `CBN_SELCHANGE` | 用户切换 |

1521-1525 是这条路径的核心问题：

```cpp
if (LOWORD(wParam) == kTreeFilterBarId && HIWORD(wParam) == EN_CHANGE) {
    state->treeFilterQuery = Ksword::Ui::GetFilterBarText(state->treeFilterBar);
    RebuildRegistryTree(*state);
    return 0;
}
```

每次按键的同步工作量：

1. `ClearRegistryTree`（415-422）→ `ClearTreeNodeDataRecursive`（397-413）递归走遍**整棵已物化树**，每个节点一次 `TreeItemData`（即一次 `TVM_GETITEM`）+ 一次 `delete`，共 `O(K)`；然后 `Ksword::Ui::ClearTreeView` → `TreeView_DeleteItem(TVI_ROOT)`（`TreeViewUtil.cpp:66`）再让控件析构一遍。K = 所有已加载子键 ×2（占位节点也算一个节点、也有 `RegistryTreeNodeData`）。用户展开过两三层大键之后 K 轻松上千。
2. 重插 5 个根节点（10 次插入）。
3. `SelectPathInTree(CurrentPath(state))`：`FindRootTreeItemByPath` 找到根 → `TreeView_Expand(root, TVE_EXPAND)` → **`EnsureTreeChildrenLoaded(root)` 立刻发起一次 HKLM 的后台枚举**。也就是说清空之后又马上重新拉一次根的子键；结果回来时（第 1 条的路径）在 UI 线程再插 N 个节点。

**净效果：打一个字符 = 拆掉整棵树 + 重新拉一次 `HKLM` 子键 + 重新全量插入。** 而且筛选语义是"只作用于新枚举出来的那批子键"（`571-584` 在 worker 里过滤），已加载的节点被无条件丢弃 —— 占位文字写的是"筛选已加载的树节点"，实际行为是"丢弃已加载节点后重新加载"。

### 3. FindTreeItemByPath / FindChildTreeItemByText / FindRootTreeItemByPath

```cpp
// RegistryView.cpp:465-479
HTREEITEM FindTreeItemByPath(HWND treeView, HTREEITEM item, const std::wstring& path) {
    while (item) {
        const RegistryTreeNodeData* data = TreeItemData(treeView, item);   // 每个节点一次 TVM_GETITEM
        if (data && !data->placeholder && data->path == path) {
            return item;
        }
        if (HTREEITEM child = TreeView_GetChild(treeView, item)) {
            if (HTREEITEM found = FindTreeItemByPath(treeView, child, path)) {
                return found;
            }
        }
        item = TreeView_GetNextSibling(treeView, item);
    }
    return nullptr;
}
```

- `FindTreeItemByPath`：**递归全树前序遍历，无缓存、无路径索引、无提前剪枝**（不比较路径前缀，所以哪怕目标在另一个根下面也要把前面的根扫完）。每访问一个节点：1 次 `TVM_GETITEM`（`342-355`）+ 一次 `std::wstring` 全等比较。
  调用点：
  - `RegistryView.cpp:523` — `ReleaseTreeChildrenRequest`（错误/过期路径）
  - `RegistryView.cpp:550` — `EnsureTreeChildrenLoaded`，**每次展开入口，为了把上一个 `treeLoadingPath` 的 `childrenLoading` 清掉**
  - `RegistryView.cpp:599` — 子节点任务回调，**每次展开结果落地**
- `FindChildTreeItemByText`（`424-445`）：只扫**直接子节点**，对每个子节点 `TVM_GETITEM` 取文本到固定 512 `wchar_t` 缓冲，再 `_wcsicmp`。O(该层子键数)。调用点：`RegistryView.cpp:637`（`SelectPathInTree` 循环内，每层一次）。缓冲区 512 是硬截断，名字超过 511 字符会匹配失败（注册表名上限 255，实际不会触发）。
- `FindRootTreeItemByPath`（`447-463`）：只扫 5 个根的同级链表，`O(5)`，可忽略。调用点：`RegistryView.cpp:624`。

净代价：每次展开多两次 `O(N)` 全树 `SendMessage` 扫描，量级小于插入本身，但和插入串行叠加在同一个 UI 消息里。

### 4. InsertTreeNode 的额外成本

```cpp
// RegistryView.cpp:357-393（关键行）
auto* nodeData = new RegistryTreeNodeData();          // 362: 无条件堆分配
nodeData->path = path;                                // 363: 路径字符串再拷一份
insert.item.mask = TVIF_TEXT | TVIF_PARAM;            // 370
insert.item.lParam = reinterpret_cast<LPARAM>(nodeData);
HTREEITEM item = TreeView_InsertItem(treeView, &insert);   // 374
if (hasChildren) {
    auto* placeholder = new RegistryTreeNodeData();   // 381: 再分配一个
    childInsert.item.pszText = const_cast<LPWSTR>(L"");    // 387
    HTREEITEM placeholderItem = TreeView_InsertItem(treeView, &childInsert);   // 389
}
```

- `hasChildren` 的唯一实参是 505 行的字面量 `true`，没有任何"这个子键自己还有没有子键"的判断（判断要再走一次注册表，作者显然是刻意省了，但代价是 2× 插入）。
- 每个节点一份完整 `path` 副本（不是指针/共享），内存 `O(N × 路径长度)`。
- 控件样式（`Ui/TreeViewUtil.cpp:10-11`）：`TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS`。**没有 `TVS_SORT`**（好事，避免每次插入 O(log N) 比较），**没有 `TVS_NOHSCROLL`**（横向滚动条会按最长文本反复重算），也**没有 `TVS_NONEVENHEIGHT`**。`TVS_HASLINES` 让每个节点绘制时多一条线，绘制成本抬升但对插入影响有限。控件带 `WS_EX_CLIENTEDGE`，没有 double buffer，这解释了为什么"逐个插入 + 不抑制重绘"的组合特别慢。

### 5. SelectPathInTree / TVN_SELCHANGED 这条链

```cpp
// RegistryView.cpp:629-651
TreeView_Expand(state.tree, current, TVE_EXPAND);
EnsureTreeChildrenLoaded(state, current);          // 631: 可能发起后台枚举
std::wstring remaining = parsed.subKey;
while (!remaining.empty()) {
    ...
    current = FindChildTreeItemByText(state.tree, current, segment);   // 637: O(该层子键数)
    if (!current) { break; }
    TreeView_Expand(state.tree, current, TVE_EXPAND);
    EnsureTreeChildrenLoaded(state, current);      // 642: 可能再发起后台枚举
    ...
}
TreeView_SelectItem(state.tree, current);
TreeView_EnsureVisible(state.tree, current);
```

同步工作量：`O(深度 × 每层子键数)` 次 `TVM_GETITEM` 文本比较 + 每层一次 `TVE_EXPAND`（每次展开会重算可见项）。本身不至于卡，但因为 642 行会**为链上每一层都排一个异步枚举**（`AsyncSnapshotTask` 是"新请求覆盖旧请求"的合并语义，见 `AsyncTask.h:60-98`），实际只会落地最后一个，中间的枚举被丢弃但仍会执行完（`RetiredBackgroundValue`），所以后台线程白干 + UI 侧只收到一次 `O(N)` 插入。这是"点一下树卡一下"的直接来源。

`TVN_SELCHANGEDW`（1486-1495）→ `NavigateTo`（874-879）：

```cpp
void NavigateTo(RegistryViewState& state, const std::wstring& path) {
    InvalidateCurrentSnapshot(state);            // 875: 丢快照 + list.setRows({}) + ++displayGeneration
    ::SetWindowTextW(state.pathEdit, path.c_str());// 876: 触发 pathEdit EN_CHANGE -> 1507-1513 再 Invalidate 一次
    SelectPathInTree(state, path);               // 877: 见上
    RefreshSnapshot(state);                      // 878: 后台全量枚举该键的 subkeys + values
}
```

- `InvalidateCurrentSnapshot`（`1015-1029`）会 `state.list.setRows({})`，`VirtualListView::setRows` 里 `captureViewportAnchor()` + `rebuildVisibleIndexes()` + `updateItemCount()`，都是 UI 线程同步工作。
- 876 行 `SetWindowTextW(pathEdit)` 会**再触发一次** `EN_CHANGE`（1507），里面又跑一次 `InvalidateCurrentSnapshot`。等于每次导航都做两遍清空。
- 结算侧 `PopulateList`（`756-790`）在 UI 线程把整个 `snapshot.rows` 转成 `VirtualListRow` 并 `setRows(*rows)` 深拷贝（见结论 5），**无 `kMaxDisplayRows` 截断**。
- `RefreshSnapshot` 的枚举本身在 worker 线程（`809-817`），这部分不阻塞；阻塞点全在上面列的 UI 侧回调里。

### 6. 其它

- `SetStatus`（247-251）在 `ReplaceTreeNodeChildren` 末尾调用，`SetWindowTextW` 同步重绘状态栏，成本可忽略。
- `EnsureTreeChildrenLoaded` 的 worker（`564-586`）在带 query 时会构造一份 `rows` 再 `FilterRowIndexes`，属于后台线程，不影响 UI。
- `ReleaseTreeChildrenRequest`（510-536）在错误/过期路径调用，内含一次 `FindTreeItemByPath`（O(N)），非热路径。

## 数量上限现状

| 路径 | 有上限吗 | 证据 |
|---|---|---|
| 树：子键枚举 `EnumerateRegistrySubKeyNames`（WinAPI） | **无** | `RegistryBrowse.cpp:75` `for (DWORD index = 0; index < subKeyCount; ++index)`，`subKeyCount` 来自 `RegQueryInfoKeyW`，无 clamp |
| 树：子键枚举（R0） | **无** | `RegistryActions.cpp:191-193` 遍历 `result.subKeys` 全量 push |
| 树：UI 插入 | **无** | `RegistryView.cpp:503` 直接遍历 `snapshot.subKeys`，无 `kMaxDisplayRows`/无截断/无分批 |
| 树：筛选后子键 | **无**（只做过滤，不做截断） | `RegistryView.cpp:571-584` |
| 列表：快照 → `VirtualListView` | **无** | `RegistryView.cpp:781` `state.list.setRows(*rows)`，`VirtualListView.cpp:41-45` 不 clamp |
| 列表：值筛选 | 无行数截断 | `RequestValueFilter` 743-744 |
| 同类模块的参照上限 | 有，但注册表没用 | `Ui/ListViewUtil.h:15` `inline constexpr std::size_t kMaxDisplayRows = 5000;`，使用方如 `Features/AuditCommon/AuditTable.cpp:139`、`Features/ProcessDetail/ProcessDetailPage.cpp:1067`。**`RegistryView.cpp` 全文未引用 `kMaxDisplayRows`**（grep 仅命中 780 行 `ScopedListViewRedrawLock`） |
| 树控件重绘抑制 | **无** | `RegistryView.cpp` 全文无 `WM_SETREDRAW`，无作用于 `state.tree` 的 `ScopedWindowRedrawLock` |

## 修复建议（按性价比排序，不写代码，写做法）

1. **给树的批量插入加"暂停重绘 + 一次刷新"**（改 `ReplaceTreeNodeChildren`）
   进入插入循环前对 `state.tree` 发 `WM_SETREDRAW(FALSE)`（现成有 `Ksword::Ui::ScopedWindowRedrawLock`，或 `ScopedWindowRedrawLock` 的等价物），循环结束后 `WM_SETREDRAW(TRUE)` + 一次 `InvalidateRect`。同时把删除段和插入段都包进去。这一步不改语义、不动数据结构，预计能吃掉插入成本的一大半（省掉 2N 次可见项重算 + 重绘）。
   再加一个保险：插入前 `TreeView_SetItem` 之外不要碰 `EnsureVisible`。

2. **把 `hasChildren` 从恒 `true` 改成"惰性占位只加一个"**
   现状是 2N 插入 / 2N 分配。改成：只在**父节点被展开时**补占位（即 `TVN_ITEMEXPANDINGW` 里发现无子节点则插入占位），或者干脆保留 `TVS_HASBUTTONS` 并给每个子节点插占位但**复用同一个共享占位对象**（占位 `lParam` 传 `nullptr` 或一个 sentinel，`ClearTreeNodeDataRecursive` 里据此跳过 `delete`）。直接把 2N 降到 N。

3. **给树子键加硬上限 + 截断提示**
   照抄 `kMaxDisplayRows` 的既有做法：`EnumerateRegistrySubKeyNames` 里 `subKeyCount` clamp 到上限（例如 2000），并把 `total/shown` 带回 `RegistryTreeChildrenSnapshot`，在 `statusText` 里写"仅显示前 X / 共 Y 个子键，请用筛选框缩小范围"。注意与第 4 条配合，否则"筛选"永远筛不到被截掉的键。

4. **树筛选框改成"对已加载节点做视图过滤"，并加输入防抖**
   现在的语义是"清空重建 + 重新枚举"，`RebuildRegistryTree` 不该出现在 `EN_CHANGE` 里。做法：
   - 去掉 1523 行的 `RebuildRegistryTree`，改为仅记录 `treeFilterQuery` 并 `SetTimer` 做 150–250ms 防抖；
   - 防抖到期后，只对**已展开且已加载**的节点按其子键文本做 show/hide（`TreeView_DeleteItem` 删被过滤掉的子节点即可，`RegistryTreeNodeData` 里加一个 `loadedChildren` 缓存，避免重新枚举）；
   - 未加载的节点保持"展开时才按当前 query 过滤"的既有行为（`571-584` 那段 worker 过滤可以保留）。
   这样每次按键从 `O(K) 重建` 降到 `O(受影响层子键数)`。

5. **`FindTreeItemByPath` 换成路径索引**
   在 `RegistryViewState` 里维护 `std::unordered_map<std::wstring, HTREEITEM>`，只在 `InsertTreeNode` / `ReplaceTreeNodeChildren` / `ClearRegistryTree` 三处增删。把 523 / 550 / 599 / 637 四处 `O(N)` 全树扫描一次性降为 `O(1)`。注意 `HTREEITEM` 在树被清空后失效，`ClearRegistryTree` 时整表清空即可（这一点现有代码本来就保证：清空后不会再用旧句柄）。

6. **展开前不做全树扫描**（第 5 条的即时替代，若不引入索引）
   550 行只是为了清掉"上一个 loading 路径"的标志位，可以直接在 `RegistryViewState` 里存一个 `HTREEITEM treeLoadingItem` 而不是路径，省掉这次 `O(N)` 扫描。

7. **修掉 `NavigateTo` 的双重 Invalidate**
   876 行 `SetWindowTextW(pathEdit)` 会触发 `EN_CHANGE`（1507-1513）再清一遍快照。加一个 `state.suppressPathEditChange` 守卫，或把 `EN_CHANGE` 的判断改成"文本确实不同于 `state.snapshot` 对应路径"。

8. **`PopulateList` 去掉整表深拷贝并按上限截断**
   `VirtualListView::setRows` 是按值传参，`781` 行必然深拷贝一次。改用已有的 `setSharedRows(std::shared_ptr<const std::vector<VirtualListRow>>)`（`VirtualListView.cpp:47`），直接传 `rows` 这个 shared_ptr；同时按 `kMaxDisplayRows` 截断并在状态栏说明。
