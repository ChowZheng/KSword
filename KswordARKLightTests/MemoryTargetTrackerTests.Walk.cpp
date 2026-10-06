// 内存工作台目标跟踪器（MemoryTargetTracker.h）的随机游走测试。
//
// 从 MemoryTargetTrackerTests.cpp 按职责拆出（单文件不得超过 800 行）。本文件只做一件事：
// 用 1 万步确定性伪随机序列驱动 tracker，并验证两条核心性质——
//   (1) 会话身份变了 => 来源代次必变；
//   (2) 来源代次变而身份没变 => 该步只能是 Reload。
// 同时用一份独立写的参考模型逐步对拍会话的每个字段，并断言游走确实覆盖了每一类事件，
// 避免性质因为序列太单调而空过。入口见 MemoryTargetTrackerTestSupport.h。

#include "TestSupport.h"

#include "MemoryTargetTrackerTestSupport.h"

#include "../shared/evidence/memory_workbench/MemoryTargetTracker.h"

#include <cstdint>
#include <cstdio>

namespace {

using ksword::memwb::Channel;
using ksword::memwb::HasChange;
using ksword::memwb::IsPinnedAttachGeneration;
using ksword::memwb::kIdentityChangeMask;
using ksword::memwb::MemoryTargetSession;
using ksword::memwb::MemoryTargetTracker;
using ksword::memwb::Scope;
using ksword::memwb::SessionError;
using ksword::memwb::TargetChange;
using ksword::memwb::Validate;
using Follow = ksword::memwb::MemoryTargetTracker::Follow;

// kPin：钉住代次的标记位（2^63），参考模型直接用它加计数手算钉住代次。
constexpr std::uint64_t kPin = 0x8000000000000000ULL;

// ------------------------------------------------------------
// 十、1 万步确定性伪随机游走：两条核心性质 + 参考模型对拍。
// ------------------------------------------------------------

// DeterministicRng：xorshift64* 伪随机数发生器。自己实现而不用标准库分布，
// 因为标准库的分布实现各编译器不同，游走序列必须在 g++ 与 MSVC 下完全一致。
class DeterministicRng {
public:
    // 构造：seed 固定种子（非零）。
    explicit DeterministicRng(const std::uint64_t seed)
        : state_(seed)
    {
    }

    // Next：产生下一个 64 位伪随机数。
    std::uint64_t Next() {
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return state_ * 2685821657736338717ULL;
    }

    // Below：产生 [0, limit) 内的数，limit 必须非零。取高位而不是低位，低位的随机性较差。
    std::uint32_t Below(const std::uint32_t limit) {
        const std::uint64_t high = Next() >> 33;
        return static_cast<std::uint32_t>(high % limit);
    }

private:
    // state_：发生器内部状态。
    std::uint64_t state_;
};

// WalkModel：游走里与被测类**分开写**的参考模型。它只用最朴素的"六项状态 + 一次性派生会话"
// 的描述，不共享被测代码的任何实现，用来对拍会话的每一个字段。
struct WalkModel {
    // ModelProcess：参考模型里的进程记录。
    struct ModelProcess {
        // present：是否有这条记录。
        bool present = false;
        // pid：进程号。
        std::uint32_t pid = 0;
        // createTime：进程创建时间（100ns）。
        std::uint64_t createTime = 0;
        // generation：附加代次。
        std::uint64_t generation = 0;
        // bits：地址宽度。
        std::uint32_t bits = 64;
    };

    // scope：当前范围。
    Scope scope = Scope::ProcessVirtual;
    // channel：当前通道。
    Channel channel = Channel::UserMode;
    // ddmaGeneration：DDMA 暂存扇区代次（仅通道为 Ddma 时进入会话）。
    std::uint64_t ddmaGeneration = 0;
    // pinned：是否钉住了进程。
    bool pinned = false;
    // dock：Dock 的附加记录。
    ModelProcess dock;
    // pin：钉住记录。
    ModelProcess pin;
    // pinCounter：钉住代次的自增计数，第 n 次新分配的钉住代次是 2^63 + n。
    std::uint64_t pinCounter = 0;

    // Derive：按模型状态算出期望的会话。
    MemoryTargetSession Derive() const {
        MemoryTargetSession expected;
        expected.scope = scope;
        expected.channel = channel;
        expected.ddmaGeneration = (channel == Channel::Ddma) ? ddmaGeneration : 0ULL;
        const ModelProcess& chosen = pinned ? pin : dock;
        if (scope == Scope::ProcessVirtual && chosen.present) {
            expected.pid = chosen.pid;
            expected.processCreateTime100ns = chosen.createTime;
            expected.attachGeneration = chosen.generation;
            expected.addressBits = chosen.bits;
        }
        return expected;
    }
};

// WalkOp：游走里的九种操作。
enum class WalkOp : std::uint32_t {
    FollowAttach = 0,
    FollowDetach,
    Pin,
    Unpin,
    SetScope,
    SetChannel,
    ObserveDdma,
    Reload,
    NoteContentChanged,
    Count
};

// ExpectedIdentityBits：独立算出两个会话之间应有的身份位（与被测类的实现分开写）。
TargetChange ExpectedIdentityBits(const MemoryTargetSession& a, const MemoryTargetSession& b) {
    std::uint32_t bits = 0;
    if (a.scope != b.scope) {
        bits |= 1U;
    }
    if (a.pid != b.pid || a.processCreateTime100ns != b.processCreateTime100ns
        || a.attachGeneration != b.attachGeneration) {
        bits |= 2U;
    }
    if (a.channel != b.channel) {
        bits |= 4U;
    }
    if (a.ddmaGeneration != b.ddmaGeneration) {
        bits |= 8U;
    }
    if (a.addressBits != b.addressBits) {
        bits |= 16U;
    }
    return static_cast<TargetChange>(bits);
}

// FieldsEqual：逐字段比较两个会话（不借助 SameTarget，以免两边同时错）。
bool FieldsEqual(const MemoryTargetSession& a, const MemoryTargetSession& b) {
    return a.scope == b.scope && a.pid == b.pid && a.processCreateTime100ns == b.processCreateTime100ns
        && a.attachGeneration == b.attachGeneration && a.channel == b.channel
        && a.ddmaGeneration == b.ddmaGeneration && a.addressBits == b.addressBits;
}

// 游走使用的取值表：小范围取值让"同参数重复调用"与"同身份钉住"频繁出现。
constexpr std::uint32_t kWalkPids[] = { 0, 1, 2, 3, 4 };
constexpr std::uint64_t kWalkCreateTimes[] = { 0, 10, 20 };
constexpr std::uint64_t kWalkGenerations[] = { 1, 2, 3, 4, 0x7FFFFFFFFFFFFFFFULL, 0x8000000000000000ULL };
constexpr std::uint32_t kWalkBits[] = { 32, 64, 64, 64, 48 };

// WalkStats：游走的覆盖统计，最后据此断言游走没有空转。
struct WalkStats {
    // identityChanges：会话身份变化的步数。
    int identityChanges = 0;
    // reloads：Reload 的步数。
    int reloads = 0;
    // idempotentNoChange：返回 None 的步数（同参数重复、被吸收的 Dock 事件等）。
    int idempotentNoChange = 0;
    // rejected：被拒绝的步数。
    int rejected = 0;
    // policyOnly：只改了跟随模式（返回恰为 Policy）的步数。
    int policyOnly = 0;
    // pinAdoptions：钉住了与 Dock 同身份的进程、会话没动的步数。
    int pinAdoptions = 0;
    // freshPins：新分配钉住代次的步数。
    int freshPins = 0;
    // dockEventsIgnored：Dock 附加被吸收（返回 None）的步数。
    int dockEventsIgnored = 0;
    // ddmaChanges：掩码含 Ddma 位的步数。
    int ddmaChanges = 0;
    // bitsChanges：掩码含 Bits 位的步数。
    int bitsChanges = 0;
    // kernelOrPhysicalSteps：步后范围为内核或物理的步数。
    int kernelOrPhysicalSteps = 0;
    // contentNotes：NoteContentChanged 的步数。
    int contentNotes = 0;
};

// TestRandomWalk：1 万步游走本体。传入：suite 断言容器。传出：无（违例记在 suite 上）。
void TestRandomWalk(KswordTests::Suite& suite) {
    // 固定种子，序列完全确定。
    DeterministicRng rng(0x9E3779B97F4A7C15ULL);
    MemoryTargetTracker tracker;
    WalkModel model;

    // 每条性质一个违例计数；第一次违例的步号单独记下，便于排查。
    int violationIdentityBumpsOnce = 0;
    int violationNoIdentityNoBump = 0;
    int violationReloadOnly = 0;
    int violationContentOnlyContent = 0;
    int violationMaskMatchesFields = 0;
    int violationSessionMatchesModel = 0;
    int violationRejectedChangesNothing = 0;
    int violationPolicyBit = 0;
    int violationSessionInvariants = 0;
    int violationPinnedGenerations = 0;
    int violationRejectionPredicate = 0;
    int firstViolationStep = -1;

    // lastFreshPin：上一次"新分配"的钉住代次；每次新分配的值必须比它大（严格递增，互不相同）。
    std::uint64_t lastFreshPin = 0;
    WalkStats stats;

    for (int step = 0; step < 10000; ++step) {
        // 步前快照：会话、两个计数器、跟随模式。
        const MemoryTargetSession oldSession = tracker.Session();
        const std::uint64_t oldSource = tracker.Revisions().Source();
        const std::uint64_t oldContent = tracker.Revisions().Content();
        const bool oldPinned = (tracker.FollowMode() == Follow::Pinned);

        // 选操作与参数，并用参考模型独立判断"这次调用应不应该被拒绝"。
        const WalkOp operation = static_cast<WalkOp>(rng.Below(static_cast<std::uint32_t>(WalkOp::Count)));
        TargetChange mask = TargetChange::None;
        bool expectRejected = false;
        bool pinAdopted = false;
        bool pinFresh = false;

        switch (operation) {
        case WalkOp::FollowAttach: {
            const std::uint32_t pid = kWalkPids[rng.Below(5)];
            const std::uint64_t createTime = kWalkCreateTimes[rng.Below(3)];
            const std::uint64_t generation = kWalkGenerations[rng.Below(6)];
            const std::uint32_t bits = kWalkBits[rng.Below(5)];
            // 参考模型的拒绝判据：pid 为 0、位宽不是 32/64、代次 >= 2^63。
            expectRejected = (pid == 0U) || (bits != 32U && bits != 64U) || (generation >= kPin);
            mask = tracker.FollowAttach(pid, createTime, generation, bits);
            if (!expectRejected) {
                model.dock.present = true;
                model.dock.pid = pid;
                model.dock.createTime = createTime;
                model.dock.generation = generation;
                model.dock.bits = bits;
            }
            break;
        }
        case WalkOp::FollowDetach:
            mask = tracker.FollowDetach();
            model.dock = WalkModel::ModelProcess();
            break;
        case WalkOp::Pin: {
            // 一半的钉住请求直接取 Dock 当前进程的身份，否则"同身份钉住"这条路径
            // 在随机取值下几乎走不到（实测只有约 1%），性质就验得太稀。
            const bool copyDock = model.dock.present && (rng.Below(2) == 0U);
            const std::uint32_t randomPid = kWalkPids[rng.Below(5)];
            const std::uint64_t randomCreateTime = kWalkCreateTimes[rng.Below(3)];
            const std::uint32_t randomBits = kWalkBits[rng.Below(5)];
            const std::uint32_t pid = copyDock ? model.dock.pid : randomPid;
            const std::uint64_t createTime = copyDock ? model.dock.createTime : randomCreateTime;
            const std::uint32_t bits = copyDock ? model.dock.bits : randomBits;
            expectRejected = (pid == 0U) || (bits != 32U && bits != 64U);
            mask = tracker.Pin(pid, createTime, bits);
            if (!expectRejected) {
                const bool samePinned = model.pinned && model.pin.present && model.pin.pid == pid
                    && model.pin.createTime == createTime && model.pin.bits == bits;
                if (!samePinned) {
                    const bool sameDock = model.dock.present && model.dock.pid == pid
                        && model.dock.createTime == createTime && model.dock.bits == bits;
                    model.pin.present = true;
                    model.pin.pid = pid;
                    model.pin.createTime = createTime;
                    model.pin.bits = bits;
                    if (sameDock) {
                        model.pin.generation = model.dock.generation;
                        pinAdopted = true;
                    } else {
                        ++model.pinCounter;
                        model.pin.generation = kPin + model.pinCounter;
                        pinFresh = true;
                    }
                    model.pinned = true;
                }
            }
            break;
        }
        case WalkOp::Unpin:
            mask = tracker.Unpin();
            model.pinned = false;
            model.pin = WalkModel::ModelProcess();
            break;
        case WalkOp::SetScope: {
            const std::uint32_t raw = rng.Below(4);
            expectRejected = (raw > 2U);
            mask = tracker.SetScope(static_cast<Scope>(raw));
            if (!expectRejected) {
                model.scope = static_cast<Scope>(raw);
            }
            break;
        }
        case WalkOp::SetChannel: {
            const std::uint32_t raw = rng.Below(5);
            const std::uint64_t generation = rng.Below(4);
            expectRejected = (raw > 3U);
            mask = tracker.SetChannel(static_cast<Channel>(raw), generation);
            if (!expectRejected) {
                model.channel = static_cast<Channel>(raw);
                model.ddmaGeneration = (model.channel == Channel::Ddma) ? generation : 0ULL;
            }
            break;
        }
        case WalkOp::ObserveDdma: {
            const std::uint64_t generation = rng.Below(4);
            mask = tracker.ObserveDdma(generation);
            if (model.channel == Channel::Ddma) {
                model.ddmaGeneration = generation;
            }
            break;
        }
        case WalkOp::Reload:
            mask = tracker.Reload();
            break;
        case WalkOp::NoteContentChanged:
            tracker.NoteContentChanged();
            break;
        case WalkOp::Count:
            break;
        }

        // 步后读数。
        const MemoryTargetSession newSession = tracker.Session();
        const std::uint64_t newSource = tracker.Revisions().Source();
        const std::uint64_t newContent = tracker.Revisions().Content();
        const bool newPinned = (tracker.FollowMode() == Follow::Pinned);
        const bool identityChanged = !FieldsEqual(oldSession, newSession);
        const bool isReload = (operation == WalkOp::Reload);
        const bool isContentOp = (operation == WalkOp::NoteContentChanged);
        const bool returnsMask = !isContentOp;

        // 记下第一次违例的步号，之后逐条性质累加。
        const auto noteViolation = [&](int& counter) {
            ++counter;
            if (firstViolationStep < 0) {
                firstViolationStep = step;
            }
        };

        // 性质 1：会话身份变了 => 来源代次恰好 +1（一次，不多不少）。
        if (identityChanged && newSource != oldSource + 1ULL) {
            noteViolation(violationIdentityBumpsOnce);
        }

        // 性质 2：来源代次变而身份没变 => 该步只能是 Reload，且返回值恰为 Reload。
        if (!identityChanged && newSource != oldSource) {
            if (!isReload || mask != TargetChange::Reload || newSource != oldSource + 1ULL) {
                noteViolation(violationReloadOnly);
            }
        }

        // 性质 2 的反面：身份没变且不是 Reload => 来源代次不动（没有无谓 bump）。
        if (!identityChanged && !isReload && newSource != oldSource) {
            noteViolation(violationNoIdentityNoBump);
        }

        // 性质 3：NoteContentChanged 只动内容代次；其余操作绝不动内容代次。
        if (isContentOp) {
            if (newContent != oldContent + 1ULL || newSource != oldSource || identityChanged) {
                noteViolation(violationContentOnlyContent);
            }
        } else if (newContent != oldContent) {
            noteViolation(violationContentOnlyContent);
        }

        // 性质 4：返回掩码里的身份位与"逐字段独立比较"的结果完全一致。
        if (returnsMask && expectRejected == false) {
            if ((mask & kIdentityChangeMask) != ExpectedIdentityBits(oldSession, newSession)) {
                noteViolation(violationMaskMatchesFields);
            }
        }

        // 性质 5：会话与参考模型逐字段一致（对拍）。
        if (!FieldsEqual(newSession, model.Derive())) {
            noteViolation(violationSessionMatchesModel);
        }

        // 性质 6：被拒绝 <=> 参考模型的判据；被拒绝时状态完全不变。
        if (returnsMask && ((mask == TargetChange::Rejected) != expectRejected)) {
            noteViolation(violationRejectionPredicate);
        }
        if (mask == TargetChange::Rejected) {
            if (identityChanged || newSource != oldSource || newContent != oldContent || newPinned != oldPinned) {
                noteViolation(violationRejectedChangesNothing);
            }
        }

        // 性质 7：Policy 位 <=> 跟随模式变了（Pin/Unpin 才可能改变模式）。
        if (returnsMask && mask != TargetChange::Rejected) {
            const bool policyInMask = HasChange(mask, TargetChange::Policy);
            if (policyInMask != (oldPinned != newPinned)) {
                noteViolation(violationPolicyBit);
            }
        }

        // 性质 8：会话恒自洽——只有"进程范围且无目标"一种无效；内核/物理的进程字段恒为零、位宽 64。
        const SessionError validity = Validate(newSession);
        const bool noTarget = (newSession.scope == Scope::ProcessVirtual) && (newSession.pid == 0U);
        const bool validityOk = noTarget ? (validity == SessionError::NeedsPid) : (validity == SessionError::None);
        const bool zeroedOk = (newSession.scope == Scope::ProcessVirtual)
            || (newSession.pid == 0U && newSession.processCreateTime100ns == 0ULL
                && newSession.attachGeneration == 0ULL && newSession.addressBits == 64U);
        if (!validityOk || !zeroedOk) {
            noteViolation(violationSessionInvariants);
        }

        // 性质 9：跟随 Dock 的进程会话绝不带钉住标记；新分配的钉住代次严格递增。
        if (newSession.scope == Scope::ProcessVirtual && newSession.pid != 0U && !newPinned
            && IsPinnedAttachGeneration(newSession.attachGeneration)) {
            noteViolation(violationPinnedGenerations);
        }
        if (pinFresh && mask != TargetChange::Rejected) {
            const std::uint64_t allocated = kPin + model.pinCounter;
            if (allocated <= lastFreshPin) {
                noteViolation(violationPinnedGenerations);
            }
            lastFreshPin = allocated;
        }

        // 覆盖统计。
        if (returnsMask && mask == TargetChange::Rejected) {
            ++stats.rejected;
        }
        if (identityChanged) {
            ++stats.identityChanges;
        }
        if (isReload) {
            ++stats.reloads;
        }
        if (returnsMask && mask == TargetChange::None) {
            ++stats.idempotentNoChange;
        }
        if (returnsMask && mask == TargetChange::Policy) {
            ++stats.policyOnly;
        }
        if (pinAdopted && !identityChanged) {
            ++stats.pinAdoptions;
        }
        if (pinFresh) {
            ++stats.freshPins;
        }
        if (operation == WalkOp::FollowAttach && mask == TargetChange::None) {
            ++stats.dockEventsIgnored;
        }
        if (HasChange(mask, TargetChange::Ddma)) {
            ++stats.ddmaChanges;
        }
        if (HasChange(mask, TargetChange::Bits)) {
            ++stats.bitsChanges;
        }
        if (newSession.scope != Scope::ProcessVirtual) {
            ++stats.kernelOrPhysicalSteps;
        }
        if (isContentOp) {
            ++stats.contentNotes;
        }
    }

    // 九条性质各报告一次：违例计数必须为 0。
    suite.expect(violationIdentityBumpsOnce == 0,
        L"walk: whenever the session identity changes the source revision moves by exactly one");
    suite.expect(violationReloadOnly == 0,
        L"walk: a source revision change without an identity change is only ever a Reload");
    suite.expect(violationNoIdentityNoBump == 0,
        L"walk: an unchanged identity without a Reload never bumps the source revision");
    suite.expect(violationContentOnlyContent == 0,
        L"walk: only NoteContentChanged moves the content revision and it moves nothing else");
    suite.expect(violationMaskMatchesFields == 0,
        L"walk: the identity bits of the returned mask equal an independent field comparison");
    suite.expect(violationSessionMatchesModel == 0,
        L"walk: the session matches the independently written reference model at every step");
    suite.expect(violationRejectionPredicate == 0,
        L"walk: Rejected is returned exactly when the reference model says the arguments are invalid");
    suite.expect(violationRejectedChangesNothing == 0,
        L"walk: a rejected call leaves the session, both revisions and the follow mode untouched");
    suite.expect(violationPolicyBit == 0, L"walk: the Policy bit appears exactly when the follow mode changed");
    suite.expect(violationSessionInvariants == 0,
        L"walk: the session is always self-consistent and kernel or physical sessions carry no process fields");
    suite.expect(violationPinnedGenerations == 0,
        L"walk: followed sessions never carry the pin bit and fresh pinned generations strictly increase");

    // 违例时把第一次出问题的步号写到 stderr，方便复现（正常运行不输出）。
    if (firstViolationStep >= 0) {
        std::fprintf(stderr, "tracker random walk: first violation at step %d\n", firstViolationStep);
    }

    // 覆盖断言：游走必须确实走到了每一类事件，否则上面的"零违例"可能是空过。
    // 下限取实测值的约一半或更低（序列固定，实测值不会漂移），留足余量但仍能拦住"某类事件根本没发生"。
    suite.expect(stats.identityChanges >= 1000, L"walk coverage: at least 1000 identity changes happened");
    suite.expect(stats.reloads >= 500, L"walk coverage: at least 500 reloads happened");
    suite.expect(stats.idempotentNoChange >= 700, L"walk coverage: at least 700 no-change calls happened");
    suite.expect(stats.rejected >= 500, L"walk coverage: at least 500 rejected calls happened");
    suite.expect(stats.policyOnly >= 50, L"walk coverage: at least 50 policy-only calls happened");
    suite.expect(stats.pinAdoptions >= 20, L"walk coverage: at least 20 same-identity pins happened");
    suite.expect(stats.freshPins >= 200, L"walk coverage: at least 200 fresh pinned generations were allocated");
    suite.expect(stats.dockEventsIgnored >= 100, L"walk coverage: at least 100 dock attaches were absorbed");
    suite.expect(stats.ddmaChanges >= 100, L"walk coverage: at least 100 ddma generation changes happened");
    suite.expect(stats.bitsChanges >= 100, L"walk coverage: at least 100 width changes happened");
    suite.expect(stats.kernelOrPhysicalSteps >= 3000,
        L"walk coverage: at least 3000 steps ran in kernel or physical scope");
    suite.expect(stats.contentNotes >= 500, L"walk coverage: at least 500 content notes happened");
}

} // namespace

// RunTrackerRandomWalk：对外的入口，声明见 MemoryTargetTrackerTestSupport.h。
void RunTrackerRandomWalk(KswordTests::Suite& suite) {
    TestRandomWalk(suite);
}
