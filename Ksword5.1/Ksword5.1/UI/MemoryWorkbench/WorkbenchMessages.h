#pragma once

// ============================================================
// WorkbenchMessages.h
// 作用：
// - 内存工作台会话条/状态条/确认框/动作/字符串写入对话框用到的全部中文可见文案
//   的唯一入口。CommitOutcome、GateReason（通道不可用原因）、ExprError、Issue、
//   PageState（旧称“IoReadStatus”，实际枚举名见 MemoryPageReader.h）等枚举到
//   文案的翻译都只在本文件出现一次，其余 WP-G 文件一律调用这里的函数，不自己
//   拼接这些枚举对应的句子。
// - 本文件只做"枚举/结构体 -> QString"的纯文本组装，不持有任何状态，不做 I/O，
//   因此可以被夹具直接调用做断言。
// - “旧读取失败诊断”迁移位：ExplainReadFailure 是旧 ViewBreakpointUtil.cpp:149-217
//   那段诊断文案的壳——判断逻辑（VirtualQueryEx、同名进程数、低 64 KB 保护区）仍
//   由上层（读取通道/端口层，归其它工作包）算好后填进 ReadFailureContext，这里只管
//   把算好的事实拼成一句完整说明。
// ============================================================

#include <QString>

#include "../../../../shared/evidence/memory_workbench/MemoryAddressExpr.h"
#include "../../../../shared/evidence/memory_workbench/MemoryChannelGate.h"
#include "../../../../shared/evidence/memory_workbench/MemoryIoPort.h"
#include "../../../../shared/evidence/memory_workbench/MemoryPageReader.h"
#include "../../../../shared/evidence/memory_workbench/MemoryTargetSession.h"
#include "../../../../shared/evidence/memory_workbench/MemoryWriteTransaction.h"
#include "../../../../shared/evidence/memory_workbench/SessionAddressResolver.h"

namespace ks::ui::workbench_messages
{
    // ReadFailureContext：组装"读取失败诊断"整段说明所需的全部已知事实。
    // 每个字段的计算方都不是本文件：regionSummary/regionIsFree/sameNameProcessCount
    // 来自 VirtualQueryEx 与进程缓存（读取通道/端口层）；isLowAddressGuard 是地址与
    // 常量 0x10000 的比较，调用方算好传入即可。本结构体只是"事实载体"。
    struct ReadFailureContext
    {
        // channelName：通道显示名（例如“标准驱动（R0）”），用于说明"这是哪条通道失败的"。
        QString channelName;
        // address：读取失败的地址。
        quint64 address = 0;
        // pid：目标进程号；0 表示无进程上下文（内核/物理范围）。
        quint32 pid = 0;
        // regionQueryOk：地址所在内存区域的查询是否成功（旧代码用 VirtualQueryEx）。
        bool regionQueryOk = false;
        // regionSummary：区域查询成功时的摘要文本（地址/大小/状态/保护），已格式化好。
        QString regionSummary;
        // regionIsFree：区域状态是否为“未映射”（MEM_FREE）——意味着继续换通道也读不到。
        bool regionIsFree = false;
        // sameNameProcessCount：与目标同名的进程数；regionIsFree 为真时才有意义，
        // 大于 1 提示"很可能附加到了另一个同名进程"。
        int sameNameProcessCount = 0;
        // processName：目标进程名，用于拼"本机有 N 个都叫 X 的进程"。
        QString processName;
        // isLowAddressGuard：地址是否落在每进程固定的低 64 KB 空指针保护区内。
        bool isLowAddressGuard = false;
        // rawFailureText：通道自身返回的原始失败说明（可能是英文/错误码），原样附在最后。
        QString rawFailureText;
    };

    // ChannelName / ScopeName / WriteModeName：枚举的短名，用于会话条分段按钮文字、
    // 状态条"通道·范围"段与对话框正文里引用当前目标。
    QString ChannelName(ksword::memwb::Channel channel);
    QString ScopeName(ksword::memwb::Scope scope);
    QString WriteModeName(ksword::memwb::WriteMode mode);

    // ChannelDescription：会话条通道分段按钮的常规悬停提示（通道说明，不含不可用原因），
    // 取旧 UiBuild.cpp:538 长说明拆成的四段。
    QString ChannelDescription(ksword::memwb::Channel channel);

    // ChannelUnavailableReason：某个通道在给定 GateVerdict 下显示给用户的原因文案。
    // available 为真且非"未知"时返回空串（表示不需要报原因）；IsUnknown() 时返回
    // "可用性尚未探测完成"一类的提示，供调用方在非禁用场景下也能说明"为什么还不确定"。
    QString ChannelUnavailableReason(ksword::memwb::Channel channel, ksword::memwb::GateVerdict verdict);

    // Translate 系列：五类枚举 -> 中文文案，调用方不得自己为这些枚举拼句子。
    QString Translate(ksword::memwb::CommitOutcome outcome);
    QString Translate(ksword::memwb::ApprovalAnswer answer);
    QString Translate(ksword::memwb::ExprError error);
    QString Translate(ksword::memwb::Issue issue);
    // Translate(PageState)：ux/design 文档里称"IoReadStatus"，仓库里的实际枚举名是
    // MemoryPageReader.h 的 PageState（Valid/PartiallyValid/Unreadable/NotAttempted）。
    QString Translate(ksword::memwb::PageState state);
    // Translate(IoReadStatus)：MemoryIoPort.h 里**真正**叫 IoReadStatus 的枚举
    // （Ok/Partial/Unreadable/Failed），与上面 PageState 不是同一个类型——B11：
    // Failed 是"通道自身失败，没得出关于目标任何结论"，Unreadable 是"目标本身不可读
    // 但通道是健康的"，这两句话必须分开，不能共用 PageState 的文案。
    QString Translate(ksword::memwb::IoReadStatus status);
    // Translate(SessionError)：会话自洽性检查（MemoryTargetSession.h::Validate）失败
    // 原因，供 CommitReport::sessionError（outcome==InvalidSession 时）展示细节。
    QString Translate(ksword::memwb::SessionError error);
    // Translate(ModeSwitchStatus)：SetMode / ResolveModeSwitch 的结果短句。
    QString Translate(ksword::memwb::ModeSwitchStatus status);
    // Translate(StageStatus)：MemoryDiffOverlay::Stage 的拒绝原因短句。
    QString Translate(ksword::memwb::StageStatus status);

    // CommitReportSummary：按 CommitReport 组装"写入结果"段文字；scratchAreaDirty /
    // readModifyWriteWindow / needsReread 三个旗标由状态条的常驻/瞬时 chip 单独表达
    // （见 WorkbenchStatusBar），本函数只负责 outcome 与块数/字节数那句话。
    QString CommitReportSummary(const ksword::memwb::CommitReport& report);

    // ExplainReadFailure：拼出完整的读取失败诊断整段说明，见文件头“迁移位”说明。
    QString ExplainReadFailure(const ReadFailureContext& context);

    // —— 会话条 ——
    // TargetChipAttachedText / TargetChipUnattachedText：目标 chip 在已附加/未附加
    // 两种状态下显示的文字；TargetChipTooltip 是该 chip 的悬停提示。
    QString TargetChipAttachedText(
        const QString& processName,
        quint32 pid,
        quint32 addressBits,
        bool canReadWrite);
    QString TargetChipUnattachedText();
    // TargetChipNoProcessText：B3——内核/物理范围本来就不需要进程，chip 应显示中性
    // 提示（而不是"未附加进程"的红字），也不该诱导用户去点它选进程。传入
    // ProcessVirtual 时返回空串（这个范围走 TargetChipAttachedText/UnattachedText）。
    QString TargetChipNoProcessText(ksword::memwb::Scope scope);
    // TargetChipTooltip：目标 chip 的悬停提示。needsProcess 为假（内核/物理范围，
    // 调用方按 ScopeNeedsProcess 传入）时返回中性说明，不再暗示"点击能选进程"——
    // 那两个范围压根不存在"选目标"这一步（S-e，第二轮修复）。
    QString TargetChipTooltip(bool needsProcess);
    // WriteModeTooltip：写入模式开关的完整悬停提示（含立即/暂存两种语义说明）。
    QString WriteModeTooltip();
    // PendingPatchesText："N 字节待写入（M 处）"。
    QString PendingPatchesText(quint64 bytesPending, quint64 blocksPending);
    QString ApplyButtonTooltip();
    QString DiscardButtonTooltip();

    // —— 写入模式三选一（切换时存在未提交补丁） ——
    QString ModeSwitchDialogTitle();
    QString ModeSwitchDialogBody(quint64 pendingBytes, quint64 pendingBlocks, ksword::memwb::WriteMode toMode);
    QString ApplyThenSwitchButtonText();
    QString DiscardThenSwitchButtonText();
    QString ModeSwitchCancelButtonText();
    // ApplyFailedDiagnosticsPrefix：ApplyThenSwitch 失败时写进诊断抽屉的说明前缀。
    QString ApplyFailedDiagnosticsPrefix();

    // —— 离开前存在未提交的暂存补丁（WP-J6 Wave 3 新增，修复缺陷 3）——
    // 原实现在"身份类变更/离开视图但有未提交暂存"场景里误用了上面的
    // ModeSwitchDialogTitle/Body（from/to 传同一个写入模式值，文案"切换到
    // 「立即写入」前必须先处理"自相矛盾——压根没有"切换"这件事）。本组三个
    // 函数专门服务那个场景，三选一的返回值仍然复用既有的 ModeSwitchDecision
    // 枚举（ApplyThenSwitch/DiscardThenSwitch/Cancel 的语义改读成"应用并离开/
    // 丢弃并离开/取消"，不新增枚举）。
    QString LeaveWithPendingDialogTitle();
    // LeaveWithPendingDialogBody：正文写明待写入字节数/块数与离开原因；
    // reasonText 由调用方把 WorkbenchTarget::LeaveReason 翻译成人话短句
    // （例如"切换范围"）后传入，本函数不关心具体枚举值，不引入新的头文件依赖。
    QString LeaveWithPendingDialogBody(quint64 pendingBytes, quint64 pendingBlocks, const QString& reasonText);
    QString LeaveWithPendingApplyButtonText();
    QString LeaveWithPendingDiscardButtonText();
    QString LeaveWithPendingCancelButtonText();

    // —— 普通确认（ConfirmUi，可被设置抑制） ——
    QString UiConfirmTitle();
    // UiConfirmBody：B2——正文必须写明"通道 · 范围 · 目标"，不得把 request.targetIdentity
    // 这种机器可读的 IdentityKey 原样显示给用户。targetDescription 由调用方给出
    // （生产环境下取会话条目标 chip 已有的"进程名 · PID · 位数"文案），空串时退回
    // "当前目标"这种保守说法，绝不展示原始 IdentityKey。
    QString UiConfirmBody(
        const ksword::memwb::UiConfirmRequest& request,
        ksword::memwb::Scope scope,
        ksword::memwb::Channel channel,
        const QString& targetDescription);
    QString DontAskAgainThisRunText();
    QString UiConfirmAcceptButtonText();
    QString UiConfirmRejectButtonText();

    // —— 强制同意（ConfirmApproval，永远弹，不受抑制开关影响） ——
    QString ApprovalDialogTitle();
    // ApprovalDialogBody：同 UiConfirmBody 的 B2 修法。
    QString ApprovalDialogBody(
        const ksword::memwb::ApprovalRequest& request,
        ksword::memwb::Scope scope,
        ksword::memwb::Channel channel,
        const QString& targetDescription);
    QString ApprovalThisBlockOnlyButtonText();
    QString ApprovalRestOfBatchButtonText();
    QString ApprovalCancelButtonText();

    // —— 状态条常驻/瞬时 chip ——
    QString ScratchAreaDirtyChipText();
    QString ScratchAreaDirtyAckTooltip();
    QString ReadModifyWriteWindowChipText();
    QString NeedsRereadHintText();
    QString CopyDiagnosticsButtonTooltip();
    QString ExpandDiagnosticsTooltip(bool expanded);
    // DiagnosticsWrapCheckboxText：诊断抽屉的"自动换行"勾选框文字（B12）。
    QString DiagnosticsWrapCheckboxText();

    // —— 字符串写入对话框 ——
    QString StringWriteDialogTitle();
    // StringWriteEncodingLabel：0=ANSI，1=UTF-8，2=UTF-16LE；越界返回空串。
    QString StringWriteEncodingLabel(int encodingIndex);
    QString StringWriteNulCheckboxText();
    QString StringWriteInputPlaceholder();
    // StringWritePreviewText："将写入 N 字节"。
    QString StringWritePreviewText(quint64 byteCount);
    // StringWriteAnsiLossyText：B1——当前文本含本机 ANSI 代码页无法表示的字符时，
    // 预览区显示的红字说明；此时不得编码出任何字节，"写入"按钮必须禁用。
    QString StringWriteAnsiLossyText();
    QString StringWriteOkButtonText();
    QString StringWriteCancelButtonText();
}
