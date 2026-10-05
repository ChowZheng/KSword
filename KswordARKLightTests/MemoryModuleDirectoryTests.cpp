// 内存工作台模块目录（shared/evidence/memory_workbench/MemoryModuleDirectory.h）的离线测试。
//
// 为什么这个目录值得一整套断言：它属于**算错了不会报错**的那一类。
// 旧代码只有一份模块缓存，却被"预览进程"和"已附加进程"共用：预览进程 B 时缓存被换成
// B 的模块，随后在已附加的进程 A 上解析 client.dll+X 用的却是 B 的基址。读取照样成功，
// 只是读到了别的进程。新目录把"这份快照属于谁"写进数据，下面的回归测试（所有者不符
// 恒 OwnerMismatch）正是为这个旧缺陷钉的。
//
// 断言原则与 MemoryAddressExprTests.cpp 一致：
//   * 期望值手算写死，绝不从被测函数反算；
//   * 边界两侧都测（所有者的创建时间差 1、票据差 1、尾部匹配的分量边界两侧、
//     ASCII 折叠的字母边界两侧）；
//   * 拒绝路径显式测，并同时核对"目录原样不变"；
//   * 失败/非命中时 base 恒为 0、matches 与结果种类相符。

#include "ModuleResolverTestSupport.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace memwb_wpb_test;

using Kind = MemoryModuleDirectory::Lookup::Kind;
using Lookup = MemoryModuleDirectory::Lookup;
using State = MemoryModuleDirectory::State;
using ksword::memwb::IsSameModuleOwner;
using ksword::memwb::ModuleOwnerForSession;

// 标签：带上被测输入与实际结果，失败时一眼看出哪条、得到了什么。
std::wstring LookupLabel(const wchar_t* what, const std::string& name, const Lookup& lookup) {
    std::wostringstream stream;
    stream << L"module dir: " << what << L" [" << Widen(name) << L"] got kind="
           << static_cast<int>(lookup.kind) << L" base=0x" << std::hex << lookup.base
           << std::dec << L" matches=" << lookup.matches.size();
    return stream.str();
}

// 命中且唯一：kind==Found、base 相等、matches 恰一条且其 base 与之相符。
void ExpectFound(
    KswordTests::Suite& suite,
    const wchar_t* what,
    const MemoryModuleDirectory& directory,
    const ModuleOwner& wanted,
    const std::string& name,
    const std::uint64_t base) {
    const Lookup lookup = directory.Find(wanted, name);
    const bool good = lookup.kind == Kind::Found && lookup.base == base
        && lookup.matches.size() == 1U && lookup.matches.front()->base == base;
    suite.expect(good, LookupLabel(what, name, lookup).c_str());
}

// 非命中种类：kind 相符、base 为 0；Ambiguous 要求 matches 个数恰为 expectedMatches，
// 其余种类 matches 必须为空。
void ExpectKind(
    KswordTests::Suite& suite,
    const wchar_t* what,
    const MemoryModuleDirectory& directory,
    const ModuleOwner& wanted,
    const std::string& name,
    const Kind kind,
    const std::size_t expectedMatches = 0U) {
    const Lookup lookup = directory.Find(wanted, name);
    const bool good = lookup.kind == kind && lookup.base == 0U
        && lookup.matches.size() == expectedMatches;
    suite.expect(good, LookupLabel(what, name, lookup).c_str());
}

// ------------------------------------------------------------
// 一、所有者比较：scope/pid 必须相等；创建时间双方都已知时也必须相等。
// ------------------------------------------------------------
void TestOwnerComparison(KswordTests::Suite& suite) {
    struct Case {
        ModuleOwner a;
        ModuleOwner b;
        bool expected;
        const wchar_t* label;
    };
    const Case cases[] = {
        { OwnerA(), OwnerA(), true, L"module dir: identical owners are the same" },
        { OwnerA(), MakeOwner(Scope::ProcessVirtual, kPidA, kCreateB), false,
          L"module dir: same pid with two known, different create times is a different instance" },
        // 边界：创建时间只差 1 也是不同实例，差 0 是同一个。
        { OwnerA(), MakeOwner(Scope::ProcessVirtual, kPidA, kCreateA + 1U), false,
          L"module dir: a create-time difference of one tick is a different instance" },
        { OwnerA(), MakeOwner(Scope::ProcessVirtual, kPidA, kCreateA), true,
          L"module dir: equal create times are the same instance" },
        { OwnerA(), MakeOwner(Scope::ProcessVirtual, kPidA, 0U), true,
          L"module dir: an unknown create time on the right falls back to pid" },
        { MakeOwner(Scope::ProcessVirtual, kPidA, 0U), OwnerA(), true,
          L"module dir: an unknown create time on the left falls back to pid" },
        { MakeOwner(Scope::ProcessVirtual, kPidA, 0U), MakeOwner(Scope::ProcessVirtual, kPidA, 0U), true,
          L"module dir: two unknown create times with the same pid are the same" },
        { OwnerA(), OwnerB(), false, L"module dir: different pid and create time" },
        { OwnerA(), MakeOwner(Scope::ProcessVirtual, kPidB, kCreateA), false,
          L"module dir: different pid with the same create time is still different" },
        { MakeOwner(Scope::ProcessVirtual, kPidA, 0U), MakeOwner(Scope::ProcessVirtual, kPidB, 0U), false,
          L"module dir: unknown create times never make different pids equal" },
        { MakeOwner(Scope::ProcessVirtual, 0U, 0U), KernelOwner(), false,
          L"module dir: process pid 0 is not the kernel owner (scope differs)" },
        { KernelOwner(), KernelOwner(), true, L"module dir: kernel owner equals itself" },
        { MakeOwner(Scope::ProcessVirtual, kPidA, 0U), MakeOwner(Scope::Physical, kPidA, 0U), false,
          L"module dir: scope differs with the same pid" },
    };
    for (const Case& test : cases) {
        suite.expect(IsSameModuleOwner(test.a, test.b) == test.expected, test.label);
        // 比较是对称的。
        suite.expect(IsSameModuleOwner(test.b, test.a) == test.expected,
            L"module dir: owner comparison is symmetric");
    }

    // operator== 是逐字段严格相等：未知创建时间不当通配符。
    suite.expect(OwnerA() == OwnerA(), L"module dir: strict equality of identical owners");
    suite.expect(!(OwnerA() == MakeOwner(Scope::ProcessVirtual, kPidA, 0U)),
        L"module dir: strict equality does not treat an unknown create time as a wildcard");
    suite.expect(!(OwnerA() == MakeOwner(Scope::KernelVirtual, kPidA, kCreateA)),
        L"module dir: strict equality compares scope");
}

// ------------------------------------------------------------
// 二、由会话推所有者：加载方与查询方口径一致。
// ------------------------------------------------------------
void TestOwnerForSession(KswordTests::Suite& suite) {
    suite.expect(ModuleOwnerForSession(ProcessSession()) == OwnerA(),
        L"module dir: a process session maps to (process, pid, create time)");

    // pid 为 0 的进程会话照实映射，不被悄悄规范化成内核。
    MemoryTargetSession noPid = ProcessSession();
    noPid.pid = 0U;
    noPid.processCreateTime100ns = 0U;
    suite.expect(ModuleOwnerForSession(noPid) == MakeOwner(Scope::ProcessVirtual, 0U, 0U),
        L"module dir: a process session without a pid keeps scope process");

    // 内核会话即使带着残留的 pid/创建时间，也要归零。
    MemoryTargetSession dirtyKernel = KernelSession();
    dirtyKernel.pid = 77U;
    dirtyKernel.processCreateTime100ns = 88U;
    suite.expect(ModuleOwnerForSession(dirtyKernel) == KernelOwner(),
        L"module dir: a kernel session maps to (kernel, 0, 0) regardless of stale fields");

    suite.expect(ModuleOwnerForSession(PhysicalSession()) == MakeOwner(Scope::Physical, 0U, 0U),
        L"module dir: a physical session maps to (physical, 0, 0)");

    // 越界枚举值不能冒充进程或内核。
    MemoryTargetSession badScope = ProcessSession();
    badScope.scope = static_cast<Scope>(9U);
    suite.expect(ModuleOwnerForSession(badScope) == MakeOwner(Scope::Physical, 0U, 0U),
        L"module dir: an out-of-range scope never impersonates a process or the kernel");
}

// ------------------------------------------------------------
// 三、状态机：Empty -> Loading -> Ready / Failed -> Clear。
// ------------------------------------------------------------
void TestStateMachine(KswordTests::Suite& suite) {
    MemoryModuleDirectory directory;
    suite.expect(directory.GetState() == State::Empty && directory.Ticket() == 0U
            && directory.Records().empty() && directory.FailureDetail().empty()
            && directory.Owner() == ModuleOwner(),
        L"module dir: a fresh directory is Empty with default owner, ticket 0 and no records");
    // Empty 时没有所有者可言：任何查询都是 NotReady，而不是 OwnerMismatch。
    ExpectKind(suite, L"empty directory, process owner", directory, OwnerA(), "client.dll", Kind::NotReady);
    ExpectKind(suite, L"empty directory, kernel owner", directory, KernelOwner(), "ntoskrnl.exe", Kind::NotReady);

    // 一个默认构造的 Lookup 不会被当成"找到"。
    const Lookup untouched;
    suite.expect(untouched.kind == Kind::NotReady && untouched.base == 0U && untouched.matches.empty(),
        L"module dir: a default Lookup is NotReady with base 0 and no matches");

    // BeginLoad：Loading，登记所有者与票据，没有记录。
    directory.BeginLoad(OwnerA(), 7U);
    suite.expect(directory.GetState() == State::Loading && directory.Owner() == OwnerA()
            && directory.Ticket() == 7U && directory.Records().empty(),
        L"module dir: BeginLoad registers owner and ticket and enters Loading");
    ExpectKind(suite, L"loading, same owner", directory, OwnerA(), "client.dll", Kind::NotReady);
    // Loading 期间属于别人的查询是 OwnerMismatch，不是 NotReady。
    ExpectKind(suite, L"loading, other process", directory, OwnerB(), "client.dll", Kind::OwnerMismatch);
    ExpectKind(suite, L"loading, kernel owner", directory, KernelOwner(), "client.dll", Kind::OwnerMismatch);

    // Commit：Ready。手算记录数：client 1 + ntdll 2 + user32 1 + shared 1 + top 1 + 游戏 1 + many 7 = 14。
    suite.expect(directory.Commit(OwnerA(), 7U, ProcessRecordsA()), L"module dir: matching Commit is accepted");
    suite.expect(directory.GetState() == State::Ready && directory.Records().size() == 14U
            && directory.FailureDetail().empty(),
        L"module dir: after Commit the directory is Ready with all 14 records");
    ExpectFound(suite, L"ready, same owner", directory, OwnerA(), "client.dll", kClientBaseA);

    // Clear：回到 Empty，所有者、票据、记录全部清空。
    directory.Clear();
    suite.expect(directory.GetState() == State::Empty && directory.Ticket() == 0U
            && directory.Records().empty() && directory.Owner() == ModuleOwner(),
        L"module dir: Clear returns to Empty and forgets owner, ticket and records");
    ExpectKind(suite, L"cleared directory", directory, OwnerA(), "client.dll", Kind::NotReady);
}

// ------------------------------------------------------------
// 四、Commit / Fail 的守卫：owner、ticket、状态不符一律丢弃且目录原样不变。
// ------------------------------------------------------------
void TestCommitAndFailGuards(KswordTests::Suite& suite) {
    // 登记 (A, ticket 5) 之后，下面每一种"差一点"的提交都必须被拒绝。
    struct Case {
        ModuleOwner owner;
        std::uint64_t ticket;
        const wchar_t* label;
    };
    const Case wrong[] = {
        { OwnerA(), 4U, L"module dir: Commit with ticket one lower is rejected" },
        { OwnerA(), 6U, L"module dir: Commit with ticket one higher is rejected" },
        { OwnerA(), 0U, L"module dir: Commit with ticket 0 is rejected" },
        { OwnerA(), (std::numeric_limits<std::uint64_t>::max)(), L"module dir: Commit with the maximum ticket is rejected" },
        { OwnerB(), 5U, L"module dir: Commit for another process is rejected" },
        { MakeOwner(Scope::ProcessVirtual, kPidA, kCreateA + 1U), 5U,
          L"module dir: Commit for the same pid but a different create time is rejected" },
        { MakeOwner(Scope::ProcessVirtual, kPidA, 0U), 5U,
          L"module dir: Commit requires the owner exactly as registered (unknown create time is not a wildcard)" },
        { MakeOwner(Scope::KernelVirtual, kPidA, kCreateA), 5U,
          L"module dir: Commit for the same fields under another scope is rejected" },
        { KernelOwner(), 5U, L"module dir: Commit for the kernel owner is rejected" },
    };

    MemoryModuleDirectory directory;
    directory.BeginLoad(OwnerA(), 5U);
    for (const Case& test : wrong) {
        std::vector<ModuleRecord> records = ProcessRecordsA();
        const bool accepted = directory.Commit(test.owner, test.ticket, std::move(records));
        suite.expect(!accepted, test.label);
        // 被拒绝时目录原样不变，传入的记录也没有被移走。
        suite.expect(directory.GetState() == State::Loading && directory.Ticket() == 5U
                && directory.Owner() == OwnerA() && directory.Records().empty(),
            L"module dir: a rejected Commit leaves the directory untouched");
        // 被拒绝时右值参数不能被移走：调用方还要靠它重试/丢弃。
        suite.expect(records.size() == 14U, L"module dir: a rejected Commit does not consume the records");
        // Fail 同理：相同的"差一点"也必须被拒绝，目录保持 Loading。
        suite.expect(!directory.Fail(test.owner, test.ticket, "stale"),
            L"module dir: Fail with a wrong owner or ticket is rejected");
        suite.expect(directory.GetState() == State::Loading && directory.FailureDetail().empty(),
            L"module dir: a rejected Fail leaves the directory Loading with no failure detail");
    }

    // 拒绝了一堆陈旧回调之后，正确的那一次仍然能提交。
    suite.expect(directory.Commit(OwnerA(), 5U, ProcessRecordsA()),
        L"module dir: the correct Commit still succeeds after stale ones were rejected");
    suite.expect(directory.GetState() == State::Ready, L"module dir: directory is Ready after the correct Commit");

    // 同一票据的重复提交：已经 Ready，不是 Loading，必须拒绝，记录不变。
    suite.expect(!directory.Commit(OwnerA(), 5U, ProcessRecordsB()),
        L"module dir: a second Commit with the same ticket is rejected");
    suite.expect(directory.Records().size() == 14U, L"module dir: a rejected second Commit does not replace the records");
    suite.expect(!directory.Fail(OwnerA(), 5U, "late"), L"module dir: Fail after Ready is rejected");
    suite.expect(directory.GetState() == State::Ready && directory.Records().size() == 14U,
        L"module dir: a rejected Fail after Ready keeps the directory Ready");

    // 没有 BeginLoad 就提交：Empty 不接受。
    MemoryModuleDirectory empty;
    suite.expect(!empty.Commit(OwnerA(), 0U, ProcessRecordsA()), L"module dir: Commit on an Empty directory is rejected");
    suite.expect(!empty.Fail(OwnerA(), 0U, "x"), L"module dir: Fail on an Empty directory is rejected");
    suite.expect(empty.GetState() == State::Empty && empty.Records().empty(),
        L"module dir: rejected Commit/Fail leave an Empty directory Empty");

    // 票据取值边界：0 与最大值都是合法票据，往返成功。
    MemoryModuleDirectory edge;
    suite.expect(LoadDirectory(edge, OwnerA(), 0U, ProcessRecordsA()), L"module dir: ticket 0 round-trips");
    suite.expect(LoadDirectory(edge, OwnerB(), (std::numeric_limits<std::uint64_t>::max)(), ProcessRecordsB()),
        L"module dir: the maximum ticket round-trips");
}

// ------------------------------------------------------------
// 五、Fail：失败状态、失败说明、之后的行为。
// ------------------------------------------------------------
void TestFailure(KswordTests::Suite& suite) {
    MemoryModuleDirectory directory;
    directory.BeginLoad(OwnerA(), 9U);
    suite.expect(directory.Fail(OwnerA(), 9U, "enum-failed"), L"module dir: matching Fail is accepted");
    suite.expect(directory.GetState() == State::Failed && directory.FailureDetail() == "enum-failed"
            && directory.Records().empty(),
        L"module dir: after Fail the directory is Failed with the detail and no records");
    ExpectKind(suite, L"failed, same owner", directory, OwnerA(), "client.dll", Kind::NotReady);
    // 失败的目录属于谁依然记着：别人来查是 OwnerMismatch。
    ExpectKind(suite, L"failed, other process", directory, OwnerB(), "client.dll", Kind::OwnerMismatch);

    // 已经 Failed：Commit/Fail 都不再接受（要先 BeginLoad 开新一轮）。
    suite.expect(!directory.Commit(OwnerA(), 9U, ProcessRecordsA()), L"module dir: Commit after Fail is rejected");
    suite.expect(!directory.Fail(OwnerA(), 9U, "again"), L"module dir: a second Fail is rejected");
    suite.expect(directory.GetState() == State::Failed && directory.FailureDetail() == "enum-failed",
        L"module dir: rejected calls after Fail keep the first failure detail");

    // 新一轮加载清掉失败说明。
    directory.BeginLoad(OwnerA(), 10U);
    suite.expect(directory.GetState() == State::Loading && directory.FailureDetail().empty(),
        L"module dir: BeginLoad after Failed clears the failure detail");
    suite.expect(directory.Commit(OwnerA(), 10U, ProcessRecordsA()) && directory.FailureDetail().empty(),
        L"module dir: the retry can succeed and leaves no failure detail behind");

    // Clear 也清失败说明。
    MemoryModuleDirectory cleared;
    cleared.BeginLoad(OwnerA(), 1U);
    cleared.Fail(OwnerA(), 1U, "x");
    cleared.Clear();
    suite.expect(cleared.GetState() == State::Empty && cleared.FailureDetail().empty(),
        L"module dir: Clear forgets the failure detail");
}

// ------------------------------------------------------------
// 六、重新加载：旧记录立即清空，旧一轮的在途结果作废。
// ------------------------------------------------------------
void TestReload(KswordTests::Suite& suite) {
    // 换所有者：Ready(A) 之后 BeginLoad(B)。
    MemoryModuleDirectory directory;
    LoadDirectory(directory, OwnerA(), 1U, ProcessRecordsA());
    directory.BeginLoad(OwnerB(), 2U);
    suite.expect(directory.GetState() == State::Loading && directory.Records().empty(),
        L"module dir: BeginLoad for a new owner drops the old records immediately");
    // A 的旧记录不能被任何人读到：A 来查是 OwnerMismatch，B 来查是 NotReady。
    ExpectKind(suite, L"reload, old owner asks", directory, OwnerA(), "client.dll", Kind::OwnerMismatch);
    ExpectKind(suite, L"reload, new owner asks", directory, OwnerB(), "client.dll", Kind::NotReady);
    // 第一轮（A，票据 1）的在途结果这时才回来：必须被丢弃。
    suite.expect(!directory.Commit(OwnerA(), 1U, ProcessRecordsA()),
        L"module dir: the in-flight result of the previous load is discarded");
    suite.expect(directory.GetState() == State::Loading, L"module dir: the stale result did not end the new load");
    suite.expect(directory.Commit(OwnerB(), 2U, ProcessRecordsB()), L"module dir: the new load commits");
    ExpectFound(suite, L"after owner switch", directory, OwnerB(), "client.dll", kClientBaseB);
    ExpectKind(suite, L"after owner switch, old owner", directory, OwnerA(), "client.dll", Kind::OwnerMismatch);

    // 同一所有者刷新：旧记录同样立即清空，旧票据的结果作废。
    MemoryModuleDirectory refresh;
    LoadDirectory(refresh, OwnerA(), 1U, ProcessRecordsA());
    refresh.BeginLoad(OwnerA(), 2U);
    ExpectKind(suite, L"refresh in progress", refresh, OwnerA(), "client.dll", Kind::NotReady);
    suite.expect(!refresh.Commit(OwnerA(), 1U, ProcessRecordsA()),
        L"module dir: a result with the previous ticket is stale after a refresh began");
    suite.expect(refresh.Commit(OwnerA(), 2U, ProcessRecordsA()), L"module dir: the refreshed load commits");
    ExpectFound(suite, L"after refresh", refresh, OwnerA(), "client.dll", kClientBaseA);
}

// ------------------------------------------------------------
// 七、进程目录的名字匹配：文件名 / 完整路径 / 大小写 / 精确度。
// ------------------------------------------------------------
void TestProcessNameMatching(KswordTests::Suite& suite) {
    const Fixture fixture;
    const MemoryModuleDirectory& dir = fixture.process;

    // 文件名匹配，大小写不敏感。
    ExpectFound(suite, L"file name", dir, OwnerA(), "client.dll", kClientBaseA);
    ExpectFound(suite, L"file name upper", dir, OwnerA(), "CLIENT.DLL", kClientBaseA);
    ExpectFound(suite, L"file name mixed", dir, OwnerA(), "Client.Dll", kClientBaseA);
    ExpectFound(suite, L"another module", dir, OwnerA(), "user32.dll", kUser32Base);

    // 文件名必须整串相等：前缀、后缀、首尾空白都不算命中（目录不剪空白，那是上游的事）。
    ExpectKind(suite, L"prefix of a name", dir, OwnerA(), "client", Kind::NotFound);
    ExpectKind(suite, L"suffix of a name", dir, OwnerA(), "lient.dll", Kind::NotFound);
    ExpectKind(suite, L"truncated name", dir, OwnerA(), "client.dl", Kind::NotFound);
    ExpectKind(suite, L"longer name", dir, OwnerA(), "client.dll.bak", Kind::NotFound);
    ExpectKind(suite, L"trailing space", dir, OwnerA(), "client.dll ", Kind::NotFound);
    ExpectKind(suite, L"leading space", dir, OwnerA(), " client.dll", Kind::NotFound);
    ExpectKind(suite, L"missing module", dir, OwnerA(), "nothere.dll", Kind::NotFound);

    // 完整路径匹配：整条路径相等（大小写不敏感，两种分隔符等价）。
    ExpectFound(suite, L"full path", dir, OwnerA(), "C:\\Game\\client.dll", kClientBaseA);
    ExpectFound(suite, L"full path folded", dir, OwnerA(), "c:\\game\\CLIENT.dll", kClientBaseA);
    ExpectFound(suite, L"full path with slashes", dir, OwnerA(), "C:/Game/client.dll", kClientBaseA);
    // 进程目录**不**做尾部匹配：只有内核路径才有简写习惯。
    ExpectKind(suite, L"process path tail", dir, OwnerA(), "Game\\client.dll", Kind::NotFound);
    ExpectKind(suite, L"process path tail with leading slash", dir, OwnerA(), "\\Game\\client.dll", Kind::NotFound);
    ExpectKind(suite, L"process path truncated", dir, OwnerA(), "C:\\Game\\client.dl", Kind::NotFound);
    ExpectKind(suite, L"same file name in another directory", dir, OwnerA(), "C:\\Other\\client.dll", Kind::NotFound);
    ExpectKind(suite, L"process path system32 tail", dir, OwnerA(), "System32\\ntdll.dll", Kind::NotFound);

    // 命中的指针确实指向目录里那条记录，而不是副本。
    const Lookup found = dir.Find(OwnerA(), "client.dll");
    suite.expect(found.matches.size() == 1U && found.matches.front() == &dir.Records().front(),
        L"module dir: the match points into the directory's own record storage");
    suite.expect(found.matches.front()->fullPath == "C:\\Game\\client.dll"
            && found.matches.front()->size == 0x2000000ULL,
        L"module dir: the matched record carries its full path and size");

    // 基址为 0 的模块是合法命中：靠 kind 区分"找到了基址 0"与"没找到"。
    MemoryModuleDirectory zero;
    LoadDirectory(zero, OwnerA(), 1U, { MakeRecord("zero.dll", "C:\\zero.dll", 0U) });
    ExpectFound(suite, L"module based at zero", zero, OwnerA(), "zero.dll", 0U);
}

// ------------------------------------------------------------
// 八、大小写折叠只限 ASCII：非 ASCII 字节原样逐字节比较。
// ------------------------------------------------------------
void TestAsciiOnlyFolding(KswordTests::Suite& suite) {
    MemoryModuleDirectory dir;
    LoadDirectory(dir, OwnerA(), 1U, {
        MakeRecord("a@b.dll", "C:\\t\\a@b.dll", 1U),
        MakeRecord("a[b.dll", "C:\\t\\a[b.dll", 2U),
        // UTF-8 的 É（C3 89）。
        MakeRecord("\xC3\x89.dll", "C:\\t\\\xC3\x89.dll", 3U),
        // 单字节 0xC0：与 0xE0 只差 0x20，locale 感知的 tolower 在某些代码页会把它们折叠。
        MakeRecord("\xC0.dll", "C:\\t\\\xC0.dll", 4U),
        MakeRecord("A.dll", "C:\\t\\A.dll", 5U),
        MakeRecord("Z.dll", "C:\\t\\Z.dll", 6U),
    });

    // 字母边界两侧：A 与 Z 本身折叠，@ 与 [ 不是字母，不参与折叠。
    ExpectFound(suite, L"lowercase a vs uppercase A", dir, OwnerA(), "a.dll", 5U);
    ExpectFound(suite, L"lowercase z vs uppercase Z", dir, OwnerA(), "z.dll", 6U);
    ExpectFound(suite, L"letters fold around a non-letter", dir, OwnerA(), "A@B.DLL", 1U);
    ExpectFound(suite, L"bracket matches itself", dir, OwnerA(), "A[B.dll", 2U);
    // 与 '@'(0x40)/'['(0x5B) 恰差 0x20 的 '`'(0x60)/'{'(0x7B) 必须不等：
    // 朴素的 "|= 0x20" 折叠会把它们当成相等。
    ExpectKind(suite, L"backtick is not at", dir, OwnerA(), "a`b.dll", Kind::NotFound);
    ExpectKind(suite, L"brace is not bracket", dir, OwnerA(), "a{b.dll", Kind::NotFound);

    // 非 ASCII：É(C3 89) 与 é(C3 A9) 不相等；0xC0 与 0xE0 不相等；原字节本身相等。
    ExpectFound(suite, L"identical UTF-8 bytes", dir, OwnerA(), "\xC3\x89.dll", 3U);
    ExpectKind(suite, L"lowercase UTF-8 accent", dir, OwnerA(), "\xC3\xA9.dll", Kind::NotFound);
    ExpectFound(suite, L"identical high byte", dir, OwnerA(), "\xC0.dll", 4U);
    ExpectKind(suite, L"high byte differing by 0x20", dir, OwnerA(), "\xE0.dll", Kind::NotFound);

    // 夹具里的 UTF-8 "游戏.dll"：ASCII 部分（扩展名）折叠，汉字字节原样相等。
    const Fixture fixture;
    const std::string game = kUtf8GameDll;
    ExpectFound(suite, L"UTF-8 name", fixture.process, OwnerA(), game, 0x7FF900000000ULL);
    const std::string gameUpper = std::string("\xE6\xB8\xB8\xE6\x88\x8F") + ".DLL";
    ExpectFound(suite, L"UTF-8 name with upper-case extension", fixture.process, OwnerA(), gameUpper,
        0x7FF900000000ULL);
}

// ------------------------------------------------------------
// 九、内核目录的尾部匹配：必须落在路径分量边界上。
// ------------------------------------------------------------
void TestKernelTailMatching(KswordTests::Suite& suite) {
    const Fixture fixture;
    const MemoryModuleDirectory& dir = fixture.kernel;
    const ModuleOwner kernel = KernelOwner();

    // 文件名匹配照常。
    ExpectFound(suite, L"kernel file name", dir, kernel, "ci.dll", kCiBase);
    ExpectFound(suite, L"kernel file name upper", dir, kernel, "CI.DLL", kCiBase);
    ExpectFound(suite, L"kernel exe by name", dir, kernel, "ntoskrnl.exe", kNtoskrnlBase);

    // 路径：整串相等、去掉开头若干分量的尾部、带不带开头分隔符、斜杠写法。
    ExpectFound(suite, L"kernel exact path", dir, kernel, "\\SystemRoot\\system32\\CI.dll", kCiBase);
    ExpectFound(suite, L"kernel exact path folded", dir, kernel, "\\systemroot\\SYSTEM32\\ci.dll", kCiBase);
    ExpectFound(suite, L"kernel tail", dir, kernel, "system32\\ci.dll", kCiBase);
    ExpectFound(suite, L"kernel tail with leading separator", dir, kernel, "\\system32\\ci.dll", kCiBase);
    ExpectFound(suite, L"kernel tail from the root component", dir, kernel, "SystemRoot\\system32\\CI.dll", kCiBase);
    ExpectFound(suite, L"kernel tail with slashes", dir, kernel, "system32/ci.dll", kCiBase);

    // 分量边界两侧：多一个字母的半截分量不能命中。
    ExpectKind(suite, L"kernel tail starting mid-component", dir, kernel, "ystem32\\ci.dll", Kind::NotFound);
    ExpectKind(suite, L"kernel tail starting mid root component", dir, kernel,
        "stemRoot\\system32\\CI.dll", Kind::NotFound);
    ExpectKind(suite, L"kernel path with a different directory", dir, kernel, "drivers\\ci.dll", Kind::NotFound);
    ExpectKind(suite, L"kernel path trailing separator", dir, kernel, "system32\\", Kind::NotFound);
    ExpectKind(suite, L"kernel lone separator", dir, kernel, "\\", Kind::NotFound);

    // 内核里常见的 \??\C:\... 形式：用户写 Win32 路径也能命中（后缀从 'C:' 起，前一字节是分隔符）。
    ExpectFound(suite, L"win32 spelling of an NT path", dir, kernel,
        "C:\\Windows\\system32\\drivers\\foo.sys", kFooBase);
    ExpectFound(suite, L"NT path exact", dir, kernel, "\\??\\C:\\Windows\\system32\\drivers\\foo.sys", kFooBase);
    ExpectFound(suite, L"tail of an NT path", dir, kernel, "drivers\\foo.sys", kFooBase);
    ExpectFound(suite, L"deeper tail of an NT path", dir, kernel, "Windows\\system32\\drivers\\foo.sys", kFooBase);
    ExpectKind(suite, L"NT path tail mid-component", dir, kernel, "indows\\system32\\drivers\\foo.sys",
        Kind::NotFound);
    ExpectKind(suite, L"path longer than the record path", dir, kernel,
        "X\\??\\C:\\Windows\\system32\\drivers\\foo.sys", Kind::NotFound);
    ExpectKind(suite, L"tail with trailing separator", dir, kernel,
        "C:\\Windows\\system32\\drivers\\foo.sys\\", Kind::NotFound);
    ExpectFound(suite, L"kernel foo by name", dir, kernel, "FOO.SYS", kFooBase);

    // 尾部匹配只属于内核目录：进程目录要求整条路径相等（已在进程用例里钉过，这里再对照一次）。
    ExpectKind(suite, L"process directory does not tail-match", fixture.process, OwnerA(),
        "Windows\\System32\\ntdll.dll", Kind::NotFound);

    // 内核重名：按名字二义，按路径尾部可以消歧；只有尾部也二义时才继续 Ambiguous。
    const Lookup dup = dir.Find(kernel, "dup.sys");
    suite.expect(dup.kind == Kind::Ambiguous && dup.base == 0U && dup.matches.size() == 2U
            && dup.matches[0]->base == kDupDriversBase && dup.matches[1]->base == kDupSystem32Base,
        L"module dir: a duplicated kernel name is ambiguous and lists both in directory order");
    ExpectFound(suite, L"kernel duplicate disambiguated by drivers tail", dir, kernel, "drivers\\dup.sys",
        kDupDriversBase);
    ExpectFound(suite, L"kernel duplicate disambiguated by system32 tail", dir, kernel, "system32\\dup.sys",
        kDupSystem32Base);
    ExpectKind(suite, L"kernel duplicate with a tail both share", dir, kernel, "\\dup.sys", Kind::Ambiguous, 2U);
    ExpectFound(suite, L"kernel deeper drivers path", dir, kernel, "\\system32\\drivers\\shared.dll",
        kSharedBaseKernel);
}

// ------------------------------------------------------------
// 十、重名（含 WOW64 式 ntdll 重名）。
// ------------------------------------------------------------
void TestAmbiguity(KswordTests::Suite& suite) {
    const Fixture fixture;
    const MemoryModuleDirectory& dir = fixture.process;

    // WOW64 进程里 ntdll.dll 必然有两份：按名字是 Ambiguous，两条都列出，顺序即目录顺序。
    const Lookup ntdll = dir.Find(OwnerA(), "ntdll.dll");
    suite.expect(ntdll.kind == Kind::Ambiguous && ntdll.base == 0U && ntdll.matches.size() == 2U
            && ntdll.matches[0]->fullPath == "C:\\Windows\\System32\\ntdll.dll"
            && ntdll.matches[1]->fullPath == "C:\\Windows\\SysWOW64\\ntdll.dll",
        L"module dir: WOW64-style duplicate ntdll.dll is ambiguous with both paths listed in order");
    ExpectKind(suite, L"ambiguity ignores case", dir, OwnerA(), "NTDLL.DLL", Kind::Ambiguous, 2U);
    // 给完整路径即可消歧，各取各的基址。
    ExpectFound(suite, L"native ntdll by path", dir, OwnerA(), "C:\\Windows\\System32\\ntdll.dll", kNtdllNativeBase);
    ExpectFound(suite, L"wow ntdll by path", dir, OwnerA(), "c:\\windows\\syswow64\\NTDLL.DLL", kNtdllWowBase);

    // 七份 many.dll：全部列在 matches 里（目录不截断，"前 5 条"是解析器的事）。
    const Lookup many = dir.Find(OwnerA(), "many.dll");
    suite.expect(many.kind == Kind::Ambiguous && many.matches.size() == 7U
            && many.matches.front()->fullPath == "C:\\Dir1\\many.dll"
            && many.matches.back()->fullPath == "C:\\Dir7\\many.dll",
        L"module dir: seven same-named modules are all listed in directory order");
    ExpectFound(suite, L"one of seven by path", dir, OwnerA(), "C:\\Dir4\\many.dll", 0x40000000ULL);
}

// ------------------------------------------------------------
// 十一、旧缺陷回归：预览进程 B 的目录对会话 A 恒 OwnerMismatch。
// ------------------------------------------------------------
void TestOwnerMismatchRegression(KswordTests::Suite& suite) {
    // 用户在下拉框里预览进程 B：目录被换成 B 的模块。
    MemoryModuleDirectory preview;
    LoadDirectory(preview, OwnerB(), 1U, ProcessRecordsB());

    // 会话 A 去查：无论是两个进程共有的 client.dll、只有 B 有的 previewonly.dll，
    // 还是 A 才有的 ntdll.dll，一律 OwnerMismatch，base 恒 0——绝不是 B 的基址。
    ExpectKind(suite, L"session A asks for a name both processes have", preview, OwnerA(), "client.dll",
        Kind::OwnerMismatch);
    ExpectKind(suite, L"session A asks for a name only B has", preview, OwnerA(), "previewonly.dll",
        Kind::OwnerMismatch);
    ExpectKind(suite, L"session A asks for a name only A has", preview, OwnerA(), "ntdll.dll",
        Kind::OwnerMismatch);
    ExpectKind(suite, L"session A asks by full path", preview, OwnerA(), "D:\\Other\\client.dll",
        Kind::OwnerMismatch);
    // B 自己查则正常。
    ExpectFound(suite, L"session B asks", preview, OwnerB(), "client.dll", kClientBaseB);

    // pid 被复用：同 pid、双方创建时间都已知且不同 -> 是另一个进程实例。
    ExpectKind(suite, L"recycled pid", preview, MakeOwner(Scope::ProcessVirtual, kPidB, kCreateB + 1U),
        "client.dll", Kind::OwnerMismatch);
    // 身份未锚定（创建时间未知）：只比 pid，仍可用。
    ExpectFound(suite, L"unanchored identity falls back to pid", preview,
        MakeOwner(Scope::ProcessVirtual, kPidB, 0U), "client.dll", kClientBaseB);
    // 范围不同：同样的 pid 数字出现在内核/物理所有者上也不是同一个所有者。
    ExpectKind(suite, L"same pid under the kernel scope", preview,
        MakeOwner(Scope::KernelVirtual, kPidB, kCreateB), "client.dll", Kind::OwnerMismatch);
    ExpectKind(suite, L"kernel owner asks a process directory", preview, KernelOwner(), "client.dll",
        Kind::OwnerMismatch);

    // 进程目录与内核目录互相不串：pid 0 的进程会话不能读内核目录。
    MemoryModuleDirectory kernelDir;
    LoadDirectory(kernelDir, KernelOwner(), 1U, KernelRecords());
    ExpectKind(suite, L"process pid 0 asks the kernel directory", kernelDir,
        MakeOwner(Scope::ProcessVirtual, 0U, 0U), "ntoskrnl.exe", Kind::OwnerMismatch);
    ExpectFound(suite, L"kernel owner asks the kernel directory", kernelDir, KernelOwner(), "ntoskrnl.exe",
        kNtoskrnlBase);

    // Failed 的目录同样记得自己属于谁：别人来问是 OwnerMismatch，本人来问是 NotReady。
    MemoryModuleDirectory failed;
    failed.BeginLoad(OwnerB(), 1U);
    failed.Fail(OwnerB(), 1U, "x");
    ExpectKind(suite, L"failed directory, other owner", failed, OwnerA(), "client.dll", Kind::OwnerMismatch);
    ExpectKind(suite, L"failed directory, own owner", failed, OwnerB(), "client.dll", Kind::NotReady);
}

// ------------------------------------------------------------
// 十二、边角：空名字、空记录、name/fullPath 缺省的记录。
// ------------------------------------------------------------
void TestEdgeCases(KswordTests::Suite& suite) {
    // 空名字永远 NotFound，哪怕目录里有名字和路径都为空的记录——绝不"匹配全部"。
    MemoryModuleDirectory dir;
    LoadDirectory(dir, OwnerA(), 1U, {
        MakeRecord("", "", 0x11U),
        MakeRecord("", "C:\\X\\foo.dll", 0x22U),
        MakeRecord("alias.dll", "C:\\X\\real.dll", 0x33U),
        MakeRecord("pathless.dll", "", 0x44U),
    });
    ExpectKind(suite, L"empty name", dir, OwnerA(), "", Kind::NotFound);

    // name 为空时按 fullPath 最后一个分量取文件名。
    ExpectFound(suite, L"name derived from the path", dir, OwnerA(), "foo.dll", 0x22U);
    ExpectFound(suite, L"derived name folds case", dir, OwnerA(), "FOO.DLL", 0x22U);
    ExpectFound(suite, L"derived-name record by full path", dir, OwnerA(), "C:\\X\\foo.dll", 0x22U);

    // name 非空时以 name 为准：real.dll 是路径的最后一个分量，但不是它的名字。
    ExpectFound(suite, L"explicit name wins", dir, OwnerA(), "alias.dll", 0x33U);
    ExpectKind(suite, L"path component is not the name", dir, OwnerA(), "real.dll", Kind::NotFound);
    ExpectFound(suite, L"explicit-name record by full path", dir, OwnerA(), "C:\\X\\real.dll", 0x33U);

    // 没有完整路径的记录：按名字能找到，按路径找不到。
    ExpectFound(suite, L"pathless record by name", dir, OwnerA(), "pathless.dll", 0x44U);
    ExpectKind(suite, L"pathless record by path", dir, OwnerA(), "C:\\pathless.dll", Kind::NotFound);

    // Ready 但一条记录都没有：是 NotFound，不是 NotReady——"枚举成功但没有模块"与
    // "还没枚举"必须能区分。
    MemoryModuleDirectory none;
    LoadDirectory(none, OwnerA(), 1U, {});
    suite.expect(none.GetState() == State::Ready && none.Records().empty(),
        L"module dir: committing zero records is Ready with no records");
    ExpectKind(suite, L"ready but empty", none, OwnerA(), "client.dll", Kind::NotFound);
}

} // namespace

int RunMemwbModuleDirTests() {
    KswordTests::Suite suite(L"MEMWB module dir");
    TestOwnerComparison(suite);
    TestOwnerForSession(suite);
    TestStateMachine(suite);
    TestCommitAndFailGuards(suite);
    TestFailure(suite);
    TestReload(suite);
    TestProcessNameMatching(suite);
    TestAsciiOnlyFolding(suite);
    TestKernelTailMatching(suite);
    TestAmbiguity(suite);
    TestOwnerMismatchRegression(suite);
    TestEdgeCases(suite);
    suite.report();
    return suite.failures();
}
