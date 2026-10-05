// 内存工作台写入确认策略（shared/evidence/memory_workbench/MemoryWritePolicy.h）的离线测试。
//
// 为什么这个模块值得一整套穷举断言：它属于**判错了不会报错**的那一类。
// 该弹的确认被抑制掉，用户在内核/物理内存或磁盘暂存扇区上的一次误写就没有任何人拦；
// 不该弹的确认弹出来，则每次编辑都被打断，用户只好打开全局"跳过危险确认"——两头都是
// 悄悄发生的。具体到本模块有四处：
//   * 进程范围+立即写入被误判成需要确认（每次键入都弹窗）；
//   * 暂存->应用被"本次运行不再询问"抑制（该每次都问的变成永远不问）；
//   * 记忆位图的下标算错，记住 (内核, R0) 时连带抑制了别的组合；
//   * 全局跳过开关打开时仍把"弹"返回给写事务（用户明确要求不弹）。
//
// 断言原则与 MemoryTargetSessionTests.cpp 一致：
//   * 期望值按"确认策略表"逐格手写（24 格 x 全局开关 x 已记忆），不从被测函数反算；
//   * 十二个 (范围, 通道) 组合各自单独记忆一次，证明下标互不串位；
//   * 拒绝路径显式测：越界枚举保守地要求确认，且不留下记忆；
//   * 本类只返回 suppressed 布尔，审计由写事务负责，这里只断言返回值。

#include "TestSupport.h"

#include "../shared/evidence/memory_workbench/MemoryWritePolicy.h"

#include <cstdint>

namespace {

using ksword::memwb::Channel;
using ksword::memwb::ConfirmDecision;
using ksword::memwb::ConfirmReason;
using ksword::memwb::IsRiskyImmediateCombination;
using ksword::memwb::MemoryWritePolicy;
using ksword::memwb::Scope;
using ksword::memwb::WriteMode;

// 三个范围、四个通道的全集，供穷举循环使用。
constexpr Scope kScopes[] = { Scope::ProcessVirtual, Scope::KernelVirtual, Scope::Physical };
constexpr Channel kChannels[] = { Channel::UserMode, Channel::StandardDriver, Channel::Hvm, Channel::Ddma };

// ------------------------------------------------------------
// 一、枚举数值与默认值。
// ------------------------------------------------------------
void TestEnumValuesAndDefaults(KswordTests::Suite& suite) {
    suite.expect(static_cast<std::uint32_t>(ConfirmReason::GlobalSkip) == 0U, L"policy: GlobalSkip is 0");
    suite.expect(static_cast<std::uint32_t>(ConfirmReason::ProcessImmediate) == 1U, L"policy: ProcessImmediate is 1");
    suite.expect(static_cast<std::uint32_t>(ConfirmReason::RememberedThisRun) == 2U, L"policy: RememberedThisRun is 2");
    suite.expect(static_cast<std::uint32_t>(ConfirmReason::RiskyImmediate) == 3U, L"policy: RiskyImmediate is 3");
    suite.expect(static_cast<std::uint32_t>(ConfirmReason::StagedApply) == 4U, L"policy: StagedApply is 4");
    suite.expect(static_cast<std::uint32_t>(ConfirmReason::InvalidInput) == 5U, L"policy: InvalidInput is 5");

    // 默认结论是保守的一档：要弹、不提供勾选框、无效输入。
    const ConfirmDecision fresh;
    suite.expect(!fresh.suppressed, L"policy: a default decision does not suppress the confirmation");
    suite.expect(!fresh.offerDontAskAgain, L"policy: a default decision offers no checkbox");
    suite.expect(fresh.reason == ConfirmReason::InvalidInput, L"policy: a default decision is InvalidInput");

    // 新建策略什么都没记。
    const MemoryWritePolicy policy;
    for (const Scope scope : kScopes) {
        for (const Channel channel : kChannels) {
            suite.expect(!policy.IsRemembered(scope, channel), L"policy: a new policy remembers nothing");
        }
    }
}

// ------------------------------------------------------------
// 二、"需要首次确认的一类"：九个危险组合 + 三个安全组合。
// ------------------------------------------------------------
void TestRiskyCombinations(KswordTests::Suite& suite) {
    // 进程范围：只有磁盘传输通道危险；R3/R0/HVM 不危险。
    suite.expect(!IsRiskyImmediateCombination(Scope::ProcessVirtual, Channel::UserMode),
        L"risky: process R3 is not risky");
    suite.expect(!IsRiskyImmediateCombination(Scope::ProcessVirtual, Channel::StandardDriver),
        L"risky: process R0 is not risky");
    suite.expect(!IsRiskyImmediateCombination(Scope::ProcessVirtual, Channel::Hvm),
        L"risky: process HVM is not risky");
    suite.expect(IsRiskyImmediateCombination(Scope::ProcessVirtual, Channel::Ddma),
        L"risky: process DDMA is risky because it borrows disk scratch sectors");

    // 内核与物理范围：四个通道全部危险。
    for (const Channel channel : kChannels) {
        suite.expect(IsRiskyImmediateCombination(Scope::KernelVirtual, channel), L"risky: every kernel combination is risky");
        suite.expect(IsRiskyImmediateCombination(Scope::Physical, channel), L"risky: every physical combination is risky");
    }

    // 总数手算：内核 4 + 物理 4 + 进程/DDMA 1 = 9。
    int riskyCount = 0;
    for (const Scope scope : kScopes) {
        for (const Channel channel : kChannels) {
            if (IsRiskyImmediateCombination(scope, channel)) {
                ++riskyCount;
            }
        }
    }
    suite.expect(riskyCount == 9, L"risky: nine of the twelve combinations are risky");

    // 越界值不属于任何一类（由 Decide 单独处理）。
    suite.expect(!IsRiskyImmediateCombination(static_cast<Scope>(3), Channel::Ddma), L"risky: a bad scope is not risky");
    suite.expect(!IsRiskyImmediateCombination(Scope::KernelVirtual, static_cast<Channel>(4)),
        L"risky: a bad channel is not risky");
    suite.expect(!IsRiskyImmediateCombination(static_cast<Scope>(0xFFFFFFFFU), static_cast<Channel>(0xFFFFFFFFU)),
        L"risky: two bad values are not risky");
}

// ------------------------------------------------------------
// 三、确认策略表逐格手写：全局开关关、无记忆。
// ------------------------------------------------------------

// PolicyRow：一行表项 = 模式、范围、通道 + 期望的三个字段 + 标签。
struct PolicyRow {
    WriteMode mode;
    Scope scope;
    Channel channel;
    bool expectSuppressed;
    bool expectOffer;
    ConfirmReason expectReason;
    const wchar_t* label;
};

const PolicyRow kRows[] = {
    // ---- 立即写入 / 进程范围：只有 DDMA 要弹 ----
    { WriteMode::Immediate, Scope::ProcessVirtual, Channel::UserMode, true, false, ConfirmReason::ProcessImmediate,
      L"row: immediate process R3 does not ask" },
    { WriteMode::Immediate, Scope::ProcessVirtual, Channel::StandardDriver, true, false, ConfirmReason::ProcessImmediate,
      L"row: immediate process R0 does not ask" },
    { WriteMode::Immediate, Scope::ProcessVirtual, Channel::Hvm, true, false, ConfirmReason::ProcessImmediate,
      L"row: immediate process HVM does not ask" },
    { WriteMode::Immediate, Scope::ProcessVirtual, Channel::Ddma, false, true, ConfirmReason::RiskyImmediate,
      L"row: immediate process DDMA asks the first time and offers the checkbox" },
    // ---- 立即写入 / 内核范围：四个通道都要弹 ----
    { WriteMode::Immediate, Scope::KernelVirtual, Channel::UserMode, false, true, ConfirmReason::RiskyImmediate,
      L"row: immediate kernel R3 asks (the policy does not judge channel support)" },
    { WriteMode::Immediate, Scope::KernelVirtual, Channel::StandardDriver, false, true, ConfirmReason::RiskyImmediate,
      L"row: immediate kernel R0 asks" },
    { WriteMode::Immediate, Scope::KernelVirtual, Channel::Hvm, false, true, ConfirmReason::RiskyImmediate,
      L"row: immediate kernel HVM asks" },
    { WriteMode::Immediate, Scope::KernelVirtual, Channel::Ddma, false, true, ConfirmReason::RiskyImmediate,
      L"row: immediate kernel DDMA asks" },
    // ---- 立即写入 / 物理范围：四个通道都要弹 ----
    { WriteMode::Immediate, Scope::Physical, Channel::UserMode, false, true, ConfirmReason::RiskyImmediate,
      L"row: immediate physical R3 asks" },
    { WriteMode::Immediate, Scope::Physical, Channel::StandardDriver, false, true, ConfirmReason::RiskyImmediate,
      L"row: immediate physical R0 asks" },
    { WriteMode::Immediate, Scope::Physical, Channel::Hvm, false, true, ConfirmReason::RiskyImmediate,
      L"row: immediate physical HVM asks" },
    { WriteMode::Immediate, Scope::Physical, Channel::Ddma, false, true, ConfirmReason::RiskyImmediate,
      L"row: immediate physical DDMA asks" },
    // ---- 暂存->应用：十二格全部要弹，且没有勾选框 ----
    { WriteMode::StagedThenApply, Scope::ProcessVirtual, Channel::UserMode, false, false, ConfirmReason::StagedApply,
      L"row: staged process R3 always asks, no checkbox" },
    { WriteMode::StagedThenApply, Scope::ProcessVirtual, Channel::StandardDriver, false, false, ConfirmReason::StagedApply,
      L"row: staged process R0 always asks, no checkbox" },
    { WriteMode::StagedThenApply, Scope::ProcessVirtual, Channel::Hvm, false, false, ConfirmReason::StagedApply,
      L"row: staged process HVM always asks, no checkbox" },
    { WriteMode::StagedThenApply, Scope::ProcessVirtual, Channel::Ddma, false, false, ConfirmReason::StagedApply,
      L"row: staged process DDMA always asks, no checkbox" },
    { WriteMode::StagedThenApply, Scope::KernelVirtual, Channel::UserMode, false, false, ConfirmReason::StagedApply,
      L"row: staged kernel R3 always asks, no checkbox" },
    { WriteMode::StagedThenApply, Scope::KernelVirtual, Channel::StandardDriver, false, false, ConfirmReason::StagedApply,
      L"row: staged kernel R0 always asks, no checkbox" },
    { WriteMode::StagedThenApply, Scope::KernelVirtual, Channel::Hvm, false, false, ConfirmReason::StagedApply,
      L"row: staged kernel HVM always asks, no checkbox" },
    { WriteMode::StagedThenApply, Scope::KernelVirtual, Channel::Ddma, false, false, ConfirmReason::StagedApply,
      L"row: staged kernel DDMA always asks, no checkbox" },
    { WriteMode::StagedThenApply, Scope::Physical, Channel::UserMode, false, false, ConfirmReason::StagedApply,
      L"row: staged physical R3 always asks, no checkbox" },
    { WriteMode::StagedThenApply, Scope::Physical, Channel::StandardDriver, false, false, ConfirmReason::StagedApply,
      L"row: staged physical R0 always asks, no checkbox" },
    { WriteMode::StagedThenApply, Scope::Physical, Channel::Hvm, false, false, ConfirmReason::StagedApply,
      L"row: staged physical HVM always asks, no checkbox" },
    { WriteMode::StagedThenApply, Scope::Physical, Channel::Ddma, false, false, ConfirmReason::StagedApply,
      L"row: staged physical DDMA always asks, no checkbox" },
};

void TestPolicyTable(KswordTests::Suite& suite) {
    const MemoryWritePolicy policy;
    for (const PolicyRow& row : kRows) {
        const ConfirmDecision decision = policy.Decide(row.mode, row.scope, row.channel, false);
        suite.expect(decision.suppressed == row.expectSuppressed && decision.offerDontAskAgain == row.expectOffer
                && decision.reason == row.expectReason,
            row.label);
        // 便捷形式与 Decide 的 suppressed 一致。
        suite.expect(policy.SuppressUiConfirm(row.mode, row.scope, row.channel, false) == row.expectSuppressed,
            L"row: SuppressUiConfirm agrees with Decide");
    }
}

// ------------------------------------------------------------
// 四、全局"跳过危险确认"开关：二十四格一律不弹，仍压过一切。
// ------------------------------------------------------------
void TestGlobalSkip(KswordTests::Suite& suite) {
    MemoryWritePolicy policy;
    for (const PolicyRow& row : kRows) {
        const ConfirmDecision decision = policy.Decide(row.mode, row.scope, row.channel, true);
        suite.expect(decision.suppressed && !decision.offerDontAskAgain && decision.reason == ConfirmReason::GlobalSkip,
            L"global skip: every one of the 24 cells is suppressed with reason GlobalSkip");
    }

    // 即使该组合已被记忆，开关打开时原因仍是 GlobalSkip（开关优先）。
    suite.expect(policy.NoteConfirmed(Scope::KernelVirtual, Channel::StandardDriver, true),
        L"global skip setup: remember kernel R0");
    suite.expect(policy.Decide(WriteMode::Immediate, Scope::KernelVirtual, Channel::StandardDriver, true).reason
            == ConfirmReason::GlobalSkip,
        L"global skip: the switch takes precedence over a remembered combination");

    // 开关关掉之后，暂存应用依然每次都弹——开关是用户的设置，不是一次性的记忆。
    suite.expect(!policy.Decide(WriteMode::StagedThenApply, Scope::Physical, Channel::Ddma, false).suppressed,
        L"global skip: switching the global option off restores the staged confirmation");

    // 越界输入 + 开关打开：开关是用户显式的设置，仍按开关处理。
    const ConfirmDecision badWithSkip =
        policy.Decide(static_cast<WriteMode>(9), static_cast<Scope>(3), static_cast<Channel>(4), true);
    suite.expect(badWithSkip.suppressed && badWithSkip.reason == ConfirmReason::GlobalSkip,
        L"global skip: the explicit user switch applies even to out-of-range inputs");
}

// ------------------------------------------------------------
// 五、"本次运行不再询问"：只对危险的立即写入组合生效，且组合互不串位。
// ------------------------------------------------------------
void TestRememberSemantics(KswordTests::Suite& suite) {
    MemoryWritePolicy policy;

    // 未勾选：NoteConfirmed 返回 false，什么都不记；同一组合下次仍问（"弹到勾选为止"）。
    suite.expect(!policy.NoteConfirmed(Scope::KernelVirtual, Channel::StandardDriver, false),
        L"remember: confirming without ticking the box records nothing");
    suite.expect(!policy.IsRemembered(Scope::KernelVirtual, Channel::StandardDriver),
        L"remember: an unticked confirmation is not remembered");
    suite.expect(!policy.SuppressUiConfirm(WriteMode::Immediate, Scope::KernelVirtual, Channel::StandardDriver, false),
        L"remember: the same combination still asks after an unticked confirmation");

    // 勾选：记入，返回 true，该组合立即写入不再弹，原因 RememberedThisRun，不再提供勾选框。
    suite.expect(policy.NoteConfirmed(Scope::KernelVirtual, Channel::StandardDriver, true),
        L"remember: ticking the box records the combination");
    suite.expect(policy.IsRemembered(Scope::KernelVirtual, Channel::StandardDriver),
        L"remember: a ticked combination is remembered");
    const ConfirmDecision remembered =
        policy.Decide(WriteMode::Immediate, Scope::KernelVirtual, Channel::StandardDriver, false);
    suite.expect(remembered.suppressed && !remembered.offerDontAskAgain
            && remembered.reason == ConfirmReason::RememberedThisRun,
        L"remember: a remembered combination no longer asks and offers no checkbox");

    // 重复勾选同一组合：幂等，仍返回 true。
    suite.expect(policy.NoteConfirmed(Scope::KernelVirtual, Channel::StandardDriver, true),
        L"remember: ticking the same combination again still reports true");

    // 记忆只管"立即写入"：同一组合的暂存->应用仍然每次都弹。
    const ConfirmDecision staged =
        policy.Decide(WriteMode::StagedThenApply, Scope::KernelVirtual, Channel::StandardDriver, false);
    suite.expect(!staged.suppressed && staged.reason == ConfirmReason::StagedApply,
        L"remember: a remembered combination still asks for staged applies");

    // 记忆按"范围+通道"组合隔离：只有被记住的那一格不弹，邻近的格子都照常弹。
    suite.expect(!policy.SuppressUiConfirm(WriteMode::Immediate, Scope::KernelVirtual, Channel::Hvm, false),
        L"remember: another channel in the same scope still asks");
    suite.expect(!policy.SuppressUiConfirm(WriteMode::Immediate, Scope::Physical, Channel::StandardDriver, false),
        L"remember: the same channel in another scope still asks");
    suite.expect(!policy.SuppressUiConfirm(WriteMode::Immediate, Scope::ProcessVirtual, Channel::Ddma, false),
        L"remember: process DDMA is its own combination and still asks");

    // 进程范围的普通通道本来就不弹，勾选没有意义：返回 false，不记入。
    suite.expect(!policy.NoteConfirmed(Scope::ProcessVirtual, Channel::UserMode, true),
        L"remember: a combination that never asks has nothing to remember");
    suite.expect(!policy.IsRemembered(Scope::ProcessVirtual, Channel::UserMode),
        L"remember: process R3 is not recorded");

    // 越界值：不记入。
    suite.expect(!policy.NoteConfirmed(static_cast<Scope>(3), Channel::Ddma, true),
        L"remember: a bad scope is not recorded");
    suite.expect(!policy.NoteConfirmed(Scope::Physical, static_cast<Channel>(4), true),
        L"remember: a bad channel is not recorded");
    suite.expect(!policy.IsRemembered(static_cast<Scope>(3), Channel::Ddma), L"remember: a bad scope is never remembered");
    suite.expect(!policy.IsRemembered(Scope::Physical, static_cast<Channel>(4)),
        L"remember: a bad channel is never remembered");

    // ResetRun：清空，之前记住的组合重新要弹。
    policy.ResetRun();
    suite.expect(!policy.IsRemembered(Scope::KernelVirtual, Channel::StandardDriver),
        L"remember: ResetRun clears the memory");
    suite.expect(!policy.SuppressUiConfirm(WriteMode::Immediate, Scope::KernelVirtual, Channel::StandardDriver, false),
        L"remember: after ResetRun the combination asks again");
}

// ------------------------------------------------------------
// 六、十二个组合各记忆一次：位图下标互不串位。
// ------------------------------------------------------------
void TestBitmapIndependence(KswordTests::Suite& suite) {
    int violationNotRemembered = 0;
    int violationLeaked = 0;
    int violationRiskyNotSuppressed = 0;
    int violationNeverAskingGotRemembered = 0;

    for (const Scope rememberedScope : kScopes) {
        for (const Channel rememberedChannel : kChannels) {
            MemoryWritePolicy policy;
            const bool risky = IsRiskyImmediateCombination(rememberedScope, rememberedChannel);
            const bool recorded = policy.NoteConfirmed(rememberedScope, rememberedChannel, true);

            // 危险组合：必须被记入；不危险的不记入。
            if (recorded != risky) {
                ++violationNeverAskingGotRemembered;
            }

            // 逐格检查：只有被记住的那一格被记忆，其余十一格都没有。
            for (const Scope scope : kScopes) {
                for (const Channel channel : kChannels) {
                    const bool isTheSame = (scope == rememberedScope) && (channel == rememberedChannel);
                    const bool expectRemembered = isTheSame && risky;
                    if (policy.IsRemembered(scope, channel) != expectRemembered) {
                        if (isTheSame) {
                            ++violationNotRemembered;
                        } else {
                            ++violationLeaked;
                        }
                    }
                    // 危险且被记住的那一格，立即写入不再弹；其余危险格仍要弹。
                    if (IsRiskyImmediateCombination(scope, channel)) {
                        const bool suppressed = policy.SuppressUiConfirm(WriteMode::Immediate, scope, channel, false);
                        if (suppressed != expectRemembered) {
                            ++violationRiskyNotSuppressed;
                        }
                    }
                }
            }
        }
    }
    suite.expect(violationNeverAskingGotRemembered == 0,
        L"bitmap: exactly the risky combinations can be recorded");
    suite.expect(violationNotRemembered == 0, L"bitmap: a recorded combination reads back as remembered");
    suite.expect(violationLeaked == 0, L"bitmap: recording one combination never leaks into the other eleven");
    suite.expect(violationRiskyNotSuppressed == 0,
        L"bitmap: only the recorded risky combination stops asking, the other risky ones still ask");

    // 九个危险组合全部记住之后：九个都不弹，三个进程普通组合本来就不弹，暂存十二格仍都要弹。
    MemoryWritePolicy all;
    for (const Scope scope : kScopes) {
        for (const Channel channel : kChannels) {
            (void)all.NoteConfirmed(scope, channel, true);
        }
    }
    int stillAsking = 0;
    int stagedSuppressed = 0;
    for (const Scope scope : kScopes) {
        for (const Channel channel : kChannels) {
            if (!all.SuppressUiConfirm(WriteMode::Immediate, scope, channel, false)) {
                ++stillAsking;
            }
            if (all.SuppressUiConfirm(WriteMode::StagedThenApply, scope, channel, false)) {
                ++stagedSuppressed;
            }
        }
    }
    suite.expect(stillAsking == 0, L"bitmap: with all nine risky combinations remembered no immediate write asks");
    suite.expect(stagedSuppressed == 0, L"bitmap: remembering everything never silences a staged apply");
}

// ------------------------------------------------------------
// 七、越界输入：保守地要求确认，不留记忆。
// ------------------------------------------------------------
void TestInvalidInputs(KswordTests::Suite& suite) {
    const MemoryWritePolicy policy;

    // 模式越界：连进程 R3（本来不弹）也要弹——越界时宁可多问。
    const ConfirmDecision badMode =
        policy.Decide(static_cast<WriteMode>(2), Scope::ProcessVirtual, Channel::UserMode, false);
    suite.expect(!badMode.suppressed && !badMode.offerDontAskAgain && badMode.reason == ConfirmReason::InvalidInput,
        L"invalid: a bad write mode asks and offers no checkbox");
    const ConfirmDecision hugeMode =
        policy.Decide(static_cast<WriteMode>(-1), Scope::ProcessVirtual, Channel::UserMode, false);
    suite.expect(!hugeMode.suppressed && hugeMode.reason == ConfirmReason::InvalidInput,
        L"invalid: a negative write mode asks");

    // 范围越界、通道越界。
    const ConfirmDecision badScope =
        policy.Decide(WriteMode::Immediate, static_cast<Scope>(3), Channel::UserMode, false);
    suite.expect(!badScope.suppressed && !badScope.offerDontAskAgain && badScope.reason == ConfirmReason::InvalidInput,
        L"invalid: a bad scope asks and offers no checkbox");
    const ConfirmDecision badChannel =
        policy.Decide(WriteMode::Immediate, Scope::ProcessVirtual, static_cast<Channel>(4), false);
    suite.expect(!badChannel.suppressed && !badChannel.offerDontAskAgain
            && badChannel.reason == ConfirmReason::InvalidInput,
        L"invalid: a bad channel asks and offers no checkbox");
    const ConfirmDecision hugeBoth = policy.Decide(
        WriteMode::StagedThenApply, static_cast<Scope>(0xFFFFFFFFU), static_cast<Channel>(0xFFFFFFFFU), false);
    suite.expect(!hugeBoth.suppressed && hugeBoth.reason == ConfirmReason::InvalidInput,
        L"invalid: the largest scope and channel values ask");

    // SuppressUiConfirm 对越界输入同样返回 false。
    suite.expect(!policy.SuppressUiConfirm(WriteMode::Immediate, static_cast<Scope>(3), static_cast<Channel>(4), false),
        L"invalid: SuppressUiConfirm returns false for bad inputs");
}

} // namespace

int RunMemwbWritePolicyTests() {
    KswordTests::Suite suite(L"MEMWB write policy");
    TestEnumValuesAndDefaults(suite);
    TestRiskyCombinations(suite);
    TestPolicyTable(suite);
    TestGlobalSkip(suite);
    TestRememberSemantics(suite);
    TestBitmapIndependence(suite);
    TestInvalidInputs(suite);
    suite.report();
    return suite.failures();
}
