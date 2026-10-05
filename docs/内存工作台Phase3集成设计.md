# 内存工作台 Phase 3 集成设计

输入 `target.md`、`ux.md`；**`backend.md` 缺失**，WP-C 补后端适配层最小设计（未经专项评审，验收加"旧代码逐分支对照"）。D1–D4 与书签/补丁决策不变。路径缩写：S=`shared/evidence/memory_workbench/`，U=`Ksword5.1/Ksword5.1/UI/MemoryWorkbench/`，M=`Ksword5.1/Ksword5.1/MemoryDock/`，T=`KswordARKLightTests/`，X=`tools/memwb_ui/`。

## 0 与三份专项相比的取舍和修正

**接口不一致（已定案）**
1. 导航：`WorkbenchOpenRequest` 并入 `NavRequest`；入口 `openAt(NavRequest)→NavStatus`，Dock 的 `navigateWorkbench` 只建视图/切页签/翻译状态。
2. 选进程：chip 只读，数据取 `WorkbenchTarget::session()`（钉住时 `m_attached*` 会错）；点 chip 展开钉住输入框，"在 Dock 附加…"才发 `pickTargetRequested`。ux §12.2（保护进程）因钉住 pid 走 R0/HVM/DDMA 而消解。
3. 地址统一走 `WorkbenchTarget::evaluate`；不用 HexGotoBar，target 的 `setAddressResolver` 移出本阶段。
4. `ChannelSupportsScope`+`EvaluateChannel` 合并为 Core `MemoryChannelGate`；DDMA 变化拉取（可见时 1 Hz 比 `ddmaSessionGeneration()`），不订阅回调；services 补 `queryAvailability/queryProtection`。
5. 设置键全归 `WorkbenchSettings`；地址簿 store 与 int3 账本为进程级单例（`WorkbenchShared`），取消"内嵌只读"。
6. int3 退出：提示在前（默认"全部还原后继续"，选"保留"留账本），`aboutToDetach` 仅作未经提示路径的安全网；守卫放 `MainWindow::closeEvent` **最前**（其末段停 R0 驱动，晚了还原必失败）。
7. "R0读取此区域"保留（强制 StandardDriver），另增"在工作台查看"（沿用会话通道）。

**遗漏**
8. 后端适配：逻辑下沉 Core、端口注入、真实端口为薄壳，使分块/二分/DDMA 脏扇区/内核事务回滚进 Light。
9. 基线窗口（ux §12.1 留空）：`RefreshBaseline` 在 identity 变时清补丁，故 identity 须与窗口无关，见 §1 基线窗口。
10. 重读须"原位替换页"，不用 `HexCanvas::refresh()`（清缓存闪屏、previous 丢失使青色失效），它只用于换目标。
11. 页读只核对 source（target §2.4 含 content，连续编辑会丢光在途页）；写后对写入范围局部重读（`AcceptWrite` 后画布仍显示页缓存旧值）。
12. 撤销：`MemoryEditHistory` 按等长整快照记账，窗口一动即 SizeMismatch，不能复用；新增 `MemoryEditJournal`（同 32 MiB/256 步）。
13. 地址簿值编辑落在窗口外是异步的，加 PendingStage（票据+2 s 超时）。
14. Tab2 结果表保留，3b 起"加入地址簿"（上限 10000，不自动灌入）；Phase 2 面板并入 3a；旧书签从未持久化，无迁移。

**降级**
15. ux"窗口外暗显"降级为拒绝提示（OutOfWindow），不改画布绘制。
16. 内核 R0 写：旧版失败回滚整批；新实现只保证单次 Write（≤64 B 一片）内回滚，已回读块保留并报 `blocksWritten`→§6-2；固定带 `FORCE|UI_CONFIRMED`（旧行为）。
17. int3 用会话通道，R0 `FORCE_REQUIRED` 不自动强制（失败提示改 R3），DDMA/内核/物理范围禁用；安装通道记在 `Int3Controller` 旁路表。
18. 新名与仓库无重名；target 的 `MemoryAddressResolverTests` 易混 `MemoryAddressExprTests.Resolver`，改 `SessionResolverTests`。

## 1 总体架构与数据流

```
MemoryDock(旧页签/钩子/分发器) → WorkbenchTarget ⇒ Tracker·ModuleDirectory·SessionResolver
MemoryWorkbenchView(会话条|地址条|子页|侧栏|状态条)
 ├ HexPane: HexCanvas ⇄ Inspector/FindBar ◀ overlay（唯一一份）
 ├ PageProvider ─读线程→ PageReader → IMemoryIoPort → IoPorts → MemoryAccessBackend/ArkDriverClient
 ├ WriteController(UI线程) ─ MemoryWriteTransaction → IoByteStore → IMemoryIoPort/内核事务端口
 ├ BaselineFeeder: canvas.copyCachedRange → overlay.RefreshBaseline
 └ WorkbenchShared(单例)
```

**读路径**
- R1 画布 `PlanFetch/MarkInFlight`→`provider.RequestPages(ranges, canvasRev)`；取值快照 `TargetCapture`；Gate 判不可用→`cancelPages`+状态条标红（不标不可读，不换通道）。
- R2 单线程读通道，worker 只持值，`MemoryPageReader`：虚拟 ≤1 MiB、物理 ≤64 KiB（由端口 `Limits()` 给）；整段失败按页二分；`ok&&!partial` 才全有效，部分读按前缀置掩码，零填充/翻译失败=不可读（不变式 5/6）。
- R3 排队回 UI 线程，`isSourceStale` 通过才 `deliverPage/Unreadable`，否则 `cancelPages`；随后 `contentChanged`→Feeder。
- R4 重读（F5/实时/写后）=`requestReload()`+`reread(窗口∪可见页)`，用画布当前代次原位替换；换目标才 `setAddressSpace`。

**写路径**
- W1 一切手势只走 `canvas.stageBytes`→`overlay.Stage`→`editStaged`→`noteContentChanged`、journal 记账（≤1.5 s 合并）；Immediate→`OnEditCompleted()`，Staged→等 Ctrl+Enter。
- W2 提交前：`ConfirmPolicy`→`SetUiConfirmSuppressed`；`target.session()` 拉 DDMA 代次；画布只读，挂起实时刷新。
- W3 `tx.Commit()` 在 UI 线程同步，store 按（范围,通道）派发：VA→`WriteVirtual`；内核+R0→`MemoryKernelMutation`（PREPARE→dry-run→FORCE→回读→片内回滚）；物理→4 KiB 切片无事务；DDMA 持闸。sink 返回前 `observeDdma()`。
- W4 收尾：已写块局部重读；`needsReread`→`requestReload`；`scratchAreaDirty`→常驻红 chip；仅权限类失败弹框；journal 记前后值。

**基线窗口**
1. overlay identity=`IdentityKey(session,0,0)`；窗口=围绕插入点的"已落定页"连续跨度，≤256 页（暂定，待渲染基准）。
2. Feeder 仅在"脏（窗口或 source 变）且无在途请求"时防抖 50 ms `RefreshBaseline`；非脏不重建，否则 SelfWritten 每次回填被清。
3. 重读不重算位置→base/len 不变→previous 保留→青色成立；滚出窗口才重算。查找范围=窗口。

**导航路径**
- N1 来源→Dock 分发器：`jumpToAddress(uint64)` 保名，`routeJumps` 假→`jumpToAddressLegacy`，真→`navigateWorkbench`；模块表用 `jumpToModuleBase`（pid=`m_moduleCachePid`），区域表用 `viewRegionViaDriver`。
- N2 `openAt`：身份变化（范围/pid/通道）合并为**一次**离开守卫：暂存→D1 三选一，int3 未还原→三选一，取消→`LeaveRefused`。
- N3 依次 `requestScope/requestPin|FollowDock/requestChannel`→`onSessionChanged(mask)`：身份类→清 overlay 与 journal、`setAddressSpace`、provider 换代；仅 Reload→软重读。
- N4 地址回车才 `evaluate`；内核半区而范围=进程→`NeedsScopeSwitch`+"切换并跳转"按钮，不静默改范围；同目标内跳转只 `scrollToAddress+setCaretAddress`；无目标→`NeedsAttach`，不排队。

## 2 新增/改动文件清单（≤800 行；行数为 h/cpp 估算）

- **A（S:）** `MemoryTargetTracker` 170/240；`MemoryChannelGate` 90/150；`MemoryWritePolicy` 70/110；`MemoryProcessMatch` 70/130。
- **B（S:）** `MemoryModuleDirectory` 150/240；`SessionAddressResolver` 140/220（含 Issue、ScopeAddressSpace）。
- **C（S:）** `MemoryIoPort.h` 150；`MemoryPageReader` 90/330；`MemoryIoByteStore` 110/320；`MemoryKernelMutation` 90/300；`MemoryPatchByteStore` 60/100。（U:）`WorkbenchIoPorts`（.cpp/.Kernel）90/420/260（真实端口，含 DDMA 闸）。
- **D（S:）** `MemoryBaselineWindow` 80/150；`MemoryEditJournal` 120/280。
- **I（U:）** `WorkbenchTarget`（.cpp/.Anchor/.Modules）170/420/190/260（仅 Anchor 含 Windows.h）；`WorkbenchNavigation.h` 110；`WorkbenchServices.h` 110。
- **G（U:）** `WorkbenchSessionBar` 130/460；`WriteModeSwitch` 80/220；`WorkbenchStatusBar` 130/460；`WorkbenchConfirmations` 100/480；`WorkbenchActions` 100/280；`WorkbenchSettings` 90/220；`WorkbenchMessages` 60/400（i18n 唯一入口）；`WorkbenchStringWriteDialog` 60/230。
- **H（U:）** `WorkbenchDisasmView`（.cpp/.Edit）140/620/460；`WorkbenchTextView` 70/290；`WorkbenchCompareView` 90/430。
- **E（U:）** `AddressBookStore` 100/320；`AddressBookModel` 120/480；`AddressBookPanel`（.cpp/.Menu）150/620/380。
- **F（U:）** `Int3Controller` 100/380；`Int3PatchPanel` 100/480。
- **J（U:）** `WorkbenchShared` 70/160；`WorkbenchHexPane`（.cpp/.Panels）130/450/350；`WorkbenchWriteController`（.cpp/.Undo）140/640/360；`WorkbenchPageProvider` 100/360；`WorkbenchBaselineFeeder` 90/330；`MemoryWorkbenchView`（.cpp/.Session/.Nav）260/620/600/560。
- **K（M:）** `MemoryDock.Workbench` 620；`.WorkbenchServices` 280；`.WorkbenchNavMenus` 200（可选）。
- **测试** T:12 个套件+支持头（≤600）；X:`memwb_ui_tests.Workbench.*` 10 文件（≤700）。共 110 新文件、约 3.1 万行。

**改动现有文件（函数级）**
- `MemoryDock.h` +≈35 行，新增文字不得含 `test_memory_bookmarks.py` 正则命中的标识符（`[^;]*;` 会吞多行）；构造函数在任何附加前创建 `WorkbenchTarget`。
- `UiWireAndStatus`：刷新钮 lambda 十个分支改按控件路由（WP-0，先于插页签）；模块表双击×2→`jumpToModuleBase`；"R0读取此区域"→`viewRegionViaDriver`；搜索结果双击/右键×2 改读 `item(row,0)` 的 UserRole。
- `UiBuild`：图标循环**之后** `insertTab(3,…)`。`ProcessRegion`：attach 末/detach 首/detach 末三钩子；模块缓存提交处写 `m_moduleCachePid`（两处清零）；`setProcessDetailMemoryScope` 可见集。
- `ViewBreakpointUtil`：`jumpToAddress` 体原样改名 `jumpToAddressLegacy`，旧地址框改调它；`confirmDiscardMemoryEditsForProcessChange` 末加 `requestLeave`。
- `ProcessPteTranslate` 默认地址取 `focusAddress()`；`SearchFlow`（3b）"加入地址簿"；`MainWindow::closeEvent` 最前加 `confirmQuit`；`HexCanvas` +`copyCachedRange`；`HexViewWidgets` +`setSegmentEnabled`。
- qrc +11 别名；vcxproj/filters ≈95 项；语言包 ≈380 条（定点插入，en 禁汉字）；Light 四处登记；组件清单文档。

## 3 线程模型与不变式落实

**同步 Commit 能衔接，条件五条**
1. `MemoryWriteTransaction` 持 overlay 引用且 `AcceptWrite` 与画布共享，Commit 必须在 UI 线程；Core 无进度/已接受块出口，拆 worker 须改 Core，3a 不做（逃生口：overlay 快照+回放 AcceptWrite，实测冻结不可接受时另立项）。
2. 端口线程安全（每次按 PID 开句柄，不共享 Dock 句柄），DDMA 读写共用一把闸。
3. Commit 内不泵事件，只有两个模态确认；sink 返回前 `observeDdma()`，Core (d) 步才能复核到真实代次。
4. 提交期间挂起实时刷新、画布只读；否则 1 s 重读使超过 1 s 的确认框全部 Stale。
5. 读线程=1 条串行池，只持值；DDMA 读返回 `scratchDirty`→本代次停 DDMA 读并报红。

代价：DDMA/大批暂存提交时 UI 无响应（同旧 Tab5，无回归），确认框写明块数与字节数。

**不变式（1–16）负责方**
1 画布/Feeder/Provider 零写，写仅 tx 与 int3 账本；2 基线窗口 identity 规则；3 Core 事务+端口；4 Gate 无自动项，DDMA 不可用不弹回 R0；5/6 `MemoryPageReader` 掩码+状态条"已读 M/N"；7 `MemoryKernelMutation`；8 `evaluate` 唯一入口；9 `MemoryEditJournal`；10 旧汇编核心原样；11 tx (d) 步+`observeDdma`+PendingStage 票据；12 `ChangeKind`+保 previous；13 source 判据+各票据；14 红 chip+DDMA 读故障闩锁；15 Policy 只置 `SetUiConfirmSuppressed`，`ConfirmApproval` 永远弹；16 读线程只持值，端口按 PID 开句柄。

## 4 分阶段落地

纪律：main 上显式路径提交，不建分支不 stash；共享文件（vcxproj/filters、语言包、qrc、`MemoryDock.h`、Light 四处）仅 WP-Z/K 改；3a 开工前 Phase 1b-ii 须已提交；每波末至多一次主程序构建。

**3a 并存**（`routeJumps=false`，旧跳转全走旧页，内嵌窗口不变）。验证：主程序构建 SUCCESS；旧功能同旧；`test_memory_bookmarks.py` 全过；手测 ux §7 八任务；Light/夹具全绿。回退：`enabled=false`（无需发版）；WP-K、WP-0 各为独立提交可 revert。

**3b 入口切换**：`routeJumps` 默认真，旧页签标"（旧）"并后移，内嵌窗口换工作台（禁内核/物理），证据页右键"在内存工作台打开"，Tab2"加入地址簿"。验证：8 个跳转点与 `focusMemoryDockByPid/DdmaPage` 反射调用（名字不动）逐一走通；预览 B 再双击其模块应钉住 B；排序后双击搜索结果取对地址；内嵌窗口关闭时 int3 已还原。回退：关 `routeJumps` 勾选。

**3c 删除**：前置——≥2 个发行包 `routeJumps` 默认真无回归单、Tab3+Tab5 功能对照清单全绿、grep 无旧成员残留。删三旧页签及成员、`DriverMemoryRw`（1866→≈200）、`ViewBreakpointUtil`（1339→≈300）、刷新钮三旧分支；`test_memory_bookmarks.py` 随被抠函数退役（提交说明附断言对照表）。验证：Release 构建单独一次（`MemoryDock.h` 扇出重编译）、grep 门禁、i18n audit。回退：revert 该提交。

**3a 工作包**（波 1 并行：0、A、B、C、D；波 2 并行：E、F、G、H、I；波 3：J；波 4：K；波 1 首日先冻结 A–D、I 的头文件；Z 每波末串行汇合）

| WP | 依赖 | 验收判据 |
|---|---|---|
| 0 刷新钮路由、`setSegmentEnabled`、`copyCachedRange`、qrc | — | 构建 SUCCESS；十页刷新手测同旧；lambda 内 `tabIndex ==` 为 0；夹具：禁用段不可点，`copyCachedRange` 与 `cellStateAt` 逐字节一致 |
| A、B | — | `/W4 /WX` 零警告；tracker 随机游走 1 万步（身份变⇒source 变）；Gate 3×4 格；预览进程 B 的目录对会话 A 恒 OwnerMismatch；Ddma/Physical 下 ReadPointer 0 次；独立验证者注入变异全抓 |
| C | — | 假端口故障注入：二分、部分读掩码、零填充=不可读、DDMA 脏扇区停读、内核事务五失败点各一例（`rolledBack/bytesWritten` 逐例断言）；旧 `driverApplyMemoryDiffFromUi` 五分支对照表；主程序构建；真机 R3/R0 进程+内核读写各一轮 |
| D | — | 窗口：含插入点、≤上限、页对齐、仅已落定页、同输入同输出；journal：32 MiB/256 步边界、整步拒绝、切 redo、合并；变异全抓 |
| E 地址簿 | 0 | 夹具：排序后按 id 删除/跳转/编辑落对条目；序列化往返；坏文件报"第 N 行"且不覆盖；不可见时读取 0 次 |
| H 子页 | 0 | 夹具：单击不进编辑，双击/F2/Enter 进；数据取 `Materialize`；汇编预览沿用覆盖长度校验；对比页对象数 O(可见行) |
| F int3 | C | 夹具：安装/还原确认计数=0；Diverged 仅可丢弃；还原通道=安装通道；`FORCE_REQUIRED` 不自动强制；退出三选一 |
| G 会话条等 | A、0 | 夹具：不可用段置灰且 tooltip 为原因、不自动换通道；模式切换三选一四分支；"本次其余"仅 `blocksTotal−blockIndex>1` 出现；脏扇区红 chip 点 × 才消；en 无汉字 |
| I Target | A、B | 夹具（假服务）：`aboutToDetach` 同步且句柄有效；守卫否决不调 tracker；DDMA 代次拉取；枚举票据丢弃；PID 复用探针只记读数 |
| J 装配 | C–I、0 | 夹具全链路（假端口）：换目标丢旧结果；Immediate/Staged 全状态，失败保留橙色；确认框期间实时刷新挂起不 Stale；软重读青色成立；写后页缓存刷新；窗口跟随不丢补丁；PendingStage 超时拒绝；撤销补写旧值，目标被改整步拒绝 |
| K Dock 接线 | I、J | 主程序构建；`routeJumps=false` 逐项同旧；`test_memory_bookmarks.py` 全过；三钩子在附加/分离/内嵌关闭各触发一次；一致性校验告警 0 |
| Z 登记 | 每波末 | `Test-KswordSuiteManifest.ps1` 与 i18n audit PASS；vcxproj/filters 与目录比对一致 |

## 5 测试计划

- **Light 新增 12 套件**（前缀 `MEMWB `）：target tracker、channel gate、write policy、process match、module dir、session resolver、page reader、io bytestore、kernel mutation、patch store、baseline window、edit journal；假端口（脚本化结果、延迟、故障）在 `MemoryIoTestSupport.h`。验证：MSVC `/W4 /WX`+同进程 g++ 链接全部套件；变异由不写实现的一方注入。
- **夹具新增**（不在 CI，每个 WP 贴 `memwb_ui_tests: N checks`）：FakeIoPort/FakeServices（Target/Provider/WriteController/Feeder/Nav/AddressBook/Int3/截图）。
- **只能靠主程序构建+UI 回归**：Dock 钩子在真进程的附加/分离；真实 R0/HVM/DDMA；`closeEvent` 与停驱顺序；真窗口里模态框/QMenu 不透明；内嵌窗口；PID 复用探针。手工冒烟=ux §7 八任务+target §9 五项。
- **Python 抠函数**：仅 `test_memory_bookmarks.py` 取 MemoryDock 源码；3a/3b 不改其抠取的函数、两个 lambda 与正则命中的头文件标识符，3c 随函数退役；其余 Python 与旧历史/汇编脚本不动。

## 5.5 WP-C 已核实的后端事实（2026-10-03，读门面/驱动/协议源码所得；`backend.md` 专项缺失的补充）

1. 单次上限：虚拟读 1 MiB、虚拟写 256 KiB、物理读 64 KiB、物理写 4 KiB；私有页表窗口通道门面内部按 1024 字节切片；磁盘传输通道按 4 KiB 页切片；分步字节事务每片 ≤64 字节。
2. **读语义（V-06 根因）**：用户态、私有页表窗口、磁盘传输、物理读（标准驱动 flags=0）经门面读取都是诚实的"前缀语义"；唯一例外是标准驱动**虚拟读**——门面固定带 `ZERO_FILL_UNREADABLE`，驱动此时把 `bytesRead` 设成满长度、不可读部分补零、状态写 PARTIAL_COPY/ZERO_FILLED，且不返回哪些页真读到了（`process_memory.c` 约 840-915 行），门面再把 `bytesDone` 设为满长度，旧查看器因此把未读字节当真实 `00`。真实端口对该通道必须直接调 `DriverClient::readVirtualMemory`、**不带补零标志**（内核地址只带 `KERNEL_ADDRESS`）：`OK`→全部；`PARTIAL_COPY` 且 `bytesRead>0`→前缀；`COPY_FAILED`→目标不可读；`io.ok==false`→通道失败。
3. 用户态读：门面把"打开进程失败"与"读到 0 字节"报成同一种失败文本，端口自己实现以区分 `Failed` 与 `Unreadable`。读结果四态：`Ok / Partial(前缀) / Unreadable(目标页读不到→画布 ??) / Failed(通道自身失败→取消请求、状态条报红、可重试，不标不可读)`。
4. 写：`forceRequired`→`needsApproval`（调用方取得同意后重试同一次写入，重写是幂等的）；标准驱动+内核虚拟地址的写走分步字节事务（每片 PREPARE(DRY_RUN|EXPECTED_BEFORE)→校验→DRY_RUN 提交→`FORCE|UI_CONFIRMED` 提交→回读→失败时**先回读、已等于写前字节就不调用回滚**，否则回滚后再回读核对）；用户决策：只保证单次 Write 内回滚。
5. 磁盘传输通道的门面没有互斥、旧代码在 UI 线程同步执行；真实端口加一把进程级互斥闸（注意旧页签/DDMA 页仍不经这把闸，3a 并存期间有残余并发风险）。

## 6 用户决策记录（2026-10-03，全部已拍板）

1. **确认策略 = 采纳 ux 的表**：进程范围+立即写不弹确认；内核/物理/磁盘传输通道+立即写，每个"范围+通道"首次确认一次；暂存→应用每次确认；驱动要求的强制同意永远逐块询问。
2. **内核 R0 写回滚 = 接受单次 Write 内回滚**：已回读确认的块保留，如实报 `blocksWritten`，未写的补丁继续留在暂存里可重试；不实现旧版"整批回滚"。
3. **立即模式 Ctrl+Z = 向目标补写旧值**：走同一确认/审计/回读链路，目标字节已被别处改过则整步拒绝，状态条注明这是一次新的写入。
4. **推进策略 = 按设计全量推进**（用户选择，未采纳"先竖切"的建议）：13 个工作包、4 波，每波末串行汇合（登记/验证），每波至多一次主程序构建；为控制全量推进的集成风险，每个工作包都必须有独立验证者注入变异、旧代码逐分支对照，且 3a 全程保持 `routeJumps=false`（旧页签行为不变）。
