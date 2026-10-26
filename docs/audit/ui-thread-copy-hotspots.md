# UI 线程快照拷贝热点

> 审计范围：`KswordARKLight/`（只读，未改动源码）
> 审计目标：找出**在 UI 线程上对整份快照做深拷贝/重建**的热点 —— 这些是切页、刷新、筛选时卡顿的来源。
> 项目约定（产品要求）：列表快照要**共享不深拷贝**。正例：`Ui/VirtualListView::setSharedRows(std::shared_ptr<const std::vector<VirtualListRow>>)`、`Ui/ImmutableTextRows.h`。

---

## 状态说明（审计期间代码已变动）

本报告审计期间，其他 agent 提交/正在提交多笔改动。本节记录**当前落地状态**，各条目保留原始发现作为**回归防线**（这些形态一旦回退就会重新引入整表深拷贝）。

### 已提交

| 提交 | 内容 | 对本报告的影响 |
|---|---|---|
| `45ca93b0` | 列表共享快照清扫，17 处 `setRows(*ptr)` → `setSharedRows(ptr)` | **#7～#10、#18～#29 已修复**，提交说明含交接后不变异的安全性核查 |
| `969604fc` | 注册表树迁到 `Ui/LazyTreeView`，展开不再一次插完整个子键列表 | **#6 已修复**（`RegistryView.cpp` 改 `setSharedRows`）；树展开路径同时被重写 |
| `4c2c4d98` | 新增 `Ui/LazyTreeView` 惰性树封装 | 分批插入（`kBatchSize=400`）+ 单父节点上限（`kMaxRowsPerParent=5000`） |
| `2256f567` | `Ui/ImmutableTextRows.h` 增加 `adopt(std::shared_ptr<const Values>)` —— 接管已冻结的行向量，每个 `RowRef` 别名指向共享 vector 的元素，不拷任何单元格文本；`KernelPage.cpp` 4 处 `currentRows_ = *currentRawRows_` → `currentRows_.adopt(currentRawRows_)` | **#2 已修复**（4 处调用点全改）；**#1 部分消解**（`flat_` 被预置，首次隐式转换零拷贝，但被 `:5113` 的 move-assign 清空 → 隔次仍触发） |
| `dc282eb1` | `Ui/EvidenceSessionView.cpp` 改用 `VirtualListView`（owner-data）+ `AsyncSnapshotTask` 后台构建行表 + `setSharedRows` 交接 + `kMaxDisplayRows` 截断 + 状态提示 | **#30 全量解决** —— 插入成本与物化成本同时消除，可作 #31 的实现范式 |
| `a787bdb3` | 抽离内核值辅助函数到 `KernelValueSupport.h/.cpp`（纯搬迁，不改行为） | **KernelPage.cpp 行号整体上移约 498 行**（本报告行号已按此校准） |
| `a44cbf6f` | 工具栏行状态文字给可读下限，窄窗口不再塌成 0 宽 | KernelPage.cpp 行号此前整体下移约 16 行 |

**`adopt()` 的设计值得肯定**：它同时解决了本报告 #2 的深拷贝**和** #1/#5 的重复物化 —— 因为 `flat_=std::move(values)` 让被接管的 vector 直接成为 `contiguousSnapshot()` 的缓存，后续 `operator const Values&` 不再触发重新物化。这是本报告建议的「一处改动同时解决多个热点」的正确实现形态。

**但收益有边界，需如实记录**：#1 的隐式物化只在「`adopt()` 之后、下一次重建之前」被消除。`RebuildResultListFromCache` 自身的收尾 `currentRows_ = std::move(visibleRows)`（`KernelPage.cpp:5113`）会走 `assign(Values&&)` 并 `flat_.reset()`，把 `adopt()` 预置的缓存清掉 —— 所以重建之间仍会重新物化。**根治点在 #3**（见下）。

### 进行中（尚无提交）

当前无「进行中」项：本报告涉及的改造均已落到提交。以下条目保留作为**回归防线**（这些形态一旦回退就会重新引入整表深拷贝）。

### 当前状态小结

- **`setRows(*ptr)` 全项目剩余 0 处。**
- **#1～#5（Kernel 5 处严重热点）**：根因改造 `adopt()` 已提交（`2256f567`），4 处调用点同步改为 `currentRows_.adopt(currentRawRows_)`。
  - **#2 已完全修复。**
  - **#1 部分消解** —— `adopt()` 让首次隐式转换零拷贝，但 `KernelPage.cpp:5113` 的 move-assign 会 `flat_.reset()`，**隔次仍触发全量物化**。
  - **#3（`visibleRows.push_back(row)` 按值拷贝每行 cells）不受 `adopt()` 覆盖** —— 那是另一层拷贝，**是当前 Kernel 侧的主要残留**。
  - **#4 判定已下修**（原判「严重」有误）：`fallbackDisplay = currentRows_` 走默认拷贝赋值，只拷两个 `shared_ptr`；真正的物化发生在**后台线程**的 `KernelProjectionValues.inl:15`。残留代价仅 `detach()` 时的 N×16 B 引用表复制。
  - **#5（显式 `contiguousSnapshot()`）仍存在** —— 但它本就是「反向产出扁平快照」的必要步骤，改法方向见条目正文。
- **#30（证据会话）**：已按「owner-data 虚拟表 + 后台构建 + 共享交接 + 截断提示」完整改造并提交（`dc282eb1`），插入成本与物化成本同时消除。
- **#31（Kernel 回调拦截面板）**：尚未开始 —— 是当前唯一未动的**主要**热点。改造顺序有讲究，见该条目。

### 行号说明

**本报告的行号以 `a787bdb3`（当前 HEAD）为准。**
涉及 `Features/Kernel/KernelPage.cpp`、`Ui/EvidenceSessionView.cpp`、`Ui/ImmutableTextRows.h` 的条目行号可能继续漂移 —— 各条目均标注了**函数名**以便定位，不依赖行号。
本报告统一以**提交号**标注状态，不使用「工作区未提交」这类描述 —— 工作区状态是瞬时的，任何人在别的时刻复核都会对不上；提交号是稳定的。

### 引用校验方式（复核者请照此重跑）

本报告全部 **135 处唯一 `文件:行` 引用**（正文提及 183 次）经**全量脚本校验**，而非抽样：

| 校验项 | 方法 | 结果 |
|---|---|---|
| 文件存在 + 路径匹配 | 递归索引 `KswordARKLight/` + 仓库根 `shared/`，比对路径后两段 | 135/135 |
| 行号在文件实际行数内 | 逐条读取行数并比对区间 | 135/135 |
| **锚点行非噪声** | 起始行不得是空行 / 纯注释 / 孤括号 | 135/135（仅 2 处**有意**引用契约注释：`Ui/ListViewUtil.h:106-107`「不插占位项」、`Ui/VirtualListView.h:47-48`「交接后不得修改」——上下文已写明是引用注释） |
| **语义指向** | 导出全部 134 个锚点的源码行逐条目视核对 | 135/135 命中 |
| **函数归属比对** | 对同时给出「函数名 + 行号」的断言，核对**该行确实落在该函数体内** | 见下 —— **查出 2 处真实错误并已修正** |
| **定义处 vs 调用点** | 核对每个「`函数名`（`:NNNN`）」形态的行号**是该函数的定义行还是调用行**；两者必须显式区分 | 见下 —— **查出 3 处歧义并已修正** |
| **非函数实体标注** | 核对每个非函数引用（类型 / 常量 / 类 / 成员）是否显式写明种类，且成员行号是否指向**声明处**而非「提及处」 | 见下 —— **查出 5 类问题并已修正** |

> ⚠️ **行号会漂移，锚点必须能自验证**：本报告多数条目已带「下一个函数起于 `:NNNN`」这类锚点，可在代码变化后自行重新定位。**若某条引用的起始行变成空行或与描述不符，即为失效，请按函数名重新定位而不是照抄行号。**

**「函数归属比对」查出的 2 处真实错误**（前四道检查都查不出 —— 行号在范围内、不是噪声、内容语义也对得上，只是**落在邻近的另一个函数里**）：

| 引用 | 报告原写的归属 | 实际归属 | 处理 |
|---|---|---|---|
| `KernelPage.cpp:5750` | `ApplyCachedFilter` | **`KernelPage::ScheduleCachedFilter`（定义于 `:5743`）** —— `::SetTimer(hwnd_, kTimerR0EvidenceFilter, 200, nullptr)` 是在**设定定时器**，不是执行筛选 | 已改为准确表述：定时器由 `ScheduleCachedFilter` 设置，到期后才进 `ApplyCachedFilter` |
| `KernelPage.cpp:7085` 的 `rowValue` | 与 `RenderCallbackLocalModel` 关联 | **`PopulateCallbackInterceptPanel`（定义于 `:7052`，函数体 `:7052-7218`）** —— `rowValue` 定义在此，`RenderCallbackLocalModel`（定义于 `:7232`，函数体 `:7232-7282`）**完全不用它** | 引用本身准确，但顺带校正了规模数字（见 #31 规模估计） |

> 这类错误**行号在范围内、语义也说得通**，只有把「该行属于哪个函数」与「报告说它属于哪个函数」逐一对照才暴露 —— 该检查由 TreeSurvey-bunny 在树侧报告里先行提出（他在 `KernelPage.cpp:5840` 上抓到同类错误），本报告据此自查并同样抓到 2 处。

**「定义处 vs 调用点」查出的 3 处歧义**（行号完全正确，但 `函数名（:NNNN）` 这个形态默认读作定义处，读者按定义去找会找不到）：

| 引用 | 原写法 | 实际 | 处理 |
|---|---|---|---|
| `KernelPage.cpp:7073` | `RenderCallbackLocalModel`（`:7073`） | **调用点**（`RenderCallbackLocalModel();` 这一行），定义在 `:7232` | 改为「定义于 `:7232`，**调用点在 `:7073`**」 |
| `KernelPage.cpp:7073` 等 | `PopulateCallbackInterceptPanel`（`:7052`）/ `RenderCallbackLocalModel`（`:7232`） | 均为定义行，但形态上与调用点混用 | 统一加「定义于」前缀 |
| `KernelPage.cpp:5234` / `:5840` | 只给行号、**不给函数名** | `:5234` 在 `ApplyCachedValueProjection`（`:5191`）；`:5840` 在 `RequestResultSort`（`:5785`） | 补全函数名 + 定义行号 |

> **五类引用约定**（本报告统一遵守）：
> - 指**定义**：写「**定义于** `:NNNN`」；
> - 指**调用**：写「**调用点** `:NNNN`」；
> - 指**函数体内的某条语句**：写「**定义于** `:NNNN`，……语句在 `:NNNN`」；
> - **裸行号**（无函数名）只用于**语句行**，且上下文必须能自明；
> - 指**非函数实体**（类型 / 常量 / 类 / 成员）：显式写明种类 —— 「类定义」「常量定义」「结构体定义」「成员声明」「类型成员声明」「控件创建语句」「Win32 宏」「Win32 结构体字段」。

**第 7 道检查（非函数实体）在本报告查出的问题**：

我原先把控件成员的行号直接并列，读者会误以为是「该成员的声明处」，实际那些行是**控件创建语句**：

| 引用 | 原写法 | 实际 | 已改为 |
|---|---|---|---|
| `resultList_`/`propertyList_`/`summaryList_`（`:2169`/`:2186`/`:2215`） | 并列三个 HWND 与三行 | **控件创建语句**（`::CreateWindowExW(...)`）；成员声明在 `Features/Kernel/KernelPage.h:324`/`:325`/`:326` | 标注创建语句 + 补成员声明行号 |
| `callbackGroupList_`/`callbackBypassList_`/`callbackFileMonitorList_`（`:2390`/`:2435`/`:2475`） | 并列 | 同上，**控件创建语句**；成员声明在 `KernelPage.h:357`/`:372`/`:383` | 同上 |
| `callbackRuleLists_`（`:2423`） | 与上面三个并列 | 性质不同 —— 是 `callbackRuleLists_.push_back(ruleList)`，**把新建的 ruleList 存入 vector**；成员声明在 `KernelPage.h:364` | 单独说明 |
| `primaryTab_`/`secondaryTab_`（`:1892`/`:1900`） | 取锁行 | 那是**取锁语句**；成员声明在 `KernelPage.h:278`/`:279` | 补成员声明行号 |
| `detailList_`（`HandlePage.cpp:896`/`:943`） | 取锁行 | 同上；成员声明在 `Features/Handle/HandlePage.h:84` | 补成员声明行号 |

> **关键判据（TreeSurvey-bunny 补充）**：**「这一行提到了某个成员」≠「这一行是它的声明」**。成员的行号引用必须区分三种位置：**声明处**（在 `.h` 里）、**创建/初始化处**（在 `.cpp` 里，如 `::CreateWindowExW`）、**使用处**（如取锁语句）。三者不能混用。

**给复现者的实现要点**（我踩过的坑）：

1. **函数区间索引必须按「下一个顶层定义」切**，不能按「行首非缩进」切 —— 函数体内有顶格写的语句（`}` 后紧跟的 `case`、lambda 体），按缩进切会把区间切碎。
2. **顶层定义正则要排除关键字开头**（`if`/`for`/`while`/`switch`/`catch`/`return`/`else`/`do`），否则 `if (xxx) {` 会被当成函数定义。
3. **正则要允许返回类型前缀**（`std::vector<...> void Foo(` 这类形态），否则顶层定义一个都匹配不上。**这条对 `Ui/LazyTreeView.cpp` 尤其关键** —— 那里大量函数是 `HTREEITEM LazyTreeView::insertBatch(...)` 形态。（TreeSurvey-bunny 补充。）
4. **只对「强断言形态」生效** —— 代码块头 `// Class::Method  (File.cpp:NNN)` 与 `` `Class::Method`（`File.cpp:NNN`） ``。把 API 调用名（`setRows`/`push_back`/`std::move`）当归属断言会产生大量误报（我第一版 53 条全是误报）。
5. **Win32 宏需白名单** —— `EN_CHANGE`/`WM_COMMAND`/`TVM_INSERTITEM`/`TVIF_CHILDREN`/`WM_TIMER` 不是函数，不参与归属比对。（TreeSurvey-bunny 补充。）
6. **排除「元说明」形态** —— 「原写作「`X`（`File.cpp:NNN`）」」是在举例说明旧写法，不是断言。（TreeSurvey-bunny 补充。）

7. **归属判定要双向兼容写法** —— 「报告写的名 == 实际 owner 名」**或**「owner 的末段 == 报告写的名」，否则 `Class::Method` 与 `Method` 两种写法互相不认。
8. **成员变量名不是函数** —— `propertyList_` / `summaryList_` / `callbackGroupList_` 这类 `HWND` 成员出现在「`名字` `:NNNN`」形态里时是**控件创建语句**，不参与归属比对。（TreeSurvey-bunny 补充。）
   - **扩展（TreeSurvey-bunny 补充）**：判定成员**不能只靠 `name.endswith("_")`** —— camelCase 成员（`refreshTask` / `childSource_` / `filterRows`）同样要排除，否则误报。
   - **更关键**：要核**该行是不是声明处**，不能只看「这一行提到了这个成员」——「这一行提到了某个成员」≠「这一行是它的声明」。成员的行号引用必须区分**声明处**（`.h` 里）/ **创建初始化处**（`.cpp` 里，如 `::CreateWindowExW`）/ **使用处**（如取锁语句）。TreeSurvey-bunny 在 `refreshTask` 上踩的正是后者：原标 `HardwareView.cpp:549`（`BeginDeviceRefresh` 的定义行），实际成员声明在 `HardwareView.cpp:91`。
9. **无文件名的引用需由上下文推断文件** —— 表格内 `` `函数名` `:NNNN` `` 形态没有文件名，脚本需从同一行最近的 `file:NNN` 推断，否则**整批漏检**（TreeSurvey-bunny 的脚本首版就栽在这里，19 条全被跳过）。我这边因表格内引用都带文件名前缀，未触发此问题，但这条对其他报告成立。
10. **引用数量的统计必须重跑 grep，不能凭印象** —— 我在本报告曾写「`KernelProjectionValues.inl` 里 **13 处** `rawRows = !currentRawRows_->empty() ? ...`」，重跑 grep 实际是 **9 处**（另有 1 处 `FirstRowUInt64(...)` 内联形态，共 10 个消费点）；同段的 `KernelPage.cpp` 行号 `:6501`/`:11402` 也已漂移（实为 `:6003`/`:10904`）。**「数量」与「行号」一样会因代码变动而失效**，必须每次重跑。

---

## 判定口径

### 快照的「共享」与「深拷贝」判定

一个列表快照的拥有权分三层，任何一层出现**值语义的复制**就算深拷贝：

| 层 | 内容 | 共享形态（正例） | 深拷贝（反例） |
|---|---|---|---|
| L1 容器 | `vector<Row>` 本体 | `shared_ptr<const vector<Row>>` 传递 | `setRows(*sharedPtr)` / `*dst = *src` / `rows = other` |
| L2 行对象 | `Row` 结构体本身 | `shared_ptr<const Row>` / `ImmutableTextRows::RowRef` | `push_back(row)`（按值）、`Row(...)` 临时对象 |
| L3 单元格文本 | `std::wstring` 缓冲 | 同一 `wstring` 缓冲被多处引用 | `wstring` 拷贝构造 / 重新拼接 / `to_wstring` |

### 判定规则（本次审计采用）

1. **共享**：`shared_ptr<const T>` 整体传递，控件只持有指针；行文本缓冲不做拷贝。
2. **深拷贝（严重）**：UI 线程上出现 L1/L2/L3 任一层值语义复制，且规模随快照行数 N 线性增长。
3. **重建（严重）**：UI 线程上按行**重新生成**单元格文本（`std::to_wstring`、格式化函数、`operator+` 拼接），即使最终共享。
4. **过渡期例外**：一次性、O(小常数)、非随 N 增长的复制（如详情面板 6~8 行）判为「可接受」。

### 关键机制说明（决定了为什么这些是真热点）

- `Ui/AsyncTask.h:33` `AsyncSnapshotTask<Result>`：工作线程产出 `Result`，经 `PostMessageW` 后由 UI 线程 `consume()` 回调 `Deliver`。
  - `Deliver` 的签名是 `void(std::uint64_t, std::optional<Result>&&, std::exception_ptr)` —— **结果按 `&&` 移动传递，跨线程零拷贝**。
  - 结论：**后台任务本身不产生拷贝**。真正的拷贝全部发生在 UI 线程的 `Deliver` 回调体内 —— 也就是「快照已经算好了，但装进控件时又复制一遍」。
- `Ui/BackgroundWork.h:220` `TryAcceptBackgroundWork()` / `RetireBackgroundValue()`：只负责生命周期与延迟销毁，不搬运数据。
- `Ui/VirtualListView.h:49` `setSharedRows()`：正例。`Ui/VirtualListView.cpp:41` `setRows(std::vector<VirtualListRow> rows)`：按值形参 + `make_shared` —— 调用方若写 `setRows(*sharedPtr)`，**整个 vector 被深拷贝一次**。
- `Ui/ImmutableTextRows.h:86` `operator const Values&()`：这是本项目**最隐蔽的深拷贝触发器** —— 它调用 `contiguousSnapshot()`（`Ui/ImmutableTextRows.h:89`），把 `shared_ptr<const Refs>` 里 N 个 `shared_ptr<const Row>` **物化成 N 个独立 `vector<wstring>`**（每行一次 `push_back(*row)` 深拷贝），并且这个 `flat_` 缓存会被 `detach()`（`Ui/ImmutableTextRows.h:99`）在每次 `push_back`/`reserve` 时 `reset()` 掉 —— 意味着**增量写入路径上会反复重新物化整份快照**。

### 规模换算口径（本报告统一使用）

- `std::wstring` 在 MSVC x64 下 = **32 字节**（含 SSO 缓冲，≤15 字符不额外分配堆）；>15 字符时额外堆分配 `(字符数+1)*2` 字节（向上取 16 字节对齐）。
- `std::vector<std::wstring>` = **24 字节**（3 个指针）+ 每个元素 32 字节。
- `Ksword::Ui::VirtualListRow` = `wstring stableKey`(32) + `vector<wstring> cells`(24) + `LPARAM`(8) + `int`(4) + 2×`COLORREF`(8) ≈ **80 字节**结构体 + cells 堆。
- 一个典型列表行（stableKey ≈ 20 字符 + 6 个单元格各 ≈ 20 字符）：
  - 结构体 80 B
  - stableKey 堆 ≈ 48 B（(20+1)*2 = 42 → 对齐 48）
  - cells vector 堆 = 6×32 = 192 B + cells vector 本体 24 B
  - 6 个单元格堆 ≈ 6×48 = 288 B
  - **单行 ≈ 630 字节**
- 通用公式：**N 行 × 630 字节 ≈ N×0.6 KB**。

---

## 热点清单（按代价排序）

| # | 文件:行 | 场景 | 拷贝的是什么 | 规模估计 | 判定 |
|---|---------|------|--------------|----------|------|
| 1 | `Features/Kernel/KernelPage.cpp:5064` | 切页 / 重建结果表（`RebuildResultListFromCache`） | `ImmutableTextRows` 隐式转 `vector<vector<wstring>>`，物化整份快照 + 再逐行拷贝 | N 行 × 630 B = N×0.6 KB；N=5000 → **3 MB/次** | 严重 → **部分消解**（`adopt` 后隔次触发） |
| 2 | `Features/Kernel/KernelPage.cpp:5704`（另 `:1234`/`:1249`/`:1263`） | 每次筛选 / 每次 combo 切换 / 每次勾选 | `currentRows_ = *currentRawRows_` → `ImmutableTextRows::assign` → N 个 `make_shared<const Row>(row)` 全量深拷贝 | N 行 × 630 B；N=5000 → **3 MB/次** | 严重 → **已修复**（`2256f567`） |
| 3 | `Features/Kernel/KernelPage.cpp:5058-5113` | 重建可见行（`RebuildResultListFromCache`） | `visibleRows.push_back(row)` —— 按值拷贝每行的 `vector<wstring>` | N 行 × 576 B（cells 部分） | 严重（**`adopt()` 不覆盖**） |
| 4 | `Features/Kernel/KernelPage.cpp:5135` | 缓存投影源 | `fallbackDisplay = currentRows_` —— 拷贝两个 `shared_ptr`；下游 `contiguousSnapshot()` 在**后台线程** | N × 16 B（`detach()` 时的引用表复制） | **判定已下修**（原判严重有误） |
| 5 | `Features/Kernel/KernelPage.cpp:4878` | 应用结果 | `currentRawRows_ = currentRows_.contiguousSnapshot()` 显式物化整份快照 | N 行 × 630 B | 严重 |
| 6 | `Features/Registry/RegistryView.cpp:644` | 刷新快照（`PopulateList`） | `setRows(*rows)` —— 按值形参导致整个 vector 深拷贝 | N 值项 × 630 B | 严重 |
| 7 | `Features/Handle/HandlePage.cpp:690` | 刷新句柄快照 | `setRows(*state_->filterRows)` 整份深拷贝 | N 句柄 × 630 B；N=20000 → **12 MB/次** | 严重 |
| 8 | `Features/Service/ServiceView.cpp:430` | 刷新服务列表 | `setRows(*filterRows)` 整份深拷贝 | N 服务 × 630 B | 严重 |
| 9 | `Features/Driver/DriverObjectView.cpp:536` | 刷新驱动对象 | `setRows(*state.filterRows)` 整份深拷贝 | N 对象 × 630 B | 严重 |
| 10 | `Features/SysTools/EventLogView.cpp:270` | 刷新事件日志 | `setRows(*filterRows)` 整份深拷贝 | N 事件 × 630 B；N=10000 → **6 MB/次** | 严重 |
| 11 | `Features/Process/ProcessView.cpp:673-687` | 构建进程展示行（`BuildPresentationRows`） | `filterRow.cells = row.cells` —— 每行 cells 全量拷贝一次 | N 进程 × (columns+extra) × 32 B | 严重 |
| 12 | `Features/Process/ProcessView.cpp:675` | 同上 | `filterRow.cells = row.cells` 后再 `push_back` 追加全量列文本 | 同上 × 2 | 严重 |
| 13 | `Features/Misc/MiscFeature.cpp:533-535` | 填充审计表 | `BuildAuditVirtualRows` 重建 + `setRows(*filterRows)` 二次深拷贝 | N 审计行 × 630 B × 2 | 严重 |
| 14 | `Features/Driver/DriverObjectView.cpp:514-517` | 筛选回调 | `state.visibleRows.push_back(state.snapshotRows[index])` 按值拷贝整行 | 命中行数 × 行大小 | 严重 |
| 15 | `Features/Misc/MiscFeature.cpp:516-519` | 筛选回调 | 同上，按值拷贝 `MiscAuditRow` | 命中行数 × 行大小 | 严重 |
| 16 | `Features/Driver/DriverOverviewView.cpp:565-568` | 筛选回调 | 同上 | 命中行数 × 行大小 | 严重 |
| 17 | `Features/File/FileView.cpp:461-465` | 目录枚举完成 | `filterRow.cells = row.cells` 每行全量拷贝 | N 文件 × 6 单元格 × 32 B | 中等 |
| 18 | `Features/Startup/StartupView.cpp:336` | 刷新启动项 | `setRows(*filterRows)` 整份深拷贝 | N 启动项 × 630 B | 中等 |
| 19 | `Features/SysTools/FileHolderView.cpp:216` | 刷新文件句柄 | `setRows(*filterRows)` 整份深拷贝 | N × 630 B | 中等 |
| 20 | `Features/SysTools/ContextMenuView.cpp:292` | 刷新右键菜单项 | `setRows(*filterRows)` 整份深拷贝 | N × 630 B | 中等 |
| 21 | `Features/NetTools/NetToolsConnectionView.cpp:305` | 刷新连接 | `setRows(*filterRows)` 整份深拷贝 | N × 630 B | 中等 |
| 22 | `Features/NetTools/NetToolsFirewallView.cpp:296` | 刷新防火墙规则 | `setRows(*filterRows)` 整份深拷贝 | N × 630 B | 中等 |
| 23 | `Features/Privilege/PrivilegeView.cpp:283` | 刷新权限 | `setRows(*filterRows)` 整份深拷贝 | N × 630 B | 中等 |
| 24 | `Features/Driver/DriverUnloadedView.cpp:382` | 刷新已卸载驱动 | `setRows(*filterRows)` 整份深拷贝 | N × 630 B | 中等 |
| 25 | `Features/Driver/DriverOverviewView.cpp:587` | 刷新驱动总览 | `setRows(*state.filterRows)` 整份深拷贝 | N × 630 B | 中等 |
| 26 | `Features/Memory/DriverMemoryView.cpp:329` | 重建内存历史 | `setRows(*rows)` 整份深拷贝 | N 历史 × 630 B | 中等 |
| 27 | `Features/WindowTools/WindowToolsCaptureView.cpp:220` | 刷新截图窗口列表 | `setRows(*shared)` 整份深拷贝 | N × 630 B | 中等 |
| 28 | `Features/WindowTools/WindowToolsHierarchyView.cpp:374` | 刷新窗口层级 | `setRows(*shared)` 整份深拷贝 | N × 630 B | 中等 |
| 29 | `Features/WindowTools/WindowToolsHotkeyView.cpp:179` | 刷新热键 | `setRows(*shared)` 整份深拷贝 | N × 630 B | 中等 |
| 30 | `Ui/EvidenceSessionView.cpp` | 证据会话刷新（5 处同步调用点：增删证据 / 清空 / `WM_CREATE` / 刷新按钮 / 重显窗口） | `DeleteAllItems` + 逐行重建：`std::to_wstring`×2 + `TimestampText` 格式化 | N 证据 × (5 单元格格式化 + 5 次跨进程消息) | 严重 → 已修复 |
| 31 | `Features/Kernel/KernelPage.cpp:7052-7168` | 回调拦截面板（`PopulateCallbackInterceptPanel`） | `DeleteAllItems`×4 + 逐行 `rowValue` 线性扫描列 + `AddListRow` 逐格插入；**且无重绘抑制**（本文件唯一没有 `ScopedListViewRedrawLock` 的批量插入路径） | N 回调行 × (11 列 × 线性扫描 + 11 次跨进程消息)；规则侧 2N 行插入 × 无 `WM_SETREDRAW` 抑制 | 严重 |
| 32 | `Features/Kernel/KernelPage.cpp:4059-4079` | 切页保存辅助表 | `CaptureReportListRows` 把每个单元格 `ListViewText` 取出重建 `vector<wstring>` | 小表（几十行） | 可接受（边界） |
| 33 | `Features/Window/WindowView.cpp:1052` | 选中窗口 | `const WindowSnapshotRow input = *row;` 按值拷贝整行到 lambda | 单行 | 可接受（边界） |
| 34 | `Features/Kernel/KernelPage.cpp:4082-4100` | 切页恢复辅助表 | `DeleteAllItems` + 逐行 `ListViewInsertRow` 重插 | 小表 | 可接受（边界） |

> 与 `docs/audit/list-virtualization-survey.md`（ListSurvey-step5）交叉核对：#6～#29 这 18 处 `setRows(*shared)` 与该报告「次要问题」一节的清单**完全一致**（双方独立统计，同为 18 处）。#30、#31 两处「UI 线程全量重建」由 ListSurvey-step5 补充发现，已并入本报告。

---

## 逐个说明

### 1. `Features/Kernel/KernelPage.cpp:5064` — `ImmutableTextRows` 隐式物化整份快照 ⚠️ 部分已消解（`2256f567`）

**代码**
```cpp
// KernelPage::RebuildResultListFromCache  (KernelPage.cpp:5058)
void KernelPage::RebuildResultListFromCache() {
    const std::vector<std::vector<std::wstring>>& sourceRows = currentRows_;   // ← 5562 隐式转换
    const std::vector<int>& sourceIndents = currentRowIndents_;
    ClearResultTable();
    ...
```

**问题**
`currentRows_` 的类型是 `Ksword::Ui::ImmutableTextRows`（`Features/Kernel/KernelPage.h:394`）。把它绑定到 `const std::vector<std::vector<std::wstring>>&` 会触发 `Ui/ImmutableTextRows.h:86` 的 `operator const Values&()`，其内部调用 `contiguousSnapshot()`（`Ui/ImmutableTextRows.h:89`）：

```cpp
std::shared_ptr<const Values> contiguousSnapshot() const {
    if(!flat_){
        auto flat=std::make_shared<Values>();flat->reserve(rows_->size());
        for(const auto& row:*rows_) flat->push_back(*row);   // ← 每行一次 vector<wstring> 深拷贝
        flat_=std::move(flat);
    }
    return flat_;
}
```

这一步在 **UI 线程**上把 N 个 `shared_ptr<const Row>` 物化成 N 个独立的 `vector<wstring>`，即**整份快照的完整深拷贝**。项目的正例设计（`ImmutableTextRows` 用 `RowRef` 共享行）在这条路径上被完全绕过。

**⚠️ 判定修正（`2256f567` 之后）**：`adopt()` 把 `flat_` 预置为被接管的 vector，因此**紧跟 `adopt()` 之后的第一次隐式转换是零拷贝的**（已用最小复现验证：`adopt` → `operator const Values&` 返回的 `data()` 与原 vector 同一地址）。

但**这个收益会被 `RebuildResultListFromCache` 自身的收尾清掉**：`KernelPage.cpp:5113` 的 `currentRows_ = std::move(visibleRows)` 走 `ImmutableTextRows.h:52` → `assign(Values&&)`（`ImmutableTextRows.h:101`），其中 `flat_.reset()`。于是**下一次**重建时 `flat_` 又为空，隐式物化照旧发生。

即：`adopt()` 把「每次重建物化一次」压缩成「**隔次**重建物化一次」，但没有根除。

**规模估计**
N=5000 行 × 630 B/行 ≈ **3.1 MB** 分配 + 拷贝，发生在切页 / 重建结果表（`adopt()` 之后隔次触发）。

**改法方向**
不要把 `currentRows_` 隐式转成扁平 `Values`。改为直接用 `ImmutableTextRows` 的迭代器 / `snapshot()`（返回 `shared_ptr<const Refs>`）遍历行；行文本缓冲保持共享。需要扁平形态时（例如导出、序列化），改在**工作线程**里做一次物化，或让调用方直接消费 `RowRef`。
**根治点**：让 `:5113` 的赋值不经过 `assign(Values&&)`（例如新增一个「按索引重建 Refs」的共享式入口），使 `flat_` 不被清空。

---

### 2. `Features/Kernel/KernelPage.cpp:5704` — 每次筛选全量重拷 raw 快照 ✅ 已修复（`2256f567`）

**代码**
```cpp
// KernelPage::ApplyCachedFilter  (KernelPage.cpp:5694)
void KernelPage::ApplyCachedFilter(const KernelFeatureId featureId) {
    ...
    currentColumns_ = currentRawColumns_;
    currentRows_ = *currentRawRows_;          // ← 原 5704（已改为 adopt）
    if (IsObjectNamespaceFeature(descriptor->id)) {
        RebuildObjectNamespaceListFromCache(descriptor->id);
    ...
```

**问题**
`currentRawRows_` 是 `std::shared_ptr<const KernelRows>`（`KernelPage.h:399`），`KernelRows` = `std::vector<std::vector<std::wstring>>`。`currentRows_ = *currentRawRows_` 落到 `Ui/ImmutableTextRows.h:51` 的 `operator=(const Values& values)` → `assign(values)`（`ImmutableTextRows.h:100`）：

```cpp
void assign(const Values& values){
    auto refs=std::make_shared<Refs>();refs->reserve(values.size());
    for(const auto& row:values) refs->push_back(std::make_shared<const Row>(row));  // ← 每行深拷贝
    rows_=std::move(refs);flat_.reset();
}
```

**N 行，每行一次 `vector<wstring>` 拷贝构造**。而 `currentRawRows_` 本身已经是不可变快照，完全可以直接共享 —— `ImmutableTextRows` 已经提供了 `reorder()`（`ImmutableTextRows.h:76`）这种共享式重排，说明设计上支持共享，只是这里没走那条路。

**这一模式在 `KernelPage.cpp` 里有 4 处，不止 1 处**（首轮审计只抓到筛选那一处，本轮复核时发现）：

| 行号 | 触发场景 | 交互类型 |
|---|---|---|
| `:1234` | `DeviceDriverObjects` 特性 —— 设备驱动目录/类型 combo 切换 | `WM_COMMAND` / `CBN_SELCHANGE` |
| `:1249` | `BaseNamedObjects` 特性 —— BaseNamed 作用域/类型 combo 切换 | `WM_COMMAND` / `CBN_SELCHANGE` |
| `:1263` | 风险过滤 / 证据含非模块项 勾选框 | `WM_COMMAND` / `BN_CLICKED` |
| `:6202` | 筛选框输入（200ms 防抖） | 定时器 → `ApplyCachedFilter` |

前三处是**用户点击 combo 或勾选框时的同步响应** —— 比筛选打字更敏感（每次点击立即触发一次全量深拷贝 + 一次 `Rebuild*ListFromCache` 重建）。

**规模估计**
N=5000 行 × 630 B ≈ **3.1 MB/次**。
- `:6202`：每次筛选触发一次（200ms 防抖定时器驱动 —— 定时器由 `KernelPage::ScheduleCachedFilter`（定义于 `:5743`，函数体 `:5743-5754`）的 `::SetTimer(hwnd_, kTimerR0EvidenceFilter, 200, nullptr)`（`:5750`）设置，到期后才进 `ApplyCachedFilter`）—— 用户在筛选框打字就是持续触发。
- `:1234` / `:1249` / `:1263`：每次 combo 切换或勾选**立即**触发一次。

**改法方向**
`ImmutableTextRows` 需要一个「从已有 `shared_ptr<const Values>` 接管」的入口：直接构造 `Refs`（每个元素指向 `*rows` 里的 `Row`，不拷贝文本），而不是逐行 `make_shared<const Row>(row)`。这样 `currentRows_` 与 `currentRawRows_` 共享同一批行缓冲。
**4 处要一起改** —— 只改筛选那一处，combo 切换与勾选框仍然会整表深拷贝。

> **✅ 已修复（`2256f567`）**：`Ui/ImmutableTextRows.h` 新增了 `adopt(std::shared_ptr<const Values>)`，
> `KernelPage.cpp` 的 4 处调用点已改为 `currentRows_.adopt(currentRawRows_)`。
> 该入口的实现同时把 `flat_` 指向被接管的 vector，因此**顺带消除了 #1/#5 的重复物化** —— 这是本报告建议的正确形态。
> 本条目保留作为回归防线：`adopt()` 的语义要求交接后调用方不再修改该 vector。

---

### 3. `Features/Kernel/KernelPage.cpp:5058-5113` — 可见行重建时按值拷贝每行 ⚠️ **当前 Kernel 侧主要残留**

**代码**
```cpp
    std::vector<std::vector<std::wstring>> visibleRows;
    std::vector<int> visibleIndents;
    visibleRows.reserve(sourceRows.size());
    visibleIndents.reserve(sourceRows.size());
    for (std::size_t sourceIndex = 0; sourceIndex < sourceRows.size(); ++sourceIndex) {
        const std::vector<std::wstring>& row = sourceRows[sourceIndex];
        ...
        visibleRows.push_back(row);      // ← 5109 按值拷贝整行
        visibleIndents.push_back(...);
    }
    ...
    currentRows_ = std::move(visibleRows);   // ← 5113 再整体赋给 ImmutableTextRows
```

**问题**
两重浪费：
1. `visibleRows.push_back(row)` —— `row` 是 `const vector<wstring>&`，`push_back` 走拷贝构造，**每行所有单元格的字符串缓冲全部复制**。
2. `currentRows_ = std::move(visibleRows)` —— `visibleRows` 是 `vector<vector<wstring>>`，赋给 `ImmutableTextRows` 触发 `ImmutableTextRows.h:52` `operator=(Values&& values)` → `assign(Values&&)`（`ImmutableTextRows.h:101`），**又一次逐行 `make_shared<const Row>(std::move(row))`**（这里是移动，但仍然 N 次堆分配 + N 次 `shared_ptr` 控制块分配）。

**为什么 `adopt()` 覆盖不到这里**
`adopt()` 消除的是「**从 `currentRawRows_` 接管**」这一层的深拷贝。但本条是**另一层**：`currentRows_`（共享式 `RowRef`）→ `visibleRows`（扁平 `vector<vector<wstring>>`）→ 再回到 `currentRows_`。
中间那个 `visibleRows` 是**代码自己造的**扁平副本，`adopt()` 没有任何入口能接管它 —— 这层拷贝必须靠改代码结构消除，不能靠换一个赋值入口解决。

**这一层还连带毁掉了 #1 的修复收益**：`assign(Values&&)` 里的 `flat_.reset()` 会把 `adopt()` 预置的 `flat_` 缓存清空（见 #1 条目）。

**规模估计**
第 1 步：N × (24 + 32×列数 + 单元格堆) ≈ N × 576 B（6 列）。N=5000 → **2.9 MB**。
第 2 步：N 次 `make_shared` ≈ N × (48 B 控制块 + 24 B vector 头) ≈ N × 72 B → 360 KB 分配。

**改法方向**
可见集应该只存**索引**（`vector<size_t>`），行数据一律回到 `currentRows_` 里按索引取；`currentRows_` 用 `ImmutableTextRows::reorder()` 或等价的共享式重建，避免 `vector<vector<wstring>>` 中转。

---

### 4. `Features/Kernel/KernelPage.cpp:5135` — `fallbackDisplay` 第二份快照（**判定已下修**，见下）

**代码**
```cpp
std::shared_ptr<const KernelProjectionSource> KernelPage::CachedProjectionSource(const KernelFeatureId featureId) {
    ...
    auto source=std::make_shared<KernelProjectionSource>();
    source->featureId=featureId;source->serial=++projectionSourceSerial_;
    source->columns=!currentRawColumns_.empty()?currentRawColumns_:currentColumns_;
    source->rows=currentRawRows_;
    if(source->rows->empty()) source->fallbackDisplay=currentRows_;   // ← 5633
    ...
```

**⚠️ 判定修正：本条原判为「严重 · N 行 × 630 B 整表物化」，这是错的。**
`fallbackDisplay` 的类型是 `Ksword::Ui::ImmutableTextRows`（`Features/Kernel/KernelProjectionValues.h:22`），`currentRows_` 也是 `ImmutableTextRows`（类型成员声明，`Features/Kernel/KernelPage.h:394`）。
所以 `source->fallbackDisplay = currentRows_` 走的是 `ImmutableTextRows.h:41` 的**默认拷贝赋值运算符** —— 只拷贝两个 `shared_ptr`（`rows_` 与 `flat_`），**不触碰任何单元格文本**。它不会在 UI 线程上物化整份快照。

真正会物化的是**下游**的 `fallbackDisplay.contiguousSnapshot()`，而那一处发生在**后台线程**（`Features/Kernel/KernelProjectionValues.inl:15`，位于 `KernelValueProjectionBuilder` 构造函数内，由投影任务驱动）—— 正是本报告认定的正确位置。

**残留的真实代价**：`fallbackDisplay` 与 `currentRows_` 各自持有一份 `rows_` 引用，使 `rows_.use_count() > 1`。之后任何一次对 `currentRows_` 的 `push_back`/`reserve` 都会触发 `ImmutableTextRows.h:99` 的 `detach()`：

```cpp
void detach(){flat_.reset();if(rows_.use_count()!=1)rows_=std::make_shared<Refs>(*rows_);}
```

→ 复制一张 N 项的 `shared_ptr` 引用表（**不是**文本拷贝）。

**规模估计**
N × 16 B（`shared_ptr<const Row>` = 2 指针）。N=5000 → **80 KB** + N 次原子递增。
比 #1/#3（3 MB 级）低约 **两个数量级**。

**改法方向**
让 `KernelProjectionSource` 只持有**一个**权威快照引用（`shared_ptr<const Rows>` 或 `ImmutableTextRows`），`fallbackDisplay` 不要另存一份；或者让 `ImmutableTextRows::detach()` 在 `use_count()==1` 时不复制（当前实现已经是这个语义，问题在于调用方持有第二份引用导致 `use_count()>1`）。
**本条已由 `2256f567` 的作者复核确认「只拷贝 shared_ptr，本来就不贵，保持原样」** —— 与本报告修正后的判定一致。

---

### 5. `Features/Kernel/KernelPage.cpp:4878` — 显式物化整份快照

**代码**
```cpp
    } else {
        for (std::size_t rowIndex = 0; rowIndex < result.rows.size(); ++rowIndex) {
            AddDynamicResultRow(static_cast<int>(rowIndex), result.rows[rowIndex], orderedColumns);
        }
        currentRawRows_ = currentRows_.contiguousSnapshot();   // ← 5376
    }
```

**问题**
`contiguousSnapshot()` 是显式的全量物化：N 行 → N 个独立 `vector<wstring>`。这是**唯一显式**的一处，容易被误认为"这里就是要一份扁平副本"，但：
- 它把 `currentRows_`（共享式 `RowRef`）的共享优势彻底抹掉；
- `currentRawRows_` 的下游消费者都只是在**只读遍历**：`KernelPage.cpp` 里 **4 处** —— `:5565`、`:6003`、`:10904`（三处均为 `const std::vector<std::vector<std::wstring>>& rawRows = !currentRawRows_->empty() ? *currentRawRows_ : currentRows_;`）与 `:10981`（`FirstRowUInt64(!currentRawRows_->empty() ? ...)` 内联形态）；`Features/Kernel/KernelProjectionValues.inl` 里 **10 处** —— `:186`、`:394`、`:496`、`:695`、`:884`、`:1077`、`:1441`、`:1644`、`:2064`（同一 `rawRows = ...` 形态）与 `:1721`（`FirstRowUInt64(...)` 内联形态）；
- 这些只读遍历完全可以直接走 `currentRows_` 的 `RowRef`，不需要扁平副本。

**规模估计**
N × 630 B。N=5000 → **3.1 MB**。

**改法方向**
`currentRawRows_` 改为持有 `shared_ptr<const ImmutableTextRows>`（或直接复用 `currentRows_`），下游 13+ 处 `rawRows` 遍历改成走 `RowRef` 迭代。

---

### 6. `Features/Registry/RegistryView.cpp:644` — `setRows(*rows)` 深拷贝整份快照 ✅ 已修复（`969604fc`）

**代码**
```cpp
void PopulateList(RegistryViewState& state) {
    ...
    auto rows = std::make_shared<std::vector<Ksword::Ui::VirtualListRow>>();
    rows->reserve(state.snapshot.rows.size());
    for (std::size_t index = 0; index < state.snapshot.rows.size(); ++index) {
        ...
        displayRow.cells = { row.name.empty() ? std::wstring(L"(Default)") : row.name,
                             row.typeText, row.dataText, row.detailText };
        rows->push_back(std::move(displayRow));
    }
    {
        Ksword::Ui::ScopedListViewRedrawLock listRedraw(state.list.hwnd());
        state.list.setRows(*rows);      // ← 781 整份深拷贝
        state.list.setVisibleIndexes({});
    }
    state.filterRows = std::move(rows);
```

**问题**
`rows` 已经是 `shared_ptr<vector<VirtualListRow>>`，且随即被存进 `state.filterRows` 供后台筛选任务共享。但 `setRows` 的形参是**按值** `std::vector<VirtualListRow>`（`Ui/VirtualListView.h:46`），因此 `setRows(*rows)` 会：
1. 拷贝构造整个 `vector<VirtualListRow>`（N 次 `VirtualListRow` 拷贝 → 每行一次 `vector<wstring>` 深拷贝 + `wstring stableKey` 拷贝）；
2. 再 `make_shared<const vector<VirtualListRow>>(std::move(rows))` 装进控件。

**同一个快照在内存里因此存在两份**（`state.filterRows` 一份、控件内部一份），且第二份是逐字节拷贝出来的。

**规模估计**
N 个注册表值项 × 630 B。N=2000 → **1.26 MB/次**，每次刷新 / 每次路径跳转触发。

**改法方向**
改用 `state.list.setSharedRows(rows)`。但注意 `setSharedRows` 要求 `shared_ptr<const vector<VirtualListRow>>`，而 `rows` 是 `shared_ptr<vector<...>>`；需要把 `rows` 声明为 `shared_ptr<const ...>`（在填充完成后再冻结），或在交接后用 `const_pointer_cast` 语义对齐。**注意交接后调用方绝不能再修改该 vector**（`Ui/VirtualListView.h:47-48` 明确要求）。

---

### 7. `Features/Handle/HandlePage.cpp:690` — 句柄快照双份常驻 ✅ 已修复（`45ca93b0`）

**代码**
```cpp
void HandlePage::PopulateList() {
    ...
    if (!state_->filterRows) {
        return;
    }
    state_->handleList.setRows(*state_->filterRows);   // ← 690 整份深拷贝
    RequestFilter(filterBar_ ? Ksword::Ui::GetFilterBarText(filterBar_) : state_->filterQuery, selectedStableKey, topStableKey);
}
```

**问题**
`state_->filterRows` 是 `std::shared_ptr<const std::vector<Ksword::Ui::VirtualListRow>>`（`HandlePage.cpp:93`），**已经是不可变共享快照**。`setRows(*state_->filterRows)` 因为按值形参，把它完整复制了一份到控件内部。同一个句柄快照在内存里同时存在两份完整拷贝。

**规模估计**
句柄表是本项目行数最多的快照之一。一台典型 Windows 机器上单进程句柄数可达 **2 万～10 万**（每个句柄行约 6 列）。
- N=20000 → **12.6 MB/次**拷贝 + 12.6 MB 常驻内存翻倍。
- N=100000 → **63 MB/次**。

**改法方向**
`setSharedRows(state_->filterRows)` —— `state_->filterRows` 类型已经是 `shared_ptr<const ...>`，**签名直接匹配，零改动即可消除整次拷贝**。这是全项目性价比最高的一处修改。

---

### 8-10. `ServiceView.cpp:430` / `DriverObjectView.cpp:536` / `EventLogView.cpp:270` — 同构的 `setRows(*filterRows)` ✅ 已修复（`45ca93b0`）

**代码（ServiceView，三处结构一致）**
```cpp
    auto filterRows = std::make_shared<std::vector<Ksword::Ui::VirtualListRow>>(std::move(rows));
    state.serviceList.setRows(*filterRows);      // ← ServiceView.cpp:430
    state.filterRows = std::move(filterRows);
    ++state.displayGeneration;
```

**问题**
与 #6/#7 同构：`filterRows` 随即被存进 `state.filterRows` 供后台筛选任务共享，但 `setRows(*filterRows)` 又完整复制了一份。

**规模估计**
- 服务：N ≈ 300～500 → **0.2～0.3 MB/次**
- 驱动对象：N ≈ 500～3000 → **0.3～1.9 MB/次**
- 事件日志：N 可达 **5000～50000**（每条事件一行）→ **3～31 MB/次**

**改法方向**
统一改为 `setSharedRows`。注意这三处的 `filterRows` 是 `shared_ptr<vector<...>>`（非 const），需要把类型改成 `shared_ptr<const ...>` 或在交接后不再修改。

---

### 11-12. `Features/Process/ProcessView.cpp:673-687` — 进程展示行双重 cells 拷贝

**代码**
```cpp
        Ksword::Ui::VirtualListRow filterRow{};
        filterRow.stableKey = row.stableKey;
        filterRow.cells = row.cells;                                  // ← 675 第一次拷贝
        // 全量列文本仅用于筛选，不会改变当前 ListView 的可见列布局。
        if (const ProcessSnapshotRow* process = model.rowForDisplayRow(sourceRow)) {
            for (const ProcessColumnDescriptor& column : ProcessColumnDescriptors()) {
                filterRow.cells.push_back(ProcessColumnText(*process, column.id));  // ← 679 追加
            }
        }
```

**问题**
两处拷贝叠加：
1. `filterRow.cells = row.cells` —— 拷贝 `row.cells`（可见列，约 8 列）的全部字符串缓冲；
2. 紧接着 `push_back` 追加**全量列文本**（`ProcessColumnDescriptors()` 全部列，可能十几列）。

结果 `filterRow.cells` 是 `row.cells` 的超集，但两者都完整存在内存里 —— 每个进程的每个单元格文本存在两份。

**规模估计**
N 进程 × (可见列 8 + 全量列 ~14) × 32 B 结构 + 堆 ≈ N × 700 B。
N=800 → **0.56 MB/次**；进程多开时 N=3000 → **2.1 MB/次**。

**改法方向**
`filterRow.cells` 不要从 `row.cells` 拷贝开始。改为一次性构造：先 `reserve` 总列数，再逐列 push。`row.cells` 与 `filterRow.cells` 若列集合有重叠，可考虑列文本也走共享（用 `shared_ptr<const wstring>` 或 `ImmutableTextRows` 的行共享）。

---

### 13. `Features/Misc/MiscFeature.cpp:533-535` — 审计表重建 + 二次深拷贝

**代码**
```cpp
void PopulateAuditList(MiscAuditViewState& state) {
    if (!state.list) { return; }
    state.filterRows = std::make_shared<const std::vector<Ksword::Ui::VirtualListRow>>(BuildAuditVirtualRows(state.rows));
    ++state.snapshotGeneration;
    state.virtualList.setRows(*state.filterRows);   // ← 535
    RequestAuditFilter(state, ...);
}
```

**问题**
`BuildAuditVirtualRows`（定义于 `MiscFeature.cpp:448`）本身已经是**在 UI 线程上重建整份展示行**：每行 `display.stableKey = AuditStableKey(rows[index])`（`row.category + L"\n" + row.item + L"\n" + row.source`，**三次字符串拼接产生新分配**）+ `display.cells = AuditCells(rows[index])`（构造一个 6 元素 `vector<wstring>` 临时量再拷贝赋值）。然后 `setRows(*state.filterRows)` **再完整复制一遍**。

一次刷新 = **2 份整份快照**（构建一份 + 拷贝一份），外加每行 3 次字符串拼接。

**规模估计**
N 审计行（注册表/服务/WMI/R0 混合）× 630 B × 2。
N=2000 → **2.5 MB/次**；N=10000 → **12.6 MB/次**。

**改法方向**
1. `BuildAuditVirtualRows` 移出 UI 线程（放进 `AsyncSnapshotTask` 的 Work 里），让 `Deliver` 只做交接；
2. `setRows(*filterRows)` → `setSharedRows(state.filterRows)`（类型已是 `shared_ptr<const ...>`，签名直接匹配）。

---

### 14-16. `DriverObjectView.cpp:514-517` / `MiscFeature.cpp:516-519` / `DriverOverviewView.cpp:565-568` — 筛选回调里按值拷贝整行

**代码**
```cpp
            state.visibleRows.clear();
            state.visibleRows.reserve(result->visibleIndexes.size());
            for (const std::size_t index : result->visibleIndexes) {
                if (index < state.rows.size()) {
                    state.visibleRows.push_back(state.rows[index]);   // ← 按值拷贝整行
                }
            }
            state.virtualList.setVisibleIndexes(std::move(result->visibleIndexes));
```

**问题**
`state.rows` 是 `std::vector<MiscAuditRow>`（`MiscFeature.cpp:120`），`MiscAuditRow` 内含多个 `std::wstring` 字段。`push_back(state.rows[index])` 按值拷贝整条记录（含所有字符串缓冲）。

而 `visibleRows` 的**唯一用途**是计算 stable key 以恢复选中和滚动位置：
```cpp
std::wstring StableKeyAt(const MiscAuditViewState& state, const int visibleIndex) {
    ...
    return AuditStableKey(state.visibleRows[static_cast<std::size_t>(visibleIndex)]);
}
```
（`MiscFeature.cpp:461-466`）—— 只需要 key，不需要整行数据。

**规模估计**
命中行数 × `MiscAuditRow` 大小（约 6 个 `wstring` = 192 B 结构 + 堆）。
N=2000 全命中 → **0.4～1 MB/次**，**每次筛选触发**。

**改法方向**
`visibleRows` 改为存 `std::vector<std::size_t>`（索引）或 `std::vector<std::wstring>`（**只存 key**）。需要 key 时用 `AuditStableKey(state.rows[index])` 现算，或在构建阶段预计算一个 `vector<wstring>` key 表并共享。

---

### 17. `Features/File/FileView.cpp:461-465` — 目录项 cells 拷贝

**代码**
```cpp
    for (const FileEntry& entry : state.entries) {
        FilePresentationRow row{};
        row.cells = { entry.name, EntryTypeText(entry),
                      FileSystemEnumerator::formatSize(entry.size, entry.kind),
                      FileSystemEnumerator::formatLastWriteTime(entry.lastWriteTime),
                      FileSystemEnumerator::formatAttributes(entry.attributes),
                      entry.fullPath };
        Ksword::Ui::VirtualListRow filterRow{};
        filterRow.stableKey = entry.fullPath;
        filterRow.cells = row.cells;                    // ← 463 每行 6 单元格全量拷贝
        state.presentationRows.push_back(std::move(row));
        filterRows->push_back(std::move(filterRow));
    }
```

**问题**
`row.cells` 与 `filterRow.cells` 内容完全相同（FileView 的 filter 行就是展示行），却各自持有一份字符串缓冲副本。另外 `formatSize` / `formatLastWriteTime` / `formatAttributes` 三次格式化在 UI 线程做。

**规模估计**
N 目录项 × 6 单元格 × (32 B 结构 + ~48 B 堆) ≈ N × 480 B。
N=5000（大目录）→ **2.4 MB/次**。

**改法方向**
`filterRow.cells` 与 `row.cells` 共享同一份缓冲（例如两者都持有 `shared_ptr<const vector<wstring>>`），或者让筛选直接复用 `presentationRows`（筛选已经在 `state.filterRows` 上跑，那是独立的一份）。

---

### 18-29. 其余 `setRows(*shared)` 同构点 ✅ 已修复（`45ca93b0`）

**代码（StartupView，其余 11 处结构完全一致）**
```cpp
    auto filterRows = std::make_shared<std::vector<Ksword::Ui::VirtualListRow>>(std::move(rows));
    state.entryList.setRows(*filterRows);      // ← StartupView.cpp:336
    state.filterRows = std::move(filterRows);
```

**问题**
与 #6～#10 同构：`setRows` 按值形参 → 整份快照深拷贝；且该 vector 随即被存为共享快照供后台筛选使用，导致同一份数据常驻两份。

**完整清单**
| 文件:行 | 快照 | 典型 N | 单次拷贝量 |
|---|---|---|---|
| `Features/Startup/StartupView.cpp:336` | 启动项 | 100～500 | 0.06～0.3 MB |
| `Features/SysTools/FileHolderView.cpp:216` | 文件句柄 | 100～2000 | 0.06～1.3 MB |
| `Features/SysTools/ContextMenuView.cpp:292` | 右键菜单项 | 50～500 | 0.03～0.3 MB |
| `Features/NetTools/NetToolsConnectionView.cpp:305` | 网络连接 | 50～500 | 0.03～0.3 MB |
| `Features/NetTools/NetToolsFirewallView.cpp:296` | 防火墙规则 | 100～1000 | 0.06～0.6 MB |
| `Features/Privilege/PrivilegeView.cpp:283` | 权限令牌 | 50～500 | 0.03～0.3 MB |
| `Features/Driver/DriverUnloadedView.cpp:382` | 已卸载驱动 | 20～200 | 0.01～0.13 MB |
| `Features/Driver/DriverOverviewView.cpp:587` | 驱动总览 | 50～500 | 0.03～0.3 MB |
| `Features/Memory/DriverMemoryView.cpp:329` | 内存操作历史 | 100～5000 | 0.06～3.1 MB |
| `Features/WindowTools/WindowToolsCaptureView.cpp:220` | 截图窗口 | 50～500 | 0.03～0.3 MB |
| `Features/WindowTools/WindowToolsHierarchyView.cpp:374` | 窗口层级 | 100～2000 | 0.06～1.3 MB |
| `Features/WindowTools/WindowToolsHotkeyView.cpp:179` | 热键 | 10～100 | < 0.06 MB |

**改法方向**
全部改为 `setSharedRows`，并把 `filterRows` 的类型统一为 `shared_ptr<const std::vector<Ksword::Ui::VirtualListRow>>`。
**注意正例对照**：`Features/Window/WindowView.cpp:1221`、`Features/ProcessDetail/ProcessDetailPage.Module.cpp:670/698/796`、`Features/ProcessDetail/ProcessDetailPage.Thread.cpp:448/481/543`、`Features/HardwareStats/BusDeviceView.cpp:351`、`Features/HardwareStats/UsbTopologyView.cpp:355`、`Features/WindowTools/WindowToolsClipboardView.cpp:367`、`Features/Network/NetworkView.cpp:180`、`Features/Registry/RegistrySearchView.cpp:324` —— 这些页面**已经在用 `setSharedRows`**，说明正例路径完全可用，上表 12 处只是没有跟进。

---

### 30. `Ui/EvidenceSessionView.cpp` — 证据会话全量重建 ✅ 已修复（`dc282eb1`）

**原代码**
```cpp
void RefreshSessionSnapshot(EvidenceSessionViewState& state) {
    state.items = GlobalEvidenceSession().sharedSnapshot();
    if (state.list) {
        ScopedListViewRedrawLock redrawLock(state.list);
        ListView_DeleteAllItems(state.list);
        for (std::size_t index = 0; index < state.items.size(); ++index) {
            const EvidenceItem& item = *state.items[index];
            InsertListViewTextRow(state.list, {
                std::to_wstring(item.sequence),
                TimestampText(item.timestamp100ns),
                item.source,
                item.format,
                std::to_wstring(item.text.size()),
            }, static_cast<LPARAM>(index));
        }
    }
```

**原问题**（三重代价，全在 UI 线程）：
1. **`sharedSnapshot()` 返回值拷贝** —— `Ui/EvidenceSession.h:42` `using SharedSnapshot = std::vector<std::shared_ptr<const EvidenceItem>>`，`Ui/EvidenceSession.cpp:148-151` 的 `sharedSnapshot()`（`:150` 处 `return items_;`）**按值返回 `items_`**，于是 `state.items = ...` 是一次 N 个 `shared_ptr` 的 vector 拷贝（N 次原子递增 + N×16 B 分配）。`EvidenceItem` 内容是共享的，但**容器本身被复制了**。
2. **每次刷新都重新格式化** —— `std::to_wstring(item.sequence)`、`std::to_wstring(item.text.size())` 各产生一个新 `wstring`；`TimestampText` 走 `FileTimeToLocalFileTime` + `FileTimeToSystemTime` + `swprintf_s`，是最贵的一个。
3. **`DeleteAllItems` + 逐行 `InsertListViewTextRow`** —— 每行 1 次 `ListView_InsertItem` + 4 次 `ListView_SetItemText`，**每次都是跨进程 SendMessage**。

`EvidenceSession` 是**全局累积**的（`Ui/EvidenceSession.cpp:143` `items_.push_back(...)`，无上限裁剪），N 会随会话时长单调增长。N=1000 → **5000 次跨进程消息**。原 `RefreshSessionSnapshot` 有 **5 处同步调用点**，触发方式各异（`dc282eb1` 重写前的行号，以下为重写后当前行号）：

| 调用点 | 触发方式 |
|---|---|
| `Ui/EvidenceSessionView.cpp:365` | 删除单项证据后（`DeleteSelectedItem`） |
| `Ui/EvidenceSessionView.cpp:384` | 清空会话后（`ClearSession`） |
| `Ui/EvidenceSessionView.cpp:483` | `WM_CREATE` 首次创建 |
| `Ui/EvidenceSessionView.cpp:502` | `WM_COMMAND` 刷新按钮（`kRefreshButtonId`） |
| `Ui/EvidenceSessionView.cpp:631` | `ShowEvidenceSessionInspector` 重新显示已存在窗口 |

即：**每一次证据增删、每一次窗口显示都会同步重建整表**，不只是「点刷新才触发」。

**✅ 已按本报告建议改造**（已提交 `dc282eb1`）：
- 列表改为 `VirtualListView`（owner-data，`LVS_OWNERDATA`）→ **插入成本归零**（不再有 `DeleteAllItems` + 逐格跨进程消息）
- 行表构建移入 `AsyncSnapshotTask` 的 Work（`BuildSnapshotRows`）→ **格式化与物化成本离开 UI 线程**
- `state.list.setSharedRows(std::move(result.rows))` → **快照零拷贝交接**
- `kMaxDisplayRows` 截断 + 状态文本提示「仅显示前 X 项 / 共 Y 项」→ **有固定上界，且不误导**
- 会话快照本身也在 worker 上取（`GlobalEvidenceSession().sharedSnapshot()` 在 Work 内调用），删除/清空能立即反映到构建出的行表

**本条目保留作为回归防线**：新实现引入了一条值得注意的约束 —— `EvidenceSnapshotRows` 把「行表」与「快照」打包一起传递，注释写明「The two travel together so a row index can never address a different evidence item than the one the row was built from」。这是因为 owner-data 控件的 `itemData` 存的是**快照下标**而非行位置，`ModelIndexForPosition`（`Ui/EvidenceSessionView.cpp`）需要经 `visibleIndexes()` 两步映射才能回到快照下标。后续若拆开这两个容器，会引入「行指向了另一个证据项」的错误。

---

### 31. `Features/Kernel/KernelPage.cpp:7052-7168` — 回调拦截面板全量重建

**代码**
```cpp
void KernelPage::PopulateCallbackInterceptPanel() {
    ...
    ListView_DeleteAllItems(callbackGroupList_);
    for (HWND ruleList : callbackRuleLists_) {
        ListView_DeleteAllItems(ruleList);
    }
    if (callbackBypassList_) { ListView_DeleteAllItems(callbackBypassList_); }
    if (callbackFileMonitorList_) { ListView_DeleteAllItems(callbackFileMonitorList_); }
    RenderCallbackLocalModel();
    ...
    for (const std::vector<std::wstring>& row : currentRows_) {      // ← 遍历整份快照
        const std::wstring section = rowValue(row, currentColumns_, { L"Section" });
        ...
        AddListRow(callbackFileMonitorList_, {
            rowValue(row, currentColumns_, { L"Time" }),
            rowValue(row, currentColumns_, { L"PID", L"Pid" }),
            ...                                                        // ← 11 个格子
        });
    }
```

**问题**
三重代价：

1. **4+ 次 `DeleteAllItems`** —— 每次都丢弃并重建全部行，且**没有行数上限**。
2. **`rowValue` 是 O(列数) 线性扫描，且按值返回 `std::wstring`** —— `Features/Kernel/KernelPage.cpp:7085-7094`：
   ```cpp
   const auto rowValue = [](const std::vector<std::wstring>& row, const std::vector<std::wstring>& columns,
                            std::initializer_list<const wchar_t*> names) -> std::wstring {
       for (const wchar_t* name : names) {
           for (std::size_t column = 0; column < columns.size() && column < row.size(); ++column) {
               if (_wcsicmp(columns[column].c_str(), name) == 0 && !row[column].empty()) {
                   return row[column];      // ← 按值返回：一次 wstring 拷贝
               }
           }
       }
       return {};
   };
   ```
   每个 `rowValue` 调用 = **别名数 × 列数**次 `_wcsicmp` 字符串比较 + **一次 `wstring` 拷贝**（返回值按值）。
3. **`AddListRow` 逐格插入** —— `Features/Kernel/KernelPage.cpp:686-700`：1 次 `ListView_InsertItem` + (列数-1) 次 `ListView_SetItemText`，**每次都是跨进程 SendMessage**。

**第 4 重代价（重绘口径，本报告原未记录，由 TreeSurvey-bunny 提出并经我核实）**

回调面板是 `KernelPage.cpp` 里**唯一没有重绘抑制的批量插入路径**。同文件其他列表重建路径都套了 `Ksword::Ui::ScopedListViewRedrawLock`（`Ui/ListViewUtil.h:57`，内部对 ListView 与 Header 各持一把 `ScopedWindowRedrawLock`，即 `WM_SETREDRAW(FALSE)` + 退出时一次 invalidate）：
- `KernelPage.cpp:4089`（`RestoreReportListRows`）
- `KernelPage.cpp:4841-4843`（`ApplyCachedValueProjection`）
- `KernelPage.cpp:5234`
- `KernelPage.cpp:5840`

而 `PopulateCallbackInterceptPanel`（定义于 `:7052`）**一个都没有** —— 于是 `DeleteAllItems` ×4 + 逐行 `AddListRow` 的每一次插入都触发一次列表重绘。

**放大倍数**：规则侧每条规则插 **2 行**（一条规则行 + 一条「发起程序匹配 / 目标程序匹配 / 备注」说明行，`KernelPage.cpp:7257` 与 `:7269`），每行 10~11 列。N 条规则 → **2N 行插入**，每次一行 10~11 次跨进程消息。

**且触发频次远高于「刷新时一次」**：`RenderCallbackLocalModel`（定义于 `:7232`）自身也做 `DeleteAllItems`（调用点 `:7238`、`:7240-7242`）+ 逐行 `AddListRow`（调用点 `:7243` 起），它有 **12 处调用点**，**全部是用户交互**：

| 调用点 | 触发动作 | 所属函数（起始行） |
|---|---|---|
| `:7073` | 面板填充 | `PopulateCallbackInterceptPanel` `:7052` |
| `:7514` | 新增规则组 | `OnCallbackAddGroup` `:7500` |
| `:7532` | 删除规则组 | `OnCallbackRemoveGroup` `:7518` |
| `:7547` | 重命名规则组 | `OnCallbackRenameGroup` `:7536` |
| `:7576` | 上下移动规则组 | `OnCallbackMoveGroup` `:7554` |
| `:7588` | 启用/禁用规则组 | `OnCallbackToggleGroupEnabled` `:7580` |
| `:7629` | 新增规则 | `OnCallbackAddRule` `:7596` |
| `:7646` | 删除规则 | `OnCallbackRemoveRule` `:7633` |
| `:7677` | 上下移动规则 | `OnCallbackMoveRule` `:7650` |
| `:7694` | 启用/禁用规则 | `OnCallbackToggleRuleEnabled` `:7681` |
| `:7813` | 粘贴规则文本 | `OnCallbackPasteRuleText` `:7741` |
| `:8157` | 导入配置文件 | `ApplyCallbackFileIoResult` `:8126` |

**→ 每次规则增删改操作都会整表重建并全量重绘。**（行号与函数归属由 TreeSurvey-bunny 提供，我逐条 `sed -n` 复核命中。）

> 这条属**重绘口径**而非拷贝口径，但形态上与本条目同源，且改法与前三条同批落地（owner-data 虚拟表天然消除逐行插入与重绘）。故记入本条目。

**规模估计**
N 回调运行时行（CallbackIntercept 快照通常数百到数千行）。FileMonitor 分支每行 **11 个格子、9 次 `rowValue` 调用**（`:7148` 的 `FsctlName` 与 `:7149` 的 `ControlCode` 提前取出，`:7160`/`:7161` 复用，不重复调用）。
- 每行：约 9 × (2~3 别名 × ~15 列) ≈ **250+ 次 `_wcsicmp`** + **9 次 wstring 拷贝** + **11 次跨进程消息**（11 格 → 1 次 `ListView_InsertItem` + 10 次 `ListView_SetItemText`）
- N=1000 → **25 万次字符串比较** + **1.1 万次跨进程消息** ≈ 数百 ms ~ 秒级卡顿

**触发路径**
`Features/Kernel/KernelPage.cpp:1271`（数据到达回调）与 `:4943`（`RebuildR0EvidenceListFromCache` 之后的特性重建路径）。**两者都在 UI 线程同步执行。**

**改法方向**
1. **列名 → 列下标 的映射表预计算一次**（`currentColumns_` 变化时才重算），让 `rowValue` 从 O(别名×列) 降到 O(1) 查表。
2. `rowValue` 改为返回 `const std::wstring&` 或 `std::wstring_view`（配合一个"未命中"哨兵），消除每次调用的 wstring 拷贝。
3. 这 4 个 ListView 改用 owner-data 虚拟表 + 共享行快照 —— 与 #30、#32 同一条改造路径：切页/刷新时只换快照指针，不做 `DeleteAllItems` + 逐格插入。
4. **先行止血（不必等虚拟化改造）**：给 `callbackFileMonitorList_` 等 4 个 ListView 分别套 `kMaxDisplayRows`（常量定义，`Ui/ListViewUtil.h:15`）截断 + `RowCapNotice`（函数，定义于 `Ui/ListViewUtil.h:116`）提示。详见文末「附录」。
5. **补重绘抑制（独立可上线，性价比最高的单点改动）**：回调面板是 `KernelPage.cpp` 里唯一没套 `Ksword::Ui::ScopedListViewRedrawLock`（`Ui/ListViewUtil.h:57`）的批量插入路径 —— 同文件其他批量插入路径都套了（**取锁语句在下列各行**，函数定义一并给出）：
   - `RestoreReportListRows`（定义于 `:4082`）—— 取锁语句在 `:4089`
   - `RenderResult`（定义于 `:4840`）—— 取锁语句在 `:4841-4843`
   - `ApplyCachedValueProjection`（定义于 `:5191`）—— 取锁语句在 `:5234`
   - `RequestResultSort`（定义于 `:5785`）—— 取锁语句在 `:5840`**不需要虚拟化改造，直接把「每次插入一次重绘」压成「整批一次重绘」。**

   > **注**：`RenderResult` 里紧挨着的第 4 把锁 `:4844`（`ScopedWindowRedrawLock objectTreeLock(objectNamespaceTree_)`）作用于**内核树**，且**在 `RenderResult`（定义于 `:4840`，函数体 `:4840-4988`，下一个函数 `AddDynamicResultColumns` 起于 `:4990`）全函数体内只出现这一行、并不操作该树 —— 是空转的**。
   > 更准确地说：**树控件的重绘抑制事实上是「零覆盖」** —— 内核树的插入/删除循环本身就不取锁（`PumpObjectTreeProjection`，`KernelPage.cpp:5378-5438`，下一个函数 `RebuildObjectNamespaceTreeFromCurrentRows` 起于 `:5440`；内含真正的树插入 `TreeView_InsertItem`（调用点 `:5425`），但全函数 `WM_SETREDRAW` / `ScopedWindowRedrawLock` **0 命中**）；而全仓库唯一对树取锁的地方就是这把空转的 `:4844`。
   > 这**不构成缺陷**（树永久隐藏，隐藏控件不绘制，重绘抑制对它本无意义），但**别把它当成「内核页连树都做了重绘抑制」的证据** —— 这也是它与回调面板无关的原因（不是同一批控件）。
   > （树侧的完整结论见 TreeSurvey-bunny 的 `docs/audit/tree-survey.md` §5.2 与 §6.5。）

   > **⚠️ 加锁位置有讲究，不能简单「两处各加一把」**：
   > `ScopedWindowRedrawLock`（类定义，`Ui/ListViewUtil.h:40`）**不是可重入的** —— 构造无条件发 `WM_SETREDRAW(FALSE)`（`Ui/ListViewUtil.cpp:14`），析构无条件发 `WM_SETREDRAW(TRUE)`（`:24`）。`WM_SETREDRAW` 是**布尔开关而非计数器**，所以：
   > ```
   > 外层锁构造 → WM_SETREDRAW(FALSE)
   >   内层锁构造 → WM_SETREDRAW(FALSE)
   >   内层锁析构 → WM_SETREDRAW(TRUE)   ← 提前恢复！外层剩余插入又开始逐行重绘
   > 外层锁析构 → WM_SETREDRAW(TRUE)
   > ```
   > 而 `PopulateCallbackInterceptPanel`（定义于 `:7052`）**内部会调用** `RenderCallbackLocalModel`（**定义于 `:7232`，调用点在 `:7073`**），两者作用在同一批控件上 —— `callbackGroupList_` 与 `callbackRuleLists_` 是各自独立的 `WC_LISTVIEWW` HWND，**控件创建语句**在 `:2390`（`callbackGroupList_`）、`:2435`（`callbackBypassList_`）、`:2475`（`callbackFileMonitorList_`）、`:2423`（`callbackRuleLists_` 的 `push_back`，把新建的 `ruleList` 存入 vector）；成员声明在 `Features/Kernel/KernelPage.h:357`/`:364`/`:372`/`:383`。
   >
   > **这不是既有缺陷，而是修复时才会引入的新风险。** 我扫了全仓库 17 处重绘锁用法（`grep` 排除 `Ui/ListViewUtil.*` 自身），**现有代码无一处嵌套** —— 包括最容易出问题的 `HandlePage.cpp:896` 与 `:943`（同一函数 `PopulateDetail`、同一控件 `detailList_`（成员声明在 `Features/Handle/HandlePage.h:84`）），但 `:943` 位于 `detailTask->request()` 的**异步 deliver 回调**里（请求在 `:905`，回调 lambda 从 `:928` 起），是另一个时刻的独立取锁，`:896` 的锁早已析构。`KernelPage.cpp:4841-4844` 那组三把 ListView 锁作用于 `resultList_`/`propertyList_`/`summaryList_` 三个不同 HWND（**控件创建语句**在 `:2169`/`:2186`/`:2215`，成员声明在 `Features/Kernel/KernelPage.h:324`/`:325`/`:326`），也不构成同句柄嵌套；`KernelPage.cpp:1892` 与 `:1900` 则是 `primaryTab_`/`secondaryTab_` 两个**互斥 if 分支**里的独立取锁（成员声明在 `Features/Kernel/KernelPage.h:278`/`:279`），同样不嵌套。
   >
   > **所以修法可以简化**：
   > - **首选（推荐）**：只在**最外层入口**取锁 —— `PopulateCallbackInterceptPanel` 与 `RenderCallbackLocalModel` 各自作为独立入口时各取一把，`RenderCallbackLocalModel` 作为被调用方**不再重复取**。改动最小、语义最清晰。
   > - **可选加固（防复发）**：把 `ScopedWindowRedrawLock` 改成**引用计数语义**（进入 +1、退出 −1、归零才恢复 `WM_SETREDRAW(TRUE)`）。这样「哪层取锁」就不再是调用者的责任 —— 现有 17 处用法一并受益。**这不是修复本条的前置条件**，是防止将来有人写嵌套调用时静默失效的护栏。
   >   **且这是行为等价重写，不引入回归风险**：现有 17 处**全部是单句柄、单层作用域**（每个作用域内只有一把锁，无嵌套），改成引用计数后进 1 出 1、计数 1→0、归零才恢复 —— 与现状语义完全一致。可以放心做。
6. **规则侧行数上限**：`callbackRules_` 每条规则插 2 行（`:7257` + `:7269`），N 条规则 → 2N 行插入，同样需要 `kMaxDisplayRows` 封顶 + `RowCapNotice`。
   **规则数本身也无上限**：新增走 `callbackRules_.push_back`（调用点 `:7628`）、粘贴走 `:7812`、导入走 `LoadCallbackLocalConfig`（定义于 `:8506`）—— 后者是 `while (std::getline(input, line))`（`:8517`）读到文件结束为止逐条 `rules.push_back`（调用点 `:8598`），**全程无 cap**。所以「2N 行插入」的 N 由用户导入的文件大小决定。

> **⚠️ 改造顺序（有讲究，顺序反了会更糟）**：
> **先把「行表构建 + 格式化」整体搬进 Work，再切 owner-data。**
> 反过来的话，中间态会变成「owner-data + UI 线程物化」—— 那比现在更糟：现在至少是逐格插入、开销集中在可见区重绘；改成 owner-data 后若行表仍在 UI 线程构建，N 行 `VirtualListRow` 的构造 + 5 次格式化会全压在 UI 线程上，且失去了「只重绘可见区」这个天然缓冲。
> 正确形态参照 `dc282eb1`（#30）：**两件事一起做** —— 行表构建移到 `AsyncSnapshotTask` 的 Work，同时用 `VirtualListView`（owner-data）+ `setSharedRows` 交接。

---

### 32. `Features/Kernel/KernelPage.cpp:4059-4079` — 切页保存辅助表（边界，可接受但可优化）

**代码**
```cpp
std::vector<std::vector<std::wstring>> KernelPage::CaptureReportListRows(HWND list) const {
    ...
    const int rowCount = ListView_GetItemCount(list);
    const int columnCount = HeaderColumnCount(list);
    rows.reserve(...);
    for (int row = 0; row < rowCount; ++row) {
        std::vector<std::wstring> cells;
        cells.reserve(...);
        for (int column = 0; column < columnCount; ++column) {
            cells.push_back(ListViewText(list, row, column));   // ← 每次读取都新建 wstring
        }
        rows.push_back(std::move(cells));
    }
    return rows;
}
```

**问题**
每次切页都把辅助 ListView 的**全部单元格文本读出来重建**（`ListViewText` → `ListView_GetItemText` → 新 `wstring`），再在切回时用 #32 的 `DeleteAllItems` + 逐行 `ListViewInsertRow` 插回去。

**规模估计**
辅助表是"小表"（属性表 / 汇总表），几十行 × 十几列 → **几十 KB**，规模可控，故判为**可接受**。

**改法方向**
不是性能热点，但可以顺手改善：这些辅助表本来就是小数据，可以同样改成 owner-data 虚拟表（`LVS_OWNERDATA` + 共享行快照），切页时只换快照指针，不做文本往返。

---

### 33. `Features/Window/WindowView.cpp:1052` — 按值拷贝单行到 lambda（边界）

**代码**
```cpp
    const WindowSnapshotRow input = *row;
    WindowTools::UpdateWindowHierarchyReportView(state->hierarchyReportView, input.hwnd);
    const std::uint64_t generation = state->snapshotGeneration;
    ListView_DeleteAllItems(state->detailList);
    AddDetailRow(state->detailList, 0, L"状态", L"正在后台查询窗口与 Win32k 详情…");
    state->detailTask->request(
        [input, generation, modelIndex] {          // ← 按值捕获整行
            WindowDetailSnapshot snapshot{};
            snapshot.row = input;                  // ← 又拷贝一次
            ...
```

**问题**
`WindowSnapshotRow` 按值拷贝一次进 lambda，`snapshot.row = input` 又拷贝一次。单行规模，**判为可接受**，但写法上不一致：项目里 `AsyncSnapshotTask` 的 `Deliver` 已经用 `std::optional<Result>&&` 做移动传递，这里却按值。

**规模估计**
单行 × ~200 B。**可忽略**。

**改法方向**
改成捕获 `const WindowSnapshotRow*`（指向 `state->model` 中稳定的存储）或用 `std::move`，与 #7 的"共享"原则保持一致。

---

### 34. `Features/Kernel/KernelPage.cpp:4082-4100` — 切页恢复辅助表（边界）

**代码**
```cpp
void KernelPage::RestoreReportListRows(HWND list, const std::vector<std::vector<std::wstring>>& rows) {
    if (!list) { return; }
    Ksword::Ui::ScopedListViewRedrawLock redrawLock(list);
    ListView_DeleteAllItems(list);
    for (const std::vector<std::wstring>& row : rows) {
        ListViewInsertRow(list, row);
    }
}
```

**问题**
「`DeleteAllItems` + 重插」模式 —— 正例见任务描述。但**辅助表行数很小**（属性表/汇总表几十行），所以规模可控。

**规模估计**
几十行 × 十几列，每格一次 `ListView_SetItemText` 跨进程消息 → **几百次 SendMessage**，量级在千次以下。**可接受**。

**改法方向**
与 #30 配套：改用 owner-data 虚拟表 + 共享行快照，彻底消除"读出文本再写回去"。

---

## 附：判定为「正例、无需改动」的回调约定

审计确认以下回调签名**不产生跨线程拷贝**，符合项目约定：

- `Ui/AsyncTask.h:37` `Deliver = std::function<void(std::uint64_t, std::optional<Result>&&, std::exception_ptr)>` —— 结果按右值移动传递。
- `Ui/AsyncTask.h:140` `deliver(completion->generation, std::move(completion->result), completion->error);` —— 移动进回调。
- 所有 `request(work, deliver)` 的 `work` lambda 捕获的都是 `shared_ptr`（如 `RegistryView.cpp:644` `[rows, generation, useRegex, query = ..., ...]`），**共享**而非拷贝。

审计中**未发现**「`PostMessage` 携带大载荷」的问题：全项目仅 `Features/Kernel/KernelCallbackEventReceiver.cpp:66` 一处 `PostMessageW` 携带 `snapshot.get()` 指针（且有对应的所有权交接），不涉及值载荷。

### 一处正例但仍有小瑕疵 ✅ 已消解

`Ui/EvidenceSession.cpp:148-151`：
```cpp
EvidenceSession::SharedSnapshot EvidenceSession::sharedSnapshot() const {
    std::scoped_lock lock(mutex_);
    return items_;                    // ← 按值返回：vector 本身被拷贝
}
```
`EvidenceItem` 的内容是共享的（`shared_ptr<const EvidenceItem>`），但**容器被复制了**（N 次 `shared_ptr` 拷贝 + N 次原子递增）。

> **✅ 已消解**：`Ui/EvidenceSessionView.cpp` 改造后，`sharedSnapshot()` 的调用点已移入 `AsyncSnapshotTask` 的 Work 内
> （`return BuildSnapshotRows(GlobalEvidenceSession().sharedSnapshot(), generation);`）——
> **容器拷贝不再发生在 UI 线程**，而是随快照构建在 worker 上一次性完成。
> 若后续 `sharedSnapshot()` 还要被 UI 线程直接调用，仍应改为返回 `std::shared_ptr<const SharedSnapshot>`。

---

## 汇总建议（按投入产出排序）

1. **~~12 处 `setRows(*shared)` → `setSharedRows(shared)`~~ ✅ 已完成（`45ca93b0` + `969604fc`）**
   本项是投入产出比最高的改造，已由其他 agent 落地，全项目 `setRows(*ptr)` 剩余 0 处。保留本条作为**回归防线**：新增页面时若写 `setRows(*sharedPtr)` 会重新引入整表深拷贝，`Ui/VirtualListView.h:47-48` 的注释已写明「caller must never mutate the vector after hand-off」的交接契约。
   当时的收益量级：`HandlePage.cpp:690`（句柄表 12.6~63 MB/次）、`EventLogView.cpp:270`（可达 31 MB/次）。

2. **3 处筛选回调的 `visibleRows` 改存索引 / key**（#14～#16）
   去掉每行整条记录的按值拷贝。

3. **`ImmutableTextRows` 增加「接管既有 `shared_ptr<const Values>`」入口**（#2、#1）✅ 已完成（`2256f567`）
   从根上消除 `assign()` 的全量深拷贝。
   **已落地为 `adopt(std::shared_ptr<const Values>)`**：`KernelPage.cpp` 的 4 处调用点已改用它。
   关键实现细节值得保留在后续改造里：`adopt()` 把 `flat_` 指向被接管的 vector，
   **因此被接管的 vector 同时成为 `contiguousSnapshot()` 的缓存** —— 后续 `operator const Values&` 不再触发重新物化。
   这正是「一处改动同时解决多个热点」的正确形态。
   **边界（须一并交给下一棒）**：`adopt()` 只消除「从 `currentRawRows_` 接管」这一层；#3 自己造的 `visibleRows` 中间副本它管不着，且那个副本的 `assign(Values&&)` 会 `flat_.reset()` 把 `adopt()` 的缓存清掉 —— 所以 **#1 仍隔次触发，#3 仍是 Kernel 侧主要残留**。

4. **`RebuildResultListFromCache` 改用 `RowRef` 迭代 + 索引可见集**（#1、#3）—— **当前 Kernel 侧优先级最高**
   不再构造 `vector<vector<wstring>>` 中转：可见集只存 `vector<size_t>` 索引，行数据回到 `currentRows_` 按索引取。
   **这一条同时解决 #1 与 #3**：只要不再走 `assign(Values&&)`，`flat_` 就不会被 `reset()`，隐式物化也随之消失。
   注：#3 的 `visibleRows.push_back(row)` 按值拷贝**每行 cells**（N × 576 B）**不受 `adopt()` 覆盖** —— 那是另一层拷贝。

5. **行构建移出 UI 线程**（#13 `MiscFeature.cpp:533`、#17 `FileView.cpp:461`）
   `BuildAuditVirtualRows` / `BuildPresentationRows` 这类纯格式化函数应放进 `AsyncSnapshotTask` 的 Work。

6. **`ProcessView.cpp:673-687` 的 cells 双份**（#11、#12）
   `filterRow.cells` 与 `row.cells` 共享缓冲。

7. **无 owner-data 的 ListView 改虚拟化**（#31 `KernelPage` 回调拦截面板、#32/#34 辅助表）
   这一类不是「拷贝」而是「重建」，但代价等价甚至更高：每次都是 `DeleteAllItems` + 逐格跨进程 `SendMessage`，且没有行数上限。
   建议统一改造为 `LVS_OWNERDATA` + 共享行快照 —— 与 #6～#29 是同一条技术路径（`VirtualListView` 已经支持）。
   **#30 已按此路径完成改造**（owner-data + 后台构建 + `setSharedRows` + 截断提示），可直接作为 #31 的实现范式。
   **#31 可先行上线止血**：文末「附录」的 `kMaxDisplayRows` + `RowCapNotice` 补丁。
   **⚠️ #31 的改造顺序**：先搬「行表构建 + 格式化」进 Work，再切 owner-data；反过来中间态是「owner-data + UI 线程物化」，比现在更糟。

8. **`rowValue` 查表化 + 返回引用**（#31）
   列名→列下标映射预计算，`std::wstring` 改 `std::wstring_view`，消除 O(别名×列) 扫描与每次调用的字符串拷贝。

---

## 与其他审计报告的交叉引用

> 归属说明：以下报告由不同队友产出，本节只做**引用与分工界定**，不替他人下结论、不重复排期。
> 本报告（拷贝/线程视角）的读者关心「哪些拷贝在 UI 线程、代价多大」；改造排期以各专题报告为准。

- `docs/audit/list-virtualization-survey.md`（**ListSurvey-step5**，ListView 侧）：其「次要问题」一节的 18 处 `setRows(*shared)` 清单与本报告 #6～#29 **完全一致**（双方独立统计，同为 18 处）。该报告另发现的 2 处「UI 线程全量重建」已并入本报告 #30、#31（对方对应其 §37、§38）。
- `docs/audit/registry-expand-perf.md`（**RegistryPerf-flash**，注册表展开性能）：注册表树展开路径 —— 子键枚举在 `AsyncSnapshotTask` 的 Work 里（后台线程），这一点是正例。
  **该路径已由 `969604fc` + `4c2c4d98` 重写**：`Ui/LazyTreeView` 接管了节点记录、**分批插入（`kBatchSize=400`）**、**单父节点上限（`kMaxRowsPerParent=5000`）** 与按路径哈希索引；`hasChildren` 改由 `TVIF_CHILDREN` 发布，占位子节点全部删除。原先「一次插完整个子键列表、每子键再插一个占位节点」的 2N 次 `TVM_INSERTITEM` + 2N 次堆分配 + N 次全路径拼接已消除。
  我原先在此处标注的 `ReplaceTreeNodeChildren`（旧 `:481-507`）随提交一起重写，现位于 `RegistryView.cpp:348`，实现已简化为构造 `TreeRow` 列表后交给 `state.tree.setChildren(item, std::move(children))`。
  **我原先请求 TreeSurvey-bunny 判断的「是否单列一条」现已由该提交回答：树侧已按批次+上限改造完毕，无需再单列。**
- 树控件相关的结论以 **TreeSurvey-bunny** 的产出为准（`docs/audit/tree-survey.md`，判定：严重 0 / 风险 1 / OK 3 树实例 + 2 封装）。
- `a74eba1a` 把硬件设备树也迁到 `Ui/LazyTreeView`（新增 `ChildSource` 同步子节点来源）—— **全仓库两棵可见树都已接封装**。
- **回调拦截面板的重绘抑制缺失**：TreeSurvey-bunny 在 `docs/audit/tree-survey.md` 的 **§6.5** 中以「**非树控件**」名义记录了这一形态（登记理由是说明「UI 线程逐项重建 + 无上限 + 无重绘抑制」不止存在于树控件，并防止后人按「内核页已做对」的印象漏掉它）。**该形态属本报告口径**，已完整记入本报告 **#31 条目**；**改造建议以本报告 #31 的第 5、6 条为准**（他的 §6.5 只写事实与判定，不写改法，分工无重叠）。

### 一处需要纠正的常见误读：`kBatchSize` 约束的是**插入**，不是 `TreeRow` **构造**

> 这一条由 **TreeSurvey-bunny** 提出，我独立复核确认成立，并补入本报告 —— 因为残留的字符串拷贝属本报告（拷贝/线程视角）的口径。

`RegistryView.cpp:352-360` 在 UI 线程（`AsyncSnapshotTask` 的 deliver 回调内，`RegistryView.cpp:457` 调用）把每个子键名拷成 `TreeRow`：

```cpp
std::vector<Ksword::Ui::TreeRow> children;
children.reserve(snapshot.subKeys.size());                        // 353: 一次性 reserve 全部 N
for (const std::wstring& subKey : snapshot.subKeys) {             // 354: 遍历全部 N，不是前 400
    Ksword::Ui::TreeRow row;
    row.text = subKey;                                            // 356: N 次 wstring 拷贝
    row.key = DisplayPathFromRootAndChild(snapshot.path, subKey);  // 357: N 次路径拼接+分配
    row.hasChildren = true;
    children.push_back(std::move(row));                           // 359
}
state.tree.setChildren(item, std::move(children));                 // 364
```

`kBatchSize = 400` 只在 `LazyTreeView::insertBatch`（`Ui/LazyTreeView.cpp:202`）里生效，限制的是**从 `node.pending` 取多少行去 `TVM_INSERTITEM`**。而上面这个循环在 `setChildren` **之前**就把全部 N 行构造完了 —— `installChildren`（定义于 `Ui/LazyTreeView.cpp:296`）只是把整份 `children` 收进 `node.pending`，插入是之后的事。

| UI 线程上的工作 | 次数 |
|---|---|
| `wstring` 拷贝（`row.text`） | **N** |
| 路径拼接（`row.key`，`RegistryView.cpp:226` 的 `rootText + L"\\" + childName`，新分配） | **N** |
| `TreeRow` 构造 + `push_back` | **N** |
| `reserve` 一次性分配 | 1（N × sizeof(TreeRow)） |
| **`TVM_INSERTITEM`** | **≤400/批** |

**N 无上限**：后端枚举不 clamp —— `shared/usermode/backend/registry/RegistryBrowse.cpp:75` 的 `for (DWORD index = 0; index < subKeyCount; ++index)`，`subKeyCount` 直接来自 `RegQueryInfoKeyW`。

**但结论方向不变，理由要换成「与改造前比」**：改造前每个子键 **2 次 `TVM_INSERTITEM`**（本体 + 占位子节点）+ 2 次堆分配 + N 次路径拼接，**且无 `WM_SETREDRAW` 抑制**（按 RegistryPerf-flash 在 `docs/audit/registry-expand-perf.md:60-67` 的推算，未抑制重绘的 `TVM_INSERTITEM` 约 20–100 µs，`HKCR`（10000+ 子键）即 0.4–2 s）；改造后这段循环里 **0 次 `TVM_INSERTITEM`**，剩下的 N 次字符串分配与拷贝在**毫秒级**（单次 `wstring` 拼接百纳秒级）。**低 2～3 个数量级，可接受，不构成风险判定。**

**一条内存侧事实（若要算「展开后树持有了多少内存」）**：`installChildren` 把整份 `children` 收进 `node.pending`（`Ui/LazyTreeView.cpp:301`），只有被插进控件的行才在 `insertBatch` 里 `row.text.clear(); row.text.shrink_to_fit();` 释放（`Ui/LazyTreeView.cpp:227-230`）。**未显示的行**（超出 400 且用户未点「载入更多」）连同 `text`/`key` 一起驻留在 `pending` 里，量级 O(未显示行数)，与 `kMaxRowsPerParent = 5000` 一致，**不构成无界增长**。

> **口径归属**：本条是**残留但可接受**，不单列为热点条目 —— 注册表树的插入路径已由 `969604fc` 改造完毕，树侧判定以 TreeSurvey-bunny 的产出为准。本节只记录「哪些拷贝仍在 UI 线程、代价多大」。

---

## 附录：可立即上线的「止血」补丁（不必等虚拟化改造）

#30、#31 这类「无 owner-data 的 ListView，每次刷新 `DeleteAllItems` + 逐格跨进程插入」有一个**改动极小、可独立上线**的止血手段，项目里已有现成范式：

- `Ui/ListViewUtil.h:15` `kMaxDisplayRows = 5000` —— 单个 ListView 单次批量替换的行数上限。
- `Ui/ListViewUtil.h:108` `ReplaceListViewRowsCapped(listView, rows, maxRows = kMaxDisplayRows)` —— 删除旧行并在 `maxRows` 处截断，**不插入占位「更多」行**（避免污染选择/复制/导出）。
- `Ui/ListViewUtil.h:116` `RowCapNotice(result)` —— 生成「已显示前 X / 共 Y 条，其余未载入列表」提示文本。

既有使用点（说明这是项目已认可的模式，不是新发明）：
- `Features/AuditCommon/AuditTable.cpp:145` —— `ReplaceAuditTableRows` 直接委托 `ReplaceListViewRowsCapped`。
- `Features/Hardware/HardwareHwidDispatchView.cpp:389` —— 单独取 `RowCapNotice` 拼进状态文本。

**用法**：
- ~~#30 `EvidenceSessionView.cpp`~~ ✅ **已按此方案完成改造**：改用 `VirtualListView`（owner-data）+ `AsyncSnapshotTask` 后台构建 + `setSharedRows` 交接 + `kMaxDisplayRows` 截断 + 状态文本提示「仅显示前 X 项 / 共 Y 项」。**本条对它已不再是待办。**
- **#31 `KernelPage.cpp:7052-7168`（回调拦截面板）仍适用** —— 4 个 ListView 仍需分别截断 + 提示。

**代价**：N=10000 时跨进程消息从 50000 次降到 25000 次（5 列 × 5000），仍是 O(上限) 而非 O(N)。
**收益**：把「会话越长越卡」变成「有固定上界」，不必等虚拟化改造排期。

**注意**：截断只是止血，**不解决根因**。根因仍是「UI 线程上按行重建控件 + 逐格跨进程消息」，最终仍应改为 owner-data 虚拟表 + 共享行快照（`VirtualListView` 已具备该能力，见改造建议第 7 条）。截断后必须显示 `RowCapNotice`，否则用户会误以为「数据只有这么多」——这与 `Ui/ListViewUtil.h:106-107` 的设计意图一致。

> **#30 的实际实现比止血更进一步**：它没有停在「截断」，而是把插入成本整体消除（owner-data 替掉了 `DeleteAllItems` + 逐格 `SendMessage`），同时把物化成本移出 UI 线程（后台构建）。这印证了「虚拟化消除插入成本、物化成本要靠共享快照那类手段消除」需要**同时**做两件事 —— 只做虚拟化仍会在 UI 线程留下 N 行 `VirtualListRow` 的构造。

---

*本报告仅作只读审计，未改动任何源码。行数 N 的取值为典型 Windows 桌面环境的经验量级，实际值随目标机器而变；字节量级按第「规模估计」节的统一公式推算。*
