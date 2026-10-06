// ============================================================
// MemoryWritePolicy.cpp
// 作用：
// - 实现 MemoryWritePolicy.h 声明的写入确认策略。
// - 全部是纯逻辑，不依赖 Qt 与 Win32，也不碰审计与写事务本身。
// ============================================================

#include "MemoryWritePolicy.h"

namespace ksword::memwb
{
    namespace
    {
        // kChannelStride：位图下标里每个范围占用的通道槽数（通道有四个合法取值）。
        constexpr std::uint32_t kChannelStride = 4U;

        // IsKnownScope：判断 scope 是否是三个合法取值之一。
        // 传入：scope 待判断的范围。传出：合法为 true。逐个列出，新增枚举值时会被迫来改这里。
        bool IsKnownScope(const Scope scope) noexcept
        {
            const bool isProcess = (scope == Scope::ProcessVirtual);
            const bool isKernel = (scope == Scope::KernelVirtual);
            const bool isPhysical = (scope == Scope::Physical);
            return isProcess || isKernel || isPhysical;
        }

        // IsKnownChannel：判断 channel 是否是四个合法取值之一。
        // 传入：channel 待判断的通道。传出：合法为 true。
        bool IsKnownChannel(const Channel channel) noexcept
        {
            const bool isUser = (channel == Channel::UserMode);
            const bool isDriver = (channel == Channel::StandardDriver);
            const bool isHvm = (channel == Channel::Hvm);
            const bool isDdma = (channel == Channel::Ddma);
            return isUser || isDriver || isHvm || isDdma;
        }

        // IsKnownMode：判断 mode 是否是两个合法取值之一。
        // 传入：mode 待判断的写入模式。传出：合法为 true。
        bool IsKnownMode(const WriteMode mode) noexcept
        {
            const bool isImmediate = (mode == WriteMode::Immediate);
            const bool isStaged = (mode == WriteMode::StagedThenApply);
            return isImmediate || isStaged;
        }

        // CombinationBit：把"范围+通道"组合映射成位图里的一位。
        // 传入：scope/channel 必须都已通过 IsKnown 校验。传出：只有一位为 1 的掩码。
        std::uint32_t CombinationBit(const Scope scope, const Channel channel) noexcept
        {
            const std::uint32_t index =
                static_cast<std::uint32_t>(scope) * kChannelStride + static_cast<std::uint32_t>(channel);
            return 1U << index;
        }

        // MakeDecision：构造一个结论。传入：三个字段。传出：结论值。
        ConfirmDecision MakeDecision(
            const bool suppressed,
            const bool offerDontAskAgain,
            const ConfirmReason reason) noexcept
        {
            ConfirmDecision decision;
            decision.suppressed = suppressed;
            decision.offerDontAskAgain = offerDontAskAgain;
            decision.reason = reason;
            return decision;
        }
    }

    // IsRiskyImmediateCombination：内核/物理范围或 Ddma 通道属于需要首次确认的一类。
    bool IsRiskyImmediateCombination(const Scope scope, const Channel channel) noexcept
    {
        // 越界值不属于任何一类，由 Decide 另行处理。
        if (!IsKnownScope(scope) || !IsKnownChannel(channel))
        {
            return false;
        }

        // 内核/物理范围：写错可能直接拖垮系统。磁盘传输通道：每次写都会借用磁盘暂存扇区。
        const bool nonProcessScope = (scope != Scope::ProcessVirtual);
        const bool diskTransfer = (channel == Channel::Ddma);
        return nonProcessScope || diskTransfer;
    }

    // Decide：按固定顺序判定，先命中先返回。
    ConfirmDecision MemoryWritePolicy::Decide(
        const WriteMode mode,
        const Scope scope,
        const Channel channel,
        const bool globalSkipDangerousConfirm) const noexcept
    {
        // 第一步：全局"跳过危险确认"开关。它是用户显式的设置，压过其余一切（含越界输入）；
        // 审计仍由写事务照写，这里只负责告诉它"不弹"。
        if (globalSkipDangerousConfirm)
        {
            return MakeDecision(true, false, ConfirmReason::GlobalSkip);
        }

        // 第二步：越界输入，保守地要求确认，且不提供勾选框（不给越界组合留下记忆）。
        const bool inputsKnown = IsKnownMode(mode) && IsKnownScope(scope) && IsKnownChannel(channel);
        if (!inputsKnown)
        {
            return MakeDecision(false, false, ConfirmReason::InvalidInput);
        }

        // 第三步：暂存后应用，每次都弹，没有"不再询问"。
        if (mode == WriteMode::StagedThenApply)
        {
            return MakeDecision(false, false, ConfirmReason::StagedApply);
        }

        // 第四步：立即写入。进程范围且不是磁盘传输通道 -> 不弹。
        if (!IsRiskyImmediateCombination(scope, channel))
        {
            return MakeDecision(true, false, ConfirmReason::ProcessImmediate);
        }

        // 第五步：属于需要首次确认的一类。本次运行已勾选不再询问则不弹。
        if (IsRemembered(scope, channel))
        {
            return MakeDecision(true, false, ConfirmReason::RememberedThisRun);
        }

        // 第六步：要弹，并且允许用户勾选"本次运行不再询问"。
        return MakeDecision(false, true, ConfirmReason::RiskyImmediate);
    }

    // SuppressUiConfirm：只取 suppressed 的便捷形式。
    bool MemoryWritePolicy::SuppressUiConfirm(
        const WriteMode mode,
        const Scope scope,
        const Channel channel,
        const bool globalSkipDangerousConfirm) const noexcept
    {
        return Decide(mode, scope, channel, globalSkipDangerousConfirm).suppressed;
    }

    // NoteConfirmed：用户同意之后调用，勾选了才记忆。
    bool MemoryWritePolicy::NoteConfirmed(
        const Scope scope,
        const Channel channel,
        const bool dontAskAgainThisRun) noexcept
    {
        // 没勾选：什么都不记（同一组合下次仍会问）。
        if (!dontAskAgainThisRun)
        {
            return false;
        }

        // 只有需要首次确认的一类才有"记忆"的意义；进程范围的普通通道本来就不弹，
        // 越界组合也不留记忆。
        if (!IsRiskyImmediateCombination(scope, channel))
        {
            return false;
        }

        rememberedMask_ |= CombinationBit(scope, channel);
        return true;
    }

    // IsRemembered：该组合是否已记入。
    bool MemoryWritePolicy::IsRemembered(const Scope scope, const Channel channel) const noexcept
    {
        if (!IsKnownScope(scope) || !IsKnownChannel(channel))
        {
            return false;
        }
        return (rememberedMask_ & CombinationBit(scope, channel)) != 0U;
    }

    // ResetRun：清空记忆。
    void MemoryWritePolicy::ResetRun() noexcept
    {
        rememberedMask_ = 0U;
    }
}
