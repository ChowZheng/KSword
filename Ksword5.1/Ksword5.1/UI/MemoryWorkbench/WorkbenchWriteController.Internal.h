#pragma once

// ============================================================
// WorkbenchWriteController.Internal.h
// 作用：
// - WorkbenchWriteController.cpp / .PendingStage.cpp / .Undo.cpp 三个实现文件共用
//   的纯实现细节：字节互转、重入安全的只读/挂起 RAII 守卫、以及两个 W4 收尾用的
//   自由函数。
// - 本文件不是公开接口，不被 WorkbenchHexPane/MemoryWorkbenchView 包含；仅供本
//   类自己的三个 .cpp 文件 #include。命名风格仿照 WorkbenchWriteController.Undo.h
//   （卫星头文件拆分实现细节），不是 .inc 堆叠。
//
// ------------------------------------------------------------
// 历史变更（审核 review-wpJ4.md 之后，主会话裁决落实）
// ------------------------------------------------------------
// 本文件曾经包含一张"以 this 指针为键的文件级关联表"（ControllerExtraState /
// ExtraRegistry / Extra / ReleaseExtra / ExtraCountForTest），用来绕开
// WorkbenchWriteController.h 头文件冻结、不允许新增私有字段的限制，存放代次镶像
// 与 PendingStage 票据表这两项运行期状态。审核报告 D 节明确指出这是"本包最不
// 常规的实现手段"，有地址复用串用的风险；主会话裁决头文件解冻后，这两项状态已经
// 改为 WorkbenchWriteController 的直接私有成员（当时命名为 localRevisions_ /
// pendingStages_，见 WorkbenchWriteController.h），这张关联表与依赖它的
// TestExtraStateCleanedUpOnDestruction（M-L8）已经一并删除，不再需要。
//
// 第二轮独立审核（review2-wpJ4.md）之后，主会话进一步裁决：localRevisions_ 这份
// "代次镶像"本身也删除——WorkbenchTarget::revisions() 到位后，直接引用
// target_->revisions() 这个活引用即可（见 WorkbenchWriteController.cpp 的
// ensureTransaction），不需要本类再维护一份随时可能漂移的拷贝。pendingStages_/
// commitDepth_ 仍是直接私有成员，不受这次改动影响。
//
// 本文件现在只保留三类东西：
//   1) 字节/时间戳/身份串的纯转换函数（ToByteVector/ToByteArray/ComputeTick/
//      ComputeSessionIdentityKey）；
//   2) CommitReadOnlyGuard——D3/D4 修复后的重入安全只读/挂起守卫，depth 由调用方
//      （WorkbenchWriteController 自己的成员函数）以引用传入，本类不拥有它；
//   3) HandleCommitReport / EmitPendingPatchesChanged——W4 收尾的两个自由函数，
//      被 commitPendingNow/onEditCompleted/resolveModeSwitch/undo/redo 共用。
// ============================================================

#include "WorkbenchWriteController.h"
#include "WorkbenchWriteController.Undo.h"

#include "../../../../shared/evidence/memory_workbench/MemoryDiffOverlay.h"
#include "../../../../shared/evidence/memory_workbench/MemoryTargetSession.h"

#include <QByteArray>
#include <QString>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace ks::ui::detail
{
    // ToByteVector / ToByteArray：QByteArray（装配层/Qt 世界）与 std::vector<uint8_t>
    // （Core/标准库世界）之间的互转，纯拷贝，没有别的逻辑。
    inline std::vector<std::uint8_t> ToByteVector(const QByteArray& bytes)
    {
        const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.constData());
        return std::vector<std::uint8_t>(data, data + bytes.size());
    }

    inline QByteArray ToByteArray(const std::vector<std::uint8_t>& bytes)
    {
        return QByteArray(reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()));
    }

    // ComputeTick：取单调时间戳（毫秒）。provider 非空时转发给它；否则退回系统
    // 单调时钟（std::chrono::steady_clock），供 MemoryEditJournal 的合并窗口判断。
    inline std::uint64_t ComputeTick(const WorkbenchWriteController::TickProviderFn& provider)
    {
        if (provider)
        {
            return provider();
        }
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
    }

    // ComputeSessionIdentityKey：撤销协调器的临时 scratch overlay 需要一个
    // identityKey 字符串（见 WorkbenchWriteController.Undo.h 的 setIdentityKey）。
    // 这里用会话本身（基址/长度固定填 0，因为这个字符串只是"这一次撤销/重做提交"
    // 的身份标签，不对应任何真实窗口）生成，调用方法：换目标/换范围/换通道时
    // 重新调用一次。
    inline std::string ComputeSessionIdentityKey(const ksword::memwb::MemoryTargetSession& session)
    {
        return ksword::memwb::IdentityKey(session, 0, 0);
    }

    // ------------------------------------------------------------
    // CommitReadOnlyGuard（D3/D4 修复：重入深度计数版）
    // ------------------------------------------------------------
    // depth 是调用方（WorkbenchWriteController 自己的成员函数，唯一能访问私有
    // 成员 commitDepth_ 的地方）以引用传入的那一个计数器；本类不拥有它，只在
    // 构造/析构时对它做 ++/--。两个钩子（画布只读 / BaselineFeeder 挂起）各自
    // 用 const 引用传入，同样不拥有。
    //
    // 只有 0→1（最外层进入）才真正调用两个钩子的 true，只有 1→0（最外层退出）
    // 才调用 false；中间层级的嵌套 Commit——例如撤销/重做在外层确认框的回调里
    // 被触发（D4）、或地址簿经 beginPendingStage 间接触发的编辑在外层确认框还
    // 没返回时又完成一次（D3）——不会重复触碰钩子，也不会把画布/BaselineFeeder
    // 提前解锁。这是 D3/D4 的直接修复：旧版本每次调用都新建一个独立的、不知道
    // 外层是否还活着的 Guard 实例，嵌套的那个实例析构时无条件调用 hook(false)，
    // 把外层还没结束的只读状态提前撤销。
    //
    // 用 RAII 而不是 try/catch，是为了在确认接口（ConfirmUi/ConfirmApproval）
    // 抛异常时也能保证深度计数正确回退、钩子在最外层退出时仍会被调用，同时不
    // 吞掉异常——异常在 guard 析构之后继续向上抛。
    class CommitReadOnlyGuard
    {
    public:
        CommitReadOnlyGuard(
            int& depth,
            const WorkbenchWriteController::CanvasReadOnlyHook& canvasHook,
            const WorkbenchWriteController::CommitSuspendHook& suspendHook)
            : depth_(depth)
            , canvasHook_(canvasHook)
            , suspendHook_(suspendHook)
        {
            ++depth_;
            if (depth_ == 1)
            {
                if (canvasHook_)
                {
                    canvasHook_(true);
                }
                if (suspendHook_)
                {
                    suspendHook_(true);
                }
            }
        }

        ~CommitReadOnlyGuard()
        {
            --depth_;
            if (depth_ == 0)
            {
                if (canvasHook_)
                {
                    canvasHook_(false);
                }
                if (suspendHook_)
                {
                    suspendHook_(false);
                }
            }
        }

        CommitReadOnlyGuard(const CommitReadOnlyGuard&) = delete;
        CommitReadOnlyGuard& operator=(const CommitReadOnlyGuard&) = delete;

    private:
        int& depth_;
        const WorkbenchWriteController::CanvasReadOnlyHook& canvasHook_;
        const WorkbenchWriteController::CommitSuspendHook& suspendHook_;
    };

    // HandleCommitReport：Commit 系列调用（onEditCompleted 的立即分支、
    // commitPendingNow、resolveModeSwitch 的 ApplyThenSwitch 分支、以及 undo()/
    // redo() 成功回放之后的 W4 收尾）共用的收尾——发信号、按 needsReread 请求
    // 重读、对真正落地的块记账与局部重读。
    // preCommitBlocks 必须是调用 Commit 之前、从 overlay 取到的 DiffBlocks 快照
    // （按地址升序）；对 undo()/redo() 的调用场景，preCommitBlocks 是只有一个
    // 元素的向量，由调用方根据 UndoReplayResult 的 address/before/after 现场
    // 构造（撤销/重做从不触碰主 overlay，没有真正的"快照"可取）。
    // undo 参数为 nullptr 时跳过记账（journal->record 的调用）——undo()/redo()
    // 自己的回放不应该向 journal 再记一步，否则会不断生出新的可撤销步骤，见
    // WorkbenchWriteController.Undo.h 文件头的说明。定义在
    // WorkbenchWriteController.cpp。
    void HandleCommitReport(
        WorkbenchWriteController* controller,
        WorkbenchTarget* target,
        WorkbenchUndoCoordinator* undo,
        const WorkbenchWriteController::RereadRangeFn& rereadCallback,
        const ksword::memwb::CommitReport& report,
        const std::vector<ksword::memwb::DiffBlock>& preCommitBlocks);

    // EmitPendingPatchesChanged：按 overlay 当前的暂存状态发一次
    // pendingPatchesChanged；overlay 为空时发 (0, 0)。定义在
    // WorkbenchWriteController.cpp。
    void EmitPendingPatchesChanged(
        WorkbenchWriteController* controller,
        ksword::memwb::MemoryDiffOverlay* overlay);
}
