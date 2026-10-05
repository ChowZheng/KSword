// 内存工作台目标会话（shared/evidence/memory_workbench/MemoryTargetSession.h）的离线测试。
//
// 为什么这个模块值得一整套穷举断言：它属于**判错了不会报错**的那一类。
// 目标会话判断失误之后，界面照样显示、驱动照样去读，只是读到的是上一个目标的
// 数据，或者陈旧的异步结果被当成新鲜结果写回了界面。具体到本模块有三处：
//   * IdentityKey 漏掉一个字段 -> 换了通道/换了 DDMA 暂存扇区后仍复用上一次的读取；
//   * 内核地址阈值写成 0xFFFF000000000000（旧代码里确有一处这样写）-> 一段本属
//     不规范空洞的地址被当成内核地址放行；
//   * SessionRevisions 只核对一个计数器 -> 重读之后的旧编辑回调被当成新鲜。
//
// 断言原则与 NumericTextParseTests.cpp 一致：
//   * 期望值独立手算写死（含整串身份键），绝不从被测函数反算；
//   * 边界两侧都测（内核半区分界、32/64 位宽、pid 零与非零）；
//   * 该被拒绝的输入必须被显式拒绝，并且校验顺序也被钉住；
//   * 每个字段单独变化一次，才能证明没有字段被漏掉。

#include "TestSupport.h"

#include "../shared/evidence/memory_workbench/MemoryTargetSession.h"

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>

namespace {

using ksword::memwb::Channel;
using ksword::memwb::IdentityKey;
using ksword::memwb::IsKernelVirtualAddress;
using ksword::memwb::kKernelSplit;
using ksword::memwb::MemoryTargetSession;
using ksword::memwb::RevisionSnapshot;
using ksword::memwb::SameTarget;
using ksword::memwb::Scope;
using ksword::memwb::SessionError;
using ksword::memwb::SessionRevisions;
using ksword::memwb::Validate;

// uint64 的最大值，测试里到处用来检验"没有被截断/回绕"。
constexpr std::uint64_t kMax64 = 0xFFFFFFFFFFFFFFFFULL;

// MakeSession：按给定的范围、pid、位宽造一个会话，其余字段取固定的非零值，
// 这样 Validate 之外的字段"不影响校验结果"这一点也能被顺带验证。
MemoryTargetSession MakeSession(const Scope scope, const std::uint32_t pid, const std::uint32_t bits) {
    MemoryTargetSession session;
    session.scope = scope;
    session.pid = pid;
    session.processCreateTime100ns = 133000000000000000ULL;
    session.attachGeneration = 3;
    session.channel = Channel::StandardDriver;
    session.ddmaGeneration = 4;
    session.addressBits = bits;
    return session;
}

// MakeBaseSession：身份键测试用的基准会话，七个字段取互不相同的小数值，
// 手算整串键时一眼能对上。它是一个合法会话：进程范围、pid 1234、64 位。
MemoryTargetSession MakeBaseSession() {
    MemoryTargetSession session;
    session.scope = Scope::ProcessVirtual;
    session.pid = 1234;
    session.processCreateTime100ns = 5;
    session.attachGeneration = 7;
    session.channel = Channel::Hvm;
    session.ddmaGeneration = 9;
    session.addressBits = 64;
    return session;
}

// ------------------------------------------------------------
// 一、枚举数值与默认值：数值会进入身份键，必须钉死。
// ------------------------------------------------------------
void TestEnumValuesAndDefaults(KswordTests::Suite& suite) {
    // 范围枚举的数值。
    suite.expect(static_cast<std::uint32_t>(Scope::ProcessVirtual) == 0U,
        L"session: Scope::ProcessVirtual is numerically 0");
    suite.expect(static_cast<std::uint32_t>(Scope::KernelVirtual) == 1U,
        L"session: Scope::KernelVirtual is numerically 1");
    suite.expect(static_cast<std::uint32_t>(Scope::Physical) == 2U,
        L"session: Scope::Physical is numerically 2");

    // 通道枚举的数值与顺序：四个、顺序固定、没有"自动"项。
    suite.expect(static_cast<std::uint32_t>(Channel::UserMode) == 0U,
        L"session: Channel::UserMode is numerically 0");
    suite.expect(static_cast<std::uint32_t>(Channel::StandardDriver) == 1U,
        L"session: Channel::StandardDriver is numerically 1");
    suite.expect(static_cast<std::uint32_t>(Channel::Hvm) == 2U,
        L"session: Channel::Hvm is numerically 2");
    suite.expect(static_cast<std::uint32_t>(Channel::Ddma) == 3U,
        L"session: Channel::Ddma is numerically 3");

    // 错误码的数值：前四项与设计文档约定的顺序一致，None 必须是 0。
    suite.expect(static_cast<std::uint32_t>(SessionError::None) == 0U,
        L"session: SessionError::None is numerically 0");
    suite.expect(static_cast<std::uint32_t>(SessionError::NeedsPid) == 1U,
        L"session: SessionError::NeedsPid is numerically 1");
    suite.expect(static_cast<std::uint32_t>(SessionError::PidMustBeZero) == 2U,
        L"session: SessionError::PidMustBeZero is numerically 2");
    suite.expect(static_cast<std::uint32_t>(SessionError::BadAddressBits) == 3U,
        L"session: SessionError::BadAddressBits is numerically 3");

    // 默认构造的会话：七个字段取安全初值，并且**故意无效**——没选目标的状态
    // 不能被误用来读任何东西。
    const MemoryTargetSession fresh;
    suite.expect(fresh.scope == Scope::ProcessVirtual, L"session: a fresh session defaults to process scope");
    suite.expect(fresh.pid == 0U, L"session: a fresh session has pid 0");
    suite.expect(fresh.processCreateTime100ns == 0ULL, L"session: a fresh session has create time 0");
    suite.expect(fresh.attachGeneration == 0ULL, L"session: a fresh session has attach generation 0");
    suite.expect(fresh.channel == Channel::UserMode,
        L"session: a fresh session defaults to the least privileged channel");
    suite.expect(fresh.ddmaGeneration == 0ULL, L"session: a fresh session has ddma generation 0");
    suite.expect(fresh.addressBits == 64U, L"session: a fresh session defaults to 64-bit addresses");
    suite.expect(Validate(fresh) == SessionError::NeedsPid,
        L"session: a fresh session is invalid until a process is chosen");
}

// ------------------------------------------------------------
// 二、Validate：pid 规则的三种范围各测一遍，零与非零两侧都测。
// ------------------------------------------------------------
void TestValidatePidRules(KswordTests::Suite& suite) {
    // 进程虚拟地址空间：pid 非零才合法，零被显式拒绝。
    suite.expect(Validate(MakeSession(Scope::ProcessVirtual, 1234, 64)) == SessionError::None,
        L"session: a process session with a real pid is valid");
    suite.expect(Validate(MakeSession(Scope::ProcessVirtual, 1, 64)) == SessionError::None,
        L"session: pid 1 is the smallest valid process pid");
    suite.expect(Validate(MakeSession(Scope::ProcessVirtual, 0xFFFFFFFFU, 64)) == SessionError::None,
        L"session: the largest 32-bit pid is still a valid process pid");
    suite.expect(Validate(MakeSession(Scope::ProcessVirtual, 0, 64)) == SessionError::NeedsPid,
        L"session: a process session with pid 0 needs a pid");

    // 内核虚拟地址空间：pid 必须为零，非零（含最小的 1）被拒绝。
    suite.expect(Validate(MakeSession(Scope::KernelVirtual, 0, 64)) == SessionError::None,
        L"session: a kernel session with pid 0 is valid");
    suite.expect(Validate(MakeSession(Scope::KernelVirtual, 1, 64)) == SessionError::PidMustBeZero,
        L"session: a kernel session rejects pid 1");
    suite.expect(Validate(MakeSession(Scope::KernelVirtual, 4, 64)) == SessionError::PidMustBeZero,
        L"session: a kernel session rejects the System process pid 4");
    suite.expect(Validate(MakeSession(Scope::KernelVirtual, 0xFFFFFFFFU, 64)) == SessionError::PidMustBeZero,
        L"session: a kernel session rejects the largest pid");

    // 物理地址空间：规则同内核。
    suite.expect(Validate(MakeSession(Scope::Physical, 0, 64)) == SessionError::None,
        L"session: a physical session with pid 0 is valid");
    suite.expect(Validate(MakeSession(Scope::Physical, 1, 64)) == SessionError::PidMustBeZero,
        L"session: a physical session rejects pid 1");
    suite.expect(Validate(MakeSession(Scope::Physical, 1234, 64)) == SessionError::PidMustBeZero,
        L"session: a physical session rejects a real pid");
}

// ------------------------------------------------------------
// 三、Validate：地址宽度只有 32 与 64 合法，两侧邻近值都被拒绝。
// ------------------------------------------------------------
void TestValidateAddressBits(KswordTests::Suite& suite) {
    // 三种范围各配一个对它合法的 pid，只让位宽变化。
    suite.expect(Validate(MakeSession(Scope::ProcessVirtual, 1234, 32)) == SessionError::None,
        L"session: 32-bit width is valid for a process");
    suite.expect(Validate(MakeSession(Scope::ProcessVirtual, 1234, 64)) == SessionError::None,
        L"session: 64-bit width is valid for a process");
    suite.expect(Validate(MakeSession(Scope::KernelVirtual, 0, 32)) == SessionError::None,
        L"session: 32-bit width is valid for the kernel scope");
    suite.expect(Validate(MakeSession(Scope::KernelVirtual, 0, 64)) == SessionError::None,
        L"session: 64-bit width is valid for the kernel scope");
    suite.expect(Validate(MakeSession(Scope::Physical, 0, 32)) == SessionError::None,
        L"session: 32-bit width is valid for the physical scope");
    suite.expect(Validate(MakeSession(Scope::Physical, 0, 64)) == SessionError::None,
        L"session: 64-bit width is valid for the physical scope");

    // 被拒绝的位宽：0、两个合法值两侧的邻近值、其它常见位宽、最大值。
    const std::uint32_t rejectedWidths[] = { 0, 1, 16, 31, 33, 48, 63, 65, 128, 0xFFFFFFFFU };
    for (const std::uint32_t width : rejectedWidths) {
        suite.expect(Validate(MakeSession(Scope::ProcessVirtual, 1234, width)) == SessionError::BadAddressBits,
            L"session: an unsupported width is rejected for a process");
        suite.expect(Validate(MakeSession(Scope::KernelVirtual, 0, width)) == SessionError::BadAddressBits,
            L"session: an unsupported width is rejected for the kernel scope");
        suite.expect(Validate(MakeSession(Scope::Physical, 0, width)) == SessionError::BadAddressBits,
            L"session: an unsupported width is rejected for the physical scope");
    }
}

// ------------------------------------------------------------
// 四、Validate：多处违规并存时按固定顺序报告最先的一项。
// ------------------------------------------------------------
void TestValidateOrder(KswordTests::Suite& suite) {
    // pid 违规排在位宽违规之前。
    suite.expect(Validate(MakeSession(Scope::ProcessVirtual, 0, 16)) == SessionError::NeedsPid,
        L"session: the pid error is reported before the width error for a process");
    suite.expect(Validate(MakeSession(Scope::KernelVirtual, 1, 0)) == SessionError::PidMustBeZero,
        L"session: the pid error is reported before the width error for the kernel scope");
    suite.expect(Validate(MakeSession(Scope::Physical, 7, 33)) == SessionError::PidMustBeZero,
        L"session: the pid error is reported before the width error for the physical scope");

    // scope 越界排在一切之前：此时 pid 规则无从谈起。
    MemoryTargetSession badScope = MakeSession(Scope::ProcessVirtual, 0, 16);
    badScope.scope = static_cast<Scope>(3);
    badScope.channel = static_cast<Channel>(9);
    suite.expect(Validate(badScope) == SessionError::BadScope,
        L"session: a bad scope is reported before every other error");

    // channel 越界排在 pid 与位宽之前。
    MemoryTargetSession badChannel = MakeSession(Scope::ProcessVirtual, 0, 16);
    badChannel.channel = static_cast<Channel>(4);
    suite.expect(Validate(badChannel) == SessionError::BadChannel,
        L"session: a bad channel is reported before the pid and width errors");
}

// ------------------------------------------------------------
// 五、Validate：枚举越界值不能被放行，其余字段不影响校验。
// ------------------------------------------------------------
void TestValidateEnumRangeAndIgnoredFields(KswordTests::Suite& suite) {
    // scope 越界：紧邻合法区间的 3，以及最大值，都必须拒绝。
    MemoryTargetSession scopeJustPast = MakeSession(Scope::ProcessVirtual, 1234, 64);
    scopeJustPast.scope = static_cast<Scope>(3);
    suite.expect(Validate(scopeJustPast) == SessionError::BadScope,
        L"session: scope value 3 is one past the last valid scope and is rejected");
    MemoryTargetSession scopeHuge = MakeSession(Scope::KernelVirtual, 0, 64);
    scopeHuge.scope = static_cast<Scope>(0xFFFFFFFFU);
    suite.expect(Validate(scopeHuge) == SessionError::BadScope,
        L"session: the largest scope value is rejected");

    // channel 越界：紧邻合法区间的 4，以及最大值，都必须拒绝。
    MemoryTargetSession channelJustPast = MakeSession(Scope::ProcessVirtual, 1234, 64);
    channelJustPast.channel = static_cast<Channel>(4);
    suite.expect(Validate(channelJustPast) == SessionError::BadChannel,
        L"session: channel value 4 is one past the last valid channel and is rejected");
    MemoryTargetSession channelHuge = MakeSession(Scope::ProcessVirtual, 1234, 64);
    channelHuge.channel = static_cast<Channel>(0xFFFFFFFFU);
    suite.expect(Validate(channelHuge) == SessionError::BadChannel,
        L"session: the largest channel value is rejected");

    // 四个合法通道在合法会话上都通过：没有哪个通道被"误伤"。
    const Channel channels[] = { Channel::UserMode, Channel::StandardDriver, Channel::Hvm, Channel::Ddma };
    for (const Channel channel : channels) {
        MemoryTargetSession session = MakeSession(Scope::ProcessVirtual, 1234, 64);
        session.channel = channel;
        suite.expect(Validate(session) == SessionError::None,
            L"session: every one of the four channels is valid");
    }

    // 创建时间与三个代次不参与自洽性校验：0 与最大值都不影响结果。
    MemoryTargetSession zeros = MakeSession(Scope::ProcessVirtual, 1234, 64);
    zeros.processCreateTime100ns = 0;
    zeros.attachGeneration = 0;
    zeros.ddmaGeneration = 0;
    suite.expect(Validate(zeros) == SessionError::None,
        L"session: create time and generations at zero do not affect validity");
    MemoryTargetSession maxed = MakeSession(Scope::ProcessVirtual, 1234, 64);
    maxed.processCreateTime100ns = kMax64;
    maxed.attachGeneration = kMax64;
    maxed.ddmaGeneration = kMax64;
    suite.expect(Validate(maxed) == SessionError::None,
        L"session: create time and generations at the maximum do not affect validity");
}

// ------------------------------------------------------------
// 六、IdentityKey：整串格式手算写死，格式即契约。
// ------------------------------------------------------------
void TestIdentityKeyExactFormat(KswordTests::Suite& suite) {
    // 基准会话 + 基址 0x7FF612340000 + 长度 4096。十六进制固定 16 位小写，
    // 长度与各字段十进制。整串手写，不从被测函数反算。
    const std::string baseKey = IdentityKey(MakeBaseSession(), 0x00007FF612340000ULL, 4096);
    suite.expect(baseKey ==
        "memwb-target/1|scope=0|pid=1234|ct=5|gen=7|ch=2|ddma=9|bits=64|base=0x00007ff612340000|len=4096",
        L"identity key: the base session produces the exact documented string");

    // 全零：默认会话、基址 0、长度 0。前导零必须补满 16 位。
    const std::string zeroKey = IdentityKey(MemoryTargetSession(), 0, 0);
    suite.expect(zeroKey ==
        "memwb-target/1|scope=0|pid=0|ct=0|gen=0|ch=0|ddma=0|bits=64|base=0x0000000000000000|len=0",
        L"identity key: an all-zero session produces the exact documented string");

    // 全最大值：uint64 最大 = 18446744073709551615，uint32 最大 = 4294967295。
    // 检验每个字段都没有被截断成 32 位或回绕。
    MemoryTargetSession maxed;
    maxed.scope = Scope::Physical;
    maxed.pid = 0xFFFFFFFFU;
    maxed.processCreateTime100ns = kMax64;
    maxed.attachGeneration = kMax64;
    maxed.channel = Channel::Ddma;
    maxed.ddmaGeneration = kMax64;
    maxed.addressBits = 32;
    const std::string maxKey = IdentityKey(maxed, kMax64, kMax64);
    suite.expect(maxKey ==
        "memwb-target/1|scope=2|pid=4294967295|ct=18446744073709551615|gen=18446744073709551615"
        "|ch=3|ddma=18446744073709551615|bits=32|base=0xffffffffffffffff|len=18446744073709551615",
        L"identity key: maximum values are written in full without truncation");

    // 只含 ASCII，且不含换行：可以安全地写进日志与缓存键。
    bool asciiOnly = true;
    for (const char character : baseKey) {
        const unsigned char code = static_cast<unsigned char>(character);
        if (code < 0x20U || code > 0x7EU) {
            asciiOnly = false;
        }
    }
    suite.expect(asciiOnly, L"identity key: the key contains only printable ASCII");
}

// 以下一组小函数各自只改会话的一个字段，供表驱动测试逐字段变化使用。

// MutateScope：只改范围。
void MutateScope(MemoryTargetSession& session) {
    session.scope = Scope::Physical;
}

// MutatePid：只改 pid。
void MutatePid(MemoryTargetSession& session) {
    session.pid = 1235;
}

// MutateCreateTime：只改进程创建时间。
void MutateCreateTime(MemoryTargetSession& session) {
    session.processCreateTime100ns = 6;
}

// MutateAttachGeneration：只改附加代次。
void MutateAttachGeneration(MemoryTargetSession& session) {
    session.attachGeneration = 8;
}

// MutateChannel：只改通道。
void MutateChannel(MemoryTargetSession& session) {
    session.channel = Channel::Ddma;
}

// MutateDdmaGeneration：只改 DDMA 代次。
void MutateDdmaGeneration(MemoryTargetSession& session) {
    session.ddmaGeneration = 10;
}

// MutateAddressBits：只改位宽。
void MutateAddressBits(MemoryTargetSession& session) {
    session.addressBits = 32;
}

// FieldMutation：一行表项 = 改哪个字段 + 两类断言各自的标签。
struct FieldMutation {
    // keyLabel：身份键测试的断言标签。
    const wchar_t* keyLabel;
    // sameLabel：同目标测试的断言标签。
    const wchar_t* sameLabel;
    // apply：只改一个字段的函数。
    void (*apply)(MemoryTargetSession&);
};

// 七个字段各一行。表里少一行，对应字段就没有被测。
const FieldMutation kFieldMutations[] = {
    { L"identity key: changing scope changes the key",
      L"same target: changing scope makes the targets differ", &MutateScope },
    { L"identity key: changing pid changes the key",
      L"same target: changing pid makes the targets differ", &MutatePid },
    { L"identity key: changing the process create time changes the key",
      L"same target: changing the process create time makes the targets differ", &MutateCreateTime },
    { L"identity key: changing the attach generation changes the key",
      L"same target: changing the attach generation makes the targets differ", &MutateAttachGeneration },
    { L"identity key: changing the channel changes the key",
      L"same target: changing the channel makes the targets differ", &MutateChannel },
    { L"identity key: changing the ddma generation changes the key",
      L"same target: changing the ddma generation makes the targets differ", &MutateDdmaGeneration },
    { L"identity key: changing the address width changes the key",
      L"same target: changing the address width makes the targets differ", &MutateAddressBits },
};

// ------------------------------------------------------------
// 七、IdentityKey：同目标同基址同长度才相等，任一项不同必须不同。
// ------------------------------------------------------------
void TestIdentityKeyEachFieldMatters(KswordTests::Suite& suite) {
    const std::uint64_t base = 0x00007FF612340000ULL;
    const std::uint64_t length = 4096;
    const std::string reference = IdentityKey(MakeBaseSession(), base, length);

    // 相同输入必须得到相同的键；拷贝出来的会话也一样。
    suite.expect(IdentityKey(MakeBaseSession(), base, length) == reference,
        L"identity key: identical inputs produce identical keys");
    const MemoryTargetSession copy = MakeBaseSession();
    suite.expect(IdentityKey(copy, base, length) == reference,
        L"identity key: a copied session produces the same key");

    // 逐字段变化：每个字段单独改一次，键必须与基准不同；同时收集所有键，
    // 最后断言它们两两不同（基准 + 七个变化 = 八个不同的键）。
    std::set<std::string> distinctKeys;
    distinctKeys.insert(reference);
    for (const FieldMutation& mutation : kFieldMutations) {
        MemoryTargetSession changed = MakeBaseSession();
        mutation.apply(changed);
        const std::string changedKey = IdentityKey(changed, base, length);
        suite.expect(changedKey != reference, mutation.keyLabel);
        distinctKeys.insert(changedKey);
    }
    suite.expect(distinctKeys.size() == 8U,
        L"identity key: seven single-field changes plus the base give eight different keys");

    // 基址与长度各差 1 也必须不同。
    suite.expect(IdentityKey(MakeBaseSession(), base + 1, length) != reference,
        L"identity key: a base address one higher changes the key");
    suite.expect(IdentityKey(MakeBaseSession(), base - 1, length) != reference,
        L"identity key: a base address one lower changes the key");
    suite.expect(IdentityKey(MakeBaseSession(), base, length + 1) != reference,
        L"identity key: a length one larger changes the key");
    suite.expect(IdentityKey(MakeBaseSession(), base, length - 1) != reference,
        L"identity key: a length one smaller changes the key");

    // 基址与长度互换后必须不同：它们不能被当成无序的一对。
    suite.expect(IdentityKey(MakeBaseSession(), 0x1000, 0x10) != IdentityKey(MakeBaseSession(), 0x10, 0x1000),
        L"identity key: swapping base and length changes the key");

    // 三个范围、四个通道、两种位宽各自两两不同。
    std::set<std::string> scopeKeys;
    for (const Scope scope : { Scope::ProcessVirtual, Scope::KernelVirtual, Scope::Physical }) {
        MemoryTargetSession session = MakeBaseSession();
        session.scope = scope;
        scopeKeys.insert(IdentityKey(session, base, length));
    }
    suite.expect(scopeKeys.size() == 3U, L"identity key: the three scopes give three different keys");

    std::set<std::string> channelKeys;
    for (const Channel channel : { Channel::UserMode, Channel::StandardDriver, Channel::Hvm, Channel::Ddma }) {
        MemoryTargetSession session = MakeBaseSession();
        session.channel = channel;
        channelKeys.insert(IdentityKey(session, base, length));
    }
    suite.expect(channelKeys.size() == 4U, L"identity key: the four channels give four different keys");
}

// ------------------------------------------------------------
// 八、IdentityKey：字段之间不会粘连，数值互换不会撞键。
// ------------------------------------------------------------
void TestIdentityKeyHasNoAmbiguity(KswordTests::Suite& suite) {
    // 两个字段的取值互换：pid=1,gen=2 与 pid=2,gen=1 必须不同。
    MemoryTargetSession a = MakeBaseSession();
    a.pid = 1;
    a.attachGeneration = 2;
    MemoryTargetSession b = MakeBaseSession();
    b.pid = 2;
    b.attachGeneration = 1;
    suite.expect(IdentityKey(a, 0, 0) != IdentityKey(b, 0, 0),
        L"identity key: swapping the pid and attach generation values changes the key");

    // 数位边界：pid=12,ct=3 与 pid=1,ct=23 拼起来的数位串相同，但键必须不同。
    MemoryTargetSession c = MakeBaseSession();
    c.pid = 12;
    c.processCreateTime100ns = 3;
    MemoryTargetSession d = MakeBaseSession();
    d.pid = 1;
    d.processCreateTime100ns = 23;
    suite.expect(IdentityKey(c, 0, 0) != IdentityKey(d, 0, 0),
        L"identity key: digits cannot slide across the pid and create time boundary");

    // 同一数值分别放进 ddma 与 gen：位置不同键就不同。
    MemoryTargetSession e = MakeBaseSession();
    e.attachGeneration = 5;
    e.ddmaGeneration = 6;
    MemoryTargetSession f = MakeBaseSession();
    f.attachGeneration = 6;
    f.ddmaGeneration = 5;
    suite.expect(IdentityKey(e, 0, 0) != IdentityKey(f, 0, 0),
        L"identity key: swapping the attach and ddma generations changes the key");

    // 两个只差最低位的最大值代次：不能因为被截断而相等。
    MemoryTargetSession g = MakeBaseSession();
    g.ddmaGeneration = kMax64;
    MemoryTargetSession h = MakeBaseSession();
    h.ddmaGeneration = kMax64 - 1;
    suite.expect(IdentityKey(g, 0, 0) != IdentityKey(h, 0, 0),
        L"identity key: ddma generations differing in the lowest bit stay different");

    // 键本身不裁决：不自洽的会话（进程范围却 pid 为 0）也照常给出键，
    // 由调用方先 Validate。
    suite.expect(!IdentityKey(MemoryTargetSession(), 0, 0).empty(),
        L"identity key: an invalid session still produces a key because validation is the caller's job");
}

// ------------------------------------------------------------
// 九、SameTarget：七个字段全比，且对称。
// ------------------------------------------------------------
void TestSameTarget(KswordTests::Suite& suite) {
    // 相同与拷贝相同。
    const MemoryTargetSession original = MakeBaseSession();
    const MemoryTargetSession copy = original;
    suite.expect(SameTarget(original, original), L"same target: a session is the same target as itself");
    suite.expect(SameTarget(original, copy), L"same target: a copy is the same target");
    suite.expect(SameTarget(MemoryTargetSession(), MemoryTargetSession()),
        L"same target: two default sessions are the same target");

    // 逐字段变化：七个字段各改一次，必须判不同，并且对称。
    for (const FieldMutation& mutation : kFieldMutations) {
        MemoryTargetSession changed = MakeBaseSession();
        mutation.apply(changed);
        suite.expect(!SameTarget(original, changed), mutation.sameLabel);
        suite.expect(!SameTarget(changed, original),
            L"same target: the comparison is symmetric when one field differs");
    }

    // 最大值差一位也要判不同：七个字段里宽度最大的三个 uint64。
    MemoryTargetSession high = MakeBaseSession();
    high.processCreateTime100ns = kMax64;
    high.attachGeneration = kMax64;
    high.ddmaGeneration = kMax64;
    MemoryTargetSession nearHigh = high;
    nearHigh.ddmaGeneration = kMax64 - 1;
    suite.expect(SameTarget(high, high), L"same target: maximum-valued sessions equal themselves");
    suite.expect(!SameTarget(high, nearHigh),
        L"same target: ddma generations differing in the lowest bit are different targets");

    // SameTarget 与 IdentityKey 对"是否同一目标"的结论必须一致：同目标 -> 同键。
    suite.expect(IdentityKey(original, 0x1000, 16) == IdentityKey(copy, 0x1000, 16),
        L"same target: targets judged the same also produce the same identity key");
}

// ------------------------------------------------------------
// 十、内核地址判据：只有一份阈值，分界两侧都测。
// ------------------------------------------------------------
void TestKernelSplit(KswordTests::Suite& suite) {
    // 阈值本身手算写死：x64 规范地址的内核半区从 0xFFFF800000000000 开始。
    suite.expect(kKernelSplit == 0xFFFF800000000000ULL,
        L"kernel split: the threshold is 0xFFFF800000000000");

    // 分界两侧：差 1 即换边。
    suite.expect(!IsKernelVirtualAddress(0xFFFF7FFFFFFFFFFFULL),
        L"kernel split: one below the threshold is not a kernel address");
    suite.expect(IsKernelVirtualAddress(0xFFFF800000000000ULL),
        L"kernel split: exactly the threshold is a kernel address");
    suite.expect(IsKernelVirtualAddress(0xFFFF800000000001ULL),
        L"kernel split: one above the threshold is a kernel address");

    // 两端：0 与全一。
    suite.expect(!IsKernelVirtualAddress(0ULL), L"kernel split: address 0 is not a kernel address");
    suite.expect(IsKernelVirtualAddress(0xFFFFFFFFFFFFFFFFULL),
        L"kernel split: the highest address is a kernel address");

    // 用户半区顶端。
    suite.expect(!IsKernelVirtualAddress(0x00007FFFFFFFFFFFULL),
        L"kernel split: the top of the user half is not a kernel address");

    // 旧代码里曾有一处用 0xFFFF000000000000 当阈值：这一段本属不规范空洞，
    // 在唯一的判据下必须判为非内核。分界两侧都测。
    suite.expect(!IsKernelVirtualAddress(0xFFFF000000000000ULL),
        L"kernel split: the old 0xFFFF000000000000 threshold no longer counts as kernel");
    suite.expect(!IsKernelVirtualAddress(0xFFFF000000000001ULL),
        L"kernel split: just above the old threshold is still not a kernel address");
    suite.expect(!IsKernelVirtualAddress(0xFFFF7FFF00000000ULL),
        L"kernel split: the upper part of the old window is still not a kernel address");

    // 不规范空洞（用户半区之上、内核半区之下）整体都是 false。
    suite.expect(!IsKernelVirtualAddress(0x0000800000000000ULL),
        L"kernel split: the start of the non-canonical hole is not a kernel address");
    suite.expect(!IsKernelVirtualAddress(0x7FFFFFFFFFFFFFFFULL),
        L"kernel split: the middle of the non-canonical hole is not a kernel address");
    suite.expect(!IsKernelVirtualAddress(0x8000000000000000ULL),
        L"kernel split: the sign-bit-only address is not a kernel address");

    // 32 位目标的地址都小于 2^32，因此恒为 false，不需要另一套阈值。
    suite.expect(!IsKernelVirtualAddress(0x00000000FFFFFFFFULL),
        L"kernel split: the top of a 32-bit address space is not a kernel address");
    suite.expect(!IsKernelVirtualAddress(0x0000000080000000ULL),
        L"kernel split: the 32-bit 2 GiB boundary is not a kernel address");
}

// ------------------------------------------------------------
// 十一、SessionRevisions：四种组合，两个计数器独立。
// ------------------------------------------------------------
void TestRevisionsFourCombinations(KswordTests::Suite& suite) {
    // 初始：两个计数器都是 0，刚捕获的快照不陈旧。
    SessionRevisions revisions;
    suite.expect(revisions.Source() == 0ULL, L"revisions: the source counter starts at 0");
    suite.expect(revisions.Content() == 0ULL, L"revisions: the content counter starts at 0");
    const RevisionSnapshot initial = revisions.Capture();
    suite.expect(initial.source == 0ULL && initial.content == 0ULL,
        L"revisions: the initial snapshot captures 0 and 0");

    // 组合一：都不改 -> 不陈旧，重复捕获结果相同。
    suite.expect(!revisions.IsStale(initial), L"revisions: nothing changed means not stale");
    suite.expect(revisions.Capture() == initial,
        L"revisions: capturing twice without changes gives equal snapshots");

    // 组合二：只改 source -> 陈旧，且内容代次纹丝不动。
    SessionRevisions onlySource;
    const RevisionSnapshot beforeSource = onlySource.Capture();
    onlySource.BumpSource();
    suite.expect(onlySource.IsStale(beforeSource), L"revisions: a source-only change is stale");
    suite.expect(onlySource.Source() == 1ULL, L"revisions: BumpSource moves the source counter to 1");
    suite.expect(onlySource.Content() == 0ULL,
        L"revisions: BumpSource leaves the content counter untouched");

    // 组合三：只改 content -> 陈旧，且来源代次纹丝不动。
    SessionRevisions onlyContent;
    const RevisionSnapshot beforeContent = onlyContent.Capture();
    onlyContent.BumpContent();
    suite.expect(onlyContent.IsStale(beforeContent), L"revisions: a content-only change is stale");
    suite.expect(onlyContent.Content() == 1ULL, L"revisions: BumpContent moves the content counter to 1");
    suite.expect(onlyContent.Source() == 0ULL,
        L"revisions: BumpContent leaves the source counter untouched");

    // 组合四：都改 -> 陈旧。
    SessionRevisions both;
    const RevisionSnapshot beforeBoth = both.Capture();
    both.BumpSource();
    both.BumpContent();
    suite.expect(both.IsStale(beforeBoth), L"revisions: changing both counters is stale");
    suite.expect(both.Source() == 1ULL && both.Content() == 1ULL,
        L"revisions: both counters moved to 1");
}

// ------------------------------------------------------------
// 十二、SessionRevisions：快照的新鲜判据、重新捕获、两个计数器不串位。
// ------------------------------------------------------------
void TestRevisionsSnapshotSemantics(KswordTests::Suite& suite) {
    // 改动之后重新捕获，新快照新鲜，旧快照仍然陈旧。
    SessionRevisions revisions;
    const RevisionSnapshot old = revisions.Capture();
    revisions.BumpSource();
    revisions.BumpContent();
    revisions.BumpContent();
    const RevisionSnapshot fresh = revisions.Capture();
    suite.expect(fresh.source == 1ULL && fresh.content == 2ULL,
        L"revisions: a re-captured snapshot holds the current values");
    suite.expect(!revisions.IsStale(fresh), L"revisions: a re-captured snapshot is fresh");
    suite.expect(revisions.IsStale(old), L"revisions: the older snapshot stays stale");
    suite.expect(!(old == fresh), L"revisions: snapshots from different moments are not equal");

    // 多次递增：计数器按次数累加，不是置位。
    SessionRevisions many;
    many.BumpSource();
    many.BumpSource();
    many.BumpSource();
    many.BumpContent();
    suite.expect(many.Source() == 3ULL, L"revisions: three source bumps give 3");
    suite.expect(many.Content() == 1ULL, L"revisions: one content bump gives 1");

    // 手工构造快照：来源对、内容错 -> 陈旧；来源错、内容对 -> 陈旧；都对 -> 新鲜。
    // 这里同时钉住"逐字段对应比较"：来源/内容两个值互换后也必须陈旧。
    const SessionRevisions fixed(1, 2);
    suite.expect(!fixed.IsStale(RevisionSnapshot{ 1, 2 }), L"revisions: a matching snapshot is fresh");
    suite.expect(fixed.IsStale(RevisionSnapshot{ 1, 3 }), L"revisions: a content mismatch alone is stale");
    suite.expect(fixed.IsStale(RevisionSnapshot{ 2, 2 }), L"revisions: a source mismatch alone is stale");
    suite.expect(fixed.IsStale(RevisionSnapshot{ 2, 1 }),
        L"revisions: a snapshot with the two values swapped is stale");
    suite.expect(fixed.IsStale(RevisionSnapshot{ 0, 0 }), L"revisions: an all-zero snapshot is stale");

    // 构造函数的初值：来源与内容各按位置对应。
    suite.expect(fixed.Source() == 1ULL, L"revisions: the first constructor argument is the source counter");
    suite.expect(fixed.Content() == 2ULL, L"revisions: the second constructor argument is the content counter");

    // 来源与内容的取值相同时，也要靠两个计数器分别核对：只改其一仍判陈旧。
    SessionRevisions twins(5, 5);
    const RevisionSnapshot twinSnapshot = twins.Capture();
    twins.BumpContent();
    suite.expect(twins.IsStale(twinSnapshot),
        L"revisions: equal counter values do not hide a content-only change");
}

// ------------------------------------------------------------
// 十三、SessionRevisions：溢出回绕有定义，回绕后旧快照仍然陈旧。
// ------------------------------------------------------------
void TestRevisionsWraparound(KswordTests::Suite& suite) {
    // 来源代次从最大值回绕到 0；内容代次不受影响。
    SessionRevisions revisions(kMax64, kMax64);
    const RevisionSnapshot atMax = revisions.Capture();
    suite.expect(atMax.source == kMax64 && atMax.content == kMax64,
        L"wraparound: the snapshot at the maximum holds the maximum values");
    suite.expect(!revisions.IsStale(atMax), L"wraparound: a snapshot at the maximum is fresh before any bump");

    revisions.BumpSource();
    suite.expect(revisions.Source() == 0ULL, L"wraparound: the source counter wraps from the maximum to 0");
    suite.expect(revisions.Content() == kMax64,
        L"wraparound: wrapping the source counter does not touch the content counter");
    suite.expect(revisions.IsStale(atMax),
        L"wraparound: a snapshot taken before the source wrapped is stale");

    // 内容代次接着回绕到 0。
    revisions.BumpContent();
    suite.expect(revisions.Content() == 0ULL, L"wraparound: the content counter wraps from the maximum to 0");
    suite.expect(revisions.Source() == 0ULL,
        L"wraparound: wrapping the content counter does not touch the source counter");
    suite.expect(revisions.IsStale(atMax),
        L"wraparound: a snapshot taken before both counters wrapped is stale");

    // 回绕之后重新捕获是新鲜的，并且计数器从 0 继续正常递增。
    const RevisionSnapshot afterWrap = revisions.Capture();
    suite.expect(!revisions.IsStale(afterWrap), L"wraparound: a snapshot taken after wrapping is fresh");
    revisions.BumpSource();
    suite.expect(revisions.Source() == 1ULL, L"wraparound: counting continues from 0 after the wrap");
    suite.expect(revisions.IsStale(afterWrap), L"wraparound: a bump after the wrap makes the snapshot stale");

    // 只有内容代次回绕：同样判陈旧，来源代次保持不变。
    SessionRevisions contentOnly(0, kMax64);
    const RevisionSnapshot contentBefore = contentOnly.Capture();
    contentOnly.BumpContent();
    suite.expect(contentOnly.Content() == 0ULL,
        L"wraparound: the content counter alone wraps from the maximum to 0");
    suite.expect(contentOnly.Source() == 0ULL,
        L"wraparound: the source counter stays at 0 when only the content wraps");
    suite.expect(contentOnly.IsStale(contentBefore),
        L"wraparound: a content-only wrap is stale even though the new value looks like the initial one");

    // 最大值减一再加一：恰好到达最大值，不是回绕。
    SessionRevisions justBelow(kMax64 - 1, 0);
    justBelow.BumpSource();
    suite.expect(justBelow.Source() == kMax64,
        L"wraparound: one below the maximum plus one reaches the maximum without wrapping");
}

} // namespace

int RunMemwbSessionTests() {
    KswordTests::Suite suite(L"MEMWB session");
    TestEnumValuesAndDefaults(suite);
    TestValidatePidRules(suite);
    TestValidateAddressBits(suite);
    TestValidateOrder(suite);
    TestValidateEnumRangeAndIgnoredFields(suite);
    TestIdentityKeyExactFormat(suite);
    TestIdentityKeyEachFieldMatters(suite);
    TestIdentityKeyHasNoAmbiguity(suite);
    TestSameTarget(suite);
    TestKernelSplit(suite);
    TestRevisionsFourCombinations(suite);
    TestRevisionsSnapshotSemantics(suite);
    TestRevisionsWraparound(suite);
    suite.report();
    return suite.failures();
}
