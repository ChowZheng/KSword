// ============================================================
// MemoryChannelGate.cpp
// 作用：
// - 实现 MemoryChannelGate.h 声明的通道可用性判据与"每范围上次通道"的记忆。
// - 全部是纯逻辑，不依赖 Qt 与 Win32。
// ============================================================

#include "MemoryChannelGate.h"

namespace ksword::memwb
{
    namespace
    {
        // kScopeCount：Scope 的合法取值个数，也是 ChannelMemory 数组长度。
        constexpr std::uint32_t kScopeCount = 3U;

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

        // MakeVerdict：构造一个结论。传入：available 是否可用；reason 原因。传出：结论值。
        GateVerdict MakeVerdict(const bool available, const GateReason reason) noexcept
        {
            GateVerdict verdict;
            verdict.available = available;
            verdict.reason = reason;
            return verdict;
        }

        // EvaluateHvm：HVM 通道自己的条件（范围与 pid 已通过）。
        // 传入：inputs 运行期输入。传出：结论。驱动未加载优先于探测状态：没有驱动就无从探测。
        GateVerdict EvaluateHvm(const GateInputs& inputs) noexcept
        {
            // 驱动没加载：探测结果即使缓存着也不可信，直接不可用。
            if (!inputs.driverLoaded)
            {
                return MakeVerdict(false, GateReason::DriverNotLoaded);
            }

            // 按探测缓存分三档：未探测=未知（允许选用），不可用=阻断，可用=放行。
            switch (inputs.hvmProbe)
            {
            case ProbeState::NotProbed:
                return MakeVerdict(true, GateReason::ProbeNotDone);
            case ProbeState::Unusable:
                return MakeVerdict(false, GateReason::ProbeFailed);
            case ProbeState::Usable:
                return MakeVerdict(true, GateReason::None);
            }

            // 探测状态越界：输入本身有问题，按无效请求处理，不猜测。
            return MakeVerdict(false, GateReason::InvalidRequest);
        }
    }

    // ChannelSupportsScope：静态范围支持表。
    bool ChannelSupportsScope(const Channel channel, const Scope scope) noexcept
    {
        // 任一越界值都不支持，先挡掉。
        if (!IsKnownChannel(channel) || !IsKnownScope(scope))
        {
            return false;
        }

        // R3 通道只支持进程虚拟地址空间；其余三个通道三个范围都支持。
        if (channel == Channel::UserMode)
        {
            return scope == Scope::ProcessVirtual;
        }
        return true;
    }

    // EvaluateChannel：完整判据，顺序见头文件。
    GateVerdict EvaluateChannel(const Scope scope, const Channel channel, const GateInputs& inputs) noexcept
    {
        // 第一步：枚举越界。
        if (!IsKnownScope(scope) || !IsKnownChannel(channel))
        {
            return MakeVerdict(false, GateReason::InvalidRequest);
        }

        // 第二步：范围不支持该通道。
        if (!ChannelSupportsScope(channel, scope))
        {
            return MakeVerdict(false, GateReason::ScopeNotSupported);
        }

        // 第三步：进程范围下没有目标进程，任何通道都谈不上读写。
        if (scope == Scope::ProcessVirtual && !inputs.hasProcessTarget)
        {
            return MakeVerdict(false, GateReason::NeedsPid);
        }

        // 第四步：通道自己的条件。
        switch (channel)
        {
        case Channel::UserMode:
            // R3 只依赖目标进程，前面已经通过。
            return MakeVerdict(true, GateReason::None);
        case Channel::StandardDriver:
            // R0 只依赖驱动已加载。
            if (!inputs.driverLoaded)
            {
                return MakeVerdict(false, GateReason::DriverNotLoaded);
            }
            return MakeVerdict(true, GateReason::None);
        case Channel::Hvm:
            return EvaluateHvm(inputs);
        case Channel::Ddma:
            // DDMA：先驱动，再会话就绪。
            if (!inputs.driverLoaded)
            {
                return MakeVerdict(false, GateReason::DriverNotLoaded);
            }
            if (!inputs.ddmaSessionReady)
            {
                return MakeVerdict(false, GateReason::SessionNotReady);
            }
            return MakeVerdict(true, GateReason::None);
        }

        // 理论上到不了这里（枚举已校验）；保守返回无效请求。
        return MakeVerdict(false, GateReason::InvalidRequest);
    }

    // DefaultChannelFor：进程->R3，内核/物理->R0，Ddma 永不作默认。
    Channel DefaultChannelFor(const Scope scope) noexcept
    {
        if (scope == Scope::KernelVirtual || scope == Scope::Physical)
        {
            return Channel::StandardDriver;
        }

        // 进程范围与越界范围都落到权限最低的一档。
        return Channel::UserMode;
    }

    // GateReasonKey：原因的稳定 ASCII 键。
    const char* GateReasonKey(const GateReason reason) noexcept
    {
        switch (reason)
        {
        case GateReason::None:
            return "none";
        case GateReason::ScopeNotSupported:
            return "scope-not-supported";
        case GateReason::NeedsPid:
            return "needs-pid";
        case GateReason::DriverNotLoaded:
            return "driver-not-loaded";
        case GateReason::ProbeNotDone:
            return "probe-not-done";
        case GateReason::ProbeFailed:
            return "probe-failed";
        case GateReason::SessionNotReady:
            return "session-not-ready";
        case GateReason::InvalidRequest:
            return "invalid-request";
        }
        return "invalid-request";
    }

    // 构造：三个范围各放默认通道。
    ChannelMemory::ChannelMemory() noexcept
        : remembered_{ Channel::UserMode, Channel::UserMode, Channel::UserMode }
    {
        Reset();
    }

    // Recall：取记住的通道。
    Channel ChannelMemory::Recall(const Scope scope) const noexcept
    {
        if (!IsKnownScope(scope))
        {
            return DefaultChannelFor(scope);
        }
        return remembered_[static_cast<std::uint32_t>(scope)];
    }

    // Remember：记住用户的显式选择。
    bool ChannelMemory::Remember(const Scope scope, const Channel channel) noexcept
    {
        // 越界值或范围不支持的通道一律拒绝，原记忆不变。
        if (!ChannelSupportsScope(channel, scope))
        {
            return false;
        }

        // 不检查此刻是否可用：不可用的通道照样记住，界面报红即可。
        remembered_[static_cast<std::uint32_t>(scope)] = channel;
        return true;
    }

    // Restore：从持久化整数恢复，读盘时的数据清洗。
    bool ChannelMemory::Restore(const Scope scope, const std::uint32_t persistedValue) noexcept
    {
        // 越界范围无处可记。
        if (!IsKnownScope(scope))
        {
            return false;
        }

        // 先把整数转成通道枚举；只有 0..3 才可能是合法通道，其余按默认。
        const Channel candidate = static_cast<Channel>(persistedValue);
        const bool supported = ChannelSupportsScope(candidate, scope);

        // DDMA 永不作启动默认：读到 Ddma 当默认处理。
        const bool isDdma = (candidate == Channel::Ddma);
        if (!supported || isDdma)
        {
            remembered_[static_cast<std::uint32_t>(scope)] = DefaultChannelFor(scope);
            return false;
        }

        remembered_[static_cast<std::uint32_t>(scope)] = candidate;
        return true;
    }

    // Reset：三个范围全部恢复默认。
    void ChannelMemory::Reset() noexcept
    {
        for (std::uint32_t index = 0; index < kScopeCount; ++index)
        {
            remembered_[index] = DefaultChannelFor(static_cast<Scope>(index));
        }
    }
}
