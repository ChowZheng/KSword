#pragma once

// ============================================================
// WorkbenchWriteController.Undo.h
// 作用：
// - WorkbenchWriteController 的撤销/重做实现细节（卫星头文件，不是第七个装配层
//   类）：定义 WorkbenchUndoCoordinator 的完整类型，供
//   WorkbenchWriteController.Undo.cpp 实现，也供装配接口文档与审核者核对撤销语义，
//   而不必把这些细节塞进主头文件（WorkbenchWriteController.h 只前置声明它并以
//   std::unique_ptr 持有，即 pimpl）。
// - 本文件只被 WorkbenchWriteController.cpp / WorkbenchWriteController.Undo.cpp
//   包含，不是公开给 WorkbenchHexPane/MemoryWorkbenchView 的接口；它们只应调用
//   WorkbenchWriteController.h 暴露的 undo()/redo()/canUndo()/canRedo() 四个方法。
//
// ------------------------------------------------------------
// 为什么撤销要绕开 MemoryDiffOverlay 的基线窗口（已解决的设计问题，非待定）
// ------------------------------------------------------------
// ux.md §6 写"立即写入：Ctrl+Z = Stage(before) + Commit()"，但如果直接 Stage 进主
// overlay_（WorkbenchHexPane 持有、随视口跟随移动的那一份），撤销的地址很可能已经
// 滚出了当前基线窗口——这种情况下 Stage 会因 OutOfWindow 被拒绝，用户滚动过的撤销
// 历史就撤不回去了。
//
// 本类的做法：每次 Undo()/Redo() 都新建一个**局部**的、只覆盖这一个块的临时
// MemoryDiffOverlay（栈上对象，调用结束即销毁，不是本类的长期成员），依次：
//   1. journal.PeekUndo()/PeekRedo() 取得 JournalReplay{address, expectedCurrent,
//      restore}；
//   2. scratch.LoadBaseline(identityKey, replay.address, replay.expectedCurrent,
//      全 1 掩码)——用"期望的当前字节"当基线，不需要真的去读目标；
//   3. scratch.Stage(replay.address, replay.restore)——在这个只有一块大小的临时
//      窗口里，Stage 必然成功（范围当然"在窗口内"，字节当然"已经读到"）；
//   4. 用 (scratch, 真实 session, 真实 revisions, 真实 store, 真实 confirmation,
//      真实 audit) 构造一个临时 MemoryWriteTransaction 并调用一次 Commit()。
// 这样完整复用了 Commit() 本身的八步管线（确认、Stale 复核、写前核对当前字节是否
// 等于 before——这一步天然涵盖了 MemoryEditJournal.h 文档里要求的 CheckReplay 语义，
// 二者不是重复校验：Commit 的 (e) 步骤读的是**此刻**目标的真实字节，CheckReplay
// 只是调用方自己想提前判断时的轻量版本，本类选择直接依赖 Commit 的 (e) 步而不是
// 自己先调一次 CheckReplay 再调 Commit，避免两套判断互相漂移）、写入、回读、
// AcceptWrite、审计，没有重新发明任何一步。
// 代价：Undo/Redo 永远是"一次新的提交"（与 ux.md"明确是一次新的写入"一致），且
// 完全不依赖当前可见窗口；即使用户已经跳转到另一个地址，Ctrl+Z 仍然可以成功。
// 仅当目标字节已经不等于 before（Commit 的 (e) 步会发现）时，Commit 返回
// TargetChanged，本类据此整步拒绝、不调用 MarkUndone()，与 MemoryEditJournal.h
// 文档"目标已被其他程序改动，无法自动撤销"的描述完全对应。
// ============================================================

#include "WorkbenchWriteController.h"

#include "../../../../shared/evidence/memory_workbench/MemoryDiffOverlay.h"
#include "../../../../shared/evidence/memory_workbench/MemoryEditJournal.h"
#include "../../../../shared/evidence/memory_workbench/MemoryIoByteStore.h"
#include "../../../../shared/evidence/memory_workbench/MemoryTargetSession.h"
#include "../../../../shared/evidence/memory_workbench/MemoryWriteTransaction.h"

#include <QString>

#include <cstdint>
#include <functional>
#include <string>

namespace ks::ui
{
    // UndoReplayOutcome：一次 Undo()/Redo() 的结果，供装配层（状态条）展示文案。
    enum class UndoReplayOutcome
    {
        // NothingToReplay：journal 里没有可撤销/可重做的步骤。
        NothingToReplay,
        // Replayed：已经补写成功（对应 CommitOutcome::Committed）。
        Replayed,
        // TargetChanged：目标当前字节与 journal 记录的"写入前应有的字节"不符，整步
        // 拒绝，journal 不移动游标——与 MemoryEditJournal.h 要求的协议一致。
        TargetChanged,
        // CommitRejected：Commit 返回了 Committed/TargetChanged 之外的其它结果
        // （UserCancelled/Stale/ApprovalDenied/WriteFailed/VerifyMismatch/
        // InvalidSession/Busy），统一归为这一类，具体原因见 lastFailureText()。
        CommitRejected,
    };

    // UndoReplayResult（COMMON 裁决第 3 项新增）：undo()/redo() 的完整返回——不再
    // 只是一个结果枚举，连带回放的地址/字节与完整 CommitReport 一起带出，供
    // WorkbenchWriteController 做 W4 收尾（局部重读、needsReread/scratchAreaDirty
    // 转发、commitFinished/commitFailed）。旧版本只返回 UndoReplayOutcome，外层
    // 完全无法对撤销/重做产生的那次写入执行任何收尾，是 impl-wpJ4.md 报告记录的
    // 接口缺口 #3，现按裁决落实。
    struct UndoReplayResult
    {
        // outcome：见 UndoReplayOutcome。
        UndoReplayOutcome outcome = UndoReplayOutcome::NothingToReplay;
        // address：本次回放写入的起始绝对地址；outcome 非 Replayed 时无意义
        // （恒为 0，调用方不应读取）。
        std::uint64_t address = 0;
        // before / after：本次回放写入前后的字节（分别对应 JournalReplay::
        // expectedCurrent / restore，长度相等即块长度）；outcome 非 Replayed 时为
        // 空。连同 address 一起就是"地址/长度"（长度=before.size()）。
        ksword::memwb::JournalBytes before;
        ksword::memwb::JournalBytes after;
        // report：临时 scratchTransaction.Commit() 的完整报告（含 failureText）；
        // outcome 非 Replayed 时是默认构造的占位值（outcome=NoChange），调用方
        // 应该用 lastFailureText() 或本结构体自己的 outcome 字段判断失败原因，
        // 不要去读这个占位 report。
        ksword::memwb::CommitReport report;
    };

    // WorkbenchUndoCoordinator：撤销/重做协调器的完整定义，补上主头文件里仅作
    // 前置声明的类型。本类不是公开接口的一部分、也不是六个装配层类之一——装配层
    // 只应调用 WorkbenchWriteController.h 暴露的 undo()/redo()/canUndo()/canRedo()。
    // WorkbenchWriteController 以组合方式持有它一份（std::unique_ptr），并把自己
    // 构造时拿到的四个长期依赖（session、revisions、store、confirmation、audit）
    // 原样转交给它——它们的生命周期约定与 WorkbenchWriteController 一致，不单独管理。
    // 不嵌套进 WorkbenchWriteController 的原因：嵌套类型若声明在 private 区段，
    // 在本卫星头文件里以 `class Outer::Inner` 形式在类外补充定义会触犯访问权限
    // （private 嵌套类型的外部补充定义同样受访问控制），放在命名空间作用域可以
    // 干净地避开这个问题，同时仍然保持"只是 WorkbenchWriteController 的实现细节"
    // 的实际语义。
    class WorkbenchUndoCoordinator final
    {
    public:
        // 构造：session/revisions/store/confirmation/audit 均为非拥有引用/指针，
        // 必须比本对象活得更久（与 MemoryWriteTransaction 自身的生命周期约定一致，
        // 见 MemoryWriteTransaction.h）；tickProvider 用于合并窗口判断，与
        // WorkbenchWriteController 共用同一个来源。
        // suppressedProvider（COMMON 裁决第 3 项新增）：每次 replayOnce 真正构造
        // 临时 scratchTransaction 之后，用它取一次"控制器当前的 uiConfirmSuppressed"
        // 并原样设置到那个临时事务上——撤销/重做与正常提交共用同一条"确认/审计/
        // 回读"链，策略判定"这个范围/通道不弹确认"时撤销也不应该额外弹一次（ux
        // 表：撤销是"同一条确认/审计/回读链"，进程范围立即写不弹确认）。为空
        // （未设置）时等价于永远返回 false（不抑制），与旧版本行为一致。
        WorkbenchUndoCoordinator(
            const ksword::memwb::MemoryTargetSession& session,
            const ksword::memwb::SessionRevisions& revisions,
            ksword::memwb::IByteStore& store,
            ksword::memwb::IConfirmationSink& confirmation,
            ksword::memwb::IAuditSink& audit,
            std::function<std::uint64_t()> tickProvider,
            std::function<bool()> suppressedProvider);

        // bindTarget：目标变化（身份类变更）时调用；转发
        // MemoryEditJournal::BindTarget，身份不同则清空历史。
        // 传出：true 表示清空了非空历史（装配层应提示一次"换了目标，撤销历史已清空"）。
        bool bindTarget(const ksword::memwb::MemoryTargetSession& session);

        // setIdentityKey：换目标/换范围/换通道时同步更新——临时 scratch overlay 的
        // LoadBaseline 需要一个与会话规则一致的 identityKey；取值规则与
        // WorkbenchBaselineFeeder::setIdentityKey 相同（均来自同一会话推导）。
        void setIdentityKey(std::string identityKey);

        // record：一次成功提交（含正常 Commit 与本协调器自己的 Undo/Redo 提交——但
        // 本类的 Undo/Redo 实现**不会**调用 record，见 ux.md"取消旧写入后清空历史"
        // 一条的反面：撤销/重做本身不记新的一步，只移动游标，否则会不断生出新的
        // 可撤销步骤）之后，由 WorkbenchWriteController 调用，把 CommitReport 对应
        // 的差异块前后字节记进 journal。
        ksword::memwb::JournalRecordResult record(
            std::uint64_t address,
            const ksword::memwb::JournalBytes& before,
            const ksword::memwb::JournalBytes& after);

        // canUndo / canRedo：转发 journal 查询。
        bool canUndo() const;
        bool canRedo() const;

        // undo / redo：执行一次撤销/重做，算法见文件头。**COMMON 裁决第 3 项**：
        // 返回值从单纯的结果枚举改为 UndoReplayResult（含回放地址/字节/完整
        // CommitReport），供外层 WorkbenchWriteController 做 W4 收尾；
        // lastFailureText() 仍然保留，给出非 Replayed 时的补充说明（英文细节串，
        // 装配层本地化），与 UndoReplayResult::report.failureText 的内容一致，
        // 只是多一份不需要先解构结构体就能读的便捷访问器。
        UndoReplayResult undo();
        UndoReplayResult redo();

        // lastFailureText：最近一次 undo()/redo() 非 Replayed 时的原因细节串。
        const std::string& lastFailureText() const noexcept;

    private:
        // replayOnce：undo()/redo() 共用的实现，isUndo 区分取 PeekUndo 还是
        // PeekRedo、成功后调用 MarkUndone 还是 MarkRedone。
        UndoReplayResult replayOnce(bool isUndo);

        const ksword::memwb::MemoryTargetSession& session_;
        const ksword::memwb::SessionRevisions& revisions_;
        ksword::memwb::IByteStore& store_;
        ksword::memwb::IConfirmationSink& confirmation_;
        ksword::memwb::IAuditSink& audit_;
        std::function<std::uint64_t()> tickProvider_;
        // suppressedProvider_：见构造函数参数说明，每次 replayOnce 取一次当前值。
        std::function<bool()> suppressedProvider_;
        ksword::memwb::MemoryEditJournal journal_;
        std::string identityKey_;
        std::string lastFailureText_;
    };
}
