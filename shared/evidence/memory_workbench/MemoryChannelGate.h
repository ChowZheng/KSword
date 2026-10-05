#pragma once

// ============================================================
// MemoryChannelGate.h
// 作用：
// - "某个通道在某个范围下此刻能不能用"的纯函数判据，合并了旧设计里的
//   ChannelSupportsScope（静态的范围支持表）与 EvaluateChannel（带运行期输入的可用性）。
// - 提供"每个范围上次使用的通道"的显式记忆（ChannelMemory）与各范围的默认通道。
//
// 为什么要有这个模块：
// - 旧 Tab3/Tab5 有四个互不联动的后端下拉，选错了没人报错；旧 refreshBackendSelectors
//   在 DDMA 不可用时还会悄悄弹回别的通道，于是"R0 读取"实际走的是 R3 这类缺陷无处追查。
//   这里把可用性收敛成一个纯函数，并且**没有任何函数会返回一个与请求不同的通道**。
//
// 冻结接口摘要（后续 E-K 各包依赖，改动须经主会话批准）：
//   enum class ProbeState                  NotProbed / Usable / Unusable：异步探测一次的缓存值。
//   struct GateInputs                      运行期输入：hasProcessTarget、driverLoaded、hvmProbe、ddmaSessionReady。
//   enum class GateReason                  None / ScopeNotSupported / NeedsPid / DriverNotLoaded /
//                                          ProbeNotDone / ProbeFailed / SessionNotReady / InvalidRequest。
//   struct GateVerdict { available, reason }   available=false 时 reason 是阻断原因；available=true 时
//                                          reason 为 None（确认可用）或 ProbeNotDone（可用性"未知"，
//                                          允许选用，不得置灰）。IsUnknown() 判后者。
//   bool ChannelSupportsScope(Channel, Scope)  静态范围支持表：UserMode 只支持 ProcessVirtual，
//                                          其余通道三个范围都支持；越界值一律 false。
//   GateVerdict EvaluateChannel(Scope, Channel, const GateInputs&)   完整判据，顺序见下。
//   Channel DefaultChannelFor(Scope)       默认通道：进程->UserMode，内核/物理->StandardDriver；
//                                          Ddma 永远不是默认；越界范围返回 UserMode。
//   const char* GateReasonKey(GateReason)  稳定的 ASCII 键（日志/词条键用，不是界面文案）。
//   class ChannelMemory
//     ChannelMemory()                      每个范围先放默认通道。
//     Channel Recall(Scope) const          该范围上次记住的通道（越界范围返回 UserMode）。
//     bool Remember(Scope, Channel)        记住用户的显式选择；范围不支持该通道或越界则拒绝并返回 false。
//     bool Restore(Scope, uint32_t raw)    从持久化的整数恢复；越界、范围不支持、或是 Ddma 一律按默认，
//                                          返回 false 表示用了默认。
//     void Reset()                         全部恢复默认。
//
// 判据顺序（EvaluateChannel，先命中先返回，测试钉住这个顺序）：
//   1. 枚举越界                       -> 不可用，InvalidRequest
//   2. 范围不支持该通道（R3 + 非进程） -> 不可用，ScopeNotSupported
//   3. 进程范围但没有 pid              -> 不可用，NeedsPid（对所有通道，没有进程谈不上读）
//   4. 通道自己的条件：
//        UserMode        -> 可用，None
//        StandardDriver  -> 驱动未加载 DriverNotLoaded；否则 None
//        Hvm             -> 驱动未加载 DriverNotLoaded；探测 NotProbed 则**可用但未知**（ProbeNotDone）；
//                           Unusable 则不可用（ProbeFailed）；Usable 则 None
//        Ddma            -> 驱动未加载 DriverNotLoaded；会话未就绪 SessionNotReady；否则 None
//
// 规则：
// - 通道此刻不可用时，ChannelMemory 仍保持用户的选择，由界面报红；这是显式记忆，不是回退。
//   整个模块没有"自动选择/回退到另一通道"的函数。
// - 未探测到的 HVM 是"未知"而不是"不可用"：探测是异步的，探测完成前把它置灰等于替用户做了
//   尚无依据的决定。
// - 本模块不拼任何用户可见文案；界面层按 GateReason 翻译。
// - 仅使用标准库，不包含 Windows.h，不包含任何 Qt 头。
// ============================================================

#include <cstdint>

#include "MemoryTargetSession.h"

namespace ksword::memwb
{
    // ProbeState：一次异步探测的缓存结果。数值固定。
    enum class ProbeState : std::uint32_t
    {
        // NotProbed：还没探测到结果（尚未开始或还在进行）。可用性"未知"。
        NotProbed = 0,
        // Usable：探测完成，可用。
        Usable = 1,
        // Unusable：探测完成，不可用。
        Unusable = 2,
    };

    // GateInputs：EvaluateChannel 的运行期输入。默认值全部是"最保守"的一档。
    struct GateInputs
    {
        // hasProcessTarget：进程范围下是否已有目标进程（会话 pid 非零）。非进程范围忽略。
        bool hasProcessTarget = false;
        // driverLoaded：KswordARK 驱动是否已加载、设备可用。驱动类通道（R0/HVM/DDMA）依赖它。
        bool driverLoaded = false;
        // hvmProbe：HVM 通道的可用性探测缓存。
        ProbeState hvmProbe = ProbeState::NotProbed;
        // ddmaSessionReady：DDMA 会话是否就绪（暂存扇区已配置，对应旧 isDdmaUsable）。
        bool ddmaSessionReady = false;
    };

    // GateReason：通道不可用（或可用性未知）的原因。数值固定，界面层按它翻译文案。
    enum class GateReason : std::uint32_t
    {
        // None：可用，没有任何保留。
        None = 0,
        // ScopeNotSupported：该通道不支持该范围（R3 不支持内核/物理）。
        ScopeNotSupported = 1,
        // NeedsPid：进程范围但还没有目标进程。
        NeedsPid = 2,
        // DriverNotLoaded：驱动未加载，驱动类通道不可用。
        DriverNotLoaded = 3,
        // ProbeNotDone：探测尚未得出结果。与 available=true 搭配时表示"未知，允许选用"。
        ProbeNotDone = 4,
        // ProbeFailed：探测完成且结论是不可用。
        ProbeFailed = 5,
        // SessionNotReady：DDMA 会话未就绪。
        SessionNotReady = 6,
        // InvalidRequest：范围或通道枚举越界、探测状态越界；也是 GateVerdict 的默认值。
        InvalidRequest = 7,
    };

    // GateVerdict：EvaluateChannel 的结论。默认值是"不可用 + InvalidRequest"（失败时的安全初值）。
    struct GateVerdict
    {
        // available：是否允许选用。未知（ProbeNotDone）时为 true。
        bool available = false;
        // reason：原因。available=false 时是阻断原因；available=true 时只会是 None 或 ProbeNotDone。
        GateReason reason = GateReason::InvalidRequest;

        // IsUnknown：是否"可用但未验证"。传出：available 且 reason 为 ProbeNotDone 时为 true。
        bool IsUnknown() const noexcept
        {
            return available && (reason == GateReason::ProbeNotDone);
        }
    };

    // ChannelSupportsScope：静态范围支持表。
    // 传入：channel 通道；scope 范围。
    // 传出：UserMode 只支持 ProcessVirtual；StandardDriver/Hvm/Ddma 三个范围都支持；任一越界返回 false。
    // 依据：MemoryAccessBackend 的 R3 通道对内核/物理地址空间有显式拒绝分支（读代码所得）。
    bool ChannelSupportsScope(Channel channel, Scope scope) noexcept;

    // EvaluateChannel：判定某个通道在某个范围下此刻能否使用，顺序见文件头。
    // 传入：scope 当前范围；channel 想用的通道；inputs 运行期输入。
    // 传出：GateVerdict。不修改任何状态，不会换成别的通道。
    GateVerdict EvaluateChannel(Scope scope, Channel channel, const GateInputs& inputs) noexcept;

    // DefaultChannelFor：某个范围的默认通道。
    // 传入：scope 范围。传出：进程->UserMode；内核/物理->StandardDriver；越界范围返回 UserMode
    // （权限最低的一档，与 MemoryTargetSession 的默认一致）。Ddma 永远不会是默认。
    Channel DefaultChannelFor(Scope scope) noexcept;

    // GateReasonKey：原因的稳定 ASCII 键，形如 "driver-not-loaded"。
    // 传入：reason 原因。传出：静态字符串（不可释放）；越界值返回 "invalid-request"。
    // 用途：日志、测试诊断、界面词条键的拼接；它不是给用户看的文案。
    const char* GateReasonKey(GateReason reason) noexcept;

    // ChannelMemory：每个范围"上次使用的通道"的显式记忆。
    class ChannelMemory
    {
    public:
        // 构造：三个范围各放默认通道。
        ChannelMemory() noexcept;

        // Recall：取某范围记住的通道。传入：scope。传出：记住的通道；越界范围返回 UserMode。
        Channel Recall(Scope scope) const noexcept;

        // Remember：记住用户的显式选择。
        // 传入：scope 范围；channel 用户选的通道。
        // 传出：true 表示已记住；范围或通道越界、或范围不支持该通道（ChannelSupportsScope 为假）
        //       时返回 false 且原记忆不变。**不**检查此刻可用与否：不可用的通道照样记住，
        //       由界面报红，这是"不回退"的体现。Ddma 经用户显式选择可以被记住。
        bool Remember(Scope scope, Channel channel) noexcept;

        // Restore：从持久化的整数恢复某范围的记忆（启动时用）。
        // 传入：scope 范围；persistedValue 读到的整数。
        // 传出：true 表示采用了读到的值；越界、范围不支持、或是 Ddma（DDMA 永不作启动默认）
        //       时记成该范围的默认通道并返回 false。这是读盘时的数据清洗，不是运行期回退。
        bool Restore(Scope scope, std::uint32_t persistedValue) noexcept;

        // Reset：三个范围全部恢复默认通道。
        void Reset() noexcept;

    private:
        // remembered_：按 Scope 数值下标存放的记忆，三项。
        Channel remembered_[3];
    };
}
