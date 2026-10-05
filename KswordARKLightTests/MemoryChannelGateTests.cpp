// 内存工作台通道闸门（shared/evidence/memory_workbench/MemoryChannelGate.h）的离线测试。
//
// 为什么这个模块值得一整套穷举断言：它属于**判错了不会报错**的那一类。
// 闸门把一个本该置灰的通道放行，用户点下去得到的是一个看不出原因的读失败；
// 闸门把一个本可用的通道置灰，用户就失去了唯一的入口。更隐蔽的是"悄悄换通道"：
// 旧 refreshBackendSelectors 在 DDMA 不可用时静默弹回别的通道，于是用户以为在用 R0 读，
// 实际走的是 R3。具体到本模块有四处：
//   * R3 被放行到内核/物理范围（后端对它有显式拒绝分支，读出来必败）；
//   * 未探测的 HVM 被当成"不可用"置灰（探测是异步的，等于替用户做了没有依据的决定）；
//   * 驱动没加载时 HVM/DDMA 的陈旧缓存把它们放行；
//   * 记忆表在通道不可用时把选择改成别的通道。
//
// 断言原则与 MemoryTargetSessionTests.cpp 一致：
//   * 期望值手算写死（含 288 种输入组合里"可用"的总数 77 与"未知"的总数 10）；
//   * 每个范围 x 通道 x 输入组合的表都完整走一遍，判据顺序（范围->pid->通道条件）逐行钉住；
//   * 拒绝路径显式测，越界枚举都不能被放行；
//   * 断言没有任何函数会返回一个与请求不同的通道。

#include "TestSupport.h"

#include "../shared/evidence/memory_workbench/MemoryChannelGate.h"

#include <cstdint>
#include <cstring>
#include <set>
#include <string>

namespace {

using ksword::memwb::Channel;
using ksword::memwb::ChannelMemory;
using ksword::memwb::ChannelSupportsScope;
using ksword::memwb::DefaultChannelFor;
using ksword::memwb::EvaluateChannel;
using ksword::memwb::GateInputs;
using ksword::memwb::GateReason;
using ksword::memwb::GateReasonKey;
using ksword::memwb::GateVerdict;
using ksword::memwb::ProbeState;
using ksword::memwb::Scope;

// 三个范围、四个通道、三种探测状态的全集，供穷举循环使用。
constexpr Scope kScopes[] = { Scope::ProcessVirtual, Scope::KernelVirtual, Scope::Physical };
constexpr Channel kChannels[] = { Channel::UserMode, Channel::StandardDriver, Channel::Hvm, Channel::Ddma };
constexpr ProbeState kProbes[] = { ProbeState::NotProbed, ProbeState::Usable, ProbeState::Unusable };

// MakeInputs：按四个输入造一个 GateInputs。
// 传入：hasPid 是否有目标进程；driver 驱动是否加载；probe HVM 探测缓存；ddmaReady DDMA 会话是否就绪。
GateInputs MakeInputs(const bool hasPid, const bool driver, const ProbeState probe, const bool ddmaReady) {
    GateInputs inputs;
    inputs.hasProcessTarget = hasPid;
    inputs.driverLoaded = driver;
    inputs.hvmProbe = probe;
    inputs.ddmaSessionReady = ddmaReady;
    return inputs;
}

// ------------------------------------------------------------
// 一、枚举数值与默认值：数值会被界面层与词条键依赖，必须钉死。
// ------------------------------------------------------------
void TestEnumValuesAndDefaults(KswordTests::Suite& suite) {
    suite.expect(static_cast<std::uint32_t>(ProbeState::NotProbed) == 0U, L"gate: NotProbed is 0");
    suite.expect(static_cast<std::uint32_t>(ProbeState::Usable) == 1U, L"gate: Usable is 1");
    suite.expect(static_cast<std::uint32_t>(ProbeState::Unusable) == 2U, L"gate: Unusable is 2");

    suite.expect(static_cast<std::uint32_t>(GateReason::None) == 0U, L"gate: None is 0");
    suite.expect(static_cast<std::uint32_t>(GateReason::ScopeNotSupported) == 1U, L"gate: ScopeNotSupported is 1");
    suite.expect(static_cast<std::uint32_t>(GateReason::NeedsPid) == 2U, L"gate: NeedsPid is 2");
    suite.expect(static_cast<std::uint32_t>(GateReason::DriverNotLoaded) == 3U, L"gate: DriverNotLoaded is 3");
    suite.expect(static_cast<std::uint32_t>(GateReason::ProbeNotDone) == 4U, L"gate: ProbeNotDone is 4");
    suite.expect(static_cast<std::uint32_t>(GateReason::ProbeFailed) == 5U, L"gate: ProbeFailed is 5");
    suite.expect(static_cast<std::uint32_t>(GateReason::SessionNotReady) == 6U, L"gate: SessionNotReady is 6");
    suite.expect(static_cast<std::uint32_t>(GateReason::InvalidRequest) == 7U, L"gate: InvalidRequest is 7");

    // 默认输入是最保守的一档：没有进程、驱动未加载、HVM 未探测、DDMA 未就绪。
    const GateInputs defaults;
    suite.expect(!defaults.hasProcessTarget, L"gate: default inputs have no process target");
    suite.expect(!defaults.driverLoaded, L"gate: default inputs have no driver");
    suite.expect(defaults.hvmProbe == ProbeState::NotProbed, L"gate: default inputs have not probed hvm");
    suite.expect(!defaults.ddmaSessionReady, L"gate: default inputs have no ddma session");

    // 默认结论是"不可用 + 无效请求"：失败时的输出保持安全初值，绝不是"可用"。
    const GateVerdict defaultVerdict;
    suite.expect(!defaultVerdict.available, L"gate: a default verdict is not available");
    suite.expect(defaultVerdict.reason == GateReason::InvalidRequest, L"gate: a default verdict is InvalidRequest");
    suite.expect(!defaultVerdict.IsUnknown(), L"gate: a default verdict is not unknown");

    // IsUnknown 只在"可用且 ProbeNotDone"时为真。
    GateVerdict unknown;
    unknown.available = true;
    unknown.reason = GateReason::ProbeNotDone;
    suite.expect(unknown.IsUnknown(), L"gate: available with ProbeNotDone is unknown");
    GateVerdict confirmed;
    confirmed.available = true;
    confirmed.reason = GateReason::None;
    suite.expect(!confirmed.IsUnknown(), L"gate: available with None is confirmed, not unknown");
    GateVerdict blocked;
    blocked.available = false;
    blocked.reason = GateReason::ProbeNotDone;
    suite.expect(!blocked.IsUnknown(), L"gate: an unavailable verdict is never unknown");
}

// ------------------------------------------------------------
// 二、ChannelSupportsScope：十二个格子手写，越界值全部拒绝。
// ------------------------------------------------------------
void TestChannelSupportsScope(KswordTests::Suite& suite) {
    // R3：只有进程范围。
    suite.expect(ChannelSupportsScope(Channel::UserMode, Scope::ProcessVirtual), L"support: R3 supports process");
    suite.expect(!ChannelSupportsScope(Channel::UserMode, Scope::KernelVirtual), L"support: R3 rejects kernel");
    suite.expect(!ChannelSupportsScope(Channel::UserMode, Scope::Physical), L"support: R3 rejects physical");
    // R0、HVM、DDMA：三个范围都支持。
    suite.expect(ChannelSupportsScope(Channel::StandardDriver, Scope::ProcessVirtual), L"support: R0 process");
    suite.expect(ChannelSupportsScope(Channel::StandardDriver, Scope::KernelVirtual), L"support: R0 kernel");
    suite.expect(ChannelSupportsScope(Channel::StandardDriver, Scope::Physical), L"support: R0 physical");
    suite.expect(ChannelSupportsScope(Channel::Hvm, Scope::ProcessVirtual), L"support: HVM process");
    suite.expect(ChannelSupportsScope(Channel::Hvm, Scope::KernelVirtual), L"support: HVM kernel");
    suite.expect(ChannelSupportsScope(Channel::Hvm, Scope::Physical), L"support: HVM physical");
    suite.expect(ChannelSupportsScope(Channel::Ddma, Scope::ProcessVirtual), L"support: DDMA process");
    suite.expect(ChannelSupportsScope(Channel::Ddma, Scope::KernelVirtual), L"support: DDMA kernel");
    suite.expect(ChannelSupportsScope(Channel::Ddma, Scope::Physical), L"support: DDMA physical");

    // 越界值：紧邻合法区间的值与最大值，对任一参数都拒绝。
    suite.expect(!ChannelSupportsScope(static_cast<Channel>(4), Scope::ProcessVirtual),
        L"support: channel value 4 is rejected");
    suite.expect(!ChannelSupportsScope(static_cast<Channel>(0xFFFFFFFFU), Scope::KernelVirtual),
        L"support: the largest channel value is rejected");
    suite.expect(!ChannelSupportsScope(Channel::StandardDriver, static_cast<Scope>(3)),
        L"support: scope value 3 is rejected");
    suite.expect(!ChannelSupportsScope(Channel::Hvm, static_cast<Scope>(0xFFFFFFFFU)),
        L"support: the largest scope value is rejected");
    suite.expect(!ChannelSupportsScope(static_cast<Channel>(4), static_cast<Scope>(3)),
        L"support: two bad values are rejected together");
}

// ------------------------------------------------------------
// 三、DefaultChannelFor：进程->R3，内核/物理->R0，DDMA 永不作默认。
// ------------------------------------------------------------
void TestDefaultChannels(KswordTests::Suite& suite) {
    suite.expect(DefaultChannelFor(Scope::ProcessVirtual) == Channel::UserMode,
        L"default: the process scope defaults to R3");
    suite.expect(DefaultChannelFor(Scope::KernelVirtual) == Channel::StandardDriver,
        L"default: the kernel scope defaults to R0");
    suite.expect(DefaultChannelFor(Scope::Physical) == Channel::StandardDriver,
        L"default: the physical scope defaults to R0");

    // DDMA 永远不是默认；默认通道一定被它自己的范围支持。
    for (const Scope scope : kScopes) {
        suite.expect(DefaultChannelFor(scope) != Channel::Ddma, L"default: ddma is never a default");
        suite.expect(ChannelSupportsScope(DefaultChannelFor(scope), scope),
            L"default: the default channel is supported by its own scope");
    }

    // 越界范围落到权限最低的一档，同样不是 DDMA。
    suite.expect(DefaultChannelFor(static_cast<Scope>(3)) == Channel::UserMode,
        L"default: an out-of-range scope falls to the least privileged channel");
    suite.expect(DefaultChannelFor(static_cast<Scope>(0xFFFFFFFFU)) == Channel::UserMode,
        L"default: the largest scope value falls to the least privileged channel");
}

// ------------------------------------------------------------
// 四、手写的判据表：判据顺序逐行钉住。
// ------------------------------------------------------------

// GateRow：一行表项 = 输入 + 期望结论 + 断言标签。
struct GateRow {
    Scope scope;
    Channel channel;
    bool hasPid;
    bool driver;
    ProbeState probe;
    bool ddmaReady;
    bool expectAvailable;
    GateReason expectReason;
    const wchar_t* label;
};

const GateRow kRows[] = {
    // ---- 进程范围 / R3：只依赖 pid，与驱动无关 ----
    { Scope::ProcessVirtual, Channel::UserMode, false, true, ProbeState::Usable, true, false, GateReason::NeedsPid,
      L"row: process R3 without a pid needs a pid even with everything else ready" },
    { Scope::ProcessVirtual, Channel::UserMode, true, false, ProbeState::NotProbed, false, true, GateReason::None,
      L"row: process R3 with a pid is available even with no driver" },
    { Scope::ProcessVirtual, Channel::UserMode, true, true, ProbeState::Unusable, true, true, GateReason::None,
      L"row: process R3 ignores the hvm probe and the ddma session" },
    // ---- 进程范围 / R0 ----
    { Scope::ProcessVirtual, Channel::StandardDriver, false, true, ProbeState::Usable, true, false, GateReason::NeedsPid,
      L"row: process R0 without a pid needs a pid" },
    { Scope::ProcessVirtual, Channel::StandardDriver, false, false, ProbeState::NotProbed, false, false, GateReason::NeedsPid,
      L"row: process R0 without a pid and without a driver reports the pid first" },
    { Scope::ProcessVirtual, Channel::StandardDriver, true, false, ProbeState::Usable, true, false, GateReason::DriverNotLoaded,
      L"row: process R0 without a driver is blocked" },
    { Scope::ProcessVirtual, Channel::StandardDriver, true, true, ProbeState::Unusable, false, true, GateReason::None,
      L"row: process R0 with a driver is available regardless of hvm and ddma state" },
    // ---- 进程范围 / HVM ----
    { Scope::ProcessVirtual, Channel::Hvm, false, true, ProbeState::Usable, true, false, GateReason::NeedsPid,
      L"row: process HVM without a pid needs a pid" },
    { Scope::ProcessVirtual, Channel::Hvm, true, false, ProbeState::Usable, true, false, GateReason::DriverNotLoaded,
      L"row: process HVM without a driver is blocked even if a stale probe said usable" },
    { Scope::ProcessVirtual, Channel::Hvm, true, false, ProbeState::NotProbed, false, false, GateReason::DriverNotLoaded,
      L"row: process HVM without a driver reports the driver before the probe" },
    { Scope::ProcessVirtual, Channel::Hvm, true, true, ProbeState::NotProbed, false, true, GateReason::ProbeNotDone,
      L"row: process HVM that is not probed yet is available but unknown" },
    { Scope::ProcessVirtual, Channel::Hvm, true, true, ProbeState::Unusable, true, false, GateReason::ProbeFailed,
      L"row: process HVM whose probe failed is blocked" },
    { Scope::ProcessVirtual, Channel::Hvm, true, true, ProbeState::Usable, false, true, GateReason::None,
      L"row: process HVM whose probe passed is available regardless of ddma" },
    // ---- 进程范围 / DDMA ----
    { Scope::ProcessVirtual, Channel::Ddma, false, true, ProbeState::Usable, true, false, GateReason::NeedsPid,
      L"row: process DDMA without a pid needs a pid" },
    { Scope::ProcessVirtual, Channel::Ddma, true, false, ProbeState::Usable, true, false, GateReason::DriverNotLoaded,
      L"row: process DDMA without a driver is blocked even with a ready session" },
    { Scope::ProcessVirtual, Channel::Ddma, true, false, ProbeState::Usable, false, false, GateReason::DriverNotLoaded,
      L"row: process DDMA without a driver reports the driver before the session" },
    { Scope::ProcessVirtual, Channel::Ddma, true, true, ProbeState::Usable, false, false, GateReason::SessionNotReady,
      L"row: process DDMA with a driver but no session is blocked" },
    { Scope::ProcessVirtual, Channel::Ddma, true, true, ProbeState::Unusable, true, true, GateReason::None,
      L"row: process DDMA with a driver and a session is available regardless of hvm" },
    // ---- 内核范围：pid 无关；R3 被范围拒绝 ----
    { Scope::KernelVirtual, Channel::UserMode, false, false, ProbeState::NotProbed, false, false, GateReason::ScopeNotSupported,
      L"row: kernel R3 is rejected by the scope with nothing else ready" },
    { Scope::KernelVirtual, Channel::UserMode, true, true, ProbeState::Usable, true, false, GateReason::ScopeNotSupported,
      L"row: kernel R3 is rejected by the scope even with everything ready" },
    { Scope::KernelVirtual, Channel::StandardDriver, false, false, ProbeState::NotProbed, false, false, GateReason::DriverNotLoaded,
      L"row: kernel R0 without a driver is blocked and a missing pid is irrelevant" },
    { Scope::KernelVirtual, Channel::StandardDriver, false, true, ProbeState::NotProbed, false, true, GateReason::None,
      L"row: kernel R0 with a driver is available without any pid" },
    { Scope::KernelVirtual, Channel::StandardDriver, true, true, ProbeState::Unusable, true, true, GateReason::None,
      L"row: kernel R0 with a driver is available with a pid too" },
    { Scope::KernelVirtual, Channel::Hvm, false, false, ProbeState::Usable, true, false, GateReason::DriverNotLoaded,
      L"row: kernel HVM without a driver is blocked" },
    { Scope::KernelVirtual, Channel::Hvm, false, true, ProbeState::NotProbed, false, true, GateReason::ProbeNotDone,
      L"row: kernel HVM not probed yet is available but unknown without any pid" },
    { Scope::KernelVirtual, Channel::Hvm, true, true, ProbeState::Unusable, true, false, GateReason::ProbeFailed,
      L"row: kernel HVM whose probe failed is blocked" },
    { Scope::KernelVirtual, Channel::Hvm, false, true, ProbeState::Usable, false, true, GateReason::None,
      L"row: kernel HVM whose probe passed is available" },
    { Scope::KernelVirtual, Channel::Ddma, false, false, ProbeState::Usable, true, false, GateReason::DriverNotLoaded,
      L"row: kernel DDMA without a driver is blocked" },
    { Scope::KernelVirtual, Channel::Ddma, false, true, ProbeState::Usable, false, false, GateReason::SessionNotReady,
      L"row: kernel DDMA without a session is blocked" },
    { Scope::KernelVirtual, Channel::Ddma, false, true, ProbeState::NotProbed, true, true, GateReason::None,
      L"row: kernel DDMA with a driver and a session is available without any pid" },
    // ---- 物理范围：与内核范围同一套规则 ----
    { Scope::Physical, Channel::UserMode, true, true, ProbeState::Usable, true, false, GateReason::ScopeNotSupported,
      L"row: physical R3 is rejected by the scope" },
    { Scope::Physical, Channel::StandardDriver, false, false, ProbeState::Usable, true, false, GateReason::DriverNotLoaded,
      L"row: physical R0 without a driver is blocked" },
    { Scope::Physical, Channel::StandardDriver, false, true, ProbeState::NotProbed, false, true, GateReason::None,
      L"row: physical R0 with a driver is available without any pid" },
    { Scope::Physical, Channel::Hvm, false, true, ProbeState::NotProbed, true, true, GateReason::ProbeNotDone,
      L"row: physical HVM not probed yet is available but unknown" },
    { Scope::Physical, Channel::Hvm, false, true, ProbeState::Unusable, true, false, GateReason::ProbeFailed,
      L"row: physical HVM whose probe failed is blocked" },
    { Scope::Physical, Channel::Hvm, false, false, ProbeState::NotProbed, true, false, GateReason::DriverNotLoaded,
      L"row: physical HVM without a driver reports the driver before the probe" },
    { Scope::Physical, Channel::Ddma, false, true, ProbeState::Usable, false, false, GateReason::SessionNotReady,
      L"row: physical DDMA without a session is blocked" },
    { Scope::Physical, Channel::Ddma, false, true, ProbeState::Usable, true, true, GateReason::None,
      L"row: physical DDMA with a driver and a session is available" },
};

void TestHandWrittenRows(KswordTests::Suite& suite) {
    for (const GateRow& row : kRows) {
        const GateVerdict verdict = EvaluateChannel(row.scope, row.channel,
            MakeInputs(row.hasPid, row.driver, row.probe, row.ddmaReady));
        suite.expect(verdict.available == row.expectAvailable && verdict.reason == row.expectReason, row.label);
    }

    // 默认输入（全保守）下三个范围各通道的结论：进程范围一律缺 pid，内核/物理 R3 被范围拒，
    // 其余驱动类缺驱动。
    const GateInputs defaults;
    for (const Channel channel : kChannels) {
        suite.expect(EvaluateChannel(Scope::ProcessVirtual, channel, defaults).reason == GateReason::NeedsPid,
            L"row: with default inputs every process channel needs a pid");
    }
    suite.expect(EvaluateChannel(Scope::KernelVirtual, Channel::UserMode, defaults).reason
            == GateReason::ScopeNotSupported,
        L"row: with default inputs kernel R3 is rejected by the scope");
    suite.expect(EvaluateChannel(Scope::Physical, Channel::Hvm, defaults).reason == GateReason::DriverNotLoaded,
        L"row: with default inputs physical HVM lacks the driver");
}

// ------------------------------------------------------------
// 五、288 种输入组合的穷举：性质 + 手算的总数。
// ------------------------------------------------------------
void TestExhaustiveSweep(KswordTests::Suite& suite) {
    // 逐 (范围, 通道) 统计"可用"数；总数手算见下。
    int availableByCell[3][4] = {};
    int unknownTotal = 0;
    int availableTotal = 0;
    int combos = 0;

    // 性质违例计数，循环结束后各报告一次。
    int violationUserModeOutsideProcess = 0;
    int violationBlockedReason = 0;
    int violationAvailableReason = 0;
    int violationUnknownOnlyHvm = 0;
    int violationIrrelevantInputs = 0;
    int violationPidOutsideProcess = 0;
    int violationDriverMonotonic = 0;

    for (std::uint32_t scopeIndex = 0; scopeIndex < 3U; ++scopeIndex) {
        const Scope scope = kScopes[scopeIndex];
        for (std::uint32_t channelIndex = 0; channelIndex < 4U; ++channelIndex) {
            const Channel channel = kChannels[channelIndex];
            for (int mask = 0; mask < 24; ++mask) {
                // 24 种组合 = pid(2) x driver(2) x probe(3) x ddmaReady(2)。
                const bool hasPid = (mask & 1) != 0;
                const bool driver = (mask & 2) != 0;
                const ProbeState probe = kProbes[(mask >> 2) % 3];
                const bool ddmaReady = ((mask >> 2) / 3) != 0;
                ++combos;
                const GateVerdict verdict = EvaluateChannel(scope, channel, MakeInputs(hasPid, driver, probe, ddmaReady));

                // 统计。
                if (verdict.available) {
                    ++availableTotal;
                    ++availableByCell[scopeIndex][channelIndex];
                }
                if (verdict.IsUnknown()) {
                    ++unknownTotal;
                }

                // R3 在非进程范围恒为 ScopeNotSupported。
                if (channel == Channel::UserMode && scope != Scope::ProcessVirtual
                    && (verdict.available || verdict.reason != GateReason::ScopeNotSupported)) {
                    ++violationUserModeOutsideProcess;
                }
                // 不可用时原因必须是阻断原因（不能是 None，也不能是只用于"未知"的 ProbeNotDone）。
                if (!verdict.available
                    && (verdict.reason == GateReason::None || verdict.reason == GateReason::ProbeNotDone)) {
                    ++violationBlockedReason;
                }
                // 可用时原因只能是 None 或 ProbeNotDone。
                if (verdict.available
                    && (verdict.reason != GateReason::None && verdict.reason != GateReason::ProbeNotDone)) {
                    ++violationAvailableReason;
                }
                // "未知"只可能出现在 HVM 通道且探测状态为 NotProbed。
                if (verdict.IsUnknown() && (channel != Channel::Hvm || probe != ProbeState::NotProbed)) {
                    ++violationUnknownOnlyHvm;
                }
                // 与通道无关的输入不得影响结论：把无关输入换成另一组，结论必须相同。
                // R3 不依赖驱动，所以它的驱动输入也取反；HVM 之外换探测状态；DDMA 之外换会话就绪。
                const GateVerdict alternate = EvaluateChannel(scope, channel,
                    MakeInputs(hasPid,
                        (channel == Channel::UserMode) ? !driver : driver,
                        (channel == Channel::Hvm) ? probe : ProbeState::Unusable,
                        (channel == Channel::Ddma) ? ddmaReady : true));
                if (alternate.available != verdict.available || alternate.reason != verdict.reason) {
                    ++violationIrrelevantInputs;
                }
                // 非进程范围下 pid 无关：把 pid 取反结论不变。
                if (scope != Scope::ProcessVirtual) {
                    const GateVerdict flipped = EvaluateChannel(scope, channel,
                        MakeInputs(!hasPid, driver, probe, ddmaReady));
                    if (flipped.available != verdict.available || flipped.reason != verdict.reason) {
                        ++violationPidOutsideProcess;
                    }
                }
                // 单调：驱动加载只会让通道更可用，不会让原本可用的变成不可用。
                if (!driver) {
                    const GateVerdict withDriver = EvaluateChannel(scope, channel,
                        MakeInputs(hasPid, true, probe, ddmaReady));
                    if (verdict.available && !withDriver.available) {
                        ++violationDriverMonotonic;
                    }
                }
            }
        }
    }

    suite.expect(combos == 288, L"sweep: 3 scopes x 4 channels x 24 input combinations is 288 verdicts");
    suite.expect(violationUserModeOutsideProcess == 0, L"sweep: R3 outside the process scope is never available");
    suite.expect(violationBlockedReason == 0, L"sweep: an unavailable verdict always carries a blocking reason");
    suite.expect(violationAvailableReason == 0, L"sweep: an available verdict only carries None or ProbeNotDone");
    suite.expect(violationUnknownOnlyHvm == 0, L"sweep: unknown only ever comes from an unprobed HVM");
    suite.expect(violationIrrelevantInputs == 0, L"sweep: inputs that do not concern a channel never change its verdict");
    suite.expect(violationPidOutsideProcess == 0, L"sweep: the pid is irrelevant outside the process scope");
    suite.expect(violationDriverMonotonic == 0, L"sweep: loading the driver never makes a channel less available");

    // 手算的总数。每格的推导见注释：
    //   进程/R3  = pid 为真：driver(2) x probe(3) x ddma(2) = 12
    //   进程/R0  = pid 真且 driver 真：probe(3) x ddma(2) = 6
    //   进程/HVM = pid 真、driver 真、探测非 Unusable(2)、ddma(2) = 4
    //   进程/DDMA= pid 真、driver 真、ddma 真：probe(3) = 3
    //   内核或物理 / R3 = 0
    //   内核或物理 / R0 = driver 真：pid(2) x probe(3) x ddma(2) = 12
    //   内核或物理 / HVM = driver 真、探测非 Unusable(2)：pid(2) x 2 x ddma(2) = 8
    //   内核或物理 / DDMA = driver 真、ddma 真：pid(2) x probe(3) = 6
    suite.expect(availableByCell[0][0] == 12, L"sweep: process R3 is available in 12 combinations");
    suite.expect(availableByCell[0][1] == 6, L"sweep: process R0 is available in 6 combinations");
    suite.expect(availableByCell[0][2] == 4, L"sweep: process HVM is available in 4 combinations");
    suite.expect(availableByCell[0][3] == 3, L"sweep: process DDMA is available in 3 combinations");
    for (std::uint32_t scopeIndex = 1; scopeIndex < 3U; ++scopeIndex) {
        suite.expect(availableByCell[scopeIndex][0] == 0, L"sweep: kernel and physical R3 are never available");
        suite.expect(availableByCell[scopeIndex][1] == 12, L"sweep: kernel and physical R0 are available in 12 combinations");
        suite.expect(availableByCell[scopeIndex][2] == 8, L"sweep: kernel and physical HVM are available in 8 combinations");
        suite.expect(availableByCell[scopeIndex][3] == 6, L"sweep: kernel and physical DDMA are available in 6 combinations");
    }
    // 总数 12+6+4+3 + 2 x (0+12+8+6) = 25 + 52 = 77。
    suite.expect(availableTotal == 77, L"sweep: 77 of the 288 verdicts are available");
    // "未知"：进程 HVM 2 个（pid 真、driver 真、NotProbed、ddma 两种）+ 内核/物理各 4 个
    // （driver 真、NotProbed、pid 两种 x ddma 两种）= 2 + 4 + 4 = 10。
    suite.expect(unknownTotal == 10, L"sweep: 10 of the available verdicts are unknown");
}

// ------------------------------------------------------------
// 六、越界枚举与越界探测状态：不能被放行。
// ------------------------------------------------------------
void TestInvalidValues(KswordTests::Suite& suite) {
    const GateInputs ready = MakeInputs(true, true, ProbeState::Usable, true);

    // 范围越界：对每个合法通道都是 InvalidRequest 且不可用。
    for (const Channel channel : kChannels) {
        const GateVerdict badScope = EvaluateChannel(static_cast<Scope>(3), channel, ready);
        suite.expect(!badScope.available && badScope.reason == GateReason::InvalidRequest,
            L"invalid: scope value 3 is rejected for every channel");
        const GateVerdict hugeScope = EvaluateChannel(static_cast<Scope>(0xFFFFFFFFU), channel, ready);
        suite.expect(!hugeScope.available && hugeScope.reason == GateReason::InvalidRequest,
            L"invalid: the largest scope value is rejected for every channel");
    }

    // 通道越界：对每个合法范围都是 InvalidRequest 且不可用。
    for (const Scope scope : kScopes) {
        const GateVerdict badChannel = EvaluateChannel(scope, static_cast<Channel>(4), ready);
        suite.expect(!badChannel.available && badChannel.reason == GateReason::InvalidRequest,
            L"invalid: channel value 4 is rejected for every scope");
        const GateVerdict hugeChannel = EvaluateChannel(scope, static_cast<Channel>(0xFFFFFFFFU), ready);
        suite.expect(!hugeChannel.available && hugeChannel.reason == GateReason::InvalidRequest,
            L"invalid: the largest channel value is rejected for every scope");
    }

    // 两个都越界：同样拒绝。
    const GateVerdict bothBad = EvaluateChannel(static_cast<Scope>(3), static_cast<Channel>(4), ready);
    suite.expect(!bothBad.available && bothBad.reason == GateReason::InvalidRequest,
        L"invalid: two out-of-range values are rejected");

    // 探测状态越界：只对 HVM 有意义，HVM 拒绝；其它通道不读它，不受影响。
    GateInputs badProbe = ready;
    badProbe.hvmProbe = static_cast<ProbeState>(3);
    const GateVerdict hvmBadProbe = EvaluateChannel(Scope::KernelVirtual, Channel::Hvm, badProbe);
    suite.expect(!hvmBadProbe.available && hvmBadProbe.reason == GateReason::InvalidRequest,
        L"invalid: a bad probe state makes HVM unavailable instead of guessing");
    suite.expect(EvaluateChannel(Scope::KernelVirtual, Channel::StandardDriver, badProbe).available,
        L"invalid: a bad probe state does not affect R0");
    suite.expect(EvaluateChannel(Scope::Physical, Channel::Ddma, badProbe).available,
        L"invalid: a bad probe state does not affect DDMA");
    // 探测状态越界时驱动未加载仍先报驱动：顺序不被越界值打乱。
    badProbe.driverLoaded = false;
    suite.expect(EvaluateChannel(Scope::KernelVirtual, Channel::Hvm, badProbe).reason == GateReason::DriverNotLoaded,
        L"invalid: the driver is still reported before an invalid probe state");
}

// ------------------------------------------------------------
// 七、GateReasonKey：八个键整串写死、互不相同，越界值有兜底。
// ------------------------------------------------------------
void TestReasonKeys(KswordTests::Suite& suite) {
    suite.expect(std::strcmp(GateReasonKey(GateReason::None), "none") == 0, L"key: None");
    suite.expect(std::strcmp(GateReasonKey(GateReason::ScopeNotSupported), "scope-not-supported") == 0,
        L"key: ScopeNotSupported");
    suite.expect(std::strcmp(GateReasonKey(GateReason::NeedsPid), "needs-pid") == 0, L"key: NeedsPid");
    suite.expect(std::strcmp(GateReasonKey(GateReason::DriverNotLoaded), "driver-not-loaded") == 0,
        L"key: DriverNotLoaded");
    suite.expect(std::strcmp(GateReasonKey(GateReason::ProbeNotDone), "probe-not-done") == 0, L"key: ProbeNotDone");
    suite.expect(std::strcmp(GateReasonKey(GateReason::ProbeFailed), "probe-failed") == 0, L"key: ProbeFailed");
    suite.expect(std::strcmp(GateReasonKey(GateReason::SessionNotReady), "session-not-ready") == 0,
        L"key: SessionNotReady");
    suite.expect(std::strcmp(GateReasonKey(GateReason::InvalidRequest), "invalid-request") == 0,
        L"key: InvalidRequest");
    suite.expect(std::strcmp(GateReasonKey(static_cast<GateReason>(8)), "invalid-request") == 0,
        L"key: an out-of-range reason falls back to invalid-request");
    suite.expect(std::strcmp(GateReasonKey(static_cast<GateReason>(0xFFFFFFFFU)), "invalid-request") == 0,
        L"key: the largest reason value falls back to invalid-request");

    // 八个合法原因的键两两不同。
    std::set<std::string> keys;
    for (std::uint32_t value = 0; value < 8U; ++value) {
        keys.insert(GateReasonKey(static_cast<GateReason>(value)));
    }
    suite.expect(keys.size() == 8U, L"key: the eight reasons have eight different keys");
}

// ------------------------------------------------------------
// 八、ChannelMemory：显式记忆、不回退、读盘清洗。
// ------------------------------------------------------------
void TestChannelMemory(KswordTests::Suite& suite) {
    ChannelMemory memory;

    // 初始：各范围放默认通道。
    suite.expect(memory.Recall(Scope::ProcessVirtual) == Channel::UserMode, L"memory: process starts at R3");
    suite.expect(memory.Recall(Scope::KernelVirtual) == Channel::StandardDriver, L"memory: kernel starts at R0");
    suite.expect(memory.Recall(Scope::Physical) == Channel::StandardDriver, L"memory: physical starts at R0");

    // 记住与召回；不同范围互不影响。
    suite.expect(memory.Remember(Scope::KernelVirtual, Channel::Hvm), L"memory: remembering HVM for kernel works");
    suite.expect(memory.Recall(Scope::KernelVirtual) == Channel::Hvm, L"memory: kernel now recalls HVM");
    suite.expect(memory.Recall(Scope::ProcessVirtual) == Channel::UserMode, L"memory: process is untouched");
    suite.expect(memory.Recall(Scope::Physical) == Channel::StandardDriver, L"memory: physical is untouched");

    // 不回退：驱动没加载、HVM 未探测——闸门说它"不可用"，但记忆仍保持 HVM，由界面报红。
    const GateInputs unloaded = MakeInputs(false, false, ProbeState::NotProbed, false);
    suite.expect(!EvaluateChannel(Scope::KernelVirtual, Channel::Hvm, unloaded).available,
        L"memory: the gate reports the remembered channel as unavailable");
    suite.expect(memory.Recall(Scope::KernelVirtual) == Channel::Hvm,
        L"memory: an unavailable channel stays selected, nothing is substituted");

    // 经用户显式选择，DDMA 可以被记住（"永不作默认"只约束默认值，不约束用户的选择）。
    suite.expect(memory.Remember(Scope::ProcessVirtual, Channel::Ddma), L"memory: an explicit ddma choice is remembered");
    suite.expect(memory.Recall(Scope::ProcessVirtual) == Channel::Ddma, L"memory: process recalls the explicit ddma");

    // 范围不支持的通道：拒绝，原记忆不变。
    suite.expect(!memory.Remember(Scope::KernelVirtual, Channel::UserMode),
        L"memory: remembering R3 for kernel is rejected");
    suite.expect(memory.Recall(Scope::KernelVirtual) == Channel::Hvm, L"memory: a rejected remember keeps the old value");
    suite.expect(!memory.Remember(Scope::Physical, Channel::UserMode), L"memory: remembering R3 for physical is rejected");
    suite.expect(memory.Recall(Scope::Physical) == Channel::StandardDriver,
        L"memory: a rejected remember keeps physical's old value");

    // 越界值：拒绝，记忆不变。
    suite.expect(!memory.Remember(static_cast<Scope>(3), Channel::StandardDriver), L"memory: a bad scope is rejected");
    suite.expect(!memory.Remember(Scope::Physical, static_cast<Channel>(4)), L"memory: a bad channel is rejected");
    suite.expect(memory.Recall(Scope::Physical) == Channel::StandardDriver,
        L"memory: a bad channel did not change physical");
    suite.expect(memory.Recall(static_cast<Scope>(3)) == Channel::UserMode,
        L"memory: recalling a bad scope gives the least privileged channel");

    // Reset：全部回默认。
    memory.Reset();
    suite.expect(memory.Recall(Scope::ProcessVirtual) == Channel::UserMode, L"memory: reset restores process");
    suite.expect(memory.Recall(Scope::KernelVirtual) == Channel::StandardDriver, L"memory: reset restores kernel");
    suite.expect(memory.Recall(Scope::Physical) == Channel::StandardDriver, L"memory: reset restores physical");
}

// ------------------------------------------------------------
// 九、"没有任何函数返回与请求不同的通道"：全部 (范围, 通道) 穷举。
// ------------------------------------------------------------
void TestNoSubstitution(KswordTests::Suite& suite) {
    int violationAcceptedButDifferent = 0;
    int violationRejectedButChanged = 0;
    int violationAcceptanceMismatch = 0;

    for (const Scope scope : kScopes) {
        for (const Channel requested : kChannels) {
            ChannelMemory memory;
            const Channel before = memory.Recall(scope);
            const bool accepted = memory.Remember(scope, requested);
            const Channel after = memory.Recall(scope);

            // 接受与否必须与静态支持表一致。
            if (accepted != ChannelSupportsScope(requested, scope)) {
                ++violationAcceptanceMismatch;
            }
            // 接受了就必须是原样记住的那个通道。
            if (accepted && after != requested) {
                ++violationAcceptedButDifferent;
            }
            // 拒绝了就必须保持原值，不能悄悄换成别的。
            if (!accepted && after != before) {
                ++violationRejectedButChanged;
            }
        }
    }
    suite.expect(violationAcceptanceMismatch == 0, L"nosub: remembering succeeds exactly when the scope supports the channel");
    suite.expect(violationAcceptedButDifferent == 0, L"nosub: an accepted channel is remembered exactly as requested");
    suite.expect(violationRejectedButChanged == 0, L"nosub: a rejected request leaves the memory as it was");
}

// ------------------------------------------------------------
// 十、Restore：读盘时的数据清洗，DDMA 永不作启动默认。
// ------------------------------------------------------------
void TestRestore(KswordTests::Suite& suite) {
    ChannelMemory memory;

    // 设计文档里持久化默认值：进程=0、内核=1、物理=1，原样恢复。
    suite.expect(memory.Restore(Scope::ProcessVirtual, 0U), L"restore: process 0 is accepted");
    suite.expect(memory.Recall(Scope::ProcessVirtual) == Channel::UserMode, L"restore: process 0 is R3");
    suite.expect(memory.Restore(Scope::KernelVirtual, 1U), L"restore: kernel 1 is accepted");
    suite.expect(memory.Recall(Scope::KernelVirtual) == Channel::StandardDriver, L"restore: kernel 1 is R0");
    suite.expect(memory.Restore(Scope::Physical, 1U), L"restore: physical 1 is accepted");
    suite.expect(memory.Recall(Scope::Physical) == Channel::StandardDriver, L"restore: physical 1 is R0");

    // HVM（2）在三个范围都被接受。
    suite.expect(memory.Restore(Scope::ProcessVirtual, 2U), L"restore: process 2 is accepted");
    suite.expect(memory.Recall(Scope::ProcessVirtual) == Channel::Hvm, L"restore: process 2 is HVM");
    suite.expect(memory.Restore(Scope::KernelVirtual, 2U), L"restore: kernel 2 is accepted");
    suite.expect(memory.Recall(Scope::KernelVirtual) == Channel::Hvm, L"restore: kernel 2 is HVM");

    // 读到 3（DDMA）当默认处理：返回 false，记成该范围默认，三个范围都一样。
    suite.expect(!memory.Restore(Scope::ProcessVirtual, 3U), L"restore: process 3 (ddma) is not used");
    suite.expect(memory.Recall(Scope::ProcessVirtual) == Channel::UserMode, L"restore: process 3 becomes the default R3");
    suite.expect(!memory.Restore(Scope::KernelVirtual, 3U), L"restore: kernel 3 (ddma) is not used");
    suite.expect(memory.Recall(Scope::KernelVirtual) == Channel::StandardDriver,
        L"restore: kernel 3 becomes the default R0");
    suite.expect(!memory.Restore(Scope::Physical, 3U), L"restore: physical 3 (ddma) is not used");
    suite.expect(memory.Recall(Scope::Physical) == Channel::StandardDriver,
        L"restore: physical 3 becomes the default R0");

    // 越界整数：当默认。
    suite.expect(!memory.Restore(Scope::KernelVirtual, 4U), L"restore: 4 is rejected");
    suite.expect(!memory.Restore(Scope::ProcessVirtual, 0xFFFFFFFFU), L"restore: the largest value is rejected");
    suite.expect(memory.Recall(Scope::ProcessVirtual) == Channel::UserMode,
        L"restore: the largest value becomes the default");

    // 范围不支持的通道：内核/物理读到 0（R3）当默认。
    suite.expect(!memory.Restore(Scope::KernelVirtual, 0U), L"restore: kernel 0 (R3) is not supported");
    suite.expect(memory.Recall(Scope::KernelVirtual) == Channel::StandardDriver,
        L"restore: kernel 0 becomes the default R0");
    suite.expect(!memory.Restore(Scope::Physical, 0U), L"restore: physical 0 (R3) is not supported");
    suite.expect(memory.Recall(Scope::Physical) == Channel::StandardDriver,
        L"restore: physical 0 becomes the default R0");

    // 失败的恢复会把该范围记成默认（覆盖之前的记忆）；越界范围则什么都不动。
    suite.expect(memory.Remember(Scope::KernelVirtual, Channel::Hvm), L"restore setup: remember HVM");
    suite.expect(!memory.Restore(static_cast<Scope>(3), 1U), L"restore: a bad scope is rejected");
    suite.expect(memory.Recall(Scope::KernelVirtual) == Channel::Hvm,
        L"restore: a bad scope leaves every real scope untouched");
    suite.expect(!memory.Restore(Scope::KernelVirtual, 9U), L"restore: a bad value is rejected");
    suite.expect(memory.Recall(Scope::KernelVirtual) == Channel::StandardDriver,
        L"restore: a bad value resets that scope to its default");
}

} // namespace

int RunMemwbChannelGateTests() {
    KswordTests::Suite suite(L"MEMWB channel gate");
    TestEnumValuesAndDefaults(suite);
    TestChannelSupportsScope(suite);
    TestDefaultChannels(suite);
    TestHandWrittenRows(suite);
    TestExhaustiveSweep(suite);
    TestInvalidValues(suite);
    TestReasonKeys(suite);
    TestChannelMemory(suite);
    TestNoSubstitution(suite);
    TestRestore(suite);
    suite.report();
    return suite.failures();
}
