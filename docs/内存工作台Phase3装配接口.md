# 内存工作台 Phase 3 装配接口（WP-J 接口冻结）

本文档只覆盖装配层（设计文档 WP-J）六个类的接口契约，供六个类的并行实现者与审核者使用。
六个头文件均已发布在 `Ksword5.1/Ksword5.1/UI/MemoryWorkbench/`：`WorkbenchShared.h`、
`WorkbenchPageProvider.h`、`WorkbenchBaselineFeeder.h`、`WorkbenchWriteController.h`
（+ 卫星头 `WorkbenchWriteController.Undo.h`，定义实现细节类 `WorkbenchUndoCoordinator`，
不是第七个装配层类）、`WorkbenchHexPane.h`、`MemoryWorkbenchView.h`。本文档只做说明与
归属，**不重复抄写头文件里已经写清楚的参数级注释**，只在表格里给出"哪个类的哪个方法"。

全部依赖的既有组件接口均以磁盘为准核实（WP-A/B/C/D/E/F/G/H/I 各包头文件、Core 层
`shared/evidence/memory_workbench/*.h`），没有凭记忆杜撰签名；真实接口与 `target.md`/
`ux.md` 字面描述不符处，已按任务规则"以已有组件真实接口为准"处理，记在 §8。

## 1 依赖图与创建/销毁顺序

```
WorkbenchShared（进程级单例，惰性创建，Configure 一次）
  ├─ AddressBookStore / AddressBookModel（共享）
  └─ Int3Controller（共享）

每个 MemoryWorkbenchView 实例（主 Dock 一个 + 每个内嵌 ProcessDetailWindow 一个）：
  MemoryWorkbenchView
   ├─ WorkbenchTarget（私有，由 WorkbenchShared::CreateServices() 产出的
   │    IWorkbenchServices 构造）
   ├─ WorkbenchConfirmations（私有，持有 WorkbenchShared::WritePolicy() 的引用）
   ├─ WorkbenchPageProvider（私有，引用同一个 WorkbenchTarget）
   ├─ WorkbenchBaselineFeeder（私有）
   ├─ WorkbenchWriteController（+ WorkbenchUndoCoordinator，私有，引用同一个
   │    WorkbenchTarget 与 WorkbenchConfirmations）
   ├─ WorkbenchHexPane（持有 MemoryDiffOverlay 唯一一份；上面三者通过
   │    setCanvas/setOverlay/setPageProvider/setBaselineFeeder/setWriteController
   │    注入其中，互为非拥有指针）
   ├─ WorkbenchDisasmView / WorkbenchTextView / WorkbenchCompareView（各自绑定一个
   │    IWorkbenchBytesProvider 适配器，包装 WorkbenchHexPane::overlay()）
   └─ WorkbenchSessionBar / 地址条 / AddressBookPanel(绑定共享模型) /
        Int3PatchPanel(绑定共享控制器) / WorkbenchStatusBar
```

创建顺序（构造函数体内，见 `MemoryWorkbenchView.h` 文件头）：
`WorkbenchTarget → WorkbenchConfirmations → WorkbenchPageProvider →
WorkbenchBaselineFeeder → WorkbenchWriteController → WorkbenchHexPane
（随后注入前四者）→ 三个只读子页 → 会话条/地址条/侧栏/状态条 → wireActions`。

销毁安全性不是靠成员声明顺序，而是 C++ 的通用规则：`MemoryWorkbenchView` 自己的
`unique_ptr` 成员（`pageProvider_`/`baselineFeeder_`/`writeController_` 等）在
`~MemoryWorkbenchView()` 函数体执行完之后、**基类 `QWidget`/`QObject` 析构之前**按
声明逆序销毁；而 `hexPane_` 这类 Qt 子控件由父子关系在基类析构阶段统一删除，天然晚于
前者。因此三条管线对象析构时，它们持有的 `WorkbenchHexPane::overlay()` 指针仍然有效，
这个安全性与这几个 `unique_ptr` 成员彼此的声明顺序无关。`WorkbenchShared` 的销毁顺序
见 `WorkbenchShared.h` 文件头："地址簿模型先于地址簿存储析构"是声明顺序的体现（二者
用 `QPointer` 弱引用，顺序本身非安全必需，只是让中间态更短）。

## 2 数据流 → 类.方法 映射

### 读路径 R1-R4

| 步骤 | 类.方法 |
|---|---|
| R1 画布请求页、Gate 判定 | `WorkbenchPageProvider::RequestPages`（内部用 `setGateInputsProvider` 注入的回调 + `ksword::memwb::EvaluateChannel`） |
| R1 取值快照 | `WorkbenchTarget::capture()`（由 `WorkbenchPageProvider` 调用） |
| R2 单线程读通道 | `WorkbenchPageProvider` 内部 `ReadPool`（唯一单线程池）调用 `ksword::memwb::ReadPages` |
| R2 DDMA 脏扇区闩锁 | `WorkbenchPageProvider::resetScratchLatch` / 内部 `scratchLatched_` |
| R3 陈旧性核对与回填 | `WorkbenchPageProvider::isSourceFresh` → `HexCanvas::deliverPage/deliverUnreadable/cancelPages` |
| R3 contentChanged 驱动 | `WorkbenchHexPane::onCanvasContentChanged`（转发给 `WriteController::notifyWindowMayCover`） |
| R4 用户重读/实时刷新 | `WorkbenchTarget::requestReload()` + `WorkbenchPageProvider::retryFailedRanges`（画布自身重新 `RequestPages` 可见页，走正常 R1-R3） |
| R4 写后局部重读 | `WorkbenchWriteController` 的 `RereadRangeFn` 回调（由 `MemoryWorkbenchView` 接到 `WorkbenchPageProvider::retryFailedRanges` 或等价定向请求） |

### 写路径 W1-W4

| 步骤 | 类.方法 |
|---|---|
| W1 编辑手势 → 暂存 | `HexCanvas::stageBytes`（既有组件）→ `WorkbenchWriteController::onEditCompleted` |
| W2 确认策略 | `MemoryWorkbenchView` 调用 `WorkbenchWriteController::setUiConfirmSuppressed`（取自 `MemoryWritePolicy::Decide`） |
| W2 画布只读/挂起刷新 | `WorkbenchWriteController::setCanvasReadOnlyHook` 回调（装配层接到 `WorkbenchHexPane::setEditable`） |
| W3 同步提交管线 | `WorkbenchWriteController::commitPendingNow` / `onEditCompleted` 内部的 `ksword::memwb::MemoryWriteTransaction::Commit` |
| W3 内核分步事务 | `WorkbenchWriteController::setKernelMutationPortFactory` 注入的 `IKernelMutationPort` |
| W4 后续动作（重读/红 chip/journal） | `WorkbenchWriteController` 的 `commitFinished`/`commitFailed`/`scratchAreaDirtyReported` 信号，由 `MemoryWorkbenchView` 转发到状态条与 `WorkbenchUndoCoordinator::record` |
| 地址簿值编辑（窗口外，异步） | `WorkbenchWriteController::beginPendingStage` / `notifyWindowMayCover` / `pendingStageResolved` |
| 撤销/重做 | `WorkbenchWriteController::undo/redo` → `WorkbenchUndoCoordinator::undo/redo`（见 §7 的 scratch overlay 方案） |

### 基线窗口

| 步骤 | 类.方法 |
|---|---|
| 换目标/换范围清补丁、换身份串 | `WorkbenchHexPane::setAddressSpace`（内部 `overlay().LoadBaseline`）+ `WorkbenchBaselineFeeder::setIdentityKey` |
| 脏且无在途请求才重算 | `WorkbenchBaselineFeeder::noteDirty`（`hasInFlightRequests` 参数） |
| 选取/重喂 | `WorkbenchBaselineFeeder::recomputeAndFeed`（内部 `ksword::memwb::SelectBaselineSpan`/`DecideBaselineRefeed`） |
| 喂入成功通知画布 | `WorkbenchBaselineFeeder::baselineRefreshed` → `HexCanvas::notifyOverlayChanged`（`WorkbenchHexPane::onBaselineRefreshed`） |

### 导航 N1-N4

| 步骤 | 类.方法 |
|---|---|
| N1 后退/前进栈 | `MemoryWorkbenchView` 的 `backStack_`/`forwardStack_`（容量 64，`openAt` 内维护） |
| N2 身份变化一次性离开守卫 | `MemoryWorkbenchView::openAt` 内调用 `WorkbenchTarget::requestIdentity`（一次合并）；离开守卫组合见 §5 不变式 2/11 |
| N3 身份类 vs 仅 Reload 的两条分支 | `MemoryWorkbenchView::onTargetSessionChanged` → `handleIdentityChange`/`handleReloadOnly` |
| N4 地址求值与范围建议 | `WorkbenchTarget::evaluate`（既有组件）→ `MemoryWorkbenchView::openAt` 按 `AddressEval::suggestScope` 返回 `NavStatus::NeedsScopeSwitch`，不静默切换 |

## 3 信号/槛接线总表

| 发送方.信号 | 接收方.槛 | 连接类型 | 理由 |
|---|---|---|---|
| `HexCanvas::visibleRangeChanged` | `WorkbenchHexPane::onCanvasVisibleRangeChanged` | Direct | 画布与 HexPane 同在 UI 线程，无需排队 |
| `HexCanvas::caretMoved` | `WorkbenchHexPane::onCanvasCaretMoved` | Direct | 同上 |
| `HexCanvas::editStaged` | `WorkbenchWriteController::onEditCompleted` | Direct | 必须在同一调用栈内决定是否立即 Commit，Queued 会让"立即写入"变成下一轮事件循环才生效 |
| `WorkbenchPageProvider` 内部读线程结果 | `WorkbenchPageProvider::onJobFinishedOnUiThread` | **Queued** | 跨线程排队回 UI 线程（R3 不变式 5：读线程只持值） |
| `WorkbenchBaselineFeeder::baselineRefreshed` | `WorkbenchHexPane::onBaselineRefreshed` | Direct | 同线程；随后立即调用 `canvas_->notifyOverlayChanged()` 避免多帧闪烁 |
| `WorkbenchWriteController::commitFinished` | `MemoryWorkbenchView::onWriteControllerCommitFinished` | Direct | 刷新状态条属于同步 UI 更新 |
| `WorkbenchWriteController::scratchAreaDirtyReported` | `WorkbenchStatusBar::reportScratchAreaDirty(true)` | Direct | 常驻红 chip，必须同步出现 |
| `WorkbenchWriteController::pendingStageResolved` | 地址簿编辑器/`AddressBookPanel` 的提示槛（由 `MemoryWorkbenchView` 中转） | Direct | 2 秒超时本身已经是异步的，结果到达后同步处理即可 |
| `WorkbenchTarget::sessionChanged` | `MemoryWorkbenchView::onTargetSessionChanged` | Direct | 身份变化必须在同一调用栈内完成清理，否则画布会短暂显示旧目标的字节 |
| `WorkbenchTarget::aboutToDetach` | `MemoryWorkbenchView::onTargetAboutToDetach` | **Direct**（`WorkbenchTarget.h` 原文强制要求：发出时 Dock 句柄仍有效） | int3 未经提示路径的安全网必须在句柄关闭前跑完 |
| `WorkbenchTarget::modulesFailed` | 地址条"刷新模块"图标的失败态（由 `MemoryWorkbenchView` 中转） | Direct | 熄灭"加载中"指示灯需要立即生效 |
| `WorkbenchSessionBar::scopeRequested/channelRequested` | `MemoryWorkbenchView` 的处理函数，内部调用 `target().requestIdentity` 后再调用 `sessionBar_->setSession(...)` 回写 | Direct | 必须在同一调用栈内完成"请求→裁决→回写"，避免 S4/S5 的显示与实际分叉 |
| `WorkbenchSessionBar::pickTargetRequested` | `MemoryWorkbenchView::onSessionBarPickTargetRequested` → 转发信号给 Dock | Direct | Dock 的进程下拉框在 UI 线程 |
| `AddressBookPanel::valueEditRequested` | `MemoryWorkbenchView` 的处理函数，调用 `WorkbenchWriteController::beginPendingStage` | Direct | 票据创建本身是同步操作，真正的异步在票据内部 |
| `Int3PatchPanel::resultMessage` | `WorkbenchStatusBar::setWriteResultText`（经 `MemoryWorkbenchView` 中转） | Direct | — |

## 4 线程模型

| 类 | UI 线程方法 | 非 UI 线程部分 |
|---|---|---|
| `WorkbenchShared` | 全部公开方法 | 无（单例本身无工作线程） |
| `WorkbenchPageProvider` | `RequestPages`/`cancelAllInFlight`/`resetScratchLatch`/`retryFailedRanges`/两个 setter | 内部 `ReadPool`：**只持值**——工作线程里出现的类型只能是 `MemoryTargetSession`（值拷贝）、地址/长度整数、`PageReadResult`；绝不能出现 `HexCanvas*`、`QWidget*` 或任何 `QObject` 指针 |
| `WorkbenchBaselineFeeder` | 全部公开方法（含 `QTimer` 回调，Qt 定时器本身在 UI 线程触发） | 无 |
| `WorkbenchWriteController`（+ Undo） | 全部公开方法，`Commit()` 本身同步执行、不派生线程 | 无（不变式 1 的"同步 Commit 能衔接"要求 Commit 不能跨线程） |
| `WorkbenchHexPane` | 全部公开方法 | 无（画布异步页回填本身已经由 `WorkbenchPageProvider` 处理成"排队回 UI 线程"） |
| `MemoryWorkbenchView` | 全部公开方法 | 无；`WorkbenchTarget` 内部的模块枚举（既有组件）在 `QThreadPool` 工作线程执行，但那是 `WorkbenchTarget` 自己的职责，不属于本文档六个类 |

只持值清单（唯一允许出现在工作线程里的类型）：`ksword::memwb::MemoryTargetSession`、
`ksword::memwb::RevisionSnapshot`、`ksword::memwb::HexViewport::FetchRange`、
`ksword::memwb::PageReadResult`、`std::uint64_t`/`std::uint8_t` 等基本类型、
`std::vector<std::uint8_t>`。

## 5 不变式 1-16 在装配层的落实点

| # | 落实点 |
|---|---|
| 1 画布/Feeder/Provider 零写 | `WorkbenchPageProvider` 只调用 `deliverPage/deliverUnreadable/cancelPages`；`WorkbenchBaselineFeeder` 只调用 `RefreshBaseline`；写只经 `WorkbenchWriteController::transaction_` 与 `WorkbenchUndoCoordinator` 的 scratch 事务，以及 `Int3Controller`（既有组件） |
| 2 基线窗口 identity 规则 | `WorkbenchBaselineFeeder::setIdentityKey` + `WorkbenchHexPane::setAddressSpace` |
| 3 Core 事务+端口 | `WorkbenchWriteController` 持有 `MemoryWriteTransaction`+`MemoryIoByteStore`+两个端口工厂 |
| 4 Gate 无自动项 | `WorkbenchPageProvider::RequestPages` 对 Gate 不可用只 `cancelPages`，不换通道、不重试 |
| 5/6 掩码+状态条"已读 M/N" | `WorkbenchPageProvider` 产出 `PageReadResult` → `MemoryWorkbenchView` 译成 `WorkbenchStatusBar::setReadResultText` |
| 7 `MemoryKernelMutation` | `WorkbenchWriteController::setKernelMutationPortFactory` |
| 8 `evaluate` 唯一入口 | `MemoryWorkbenchView::openAt` 调 `target().evaluate`，其余子页/地址条不自行调用 `EvaluateAddressExpr` |
| 9 `MemoryEditJournal` | `WorkbenchUndoCoordinator`（journal 唯一持有者，`WorkbenchWriteController` 不另存一份） |
| 10 旧汇编核心原样 | `MemoryWorkbenchView::setDisasmBackends` 只是注入点，不重写解码/汇编逻辑 |
| 11 tx (d) 步+`observeDdma`+PendingStage 票据 | `WorkbenchTarget::session()` 的 DDMA 拉取（既有组件）+ `WorkbenchWriteController::beginPendingStage` 的 2s 票据 |
| 12 `ChangeKind`+保 previous | `WorkbenchHexPane::setAddressSpace`（`LoadBaseline`，清 previous）vs `WorkbenchBaselineFeeder` 的 `RefreshSameSpan`（保 previous） |
| 13 source 判据+各票据 | `WorkbenchPageProvider::isSourceFresh`；`WorkbenchWriteController::PendingStageTicket` |
| 14 红 chip+DDMA 读故障闩锁 | `WorkbenchPageProvider` 的 `scratchLatched_` + `MemoryWorkbenchView` 转发到 `WorkbenchStatusBar::reportScratchAreaDirty` |
| 15 Policy 只置 suppressed，`ConfirmApproval` 永远弹 | `MemoryWorkbenchView` 只调用 `WorkbenchWriteController::setUiConfirmSuppressed`，从不碰 `ConfirmApproval` 路径（那条路径固定在 `WorkbenchConfirmations` 内，不受抑制开关影响） |
| 16 读线程只持值，端口按 PID 开句柄 | `WorkbenchPageProvider` 的 `ReadPool`（见 §4）；"按 PID 开句柄"是 `WorkbenchIoPort`（既有组件）自己的约定，本文档六个类不重复实现 |

## 6 Wave 2 遗留"装配层接线"清单（逐条归属）

| 项（来源） | 归属类.方法 | 说明 |
|---|---|---|
| `SetTargetDescription`（review-wpG B2） | `MemoryWorkbenchView::handleIdentityChange`/`onTargetSessionChanged` | 不需要给 `WorkbenchSessionBar` 新增 getter：`MemoryWorkbenchView` 用喂给 `sessionBar_->setTargetInfo(...)` 的同一组参数，调用既有的 `WorkbenchMessages::TargetChipAttachedText`/`TargetChipNoProcessText` 独立拼出描述文本，再调 `confirmations_->SetTargetDescription(...)`，两处共用数据源但不共用控件状态 |
| `setSession`（review-wpG S5） | `MemoryWorkbenchView` 的 `scopeRequested`/`channelRequested` 处理函数 | 范围+通道同时确认时必须调用 `WorkbenchSessionBar::setSession(scope, channel, rememberAsUserChoice)` 这一个入口，不得分别调用 `setScope` 再 `setChannel`（否则 `setChannel` 会用"旧范围"为键污染记忆，见 S5 原始实测） |
| `rememberedChannel`/`restoreChannelMemory`（fix-wpG B5） | `MemoryWorkbenchView::loadSettings`/`saveSettings` | 启动时对进程/内核/物理三个范围各调一次 `restoreChannelMemory`；退出前各调一次 `rememberedChannel` 写回 `workbench_settings::SaveChannelForScope`。哪个视图是"权威"见 §8 决策点 1 |
| `CreateWorkbenchAlternateShortcut`（Redo 候补键，fix-wpG）| `MemoryWorkbenchView::wireActions` | 对 `WorkbenchActionId::Redo` 同时调用 `CreateWorkbenchShortcut` 与 `CreateWorkbenchAlternateShortcut`，两个 `activated()` 接到同一个槛（`writeController_->redo()`） |
| F5 重复来源（S3，fix-wpG 判定"接线方式问题"）| `MemoryWorkbenchView::wireActions` | `Reread` 快捷键与"重读"工具按钮的 `clicked()` 必须接到**同一个**槛函数，不能各自实现一遍 |
| 诊断抽屉 `IWorkbenchDiagnosticsHost`（`WorkbenchStatusBar.h` 顶部说明）| `MemoryWorkbenchView::buildUi` | 需要一个包着 `CodeEditorWidget` 的生产实现类（**尚不存在，见 §8.2 缺口 G3**），由 `buildUi` 构造并传给 `WorkbenchStatusBar` 的构造参数 |
| `DecodeOneFn`/`AssembleOneFn` 生产胶水 | `MemoryDock`（WP-K，调用 `MemoryWorkbenchView::setDisasmBackends`） | 本文档六个类不直接依赖 `InstructionDecoder`/`InstructionAssembler`，注入点已冻结在 `MemoryWorkbenchView.h` |
| `IWorkbenchBytesProvider` 的 `Materialize` 适配 | `MemoryWorkbenchView` 私有嵌套类 `DisasmBytesProviderAdapter`（已在头文件声明） | `FetchWindow` 内部调用 `hexPane_->overlay()` 的 `Materialize`/`BaselineByte`/`PreviousByte`/`ChangeKind` 组装 `WorkbenchByteWindow` |
| 地址簿去重 | 新动作处理函数（建议命名 `MemoryWorkbenchView::addInsertionPointToAddressBook`，Ctrl+B 与画布右键共用） | fix-wpE.md 明确"去重契约留给装配层"；建议规则见 §8.2 缺口 G4（不是强制规格，供实现者参考） |
| int3 退出守卫 `CountForCurrentTarget`/`HasUnrestoredForCurrentTarget` | `MemoryWorkbenchView::installLeaveGuard` | 不直接调用 `CountForCurrentTarget`（它只用于 `Int3Controller::RequestLeave` 内部拼文案）；装配层只需要把 `ks::ui::LeaveReason` 映射成 `Int3LeaveScenario` 后调用一次 `RequestLeave`，见下一行 |
| `requestLeave` 守卫组合 | `MemoryWorkbenchView::installLeaveGuard` 注册给 `target().setLeaveGuard(...)` 的 lambda | 依次调用①`writeController_` 是否有未提交暂存（`PendingOverlayGuard` 语义）②`WorkbenchShared::Instance().Int3().RequestLeave(this, 映射后的 scenario)`；两者都放行才返回 true。主窗口关闭的场景不经过这条 `LeaveReason` 路径，由 `MainWindow::closeEvent` 最前直接调用 `RequestLeave(..., MainWindowClose)`（设计文档 §0 第 6 条） |
| `modulesFailed` 信号 | `MemoryWorkbenchView` 构造时连接 `target_->modulesFailed` | 熄灭地址条"刷新模块"图标的加载指示灯并显示失败提示（该信号已在 `WorkbenchTarget.h` 定义，WP-I 修复报告确认已补上，装配层只需要连接） |

## 7 分工建议、实现顺序与夹具关键判断

**依赖分析**：`WorkbenchWriteController` 的公开接口只需要一个 `MemoryDiffOverlay*`
（任意实例，不必是 `WorkbenchHexPane` 持有的那一份）与 `WorkbenchTarget*`，因此可以在
夹具里用独立构造的 `MemoryDiffOverlay` 值对象单独测试，不必等 `WorkbenchHexPane` 落地。
`WorkbenchHexPane` 的三个 `set*` 接线点接受的是接口类型的指针，真正验证"接线生效"才需要
三条管线的真实实现，但 `WorkbenchHexPane` 自身的布局/overlay 持有权/画布信号转发可以在
三者留空时独立测试。

**建议实现顺序**：
- **Wave 1（可完全并行）**：`WorkbenchShared`、`WorkbenchPageProvider`、
  `WorkbenchBaselineFeeder`、`WorkbenchWriteController`（含 `.Undo`）。四者互不依赖，
  各自用假端口/假服务/独立 `MemoryDiffOverlay` 夹具验证。
- **Wave 2**：`WorkbenchHexPane`（集成 Wave 1 的三条管线真实实现 + 既有的
  `HexCanvas`/`HexInspectorPanel`/`HexFindBar`）。
- **Wave 3**：`MemoryWorkbenchView`（集成 Wave 2 + 既有的 WP-E/F/G/H 组件 + `WorkbenchShared`）。

**各类夹具关键判断**：

- `WorkbenchShared`：`Configure()` 只生效一次（重复调用返回 `false` 且不改已持有对象）；
  `Shutdown()` 可重复调用，第二次起为空操作；未 `Configure` 时 `CreateServices()` 返回
  空指针，`AddressBook()`/`Int3()` 仍可用但不真正落地（空文件路径、工厂拒绝一切）。
- `WorkbenchPageProvider`：Gate 不可用时端口 `Read` 调用次数恒为 0；换目标后旧结果必须
  被丢弃（`cancelPages`，不得 `deliverPage`）；同一来源代次内第一次 DDMA `scratchAreaDirty`
  之后同代次请求端口调用次数恒为 0，`resetScratchLatch` 后恢复；并发 `RequestPages` 时
  端口侧观察到的调用永不重叠（单线程池）；`channelFailed` 之后不自动重试，必须显式
  `retryFailedRanges`。
- `WorkbenchBaselineFeeder`：有在途请求时 `noteDirty` 不启动计时器；插入点所在页未落定
  时发 `baselineUnavailable(InsertionPageNotSettled)` 且不调用 `RefreshBaseline`；锚点
  在窗口内滚动不触发任何喂入（`Keep` 分支，用调用计数断言）；`identityKey` 变化后下一次
  必须是 `RefreshSameSpan` 或 `Recompute`，不得对旧身份的窗口直接续喂。
- `WorkbenchWriteController`：`Idle`/`Staged` 状态下 `IByteStore::Write` 调用次数恒为 0；
  提交期间 `canvasReadOnlyHook_` 恰好被调用两次（`true` 后 `false`），即使确认接口抛
  异常也要走到 `false`；`PendingStage` 2 秒超时后发 `pendingStageResolved(false,...)`
  且不再重试；`Undo()` 遇目标已变返回 `TargetChanged` 且游标不动；`Undo`/`Redo` 本身
  不向 journal 记新的一步；journal 只对 `blocksWritten` 个真正落地的块记账（构造"前两块
  成功、第三块 `VerifyMismatch`"的假端口场景验证只记两步）。
- `WorkbenchHexPane`：`setAddressSpace` 之后 `canvas_->sourceRevision()` 变化且
  `overlay().IdentityKey()` 更新；`onCanvasVisibleRangeChanged`/`onCanvasCaretMoved`
  把参数原样转发给注入的 `WorkbenchBaselineFeeder::noteDirty`（假 Feeder 记录参数）；
  三条管线任一为空时仍能构造并显示"无数据"初态，不崩溃。
- `MemoryWorkbenchView`：身份变化的 `openAt` 对离开守卫恰好问一次（不是两次或三次，
  对应 N2"一次合并"）；守卫被拒绝时会话逐字段不变；`Reload` 掩码不清补丁，身份位必清；
  `setEmbeddedProcessMode(true)` 后对内核/物理的 `requestIdentity` 恒失败；`wireActions`
  对 `Redo` 的主键与候补键都能触发同一次 `redo()`；主窗口关闭流程里 int3 的
  `RequestLeave` 必须先于停驱动（调用顺序记录断言）。

## 8 规格矛盾与需要主会话决策的问题

### 8.0 主会话裁决（2026-10-03，审阅接口冻结后补定；以本节为准，8.1/8.2 保留原始论证）

| 项 | 裁决 | 落点 |
|---|---|---|
| 决策 1 设置持久化权威视图 | **选项 A**：只有主 Dock 的非内嵌视图落盘，内嵌窗口永不落盘 | `MemoryWorkbenchView::setSettingsAuthoritative`（默认 false，安全默认=少落盘不乱落盘）；非权威视图的 `saveSettings()` 是空操作 |
| 决策 2 权限类失败判据 | **都不选**：既有 `ks::ui::promptForPrivilegeFailure(parent, 功能名, 文本)`（`Framework/PrivilegeElevationPrompt.h`）本身就按"拒绝访问/error=5/0x80070005…"文本判据，且已提权时返回 false。不新增 `IsPrivilegeFailureText`，不改冻结契约 | `MemoryWorkbenchView::writeFailureText(QString)` 信号：每次写入失败发一次原文，宿主 Dock 连接它并调用上述既有函数；无处理者则只进状态条与诊断抽屉 |
| 决策 3 读线程池粒度 | **选项 A**：每视图一条串行池。DDMA 读写共用的那把闸在真实端口里是全进程一把锁（Wave 1 已实现），所以多视图并发不会撞 DDMA | `WorkbenchPageProvider` 不变 |
| G1 `HexCanvas::settledPageStartsInRange` | **接受**，作为 `WorkbenchHexPane`/`WorkbenchBaselineFeeder` 实现者的前置小改（改 `HexCanvas`，附测试，只增不改既有行为） | 实现波 Hex 包 |
| G2 `CodeEditorWidget::setWordWrapEnabled` | **不改 `CodeEditorWidget`**（主程序基础设施，改头会扩大重编译面）。诊断抽屉宿主实现先看 `CodeEditorWidget` 现有的换行机制能否复用；不能就让宿主的 `SetWrapEnabled` 报告"不支持"，状态条隐藏换行开关 | `WorkbenchDiagnosticsHost`（G3） |
| G3 `IWorkbenchDiagnosticsHost` 生产实现 | **并入 `MemoryWorkbenchView` 实现波**，新文件 `WorkbenchDiagnosticsHost.h/.cpp`，夹具里用等价替身 | View 实现者 |
| G4 地址簿去重 | **采纳**建议规则：同 `targetKey` 且解析后的绝对地址相同视为重复，提示而不阻止 | `MemoryWorkbenchView::addInsertionPointToAddressBook` |

**审阅时补的三处接口缺口（已改头文件）**
1. **端口/内核事务端口/审计接收器没有来源**：`WorkbenchShared::Configure` 改成接收 `WorkbenchBackends` 结构（含 `ioPortFactory`、可选 `kernelPortFactory`、`auditSinkFactory`），并新增 `CreateIoPort()`、`CreateKernelMutationPort()`、`AuditSink()`、`UsesNullAudit()`。装配层六个类不依赖任何驱动/Win32/日志头文件，一切生产实现从 Dock 侧注入。
   补充（2026-10 wave3 wpJ1 独立审核修复后的实际语义，由 `tools/memwb_ui/wpJ1/` 夹具逐条断言，变异重放用 `tools/memwb_ui/Invoke-MemwbMutation.ps1`）：若 `Configure` 之前已有代码惰性访问过 `AddressBook()`/`AddressBookTable()`（装配顺序错误——装配层必须在创建第一个 `MemoryWorkbenchView` 之前调用 `Configure`），`Configure` 返回 `false` 并打印一条 `qWarning` 诊断信息，不会像早期实现那样把已经被持有引用的对象整份替换掉（那样会让引用悬空）。`Int3()` 的提前访问不受这条限制——`Int3Controller` 改为全进程只构造一次、内部持有一份"转发工厂"，`Configure` 只是把真正的字节存储工厂填进去，提前访问过的旧引用在 `Configure` 成功之后依然有效且会自动获得真实写入能力。`AuditSink()` 的返回对象身份在单例整个生命周期内恒定（内部是一个"转发器"，真实接收器注入前丢弃、注入后转发），不会出现"`Configure` 前后身份切换"。
2. **审计不得悄悄变成空操作**：原头文件在 `WorkbenchWriteController` 里内置了 `NullAuditSink`，会让设计表里"进程范围立即写不弹确认但审计仍要写"在生产里静默失效。现改为 `setAuditSink(IAuditSink*)` 注入，**未设置时 `ensureTransaction()` 失败**（与缺 overlay/target 同等处理），漏接线在第一次提交就暴露；`WorkbenchShared::AuditSink()` 在没有生产实现时才退回内置空接收器并让 `UsesNullAudit()` 为真，装配与测试据此断言生产路径没漏接。生产审计接收器（写项目日志、需要 `Framework.h`）是 Dock 侧新文件（工作包 K），不属于这六个类。
3. **`MemoryWorkbenchView` 缺转发信号**：补 `pickTargetRequested()`（头文件里槽的注释说"经信号出口转发"，却没声明信号）与 `writeFailureText(QString)`。

### 8.1 决策点（两种选择 + 我的倾向）

**决策 1：设置持久化的"权威视图"是哪一个。**
`ChannelMemory`（范围→通道的记忆）活在每个 `WorkbenchSessionBar` 实例里，不是
`WorkbenchShared` 的进程级单例（设计文档 §0 第 5 条只把地址簿与 int3 账本定为单例）。
但 `ux.md` §9 的 `channel/process|kernel|physical` 三个持久化键只能有一份"真值"写回
`QSettings`。
- 选项 A：只有主 Dock 的（非内嵌）`MemoryWorkbenchView` 在退出前 `saveSettings()`，
  内嵌窗口永不落盘。
- 选项 B：每个视图退出时都尝试落盘，"最后一个关闭的生效"。
- 我的倾向：**选项 A**。选项 B 在多开内嵌窗口时落盘顺序不确定，容易出现"用户刚在
  内嵌窗口切的通道被主窗口的旧记忆覆盖回去"的诡异体验；选项 A 需要 `MemoryDock`
  （WP-K）在构造主视图与内嵌视图时分别传入一个 `isAuthoritative` 标记（例如
  `loadSettings()`/`saveSettings()` 各加一个默认 `true` 的布尔参数），内嵌窗口传
  `false`。

**决策 2："权限类失败才弹 `promptForPrivilegeFailure`"的判据缺失。**
`CommitReport::failureText` 是通道自己给的英文细节串，没有任何既有组件提供
"这段文本是不是权限问题"的判据；旧代码 `ViewBreakpointUtil.cpp:566` 大概率是按
`NTSTATUS`/错误码字符串做字符串匹配。
- 选项 A：在 `WorkbenchMessages.h` 新增一个纯函数判据（例如
  `bool IsPrivilegeFailureText(const QString&)`），按已知的几个权限相关 NTSTATUS/
  Win32 错误码文本关键字匹配，装配层调用它决定是否弹框。
- 选项 B：改动已冻结的 `MemoryIoPort.h`/`MemoryWriteTransaction.h` 契约，在
  `IoWriteResult`/`AccessResult`/`CommitReport` 里加一个专门的 `bool
  isPrivilegeFailure` 字段，由端口/事务管线在源头精确标记。
- 我的倾向：**选项 A**。选项 B 更干净，但要求重新审核两个已经独立复核过、被多个
  工作包依赖的冻结类型，影响面远超本次任务范围；选项 A 只是新增一个纯函数，
  不改任何已有签名，风险可控，且与"仅权限类失败弹框"这条本就是启发式判断（不是
  协议级保证）的性质相符。

**决策 3：`WorkbenchPageProvider` 的读线程池是"每视图一条"还是"全进程唯一一条"。**
设计文档 §3 不变式 5 写"读线程=一条串行池，只持值"，字面上可以理解为"全进程只有
一条"或"每个画布各自一条串行（避免同一画布内请求乱序），不同画布互不影响"。本文档
的 `WorkbenchPageProvider.h` 选择了后者：每个 `MemoryWorkbenchView` 各自持有一个
`WorkbenchPageProvider`、各自一条单线程 `ReadPool`。
- 选项 A（本文档当前实现）：每视图一条串行池。不同 Dock 实例的目标/通道完全独立，
  一个视图的慢速 DDMA 读取不会卡住另一个视图的刷新。
- 选项 B：全进程唯一一条串行池，所有视图的页请求都排在同一条队列上。
- 我的倾向：**维持选项 A**。多个内嵌窗口各自独立操作是设计文档已经支持的场景
  （target.md §6"内嵌多实例"），选项 B 会让它们互相拖慢，与"内嵌实例应该像独立
  窗口一样可用"的既有定调相悖；但不变式 5 的原始措辞确实可能另有所指，请主会话
  确认我的理解是否正确。

### 8.2 接口缺口清单（需要底座/组件新增，均未改动任何既有文件）

| 编号 | 缺口 | 建议签名 | 所属包 |
|---|---|---|---|
| G1 | `HexCanvas.h` 没有公开"某范围内已落定页起始地址"的查询，`WorkbenchBaselineFeeder` 依赖它喂 `SelectBaselineSpan`/`DecideBaselineRefeed` 的 `settledPageStarts` 参数 | `std::set<std::uint64_t> settledPageStartsInRange(std::uint64_t first, std::uint64_t last) const;`（语义：按页对齐枚举覆盖的整页，页状态为 `Valid`/`PartiallyValid`/`Unreadable` 即已落定、不含 `NotLoaded`/在途，不刷新 LRU） | `HexCanvas.h`（WP-0/Phase 1 所有者） |
| G2 | `CodeEditorWidget` 没有编程式设置自动换行的公开方法（只有内置 `m_wrapButton`），`WorkbenchStatusBar::IWorkbenchDiagnosticsHost::SetWrapEnabled` 包装它时无法真正生效 | `void setWordWrapEnabled(bool);`、`bool wordWrapEnabled() const;` | `UI/CodeEditorWidget.h`（主程序 UI 基础设施，不属于内存工作台任一既有包） |
| G3 | `WorkbenchStatusBar::IWorkbenchDiagnosticsHost` 的生产实现（包一层 `CodeEditorWidget`）尚不存在，`MemoryWorkbenchView::buildUi()` 需要它才能构造 `WorkbenchStatusBar` | 新文件 `UI/MemoryWorkbench/WorkbenchDiagnosticsHost.h/.cpp`，一个实现 `IWorkbenchDiagnosticsHost` 的小类 | 不是本次六个类之一，建议并入 WP-Z/实现波，由实现 `MemoryWorkbenchView.cpp` 的人补 |
| G4 | 地址簿"加入"动作的去重规则未由任何既有包确定（`fix-wpE.md` 明确"去重契约留给装配层"） | 非接口缺口，是行为约定；建议规则："同 `targetKey` 且解析后的绝对地址相同视为重复，提示而不阻止（允许用户坚持添加）" | 由 `MemoryWorkbenchView` 新增的 `addInsertionPointToAddressBook` 实现时采纳，供主会话确认是否改为"同模块+RVA"或"不做去重" |

### 8.3 对 ux.md 字面表述的一处偏离说明（非待决，仅记录）

`ux.md` §6 写"立即写入：Ctrl+Z = `Stage(before)` + `Commit()`"，字面理解是直接 Stage
进当前显示的那一份 `MemoryDiffOverlay`；但该 overlay 的基线窗口会随视口跟随移动，撤销
地址可能已经滚出窗口，直接 Stage 会被 `OutOfWindow` 拒绝。`WorkbenchWriteController.Undo.h`
采用的方案是为每次撤销/重做新建一个只覆盖该一个块的**临时** scratch overlay 走完整的
`Commit()` 管线，完全复用现有的确认/复核/写入/回读/审计逻辑，不依赖当前可见窗口。这
不是"矛盾"（设计文档本身没有再谈这个细节），按任务规则"遇到设计文档与已有组件真实
接口不符时，以已有组件的真实接口为准"处理，记录于此供审核时核对。

## 9 波 4（3a 并存）MemoryDock 接线记录

实现文件：`MemoryDock/MemoryDock.Workbench.cpp`（新）；改动的既有文件均为最小 Edit：`MemoryDock.h`
（成员声明）、`MemoryDock.UiBuild.cpp`（图标循环之后 `initializeWorkbenchTab()`、析构先 `shutdownWorkbench()`）、
`MemoryDock.ProcessRegion.cpp`（三个钩子 + 内嵌标志）、`MemoryDock.ViewBreakpointUtil.cpp`（`jumpToAddress`
函数体原样改名 `jumpToAddressLegacy`、`confirmDiscardMemoryEditsForProcessChange` 末尾加工作台守卫）、
`MainWindow.cpp`（`closeEvent` 最前的退出守卫）。

- **页签与懒创建**：`insertTab(3, …)`（内存搜索之后、旧内存查看器之前），首次切到该页签才创建视图
  （`ensureWorkbenchView()`：`ConfigureShared()` → 创建视图 → 注入七个服务 → `setSettingsAuthoritative(true)`
  → `loadSettings()` → 连信号 → 回放创建之前已发生的附加）。内嵌进程详情窗口：页签随 `setProcessDetailMemoryScope`
  的可见集被隐藏，`m_workbenchEmbedded` 置位，视图永不创建、分发器恒走旧路径。设置 `enabled`（默认真）为假时
  根本不插页签。
- **三个钩子**：`attachToProcess` 成功路径、`detachProcess` 最开头（句柄关闭之前，int3 安全网要求句柄仍有效）、
  `detachProcess` 最末；视图未创建时都是空操作。附加前的离开守卫经 `confirmDiscardMemoryEditsForProcessChange`
  末尾调用：只有 `WorkbenchTarget::wouldChangeOnDockAttach()` 为真才问（钉在别处时 Dock 的附加与它无关）。
- **“保留补丁继续”必须被尊重**：守卫放行且用户在 int3 三选一里选了保留时，视图置一次性记号
  `int3KeptByLeaveGuard_`，紧接着的 `aboutToDetach` 安全网据此跳过一次强制还原；未经提示的分离仍由安全网兜底。
- **跳转分发器**：`MemoryDock::jumpToAddress`（`MemoryDock.Workbench.cpp`）在 `routeJumps`（默认假）为假、内嵌
  或页签不存在时直接 `jumpToAddressLegacy`（旧函数体原样）；旧地址框直接调 `jumpToAddressLegacy`。3b 只改
  `routeJumps` 的默认值。
- **退出守卫**：`MainWindow::closeEvent` 在救援实例分支之后、`saveDockLayoutToConfig()` 之前调
  `MemoryDock::confirmWorkbenchQuit()` → `MemoryWorkbenchView::confirmQuit()`；它与离开守卫共用
  `runLeaveSequence`（三阶段原子：先问暂存、再问 int3、两个都同意才执行），原因文案固定“退出程序”，int3 场景
  为 `MainWindowClose`。用户取消则放弃本次关闭（`event->ignore()`）。
- **不在 3a**：模块表/区域表双击路由、搜索结果“加入地址簿”、`m_moduleCachePid`、旧页签标“（旧）”、删除旧页签。

## 10 波 4（3b 入口切换）MemoryDock 接线记录

新增：`MemoryDock/MemoryDock.WorkbenchEntry.cpp`（本 Dock 一侧的全部入口）、`UI/MemoryWorkbench/WorkbenchBookIntake.{h,cpp}`
（“加入地址簿”规则，不碰控件与共享单例，夹具可直接测）、`MemoryDock/WorkbenchServicesMapping` 的 `DecideModuleJumpPin`
（模块表是否钉住预览进程的纯函数，wpK1 逐分支断言）。回退开关：设置对话框“由内存工作台接管跳转”取消勾选（键
`memwb/workbench/routeJumps`，无需发版）；整体开关 `enabled` 仍可彻底关掉工作台。

- **routeJumps 默认真**（`WorkbenchSettings::LoadRouteJumps`）。`jumpToAddress` 分发器的请求范围固定为进程（原先沿用
  会话当前范围：工作台停在内核范围时双击模块会得到“地址不在当前范围内”）；`pid=0` 表示跟随 Dock，工作台若钉在别处会被带回跟随。
- **模块表**：双击/右键走 `jumpToModuleBase`。模块表可以预览未附加的进程，所以缓存落地时记 `m_moduleCachePid` +
  `m_moduleCacheCreateTime`（工作线程里用 `AcquireAnchorForPid`——与工作台锚点同一取法——读创建时间，pid==0 与分离时清零）。
  `DecideModuleJumpPin`：预览进程非空且不同于附加进程 → 钉住它（带创建时间核对进程实例）；相同或缓存为空 → 不钉住。
  内嵌窗口的工作台 `lockToDock`，钉住请求被策略拒绝（`Unavailable`，状态条报告），不会静默去看错误的进程。
- **区域表**：双击/“查看此区域”走 `jumpToAddress`；“R0读取此区域”走 `viewRegionViaDriver`——路由开启时显式
  `channel=StandardDriver` 在工作台打开区域起点，关闭时仍是旧 `prepareDriverMemoryReadAtAddress`。
- **搜索结果表**：表格启用了排序，旧代码按 `row` 当 `m_searchResultCache` 下标是错的（排序后双击会跳到别的地址，右键菜单里的
  “添加到书签/复制值”也读错行）。现在地址一律取自第 0 列单元格的 `Qt::UserRole`，其余字段按地址回查缓存；
  新增右键“加入地址簿”（该行）与“全部加入地址簿”（表中显示的全部行）：种类=搜索结果（不落盘，用户在地址簿里提升为书签才保存），
  地址簿总数上限 10000，按（目标键, 绝对地址）去重，一次批量 `addMany`（整表只重建一次）。目标键与视图同一函数生成
  （`pid:<pid>@<创建时间>`）。**绝不自动灌入**：只有用户点了才加。
- **证据页**：进程内存证据、PTE/VA 翻译、内核内存证据、内核可执行页四张表右键增加“在内存工作台打开”
  （`addOpenInWorkbenchAction`，沿父链找到所属 Dock；地址取第 0 列文本；进程类按附加进程、内核类切内核范围）。PTE 页的默认
  地址取 `workbenchFocusAddress()`（工作台路由开启且跟随本 Dock 的附加进程时的插入点），否则退回旧查看器地址。
- **旧页签**：内存查看器、断点与书签、驱动内存读写改名“（旧）”（新语言键 `memory.tab.*_legacy`）并经 `QTabBar::moveTab`
  后移到页签栏末尾；`showLegacyTabs`（默认真）为假时一并隐藏。所有页签引用都按页面控件指针而非下标，所以重排不影响刷新钮等路由。
- **内嵌进程详情窗口**：`ensureWorkbenchView` 不再因内嵌而跳过——视图在 `loadSettings()` 之后切 `setEmbeddedProcessMode(true)`
  （恒跟随本 Dock、禁内核/物理、侧栏隐藏、**不落盘设置**）。`applyProcessDetailTabVisibility` 在路由开启时把“看内存”那一页
  换成工作台（旧查看器隐藏），路由关闭时仍是旧查看器；设置对话框切换路由时内嵌实例会重新应用。
  窗口关闭 → `~MemoryDock` → `detachProcess` → `aboutToDetach` 安全网还原该视图自己的 int3 补丁。
- **int3 账本“当前目标”是全进程唯一一份**，主 Dock 的视图与内嵌窗口的视图共用它。视图新增 `syncInt3Context()`
  （与 `applyInt3Context` 内容一致时不重复写，免得面板无谓重建），在这些时刻重新声明本视图的会话：右键写入/还原 int3、
  侧栏面板三个按钮（`Int3PatchPanel::aboutToAct` 直接连接）、`runLeaveSequence`（离开守卫与退出守卫共用）、
  `onTargetAboutToDetach` 安全网、`showEvent`、窗口激活。**多目标退出时的“全部还原”仍只处理当前目标**（已记录的局限，
  账本层没有“按所有目标退出询问”的接口）。
- **窄宽度硬下限（侧栏）**：视图本身可收窄到 160px 以下无硬下限，残留的是侧栏——窄窗口里手动展开侧栏后，侧栏容器的
  `minimumSizeHint`（约 244px）让 QSplitter 不肯把它压窄，画布被挤到 1px。（不是 root 那种 `SetDefaultConstraint` 回灌：
  它只作用于顶层窗口，侧栏容器是子控件，变异实测对它加 `SetNoConstraint` 没有任何差别，所以没加。）现在窄宽度
  （< 760）且侧栏可见时 `updateSidebarWidthCap` 给主体留底（`min(300, 可用宽/2)`）：设侧栏最大宽度，并显式 `setSizes`
  （只设最大宽度 QSplitter 不会收回已记住的尺寸）；宽屏不设上限，上限解除时侧栏回到偏好宽度。回归在
  `wpJ6_tests.Narrow.cpp`。未修（低于实用宽度）：`HexViewSegmented` 固定宽约 170px、文本子页工具钮 ≤240px 越界数像素。
- **“深色下标签空白”（状态条摘要段）**：任务只给了一句话标题，先写探针而不是猜。`wpJ6_tests.DarkLabels.cpp` 渲染整个视图
  （及真实确认弹窗、字符串写入对话框），对每个有文字的控件/表格单元格/表头判“文字与底色无差别”，在 9 条环境路径
  （纯调色板 / 真实全局样式块 / 嵌进带主窗口 QSS 的 QMainWindow / 运行期切主题两个方向 / 自定义主背景色）× 约 40 个界面状态
  上扫约五千个文字单元，**没有发现任何由颜色造成的空白**（所有文字/底色配对对比度 ≥ 3.14）。唯一真实的“空白标签”是
  **窄宽度下状态条摘要段被布局压成 1px**（`R3 · 进程` 在 240px 时整段消失、360px 时只剩 `R`，读取结果被硬裁在“结论是当前不可”），
  与主题无关（深浅两套一致）。修法在 `WorkbenchStatusBar.cpp`：通道·范围段（短且有界）恢复默认“最小宽度=自身文字宽度”，绝不压缩；
  读取结果/窗口范围两段改用 `ElidedSegmentLabel`——`text()` 仍是完整原文（运行期整句翻译与 S1/N1 测试按它精确匹配），放不下时
  **绘制**成右省略，最小宽度只够一个省略号（不是 setMinimumWidth(1)，也不是“最小=文字宽度”，后者会棘轮式撑大状态条，T13）；
  写入结果段沿用原来的 Ignored + setText 省略。回归：`wpJ6_tests.Narrow.cpp::TestStatusBarSegmentsNeverBlank`（逐宽度断言 + 与普通
  QLabel 渲染逐像素对照）。探针留作永久守卫（断言：零空白/塌缩/低对比，且每条路径必须扫到 ≥300 个文字单元）。**守卫的已知盲区**：
  int3 面板“被改过”的警示行（需要 Diverged 状态）、地址簿的“已读取/过期”取值色（需要真实读值），这两处只有数值配对检查兜底。
  变异验证：往状态条与探针覆盖的生产代码里注入“文字与底同色/不省略/省略方向反/Elastic 退回普通 QLabel”等 11 种变异，全部被
  Narrow/DarkLabels/wpG T13 抓到，唯一幸存的是把 `OpaqueDialogStyle` 中 QDialog 自身 `color` 改成 `palette(window)`——等价变异
  （Qt 样式表默认不把 color 继承给子控件；同规则里改 background 会被抓）。变异必须串行跑（并行同一夹具会假性 COMPILE_ERROR）。
- **真窗口反馈修复（2026-10-06）**：
  - **控件含义**：会话条写入模式胶囊（`WriteModeSwitch`）两半改为自绘半边按钮（`CapsuleHalfButton`，图标 + 文字“即时/暂存” + 逐半边悬停说明，
    点击不自己翻转高亮，由 `setMode` 回写）——主程序全局 `QToolButton` 样式（带边框、悬停整块填强调色、`!important`）原来会把 ⚡ 那一半画成溢出胶囊的
    蓝色方块；地址栏右侧三控件（重读 / 实时刷新 / 展开侧栏）带文字标签，视图窄于 `kAddressRowLabelsMinWidth`(560) 退成纯图标
    （`updateAddressRowLabels`）。
  - **整页滚动 / 页面过长**：内存 Dock 加入 `MainWindow.cpp::DockSuppressesOuterScrollArea`（ForceNoScrollArea）；MemoryDock 每个页签用
    `UI/AdaptivePageScroll.h::EnablePageInnerScroll` 包内部滚动壳（Ignored 策略、关自填背景），`m_tabWidget` 做 `IsolateMinimumSize`。根因：
    `QTabWidget` 最小高度取所有页最大值（DDMA≈1070、系统审计≈850）+ ADS 自动滚动区把整页滚走。
  - **滚轮**：画布 `setProperty("ksword_disable_smooth_scroll", true)` 退出全局平滑滚动过滤器（该过滤器把滚动条单位当像素，画布单位是行，一档≈一页）。
  - **编辑区自适应**：`HexCanvas::setAutoBytesPerRow`（8/16/32/48/64 里取视口放得下的最大档，插入点可见时以它为锚点，不重读）、
    `minimumSizeHint`（最小 4 行，不再把 Dock 顶高）、Ctrl+滚轮/Ctrl+=/-/0 字号缩放（-4..12）、子页签行右侧“视图”菜单
    （行宽/分组/字号）、偏好 `rowWidthAuto/bytesPerRow/groupSize/hexZoom` 持久化；**自适应只在 `loadSettings` 路径默认打开**，构造函数不开。
  - **子页自动跳转**（`MemoryWorkbenchView.SubPages.cpp`）：切到反汇编/文本/对比页、十六进制插入点变化（子页可见时）、会话身份变化、数据晚到
    （`contentChanged`）都会跟随；每页一个“同步令牌”（上次跟随时十六进制选区起点）——用户在子页里手动导航后切回不覆盖，十六进制动过才重新定位；
    没有选区（选区起点 == 地址空间起点）时落到起始模块（进程名命中 → 最低基址 `.exe` → 最低基址；内核取 `ntoskrnl.exe`；模块目录加载中挂起，
    就绪/失败后重试；物理范围与 DDMA 通道不兜底）；Ctrl+D 与右键“从此处反汇编”改走 `showSubPageAt`（右键原来只在十六进制里重定位，是 bug）。
- **验证**：wpJ6 `wpJ6_tests.Entry3b.cpp`（两视图共用账本的右键/面板/显示/激活/分离安全网/退出询问、`focusAddress`、
  内嵌拒绝钉住、加入地址簿规则与 10000 上限）、wpK1 `TestModuleJumpPin`、wpG 默认值断言翻转。**真窗口未验证**：
  页签重排的实际外观、内嵌窗口里工作台的真实交互、搜索结果排序后双击与右键的真机行为、证据页菜单项。
