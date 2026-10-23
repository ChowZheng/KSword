# 状态文字遮挡点清单

审计对象：`E:\Projects\Ksword5.1\KswordARKLight\`（纯 Win32，绝对坐标 `MoveWindow`，无布局引擎）。
审计方式：只读静态审计，逐文件读布局常量与所有 `MoveWindow` / `DeferWindowPos` 调用，再用子控件矩形与文字矩形求交。

判定口径（重要）：

1. 父窗口 `WM_PAINT` 里 `DrawTextLine` / `DrawTextW` 画出的文字会被该窗口自己的**子控件**覆盖（子控件是独立 HWND，绘制在父窗口之上）。
2. 子控件之间按 z 序覆盖，后创建的在上。
3. 竖向重叠量 ≥ 文字行高的 1/2，记为「盖住整行」；< 1/2 记为「盖住半行」；横向只压住文字起点、重叠 ≤ 8px，记为「压住首字符」。
4. **组合框不计入**：`CBS_DROPDOWNLIST` 的窗口矩形包含下拉区高度（如 `ServiceView` 的 `startTypeCombo` 高 192、`RegistryView` 的 `modeCombo` 高 300、`SystemTimeView` 三个 combo 高 180、`KernelPage` 的 `includeCombo_` 高 240），矩形虽然与状态文字/筛选栏相交，但收起时只绘制约 24px 的编辑区，下拉列表是独立 popup 窗口，不构成遮挡。凡本清单未列出而读者自行算到「combo 压住文字」的地方，均属这一类。

---

## 已修复（参考基准）

| 文件:行 | 现状 rect | 原先冲突源 | 结论 |
|---------|-----------|-----------|------|
| Features/Hardware/HardwareView.cpp:863 | `RECT textRc{ 104, kGap, rc.right - kGap, kGap + 24 }` = y 6..30 | `refreshButton` x 6..92、y 6..30；`filterBar` (6,35,w-12,59) | 已修：文字左边界 104 > 92，且 30 < 35 |
| Features/Misc/MiscFeature.cpp:971 | `RECT textRc{ textLeft, kGap, rc.right - kGap, kGap + 24 }` = y 6..30，`textLeft` = 216（普通页）/350（Bugcheck 页） | 按钮右边界 206 / 340；`filterBar` y 34..62 | 已修：10px 间距，且 30 < 34 |
| Features/Window/WindowView.cpp:2080 | `RECT textRc{ 510, kGap, kGap + leftWidth, kGap + 24 }` = y 6..30 | 第一行控件（combo 6..186、combo 192..342、刷新 348..426、导出 432..510）右边界 = 510；`filterBar` y 66..90 | 已修：文字左边界 510 = 控件右边界（相接，0px 间隙） |

---

## 待修复

| # | 文件:行 | 控件/绘制 | 当前 rect | 冲突控件及其 rect | 判定依据 |
|---|---------|-----------|-----------|-------------------|----------|
| 1 | Features/Memory/DriverMemoryView.cpp:493（RECT）/ :497（DrawTextLine） | 父窗口 WM_PAINT 直接绘制的分组标签「操作历史筛选（匹配全部列和状态）」 | `(12, 96, rc.right-12, 118)`，即 y 96..118 | `historyFilter` 筛选栏，MoveWindow 于 :470 → `(12, 102, contentWidth, 24)`，`contentWidth = max(100, width-24)`，即 y 102..126 | 竖向重叠 102..118 = 16px（占 22px 行高的 73%）；文字 `DT_VCENTER` 居中于 96..118，字形主体约 100..114，整行落在筛选栏 102..126 之内 → **整行被盖住**。且筛选栏自带占位文字「筛选操作、PID、地址、状态」，两段文字在同一像素区叠字 |
| 2 | Features/Startup/StartupView.cpp:858 | 父窗口 WM_PAINT 绘制的状态文字 `textRc` | `(430, 7, rc.right-6, 31)`，即 x 430..、y 7..31 | `openButton`「打开」，MoveWindow 于 :706 → `(342, kGap, 94, 24)` = x 342..436、y 6..30 | 横向重叠 430..436 = 6px，纵向重叠 7..30 = 23px。文字格式 `DT_LEFT`，从左边界 430 开始绘制 → **首字符左侧 6px 被按钮压住** |

第 2 条的坐标推导：`kGap = 6`，按钮步进 84（刷新 6..84 → 启用 90..168 → 禁用 174..252 → 删除 258..336 → 打开 342..436）。

---

## 边界条件（只有把面板拖到很矮时才发生，均为算出的确定阈值）

| 文件:行 | 公式 | 重叠条件 |
|---------|------|----------|
| Features/Driver/DriverThreadView.cpp:417（detail）、:419（status） | `detailTop = 39 + tableHeight + 7`，`tableHeight = max(50, height-39-detailHeight-32)`，`detailHeight = clamp(height/3,110,220)`；`status` 顶边 = `height-20` | `tableHeight` 被 50 夹住（即 height < 231）时 detail 底边固定 = 96+110 = 206 > `height-20` → **客户区高度 ≤ 225 时 detail 列表压住底部状态行**；height ≥ 226 时不再重叠（间隙 0..5px） |
| Features/Monitor/CallbackMonitorView.cpp:620（detail）、:622（status） | `listHeight = max(80, height-258)`，detail 底边 = `228+listHeight`；status 顶边 = `height-26` | `listHeight` 被 80 夹住（height < 338）时 detail 底边固定 = 308 > `height-26` → **高度 ≤ 333 时 detail 压住状态行**；height ≥ 334 时不再重叠（间隙 0..4px） |
| Features/Registry/RegistryView.cpp:930（tree/list）、:950（statusText） | `contentTop = 66`，`listHeight = max(120, height-158)`，list 底边 = `66+listHeight`；statusText 顶边 = `height-22` | `listHeight` 被 120 夹住（height < 278）时 list 底边固定 = 186 > `height-22` → **高度 ≤ 207 时列表压住状态文字**；height ≥ 208 时不再重叠（间隙 0..70px，height ≥ 278 后恒为 70px） |

---

## 已确认无重叠

### 1. 页脚式状态文字（WM_PAINT 画在客户区底部，列表/详情已预留 `kStatusHeight = 22`）

判定依据：这些文件的 `<list/detail>` 高度公式里都显式减掉了 `kStatusHeight`，最底层子控件的底边恒为 `height - kStatusHeight - kGap`（或 `-92`、`-30` 等更小的值），状态带 `height-22 .. height` 内无任何子控件。

| 文件:行（绘制） | 底部子控件底边 | 状态带 |
|---|---|---|
| Features/WindowTools/WindowToolsCaptureView.cpp:628 | list 底 = height-22-6 | y height-22..height |
| Features/WindowTools/WindowToolsHotkeyView.cpp:519 | list 底 = height-22-6 | 同上 |
| Features/WindowTools/WindowToolsHierarchyView.cpp:691 | list/reportEdit 底 = height-22-6 | 同上 |
| Features/WindowTools/WindowToolsClipboardView.cpp:772 | bottom 区底 = height-22-6 | 同上 |
| Features/HardwareStats/BusDeviceView.cpp:666 | detailList 底 = height-22-6 | 同上 |
| Features/HardwareStats/DiskActivityView.cpp:516 | list 底 = height-22-6 | 同上 |
| Features/HardwareStats/PerformanceView.cpp:523 | list 底 = height-22-6 | 同上 |
| Features/HardwareStats/UsbTopologyView.cpp:647 | detailList 底 = height-22-6 | 同上 |
| Features/NetTools/NetToolsConnectionView.cpp:813 | detailList 底 = height-22-6 | 同上 |
| Features/NetTools/NetToolsDiagnosticView.cpp:454 | outputEdit 底 = height-22-6 | 同上 |
| Features/NetTools/NetToolsFirewallView.cpp:754 | detailList 底 = height-22-6 | 同上 |
| Features/SysTools/ContextMenuView.cpp:746 | detailEdit 底 = height-22-6 | 同上 |
| Features/SysTools/EventLogView.cpp:660 | detailEdit 底 = height-22-6 | 同上 |
| Features/SysTools/FileHolderView.cpp:618 | list 底 = height-22-6 | 同上 |
| Features/SysTools/IoctlDecoderView.cpp:198 | reportEdit 底 = height-22-6 | 同上 |
| Features/SysTools/SystemTimeView.cpp:486 | reportEdit 底 = height-22-6 | 同上；另 `r0StatusText` y 170..190，`reportTop = kHeaderHeight = 192`，留 2px |
| Features/Service/ServiceView.cpp:1142 | detailList 底 = height-22-6 | 同上 |
| Features/Driver/DriverUnloadedView.cpp:652 | list 底 = height-22-6 | 同上 |
| Features/Privilege/PrivilegeView.cpp:647 | detailList 底 = height-22-6 | 同上 |
| Features/Monitor/CallbackMonitorView.cpp:622 | 见上「边界条件」（正常尺寸留 4px） | y height-26..height-6 |

### 2. 表头静态状态文字（`CreateText` 生成的 STATIC 子控件）

| 文件:行 | 状态控件 rect | 同行其它控件 | 结论 |
|---|---|---|---|
| App/MainWindow.cpp:1498 | 非关闭态 `(0, height-22, width, 22)`；关闭态 `(6, height-62, width-12, 22)`（窄）/`(6, height-30, width-250, 22)`（宽） | dock 区 0..height-footerHeight；关闭按钮区 y height-33..height-7 | 无重叠（窄屏状态带 y height-62..height-40，按钮从 height-33 起；宽屏状态右边界 width-250，按钮左边界 ≥ width-238） |
| Features/File/FileFeature.cpp:1604 | `(8, height-28, w-16, 22)`，`tabH = height-40-22-8` | tab 底 = height-30 | 状态带 height-28..height-6，与 tab 留 2px |
| Features/File/FileView.cpp:351 | `(8, listTop+listH+2, w-16, 22)` | list 底 = height-30 | 状态带 height-28..height-6，list 底 height-30，留 2px |
| Features/Network/NetworkView.cpp:430 | `(176, 10, w-188, 20)` | `exportButton` (78,8,160,32)；`summaryText` y 38..76 | x 176 > 160；y 20 < 38 |
| Features/Registry/RegistryView.cpp:950 | `(6, height-22, w-12, 20)` | list 底 height-92；编辑区最后一行按钮底 height-86 | 正常尺寸无重叠（极矮见上「边界条件」第 3 条） |
| Features/Registry/RegistrySearchView.cpp:536 / :546 | hint `(8,8,w-16,21)`；status `(8, height-30, w-16, 22)` | `rowTop = 33`；list 底 = height-34 | hint 底 29 < 33；status 与 list 留 4px，status 底 = height-8 |
| Features/Driver/DriverFeature.cpp:112 | `(200, 10, w-240, 20)` | 刷新 (8,8,90,32)、导出 (96,8,178,32) | x 200 > 178 |
| Features/Driver/DriverDebugOutputView.cpp:606 | `(342, 8, ..., 20)` | `queryButton` (249,6,337,30) | 5px 间距 |
| Features/Hardware/HardwareHwidDispatchView.cpp:579 | `(500, 11, w-508, 22)` | 五个按钮结束于 x=494；`confirmCheck` y 40..62 | 6px 间距 |
| Features/SysTools/SystemTimeView.cpp:324 | `(6, 170, w-12, 20)` | `acknowledge` y 145..169；`reportTop` 192 | 1px / 2px 间隙 |
| Features/Monitor/EtwMonitorView.cpp:458 | `(470, 17, w-482, 24)` | `exportButton` (366,12,454,40)；`localFilterBar` y 46..70 | x 470 > 454；y 41 < 46 |
| Features/Handle/HandlePage.cpp:598 | `(260, 11, w-260, 20)` | `refreshButton` (136,8,248,32)；`filterBar` y 36..60 | 12px 间距；y 31 < 36 |
| Ui/EvidenceSessionView.cpp:312 | `(8, height-28, w-16, 20)` | preview 底 = height-28 | 恰好相接，无重叠 |
| Features/ProcessDetail/ProcessDetailPage.Action.cpp:104 | `(18, 518, w-18, 26)` | shellcode 行底 504 | 14px 间距（同落在 BS_GROUPBOX 内，groupbox 只画框线，不填内部） |
| Features/ProcessDetail/ProcessDetailPage.Thread.cpp:414 | `(344, 25, w-14, 28)` | 「查看调用栈」按钮右边界 338；filter y 59..85 | 6px 间距；53 < 59 |
| Features/ProcessDetail/ProcessDetailPage.Token.cpp:125 | `(430, 8, w-6, 24)` | 「自动换行」右边界 420 | 10px 间距 |
| Features/ProcessDetail/ProcessDetailPage.Token.cpp:135 | `(6, height-24, w-12, 20)` | `TokenOutput` 底 = height-30 | 6px 间距 |
| Features/ProcessDetail/ProcessDetailPage.Token.cpp:144 | `(130, 10, w-6, 24)` | 三个方按钮结束于 x=120 | 10px 间距 |
| Features/ProcessDetail/ProcessDetailPage.HotkeyKeyboard.cpp:421 | `(6, 42, w-6, 24)` | 工具栏按钮 y 6..36（`kToolbarHeight = 30`）；list y 72 | 6px / 6px 间隙 |
| Features/ProcessDetail/ProcessDetailPage.HotkeyKeyboard.cpp:434 | `(48, 8, w-6, 24)` | 刷新按钮 (6,6,40,36)；内层 tab y 42 | 8px 间距 |
| Features/ProcessDetail/ProcessDetailPage.EvidencePeb.cpp:245 | `(146, 566, w-16, 28)` | 刷新按钮 (20,566,136,594)；输出框 y 602 | 10px 间距 |
| Features/ProcessDetail/ProcessDetailPage.EvidencePeb.cpp:263 | `(214, 8, w-8, 30)` | 「应用修改」右边界 204；group y 44 | 10px 间距 |
| Features/ProcessDetail/ProcessDetailPage.Module.cpp:643 | `(304, 6, w-6, 28)` | 复选框右边界 292；filterBar y 40..66 | 12px 间距 |
| Features/WindowTools/WindowToolsHotkeyView.cpp:353（warningText） | `(6, secondRowY+3, w-12, 24)` = y +3..+27 | `filterBar` 在 `thirdRowY = secondRowY+30` | 3px 间距 |
| Ui/PlaceholderPage.cpp:50-56 | title y+28..58、status y+66..92 | 该窗口无任何子控件 | 无处可遮 |

### 3. 内核页 `Features/Kernel/KernelPage.cpp`（重点核查项，结论：无遮挡）

`statusText_` 共 18 处 `MoveWindow`（:3087 通用布局 1 处 + :3169/:3199/:3246/:3303/:3349/:3396/:3444/:3480/:3538/:3601/:3672/:3706/:3746/:3805/:3839/:3876/:3955 共 17 处分页布局）。逐处计算结果：

- 17 处分页布局里，`statusText_` 的 x 一律等于「同一行最后一个控件右边界 + 4..8px」，y 一律为 `toolbarTop + 4`、高 22 → 占 y 4..26；同一行按钮 y 2..26、筛选框 y 3..25，**x 无交集**，不遮挡。
- 下一行（筛选框那行）起点最早为 `contentTop + 28`（:3480 的 `filterTop = toolbarTop + 28`，:3396 的 `noteTop = toolbarTop + 28`），其余分页为 `contentTop + 28`(infoHeight=28) 或 `contentTop + 96`(infoHeight=96)，**均 > 26**，竖向也不相交。
- :3087 通用布局：`statusText_ = (0, contentTop+infoHeight, width, 20)`；`resultTop = contentTop + infoHeight + 20`，表格紧接着从状态行下方开始；上方 `filterTop = contentTop+72` + 高 22 = 94 ≤ `contentTop+96`。无重叠。
- 特例：:4007 先把 `callbackStatusText_` 放在工具栏行尾，:4010 立刻覆盖为 `(0, panelTop+28, width, 20)`，`mainTop = panelTop+50`，工具栏行 y 0..24 → 4px 间距。
- :4092 `callbackFileMonitorStatusText_` = `(566+, fileTop+4, w-566, 18)`，同行控件累加到 x=562，4px 间距。
- :4072 `callbackBypassStatusText_` 位于 `tabClient.top + Height - 20`（高 18），`callbackBypassList` 底 = `tabClient.top + Height - 22`，2px 间距。

补充（同类但非遮挡）：上述 17 个块里，`filterWidth` 都带最小宽度下限（120/140/160/180/220），当客户区宽度小于「固定控件累计宽度 + 下限 + 6」时，`statusText_` 的 `width` 会算成 `std::max(0, width - toolbarX) = 0`，状态文字整条消失（不是被覆盖，是没有宽度可画）。已算出的确定阈值举例：:3805 块（KernelMemoryEvidence）固定累加到 `toolbarX = 672`，`filterWidth` 下限 160，故 **width ≤ 838 时 `statusText_` 宽度为 0**。

### 4. 其它已看的、确认不涉及状态文字被遮的位置

- Docking/DockManager.cpp:312/838/858 — tab 文字画在 `0..kTabHeight(22)`，页面子窗口从 `rect.top+22` 起，`layoutLeafChildren` :997 保证错开。
- Features/WindowTools/WindowToolsHierarchyView.cpp:822 — 报表页标题 `(6,0,w-12,30)`，`reportEdit` 从 y=30 起，恰好错开。
- Features/SysTools/IoctlDecoderView.cpp:184/192 — 说明文字 y 6..30，`inputEdit` 从 y=30 起；输入框标签 `(6,30,108,54)`，`inputEdit` 从 x=114 起。
- Features/Driver/DriverObjectView.cpp:653 / DriverOverviewView.cpp:704 — 只有 filterBar（y 8..8+filterHeight）与 listView（从 `filterHeight+16` 起），页面内无状态文字。
- Features/Process/ProcessView.cpp — 注释提到 status text，但 `CreateChildControls` 未创建任何状态控件，`WM_PAINT` 也不画状态文字（只在 NM_CUSTOMDRAW 里画列表单元格）。
- Features/AuditCommon/AuditSummaryPanel.cpp、AuditStatus.cpp — 无窗口、无绘制。
