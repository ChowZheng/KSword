# 矮窗口下详情区/列表压住状态行 —— 复核与修法

复核对象：上一轮静态审计给出的 3 处边界重叠。已逐行读实际代码，重算算式，并核对宿主容器是否存在最小尺寸保护。

## 0. 先决结论：这三处都是真问题（没有容器级兜底）

| 检查项 | 结果 |
| --- | --- |
| `MainWindow.cpp:61-62` `kMinimumWindowWidth=720 / kMinimumWindowHeight=480` | 只约束**顶层框架**，见 `App/MainWindow.cpp:434-443` 的 `WM_GETMINMAXINFO` |
| 三个视图的窗口样式 | 全部 `WS_CHILD`，由 `CreateDriverThreadView`（`DriverThreadView.cpp:575-580`）、`CreateRegistryView`、`CreateCallbackMonitorView` 作为 dock page 子窗口创建，`WM_GETMINMAXINFO` 对 child 无意义 |
| dock 分割布局最小尺寸 | `Docking/DockManager.cpp:964-1009` 的 `layoutNode` / `layoutLeafChildren` **没有任何最小宽高夹取**：`splitY = rect.top + Height(rect) * node->ratio`，`ratio` 可在 0~1 间任意拖，叶子矩形直接 `MoveWindow` 给 page |
| floating host | `Docking/` 内无 `WM_GETMINMAXINFO`，浮动窗口可自由缩到极小 |
| 已有先例 | `Features/Process/ProcessView.cpp:229-233` 的 `WM_GETMINMAXINFO` 只设 `ptMaxTrackSize.x`，没有最小高度；不能作为保护 |

结论：page 客户区高度可以被拖到 100~300px 级别，下面算出的阈值都可达。**三处都需要改**（Registry 那处还比审计说的更早发生，见 §3）。

---

## Features/Driver/DriverThreadView.cpp:413

### 实测代码（`DriverThreadView.cpp:401-420`，`Layout`）

```cpp
    constexpr int gap = 7;
    constexpr int toolHeight = 25;
    ...
    const int tableTop = 39;
    const int detailHeight = std::clamp(height / 3, 110, 220);
    const int tableHeight = std::max(50, height - tableTop - detailHeight - 32);
    ::MoveWindow(state.list.hwnd(), gap, tableTop, std::max(50, width - 2 * gap), tableHeight, TRUE);
    ::MoveWindow(state.detail, gap, tableTop + tableHeight + gap,
        std::max(50, width - 2 * gap), detailHeight, TRUE);
    ::MoveWindow(state.status, gap, height - 20, std::max(50, width - 2 * gap), 18, TRUE);
```

审计给的常量与算式**正确**：`gap=7`、`tableTop=39`、`statusTop=height-20`、`detailHeight=clamp(height/3,110,220)`、`tableHeight=max(50,height-39-detailHeight-32)`。

### 阈值算式

- `detailTop = 39 + tableHeight + 7 = tableHeight + 46`
- `detailBottom = tableHeight + 46 + detailHeight`
- `statusTop = height - 20`
- **未夹取时**（`tableHeight = height - detailHeight - 71`）：
  `detailBottom = (height - detailHeight - 71) + 46 + detailHeight = height - 25` → 与 `statusTop` 恒有 **5px** 间隙，健康。
- **夹取条件**：`height - detailHeight - 71 < 50`，即 `height < detailHeight + 121`。
- **重叠条件**（夹取后 `tableHeight=50`）：`50 + 46 + detailHeight > height - 20`
  → `height < detailHeight + 116`。
- 小高度区间 `height ≤ 330` 时 `detailHeight` 恒为下限 110，代入得
  **`height < 226` 开始重叠**。

校验：`height=226` → `tableHeight=max(50,45)=50`，`detailBottom=206`，`statusTop=206`（恰好相切，0 间隙）；`height=225` → `detailBottom=206` vs `statusTop=205`，压 1px。

### 结论

**真问题。客户区高度 ≤ 225 时详情框压住状态行**（225 压 1px，越小压得越多；到 121 时压 105px 整行）。审计的 225 阈值成立，夹取起点是 `height < 231`。

### 改法（保持语义，只换矮窗口下的分配顺序）

思路：先让出状态行 + 两个间距，再在剩余空间内决定 detail 高度（detail 上限从"由 height/3 推导"改为"同时受剩余空间约束"），最后 `tableHeight` 用同一算式但下限归 0。**≥231 时取值与原式完全一致**（已逐点验算 300/400/800）。

改前（`:413-419`）：

```cpp
    const int tableTop = 39;
    const int detailHeight = std::clamp(height / 3, 110, 220);
    const int tableHeight = std::max(50, height - tableTop - detailHeight - 32);
    ::MoveWindow(state.list.hwnd(), gap, tableTop, std::max(50, width - 2 * gap), tableHeight, TRUE);
    ::MoveWindow(state.detail, gap, tableTop + tableHeight + gap,
        std::max(50, width - 2 * gap), detailHeight, TRUE);
```

改后：

```cpp
    const int tableTop = 39;
    const int statusTop = height - 20;
    // 表格至少留 50，其余高度先给详情；矮窗口下详情按剩余空间收缩，避免压住状态行。
    int detailHeight = std::clamp(height / 3, 110, 220);
    detailHeight = std::min(detailHeight, std::max(0, statusTop - gap - (tableTop + 50 + gap)));
    const int tableHeight = std::max(0, height - tableTop - detailHeight - 32);
    ::MoveWindow(state.list.hwnd(), gap, tableTop, std::max(50, width - 2 * gap), tableHeight, TRUE);
    ::MoveWindow(state.detail, gap, tableTop + tableHeight + gap,
        std::max(50, width - 2 * gap), detailHeight, TRUE);
```

验算：`height=226` → 上限 `206-7-(39+50+7)=103` → `detailHeight=103`，`tableHeight=52`，`detailBottom=39+52+7+103=201`，`statusTop=206` → 5px 间隙。`height=140` → `detailHeight=17`，`tableHeight=52`，`detailBottom=115` vs `statusTop=120` ✓。

残余：`height < 71` 时 `tableHeight` 被 0 夹住，仍可能轻微越界（此时 detail 高度已为 0，是空控件，无可视影响）。若要彻底，可在 `height < 90` 时隐藏 detail，超出本次范围。

---

## Features/Monitor/CallbackMonitorView.cpp:617

### 实测代码（`CallbackMonitorView.cpp:604-623`，`Layout`）

```cpp
    ::MoveWindow(state.filter, 8, 72, (std::max)(100, width - 16), 26, TRUE);
    const int detailHeight = 116;
    const int listHeight = (std::max)(80, height - 110 - detailHeight - 32);
    ::MoveWindow(state.list, 8, 106, (std::max)(100, width - 16), listHeight, TRUE);
    ::MoveWindow(state.detail, 8, 112 + listHeight,
        (std::max)(100, width - 16), detailHeight, TRUE);
    ::MoveWindow(state.status, 8, height - 26, (std::max)(100, width - 16), 20, TRUE);
```

审计算式正确：`listHeight = max(80, height - 258)`（`110+116+32`），detail 顶 `112+listHeight`、底 `228+listHeight`，`statusTop = height-26`。

### 阈值算式

- **未夹取**：`detailBottom = 228 + (height - 258) = height - 30` vs `statusTop = height - 26` → 恒 **4px** 间隙，健康。
- **夹取条件**：`height - 258 < 80` → `height < 338`，此时 `listHeight=80`。
- **重叠条件**：`228 + 80 = 308 > height - 26` → `height < 334`。

校验：`height=334` → `detailBottom=308`，`statusTop=308`（相切）；`height=333` → 压 1px。

### 结论

**真问题。客户区高度 ≤ 333 时详情框压住状态行**。审计阈值成立。

### 改法

`detailHeight` 目前是硬编码常量 116，是压缩不动的那一块。把它改成"优先保证状态行 + 列表至少 80"的派生值，`listHeight` 公式不动。**≥338 时 `detailHeight` 仍为 116，布局完全不变。**

改前（`:617-621`）：

```cpp
    const int detailHeight = 116;
    const int listHeight = (std::max)(80, height - 110 - detailHeight - 32);
    ::MoveWindow(state.list, 8, 106, (std::max)(100, width - 16), listHeight, TRUE);
    ::MoveWindow(state.detail, 8, 112 + listHeight,
        (std::max)(100, width - 16), detailHeight, TRUE);
```

改后：

```cpp
    const int statusTop = height - 26;
    // 列表至少 80；详情高度按剩余空间收缩，避免矮窗口下压住状态行（原 4px 间距保留）。
    int detailHeight = 116;
    detailHeight = std::min(detailHeight, std::max(0, statusTop - 6 - (112 + 80)));
    const int listHeight = (std::max)(80, height - 110 - detailHeight - 32);
    ::MoveWindow(state.list, 8, 106, (std::max)(100, width - 16), listHeight, TRUE);
    ::MoveWindow(state.detail, 8, 112 + listHeight,
        (std::max)(100, width - 16), detailHeight, TRUE);
```

验算：`height=338` → 上限 `312-192=120` → 仍 116，`listHeight=80`，`detailBottom=308` vs `statusTop=312` ✓（4px）。`height=280` → 上限 `248-192=56` → `detailHeight=56`，`listHeight=82`，`detailBottom=250` vs `statusTop=254` ✓。

残余：`height < 222` 时 `detailHeight` 被 0 夹住，之后 `listHeight` 的 80 下限本身会顶到状态行（`height ≤ 211` 时列表压状态行）。这是列表 80px 可读下限的固有代价；如需再兜底，把 `(std::max)(80, ...)` 改为 `(std::max)(0, ...)`——但会让列表在极矮窗口下消失，不建议，除非产品要求。

---

## Features/Registry/RegistryView.cpp:928

### 实测代码（`RegistryView.cpp:901-951`，`LayoutChildren`）

```cpp
    const int gap = 6;
    const int margin = 6;
    const int toolbarHeight = 24;
    const int actionRowHeight = 24;
    ...
    const int filterTop = y + toolbarHeight + gap;          // 6+24+6 = 36
    ...
    const int contentTop = filterTop + toolbarHeight + gap; // 36+24+6 = 66
    const int listHeight = std::max(120, height - contentTop - 92);
    ::MoveWindow(state.tree, treeLeft, contentTop, treeWidth, listHeight, TRUE);
    ::MoveWindow(state.list.hwnd(), listLeft, contentTop, listWidth, listHeight, TRUE);
    ...
    ::MoveWindow(state.dataEdit, editorLeft, editY, editorWidth, std::max(90, listHeight - 24 - gap - actionRowHeight * 2), TRUE);
    ...
    ::MoveWindow(state.statusText, margin, height - 22, width - margin * 2, 20, TRUE);
```

审计算式正确：`contentTop = 66`、`listHeight = max(120, height - 158)`、list 底 `66+listHeight`、`statusTop = height-22`。

### 阈值算式（树/列表）

- **未夹取**：`listBottom = 66 + (height - 158) = height - 92` vs `statusTop = height - 22` → 恒 **70px** 间隙（这 92 是预留给编辑列下半部分的）。
- **夹取条件**：`height - 158 < 120` → `height < 278`，`listHeight = 120`。
- **重叠条件**：`66 + 120 = 186 > height - 22` → `height < 208`。

校验：`height=208` → list 底 186，`statusTop=186`（相切）；`height=207` → 压 1px。

### 阈值算式（编辑列，审计遗漏）

编辑列底边由最下面一行按钮决定（`RegistryView.cpp:941-948`）：

- `dataHeight = max(90, listHeight - 78)`（`78 = 24+6+24*2`）
- `dataEdit` 之后 `readButton` 行、`deleteButton` 行的累计偏移固定 `60`（`24+6+24+6`）
- `deleteButton` 底边 = `126 + dataHeight + 60 = 186 + dataHeight`
- `height ≤ 326` 时 `dataHeight` 恒为下限 90（因为 `listHeight - 78 = height - 236 ≤ 90`），故编辑列底边恒为 **276**
- 与 `statusTop` 重叠条件：`276 > height - 22` → **`height < 298`**

### 结论

**真问题，且比审计估计更早触发。**
- 树/列表压状态文字：客户区高度 ≤ 207（审计正确）。
- 编辑列底部的"删除/重命名"按钮行压状态文字：客户区高度 **≤ 297 就已经开始**（`dataHeight` 的 `max(90, ...)` 下限是主因）。这条审计漏了，实际用户最先看到的破图是这个。

### 改法（两处，同一个根因：下限夹取取的是"最小可读高度"而不是"剩余空间"）

**(a) 树/列表**：`std::max(120, ...)` → `std::max(0, ...)`，并把上界按状态行收一遍（防极端值）。

改前（`:928-931`）：

```cpp
    const int contentTop = filterTop + toolbarHeight + gap;
    const int listHeight = std::max(120, height - contentTop - 92);
    ::MoveWindow(state.tree, treeLeft, contentTop, treeWidth, listHeight, TRUE);
    ::MoveWindow(state.list.hwnd(), listLeft, contentTop, listWidth, listHeight, TRUE);
```

改后：

```cpp
    const int contentTop = filterTop + toolbarHeight + gap;
    const int statusTop = height - 22;
    // 树/列表不再强制 120：矮窗口下按剩余空间收缩，先保证状态文字这一行。
    const int listHeight = std::clamp(height - contentTop - 92, 0,
        std::max(0, statusTop - gap - contentTop));
    ::MoveWindow(state.tree, treeLeft, contentTop, treeWidth, listHeight, TRUE);
    ::MoveWindow(state.list.hwnd(), listLeft, contentTop, listWidth, listHeight, TRUE);
```

（上界 `statusTop-gap-contentTop = height-94` 在实际区间内恒大于 `height-158`，仅作兜底；只写 `std::max(0, height - contentTop - 92)` 也能消除重叠。）

**(b) 编辑列**：把两处重复的 `std::max(90, listHeight - 24 - gap - actionRowHeight * 2)` 提成局部量，下限 90 → 0，让 `dataEdit` 随 `listHeight` 一起收缩，按钮行自然上移。

改前（`:941-942`）：

```cpp
    ::MoveWindow(state.dataEdit, editorLeft, editY, editorWidth, std::max(90, listHeight - 24 - gap - actionRowHeight * 2), TRUE);
    editY += std::max(90, listHeight - 24 - gap - actionRowHeight * 2) + gap;
```

改后：

```cpp
    const int dataHeight = std::max(0, listHeight - 24 - gap - actionRowHeight * 2);
    ::MoveWindow(state.dataEdit, editorLeft, editY, editorWidth, dataHeight, TRUE);
    editY += dataHeight + gap;
```

验算（两处同改后）：`height=208` → `listHeight=50`，`dataHeight=0`，`deleteButton` 底 `186`，`statusTop=186`（相切）✓；`height=300` → `listHeight=142`，`dataHeight=64`（原 90），编辑列底 `250` vs `statusTop=278` ✓；`height=400` → `listHeight=242`，`dataHeight=164`（与原 `max(90,164)` 一致，布局不变）✓。

残余：`height < 208` 时编辑列固定 24px 行高（`nameEdit`+`typeCombo`+两行按钮 ≈ 144px，加 `contentTop=66`）仍在状态行附近，需隐藏次要按钮行才能继续改善，属结构性下限，超出本次范围。

---

## 汇总

| 位置 | 审计阈值 | 复核阈值 | 判定 | 主因 |
| --- | --- | --- | --- | --- |
| `DriverThreadView.cpp:413-419` | ≤225 重叠 | **≤225**（夹取起点 <231） | 成立，需改 | `tableHeight` 的 `max(50,...)` 下限 + `detailHeight` 下限 110 |
| `CallbackMonitorView.cpp:617-622` | ≤333 重叠 | **≤333**（夹取起点 <338） | 成立，需改 | `listHeight` 的 `max(80,...)` 下限 + `detailHeight` 硬编码 116 |
| `RegistryView.cpp:928-950` | ≤207 重叠 | 树/列表 **≤207**；**编辑列 ≤297** | 成立且更严重，需改 | `listHeight` 的 `max(120,...)` 下限 + `dataHeight` 的 `max(90,...)` 下限 |

三处改法都只动 `Layout` 内部的局部量：不新增成员、不引入布局框架、不改控件创建与消息处理；在各自"正常高度"区间内取值与原式逐点一致（已验算），仅在矮窗口段把"硬下限"换成"按剩余空间收缩"。
