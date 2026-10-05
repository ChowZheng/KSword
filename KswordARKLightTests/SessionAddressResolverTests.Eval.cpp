// 内存工作台会话地址解析器的离线测试 —— 另一半：各范围的地址空间边界（ScopeAddressSpace）
// 与面向界面的一站式入口 EvaluateForSession（范围判定、NeedsScopeSwitch 建议、
// 模块/解引用成因透传、"求值只跑一次"）。IAddressExprResolver 的两个方法本身的测试
// 在 SessionAddressResolverTests.cpp，套件入口也在那里，由它调用本文件的
// RunMemwbSessionResolverEvalChecks。
//
// 断言原则：期望值手算写死；地址空间边界两侧都测（0x7FFFFFFFFFFF 与 0x800000000000、
// kKernelSplit 与 kKernelSplit-1、0xFFFFFFFF 与 0x100000000、52 位物理上限两侧）；
// 失败时 value 恒为 0、inScopeSpace 为假、没有范围建议；
// 假读取器的调用记录证明"纯数字/语法错误不碰读取器""求值只跑一次"。

#include "ModuleResolverTestSupport.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace memwb_wpb_test {
namespace {

using ksword::memwb::AddrSpace;
using ksword::memwb::AddrSpaceContains;
using ksword::memwb::EvaluateForSession;
using ksword::memwb::ExprError;
using ksword::memwb::IsKernelVirtualAddress;
using ksword::memwb::kKernelSplit;
using ksword::memwb::ScopeAddressSpace;

constexpr std::uint64_t kMaxU64 = 0xFFFFFFFFFFFFFFFFULL;

// ------------------------------------------------------------
// 一、地址空间边界。
// ------------------------------------------------------------
void ExpectSpace(
    KswordTests::Suite& suite,
    const wchar_t* label,
    const MemoryTargetSession& session,
    const std::uint64_t first,
    const std::uint64_t last) {
    const AddrSpace space = ScopeAddressSpace(session);
    suite.expect(space.first == first && space.last == last, label);
}

void TestScopeAddressSpace(KswordTests::Suite& suite) {
    // 手算常量：47 位用户空间、4 GiB、52 位物理。
    static_assert(ksword::memwb::kProcessSpaceLast64 == 0x7FFFFFFFFFFFULL, "47-bit user space");
    static_assert(ksword::memwb::kProcessSpaceLast32 == 0xFFFFFFFFULL, "32-bit address space");
    static_assert(ksword::memwb::kPhysicalSpaceLast == 0xFFFFFFFFFFFFFULL, "52-bit physical space");
    static_assert(kKernelSplit == 0xFFFF800000000000ULL, "kernel split");

    ExpectSpace(suite, L"session resolver: 64-bit process space is [0, 0x7FFFFFFFFFFF]",
        ProcessSession(), 0U, 0x7FFFFFFFFFFFULL);
    ExpectSpace(suite, L"session resolver: 32-bit process space is [0, 0xFFFFFFFF]",
        ProcessSession(Channel::UserMode, 32U), 0U, 0xFFFFFFFFULL);
    // 宽度字段不自洽时不猜 32 位：按 64 位处理（Validate 会另行拒绝）。
    MemoryTargetSession oddBits = ProcessSession();
    oddBits.addressBits = 48U;
    ExpectSpace(suite, L"session resolver: a non-32 width falls back to the 64-bit process space",
        oddBits, 0U, 0x7FFFFFFFFFFFULL);
    ExpectSpace(suite, L"session resolver: kernel space is [kKernelSplit, max]",
        KernelSession(), 0xFFFF800000000000ULL, kMaxU64);
    // 内核范围不受 addressBits 影响。
    MemoryTargetSession kernel32 = KernelSession();
    kernel32.addressBits = 32U;
    ExpectSpace(suite, L"session resolver: kernel space ignores the address width",
        kernel32, 0xFFFF800000000000ULL, kMaxU64);
    ExpectSpace(suite, L"session resolver: physical space is [0, 0xFFFFFFFFFFFFF]",
        PhysicalSession(), 0U, 0xFFFFFFFFFFFFFULL);
    // 越界 scope：空区间 {1, 0}。
    MemoryTargetSession badScope = ProcessSession();
    badScope.scope = static_cast<ksword::memwb::Scope>(9U);
    ExpectSpace(suite, L"session resolver: an out-of-range scope has the empty space", badScope, 1U, 0U);

    // AddrSpaceContains：闭区间，边界两侧。
    const AddrSpace process = ScopeAddressSpace(ProcessSession());
    suite.expect(AddrSpaceContains(process, 0U) && AddrSpaceContains(process, 0x7FFFFFFFFFFFULL),
        L"session resolver: both ends of the process space are inside");
    suite.expect(!AddrSpaceContains(process, 0x800000000000ULL) && !AddrSpaceContains(process, kMaxU64),
        L"session resolver: one past the process space is outside");
    const AddrSpace kernel = ScopeAddressSpace(KernelSession());
    suite.expect(AddrSpaceContains(kernel, kKernelSplit) && AddrSpaceContains(kernel, kMaxU64),
        L"session resolver: both ends of the kernel space are inside");
    suite.expect(!AddrSpaceContains(kernel, kKernelSplit - 1U) && !AddrSpaceContains(kernel, 0U),
        L"session resolver: one below the kernel split is outside");
    const AddrSpace physical = ScopeAddressSpace(PhysicalSession());
    suite.expect(AddrSpaceContains(physical, 0xFFFFFFFFFFFFFULL) && !AddrSpaceContains(physical, 0x10000000000000ULL),
        L"session resolver: the 52-bit physical limit is a sharp edge");
    // 单点区间只含自己；空区间什么都不含，包括 0、1 与最大值。
    const AddrSpace point = { 5U, 5U };
    suite.expect(!AddrSpaceContains(point, 4U) && AddrSpaceContains(point, 5U) && !AddrSpaceContains(point, 6U),
        L"session resolver: a single-point space contains exactly that point");
    const AddrSpace empty = { 1U, 0U };
    suite.expect(!AddrSpaceContains(empty, 0U) && !AddrSpaceContains(empty, 1U) && !AddrSpaceContains(empty, kMaxU64),
        L"session resolver: an empty space contains nothing");
}

// ------------------------------------------------------------
// 二、EvaluateForSession 的断言辅助。
// ------------------------------------------------------------
// 可以直接导航：成功、值相符、在范围内，没有成因、细节与建议。
void ExpectAccepted(
    KswordTests::Suite& suite,
    const wchar_t* what,
    const std::string& text,
    const AddressEval& eval,
    const std::uint64_t value) {
    const bool good = eval.expr.ok && eval.expr.value == value && eval.inScopeSpace && eval.Accepted()
        && eval.issue == Issue::None && eval.detail.empty() && eval.matchCount == 0U
        && !eval.suggestScope.has_value();
    suite.expect(good, EvalLabel(what, text, eval).c_str());
}

// 求值成功但被拒绝：值照常给出，不在范围内，没有建议（非规范空洞、超出宽度等）。
void ExpectRejected(
    KswordTests::Suite& suite,
    const wchar_t* what,
    const std::string& text,
    const AddressEval& eval,
    const std::uint64_t value) {
    const bool good = eval.expr.ok && eval.expr.value == value && !eval.inScopeSpace && !eval.Accepted()
        && eval.issue == Issue::None && !eval.suggestScope.has_value();
    suite.expect(good, EvalLabel(what, text, eval).c_str());
}

// 需要切换范围：值照常给出，不在范围内，建议内核；不静默改范围。
void ExpectNeedsKernelSwitch(
    KswordTests::Suite& suite,
    const wchar_t* what,
    const std::string& text,
    const AddressEval& eval,
    const std::uint64_t value) {
    const bool good = eval.expr.ok && eval.expr.value == value && !eval.inScopeSpace && !eval.Accepted()
        && eval.issue == Issue::None && eval.suggestScope.has_value()
        && *eval.suggestScope == Scope::KernelVirtual;
    suite.expect(good, EvalLabel(what, text, eval).c_str());
}

// 求值失败：错误码与成因相符，value 为 0，不在范围内，没有建议。
void ExpectFailed(
    KswordTests::Suite& suite,
    const wchar_t* what,
    const std::string& text,
    const AddressEval& eval,
    const ExprError error,
    const Issue issue,
    const std::string& detail = std::string(),
    const std::size_t matchCount = 0U) {
    const bool good = !eval.expr.ok && eval.expr.value == 0U && eval.expr.error == error
        && eval.issue == issue && eval.detail == detail && eval.matchCount == matchCount
        && !eval.inScopeSpace && !eval.Accepted() && !eval.suggestScope.has_value();
    suite.expect(good, EvalLabel(what, text, eval).c_str());
}

// ------------------------------------------------------------
// 三、范围判定：纯数字在三个范围里的接受/拒绝/建议。
// ------------------------------------------------------------
void TestScopeJudgement(KswordTests::Suite& suite) {
    const Fixture fixture;
    FakePointerReader reader = MakeReader();
    auto eval = [&](const std::string& text, const MemoryTargetSession& session) {
        return EvaluateForSession(text, session, &fixture.process, &fixture.kernel, &reader);
    };

    // 进程 64 位：用户半区内接受；非规范空洞拒绝且不建议；内核半区建议切换。
    const MemoryTargetSession process = ProcessSession();
    ExpectAccepted(suite, L"process zero", "0", eval("0", process), 0U);
    // 纯数字默认十六进制：1233 是 0x1233，不是十进制。
    ExpectAccepted(suite, L"process bare digits are hex", "1233", eval("1233", process), 0x1233ULL);
    ExpectAccepted(suite, L"process last user address", "7FFFFFFFFFFF", eval("7FFFFFFFFFFF", process),
        0x7FFFFFFFFFFFULL);
    ExpectRejected(suite, L"process first hole address", "800000000000", eval("800000000000", process),
        0x800000000000ULL);
    ExpectRejected(suite, L"process last hole address", "FFFF7FFFFFFFFFFF", eval("FFFF7FFFFFFFFFFF", process),
        0xFFFF7FFFFFFFFFFFULL);
    ExpectNeedsKernelSwitch(suite, L"process first kernel address", "FFFF800000000000",
        eval("FFFF800000000000", process), 0xFFFF800000000000ULL);
    ExpectNeedsKernelSwitch(suite, L"process highest address", "FFFFFFFFFFFFFFFF",
        eval("FFFFFFFFFFFFFFFF", process), kMaxU64);
    ExpectNeedsKernelSwitch(suite, L"process debugger-style kernel address", "fffff800`12345678",
        eval("fffff800`12345678", process), 0xFFFFF80012345678ULL);
    // 范围判定不会去碰读取器。
    suite.expect(reader.calls == 0, L"session resolver: plain numbers never touch the reader");

    // 进程 32 位：4 GiB 边界两侧；内核半区地址仍建议切换。
    const MemoryTargetSession process32 = ProcessSession(Channel::UserMode, 32U);
    ExpectAccepted(suite, L"32-bit last address", "FFFFFFFF", eval("FFFFFFFF", process32), 0xFFFFFFFFULL);
    ExpectRejected(suite, L"32-bit first address beyond 4 GiB", "100000000", eval("100000000", process32),
        0x100000000ULL);
    ExpectNeedsKernelSwitch(suite, L"32-bit target given a kernel address", "FFFF800000000000",
        eval("FFFF800000000000", process32), 0xFFFF800000000000ULL);

    // 内核范围：半区内接受；低于 kKernelSplit 的一律拒绝，没有"切回进程"的建议（不知道 pid）。
    const MemoryTargetSession kernel = KernelSession();
    ExpectAccepted(suite, L"kernel split itself", "FFFF800000000000", eval("FFFF800000000000", kernel),
        0xFFFF800000000000ULL);
    ExpectAccepted(suite, L"kernel highest address", "FFFFFFFFFFFFFFFF", eval("FFFFFFFFFFFFFFFF", kernel), kMaxU64);
    ExpectRejected(suite, L"one below the kernel split", "FFFF7FFFFFFFFFFF", eval("FFFF7FFFFFFFFFFF", kernel),
        0xFFFF7FFFFFFFFFFFULL);
    ExpectRejected(suite, L"user address in kernel scope", "1000", eval("1000", kernel), 0x1000ULL);
    ExpectRejected(suite, L"zero in kernel scope", "0", eval("0", kernel), 0U);

    // 物理范围：52 位上限两侧；内核半区地址不建议切换（只有进程范围才建议）。
    const MemoryTargetSession physical = PhysicalSession();
    ExpectAccepted(suite, L"physical zero", "0", eval("0", physical), 0U);
    ExpectAccepted(suite, L"physical last address", "FFFFFFFFFFFFF", eval("FFFFFFFFFFFFF", physical),
        0xFFFFFFFFFFFFFULL);
    ExpectRejected(suite, L"physical first address beyond 52 bits", "10000000000000",
        eval("10000000000000", physical), 0x10000000000000ULL);
    ExpectRejected(suite, L"physical scope given a kernel address", "FFFF800000000000",
        eval("FFFF800000000000", physical), 0xFFFF800000000000ULL);

    // 内核半区判据与全仓唯一阈值一致：建议切换当且仅当 IsKernelVirtualAddress。
    suite.expect(IsKernelVirtualAddress(0xFFFF800000000000ULL) && !IsKernelVirtualAddress(0xFFFF7FFFFFFFFFFFULL),
        L"session resolver: the kernel suggestion uses the one shared kernel split");
}

// ------------------------------------------------------------
// 四、模块表达式：成功、成因透传、范围判定叠加。
// ------------------------------------------------------------
void TestModuleExpressions(KswordTests::Suite& suite) {
    const Fixture fixture;
    FakePointerReader reader = MakeReader();
    auto eval = [&](const std::string& text, const MemoryTargetSession& session) {
        return EvaluateForSession(text, session, &fixture.process, &fixture.kernel, &reader);
    };
    const MemoryTargetSession process = ProcessSession();

    // 成功：模块 + 偏移，偏移无论带不带 0x 都按十六进制。
    const AddressEval plain = eval("client.dll+10", process);
    ExpectAccepted(suite, L"module plus offset", "client.dll+10", plain, kClientBaseA + 0x10ULL);
    suite.expect(plain.expr.usedModule && plain.expr.moduleName == "client.dll" && !plain.expr.usedDeref,
        L"session resolver: a module result reports the module name and no dereference");
    ExpectAccepted(suite, L"quoted module", "\"client.dll\"+0x10", eval("\"client.dll\"+0x10", process),
        kClientBaseA + 0x10ULL);
    ExpectAccepted(suite, L"module disambiguated by full path", "C:\\Windows\\SysWOW64\\ntdll.dll+10",
        eval("C:\\Windows\\SysWOW64\\ntdll.dll+10", process), kNtdllWowBase + 0x10ULL);

    // 基址加偏移后越过用户空间上沿：求值成功，但范围判定拒绝（边界两侧：…F 在内，…10 越界）。
    ExpectAccepted(suite, L"module offset reaching the last user address", "top.dll+F", eval("top.dll+F", process),
        0x7FFFFFFFFFFFULL);
    ExpectRejected(suite, L"module offset crossing the user space limit", "top.dll+10", eval("top.dll+10", process),
        0x800000000000ULL);

    // 失败：成因与细节透传，ExprError 对应。
    ExpectFailed(suite, L"unknown module", "nothere.dll", eval("nothere.dll", process), ExprError::UnknownModule,
        Issue::NotFound);
    const AddressEval kernelOnly = eval("ci.dll+10", process);
    ExpectFailed(suite, L"kernel-only module is not silently resolved", "ci.dll+10", kernelOnly,
        ExprError::UnknownModule, Issue::FoundInKernelOnly, "0xFFFFF80012340000");
    const AddressEval ambiguous = eval("ntdll.dll", process);
    ExpectFailed(suite, L"ambiguous module", "ntdll.dll", ambiguous, ExprError::AmbiguousModule, Issue::Ambiguous,
        "C:\\Windows\\System32\\ntdll.dll\nC:\\Windows\\SysWOW64\\ntdll.dll", 2U);

    // 旧缺陷回归：进程目录是预览进程 B 的，会话是 A —— 一律报"没有可用模块上下文"，
    // 成因是所有者不符，不是 B 的基址。
    MemoryModuleDirectory preview;
    LoadDirectory(preview, OwnerB(), 1U, ProcessRecordsB());
    ExpectFailed(suite, L"preview process directory", "client.dll",
        EvaluateForSession("client.dll", process, &preview, &fixture.kernel, &reader),
        ExprError::NeedsProcess, Issue::ModulesOwnerMismatch);
    MemoryModuleDirectory empty;
    ExpectFailed(suite, L"empty process directory", "client.dll",
        EvaluateForSession("client.dll", process, &empty, &fixture.kernel, &reader),
        ExprError::NeedsProcess, Issue::ModulesNotLoaded);
    MemoryModuleDirectory loading;
    loading.BeginLoad(OwnerA(), 1U);
    ExpectFailed(suite, L"loading process directory", "client.dll",
        EvaluateForSession("client.dll", process, &loading, &fixture.kernel, &reader),
        ExprError::NeedsProcess, Issue::ModulesLoading);
    ExpectFailed(suite, L"no directories", "client.dll",
        EvaluateForSession("client.dll", process, nullptr, nullptr, &reader),
        ExprError::NeedsProcess, Issue::ModulesNotLoaded);

    // 内核范围：用内核目录，成功后落在内核半区；进程独有的模块不回退。
    const MemoryTargetSession kernel = KernelSession();
    ExpectAccepted(suite, L"kernel module plus offset", "ci.dll+10", eval("ci.dll+10", kernel),
        kCiBase + 0x10ULL);
    ExpectAccepted(suite, L"kernel module by tail path", "system32\\ci.dll", eval("system32\\ci.dll", kernel),
        kCiBase);
    ExpectFailed(suite, L"process-only module in kernel scope", "client.dll", eval("client.dll", kernel),
        ExprError::UnknownModule, Issue::NotFound);

    // 物理范围：模块一律 NeedsProcess + ScopeHasNoModules。
    ExpectFailed(suite, L"module in physical scope", "client.dll", eval("client.dll", PhysicalSession()),
        ExprError::NeedsProcess, Issue::ScopeHasNoModules);

    // 模块查询从不触碰读取器。
    suite.expect(reader.calls == 0, L"session resolver: module expressions never touch the reader");
}

// ------------------------------------------------------------
// 五、没有目标、纯语法/数值错误。
// ------------------------------------------------------------
void TestNoTargetAndPlainErrors(KswordTests::Suite& suite) {
    const Fixture fixture;
    FakePointerReader reader = MakeReader();
    auto eval = [&](const std::string& text, const MemoryTargetSession& session) {
        return EvaluateForSession(text, session, &fixture.process, &fixture.kernel, &reader);
    };

    // 没有目标：数字算得出来，却没有地址空间放它；成因 NoTarget，不在范围内，没有建议。
    const AddressEval number = eval("1000", MemoryTargetSession());
    const bool numberGood = number.expr.ok && number.expr.value == 0x1000ULL && number.issue == Issue::NoTarget
        && !number.inScopeSpace && !number.Accepted() && !number.suggestScope.has_value();
    suite.expect(numberGood, EvalLabel(L"number without a target", "1000", number).c_str());
    // 没有目标时即使数字落在内核半区也不给建议（没有"当前范围"可言）。
    const AddressEval kernelNumber = eval("FFFF800000000000", MemoryTargetSession());
    suite.expect(kernelNumber.expr.ok && kernelNumber.issue == Issue::NoTarget && !kernelNumber.Accepted()
            && !kernelNumber.suggestScope.has_value(),
        EvalLabel(L"kernel number without a target", "FFFF800000000000", kernelNumber).c_str());
    ExpectFailed(suite, L"module without a target", "client.dll", eval("client.dll", MemoryTargetSession()),
        ExprError::NeedsProcess, Issue::NoTarget);
    ExpectFailed(suite, L"dereference without a target", "[1000]", eval("[1000]", MemoryTargetSession()),
        ExprError::DerefFailed, Issue::NoTarget);
    // 没有目标时语法错误照常报告（先求值，后看目标）。
    ExpectFailed(suite, L"syntax error without a target", "1+", eval("1+", MemoryTargetSession()),
        ExprError::BadSyntax, Issue::None);
    suite.expect(reader.calls == 0, L"session resolver: no target means no read at all");

    // 纯语法/数值错误：成因 None（解析器根本没被调用）。
    const MemoryTargetSession process = ProcessSession();
    ExpectFailed(suite, L"empty text", "", eval("", process), ExprError::Empty, Issue::None);
    ExpectFailed(suite, L"only spaces", "   ", eval("   ", process), ExprError::Empty, Issue::None);
    ExpectFailed(suite, L"dangling plus", "1+", eval("1+", process), ExprError::BadSyntax, Issue::None);
    ExpectFailed(suite, L"bare 0x", "0x", eval("0x", process), ExprError::BadNumber, Issue::None);
    ExpectFailed(suite, L"sum overflow", "FFFFFFFFFFFFFFFF+1", eval("FFFFFFFFFFFFFFFF+1", process),
        ExprError::Overflow, Issue::None);
    ExpectFailed(suite, L"unterminated bracket", "[1000", eval("[1000", process), ExprError::BadSyntax,
        Issue::None);
    suite.expect(reader.calls == 0, L"session resolver: syntax and number errors never touch the reader");

    // Accepted() 同时要求成功与在范围内：任何一项缺失都不算。
    AddressEval handBuilt;
    suite.expect(!handBuilt.Accepted(), L"session resolver: a default AddressEval is not accepted");
    handBuilt.inScopeSpace = true;
    suite.expect(!handBuilt.Accepted(), L"session resolver: in-space without a successful evaluation is not accepted");
    handBuilt.expr.ok = true;
    suite.expect(handBuilt.Accepted(), L"session resolver: success plus in-space is accepted");
    handBuilt.inScopeSpace = false;
    suite.expect(!handBuilt.Accepted(), L"session resolver: success without in-space is not accepted");
}

// ------------------------------------------------------------
// 六、解引用：读取次数、宽度、门控成因、求值只跑一次。
// ------------------------------------------------------------
void TestDereferences(KswordTests::Suite& suite) {
    const Fixture fixture;
    const MemoryTargetSession process = ProcessSession();

    // 一层解引用：值 0x2000，读取器恰好被调用一次，地址 0x1000，宽度 8。
    {
        FakePointerReader reader = MakeReader();
        const AddressEval one = EvaluateForSession("[1000]", process, &fixture.process, &fixture.kernel, &reader);
        ExpectAccepted(suite, L"one dereference", "[1000]", one, 0x2000ULL);
        suite.expect(one.expr.usedDeref && reader.calls == 1 && reader.addresses == std::vector<std::uint64_t>{ 0x1000ULL }
                && reader.widths == std::vector<std::uint32_t>{ 8U },
            L"session resolver: one dereference reads once, at the inner address, with width 8");
    }
    // 嵌套两层：两次读取，顺序由内到外。
    {
        FakePointerReader reader = MakeReader();
        const AddressEval nested = EvaluateForSession("[[1000]]", process, &fixture.process, &fixture.kernel, &reader);
        ExpectAccepted(suite, L"nested dereference", "[[1000]]", nested, 0x3000ULL);
        suite.expect(reader.calls == 2 && reader.addresses == std::vector<std::uint64_t>{ 0x1000ULL, 0x2000ULL },
            L"session resolver: nested dereferences read inner first");
    }
    // 求值只跑一次：两个解引用项恰好两次读取（若入口把表达式求值了两遍，会是 4 次）。
    {
        FakePointerReader reader = MakeReader();
        const AddressEval twice = EvaluateForSession("[1000]+[1008]", process, &fixture.process, &fixture.kernel, &reader);
        // 手算：0x2000 + 0x7777 = 0x9777。
        ExpectAccepted(suite, L"two dereference terms", "[1000]+[1008]", twice, 0x9777ULL);
        suite.expect(reader.calls == 2 && reader.addresses == std::vector<std::uint64_t>{ 0x1000ULL, 0x1008ULL },
            L"session resolver: evaluation runs once, so two dereference terms mean exactly two reads");
    }
    // 模块 + 解引用组合：模块先查（不读），再读一次。
    {
        FakePointerReader reader = MakeReader();
        reader.memory[kClientBaseA + 0x10U] = 0x500ULL;
        const AddressEval mixed = EvaluateForSession("[client.dll+10]+8", process, &fixture.process, &fixture.kernel, &reader);
        ExpectAccepted(suite, L"module inside a dereference", "[client.dll+10]+8", mixed, 0x508ULL);
        suite.expect(reader.calls == 1 && reader.addresses[0] == kClientBaseA + 0x10U,
            L"session resolver: the module is resolved without a read, then dereferenced once");
    }

    // 32 位目标：宽度 4；0xFFFFFFFF 是空间内最后一个地址；高位非零的读取结果判失败。
    {
        FakePointerReader reader = MakeReader();
        const MemoryTargetSession process32 = ProcessSession(Channel::UserMode, 32U);
        ExpectAccepted(suite, L"32-bit dereference reaching the last address", "[6008]",
            EvaluateForSession("[6008]", process32, &fixture.process, &fixture.kernel, &reader), 0xFFFFFFFFULL);
        suite.expect(reader.widths == std::vector<std::uint32_t>{ 4U },
            L"session resolver: a 32-bit target dereferences with width 4");
        ExpectFailed(suite, L"32-bit dereference returning more than 32 bits", "[6000]",
            EvaluateForSession("[6000]", process32, &fixture.process, &fixture.kernel, &reader),
            ExprError::DerefFailed, Issue::DerefReadFailed, "0x6000 pointer-too-wide");
    }

    // 解引用结果落在内核半区：值给出，建议切换（与纯数字一致）。
    {
        FakePointerReader reader = MakeReader();
        ExpectNeedsKernelSwitch(suite, L"dereference result in the kernel half", "[9000]",
            EvaluateForSession("[9000]", process, &fixture.process, &fixture.kernel, &reader),
            0xFFFFF80000001000ULL);
        // 内核范围下同一个结果是范围内地址。
        ExpectAccepted(suite, L"dereference result in kernel scope", "[9000]",
            EvaluateForSession("[9000]", KernelSession(), &fixture.process, &fixture.kernel, &reader),
            0xFFFFF80000001000ULL);
    }

    // 读取失败：成因 DerefReadFailed，detail 是失败地址（读取器给了说明则追加）。
    {
        FakePointerReader reader = MakeReader();
        ExpectFailed(suite, L"unreadable pointer", "[5000]",
            EvaluateForSession("[5000]", process, &fixture.process, &fixture.kernel, &reader),
            ExprError::DerefFailed, Issue::DerefReadFailed, "0x5000");
        reader.failureText = "STATUS_PARTIAL_COPY";
        ExpectFailed(suite, L"unreadable pointer with a note", "[5000]",
            EvaluateForSession("[5000]", process, &fixture.process, &fixture.kernel, &reader),
            ExprError::DerefFailed, Issue::DerefReadFailed, "0x5000 STATUS_PARTIAL_COPY");
        // 链中途失败就停：[[5000]] 只读了内层一次。
        reader.calls = 0;
        reader.addresses.clear();
        ExpectFailed(suite, L"failure in the middle of a chain", "[[5000]]",
            EvaluateForSession("[[5000]]", process, &fixture.process, &fixture.kernel, &reader),
            ExprError::DerefFailed, Issue::DerefReadFailed, "0x5000 STATUS_PARTIAL_COPY");
        suite.expect(reader.calls == 1, L"session resolver: a failing inner read stops the chain");
    }
    // 没有读取器。
    ExpectFailed(suite, L"dereference without a reader", "[1000]",
        EvaluateForSession("[1000]", process, &fixture.process, &fixture.kernel, nullptr),
        ExprError::DerefFailed, Issue::DerefReadFailed, "no-reader");

    // 门控：DDMA 通道与物理范围的解引用被拒绝，读取器零调用；纯数字与模块在 DDMA 下照常。
    {
        FakePointerReader reader = MakeReader();
        const MemoryTargetSession ddma = ProcessSession(Channel::Ddma);
        ExpectFailed(suite, L"dereference over DDMA", "[1000]",
            EvaluateForSession("[1000]", ddma, &fixture.process, &fixture.kernel, &reader),
            ExprError::DerefFailed, Issue::DerefDeniedDdma);
        ExpectFailed(suite, L"nested dereference over DDMA", "[[1000]]",
            EvaluateForSession("[[1000]]", ddma, &fixture.process, &fixture.kernel, &reader),
            ExprError::DerefFailed, Issue::DerefDeniedDdma);
        ExpectFailed(suite, L"dereference in physical scope", "[1000]",
            EvaluateForSession("[1000]", PhysicalSession(), &fixture.process, &fixture.kernel, &reader),
            ExprError::DerefFailed, Issue::DerefDeniedScope);
        ExpectFailed(suite, L"dereference in physical scope over DDMA", "[1000]",
            EvaluateForSession("[1000]", PhysicalSession(Channel::Ddma), &fixture.process, &fixture.kernel, &reader),
            ExprError::DerefFailed, Issue::DerefDeniedScope);
        suite.expect(reader.calls == 0, L"session resolver: gated dereferences never reach the reader");
        ExpectAccepted(suite, L"plain number over DDMA", "1000",
            EvaluateForSession("1000", ddma, &fixture.process, &fixture.kernel, &reader), 0x1000ULL);
        ExpectAccepted(suite, L"module over DDMA", "client.dll+10",
            EvaluateForSession("client.dll+10", ddma, &fixture.process, &fixture.kernel, &reader),
            kClientBaseA + 0x10ULL);
        suite.expect(reader.calls == 0, L"session resolver: numbers and modules over DDMA read nothing");
    }
}

} // namespace

void RunMemwbSessionResolverEvalChecks(KswordTests::Suite& suite) {
    TestScopeAddressSpace(suite);
    TestScopeJudgement(suite);
    TestModuleExpressions(suite);
    TestNoTargetAndPlainErrors(suite);
    TestDereferences(suite);
}

} // namespace memwb_wpb_test
