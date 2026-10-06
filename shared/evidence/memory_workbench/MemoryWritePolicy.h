#pragma once

// ============================================================
// MemoryWritePolicy.h
// 作用：
// - 决定一次写入提交前"界面确认（ConfirmUi）"该不该弹：给定 写入模式、范围、通道、全局
//   "跳过危险确认"开关和"本次运行已勾选不再询问"的集合，算出
//   MemoryWriteTransaction::SetUiConfirmSuppressed 应当取什么值。
// - 采纳用户已拍板的确认策略（docs/内存工作台Phase3集成设计.md 第 6 节第 1 条）。
//
// 确认策略表（用户已拍板）：
//   场景                                              界面确认
//   全局"跳过危险确认"开关打开                         不弹（审计照写）
//   进程范围 + 立即写入（通道不是磁盘传输）            不弹
//   内核/物理范围，或磁盘传输(Ddma)通道，+ 立即写入    弹；用户可勾选"本次运行不再询问"，
//                                                     勾选后该"范围+通道"组合本次运行内不再弹
//   暂存 -> 应用                                       每次都弹，没有"不再询问"
//   强制同意（后端要求的 ConfirmApproval）              与本类无关，永远由写事务逐块询问
//
// 冻结接口摘要（后续 E-K 各包依赖，改动须经主会话批准）：
//   enum class ConfirmReason               决定的原因：GlobalSkip / ProcessImmediate / RememberedThisRun /
//                                          RiskyImmediate / StagedApply / InvalidInput。
//   struct ConfirmDecision { suppressed, offerDontAskAgain, reason }
//                                          suppressed = SetUiConfirmSuppressed 应取的值；
//                                          offerDontAskAgain = 对话框是否应显示"本次运行不再询问"勾选框。
//   class MemoryWritePolicy
//     ConfirmDecision Decide(WriteMode, Scope, Channel, bool globalSkipDangerousConfirm) const
//     bool SuppressUiConfirm(WriteMode, Scope, Channel, bool globalSkipDangerousConfirm) const
//                                          只取 Decide(...).suppressed 的便捷形式。
//     bool NoteConfirmed(Scope, Channel, bool dontAskAgainThisRun)
//                                          用户在界面确认里点了同意之后调用；返回是否记入了"不再询问"。
//     bool IsRemembered(Scope, Channel) const   该组合本次运行是否已勾选不再询问。
//     void ResetRun()                      清空本次运行的记忆（"本次运行"的边界由调用方决定）。
//   bool IsRiskyImmediateCombination(Scope, Channel)   内核/物理范围或 Ddma 通道。
//
// 规则：
// - 本类只返回 suppressed 布尔，**不触碰审计**：suppressed=true 时写事务仍会写
//   UiConfirmSuppressed 审计事件（不变式 15），这里不管。
// - "本次运行不再询问"只存内存，不持久化；ResetRun 之外没有任何途径清掉它。
// - 对"未勾选"的确认，同一组合下次仍然会问（"首次弹一次"按"弹到用户勾选为止"实现）；
//   NoteConfirmed 只在 dontAskAgainThisRun 为真时记忆。若产品上要改成"首次确认后就不再问"，
//   只需让 NoteConfirmed 忽略该参数、一律记入，并同步改测试。
// - 对话框只在 Decide 返回 offerDontAskAgain=true 时才应出现勾选框，且只在用户**同意**之后调用
//   NoteConfirmed；用户拒绝时不调用。暂存->应用的对话框没有勾选框，不得传 dontAskAgain=true。
// - 参数越界（枚举值不合法）时保守地要求确认：suppressed=false、不提供勾选框；全局跳过开关打开时
//   仍按开关处理（那是用户显式的设置）。
// - 仅使用标准库，不包含 Windows.h，不包含任何 Qt 头。
// ============================================================

#include <cstdint>

#include "MemoryTargetSession.h"
#include "MemoryWriteTransaction.h"

namespace ksword::memwb
{
    // ConfirmReason：Decide 给出结论的原因。数值固定，测试与界面层依赖它。
    enum class ConfirmReason : std::uint32_t
    {
        // GlobalSkip：全局"跳过危险确认"开关打开，不弹。
        GlobalSkip = 0,
        // ProcessImmediate：进程范围 + 立即写入（非磁盘传输通道），不弹。
        ProcessImmediate = 1,
        // RememberedThisRun：该组合本次运行已勾选"不再询问"，不弹。
        RememberedThisRun = 2,
        // RiskyImmediate：内核/物理范围或磁盘传输通道的立即写入，尚未勾选不再询问，要弹，可勾选。
        RiskyImmediate = 3,
        // StagedApply：暂存后应用，每次都弹，没有"不再询问"。
        StagedApply = 4,
        // InvalidInput：模式/范围/通道越界，保守地要求确认。
        InvalidInput = 5,
    };

    // ConfirmDecision：Decide 的结论。默认值是"要弹、不提供勾选框、无效输入"（保守初值）。
    struct ConfirmDecision
    {
        // suppressed：SetUiConfirmSuppressed 应取的值。true 表示不弹界面确认。
        bool suppressed = false;
        // offerDontAskAgain：对话框是否应显示"本次运行不再询问"勾选框。仅 RiskyImmediate 为 true。
        bool offerDontAskAgain = false;
        // reason：原因。
        ConfirmReason reason = ConfirmReason::InvalidInput;
    };

    // IsRiskyImmediateCombination：该"范围+通道"在立即写入下是否属于需要首次确认的一类。
    // 传入：scope 范围；channel 通道。
    // 传出：范围是内核或物理，或通道是 Ddma 时为 true；进程范围的 R3/R0/HVM 为 false；
    //       任一越界值为 false（越界由 Decide 单独处理）。
    bool IsRiskyImmediateCombination(Scope scope, Channel channel) noexcept;

    // MemoryWritePolicy：确认策略。持有"本次运行已勾选不再询问"的集合。非线程安全，UI 线程使用。
    class MemoryWritePolicy
    {
    public:
        // 构造：集合为空。
        MemoryWritePolicy() noexcept = default;

        // Decide：算出一次提交前界面确认的处理方式。
        // 传入：mode 当前写入模式；scope/channel 会话的范围与通道；
        //       globalSkipDangerousConfirm 全局"跳过危险确认"开关当前值。
        // 传出：ConfirmDecision。判定顺序：全局开关 -> 越界输入 -> 暂存应用 -> 进程立即 ->
        //       已记忆 -> 首次危险立即。不修改任何状态。
        ConfirmDecision Decide(
            WriteMode mode,
            Scope scope,
            Channel channel,
            bool globalSkipDangerousConfirm) const noexcept;

        // SuppressUiConfirm：Decide(...).suppressed 的便捷形式。参数含义同 Decide。
        bool SuppressUiConfirm(
            WriteMode mode,
            Scope scope,
            Channel channel,
            bool globalSkipDangerousConfirm) const noexcept;

        // NoteConfirmed：用户在界面确认里点了"同意"之后调用。
        // 传入：scope/channel 刚确认的组合；dontAskAgainThisRun 用户是否勾选了"本次运行不再询问"。
        // 传出：true 表示这次调用把该组合记入了集合；未勾选、组合不属于需要首次确认的一类、
        //       或枚举越界都返回 false 且不改集合。
        bool NoteConfirmed(Scope scope, Channel channel, bool dontAskAgainThisRun) noexcept;

        // IsRemembered：该组合本次运行是否已勾选不再询问。传入：scope/channel。传出：已记入为 true。
        bool IsRemembered(Scope scope, Channel channel) const noexcept;

        // ResetRun：清空集合。传入/传出：无。
        void ResetRun() noexcept;

    private:
        // rememberedMask_：已勾选不再询问的组合位图，位下标 = 范围数值 * 4 + 通道数值（共 12 位）。
        std::uint32_t rememberedMask_ = 0U;
    };
}
