# KswordARKLight / KswordCLI 审核报告

- 日期：2026-10-23
- 范围：`E:\Projects\Ksword5.1\KswordARKLight\`（App / Core / Docking / Ui / Features，约 10.9k 行 cpp+h）、`E:\Projects\Ksword5.1\KswordCLI\`（80 文件，约 14.4k 行）
- 判据基线：`AGENTS.md`「主程序 UI 强制接入规则」+「全页面 UI 设计规范（强制）」；`.claude/memory/ksword-ui-architecture.md`
- 方式：15 个只读子 agent 并行审核（10 × deepseek-v4.1-flash 分区取证 + 5 × space-bunny 交叉复核），主审同步独立核对关键路径。全部只读，未构建、未运行主程序。

---

## 0. 覆盖矩阵

| # | 区域 | 模型 | 结论条数 | 状态 |
|---|---|---|---|---|
| 1 | App/Main.cpp、MainWindow、Docking/DockManager | deepseek | 23（P0×3 / P1×9） | 已交付 |
| 2 | Ui/Theme、Controls、WorkspaceHost、Splitter/Tab/List/Tree/VirtualList | deepseek | 26 + 30 | 已交付 |
| 3 | Ui 辅助组件（FilterBar、LoadingOverlay、AsyncTask、ExportUtil、EvidenceSession…） | deepseek | 24（经 #15 复核） | 已交付（经交叉复核） |
| 4 | Features/Kernel、AuditCommon | deepseek | 3（P0×3） | 已交付（采样偏少） |
| 5 | Features/ProcessDetail、Process、Handle | deepseek | 25 | 已交付 |
| 6 | Features/Driver、Memory、SysTools | deepseek | 19 | 已交付 |
| 7 | Features/NetTools、Network、WindowTools、Window、Misc | deepseek | 25 | 已交付 |
| 8 | Features/Registry、File、Startup、Service、Privilege、Hardware*、Monitor、FeatureRegistry | deepseek | 21 | 已交付 |
| 9 | CLI 前半（KswordCLI.cpp、CommandRegistry、ArkDriverExtended、R3File*、R3Kernel*、R3Hardware*） | deepseek | 21（P1×5 / P2×6 / P3×10） | 已交付 |
| 10 | CLI 后半（R3Monitor/R3Network/R3Process/R3Registry/R3Security/R3Window…） | deepseek | 21 | 已交付 |
| 11 | space-bunny：UI 规范判据（页面→框架覆盖表） | space-bunny | 覆盖表 + 10 条 | 已交付 |
| 12 | space-bunny：硬编码样式/主题违规统计 | space-bunny | 22（+统计表） | 已交付 |
| 13 | space-bunny：CLI 正确性独立复核 | space-bunny | 25（无 P0） | 已交付 |
| 14 | space-bunny：ARKLight 逻辑/生命周期独立复核 | space-bunny | 20（P1×4 / P2×10 / P3×6） | 已交付 |
| 15 | space-bunny：跨项目一致性（Light vs 主程序） | space-bunny | 26 | 已交付 |

---

## 1. 结论摘要

### 1.1 一句话

ARKLight 的 UI 差不是"某几个页面写错"，而是**它根本没有 UI 框架**：纯 Win32 + Common Controls、150 行自绘助手、7 个硬编码颜色、无 DPI、无布局系统、表格无裁剪，全部页面用绝对坐标堆控件。规范要求的共享布局/Tab/按钮角色/表格操作条在 Light 里没有承载点，所以逐页修补不会收敛。

（技术路线已确认：Light 只走 Win32 单文件路线、只支持中文。因此**不引入 Qt、不拆模块、不做 i18n**——本报告所有修复建议都在这三条边界内给出，且遵循"不重复造轮子、不过度封装"：能复用一个 40 行函数的地方不新增类。）

### 1.2 必须优先处理的 12 项

| # | 级别 | 位置 | 问题 |
|---|---|---|---|
| 1 | P0 | 全工程 | 无 DPI 感知（无 manifest、无 `SetProcessDpiAwareness`、无 `WM_DPICHANGED`），整窗位图拉伸发虚 |
| 2 | P0 | `Ui/Theme.h:27-33` | 主题只有 7 个硬编码浅色 COLORREF，无深色模式、无 token 派生、无状态色 |
| 3 | P0 | `Ui/Controls.h` | 共享控件层仅 5 个函数（Text/Button/Panel/DrawTextLine），无布局、无角色、无尺寸度量 |
| 4 | P0 | `Ui/Theme.h:9-11` + `Controls.cpp:14` | 字体刻意排除在主题外，直接取系统 NONCLIENTMETRICS，无项目字号下限 |
| 5 | P1 | `KernelPage::RestoreReportListRows` / `ProcessDetailPage::AddListRow` / `AuditTable` | 表格无行数上限与截断提示：切回视图时全量重插、审计表逐项插入（非 owner-data）；主表虽已虚拟化，但刷新仍整表重建索引 + `setRows` 深拷贝 |
| 6 | P0 | 41 页 / 24 目录 | 无布局引擎：555 处 `MoveWindow` 绝对坐标、0 处滚动容器；6 页零框架、5 页自建详情框架 |
| 7 | P1 | `App/MainWindow.cpp`（16 模块页签单行 22px） | 窄窗下页签既不可见也不可点 |
| 8 | P1 | `Features/Window/WindowView.cpp:1239` | 切换排序后模型与显示快照错位，右键"关闭窗口/置前/捕获保护"可能作用到别的窗口 |
| 9 | P1 | `KswordCLI/KswordCLI.cpp:5539/6006/4988/2631` | 强制卸载驱动、摘除内核回调、删文件、结束进程在默认路径**无确认门禁**，且主动传 `--confirm` 反而报非法参数 |
| 10 | P1 | `KswordCLI/CommandRegistry.cpp:191` + `KswordCLI.cpp:1414` | 注册表与 legacy 两套参数白名单，门禁随 `--backend` 漂移（第 9 项的共同根因） |
| 11 | P1 | `Core/DriverLease.cpp:216` + `MainWindow.cpp:1641` | 退出时租约释放遇互斥超时被当成"无需停驱动"，`finalSucceeded=true` 走成功分支 → 驱动可能残留运行 |
| 12 | P1 | `Ui/AsyncTask.h:69` + 13 处调用点 | `request()` 返回 `void` 且有三条静默 return；调用方先置 `xxxInProgress` 后发请求 → 命中即按钮永久禁用，必须重启（一处 API 改动可全消） |

---

## 2. ARKLight 架构级根因（主审核实）

### 2.1 技术栈：纯 Win32，零 Qt

```
grep "QWidget|QApplication|#include <Q"  → 0 命中
grep "CreateWindowExW"                    → 224 处（Features 内）
```

`AGENTS.md` 允许"非 Qt 子项目做语义相同的原生适配"，但适配前提是**存在等价框架**。当前 Light 的等价物是：

| 规范要求的共享入口 | Light 对应实现 | 差距 |
|---|---|---|
| `ParameterWorkspace`（参数独立滚动 + 动作固定 + 结果区） | 无 | 每页自己 `MoveWindow` 摆两栏 |
| `SecondaryPageLayout`（设置类二级页） | 无 | 无固定底栏、无 Tab 分组 |
| `DetailLayoutHost` / `StructuredFieldView` | 各页各写 `AddDetailRow` | 字段视图中 3 份以上重复 |
| `StylePageTabs` / `StyleJoinedResultTabs` | 原生 `SysTabControl32` | 无连体关系、无切换动画 |
| `NormalizeToolbarRow` / `ToolbarMetrics` | 手写 64~300 的按钮宽度数列 | 字号变化即裁切 |
| `ThemedMessageBox` | 原生 `MessageBoxW` | 无主题、无危险语义 |
| `theme.h` 动态 token + `ThemeBinding` | `Theme.h` 7 个 `COLORREF` 常量 | 无热切换、无深浅色 |
| `InstallStandardWidgetTheme` | 无 | 原生控件零统一外观 |

`Ui/Theme.cpp` 48 行、`Ui/Controls.cpp` 101 行——这就是 Light 的全部 UI 基础设施。

### 2.2 主题：只有浅色一套，且写死在头文件

```cpp
// Ui/Theme.h:27-33
COLORREF windowColor   = RGB(246, 247, 249);
COLORREF panelColor    = RGB(255, 255, 255);
COLORREF borderColor   = RGB(210, 214, 220);
COLORREF accentColor   = RGB(0, 100, 251);
COLORREF accentDarkColor = RGB(0, 76, 190);
COLORREF textColor     = RGB(31, 35, 40);
COLORREF mutedTextColor= RGB(96, 103, 112);
```

- 无 `dark` 分支：`grep SetWindowTheme|DwmSetWindowAttribute|ImmersiveDark` → 0 命中。系统深色模式下 Light 仍是浅色窗口 + 浅色标题栏。
- 无状态色：hover / pressed / selected / disabled / focus 五种状态**没有任何 token**，各页只能自绘或不绘（规范 §3 要求各状态均可辨）。
- 对比度尚可（正文 `#1F2328` on `#FFFFFF` ≈ 14.9:1，muted ≈ 5.7:1），但状态缺失比对比度更要命。

### 2.3 DPI：整条链路缺失

```
无 *.manifest（只有 .rc）
grep SetProcessDpiAwareness|DPI_AWARENESS|GetDpiForWindow  → 仅 WindowToolsHierarchyView.cpp:165 一条注释
grep WM_DPICHANGED                                          → 0 命中
MulDiv 全工程                                               → 仅 PlaceholderPage.cpp:78（进度条）
```

且所有几何量都是裸像素常量：

```cpp
// App/MainWindow.cpp:50-56
constexpr int kCommandEditWidth  = 320;
constexpr int kCommandEditHeight = 22;
constexpr int kStatusHeight      = 18;
constexpr int kNavigationPaletteWidth  = 560;
```

后果：进程 DPI-unaware → 高 DPI 下 Windows 位图放大整窗（发虚）；字号缩放 125%/150% 后所有固定宽度按钮与状态文本硬编码 x 坐标（`WindowView.cpp:1895` 的 `RECT{510, 7, …}`、`MiscFeature.cpp:952` 的 `kGap + 210/344`）立刻互相压盖。

### 2.4 布局：没有布局引擎

- 224 处 `CreateButton/CreateText` 全部自带 `int x, int y, int w, int h`。
- `MainWindow::layout()` 只有 34 行，靠固定常量重排。
- 缩放不是"重排"而是"移动同一批固定尺寸控件"，因此长中文标签与系统字号切换都无处适配（中文按全角计宽，10 个字的按钮就是 10×字号，现有 `kGap + 210/344` 一类算式在 125% 缩放下必然压盖；规范 §8 明确禁止用横向滚动掩盖溢出）。

### 2.5 语言：只做中文，不做 i18n（产品决策，前次结论撤回）

```
grep -P 'L"[^"]*[\x{4e00}-\x{9fff}]' → 6738 处（全部为直写字面量，无资源表）
grep LoadStringW|LanguageManager|i18n → 0 处
```

**结论修正**：上一版把"无 i18n 设施 → 规范 §9.4 的 zh-CN/en-US 同步无从落地"列为 P0，**现予撤回**。Light 只支持中文，不引入语言包、不引入 `LanguageManager`、不做字符串资源表；这 6738 处字面量**不需要迁移**（相当于从修复清单里划掉约 5500 行改动）。

只做中文带来两条真正需要处理的代价，但都用最小改动解决，不新增框架：

- **术语中英混用**（这一条是真问题，与 i18n 无关）：`L"…Handle"` 41 处 vs `L"…句柄"` 63 处、`Callback` 86 处 vs `回调` 80 处、`Driver` 302 处 vs `驱动` 252 处。同一个概念在 41 个页面里两种叫法，用户看到的是"同一件事两个名字"。
  做法：`Ui/Text.h` 里放十几行 `inline constexpr wchar_t*` 常量，只收口菜单 / Tab / 列头 / 按钮四类高频词；编译期直出，不查表、不分配、不注册。
- **宽度按全角算**：中文按钮宽度 ≈ 字数 × 字号，不能沿用西文经验值。阶段 1 的工具栏度量按中文字形实测（`GetTextExtentPoint32W`）取宽即可，一行代码的事，不需要排版引擎。

### 2.6 绘制：单缓冲 + 每帧创建 GDI 对象

```cpp
// App/MainWindow.cpp:561-569
case WM_PAINT: { PAINTSTRUCT ps{}; HDC dc = ::BeginPaint(hwnd, &ps); paint(dc); ::EndPaint(hwnd, &ps); return 0; }
case WM_ERASEBKGND: return 1;      // 不擦背景
```
```cpp
// Ui/Controls.cpp:79-88  PaintPanel
::FillRect(dc, &rect, AppTheme().panelBrush());
HPEN pen = ::CreatePen(PS_SOLID, 1, AppTheme().borderColor);   // 每帧新建
… ::Rectangle(dc, …);                                          // 再描一圈实线
::DeleteObject(pen);
```

`WM_ERASEBKGND` 返回 1 + 单缓冲 = 缩放/最大化时闪；`PaintPanel` 每帧 `CreatePen`/`DeleteObject`，并画 1px 实线边框（规范 §4：卡片应靠底色和留白分层，不另套描边）。`Features/` 内 `PaintPanel` 使用次数为 **0**——共享绘制入口事实上被绕开。

### 2.7 表格：没有裁剪，这是比 Qt 更难受的地方

```
ListView 创建点                      20 处
VirtualListView::create 强制附加      LVS_OWNERDATA（VirtualListView.cpp:23）→ 用它的页面均已虚拟化
主表虚拟化情况                        41 页的主表基本都已走 VirtualListView（含 Registry / Service / Startup / Hardware / Connection）
仍用 ListView_InsertItem 插真实 item  13 个文件 / 18 处，集中在详情小表与汇总投影（KernelPage 的 6 个视图 + RestoreReportListRows、ProcessDetailPage::AddListRow、AuditTable、FileFeature）
全工程"显示行数上限 / 分页 / 已截断提示"  0 处
唯一存在的行数缓冲上限                 EtwMonitorView.cpp:44  kMaxEventRows = 5000
```

Qt 的 `QAbstractItemModel` 至少让"只渲染可见行"成为默认行为；Win32 的 `SysListView32` 默认把每个 item 都建成真实窗口项，插入 N 行就是 N 次 `LVM_INSERTITEM` + N 次 `LVM_SETITEMTEXT`。Light 的主表躲过了这一层，**真正的热点在详情小表和"整表重建"路径**：

- **`KernelPage::RestoreReportListRows`**：切回视图时把最新结果集全量重插一遍（证据/摘要/日志三张表各来一次），而视图数据本身没有变化——这是纯浪费。
- **`ProcessDetailPage::AddListRow`** / **AuditTable**：强制非 owner-data 的追加语义，几千行的审计结果只能逐项插入。
- **`VirtualListView` 看着对，刷新仍全量**：`resetVisibleIndexes()`（`VirtualListView.cpp:65-68`）先按 `rows_->size()` 铺满全量索引，再异步筛选 → 行数越多，单次刷新越贵且会整表闪现、滚动位置重置（见 §4.1）。
- **`setRows(std::vector<VirtualListRow>)`** 逐刷新深拷贝全表字符串；只有 WindowView / RegistryView 用了 `setSharedRows` 免拷贝版本。
- 5 个页面在 UI 线程做整表格式化 + 二次深拷贝（§4.1），与上面叠加时是"UI 线程既格式化又重建列表"。

所以优化方向不是"换成更好的控件"，而是三条不改架构的动作：**① 详情小表能复用 `VirtualListView` 的就复用；② `setRows` 调用点全改 `setSharedRows`；③ 给"整表重建"加显示上限 + "已显示 N / 共 M 条"的截断提示**（`kMaxDisplayRows` 一个常量 + 状态栏一行文案，不需要分页控件）。ETW 页面的 `kMaxEventRows` 是现成范本，照它抄。

---

## 3. ARKLight UI 合规缺口（按规范条款）

### 3.1 Tab 与页面关系（§5）

- `App/MainWindow.cpp`：16 个模块页签挤在单行 22px 高，窗口窄于约 950px 后**既不绘制也不可点击**（无溢出菜单、无换行、无滚动）。
- `Features/*/*Feature.cpp`：Tab 全是原生 `SysTabControl32`，父子 Tab 之间无方向动画、无留白合并规则；`StyleJoinedResultTabs`（工具区与结果 Tab 连体）不存在。

### 3.2 按钮与输入（§6）

```cpp
// Ui/Controls.cpp:59-66
HWND CreateButton(..., const std::wstring& text, int x, int y, int w, int h) {
    ::CreateWindowExW(0, L"BUTTON", text.c_str(), WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON, …);
```

- 只有一种按钮：原生 `BS_PUSHBUTTON`。无 Primary / Default / **Danger** 角色 → 规范 §6 的"危险动作用危险语义"在 Light 无承载点。实际后果：
  - `NetToolsConnectionView.cpp:616`「结束连接」（删 TCB）
  - `WindowToolsClipboardView.cpp:611`「清空剪贴板」
  - `WindowToolsCaptureView.cpp:475`「应用到选中窗口」
  - `MiscFeature.cpp:824`「上传内置位图」
  四个破坏性动作与"刷新/导出"同色同宽。
- 按钮宽度全是魔数：64/72/78/82/86/88/92/96/106/110/120/126/140/150/180/300，`MoveWindow` 直接落位。
- 焦点提示：原生按钮/输入保留系统矩形，未区分鼠标与键盘来源（规范 §6 要求鼠标点击不显示键盘焦点框）。

### 3.3 表格与数据语义（§7）

| 页面 | 列数 | 列组/表头右键 | 操作条 | 明细面板 |
|---|---|---|---|---|
| 连接（`NetToolsConnectionView.cpp:636`） | 8 | 无 | 仅右键菜单 | 固定 160/700 |
| 防火墙（`NetToolsFirewallView.cpp:606`） | 9（+5 隐藏） | 无 | 仅 1 个导出 | 固定 160/700 |
| 捕获保护（`WindowToolsCaptureView.cpp:493`） | 7 | 无 | 无 | — |
| 窗口列表（`WindowView.cpp:1744`） | 5（+4 隐藏） | 无 | — | 固定 170/720 |

- 所有表的复制/导出只挂在行右键菜单，"复制可见结果"不可发现。
- 详情/表格分界是固定常量（210 / 200 / 168px），不可拖动，长字段（R0 偏移串）只能横向滚。
- 列宽不随视口变化：1280 窗口下 720px 值列被裁。
- **加载不裁剪**（结构性问题，详见 §2.7）：主表已走 `VirtualListView`（`create` 内部强制 `LVS_OWNERDATA`），但详情小表与"切视图重插"路径仍是逐项插入，且全工程 0 处"显示上限 / 已截断"提示；行数上万时 UI 线程直接卡死，且没有任何一处告诉用户"只显示了一部分"——数据看起来完整，其实是被截断的静态假象。这一类问题在 Qt 版本里由 `QAbstractItemModel` 兜住，Win32 默认不兜，必须自己补。

### 3.4 留白与分区（§4）

- 各页 `kGap` 常量各自定义，页根四边留白不等宽，父子容器重复累加。
- 分区靠"1px 实线框 + `QGroupBox` 式图例"表达（原生 `BUTTON BS_GROUPBOX`），规范明确禁止。
- 空态靠 `PlaceholderPage`（单行文字 + 进度条），不是共享空态组件。

### 3.5 状态真实性（§1）

- 状态行统一是单行 `DT_END_ELLIPSIS` 绘制（`NetToolsConnectionView.cpp:788` 等 6 处），**失败原因被省略号截断且不可复制、无 tooltip**。
- `WindowToolsCaptureView.cpp:345`：动作结果（含 `ERROR_ACCESS_DENIED`）被随后的刷新摘要覆盖，用户无法判断操作成败。
- `R3HardwareUsb` 同类问题：`--kind` 过滤在"角色分类不可用"时返回空列表 + 退出码 0。

---

### 3.6 页面结构覆盖表（41 个页面文件 / 24 个功能目录）

口径：`grep -rln WC_LISTVIEWW`、`-nH "WC_TREEVIEWW\|WC_TABCONTROLW"`、`-nH WS_EX_CLIENTEDGE` + 逐页 Read 抽样核实调用点上下文。

| 目录 | 页面文件 | 页根 / Tab / 工具带 / Scroll / Splitter / 详情框架 / LoadingOverlay / ThemedMessageBox / 列组 | 判定 |
|---|---|---|---|
| Driver | DriverDebugOutputView | ✓ / — / ✓ / — / — / — / — / — / — | 部分共享 |
| Driver | DriverFeature | ✓ / ✓ / ✓ / — / — / — / — / — / — | 部分共享 |
| Driver | DriverObjectView / OverviewView / ThreadView | ✓ ×3 / — / ✓ ×2 / — / — / — / — / — / — | 部分共享 |
| Handle | HandleFeature / HandleView | ✓ ×2 / ✓ / ✓ / — / — / — / — / — / — | 部分共享 |
| Hardware | HardwareFeature / HwidDispatchView | ✓ / ✓ / ✓ | 部分共享 |
| HardwareStats | HardwareStatsView | ✓ / ✓ / ✓ / — / — / — / — / — / — | 部分共享 |
| Kernel | **KernelPage** | **全 —**（36 处 `CreateWindowExW`、6 个 ListView、手绘分区标题） | **零框架** |
| Memory | MemoryFeature / MemoryView | ✓ / ✓ / ✓ / — / — / — / — / — / — | 部分共享 |
| Misc | MiscFeature | ✓ / ✓ / ✓ / — / — / — / — / — / — | 部分共享 |
| Monitor | CallbackMonitorView / EtwFilterDialog | CallbackMonitor **零框架**；EtwFilterDialog 模态 `DialogBoxParamW` | **零框架 + 弹窗无主题** |
| NetTools | ConnectionView / FirewallView / DiagnosticView / HotkeyView | ✓ / ✓ / ✓ / — / — / — / **DiagnosticView 零框架** / — | 部分 + 1 零框架 |
| Network | NetworkFeature / NetworkView | ✓ / ✓ / ✓ | 部分共享 |
| Privilege | PrivilegeFeature | ✓ / ✓ / ✓ | 部分共享 |
| Process | ProcessView | ✓ / ✓ / ✓ / — / — / — / — / — / — | 部分共享 |
| ProcessDetail | ProcessDetailPage（+2） | ✓ / **手绘 10 个扁平 Tab，无层级** / ✓ / — / — / ✓ / — / — / — | **Tab 关系崩坏**（`BS_GROUPBOX` ×12、`Process Detail Evidence` 中英混排） |
| Registry | RegistryFeature / RegistryView | ✓ / ✓ / ✓ | 部分共享 |
| Service | ServiceView | ✓ / ✓ / ✓ / — / — / — / — / — / — | 部分共享 |
| Startup | StartupView | ✓ / ✓ / ✓ | 部分共享 |
| SysTools | **SystemTimeView** / IoctlDecoderView | SystemTime **零框架**（`:306-324` 日期组合框被纵向拉成约 180px 高）；IoctlDecoder **零框架** | **2 零框架** |
| Window | WindowView | ✓ / ✓ / ✓ / — / — / — / — / 手写 TSV 截断 / — | 部分共享 |
| WindowTools | Clipboard / Hotkey / Hierarchy / Capture | ✓ ×4 / — / ✓ ×4 / — / — / — / — / — / — | 部分共享 |

**汇总**：41 个页面中 **30 个至少用了页根+菜单+工具带**，**6 个零框架**（`Kernel/KernelPage.cpp`、`SysTools/SystemTimeView.cpp`、`Hardware/HardwareHwidDispatchView.cpp`、`Monitor/CallbackMonitorView.cpp`、`NetTools/NetToolsDiagnosticView.cpp`、`SysTools/IoctlDecoderView.cpp`），**5 个自建详情/对话框框架**。`Ui/SplitterUtil.*` 存在但**零消费者**——"共享分隔器"只是文件。

### 3.7 共享组件被绕过（可量化）

| 指标 | 数量 | 说明 |
|---|---|---|
| `MoveWindow` 调用（`KswordARKLight/Features`） | **555** | 全部绝对坐标 |
| `SetScrollInfo` / `WM_VSCROLL` / `ScrollWindowEx` | **0** | 无滚动容器，内容超出即被裁 |
| `::MessageBoxW` | **101**（Features 内 94） | 仅 7 处走共享封装；无危险语义、无主题 |
| 裸 `WC_LISTVIEWW` `CreateWindowExW` | 19（`hbrBackground` 不随主题变化） | 表头/选中行主题无法跟随 |
| `WS_EX_CLIENTEDGE` | 66 | 与深色主题冲突的主要来源 |
| 每页自写剪贴板实现 | 22 处 | 绕过 `Ui::ExportUtil.cpp:191 CopyTextToClipboard`，**丢失证据会话记录** |
| `TrackPopupMenu` 直接调用 | 70 处 | `Ui/MenuUtil.*` 形同虚设（共享封装调用 0 处），菜单底色/状态色无统一来源 |
| 硬编码 `RGB(...)`（Features） | 28 处 / 11 文件 | 主题外自造颜色 |
| `hbrBackground = COLOR_WINDOW+1`（WNDCLASS 注册） | 62 处（51 文件） | 系统色，深色模式下与面板色断层 |
| 圆角实现 | **0** | `CreateRoundRectRgn`/`CRgn` 全无；`WS_EX_STATICEDGE` 0 处 |

**量化结论**：`Ui/` 38 个文件 5527 行，只提供**控件工厂**，不提供**页面骨架**；共享层与业务页之间没有强制约束，因此 555 处 `MoveWindow` 各自为政。

### 3.8 与主程序的关系：只借后端，不借 UI

- Light 对主程序 `/Ksword5.1/Ksword5.1/` 的引用只有 **5 个后端头**（共享 IOCTL/结构体），**UI 头文件引用为 0**。
- 主程序共享 UI 符号（`InstallStandardWidgetTheme`、`ParameterWorkspace`、`MainSurfaceStyle`、`StylePageTabs`、`StyleJoinedResultTabs`、`NormalizeToolbarRow`、`DetailLayoutRegistry`、`ThemedMessageBox`、`CodeEditorWidget`）在 Light 全树命中 **0 处**。
- `docs/` 中没有任何一份 UI 规范文档把 ARKLight 列为适用对象——现有 UI 审核基线从未覆盖过它，这解释了"一直做得差且没人发现"的成因。


### 3.9 主题"未收口"而非"未建立"（space-bunny 独立统计）

需修正一处判断：`AppTheme()` 被 **54 个文件、238 处**引用，token 体系本身是健康的，问题在**集合太窄 + 没有共享入口**。

| 指标 | 数量 |
|---|---|
| 硬编码 `RGB(...)`（不含 `Theme.h` 种子） | 40 处 / 43 文件 |
| 页面级样式覆写（`WM_CTLCOLOR*` + custom draw 赋色 + 临时 brush/pen） | 125 处 |
| 硬编码字号/字体（`lfHeight`/`lfFaceName`/`FW_*`） | 7 处（集中在 1 个函数） |
| `WM_CTLCOLORSTATIC` 块 | 40 处 / 34 文件（零共享入口） |
| 重复定义 `constexpr kGap` | 22 个文件 |
| 语义"红"变体 | **4 套值**（`RGB(176,48,48)` / `0xB23A3A` / `RGB(196,43,28)` / `RGB(176,32,32)`） |
| 语义"橙"变体 | 3 套（`RGB(176,112,0)` / `0xD77A00` / `RGB(158,104,0)`） |

**三条最具杀伤力的实证**

1. **只读多行 EDIT 走 `WM_CTLCOLORSTATIC` 被强制 TRANSPARENT → 滚动时文字涂抹。** 仓库自己在 `NetToolsDiagnosticView.cpp:434-437` 写下注释确认这是已知坑并修好，但该修法只推广到 **3 个文件**，**13 个文件仍是裸写法**：`EventLogView.cpp:657`、`ContextMenuView.cpp:743`、`IoctlDecoderView.cpp:209`、`SystemTimeView.cpp:494`、`KernelPage.cpp:2513`、`DriverMemoryView.cpp:1516`、`EtwMonitorView.cpp:370`、`ProcessDetailPage.cpp:249`、`ProcessDetailPage.Module.cpp:519`、`HardwareHwidDispatchView.cpp:721`、`DriverObjectView.cpp:283`、`DriverOverviewView.cpp:301`、`DriverThreadView.cpp:526`。这是用户最容易直接看到的"残影/花屏"。
2. **交互态对比度不达标**：Dock 页签 hover 底 `RGB(235,240,248)` vs 面板白 = **1.14:1**（等于没有 hover）；拖拽落位预览 `RGB(191,222,255)` vs 窗口底 = **1.30:1**（提示几乎不可见）。`ProcessView` 自绘整行 `CDRF_SKIPDEFAULT` 却**无 hover 分支**，`LVS_EX_TRACKSELECT` 全树 0 命中；`DockManager` 的 `hotTabDock_` 也没有 `WM_MOUSELEAVE` 复位，鼠标移出后悬停色会粘住。
3. **唯一一处硬编码字体**：`ProcessDetailPage.cpp:75-89` 的 `CreateTitleFont()` 写死 `lfHeight = -18`、`lfFaceName = L"Segoe UI"`、`lfWeight = FW_BOLD`，并用 `×1.35` 魔法倍率覆盖系统字体 —— 在无 DPI 感知的前提下必然与系统字号脱钩。

**结论修正**：根因不是"没有 token"，而是「token 只有 7 个色 + 没有页面骨架 + 没有共享着色函数 + 没有构建门禁」四者叠加。补齐顺序：扩 token（纯新增）→ 共享 `WM_CTLCOLOR` 分流入口（34 文件一次替换）→ 语义色收敛 → Win32 版 `theme_token_audit` 门禁 → 交互态补齐。

---

## 4. ARKLight 正确性 / 性能

按严重度摘要（完整条目见分区子报告）：

**P1**
- `Features/Window/WindowView.cpp:1239` 排序切换即时 `model.setSortMode`（`WindowModel.cpp:45` 立即 `rebuildRows`），但显示快照等后台 `EnumWindows` 才更新；`itemData` 存的是下标 → 期间右键"关闭窗口/置前/捕获保护"打错窗口。
- `Features/NetTools/NetToolsFirewallView.cpp:281`：行内 14 个 cell，表头 9 个；`ExportUtil.cpp:181-186` 不按标题数截断 → 导出的 TSV 列错位。
- `Features/WindowTools/WindowToolsCaptureView.cpp:662`：676 行"捕获保护"页已编进产物（`.vcxproj:206`）但无任何 Tab 入口，而 `FeatureRegistry.cpp:34` 对用户承诺了该功能。

**P2**
- 5 个页面在 **UI 线程**做整表格式化 + 二次深拷贝（`NetToolsConnectionView.cpp:363`、`FirewallView:295`、`CaptureView:219`、`HotkeyView:178`、`MiscFeature:533`）；对照 `WindowView.cpp:1131/1149` 的正确写法（worker 构建 + `setSharedRows`）。
- `Ui/VirtualListView.cpp:46`：每次刷新先恢复全量可见索引再异步筛选 → 筛选条件下整表闪现并重置滚动位置。
- `WindowView.cpp:933`：一次选中触发两个后台任务（shell 属性 + R0 `queryWin32kWindowDetail` + 全桌面 Z 序），键盘上下移动即逐格触发，无去抖。
- `MiscFeature.cpp:1083`：6 个审计子页在 `WM_CREATE` 全部建好（每页 3 个 AsyncSnapshotTask = 18 个任务对象），未惰性化。
- `WindowToolsCaptureView.cpp:345` / `WindowToolsClipboardView.cpp:489`：动作结果被刷新摘要覆盖。
- `Ui/AsyncTask.h:116-121`：`consume` 在 owner 匹配但 identity 不匹配时返回 `true`（吞消息）；而 `WM_APP+n` 编号在多页间重复（`FirewallView:48` 与 `ClipboardView:47` 都是 678/679）→ 撞号时某个任务永久 pending，刷新按钮永久禁用。

**P3**
- `WindowView.cpp:1902` 在 `WM_DESTROY` 释放 `state`，其余 7 页在 `WM_NCDESTROY` → 隐式契约，后续改动易成 UAF。
- `WindowView.cpp:1736` 进程图标设施（`ImageList_Create`、`SHGetFileInfoW`、`ProcessIconIndex`）全无调用点，死代码。
- `WindowToolsClipboardView.cpp:496`：`wstring(length)` 却写 `length+1` 个 `wchar_t` → C++11 起 UB。
- `NetToolsDiagnosticView.cpp:88` / `FirewallView:117` / `MiscFeature:566`：剪贴板写入重复实现 3 份，且绕过 `Ui/ExportUtil.cpp:191` 的 `CopyTextToClipboard`（丢失证据记录）。
- `WindowView.cpp:1700` 同页中英混排（`Refresh/Front/Restore` + `导出 TSV`）。

### 4.1 共享层细节（经复核修正口径）

**P2｜导出空态：语义丢失 + 公共 API 缺自保护**
口径已由两轮交叉复核校准（`BuildVisibleVirtualListTsv` 全仓 13 处命中 = 1 定义 + 12 调用，分布在 12 个 Feature 文件）。

- **不是**"会写出 BOM 空文件"：12/12 调用点都判空了（页内 7 处：`WindowToolsHierarchyView:431`、`WindowToolsClipboardView:411`、`WindowToolsCaptureView:276`、`WindowToolsHotkeyView:280`、`NetToolsConnectionView:382`、`NetToolsFirewallView:380`、`RegistryView:1173`；集中 4 处：`DriverFeature.cpp:210-213` 统一挡 Driver 四页；另有 `NetworkView.cpp:386-389`）。该表述已撤回。
- **残留 1｜语义丢失**：`Ui/ExportUtil.cpp:177-179` 把"本来没有数据"与"筛选后为空"压成同一个空串，12 处只能说同一句"没有可导出的可见结果"，筛选场景下用户会误判为无数据。
- **残留 2｜公共 API 不自保**：`WriteUtf8TextToDestination("")` 本身不安全——`ToUtf8WithBom`（`:118-133`）无条件插 BOM，使 `:162` 的 `bytes.empty()` 判空永远不成立，从而写出 3 字节并返回 `Saved`。当前安全性完全依赖 12 个调用点各自判空。建议把空串短路下沉到 `ToUtf8WithBom` / `WriteUtf8TextToDestination`。

**P1｜`Theme::shutdown()` 顺序隐患**
62 处 `hbrBackground` 在 `WNDCLASS` 注册时被长期持有（`FilterBar.cpp:177`、`LoadingOverlay.cpp:97`、`PlaceholderPage.cpp:108`），销毁顺序由静态析构决定。目前**仅靠**这些类全部由 `WM_ERASEBKGND` 返回真（`FilterBar.cpp:146`、`LoadingOverlay.cpp:37`、`PlaceholderPage.cpp:35`）掩盖；一旦有人放开背景擦除，就会刷到已删除的 brush。同理 `PlaceholderPage.cpp:110-111` 忽略 `RegisterClassW` 返回值。

**P3｜可见的功能/实现落差**
- `WindowToolsCaptureView.cpp:662` 的 676 行"捕获保护"页编进产物但无 Tab 入口，而 `FeatureRegistry.cpp:34` 对用户承诺该功能。
- `WindowView.cpp:1736` 的进程图标设施（`ImageList_Create` + `SHGetFileInfoW`）全无调用点。
- `WindowToolsDialogButtonWidth`、`kGap` 在每页各自定义，页根留白不等宽。

---

### 4.2 逻辑与生命周期（space-bunny 独立复核，20 条）

**P1×4 — 都会导致"功能永久卡死"或"驱动残留"**

1. **`Features/Kernel/KernelCallbackEventReceiver.cpp:190` 线程创建失败未捕获。** `workerRunning` 在 `:188` 先置 `true`，`:190` 的 `std::thread(...).detach()` 无 `try`；抛 `std::system_error` 后 `workerRunning` **永不复位**（只有 `RunReceiver` 里会复位，而线程从未启动）。链式后果：`stopping()` 恒真 → `Start()` 恒返 false → `KernelPage.cpp:7939` 提示"正在停止，取消完成后可再次启动"，但**永远恢复不了**。`KernelPage.cpp` 全文 `catch` 计数为 0，异常还会穿透 `WndProc`。
2. **`Features/Registry/RegistryView.cpp:565-571` 丢弃分支不复位 `childrenLoading`。** 回调在 `snapshot->mode != state.mode`（565）与 `!item`（569）两个分支直接 `return`，既不 `clear` `treeLoadingPath` 也不复位节点标志；配合 `AsyncSnapshotTask` 只投递最新 generation 的合并语义，该节点**永久停在"加载中"且再也无法展开**。
3. **`Core/DriverLease.cpp:216` + `App/MainWindow.cpp:1641` 退出链路三处"失败当成功"。** 互斥体等待超时（`MutexGuard` 5s）时整块清理被跳过，但 `released_`（`:207`）与 `registered_`（`:231`）已双双置位 → **不可重试**；返回值 `false` 被 `MainWindow.cpp:1641-1642` 解读为"无需停驱动"，随即 `finalSucceeded = true` 走成功分支 `DestroyWindow`。结果：驱动可能在用户退出后**残留运行**，且共享内存不留记录。
4. **`Core/DriverLease.cpp:107-112` 析构丢弃"应停驱动"结论。** `(void)releaseRequestsStop();` 把返回值丢掉，`ShouldStopOnLastRelease` 为真时无人调 `StopDriverService()`，与第 3 条叠加成同一后果。

**P2×10 — 一类根因 + 若干可辨识错误**

- **13 处"标志先置、请求后发"的死锁簇（治本一处修复即可全消）。** `xxxInProgress = true` 先于 `request()`，唯一复位点在 deliver lambda 内；而 `AsyncSnapshotTask::request` 有 **3 条静默 `return`**（`AsyncTask.h:62` 未 alive、`:69` 关闭准入被拒、`:76-78` 锁内失效）。命中后按钮永久禁用，必须重启进程。分布：`FileHolderView:239/247`、`ContextMenuView:382/386`、`StartupView:433/437`、`ServiceView:555/559`、`PrivilegeView:359/363`、`NetToolsConnectionView:443/447`、`HwidDispatchView:450/455`、`RegistryView:986/989`、`DriverMemoryView:587/592`、`:639/644`、`:707/712`、`:942/947`。根因是 `request()` 返回 `void`，调用方无从得知是否真的启动。
- **`DriverDebugOutputView.cpp:666` `WM_TIMER` 判定顺序错。** 先判 `retirementCancelled` 再判 `wParam`，会把任意无关定时器（如 `FilterBar` 的 180ms 防抖）吞掉；同结构的 `CallbackMonitorView.cpp:709-717` 才是正确范式 —— 说明是可复用的模式错误，不是笔误。
- **"不可用"被渲染成"确实为空"。** `EventLogView.cpp:323-331`、`FileHolderView.cpp:260-267` 在 `success == false` 时**清空数据**重建空表且**跳过 `RequestFilter`**，与"该通道没有事件"视觉一致；`ContextMenuView.cpp:315-318` 把 `diagnosticText` 整段丢弃；`DriverFeature.cpp:153` 漏检 `snapshot->success`，枚举失败照样重建两个虚拟行模型。四处都已经**拿到了诊断文本却没呈现**。
- `DriverFeature.cpp:157-160` + `:299-304`：`Build*VirtualRows` 在 **UI 线程**全量重建，且每次切 tab 重复两次（对照 `WindowView.cpp:1112-1133` 已正确放 worker）。
- `MainWindow.cpp:1592`：租约获取失败只 `OutputDebugStringW`，release 版等于无提示，而这是"自动停驱动已禁用"的关键状态。
- `MainWindow.cpp:1393-1394`：证据导出取不到准入时静默返回，用户已选完保存路径却什么都没发生。
- `ProcessView.cpp:2637-2645`：取消退出后不 `resetOwner`，自动刷新永不再采样，也无文案说明。

**P3×6**：`DriverService.cpp:444` `ChangeServiceConfigW` 返回值未检查（失败后仍 `StartServiceW`，可能加载旧驱动二进制）；`VirtualListView.cpp:94-101` 正则语法错误静默降级为子串匹配（唯一暗号是按钮变 `.*!`）；`AsyncShellIconLoader` 线程创建失败静默；`AsyncTask.h:69` 关闭准入拒绝无观测点；`ProcessView.cpp:487` `SetTimer` 返回值未检查；`DriverMemoryView.cpp:1544-1545` 先 `delete state` 后清 `GWLP_USERDATA`。

**基础设施评价（不必返工）**：`AsyncTask.h` 的消息只带 identity+generation、`publish` 先解锁再 `PostMessageW`、`consume` 四重校验、`RetireBackgroundValue` 把大对象析构移出 UI 线程都可圈可点；`DriverLease::acquire()` 三个失败分支都正确 `closeHandles()`，`IsEntryAlive` 用 PID+创建时间防复用。问题集中"调用方如何应对被拒绝"这一层。

---

## 5. KswordCLI

### 5.1 系统性缺陷：写操作门禁"两套标准"

根因：`CommandRegistry.cpp:112-114` 把 legacy 语法挪进 `legacySyntax`，`syntax` 换成 R3 版；而 `KswordCLI.cpp:1414-1441` 的 `validateCommandOptions` 用 `kCommandHelps[].syntax`（legacy）做白名单。不带 `--backend` 时 `dispatchR3` 返回 `nullopt`（`CommandRegistry.cpp:178`）→ 落到 legacy 分支。

结果形成闭环缺陷：**默认路径不校验 `--confirm`，且 legacy 语法不含 `--confirm`，用户主动加确认反而报 "unknown option"**。

| 命令 | 默认路径 | 风险 |
|---|---|---|
| `kernel force-unload-driver`（`KswordCLI.cpp:5539`） | 无确认 | 摘除任意驱动（含安全驱动），最坏 bugcheck |
| `callback remove` / `remove-ex`（`:6006/6026`） | 无确认，`--confirm` 非法 | 摘除内核回调，AV/EDR 对抗原语零门槛 |
| `file delete-path`（`:4988`） | 无确认 | 删任意文件 |
| `process terminate/suspend/resume/set-ppl`（`:2631-2643`） | 无确认 | 结束/挂起任意进程 |
| `memory write-va` / `write-phys`（`:2996/3054`） | **默认自带 `UI_CONFIRMED` 位** | 协议审计链被污染：日志声称"已确认"但无任何确认动作 |

### 5.2 只读门禁：不仅"没有运行时开关"，脚本本身已失效

两层都缺：

1. **无运行时开关**：全仓 `readOnly|readonly|READONLY` 只命中注释与 R3 字段名，没有 `--read-only` 或环境级只读。"只读"只是"某些命令恰好不发 IOCTL"。
2. **唯一的静态门禁脚本跑不起来**：`KswordCLI/Test-KswordCLIReadOnly.ps1:207-226` 的 `Get-HelpCommandKeys` 仍按旧 help 格式（`family | sub1 | sub2`）解析，而现行 `help` 输出（`CommandRegistry.cpp:130-133`）第二列已是英文摘要 → `$supported` 近乎为空 → `:631-634` 必然 `throw "The sibling CLI is missing catalog commands"`。且排除表（`:228-246`）与白名单（`:261-341`）**零条 R3 命令**（R3 命令 2026-10-09 才引入，脚本写于 2026-09-29）。对照 `tools/Test-KSwordCliR3Coverage.ps1:10,15` 已用可解析新格式的正则 —— 说明是脚本未跟进，不是格式不可解析。

```powershell
217: if ($line -match '^\s*(?<family>[a-z][a-z0-9-]*)\s+(?<subcommands>.+?)\s*$') {
220:     foreach ($subcommand in ($subcommands -split '\s*\|\s*')) {
221:         if ($subcommand -match '^[a-z][a-z0-9-]*$') { [void]$keys.Add("$family $subcommand") }
```

后果：写操作回归防线**静默失效**（不是报错即停）。

### 5.3 输出与编码

- `hasTrailingHelpToken`（`:1621`）扫描所有 token（含选项值）→ `--path help`、`--filter -h` 被当成帮助请求并**以退出码 0 退出**，流水线会把"没执行"当成功。
- `fixedAnsiWide`（`:5160`）逐字节扩宽（等同 Latin-1）而控制台是 `_O_U8TEXT`（`:8801`）→ 驱动返回的 PDB 名/模块名/缺失项名称乱码；同仓 `ArkDriverExtended.cpp:135` 有正确的 `MultiByteToWideChar(CP_UTF8)` 版本并存。
- `log` 家族（`:2046`）忽略 `argv[2]`，`log raed` 拼错仍返回 0。
- `r0 directory` / work-queue（`ArkDriverExtended.cpp:203/216`）静默截断，不打印 `truncated=true total=N`；同文件 `:366` 的 callback monitor 却会打印 —— 取证结论可被静默改写。
- `r0 debugger-status`（`:492`）用退出码 1（=用法错误）表示传输失败，与同文件 `finishResult` 的 2/3/5 分级冲突。
- `parseU64`（`:637`）接受 `-1` 并回绕成 `0xFFFFFFFFFFFFFFFF`；`parseHexBytes`（`:471`）丢弃非法字符（`--hex "12ZZ34"` → 写入 `12 34`），而 `R3Input.cpp:33` 的同类函数严格报错。

### 5.4 取消与并发

`R3Cancellation.cpp:6-16`：控制台处理器无条件返回 TRUE（吞掉 Ctrl+C），`cancelled` 是文件级 `static`（所有实例共享），`SetConsoleCtrlHandler` 非引用计数 → 任一实例析构即注销全局处理；在不可中断的 PDH/NtQuery 调用期间连按两次 Ctrl+C 也无法退出，且 `privilege run` 嵌套时外层取消能力会被内层析构静默解除。

### 5.5 做得对的部分（避免过度返工）

- IOCTL 缓冲区长度/对齐校验整体扎实：`validateVariable`（`:1684-1697`）、`sendFixed*Response` 的 `bytesReturned < sizeof` 检查、`checkedDwordSize`、`headerSize = sizeof - sizeof(payload)` 与 `payload[1]` 末成员布局一致；`write-va` 的分配与 `memcpy` 无越界。**未发现可确认的 P0 内存安全问题。**
- 取消令牌传播（`Cancellation` → `options.cancelled` / 25ms 轮询）设计正确。
- R3 注册层的 `--confirm` 覆盖较完整——问题在"默认路径绕过它"，不需要重写 R3 层。

---

### 5.6 space-bunny 独立复核补充（25 条，无 P0）

两模型结论方向一致：**未发现 P0 级内存破坏**（`KswordCLI.cpp:1684-1697` `validateVariable`、`:973-1040` 固定响应检查、`RawIoctlCommand.cpp:98-102` 均在解引用前比较 `bytesReturned`）。以下为 space-bunny 独有增量：

**确认门禁缺口（R3 侧其实覆盖较全，缺口是特定几条）**
- `R3Service.cpp:50,84-87`：`start` / `pause` / `continue` 不需要 `--confirm`（只有 `stop` / `set-start-type` 要）—— 启动服务同样执行任意服务二进制。
- `R3Window.cpp:108-112`：`window manage minimize|maximize|restore|foreground|close` 五个都无确认，`close` 走 `CloseWindowGracefully`。
- `R3WindowCapture.cpp:77-81`：`window capture set` 改 `WDA_EXCLUDEFROMCAPTURE`，无确认。
- `R3WindowHotkeys.cpp:52-55`：`window hotkeys probe/scan` 会**真实注册全局热键**（注释自承"can briefly intercept a hotkey"，`scan` 默认扫 1320 种组合），无确认。
- `KswordCLI.cpp:5941-5945` → `ArkDriverExtended.cpp:551-568`：R0 侧 `callback monitor-start/stop` 无确认，而 R3 侧 `system ioctl call`（`RawIoctlCommand.cpp:63-66`）有 —— 同进程内门禁强度不一致。

**协议版本维度缺位（比"长度校验"更隐蔽）**
- `validateVariable`（`:1684-1697`）**从不校验 `version`**：`version` 只被 `printCountHeader`（`:1641-1652`）打印，不参与判定。驱动/CLI 版本不匹配时按当前布局静默解释。
- `KswordCLI.cpp:3184-3198` / `:3214-3232`：`change-mapped-protection` / `change-image-protection` **先打印响应字段、后校验 `version`/`size`**，与注释"不兼容版本不解释为执行结果"自相矛盾（无越界，但输出误导）。

**可诊断性**
- `CommandRegistry.cpp:34`：`status` 只区分 `success`/`partial`/`unsupported`，退出码 1/2/3/4 全部压成 `"failed"`，且 JSON **不含数值 `code`** → "设备未加载"(2) 与 "IOCTL 失败"(3) 在 JSON 里不可区分。
- `CommandRegistry.cpp:56-57`：JSON 转义把 UTF-16 代理对拆成两个 `\uXXXX`，非 BMP 字符（emoji 路径/窗口标题）解析后成两个孤立代理项。
- `KswordCLI.cpp:446-451`：`readFileBytes` 单次 `ReadFile` 后 `resize(readBytes)` → 网络卷/被并发截断的文件会**静默写入截断载荷**；而 `R3Input.cpp:17-22` 的 R3 侧是"读满循环 + EOF 判断"，两条路径行为不一致。
- `KswordCLI.cpp:270-297`：`parseNamedArgs` 强制把下一个非 `-` token 吞作选项值（布尔开关会吃掉真实参数）、`-` 开头的值被当选项名、重复键静默覆盖；`positionals` 全仓无消费者。

**取消覆盖不成体系**
- `R3Cancellation.cpp:5,12`：`cancelled` 是文件级 `static`，**构造时清零**；`privilege run`（`R3Privilege.cpp:93-96` → `dispatchNested`）嵌套执行时内层构造会把外层的取消位抹掉。
- 有 token：`R3HardwareDisk/Performance`、`R3MonitorEtw`、`R3ProcessTelemetry`、`R3WindowHotkeys`、`R3Kernel*`、`R3SecurityShared`。
- 无 token：`R3Service`（SCM 转换最长 30s）、`R3Clipboard`、`R3ProcessHotkeys`、`R3KernelNtQuery`、`R3Window`（`--wait-ms` 最长 10s）。

---

## 6. 建议修复路线

### 设计原则（先划边界，再谈方案）

这三条决定"改到什么程度算够"，否则越改越像再造一个 Qt：

1. **不引入第二套框架**。纯 Win32 单文件路线：不引 Qt、不拆模块、不做 MVVM/数据绑定。共享代码只允许三种形态——无状态自由函数（`Controls.cpp` 那种百行级）、`constexpr` 常量表、`RegisterClass` 一次的自绘子窗口。禁止：抽象基类、虚接口层、消息总线、通用容器再造。
2. **能复用 40 行就不新增 400 行**。现成件先用尽：`VirtualListView`、`ListViewUtil`、`AsyncTask`、`DriverLease`、`SplitterUtil`（已有但零消费者）。清单里凡写"新建 XXX 模块"的，先回头确认现成件覆盖不了。
3. **优化是一等要求，不是收尾调优**。Win32 默认表格不裁剪比 Qt 更卡（§2.7）。任何列表改动必须同时满足：owner-data 虚拟化、快照共享不深拷贝、有显示上限与截断提示、重活不在 UI 线程。四条缺一条就算没改完。

### 阶段 0：止血（不改架构，1 周内可完成）

1. **DPI 感知**：加 `KswordARKLight.manifest`（PerMonitorV2）+ `WM_DPICHANGED` 重排 + 统一 `Scale(int)` 入口。这是"发虚 + 错位"的总开关。
2. **按钮角色**：`CreateButton` 增 `ButtonRole{Default,Primary,Danger}`，`Theme` 补 danger 色；先接 4 个破坏性动作。
3. **CLI 关禁**：先修 `Test-KswordCLIReadOnly.ps1:207-226` 的 help 正则与 R3 白名单/排除表（**当前脚本必然 throw，只读防线等于不存在**）；再合并两套参数白名单（`CommandRegistry.cpp:191` 复用 `legacySyntax`）；然后给 `force-unload-driver` / `callback remove(-ex)` / `file delete-path` / `process terminate` 前置 `requireConfirmOption`；移除 `memory write-*` 默认的 `UI_CONFIRMED`；补 R3 侧 4 处缺口（`service start/pause/continue`、`window manage *`、`window capture set`、`window hotkeys probe/scan`）。
4. **CLI 假成功**：修 `hasTrailingHelpToken`（`:1621`）与 `log` 未知子命令（`:2046`）；`readFileBytes`（`:446-451`）改循环读满，否则写内存/注入会静默写入截断载荷；`validateVariable` 补 `version` 判定。
5. **数据正确性**：`WindowView.cpp:1239` 排序改为 worker 一次性提交（消除打错窗口）；`ExportUtil` 增加 `columnCount` 截断（消除 TSV 列错位）；`CommandRegistry.cpp:34` 的 JSON 补数值 `code`。
6. **表格止血（半天，收益最大）**：`Ui/ListViewUtil` 增 `PopulateCapped(hwnd, rows, kMaxDisplayRows)`，超限时列表只填前 N 条、状态栏显示"已显示 N / 共 M 条（已截断）"。先接 RegistryView、Service、Startup、Hardware 四个非虚拟页面。不做分页控件、不做滚动加载——一个常量 + 一行文案。

### 阶段 1：补共享层（2–4 周）

7. `Ui/` 增三件套，API 各控制在 5 个函数以内：**布局入口**（行/列/分区 + 自然高度 + 窄窗换行，替换 555 处 `MoveWindow`）、**工具栏度量**（`GetTextExtentPoint32W` 实测宽度，按全角算）、**表格外壳**（列组、表头右键、操作条模式、可拖动分隔）。
8. `Ui/Theme` 升级为 token 表：浅/深两套 + 状态色 5 组 + 字号阶梯（14px 下限），并提供 `RefreshTheme()` 让已建控件重绘（等价 `ThemeBinding`）；同步把 62 处 `hbrBackground` 改为主题刷子并明确注销顺序。
9. 统一绘制：`PaintPanel` 去描边、改双层缓冲；`MainWindow` 加兼容 DC；`Features/` 全部改走共享 `PaintPanel`。
10. `Ui/Controls.h` 增共享 `WM_CTLCOLOR*` 分流入口（判别只读 EDIT / STATIC / BUTTON，返回正确底色与文字色），**一次替换 34 文件 × 40 处** —— 顺带根治 13 个文件里"滚动只读框文字涂抹"的残影问题。
11. `Ui/Theme.h` 补 token：`surfaceHover/Pressed/Alternate`、`accentText`、`rowStateAdded/Removed/KernelOnly`、`statusOk/Warning/Error/Unsupported/Neutral`（由种子派生 + 对比度自检）；收敛 4 套"红"、3 套"橙"到同一组语义色。
12. **表格收口（承接阶段 0 第 6 条）**：13 个 `ListView_InsertItem` 页面迁到 `LVS_OWNERDATA`，直接复用 `VirtualListView`，不新写列表控件；`setRows` 调用点全改 `setSharedRows` 免深拷贝；`resetVisibleIndexes()` 改为"筛选结果直出"，消除整表闪现与滚动重置；5 个 UI 线程整表格式化的页面改 worker 构建；追加路径照 `EtwMonitorView` 的"每 tick 至多 N 行 + 尾部裁剪"写法。
13. **收口重复实现**：22 处自写剪贴板 → `Ui::CopyTextToClipboard`；70 处 `TrackPopupMenu` → `Ui/MenuUtil`；101 处 `MessageBoxW` → 共享消息框（带危险语义）；把 `SplitterUtil` 接上（当前零消费者）。
14. **空导出守卫下沉**：`ToUtf8WithBom` / `WriteUtf8TextToDestination` 对空串短路，不依赖 12 个调用点。
15. **门禁**：扩展 `tools/theme_token_audit.py` 支持 Win32（解析 `Ui/Theme.h` 的 COLORREF 成员，标记 `RGB(...)`/`GetSysColor`/重复 `WM_CTLCOLOR`），再加两条列表规则（新增 `ListView_InsertItem` 即失败、`setRows` 调用点告警），在 `KswordARKLight.vcxproj` 加 `BeforeTargets="ClCompile"` 目标 —— 否则以上全部会重新腐化。

### 阶段 2：结构与体验（1–2 月）

16. 页签容器：16 模块单行 22px → 溢出菜单 + 分组 + 可滚动；父子 Tab 留白合并。
17. **补齐 6 个零框架页**（KernelPage、SystemTimeView、HardwareHwidDispatchView、CallbackMonitorView、NetToolsDiagnosticView、IoctlDecoderView）；重做 `ProcessDetailPage` 的 10 个扁平 Tab（改为分组导航 + 二级页），清掉 12 处 `BS_GROUPBOX`。
18. **术语收口（替代原"i18n 落地"一项）**：Light 只做中文，**不建语言包、不建字符串表、不做运行时切换**。只在 `Ui/Text.h` 放一组 `inline constexpr wchar_t*` 常量，收口菜单 / Tab / 列头 / 按钮四类高频词，解决 `Handle` 41 处 vs `句柄` 63 处、`Callback` 86 处 vs `回调` 80 处这类同概念两名问题。工作量约 20 个常量，不新增类。
19. 详情/表格分界改可拖动并持久化；详情值列随视口自适应。
20. 惰性化与增量刷新：`MiscFeature` 6 页、`CaptureView` 等按激活创建；选中事件去抖（~200ms）；长列表刷新从"整表重建"改"增量追加 + 尾部裁剪 + 只失效可见区"（`LVSICF_NOINVALIDATEALL` 已在 `VirtualListView.cpp:191` 用上，继续沿用）。

---

## 7. 未验证边界

- 全程只读静态审核：**未构建、未运行主程序**，所有 DPI/裁切/窄窗结论来自代码常量与实际字体宽度的比对，需实机确认。
- 未在浅色/深色/背景透明度三种模式下做视觉验收（Light 无深色模式，属已知结论而非未验证项）。
- 性能结论（UI 线程耗时、首帧开销）来自调用结构推断，无实测数据；`Misc` 6 子页首帧未测量。
- 15 份子报告全部交付（含 2 轮交叉勘误：`hbrBackground` 62 处/51 文件、含硬编码 RGB 文件 11 个、`FilterRowIndexes` 33 处、导出空态口径收窄）。
- 本版按产品决策撤回 1 项：~~无 i18n 设施~~ → Light 只做中文，i18n 不在范围内（§2.5）。同时按"表格必须裁剪"的硬要求补做了一次专项取证（§2.7：20 处 ListView 创建点 / 5 页 owner-data / 13 文件仍 `InsertItem` / 0 处显示上限），该组数字为本版新增实测，未走交叉复核。
- 表格卡顿的**临界行数未实测**（"上万行卡死"是按 `LVM_INSERTITEM` 逐项插入的调用结构推断，需在真实注册表大键或 ETW 高负载下量一次再定 `kMaxDisplayRows` 的取值）。
- 标注"需确认"的条目（如 `R3SecurityShared.h:33` 的 `memcpy` 缺尺寸检查、`R3MonitorEtw.cpp:40` 的下溢、`--backend r0` 的 argv 搬移、`ProcessView.cpp:2637` 取消后不复活 owner）在当前代码路径下不可触发或不变量成立，需运行时确认前置条件。
- CLI 未做逐命令功能等价性验证，也未运行 `Test-KswordCLIReadOnly.ps1`（该脚本当前本身会 throw，见 §5.2）。
