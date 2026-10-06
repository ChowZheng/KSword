// ============================================================
// WorkbenchWriteController.PendingStage.cpp
// 作用：
// - 地址簿"值"列编辑落在当前基线窗口外时的异步暂存票据系统：beginPendingStage/
//   cancelPendingStage/notifyWindowMayCover 三个公开方法，以及它们共用的"尝试一次
//   Stage"/"判定票据结果"两个私有成员方法（声明见 WorkbenchWriteController.h）。
// - 分工边界（任务书 wpJ4 明确要求写清楚）：移动窗口使之覆盖该地址是装配层
//   （MemoryWorkbenchView，订阅 WorkbenchBaselineFeeder::baselineRefreshed 或画布
//   contentChanged 后调用 notifyWindowMayCover）的职责；本类只管票据的创建、
//   2 秒超时、显式取消与"窗口可能已经移动，重试一次"，自己从不触碰
//   WorkbenchBaselineFeeder/WorkbenchPageProvider。
// - 头文件解冻后的变化：TryStagePendingEntry/FinalizePendingStage 原本是
//   Internal.h 里两个接收 controller 指针的 detail:: 自由函数（绕开访问权限去碰
//   "以 this 指针为键的关联表"），现在改为 WorkbenchWriteController 的私有成员
//   方法，直接访问 overlay_/pendingStages_，不再需要绕路，本文件也不再需要
//   #include Internal.h。
// ============================================================

#include "WorkbenchWriteController.h"

#include <QTimer>

namespace ks::ui
{
    // ------------------------------------------------------------
    // beginPendingStage：创建票据，立即尝试一次（地址恰好已经在当前窗口内时不必
    // 等待窗口移动），再挂一个 2 秒单次计时器兜底。
    // ------------------------------------------------------------
    WorkbenchWriteController::PendingStageTicket WorkbenchWriteController::beginPendingStage(
        std::uint64_t address, const QByteArray& bytes)
    {
        if (bytes.isEmpty())
        {
            // 没有字节：按"地址/字节非法"处理，不创建任何票据（头文件注释原文）。
            return 0;
        }
        const auto length = static_cast<std::uint64_t>(bytes.size());
        if (!ksword::memwb::MemoryDiffOverlay::RangeRepresentable(address, length))
        {
            // 地址 + 长度超过 uint64 上限：同上，不创建票据。
            return 0;
        }

        // 绑定发起时的目标轴；capture 的同步通知可能销毁控制器或目标，先用弱引用守卫。
        const QPointer<WorkbenchWriteController> self(this);
        const QPointer<WorkbenchTarget> target(target_);
        if (!target)
        {
            return 0;
        }
        const TargetCapture captured = target->capture();
        if (!self || !target || target_ != target.data())
        {
            return 0;
        }

        // pendingStageCounter_ 从 1 开始自增，0 永远是无效票据（头文件约定）。
        const PendingStageTicket ticket = ++pendingStageCounter_;

        PendingStageEntry entry;
        entry.address = address;
        entry.target = target;
        entry.sourceRevision = captured.rev.source;
        const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.constData());
        entry.bytes.assign(data, data + bytes.size());
        // 计时器 parent=this：本对象销毁时 Qt 会级联删除它，不需要在析构函数里
        // 逐个手动 delete；下面的 lambda 定义在成员函数体内，因此对 this 的私有
        // 成员/信号拥有和本函数一样的访问权限，即使它在之后的事件循环里才被调用。
        entry.timer = new QTimer(this);
        entry.timer->setSingleShot(true);
        QObject::connect(entry.timer, &QTimer::timeout, this,
            [this, ticket]()
            {
                // 2 秒内没有任何一次 Stage 尝试成功：票据超时，不再重试（头文件
                // 注释原文："不再重试"），显式调用 FinalizePendingStage 而不是
                // 让它悬在表里。
                FinalizePendingStage(ticket, false, QStringLiteral("timeout"));
            });

        pendingStages_.emplace(ticket, entry);
        entry.timer->start(2000);

        // 立即尝试一次：地址可能恰好已经落在当前基线窗口内，这样不需要白等
        // 2 秒才生效。
        const PendingStageAttempt attempt = TryStagePendingEntry(entry);
        if (!self)
        {
            return 0;
        }
        if (attempt.resolved)
        {
            FinalizePendingStage(ticket, attempt.ok, attempt.reason);
        }
        return ticket;
    }

    // ------------------------------------------------------------
    // cancelPendingStage：显式取消一个尚未解决的票据。票据不存在或已解决时是
    // 空操作（不发信号）；存在时按"有结果（显式取消）"的语义收尾，同样会发一次
    // pendingStageResolved（头文件信号注释列出的三种结果之一）。
    // ------------------------------------------------------------
    void WorkbenchWriteController::cancelPendingStage(PendingStageTicket ticket)
    {
        if (pendingStages_.find(ticket) == pendingStages_.end())
        {
            return; // 不存在或已经被解决过：空操作。
        }
        FinalizePendingStage(ticket, false, QStringLiteral("cancelled"));
    }

    // ------------------------------------------------------------
    // notifyWindowMayCover：装配层通知"基线窗口可能变了，重新试一次"。对每个仍
    // 未解决的票据重试一次 Stage，不要求调用方传任何范围——本类自己对每一张票据
    // 各试一次。
    // ------------------------------------------------------------
    void WorkbenchWriteController::notifyWindowMayCover()
    {
        const QPointer<WorkbenchWriteController> self(this);
        // 先整表拷贝一份快照再逐个尝试：FinalizePendingStage 会修改
        // pendingStages_（erase），在遍历原表的同时直接 erase 会使迭代器失效。
        const std::map<PendingStageTicket, PendingStageEntry> snapshot = pendingStages_;
        for (const auto& item : snapshot)
        {
            const PendingStageTicket ticket = item.first;
            const PendingStageEntry& entry = item.second;
            // 前一张票据的同步结果槽可能取消后面的票据；快照不再代表当前授权。
            if (pendingStages_.find(ticket) == pendingStages_.end())
            {
                continue;
            }
            const PendingStageAttempt attempt = TryStagePendingEntry(entry);
            // capture()/编辑通知可能同步销毁控制器，不能再进入收尾或下一轮。
            if (!self)
            {
                return;
            }
            if (attempt.resolved)
            {
                FinalizePendingStage(ticket, attempt.ok, attempt.reason);
                if (!self)
                {
                    return;
                }
            }
        }
    }

    // ------------------------------------------------------------
    // TryStagePendingEntry / FinalizePendingStage：私有成员方法，声明见
    // WorkbenchWriteController.h，被 beginPendingStage 与 notifyWindowMayCover
    // 共用。
    // ------------------------------------------------------------

    WorkbenchWriteController::PendingStageAttempt WorkbenchWriteController::TryStagePendingEntry(
        const PendingStageEntry& entry)
    {
        PendingStageAttempt attempt;
        // 窗口异步到达时重新核对来源；不把上一个 PID/通道的编辑应用到新会话。
        const QPointer<WorkbenchWriteController> self(this);
        const QPointer<WorkbenchTarget> target(entry.target);
        if (!target || target_ != target.data())
        {
            return PendingStageAttempt{true, false, QStringLiteral("cancelled")};
        }
        const TargetCapture current = target->capture();
        if (!self || !target || target_ != target.data() || current.rev.source != entry.sourceRevision)
        {
            return PendingStageAttempt{true, false, QStringLiteral("cancelled")};
        }
        if (overlay_ == nullptr)
        {
            // 没有叠加层可暂存：不可恢复，立即判定失败（不会因为之后装上 overlay
            // 就自动变好——票据生命周期里 overlay_ 不应该从空变为非空，这是防御）。
            attempt.resolved = true;
            attempt.ok = false;
            attempt.reason = QStringLiteral("no overlay");
            return attempt;
        }
        const ksword::memwb::StageStatus status = overlay_->Stage(entry.address, entry.bytes);
        switch (status)
        {
        case ksword::memwb::StageStatus::Ok:
            // 复用正常"一次编辑手势完成"的全部后续处理（内容代次 +1、立即模式下
            // 走 Commit、发 pendingPatchesChanged），不重新发明这条逻辑。
            onEditCompleted();
            attempt.resolved = true;
            attempt.ok = true;
            return attempt;
        case ksword::memwb::StageStatus::OutOfWindow:
        case ksword::memwb::StageStatus::UnreadBytes:
            // 窗口还没覆盖到这个地址：可恢复的失败，保持票据挂起，等下一次
            // notifyWindowMayCover 或超时。
            return attempt;
        case ksword::memwb::StageStatus::Empty:
        case ksword::memwb::StageStatus::AddressOverflow:
        case ksword::memwb::StageStatus::TooLarge:
        default:
            // 这三种拒绝原因与"窗口位置"无关，重试不会让结果变好：立即判定为
            // 不可恢复的失败。Empty/AddressOverflow 理论上在 beginPendingStage 的
            // 入口校验时已经挡掉，这里仍要处理是为了不对未来枚举新增值假装成功。
            attempt.resolved = true;
            attempt.ok = false;
            attempt.reason = QStringLiteral("stage rejected (%1)").arg(static_cast<int>(status));
            return attempt;
        }
    }

    void WorkbenchWriteController::FinalizePendingStage(
        PendingStageTicket ticket, bool ok, const QString& reason)
    {
        auto it = pendingStages_.find(ticket);
        if (it == pendingStages_.end())
        {
            // 票据不存在（例如超时计时器与显式取消几乎同时触发，先到的一方已经
            // 清理过）：不重复发信号，也不重复停计时器。
            return;
        }
        if (it->second.timer != nullptr)
        {
            it->second.timer->stop();
            // 立刻从 controller 的子对象列表摘除：这样即使 deleteLater() 的事件
            // 还没被处理，之后任何基于 findChildren/children() 的枚举（本类目前
            // 不这么做，纯防御）也不会再看到这个即将销毁的计时器。
            it->second.timer->setParent(nullptr);
            it->second.timer->deleteLater();
        }
        // 先把 it 失效化前需要的数据都用完，再 erase（erase 之后 it 不可再用）。
        pendingStages_.erase(it);
        // 发出结果信号：成功提交 / 超时 / 显式取消三种情形都从这里统一发出
        // （头文件信号注释列出的三种结果）。信号是 moc 生成的公开成员函数，这里
        // 直接调用等价于 emit。
        pendingStageResolved(ticket, ok, reason);
    }
}
