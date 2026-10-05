#pragma once

// ============================================================
// MemoryWriteTransaction.h
// 作用：
// - "把暂存的字节修改提交到目标"的状态机。"立即写入"与"暂存后统一应用"两种模式
//   共用同一条提交管线，区别只在触发时机：Immediate 模式下编辑完成即自动走一次完整
//   管线；StagedThenApply 模式下编辑只暂存，由调用方显式 Commit。
// - 本类自身不做任何真实 I/O：读、写、用户确认、审计全部经注入接口完成
//   （IByteStore / IConfirmationSink / IAuditSink），因此可以脱离界面完整测试。
//   Idle / Staged 状态下对 IByteStore 的写调用次数恒为 0（不变式 1）。
// - 纯 C++20 标准库实现，不依赖 Qt，不依赖 Win32。
//
// 对象所有权与调用方约定（重要）：
// - 本类持有 MemoryDiffOverlay 的非 const 引用，持有 MemoryTargetSession 与
//   SessionRevisions 的 const 引用：权威副本归 Dock，本类只读。
// - 本类**不会**递增任何代次。调用方（Dock）必须在每次编辑手势（Stage / 丢弃 / 撤销）
//   之后 BumpContent()，在每次换目标 / 重读之后 BumpSource()。写事务的陈旧判据
//   完全依赖这两个计数器与会话字段：调用方漏递增，确认窗口期间发生的变化就检测不到。
// - 调用方可以直接操作 overlay（例如丢弃一段补丁），本类每次 Commit 都重新向 overlay
//   取差异块，不依赖自己记的状态；只是这样做之后 CurrentState() 反映的只是经由本类
//   发生过的迁移。
// - 引用的对象必须比本类活得久。本类不可拷贝、不可移动。
//
// Commit() 的固定顺序（测试用调用顺序记录逐条证明）：
//   (a) 记审计"开始"；捕获会话快照与两个代次快照；向 overlay 取差异块，
//       没有差异块则返回 NoChange，不触碰 store。
//   (b) 会话 Validate 失败则返回 InvalidSession，不触碰 store。
//   (c) 状态 ConfirmPending。uiConfirmSuppressed 为假则调用 ConfirmUi，用户拒绝则
//       回到 Staged 并返回 UserCancelled；为真则跳过 ConfirmUi，但**仍然写审计**
//       （事件 UiConfirmSuppressed）。两种路径都恰好产生一条"确认"审计，
//       所以抑制时的审计条数不会少于未抑制时。
//   (d) 确认返回之后重新核对会话身份与两个代次，任何变化返回 Stale，不触碰 store。
//       对应旧代码里嵌套事件循环之后的那一刻：用户在对话框开着的时候可以切目标。
//       该核对**不受 uiConfirmSuppressed 影响**。
//   (e) 对每个差异块先 Read 当前真实字节：读失败、partial、或与块的 before 不一致，
//       整体中止并返回 TargetChanged。**所有块都复核通过之后才进入写阶段**，
//       所以复核失败时一个字节都不会写。
//   (f) 逐块 Write（状态 Writing）。后端回报 needsExplicitApproval 时调用
//       ConfirmApproval——这一步**不受 uiConfirmSuppressed 影响，永远会问**：
//         Deny          中止，返回 ApprovalDenied；
//         ThisBlockOnly 同意只对当前这一块有效，下一块再要求同意时重新询问；
//         RestOfBatch   同意持续到本次 Commit 结束，局部变量保存，不带到下一次 Commit。
//       同意标志只会附在"后端刚刚说需要同意"的那次重试上，不会预先附到别的写入上。
//       未识别的 ApprovalAnswer 取值按 Deny 处理（失败即拒绝）。
//   (g) 每块写完立即回读（状态 Verifying）：读失败、partial 或与 after 不一致即失败，
//       返回 VerifyMismatch，且不再写后面的块。
//   (h) 每块回读通过后立刻 overlay.AcceptWrite(块, 回读字节)；全部通过状态 Committed。
//       任一失败状态 Failed（写失败且后端回报 rolledBack、并且此前没有任何块留在目标上
//       时为 RolledBack），尚未写入的块（含失败的那一块）的暂存补丁**原样保留**，
//       不会因为失败丢掉用户输入；已写入并回读通过的块已被 AcceptWrite 吸收，
//       重试时不会再被当成"目标已变化"。
//   每个出口都会记"结束"审计；每次 ConfirmApproval 之后记一条审计。
//   例外：注入接口抛异常时只有"开始"没有"结束"——审计里出现落单的开始记录，
//   本身就是"这次提交异常终止"的信号。
//
// 状态迁移：
//   Idle ──Stage──> Staged ──Commit──> ConfirmPending ──> Writing <──> Verifying ──> Committed
//   ConfirmPending 的中止（用户取消 / Stale / TargetChanged）回到 Staged；
//   Writing / Verifying 的中止（ApprovalDenied / WriteFailed / VerifyMismatch）进入 Failed 或 RolledBack。
//   Failed / RolledBack / Committed 之后可以再 Stage 或再 Commit（重试）。
//   Stage 成功且 overlay 有补丁时进入 Staged；Stage 成功但全是空操作（补丁都消失了）时，
//   原为 Staged 则回到 Idle，其它状态保持不变。
//
// 不变式说明：
// - 重入保护：Commit 进行期间（含确认窗口里的嵌套事件循环）再次 Commit 返回 Busy，
//   不触碰任何状态，也不触碰 store。SetMode / ResolveModeSwitch 同样返回 Busy。
// - 异常安全：确认接口、store 或审计接口抛异常时，Commit 清掉忙标志、把进行中的状态
//   置为 Failed，然后原样重新抛出，之后可以重试。
// - TargetChanged 同时涵盖"确认变了"与"无法确认没变"（复核读失败）：两者对用户的
//   处置相同——重新读取。
// - bytesWritten 是"写入后仍留在目标上的字节数"：包含回读未通过的块与部分写入的块，
//   不包含被后端回滚掉的字节。blocksWritten 只数"写入且回读通过"的块。
// - scratchAreaDirty / readModifyWriteWindow 取所有 Read / Write 结果的逻辑或，
//   成功不会清掉它们。
// - 回滚标签：RolledBack 表示"目标没有被这次提交改动"。若更早的块已经写入并回读通过，
//   即使后面失败的块被后端回滚，状态也是 Failed，要看 blocksWritten 才知道有多少块已落地。
// ============================================================

#include "MemoryDiffOverlay.h"
#include "MemoryTargetSession.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ksword::memwb
{
    // AccessResult：IByteStore 一次读或写的结果。默认值是"失败、没读到任何东西"。
    struct AccessResult
    {
        // ok：这次访问是否成功。需要显式同意时为 false。
        bool ok = false;
        // partial：只完成了一部分（部分读 / 部分写）。ok 为 true 时 partial 也可能为 true，
        // 本类一律按失败处理。
        bool partial = false;
        // data：Read 读到的字节（Write 忽略）。
        std::vector<std::uint8_t> data;
        // bytesDone：实际完成的字节数（Read / Write 都用）。
        std::uint64_t bytesDone = 0;
        // needsExplicitApproval：后端要求用户显式同意后，带同意标志重试同一次写入。
        // 此时后端必须尚未写入任何字节。
        bool needsExplicitApproval = false;
        // rolledBack：后端已把这次失败的写入回滚（目标恢复到写之前）。
        bool rolledBack = false;
        // scratchAreaDirty：DDMA 暂存区被弄脏，必须提示用户。成功时也可能为 true。
        bool scratchAreaDirty = false;
        // readModifyWriteWindow：本次访问走了"读-改-写"窗口，期间目标可能被别人改动。
        bool readModifyWriteWindow = false;
        // failureText：失败原因，原样进入 CommitReport 供界面展示。
        std::string failureText;
    };

    // IByteStore：真实目标内存的读写接口，由调用方注入。本类只通过它碰目标。
    class IByteStore
    {
    public:
        virtual ~IByteStore() = default;

        // Read：读 [address, address + length)。
        // 传出：AccessResult，ok 且未 partial 时 data 等于 length 个字节。
        virtual AccessResult Read(std::uint64_t address, std::uint64_t length) = 0;

        // Write：写 bytes 到 address。
        // 传入：explicitApproval 为 true 表示用户已对这一次写入显式同意。
        // 传出：AccessResult。后端要求同意而 explicitApproval 为假时，返回
        //       needsExplicitApproval = true 且不得写入任何字节。
        virtual AccessResult Write(
            std::uint64_t address,
            const std::vector<std::uint8_t>& bytes,
            bool explicitApproval) = 0;
    };

    // ApprovalAnswer：用户对"后端要求显式同意"的回答。
    enum class ApprovalAnswer
    {
        // Deny：不同意，中止整次提交。
        Deny,
        // ThisBlockOnly：只对当前这一块同意。
        ThisBlockOnly,
        // RestOfBatch：对本次提交里剩下的块都同意（到本次 Commit 结束为止）。
        RestOfBatch
    };

    // UiBlockInfo：确认请求里一个块的摘要。
    struct UiBlockInfo
    {
        // address：块起始绝对地址。
        std::uint64_t address = 0;
        // length：块字节数。
        std::uint64_t length = 0;
    };

    // UiConfirmRequest：普通界面确认（可被设置抑制）的内容。
    struct UiConfirmRequest
    {
        // targetIdentity：目标身份串（IdentityKey），让对话框能写明"写到哪里"。
        std::string targetIdentity;
        // blocksTotal：要写的块数。
        std::uint64_t blocksTotal = 0;
        // bytesTotal：要写的总字节数。
        std::uint64_t bytesTotal = 0;
        // blocks：各块摘要，按地址升序。
        std::vector<UiBlockInfo> blocks;
    };

    // ApprovalRequest：协议级显式同意（不可被设置抑制）的内容。
    struct ApprovalRequest
    {
        // targetIdentity：目标身份串。
        std::string targetIdentity;
        // blockIndex：当前块在本次提交里的序号（从 0 起）。
        std::uint64_t blockIndex = 0;
        // blocksTotal：本次提交的总块数。
        std::uint64_t blocksTotal = 0;
        // address：当前块起始地址。
        std::uint64_t address = 0;
        // length：当前块字节数。
        std::uint64_t length = 0;
        // backendText：后端要求同意时附带的说明。
        std::string backendText;
    };

    // CommitOutcome：一次 Commit 的结果。默认 NoChange 是"什么都没发生"，最安全的初值。
    enum class CommitOutcome
    {
        // NoChange：没有暂存补丁，什么都没做。
        NoChange,
        // Committed：全部块已写入并回读通过。
        Committed,
        // UserCancelled：用户在界面确认里拒绝。
        UserCancelled,
        // Stale：确认期间会话身份或某个代次变了。
        Stale,
        // TargetChanged：写前复核发现目标当前字节与暂存时的原字节不一致（或读不出来）。
        TargetChanged,
        // ApprovalDenied：用户拒绝了后端要求的显式同意。
        ApprovalDenied,
        // WriteFailed：某一块写入失败（含部分写入、同意后后端仍要求同意）。
        WriteFailed,
        // VerifyMismatch：某一块写入后回读与预期不一致。
        VerifyMismatch,
        // InvalidSession：会话自身不自洽（Validate 失败）。
        InvalidSession,
        // Busy：已经有一次 Commit 在进行（重入），本次什么都没做。
        Busy
    };

    // AuditEvent：审计事件种类。
    enum class AuditEvent
    {
        // CommitStarted：Commit 开始，每次调用都有，含 NoChange 与 Busy。
        CommitStarted,
        // UiConfirmAccepted：用户在界面确认里同意。
        UiConfirmAccepted,
        // UiConfirmDenied：用户在界面确认里拒绝。
        UiConfirmDenied,
        // UiConfirmSuppressed：界面确认已被设置抑制（确认已被设置抑制，但审计不省）。
        UiConfirmSuppressed,
        // ApprovalAnswered：一次显式同意的询问及其回答，每次 ConfirmApproval 一条。
        ApprovalAnswered,
        // CommitFinished：Commit 结束，每个出口都有，含结果与计数。
        CommitFinished
    };

    // AuditRecord：一条审计记录。不同事件只填用得到的字段，其余保持初值。
    struct AuditRecord
    {
        // event：事件种类。
        AuditEvent event = AuditEvent::CommitStarted;
        // targetIdentity：目标身份串（会话 + overlay 窗口基址与长度）。
        std::string targetIdentity;
        // blocksTotal：本次提交的总块数（CommitStarted 时可能尚未取到，为 0）。
        std::uint64_t blocksTotal = 0;
        // blockIndex / address / length / approvalAnswer：仅 ApprovalAnswered 使用。
        std::uint64_t blockIndex = 0;
        std::uint64_t address = 0;
        std::uint64_t length = 0;
        // approvalAnswer：用户的回答，默认 Deny。
        ApprovalAnswer approvalAnswer = ApprovalAnswer::Deny;
        // outcome / blocksWritten / bytesWritten / scratchAreaDirty / readModifyWriteWindow：
        // 仅 CommitFinished 使用。
        CommitOutcome outcome = CommitOutcome::NoChange;
        std::uint64_t blocksWritten = 0;
        std::uint64_t bytesWritten = 0;
        bool scratchAreaDirty = false;
        bool readModifyWriteWindow = false;
        // text：补充说明（英文，供界面层本地化）。
        std::string text;
    };

    // IConfirmationSink：用户确认接口。ConfirmUi 可被设置抑制，ConfirmApproval 不可。
    class IConfirmationSink
    {
    public:
        virtual ~IConfirmationSink() = default;

        // ConfirmUi：普通界面确认。传出：true 表示用户同意提交。
        virtual bool ConfirmUi(const UiConfirmRequest& request) = 0;

        // ConfirmApproval：后端要求的显式同意。传出：用户的回答。
        virtual ApprovalAnswer ConfirmApproval(const ApprovalRequest& request) = 0;
    };

    // IAuditSink：审计接口。
    class IAuditSink
    {
    public:
        virtual ~IAuditSink() = default;

        // Record：记录一条审计。
        virtual void Record(const AuditRecord& record) = 0;
    };

    // WriteMode：写入模式。
    enum class WriteMode
    {
        // Immediate：编辑完成即自动提交（默认）。
        Immediate,
        // StagedThenApply：编辑只暂存，由调用方显式 Commit。
        StagedThenApply
    };

    // ModeSwitchDecision：存在未提交补丁时切换模式，用户在三选一对话框里的决定。
    enum class ModeSwitchDecision
    {
        // ApplyThenSwitch：先提交补丁，成功后切换；提交失败则不切换。
        ApplyThenSwitch,
        // DiscardThenSwitch：丢弃补丁后切换。
        DiscardThenSwitch,
        // Cancel：什么都不改。
        Cancel
    };

    // ModeSwitchStatus：SetMode / ResolveModeSwitch 的结果。
    enum class ModeSwitchStatus
    {
        // Switched：模式已切换到目标模式（目标等于当前模式时也是它，什么都没变）。
        Switched,
        // NeedsDecision：存在未提交补丁，模式没变，请调用方弹三选一后调用 ResolveModeSwitch。
        NeedsDecision,
        // Cancelled：用户取消，模式没变。
        Cancelled,
        // ApplyFailed：选了 ApplyThenSwitch 但提交没有成功，模式没变，补丁按失败规则保留。
        ApplyFailed,
        // NoPendingSwitch：没有待决的切换请求，什么都没做。
        NoPendingSwitch,
        // Busy：有 Commit 正在进行，什么都没做。
        Busy
    };

    // CommitReport：一次 Commit 的完整报告。默认值是"什么都没发生"。
    struct CommitReport
    {
        // outcome：结果。
        CommitOutcome outcome = CommitOutcome::NoChange;
        // blocksTotal：开始时 overlay 里的差异块数。
        std::uint64_t blocksTotal = 0;
        // blocksWritten：写入且回读通过的块数。
        std::uint64_t blocksWritten = 0;
        // bytesWritten：写入后仍留在目标上的字节数（含回读未通过的块与部分写入）。
        std::uint64_t bytesWritten = 0;
        // failureText：非 Committed / NoChange 时的失败原因（英文，供界面层本地化）。
        std::string failureText;
        // scratchAreaDirty：任一次访问报告过暂存区被弄脏；成功时也保持为真。
        bool scratchAreaDirty = false;
        // readModifyWriteWindow：任一次访问报告过读-改-写窗口；成功时也保持为真。
        bool readModifyWriteWindow = false;
        // approvalsAsked：ConfirmApproval 被调用的次数。
        std::uint64_t approvalsAsked = 0;
        // needsReread：界面应当重新读取：目标可能已被改动，或叠加层对应的基线已经陈旧。
        bool needsReread = false;
        // sessionError：outcome 为 InvalidSession 时的具体违规，其余为 None。
        SessionError sessionError = SessionError::None;
    };

    // ModeSwitchResult：ResolveModeSwitch 的返回。
    struct ModeSwitchResult
    {
        // status：切换结果。
        ModeSwitchStatus status = ModeSwitchStatus::Busy;
        // commitReport：选了 ApplyThenSwitch 且确实执行了提交时携带那次提交的报告，其余为 nullopt。
        std::optional<CommitReport> commitReport;
    };

    // MemoryWriteTransaction：写事务状态机。不是线程安全的，调用方在同一线程使用。
    class MemoryWriteTransaction
    {
    public:
        // State：事务状态。
        enum class State
        {
            Idle,
            Staged,
            ConfirmPending,
            Writing,
            Verifying,
            Committed,
            Failed,
            RolledBack
        };

        // 构造。
        // 传入：overlay 暂存叠加层；session 目标会话（Dock 持权威，本类只读）；
        //       revisions 双代次计数器（本类只读）；store 真实读写接口；
        //       confirmation 用户确认接口；audit 审计接口；initialMode 初始写入模式。
        // 所有引用必须比本对象活得久。初始状态 Idle，界面确认不抑制。
        MemoryWriteTransaction(
            MemoryDiffOverlay& overlay,
            const MemoryTargetSession& session,
            const SessionRevisions& revisions,
            IByteStore& store,
            IConfirmationSink& confirmation,
            IAuditSink& audit,
            WriteMode initialMode = WriteMode::Immediate);

        // 持有引用，不可拷贝不可移动。
        MemoryWriteTransaction(const MemoryWriteTransaction&) = delete;
        MemoryWriteTransaction& operator=(const MemoryWriteTransaction&) = delete;

        // Stage：把一次编辑转给 overlay 暂存，不触碰 store。
        // 传入：address 起始绝对地址；bytes 想写入的字节。
        // 传出：overlay 的暂存结果。成功且 overlay 有补丁时进入 Staged（提交进行中不改状态）。
        // 调用方须在成功后 BumpContent()。
        StageStatus Stage(std::uint64_t address, const std::vector<std::uint8_t>& bytes);

        // OnEditCompleted：一次编辑手势完成时调用。
        // 传出：Immediate 模式返回自动 Commit() 的报告；StagedThenApply 模式什么也不做，返回 nullopt。
        [[nodiscard]] std::optional<CommitReport> OnEditCompleted();

        // SetMode：请求切换写入模式。
        // 传出：目标等于当前模式或没有未提交补丁时直接切换（Switched）；
        //       有补丁时不切换并记下待决请求（NeedsDecision）；提交进行中返回 Busy。
        [[nodiscard]] ModeSwitchStatus SetMode(WriteMode newMode);

        // ResolveModeSwitch：用户对 NeedsDecision 作出决定后调用。
        // 传出：见 ModeSwitchStatus。没有待决请求返回 NoPendingSwitch，且**不执行任何决定**
        //       （包括 DiscardThenSwitch 不会丢补丁）。未识别的决定值按 Cancel 处理。
        //       无论结果如何，待决请求在本次调用后被清掉（Busy 与 NoPendingSwitch 除外）。
        [[nodiscard]] ModeSwitchResult ResolveModeSwitch(ModeSwitchDecision decision);

        // PendingOverlayGuard：从 from 切到 to 是否必须先让用户决定。
        // 传出：from 与 to 不同且 overlay 有未提交补丁时为 true。
        bool PendingOverlayGuard(WriteMode from, WriteMode to) const;

        // Commit：把 overlay 里全部暂存补丁提交到目标，顺序见文件头。
        // 传出：CommitReport。失败时不会丢掉未写入块的暂存补丁。
        [[nodiscard]] CommitReport Commit();

        // CurrentState：当前状态。
        State CurrentState() const;

        // Mode：当前写入模式。
        WriteMode Mode() const;

        // PendingModeSwitch：待决的切换目标；没有则为 nullopt。
        std::optional<WriteMode> PendingModeSwitch() const;

        // IsBusy：是否有 Commit 正在进行。
        bool IsBusy() const;

        // SetUiConfirmSuppressed：设置是否抑制普通界面确认。只影响 ConfirmUi，
        // 不影响显式同意询问、审计与各项复核。
        void SetUiConfirmSuppressed(bool suppressed);

        // UiConfirmSuppressed：当前是否抑制界面确认。
        bool UiConfirmSuppressed() const;

    private:
        // CommitRun：一次 Commit 的全部局部状态，定义在 MemoryWriteTransaction.Commit.cpp。
        struct CommitRun;

        // 管线的各阶段，返回 false 表示已填好 run.report 并中止。
        bool ConfirmWithUser(CommitRun& run);
        bool RecheckFreshness(CommitRun& run);
        bool PrecheckTargets(CommitRun& run);
        bool WriteAndVerifyAll(CommitRun& run);
        bool WriteOneBlock(CommitRun& run, std::size_t index);
        bool VerifyOneBlock(CommitRun& run, std::size_t index);

        // 运行 (a) 到 (h) 的整条管线。
        void RunPipeline(CommitRun& run);

        // 中止并记录：填结果与失败原因，必要时把状态放回 Staged。
        void AbortToStaged(CommitRun& run, CommitOutcome outcome, const std::string& text);

        // 写阶段失败：填结果，状态置 Failed（或 RolledBack）。
        void AbortWithFailure(CommitRun& run, CommitOutcome outcome, const std::string& text, bool rolledBack);

        // 审计辅助。
        void RecordStarted(const CommitRun& run);
        void RecordConfirm(const CommitRun& run, AuditEvent event, const std::string& text);
        void RecordApproval(
            const CommitRun& run,
            std::size_t index,
            const DiffBlock& block,
            ApprovalAnswer answer);
        void RecordFinished(const CommitRun& run);

        // overlay：暂存叠加层。
        MemoryDiffOverlay& overlay_;
        // session_：目标会话（Dock 持权威，只读）。
        const MemoryTargetSession& session_;
        // revisions_：双代次计数器（只读）。
        const SessionRevisions& revisions_;
        // store_：真实目标读写接口。
        IByteStore& store_;
        // confirmation_：用户确认接口。
        IConfirmationSink& confirmation_;
        // audit_：审计接口。
        IAuditSink& audit_;
        // state_：当前状态。
        State state_ = State::Idle;
        // mode_：当前写入模式。
        WriteMode mode_ = WriteMode::Immediate;
        // pendingMode_：待决的切换目标。
        std::optional<WriteMode> pendingMode_;
        // busy_：是否有 Commit 在进行（重入保护）。
        bool busy_ = false;
        // uiConfirmSuppressed_：是否抑制普通界面确认。
        bool uiConfirmSuppressed_ = false;
    };
}
