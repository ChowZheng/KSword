#pragma once

// ============================================================
// WorkbenchWriteController.h
// 作用：
// - 写路径 W1-W4 的装配层（见 docs/内存工作台Phase3集成设计.md §1"写路径"、
//   docs/内存工作台Phase3装配接口.md §2）：持有唯一的 ksword::memwb::
//   MemoryWriteTransaction，把画布/面板发出的编辑（已经落在 overlay 里的暂存补丁）
//   同步提交到目标内存，覆盖立即写入与暂存后应用两种模式，并驱动确认策略、
//   PendingStage 票据、journal 记账与提交期间的只读/挂起。
// - 本类不持有 MemoryDiffOverlay 的所有权（唯一一份由 WorkbenchHexPane 持有，本类
//   只持非拥有指针），不持有 WorkbenchTarget 的所有权（同理）。本类拥有的是写事务
//   管线自身需要的三个长期对象：IMemoryIoPort 实现、可选的 IKernelMutationPort
//   实现、以及 ksword::memwb::MemoryIoByteStore（后者按 (port, session, 内核端口)
//   构造，session 以引用绑定到 target_->session() 返回的同一份存储，随会话变化自动
//   跟随，不需要重新构造）。
// - 撤销/重做（Ctrl+Z/Ctrl+Y）不走本类的正常 Stage/Commit 管线，详见
//   WorkbenchWriteController.Undo.h；本头文件只声明转发给内部 UndoCoordinator 的
//   四个公开方法（Undo/Redo/CanUndo/CanRedo），UndoCoordinator 的完整定义与"为什么
//   撤销要绕开 overlay 的基线窗口"的设计说明都在那个卫星头文件里。
// ------------------------------------------------------------
// W2-W3：提交管线（Commit 系列函数的共同行为）
// ------------------------------------------------------------
// - 提交前：writePolicy（由调用方注入，通常是 WorkbenchConfirmations 持有的
//   MemoryWritePolicy 引用）决定是否抑制普通界面确认；调用方法：装配层在每次
//   会话/模式变化后，先对 confirmation_ 调一次 WorkbenchConfirmations::
//   SetCurrentContext，再对本类调一次 setUiConfirmSuppressed（取
//   MemoryWritePolicy::Decide(...).suppressed），本类不重复计算这个布尔值，只负责
//   转发给内部的 transaction_（见 setUiConfirmSuppressed 的声明注释）。
// - 提交期间（isCommitting()==true）：canvas_ 被置为只读（setEditable(false)）、
//   实时刷新被挂起（装配层应在 isCommitting() 变真时暂停自己的 1Hz 定时器）。提交
//   结束（成功或失败）后恢复只读状态（除非别的原因仍要求只读，例如通道不可写）。
// - 提交返回的 CommitReport 由本类统一解读并产出三类后续动作（W4）：
//     ① 已写块局部重读：通过 rereadRangeCallback_ 请求 PageProvider 对
//        [report 覆盖的地址范围] 做一次原位重读（不是整表 refresh）；
//     ② report.needsReread 为真：调用 target_->requestReload()（来源代次 +1，
//        触发画布重新请求可见页）；
//     ③ report.scratchAreaDirty 为真：发 scratchAreaDirtyReported(true)，装配层
//        接到 WorkbenchStatusBar::reportScratchAreaDirty；report.outcome 非
//        Committed/NoChange 时发 commitFailed；装配层（MemoryWorkbenchView）把
//        failureText 经 writeFailureText 信号交给宿主 Dock，由宿主调用既有的
//        ks::ui::promptForPrivilegeFailure 决定是否弹提权提示（本类不判断权限问题）。
// - 成功（outcome==Committed）时，对提交前快照下来的 DiffBlocks（只取前
//   blocksWritten 个，按地址升序——Commit 按序写入且遇错即停，所以这些就是真正
//   落地的块）逐块调用 undo_->record(address, before, after)（journal 本身由
//   WorkbenchUndoCoordinator 唯一持有，本类不另存一份，避免两份历史互相漂移）。
// ------------------------------------------------------------
// D1-D4 修复（独立审核 review-wpJ4.md，主会话裁决落实）：
// ------------------------------------------------------------
// - D1/D2（transaction_ 惰性构造后对 overlay_/target_ 的晚绑定失效导致崩溃）：
//   setOverlay/setTarget/setConfirmationSink/setAuditSink 在 transaction_ 已经
//   构造成功之后，若再次被调用并试图换成"不同的值"，直接拒绝（忽略新值并
//   qWarning，不用 Q_ASSERT——Release 构建也要安全）；换成"相同的值"是空操作；
//   第一次 ensureTransaction() 成功之前随意调用不受限。详见各 setter 的实现。
// - D3（确认框弹出期间事件循环继续运转，导致嵌套编辑把画布提前解锁+误发失败
//   信号）与 D4（确认框期间触发撤销，叠出第二个确认框且正在确认的编辑被静默
//   吞掉）：commitDepth_ 是跨越"正常提交"与"撤销/重做的临时提交"的统一重入
//   深度计数器，由 detail::CommitReadOnlyGuard 在构造/析构时 ++/--；只有
//   0→1（最外层进入）才真正触发 canvasReadOnlyHook_/commitSuspendHook_ 的
//   true，只有 1→0（最外层退出）才触发 false。onEditCompleted/commitPendingNow/
//   undo/redo/resolveModeSwitch 的 ApplyThenSwitch 分支动手之前都先检查
//   commitDepth_ > 0，五个入口在"忙"时的处置完全一致、没有任何差异：直接返回
//   CommitEntryStatus::Busy（或等价的 ModeSwitchStatus::Busy），并发
//   commitRejectedBusy 信号，供装配层在状态条给一句"操作进行中"提示；不新建
//   事务、不嵌套执行、也不留下任何残留标记——下一次真正独立的编辑/撤销/重做/
//   模式切换不会被这一次的忙碌拒绝污染。
// - 第二轮独立审核（review2-wpJ4.md B-1/B-2）发现：早期修复曾经让
//   onEditCompleted 在"忙"时置一个 rerunRequested_ 标记，待外层 Commit 结束后
//   自动合并补跑一次——这个机制完全不看外层那次的结果，哪怕用户刚刚在确认框里
//   点了"否"也会被无视、立刻又弹一次几乎相同的确认框；且这个标记只在
//   onEditCompleted 自己的函数体内被消费，经由另外四个入口触发的忙碌拒绝会让
//   它永久卡在 true，污染下一次完全无关的正常编辑。主会话裁决：**整个删掉这个
//   合并补跑机制**，不再自作主张替用户重试。onEditCompleted 在"忙"时只是让
//   已经真实落在 overlay_ 里的那个补丁继续暂存（画布照常显示"待写入"的橙色
//   提示，这本来就是既有设计），不自动重跑、不再弹确认框。用户之后任意一次
//   真正的新编辑，或主动点"应用"（commitPendingNow），都会触发一次新的
//   Commit()——Core 的 Commit() 本来就是对 overlay_ 当前全部 DiffBlocks 整体
//   处理，被忙碌拒绝过的那个补丁只要还留在 overlay_ 里，自然会被这次新的
//   Commit 一起带上，不需要本类另外补一次调用。
// ============================================================

#include "WorkbenchTarget.h"

#include "../../../../shared/evidence/memory_workbench/MemoryDiffOverlay.h"
#include "../../../../shared/evidence/memory_workbench/MemoryIoByteStore.h"
#include "../../../../shared/evidence/memory_workbench/MemoryIoPort.h"
#include "../../../../shared/evidence/memory_workbench/MemoryTargetSession.h"
#include "../../../../shared/evidence/memory_workbench/MemoryWriteTransaction.h"

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

// QTimer 只以指针形式出现在 PendingStageEntry 里，前置声明即可，不需要为了一个
// 指针成员拖进整个 <QTimer>（真正需要完整定义的 .PendingStage.cpp 自己
// #include <QTimer>）。
class QTimer;

namespace ks::ui
{
    // WorkbenchWriteController：写路径装配层，详见文件头。全部公开函数只在 UI 线程
    // 调用（Commit() 本身也是同步、在 UI 线程内完成，不变式 1 的"同步 Commit 能衔接"
    // 条件见设计文档 §3）。
    // WorkbenchUndoCoordinator：撤销/重做协调器，完整定义在
    // WorkbenchWriteController.Undo.h（卫星头文件，不是第七个装配层类，只是
    // WorkbenchWriteController 的实现细节被拆到另一个文件以控制单文件行数）。
    // 这里只前置声明，主头文件的包含者不需要知道它的内部结构。
    class WorkbenchUndoCoordinator;

    // CommitEntryStatus（D3/D4 修复新增）：commitPendingNow/undo/redo 在重入保护
    // 下的返回状态。Started 表示本次调用确实越过了"忙"检查、往下走了一次尝试
    // （具体结果请看随附的 CommitReport，或 undo()/redo() 场景下用
    // canUndo()/canRedo() 的变化间接判断）；Busy 表示已有 Commit 正在进行（可能是
    // 外层确认框的事件循环还没返回），本次调用被直接拒绝，完全没有触碰
    // overlay_/端口，调用方应当把它当"现在不能做，稍后再试"展示，不能当成
    // WriteFailed/TargetChanged 这类真实写入失败处理。
    // 这是本类自己的枚举，不是 ksword::memwb::CommitOutcome 的一部分——那个类型
    // 冻结在 shared/evidence，不属于本次允许修改的范围，所以新增 Busy 语义只能
    // 走装配层自己的类型。
    enum class CommitEntryStatus
    {
        Started,
        Busy,
    };

    // CommitAttempt（D3 修复新增）：commitPendingNow() 的返回外壳。status==Busy
    // 时 report 是默认构造的占位值（outcome=NoChange），调用方不应读取它；
    // status==Started 时 report 是这次 Commit() 真实返回的完整报告（与旧版本
    // "直接返回 CommitReport"语义一致，只是多包了一层状态位）。
    struct CommitAttempt
    {
        CommitEntryStatus status = CommitEntryStatus::Started;
        ksword::memwb::CommitReport report;
    };

    class WorkbenchWriteController final : public QObject
    {
        Q_OBJECT

    public:
        // IoPortFactory / KernelPortFactory：见 WorkbenchPageProvider.h 的同名类型。
        // 本类各自调用一次并长期持有返回的对象。KernelPortFactory 可以不设置
        // （setKernelMutationPortFactory 不调用）——此时内核范围+标准驱动通道的写入
        // 会按 MemoryIoByteStore 文档的规则直接失败并给出原因，不会静默降级。
        using IoPortFactory = std::function<std::unique_ptr<ksword::memwb::IMemoryIoPort>()>;
        using KernelPortFactory = std::function<std::unique_ptr<ksword::memwb::IKernelMutationPort>()>;
        // RereadRangeFn：局部重读回调，签名对应 PageProvider 的"重新对这段范围发起
        // 一次读取，使用当前来源代次原地替换"（不是整表 refresh）。
        using RereadRangeFn = std::function<void(std::uint64_t address, std::uint64_t length)>;
        // TickProviderFn：取单调时间戳（毫秒），供 MemoryEditJournal 的合并窗口判断。
        using TickProviderFn = std::function<std::uint64_t()>;
        // PendingStageTicket：见 beginPendingStage 的说明；0 为无效票据。
        using PendingStageTicket = std::uint64_t;

        // 构造：overlay/target 为非拥有指针，必须在 setPortFactory 之前、且不晚于第一次
        // Commit 调用之前被设置为非空（见 setOverlay/setTarget）；confirmation 为
        // ksword::memwb::IConfirmationSink 的实现（通常是 WorkbenchConfirmations），
        // 非拥有，必须比本对象活得久。
        explicit WorkbenchWriteController(QObject* parent = nullptr);
        ~WorkbenchWriteController() override;

        // setOverlay / setTarget / setConfirmationSink：三个非拥有依赖的装配点。
        // **D1/D2 修复**：transaction_ 一旦惰性构造成功，就已经把当时的 overlay_/
        // target_（以及 confirmation_/audit_，见 setConfirmationSink/setAuditSink）
        // 以 C++ 引用的形式永久绑定进去——事后再换成另一个不同的值，transaction_
        // 内部的引用并不会跟着变，之前的实现对此没有任何防御，换成 nullptr 之后下
        // 一次 Commit 就是空指针解引用崩溃（见 review-wpJ4.md D1/D2，探针实测
        // 0xC0000005）。现在这四个 setter 在 transaction_ 已存在时，若新值与当前
        // 值不同，直接拒绝（忽略调用并 qWarning 记一条诊断日志；不用 Q_ASSERT——
        // 断言在 Release 构建里不生效，这里要的是任何构建配置下都不崩溃）；新值与
        // 当前值相同（含都是同一个非空指针）视为空操作，不警告。这个限制只在
        // transaction_ 已经构造之后生效，装配阶段（第一次成功 Commit 之前）仍然可
        // 以随意调用来完成正常的"先设置、再使用"装配顺序。
        void setOverlay(ksword::memwb::MemoryDiffOverlay* overlay);
        void setTarget(WorkbenchTarget* target);
        void setConfirmationSink(ksword::memwb::IConfirmationSink* sink);

        // setAuditSink：注入审计接收器（非拥有，必须比本对象活得久；装配层传
        // WorkbenchShared::Instance().AuditSink()）。**审计不允许在本类内部悄悄变成空
        // 操作**：未设置时 ensureTransaction() 失败（与缺 overlay/target 同等处理），而不是
        // 退回一个空接收器——"跳过 UI 确认不跳过审计"是不变式，漏接线必须在第一次提交时
        // 就暴露，不能静默丢审计。夹具需要空审计时显式传一个测试用接收器。
        // 晚绑定限制同 setOverlay/setTarget/setConfirmationSink，见上方 D1/D2 说明。
        void setAuditSink(ksword::memwb::IAuditSink* sink);

        // setIoPortFactory / setKernelMutationPortFactory：注入端口工厂，各自只在
        // 第一次调用时真正构造长期对象（允许装配层先设置工厂、稍后再真正启用）。
        void setIoPortFactory(IoPortFactory factory);
        void setKernelMutationPortFactory(KernelPortFactory factory);

        using WriteValidationFn = ksword::memwb::MemoryIoByteStore::WriteValidationFn;
        // Binding updates remain valid after the shared byte store is created;
        // immediate, staged and undo/redo writes all use that same store.
        void setWriteValidationCallback(WriteValidationFn callback);

        // setRereadRangeCallback / setTickProvider：见上方类型说明；未设置
        // TickProvider 时内部退回系统单调时钟。
        void setRereadRangeCallback(RereadRangeFn callback);
        void setTickProvider(TickProviderFn provider);

        // setCanvasReadOnlyHook：提交期间需要把画布置为只读、提交结束后恢复——
        // 本类不直接持有 HexCanvas（避免头文件互相依赖），改用回调：
        // hook(true) 在最外层提交开始时调用，hook(false) 在最外层提交结束（任何
        // 结果，含异常路径）时调用。**D3 修复**：嵌套的 Commit（例如撤销/重做在
        // 外层确认框的回调里被触发）不会重复触碰这个钩子，只有最外层的进入/退出
        // 才会（见 commitDepth_ 与 detail::CommitReadOnlyGuard）。
        using CanvasReadOnlyHook = std::function<void(bool readOnlyDuringCommit)>;
        void setCanvasReadOnlyHook(CanvasReadOnlyHook hook);

        // setCommitSuspendHook（COMMON 裁决第 3 项新增）：与 canvasReadOnlyHook_
        // 分开装配的另一个提交期间通知——装配层用它在 Commit 进行期间挂起
        // WorkbenchBaselineFeeder 的基线刷新需求计算（不是画布只读，是"这段时间
        // 不要去算要不要换基线窗口"）。hook(true) 在最外层提交开始时调用，
        // hook(false) 在最外层提交结束（含异常路径，同一套 RAII）时调用；嵌套
        // Commit 同样不会重复触碰。两个钩子都未设置时各自是空操作，互不影响。
        using CommitSuspendHook = std::function<void(bool suspendedDuringCommit)>;
        void setCommitSuspendHook(CommitSuspendHook hook);

        // mode / requestModeSwitch：当前写入模式与切换请求的薄包装，直接转发
        // MemoryWriteTransaction::Mode()/SetMode()；NeedsDecision 时由调用方弹三选一
        // （WorkbenchConfirmations::PromptModeSwitch）后调用 resolveModeSwitch。
        ksword::memwb::WriteMode mode() const;
        ksword::memwb::ModeSwitchStatus requestModeSwitch(ksword::memwb::WriteMode newMode);
        // resolveModeSwitch：ApplyThenSwitch 分支内部会真的尝试一次 Commit，受
        // 上方 D3/D4 的重入保护——commitDepth_ > 0 时直接返回 status=Busy，不
        // 嵌套执行（见文件头 D3/D4 说明）。DiscardThenSwitch/Cancel 不触碰
        // Commit 管线，不受这条限制。
        ksword::memwb::ModeSwitchResult resolveModeSwitch(ksword::memwb::ModeSwitchDecision decision);

        // setUiConfirmSuppressed / uiConfirmSuppressed：转发
        // MemoryWriteTransaction::SetUiConfirmSuppressed/UiConfirmSuppressed（不变式
        // 15："Policy 只置 SetUiConfirmSuppressed，ConfirmApproval 永远弹"）。装配层
        // 在每次会话/模式变化后，用 MemoryWritePolicy::Decide(...).suppressed 的结果
        // 调用本方法；transaction_ 尚未惰性构造时，本类把布尔值缓存下来，等
        // ensureTransaction() 真正构造出 transaction_ 的那一刻据此初始化。这个缓存
        // 值同时也是撤销/重做临时事务（见 .Undo.h）继承的来源——撤销与正常提交
        // 共用同一条"确认/审计/回读"链，策略判定"这个范围/通道不弹"时撤销也不
        // 应该额外弹一次（COMMON 裁决第 3 项）。
        void setUiConfirmSuppressed(bool suppressed);
        bool uiConfirmSuppressed() const;

        // onEditCompleted：画布 editStaged 信号的装配层落点（W1）。立即模式下内部
        // 调用 transaction_->OnEditCompleted() 走一次完整 Commit 并发 commitFinished；
        // 暂存模式下只发 pendingPatchesChanged（供会话条刷新"N 字节待写入"）。
        // **D3 修复，第二轮审核 B-1/B-2 裁决后简化**：若调用时已经有一次 Commit
        // 在进行中（commitDepth_ > 0，典型场景是外层确认框的模态事件循环还在转，
        // 用户又触发了一次编辑），本次调用不会嵌套发起第二次 Commit——新字节已经
        // 正常 Stage 进 overlay（调用方已经做过）、内容代次已经推进，只是直接发
        // commitRejectedBusy 信号并返回，**不会**自动替用户补跑一次、也**不会**
        // 再弹一次确认框。补丁继续以"待写入"状态留在 overlay_ 里，等用户下一次
        // 真正的新编辑或主动点"应用"时，连同这次一起被正常的 Commit 处理掉。
        void onEditCompleted();

        // commitPendingNow：暂存模式下用户点"应用"（Ctrl+Enter）。**D3 修复**：
        // commitDepth_ > 0 时直接返回 {Busy, CommitReport{}}，不嵌套执行，并发
        // commitRejectedBusy。
        CommitAttempt commitPendingNow();

        // isCommitting：当前是否有 Commit 在进行——commitDepth_ > 0（覆盖正常
        // 提交与撤销/重做的临时提交两种情形）或 transaction_->IsBusy() 为真即为
        // true。装配层据此决定画布只读与实时刷新挂起是否应该生效。
        bool isCommitting() const;
        // True only while a real undo/redo write replay is executing.
        bool isHistoryReplay() const noexcept { return historyReplay_; }

        // beginPendingStage：地址簿"值"列编辑落在当前基线窗口外时的入口（设计文档
        // §1 第 13 条）。传入目标地址与待写入字节；本类请求 PageProvider/
        // BaselineFeeder 把窗口移动到覆盖该地址，覆盖成功后自动 Stage+走正常提交
        // 管线。2 秒内未能完成覆盖则票据超时，发 pendingStageResolved(false,...)。
        // 传出：非 0 票据；地址/字节非法时返回 0，不创建任何票据。
        PendingStageTicket beginPendingStage(std::uint64_t address, const QByteArray& bytes);

        // cancelPendingStage：显式取消一个尚未解决的票据（例如用户关闭了地址簿编辑
        // 器）；票据不存在或已解决时为空操作。
        void cancelPendingStage(PendingStageTicket ticket);

        // notifyWindowMayCover：装配层在基线窗口变化后调用（订阅
        // WorkbenchBaselineFeeder::baselineRefreshed 或画布 contentChanged），让本类
        // 重新检查是否有可以尝试的 PendingStage 票据。不传递具体范围——本类自己对每个
        // 未决票据重新尝试一次 Stage，失败（仍 OutOfWindow/UnreadBytes）则继续等待。
        void notifyWindowMayCover();

        // ---- 撤销/重做：转发给 UndoCoordinator，定义与设计说明见 .Undo.h ----
        // **D3/D4 修复**：commitDepth_ > 0 时（外层有 Commit 正在进行，含其自身的
        // 确认框还没返回）直接返回 Busy，不新建临时 scratch 事务、不弹第二个
        // 确认框，也不会把正在等待确认的那次编辑静默吞掉——装配层据 Busy 给状态条
        // 一句"操作进行中"提示即可。
        bool canUndo() const;
        bool canRedo() const;
        CommitEntryStatus undo();
        CommitEntryStatus redo();

    signals:
        // commitFinished：一次 Commit（含 OnEditCompleted 触发的那次，含撤销/重做
        // 产生的那次临时提交）结束，不论 outcome 是什么都会发；装配层用它刷新状态
        // 条的"写入结果"段。
        void commitFinished(ksword::memwb::CommitReport report);
        // commitFailed：commitFinished 的子集，outcome 既非 NoChange 也非 Committed
        // 时额外发一次，供装配层转发失败原文给宿主（见 MemoryWorkbenchView::
        // writeFailureText）；本类不判断失败属于哪一类。
        void commitFailed(ksword::memwb::CommitReport report);
        // scratchAreaDirtyReported：report.scratchAreaDirty 为真时发出，装配层转发给
        // WorkbenchStatusBar::reportScratchAreaDirty(true)。
        void scratchAreaDirtyReported();
        // pendingPatchesChanged：待写入字节数/块数变化（暂存模式下的 Stage/丢弃/
        // 应用），装配层据此刷新会话条的"N 字节待写入"区域。
        void pendingPatchesChanged(quint64 bytesPending, quint64 blocksPending);
        // pendingStageResolved：beginPendingStage 的票据有了结果（成功提交/超时/
        // 显式取消）。
        void pendingStageResolved(PendingStageTicket ticket, bool ok, QString reason);
        // undoRedoAvailabilityChanged：canUndo()/canRedo() 可能变化，装配层据此刷新
        // 撤销/重做动作的可用状态。
        void undoRedoAvailabilityChanged();
        // commitRejectedBusy（D3/D4 修复新增）：onEditCompleted/commitPendingNow/
        // undo/redo/resolveModeSwitch 的 ApplyThenSwitch 分支，在已经有 Commit 正在
        // 进行时拒绝了本次调用——不是失败，是"现在不能做"。source 是被拒绝的入口
        // 名字（纯诊断用，英文字面量，不是用户可见文案），装配层据此给状态条一句
        // "操作进行中，请稍候"之类的提示，不应该当成写入失败展示。
        void commitRejectedBusy(QString source);

    private:
        // PendingStageEntry：一个未解决的 PendingStage 票据的全部状态（原来存在
        // Internal.h 的"以 this 指针为键的关联表"里，头文件解冻后改为直接私有
        // 成员 pendingStages_，见下方说明）。timer 的生命周期由 Qt 的父子关系兜底
        // （parent() 恒为所属的 WorkbenchWriteController），本结构体只是存一份
        // 指针方便查找，不负责它的创建/销毁时机。
        struct PendingStageEntry
        {
            std::uint64_t address = 0;
            std::vector<std::uint8_t> bytes;
            // 发起时的目标与来源代次；异步覆盖结果不得跨目标、通道或重读继续写入。
            QPointer<WorkbenchTarget> target;
            std::uint64_t sourceRevision = 0;
            QTimer* timer = nullptr;
        };

        // PendingStageAttempt：TryStagePendingEntry 的结果。resolved=false 表示仍是
        // OutOfWindow/UnreadBytes 这类"窗口还没覆盖到"的可恢复失败，调用方应保持
        // 票据挂起；resolved=true 时 ok/reason 才有意义。
        struct PendingStageAttempt
        {
            bool resolved = false;
            bool ok = false;
            QString reason;
        };

        // byteStore：构造/取得 ksword::memwb::MemoryIoByteStore（按需惰性构造，
        // 第一次调用 Commit 系列函数时若尚未构造且 ioPortFactory_ 已设置则构造）。
        ksword::memwb::MemoryIoByteStore* byteStore();

        // ensureTransaction：惰性构造 transaction_（需要 overlay_/target_/confirmation_/audit_/
        // byteStore() 均已就位）；缺任一依赖时返回 false，调用方应把这次操作当成失败
        // 处理（不弹框、只报错，装配顺序错误是实现缺陷不是用户可见的运行期状况）。
        bool ensureTransaction();

        // TryStagePendingEntry / FinalizePendingStage：原本是 Internal.h 里两个
        // 接收 controller 指针的 detail:: 自由函数（绕开访问权限去碰关联表），
        // 头文件解冻后改为私有成员方法，直接访问 overlay_/pendingStages_，定义在
        // WorkbenchWriteController.PendingStage.cpp。
        PendingStageAttempt TryStagePendingEntry(const PendingStageEntry& entry);
        void FinalizePendingStage(PendingStageTicket ticket, bool ok, const QString& reason);

    public:
        // FinalizePendingStageForTest（仅供夹具白盒测试，见
        // wpJ4_tests.PendingStage.cpp 的 M-P6）：直接转发到私有的
        // FinalizePendingStage，核对"票据不存在时是空操作"这条防御本身没有失效
        // ——beginPendingStage/cancelPendingStage/notifyWindowMayCover 三个公开入口
        // 调用它之前都已经各自确认过票据存在，这条防线在当前调用图里到不了，只能
        // 靠白盒测试验证（命名仿照既有的 ExtraCountForTest 惯例，一看就知道是测试
        // 专用、不是生产装配接口）。
        void FinalizePendingStageForTest(PendingStageTicket ticket, bool ok, const QString& reason)
        {
            FinalizePendingStage(ticket, ok, reason);
        }

    private:
        // overlay_ / target_ / confirmation_：非拥有依赖。
        ksword::memwb::MemoryDiffOverlay* overlay_ = nullptr;
        WorkbenchTarget* target_ = nullptr;
        ksword::memwb::IConfirmationSink* confirmation_ = nullptr;
        // 代次：原来这里有一个本类自己维护的"代次镶像"（localRevisions_），由
        // setTarget 订阅 sessionChanged 手动 BumpSource()、onEditCompleted/撤销
        // 重做成功手动 BumpContent()，用来过渡到 WorkbenchTarget::revisions() 真正
        // 接入之前的阶段。**主会话后续裁决**：revisions() 已经到位（wpI 加入，见
        // WorkbenchTarget.h 同名访问器），本类不再维护这份独立镶像，直接在
        // ensureTransaction() 惰性构造 transaction_/undo_ 时引用
        // target_->revisions() 这个活引用——它是 WorkbenchTarget 内部
        // MemoryTargetTracker 的两个代次计数器的唯一真身，Reload()/
        // NoteContentChanged()/身份变更都已经在那一侧原地递增，本类不需要也不应该
        // 再镶一份随时可能漂移的拷贝。D1/D2 保证 transaction_ 构造成功之后
        // target_ 不可能再换成不同对象（见 setTarget），所以这个引用在
        // transaction_/undo_ 整个生命周期内稳定指向同一份计数器，没有悬空风险。
        // ioPortFactory_ / kernelPortFactory_：端口工厂；port_/kernelPort_ 是本类拥有的
        // 长期对象，惰性构造。
        IoPortFactory ioPortFactory_;
        KernelPortFactory kernelPortFactory_;
        std::unique_ptr<ksword::memwb::IMemoryIoPort> port_;
        std::unique_ptr<ksword::memwb::IKernelMutationPort> kernelPort_;
        std::unique_ptr<ksword::memwb::MemoryIoByteStore> byteStore_;
        WriteValidationFn writeValidationCallback_;
        bool historyReplay_ = false;
        // transaction_：唯一的写事务，惰性构造（见 ensureTransaction），持有对
        // overlay_/target_->session()/target_->revisions()/byteStore_/
        // confirmation_/audit_ 的引用，这些引用必须比 transaction_ 活得更久。
        std::unique_ptr<ksword::memwb::MemoryWriteTransaction> transaction_;
        // audit_：非拥有的审计接收器（见 setAuditSink），生产路径是写项目日志的实现。
        ksword::memwb::IAuditSink* audit_ = nullptr;
        // rereadRangeCallback_ / canvasReadOnlyHook_ / commitSuspendHook_ /
        // tickProvider_：四个装配回调。
        RereadRangeFn rereadRangeCallback_;
        CanvasReadOnlyHook canvasReadOnlyHook_;
        CommitSuspendHook commitSuspendHook_;
        TickProviderFn tickProvider_;
        // pendingStageCounter_：PendingStage 票据发生器。
        PendingStageTicket pendingStageCounter_ = 0;
        // pendingStages_（头文件解冻后新增的直接私有成员，取代 Internal.h 的
        // "以 this 指针为键的关联表"）：未解决的地址簿异步暂存票据表。
        std::map<PendingStageTicket, PendingStageEntry> pendingStages_;
        // pendingUiConfirmSuppressed_：setUiConfirmSuppressed 在 transaction_ 惰性
        // 构造之前缓存的值；ensureTransaction() 构造出 transaction_ 后立即据此调用
        // 一次 transaction_->SetUiConfirmSuppressed(...)；也是撤销/重做临时事务
        // 继承确认抑制状态的最终来源（经 uiConfirmSuppressed() 这个带 transaction_
        // 优先级的访问器）。
        bool pendingUiConfirmSuppressed_ = false;
        // undo_：撤销/重做协调器，完整类型见 .Undo.h 的 WorkbenchUndoCoordinator。
        std::unique_ptr<WorkbenchUndoCoordinator> undo_;
        // commitDepth_（D3/D4 修复新增）：detail::CommitReadOnlyGuard 的重入深度
        // 计数，覆盖"正常提交"（transaction_->OnEditCompleted()/Commit()/
        // ResolveModeSwitch()）与"撤销/重做的临时提交"（undo_->undo()/redo() 内部
        // 的 scratchTransaction.Commit()）两种情形——它们是两个独立的
        // MemoryWriteTransaction 对象，各自的 IsBusy() 互不知道对方，必须靠本类
        // 自己这一个计数器统一判断"现在是不是有任何 Commit 在跑"。0 表示空闲；
        // 只有 0→1/1→0 才触碰两个钩子；commitPendingNow/undo/redo/
        // resolveModeSwitch(ApplyThenSwitch) 动手之前都先检查它 > 0，> 0 就直接
        // 拒绝（返回 Busy），不会嵌套。
        int commitDepth_ = 0;
    };
}
