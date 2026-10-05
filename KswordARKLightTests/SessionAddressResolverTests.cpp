// 内存工作台会话地址解析器（shared/evidence/memory_workbench/SessionAddressResolver.h）
// 的离线测试 —— 本文件覆盖 IAddressExprResolver 的两个方法与套件入口：
// LookupModule（目录状态 / 所有者 / 重名 / 内核提示 / 不回退）与 ReadPointer（解引用门控）。
// EvaluateForSession 与地址空间边界在 SessionAddressResolverTests.Eval.cpp，由入口调用。
//
// 为什么这个解析器值得一整套断言：它属于**算错了不会报错**的那一类。
// 旧代码里三处毛病都是静默的：用预览进程的模块缓存解析已附加进程的表达式、进程里找不到
// 就悄悄回退内核模块、在磁盘传输通道上每次解引用都改写暂存扇区。下面的测试逐条钉住新行为：
//   * 目录所有者不符恒 ModulesOwnerMismatch，绝不拿别的进程的基址作答；
//   * 找不到不回退，只提示 FoundInKernelOnly；
//   * 物理范围与 DDMA 通道下解引用对读取器的调用次数恒为 0。
//
// 断言原则与 MemoryAddressExprTests.cpp 一致：期望值手算写死；边界两侧都测
// （列出 5 条/6 条重名路径的分界）；失败输出参数保持安全初值（base/value 恒 0）；
// 用假读取器的调用计数证明"门控拒绝时一次都没读"。

#include "ModuleResolverTestSupport.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using namespace memwb_wpb_test;

using ksword::memwb::ModuleLookup;
using ksword::memwb::SessionAddressResolver;

// LookupOutcome：一次 LookupModule 调用的全部可见结果。
struct LookupOutcome {
    ModuleLookup result = ModuleLookup::Found;
    std::uint64_t base = 0;
    Issue issue = Issue::None;
    std::string detail;
    std::size_t matchCount = 0;
};

// RunLookup：用给定会话与目录跑一次 LookupModule。baseOut 预置成垃圾值，
// 用来证明失败路径把它清零而不是带出残留。
LookupOutcome RunLookup(
    const MemoryTargetSession& session,
    const MemoryModuleDirectory* processDirectory,
    const MemoryModuleDirectory* kernelDirectory,
    const std::string& name) {
    SessionAddressResolver resolver(session, processDirectory, kernelDirectory, nullptr);
    LookupOutcome outcome;
    outcome.base = 0xDEADBEEFDEADBEEFULL;
    outcome.result = resolver.LookupModule(name, outcome.base);
    outcome.issue = resolver.LastIssue();
    outcome.detail = resolver.LastDetail();
    outcome.matchCount = resolver.LastMatchCount();
    return outcome;
}

std::wstring OutcomeLabel(const wchar_t* what, const std::string& name, const LookupOutcome& outcome) {
    std::wostringstream stream;
    stream << L"session resolver: " << what << L" [" << Widen(name) << L"] got result="
           << static_cast<int>(outcome.result) << L" base=0x" << std::hex << outcome.base << std::dec
           << L" issue=" << static_cast<int>(outcome.issue) << L" detail='" << Widen(outcome.detail)
           << L"' matches=" << outcome.matchCount;
    return stream.str();
}

// ExpectLookup：结果枚举、成因、基址、细节串、命中总数全部相符。
void ExpectLookup(
    KswordTests::Suite& suite,
    const wchar_t* what,
    const MemoryTargetSession& session,
    const MemoryModuleDirectory* processDirectory,
    const MemoryModuleDirectory* kernelDirectory,
    const std::string& name,
    const ModuleLookup result,
    const Issue issue,
    const std::uint64_t base = 0U,
    const std::string& detail = std::string(),
    const std::size_t matchCount = 0U) {
    const LookupOutcome outcome = RunLookup(session, processDirectory, kernelDirectory, name);
    const bool good = outcome.result == result && outcome.issue == issue && outcome.base == base
        && outcome.detail == detail && outcome.matchCount == matchCount;
    suite.expect(good, OutcomeLabel(what, name, outcome).c_str());
}

// ------------------------------------------------------------
// 一、没有目标 / 物理范围：NeedsProcess + 对应成因，且不碰任何目录。
// ------------------------------------------------------------
void TestNoTargetAndPhysical(KswordTests::Suite& suite) {
    const Fixture fixture;

    // 默认会话故意是无效的（进程范围、pid 0）：这是"还没选目标"。
    ExpectLookup(suite, L"default session", MemoryTargetSession(), &fixture.process, &fixture.kernel,
        "client.dll", ModuleLookup::NeedsProcess, Issue::NoTarget);
    // 其它形式的不自洽：内核会话带 pid、地址宽度非法、scope 越界。
    MemoryTargetSession kernelWithPid = KernelSession();
    kernelWithPid.pid = 5U;
    ExpectLookup(suite, L"kernel session carrying a pid", kernelWithPid, &fixture.process, &fixture.kernel,
        "ntoskrnl.exe", ModuleLookup::NeedsProcess, Issue::NoTarget);
    MemoryTargetSession oddBits = ProcessSession();
    oddBits.addressBits = 48U;
    ExpectLookup(suite, L"invalid address width", oddBits, &fixture.process, &fixture.kernel,
        "client.dll", ModuleLookup::NeedsProcess, Issue::NoTarget);
    MemoryTargetSession badScope = ProcessSession();
    badScope.scope = static_cast<ksword::memwb::Scope>(9U);
    ExpectLookup(suite, L"out-of-range scope", badScope, &fixture.process, &fixture.kernel,
        "client.dll", ModuleLookup::NeedsProcess, Issue::NoTarget);

    // 物理范围没有模块：即便两个目录里都有同名模块也不去查，四种通道都一样。
    const Channel channels[] = { Channel::UserMode, Channel::StandardDriver, Channel::Hvm, Channel::Ddma };
    for (const Channel channel : channels) {
        ExpectLookup(suite, L"physical scope, name in both directories", PhysicalSession(channel),
            &fixture.process, &fixture.kernel, "shared.dll", ModuleLookup::NeedsProcess,
            Issue::ScopeHasNoModules);
    }
    ExpectLookup(suite, L"physical scope, kernel-only name", PhysicalSession(), &fixture.process,
        &fixture.kernel, "ntoskrnl.exe", ModuleLookup::NeedsProcess, Issue::ScopeHasNoModules);
    ExpectLookup(suite, L"physical scope, no directories at all", PhysicalSession(), nullptr, nullptr,
        "client.dll", ModuleLookup::NeedsProcess, Issue::ScopeHasNoModules);
}

// ------------------------------------------------------------
// 二、进程范围的目录状态：没有目录 / Empty / Loading / Failed / 所有者不符。
// ------------------------------------------------------------
void TestProcessDirectoryStates(KswordTests::Suite& suite) {
    const Fixture fixture;
    const MemoryTargetSession session = ProcessSession();

    // 没有目录、Empty：模块没加载过。
    ExpectLookup(suite, L"no process directory", session, nullptr, &fixture.kernel, "client.dll",
        ModuleLookup::NeedsProcess, Issue::ModulesNotLoaded);
    MemoryModuleDirectory empty;
    ExpectLookup(suite, L"empty process directory", session, &empty, &fixture.kernel, "client.dll",
        ModuleLookup::NeedsProcess, Issue::ModulesNotLoaded);

    // Loading（属于当前目标）：正在加载，与"没加载过"分开报。
    MemoryModuleDirectory loading;
    loading.BeginLoad(OwnerA(), 3U);
    ExpectLookup(suite, L"process directory still loading", session, &loading, &fixture.kernel, "client.dll",
        ModuleLookup::NeedsProcess, Issue::ModulesLoading);
    // Loading 但属于别的进程：先报所有者不符，不是"正在加载"。
    MemoryModuleDirectory loadingForB;
    loadingForB.BeginLoad(OwnerB(), 3U);
    ExpectLookup(suite, L"directory loading for another process", session, &loadingForB, &fixture.kernel,
        "client.dll", ModuleLookup::NeedsProcess, Issue::ModulesOwnerMismatch);

    // Failed：算"没加载"，失败说明带出去。
    MemoryModuleDirectory failed;
    failed.BeginLoad(OwnerA(), 4U);
    failed.Fail(OwnerA(), 4U, "enum-failed");
    ExpectLookup(suite, L"failed process directory", session, &failed, &fixture.kernel, "client.dll",
        ModuleLookup::NeedsProcess, Issue::ModulesNotLoaded, 0U, "enum-failed");
    MemoryModuleDirectory failedForB;
    failedForB.BeginLoad(OwnerB(), 4U);
    failedForB.Fail(OwnerB(), 4U, "enum-failed");
    ExpectLookup(suite, L"failed directory of another process", session, &failedForB, &fixture.kernel,
        "client.dll", ModuleLookup::NeedsProcess, Issue::ModulesOwnerMismatch);

    // 旧缺陷回归：预览进程 B 的目录 Ready，会话是 A。两个进程都有 client.dll，
    // 结果必须是所有者不符，base 为 0，而不是 B 的基址。
    MemoryModuleDirectory preview;
    LoadDirectory(preview, OwnerB(), 1U, ProcessRecordsB());
    ExpectLookup(suite, L"preview process directory vs session A", session, &preview, &fixture.kernel,
        "client.dll", ModuleLookup::NeedsProcess, Issue::ModulesOwnerMismatch);
    ExpectLookup(suite, L"preview process directory, name only B has", session, &preview, &fixture.kernel,
        "previewonly.dll", ModuleLookup::NeedsProcess, Issue::ModulesOwnerMismatch);
    // 所有者不符时连内核提示也不给：目录根本不是这个目标的。
    ExpectLookup(suite, L"owner mismatch hides the kernel hint", session, &preview, &fixture.kernel,
        "ci.dll", ModuleLookup::NeedsProcess, Issue::ModulesOwnerMismatch);

    // pid 被复用：同 pid、两个已知且不同的创建时间。
    MemoryModuleDirectory recycled;
    LoadDirectory(recycled, MakeOwner(Scope::ProcessVirtual, kPidA, kCreateA + 1U), 1U, ProcessRecordsA());
    ExpectLookup(suite, L"recycled pid", session, &recycled, &fixture.kernel, "client.dll",
        ModuleLookup::NeedsProcess, Issue::ModulesOwnerMismatch);
    // 创建时间差 0：同一实例，正常命中。
    MemoryModuleDirectory sameInstance;
    LoadDirectory(sameInstance, MakeOwner(Scope::ProcessVirtual, kPidA, kCreateA), 1U, ProcessRecordsA());
    ExpectLookup(suite, L"same instance", session, &sameInstance, &fixture.kernel, "client.dll",
        ModuleLookup::Found, Issue::None, kClientBaseA);
    // 会话身份未锚定（创建时间 0）：只比 pid，仍可用。
    MemoryTargetSession unanchored = ProcessSession();
    unanchored.processCreateTime100ns = 0U;
    ExpectLookup(suite, L"unanchored session identity", unanchored, &fixture.process, &fixture.kernel,
        "client.dll", ModuleLookup::Found, Issue::None, kClientBaseA);

    // 32 位目标：目录规则与 64 位一致。
    ExpectLookup(suite, L"32-bit process session", ProcessSession(Channel::UserMode, 32U), &fixture.process,
        &fixture.kernel, "client.dll", ModuleLookup::Found, Issue::None, kClientBaseA);
}

// ------------------------------------------------------------
// 三、进程目录 Ready：命中 / 缺失 / 重名（前 5 条路径）。
// ------------------------------------------------------------
void TestProcessLookupResults(KswordTests::Suite& suite) {
    const Fixture fixture;
    const MemoryTargetSession session = ProcessSession();

    ExpectLookup(suite, L"found", session, &fixture.process, &fixture.kernel, "client.dll",
        ModuleLookup::Found, Issue::None, kClientBaseA);
    ExpectLookup(suite, L"found, other case", session, &fixture.process, &fixture.kernel, "CLIENT.DLL",
        ModuleLookup::Found, Issue::None, kClientBaseA);
    ExpectLookup(suite, L"found by full path", session, &fixture.process, &fixture.kernel,
        "C:\\Game\\client.dll", ModuleLookup::Found, Issue::None, kClientBaseA);
    ExpectLookup(suite, L"missing everywhere", session, &fixture.process, &fixture.kernel, "nothere.dll",
        ModuleLookup::NotFound, Issue::NotFound);
    // 目录里有同名模块时进程范围优先用进程目录：不会因为内核里也有就去拿内核基址。
    ExpectLookup(suite, L"name in both directories, process scope", session, &fixture.process, &fixture.kernel,
        "shared.dll", ModuleLookup::Found, Issue::None, kSharedBaseProcess);

    // WOW64 式 ntdll 重名：Ambiguous，detail 列出两条完整路径（目录顺序，换行分隔），总数 2。
    ExpectLookup(suite, L"WOW64 ntdll duplicate", session, &fixture.process, &fixture.kernel, "ntdll.dll",
        ModuleLookup::Ambiguous, Issue::Ambiguous, 0U,
        "C:\\Windows\\System32\\ntdll.dll\nC:\\Windows\\SysWOW64\\ntdll.dll", 2U);
    // 给完整路径即可消歧。
    ExpectLookup(suite, L"ntdll disambiguated by path", session, &fixture.process, &fixture.kernel,
        "C:\\Windows\\SysWOW64\\ntdll.dll", ModuleLookup::Found, Issue::None, kNtdllWowBase);

    // 七份 many.dll：只列前 5 条，总数仍是 7。
    const LookupOutcome many = RunLookup(session, &fixture.process, &fixture.kernel, "many.dll");
    const std::string firstFive = "C:\\Dir1\\many.dll\nC:\\Dir2\\many.dll\nC:\\Dir3\\many.dll\n"
        "C:\\Dir4\\many.dll\nC:\\Dir5\\many.dll";
    suite.expect(many.result == ModuleLookup::Ambiguous && many.issue == Issue::Ambiguous
            && many.base == 0U && many.matchCount == 7U && many.detail == firstFive,
        OutcomeLabel(L"seven duplicates list the first five paths only", "many.dll", many).c_str());
    suite.expect(many.detail.find("Dir6") == std::string::npos && many.detail.find("Dir7") == std::string::npos,
        L"session resolver: the sixth and seventh duplicates are not listed");

    // 分界两侧：恰好 5 份全列、6 份仍只列 5 份。
    for (std::uint64_t count = 5U; count <= 6U; ++count) {
        std::vector<ModuleRecord> records;
        std::string expectedDetail;
        for (std::uint64_t index = 1U; index <= count; ++index) {
            const std::string path = "C:\\P" + std::to_string(index) + "\\dup.dll";
            records.push_back(MakeRecord("dup.dll", path, 0x1000U * index));
            if (index <= 5U) {
                if (!expectedDetail.empty()) {
                    expectedDetail.push_back('\n');
                }
                expectedDetail += path;
            }
        }
        MemoryModuleDirectory dir;
        LoadDirectory(dir, OwnerA(), 1U, std::move(records));
        ExpectLookup(suite, count == 5U ? L"exactly five duplicates" : L"six duplicates", session, &dir, nullptr,
            "dup.dll", ModuleLookup::Ambiguous, Issue::Ambiguous, 0U, expectedDetail, static_cast<std::size_t>(count));
    }

    // 没有完整路径的重名记录退而列文件名，不会出现空行。
    MemoryModuleDirectory pathless;
    LoadDirectory(pathless, OwnerA(), 1U, { MakeRecord("x.dll", "", 1U), MakeRecord("x.dll", "", 2U) });
    ExpectLookup(suite, L"pathless duplicates", session, &pathless, nullptr, "x.dll",
        ModuleLookup::Ambiguous, Issue::Ambiguous, 0U, "x.dll\nx.dll", 2U);
}

// ------------------------------------------------------------
// 四、不再自动回退内核：FoundInKernelOnly 只是提示。
// ------------------------------------------------------------
void TestNoKernelFallback(KswordTests::Suite& suite) {
    const Fixture fixture;
    const MemoryTargetSession session = ProcessSession();

    // 进程里没有 CI.dll，内核目录已加载且命中：返回 NotFound（不是 Found），成因 FoundInKernelOnly，
    // detail 是内核基址（0x 大写十六进制），ModuleLookup 的基址输出仍为 0。
    ExpectLookup(suite, L"kernel-only module", session, &fixture.process, &fixture.kernel, "ci.dll",
        ModuleLookup::NotFound, Issue::FoundInKernelOnly, 0U, "0xFFFFF80012340000");
    ExpectLookup(suite, L"kernel-only module by tail path", session, &fixture.process, &fixture.kernel,
        "system32\\ci.dll", ModuleLookup::NotFound, Issue::FoundInKernelOnly, 0U, "0xFFFFF80012340000");
    ExpectLookup(suite, L"kernel-only exe", session, &fixture.process, &fixture.kernel, "NTOSKRNL.EXE",
        ModuleLookup::NotFound, Issue::FoundInKernelOnly, 0U, "0xFFFFF80010000000");
    // 32 位目标同样只给提示。
    ExpectLookup(suite, L"kernel-only module for a 32-bit target", ProcessSession(Channel::UserMode, 32U),
        &fixture.process, &fixture.kernel, "ci.dll", ModuleLookup::NotFound, Issue::FoundInKernelOnly, 0U,
        "0xFFFFF80012340000");

    // 内核目录没就绪（没有/Empty/Loading）或没命中：只是普通 NotFound，没有提示。
    ExpectLookup(suite, L"no kernel directory", session, &fixture.process, nullptr, "ci.dll",
        ModuleLookup::NotFound, Issue::NotFound);
    MemoryModuleDirectory emptyKernel;
    ExpectLookup(suite, L"empty kernel directory", session, &fixture.process, &emptyKernel, "ci.dll",
        ModuleLookup::NotFound, Issue::NotFound);
    MemoryModuleDirectory loadingKernel;
    loadingKernel.BeginLoad(KernelOwner(), 1U);
    ExpectLookup(suite, L"kernel directory still loading", session, &fixture.process, &loadingKernel, "ci.dll",
        ModuleLookup::NotFound, Issue::NotFound);
    ExpectLookup(suite, L"kernel directory without the name", session, &fixture.process, &fixture.kernel,
        "nothere.sys", ModuleLookup::NotFound, Issue::NotFound);
    // 内核里也是重名：不算"唯一命中"，不给提示。
    ExpectLookup(suite, L"ambiguous in the kernel only", session, &fixture.process, &fixture.kernel, "dup.sys",
        ModuleLookup::NotFound, Issue::NotFound);
    // 把一个进程目录误当作内核目录传入：所有者不符，当作没有，不给提示。
    ExpectLookup(suite, L"a process directory passed as the kernel directory", session, &fixture.process,
        &fixture.process, "ci.dll", ModuleLookup::NotFound, Issue::NotFound);

    // 反方向同样不回退：内核范围找一个只有进程目录里才有的模块，就是 NotFound。
    ExpectLookup(suite, L"process-only module in kernel scope", KernelSession(), &fixture.process,
        &fixture.kernel, "client.dll", ModuleLookup::NotFound, Issue::NotFound);
}

// ------------------------------------------------------------
// 五、内核范围：只用内核目录，所有者是内核。
// ------------------------------------------------------------
void TestKernelScopeLookup(KswordTests::Suite& suite) {
    const Fixture fixture;
    const MemoryTargetSession session = KernelSession();

    ExpectLookup(suite, L"kernel found", session, &fixture.process, &fixture.kernel, "ntoskrnl.exe",
        ModuleLookup::Found, Issue::None, kNtoskrnlBase);
    ExpectLookup(suite, L"kernel found by NT path", session, nullptr, &fixture.kernel,
        "\\SystemRoot\\system32\\CI.dll", ModuleLookup::Found, Issue::None, kCiBase);
    ExpectLookup(suite, L"kernel found by tail path", session, nullptr, &fixture.kernel, "system32\\ci.dll",
        ModuleLookup::Found, Issue::None, kCiBase);
    ExpectLookup(suite, L"kernel found by win32 spelling", session, nullptr, &fixture.kernel,
        "C:\\Windows\\system32\\drivers\\foo.sys", ModuleLookup::Found, Issue::None, kFooBase);
    // 同名在两个目录里：内核范围取内核目录的基址，不是进程目录的。
    ExpectLookup(suite, L"name in both directories, kernel scope", session, &fixture.process, &fixture.kernel,
        "shared.dll", ModuleLookup::Found, Issue::None, kSharedBaseKernel);
    ExpectLookup(suite, L"kernel missing", session, &fixture.process, &fixture.kernel, "nothere.sys",
        ModuleLookup::NotFound, Issue::NotFound);
    // 内核重名：detail 列两条内核路径。
    ExpectLookup(suite, L"kernel duplicate", session, &fixture.process, &fixture.kernel, "dup.sys",
        ModuleLookup::Ambiguous, Issue::Ambiguous, 0U,
        "\\SystemRoot\\system32\\drivers\\dup.sys\n\\SystemRoot\\system32\\dup.sys", 2U);
    ExpectLookup(suite, L"kernel duplicate disambiguated", session, &fixture.process, &fixture.kernel,
        "drivers\\dup.sys", ModuleLookup::Found, Issue::None, kDupDriversBase);

    // 内核目录的状态：没有/Empty、Loading、所有者不符。
    ExpectLookup(suite, L"no kernel directory in kernel scope", session, &fixture.process, nullptr,
        "ntoskrnl.exe", ModuleLookup::NeedsProcess, Issue::ModulesNotLoaded);
    MemoryModuleDirectory loadingKernel;
    loadingKernel.BeginLoad(KernelOwner(), 1U);
    ExpectLookup(suite, L"kernel directory loading", session, nullptr, &loadingKernel, "ntoskrnl.exe",
        ModuleLookup::NeedsProcess, Issue::ModulesLoading);
    ExpectLookup(suite, L"process directory passed as the kernel directory", session, nullptr,
        &fixture.process, "client.dll", ModuleLookup::NeedsProcess, Issue::ModulesOwnerMismatch);
}

// ------------------------------------------------------------
// 六、Last* 每次调用先清零：上一次的失败成因不会带到下一次。
// ------------------------------------------------------------
void TestOutcomeReset(KswordTests::Suite& suite) {
    const Fixture fixture;
    FakePointerReader reader = MakeReader();
    SessionAddressResolver resolver(ProcessSession(), &fixture.process, &fixture.kernel, &reader);

    std::uint64_t base = 0xDEADU;
    // 先来一次 Ambiguous（带 detail 与总数），再来一次 Found：Last* 必须全部清零。
    suite.expect(resolver.LookupModule("many.dll", base) == ModuleLookup::Ambiguous
            && resolver.LastMatchCount() == 7U && !resolver.LastDetail().empty(),
        L"session resolver: an ambiguous lookup records detail and count");
    suite.expect(resolver.LookupModule("client.dll", base) == ModuleLookup::Found && base == kClientBaseA
            && resolver.LastIssue() == Issue::None && resolver.LastDetail().empty()
            && resolver.LastMatchCount() == 0U,
        L"session resolver: a successful lookup clears the previous failure");

    // 反过来：先成功再失败，也只留下这一次的成因。
    std::uint64_t value = 0xDEADU;
    suite.expect(resolver.ReadPointer(0x1000U, 8U, value) && value == 0x2000U
            && resolver.LastIssue() == Issue::None,
        L"session resolver: a successful read reports no issue");
    suite.expect(!resolver.ReadPointer(0x5000U, 8U, value) && value == 0U
            && resolver.LastIssue() == Issue::DerefReadFailed && resolver.LastDetail() == "0x5000",
        L"session resolver: a failed read records its own issue and clears the value");
    suite.expect(resolver.LookupModule("client.dll", base) == ModuleLookup::Found
            && resolver.LastIssue() == Issue::None && resolver.LastDetail().empty(),
        L"session resolver: a lookup after a failed read starts from a clean slate");
}

// ------------------------------------------------------------
// 七、解引用门控：物理范围与 DDMA 通道对读取器的调用次数恒为 0。
// ------------------------------------------------------------
void TestDerefGatingMatrix(KswordTests::Suite& suite) {
    const Scope scopes[] = { Scope::ProcessVirtual, Scope::KernelVirtual, Scope::Physical };
    const Channel channels[] = { Channel::UserMode, Channel::StandardDriver, Channel::Hvm, Channel::Ddma };
    for (const Scope scope : scopes) {
        for (const Channel channel : channels) {
            MemoryTargetSession session = ProcessSession(channel);
            if (scope == Scope::KernelVirtual) {
                session = KernelSession(channel);
            } else if (scope == Scope::Physical) {
                session = PhysicalSession(channel);
            }
            // 期望：物理 -> DerefDeniedScope（先于通道判断）；非物理且 DDMA -> DerefDeniedDdma；
            // 其余放行，读取器恰好被调用一次。
            Issue expectedIssue = Issue::None;
            if (scope == Scope::Physical) {
                expectedIssue = Issue::DerefDeniedScope;
            } else if (channel == Channel::Ddma) {
                expectedIssue = Issue::DerefDeniedDdma;
            }
            FakePointerReader reader = MakeReader();
            SessionAddressResolver resolver(session, nullptr, nullptr, &reader);
            std::uint64_t value = 0xDEADU;
            const bool ok = resolver.ReadPointer(0x1000U, 8U, value);
            std::wostringstream label;
            label << L"session resolver: deref gating scope=" << static_cast<int>(scope)
                  << L" channel=" << static_cast<int>(channel) << L" got ok=" << (ok ? 1 : 0)
                  << L" issue=" << static_cast<int>(resolver.LastIssue()) << L" calls=" << reader.calls
                  << L" value=0x" << std::hex << value;
            if (expectedIssue == Issue::None) {
                suite.expect(ok && value == 0x2000U && resolver.LastIssue() == Issue::None && reader.calls == 1
                        && reader.addresses.size() == 1U && reader.addresses[0] == 0x1000U,
                    label.str().c_str());
            } else {
                // 拒绝路径：返回 false、value 清零、成因相符、detail 为空，且读取器一次都没被调用。
                suite.expect(!ok && value == 0U && resolver.LastIssue() == expectedIssue
                        && resolver.LastDetail().empty() && reader.calls == 0,
                    label.str().c_str());
            }
        }
    }
}

// ------------------------------------------------------------
// 八、解引用的其它失败：没有目标 / 没有读取器 / 宽度 / 读取失败 / 32 位违约。
// ------------------------------------------------------------
void TestDerefFailures(KswordTests::Suite& suite) {
    // 没有目标：先于一切门控，读取器零调用。
    {
        FakePointerReader reader = MakeReader();
        SessionAddressResolver resolver(MemoryTargetSession(), nullptr, nullptr, &reader);
        std::uint64_t value = 0xDEADU;
        suite.expect(!resolver.ReadPointer(0x1000U, 8U, value) && value == 0U
                && resolver.LastIssue() == Issue::NoTarget && reader.calls == 0,
            L"session resolver: no target refuses a dereference without touching the reader");
    }
    // 没有读取器：DerefReadFailed + 内部细节串。
    {
        SessionAddressResolver resolver(ProcessSession(), nullptr, nullptr, nullptr);
        std::uint64_t value = 0xDEADU;
        suite.expect(!resolver.ReadPointer(0x1000U, 8U, value) && value == 0U
                && resolver.LastIssue() == Issue::DerefReadFailed && resolver.LastDetail() == "no-reader",
            L"session resolver: a missing reader is a read failure with the no-reader detail");
    }
    // 宽度必须等于 addressBits/8：64 位用 4、32 位用 8 都不读；匹配的宽度原样传给读取器。
    {
        FakePointerReader reader = MakeReader();
        SessionAddressResolver resolver64(ProcessSession(), nullptr, nullptr, &reader);
        std::uint64_t value = 0xDEADU;
        suite.expect(!resolver64.ReadPointer(0x1000U, 4U, value) && value == 0U
                && resolver64.LastIssue() == Issue::DerefReadFailed && resolver64.LastDetail() == "width-mismatch"
                && reader.calls == 0,
            L"session resolver: width 4 against a 64-bit target is refused before reading");
        suite.expect(!resolver64.ReadPointer(0x1000U, 0U, value) && reader.calls == 0,
            L"session resolver: width 0 is refused before reading");
        suite.expect(resolver64.ReadPointer(0x1000U, 8U, value) && value == 0x2000U && reader.calls == 1
                && reader.widths[0] == 8U,
            L"session resolver: width 8 against a 64-bit target reads with width 8");

        SessionAddressResolver resolver32(ProcessSession(Channel::UserMode, 32U), nullptr, nullptr, &reader);
        suite.expect(!resolver32.ReadPointer(0x1000U, 8U, value) && value == 0U && reader.calls == 1,
            L"session resolver: width 8 against a 32-bit target is refused before reading");
        suite.expect(resolver32.ReadPointer(0x1000U, 4U, value) && value == 0x2000U && reader.calls == 2
                && reader.widths[1] == 4U,
            L"session resolver: width 4 against a 32-bit target reads with width 4");
    }
    // 读取失败：detail 以失败地址开头，读取器给了说明就空格接在后面；value 恒为 0。
    {
        FakePointerReader reader = MakeReader();
        SessionAddressResolver resolver(ProcessSession(), nullptr, nullptr, &reader);
        std::uint64_t value = 0xDEADU;
        suite.expect(!resolver.ReadPointer(0x5000U, 8U, value) && value == 0U
                && resolver.LastIssue() == Issue::DerefReadFailed && resolver.LastDetail() == "0x5000"
                && reader.calls == 1,
            L"session resolver: an unreadable address reports the address only");
        reader.failureText = "STATUS_PARTIAL_COPY";
        suite.expect(!resolver.ReadPointer(0xFFFF800012345678ULL, 8U, value) && value == 0U
                && resolver.LastDetail() == "0xFFFF800012345678 STATUS_PARTIAL_COPY",
            L"session resolver: the reader's note follows the address after a space");
    }
    // 32 位违约：读取器返回高位非零的值，视为失败；恰为 0xFFFFFFFF 的值放行（边界两侧）。
    {
        FakePointerReader reader = MakeReader();
        SessionAddressResolver resolver(ProcessSession(Channel::UserMode, 32U), nullptr, nullptr, &reader);
        std::uint64_t value = 0xDEADU;
        suite.expect(!resolver.ReadPointer(0x6000U, 4U, value) && value == 0U
                && resolver.LastIssue() == Issue::DerefReadFailed
                && resolver.LastDetail() == "0x6000 pointer-too-wide",
            L"session resolver: a 32-bit read returning more than 32 bits is rejected");
        suite.expect(resolver.ReadPointer(0x6008U, 4U, value) && value == 0xFFFFFFFFULL
                && resolver.LastIssue() == Issue::None,
            L"session resolver: a 32-bit read returning exactly 0xFFFFFFFF is accepted");
        // 64 位目标上同样的 0x100000000 完全合法。
        FakePointerReader reader64 = MakeReader();
        SessionAddressResolver resolver64(ProcessSession(), nullptr, nullptr, &reader64);
        suite.expect(resolver64.ReadPointer(0x6000U, 8U, value) && value == 0x100000000ULL,
            L"session resolver: the same value is fine for a 64-bit target");
    }
    // 读取器拿到的是完整会话：范围、pid、创建时间、通道原样传入，不被改写。
    {
        FakePointerReader reader = MakeReader();
        SessionAddressResolver process(ProcessSession(Channel::StandardDriver), nullptr, nullptr, &reader);
        std::uint64_t value = 0U;
        process.ReadPointer(0x1000U, 8U, value);
        SessionAddressResolver kernel(KernelSession(Channel::Hvm), nullptr, nullptr, &reader);
        kernel.ReadPointer(0x1000U, 8U, value);
        suite.expect(reader.sessions.size() == 2U
                && reader.sessions[0].scope == Scope::ProcessVirtual && reader.sessions[0].pid == kPidA
                && reader.sessions[0].processCreateTime100ns == kCreateA
                && reader.sessions[0].channel == Channel::StandardDriver
                && reader.sessions[1].scope == Scope::KernelVirtual && reader.sessions[1].pid == 0U
                && reader.sessions[1].channel == Channel::Hvm,
            L"session resolver: the reader receives the unmodified session, never a substituted channel");
    }
}

} // namespace

int RunMemwbSessionResolverTests() {
    KswordTests::Suite suite(L"MEMWB session resolver");
    TestNoTargetAndPhysical(suite);
    TestProcessDirectoryStates(suite);
    TestProcessLookupResults(suite);
    TestNoKernelFallback(suite);
    TestKernelScopeLookup(suite);
    TestOutcomeReset(suite);
    TestDerefGatingMatrix(suite);
    TestDerefFailures(suite);
    memwb_wpb_test::RunMemwbSessionResolverEvalChecks(suite);
    suite.report();
    return suite.failures();
}
