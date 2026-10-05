#pragma once

// 内存工作台"模块目录 / 会话地址解析器"（WP-B）测试的共用夹具。
//
// 为什么拆出来：MemoryModuleDirectoryTests.cpp 与 SessionAddressResolverTests*.cpp
// 共用同一套标准模块清单、会话构造器与假指针读取器。全部是 inline，只被这几份
// 测试文件包含；命名空间 memwb_wpb_test，避免与 memwb_expr_test 等夹具混淆。
//
// 夹具设计要点：
//   * 进程 A（pid 1234，创建时间已知）是"当前会话"，进程 B（pid 2222）是"预览进程"。
//     旧缺陷就是拿 B 的模块缓存去解析 A 的表达式，所以两个进程的清单里故意放了同名
//     模块 client.dll 但基址不同：目录用错所有者时，结果会带出 B 的基址，一眼可见。
//   * 进程 A 的清单里 ntdll.dll 出现两次（System32 与 SysWOW64），模拟 WOW64 进程的
//     必然重名；many.dll 出现七次，用来验证"只列前 5 条路径"。
//   * 内核清单用 NT 路径（\SystemRoot\...、\??\C:\...），验证尾部匹配。
//   * 假读取器记录每一次调用（次数、地址、宽度、会话），用来证明门控下"零次调用"。

#include "TestSupport.h"

#include "../shared/evidence/memory_workbench/MemoryModuleDirectory.h"
#include "../shared/evidence/memory_workbench/SessionAddressResolver.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace memwb_wpb_test {

using ksword::memwb::AddressEval;
using ksword::memwb::Channel;
using ksword::memwb::IPointerReader;
using ksword::memwb::Issue;
using ksword::memwb::MemoryModuleDirectory;
using ksword::memwb::MemoryTargetSession;
using ksword::memwb::ModuleOwner;
using ksword::memwb::ModuleRecord;
using ksword::memwb::Scope;

// 进程 A（当前会话）与进程 B（预览进程）的身份。创建时间取 FILETIME 风格的大数。
inline constexpr std::uint32_t kPidA = 1234U;
inline constexpr std::uint64_t kCreateA = 0x01DB000000005000ULL;
inline constexpr std::uint32_t kPidB = 2222U;
inline constexpr std::uint64_t kCreateB = 0x01DB000000006000ULL;

// 进程 A 清单里的基址，手算写死。
inline constexpr std::uint64_t kClientBaseA = 0x7FF600000000ULL;
inline constexpr std::uint64_t kNtdllNativeBase = 0x7FFA00000000ULL;
inline constexpr std::uint64_t kNtdllWowBase = 0x77000000ULL;
inline constexpr std::uint64_t kUser32Base = 0x7FFB00000000ULL;
inline constexpr std::uint64_t kSharedBaseProcess = 0x7FF700000000ULL;
// 贴着 47 位用户空间上沿的模块：基址加小偏移会越过 0x7FFFFFFFFFFF。
inline constexpr std::uint64_t kTopBase = 0x7FFFFFFFFFF0ULL;
// 进程 B 的 client.dll 基址，与 A 的不同。
inline constexpr std::uint64_t kClientBaseB = 0x7FF800000000ULL;

// 内核清单里的基址。
inline constexpr std::uint64_t kNtoskrnlBase = 0xFFFFF80010000000ULL;
inline constexpr std::uint64_t kCiBase = 0xFFFFF80012340000ULL;
inline constexpr std::uint64_t kSharedBaseKernel = 0xFFFFF80030000000ULL;
inline constexpr std::uint64_t kFooBase = 0xFFFFF80040000000ULL;
inline constexpr std::uint64_t kDupDriversBase = 0xFFFFF80050000000ULL;
inline constexpr std::uint64_t kDupSystem32Base = 0xFFFFF80060000000ULL;

// UTF-8 的"游戏.dll"，用转义字节写，避免源文件编码影响测试。
inline const char* const kUtf8GameDll = "\xE6\xB8\xB8\xE6\x88\x8F.dll";

// ------------------------------------------------------------
// 构造器：记录、所有者、会话。
// ------------------------------------------------------------
inline ModuleRecord MakeRecord(
    const std::string& name,
    const std::string& fullPath,
    const std::uint64_t base,
    const std::uint64_t size = 0x1000ULL) {
    ModuleRecord record;
    record.name = name;
    record.fullPath = fullPath;
    record.base = base;
    record.size = size;
    return record;
}

inline ModuleOwner MakeOwner(
    const Scope scope,
    const std::uint32_t pid,
    const std::uint64_t createTime) {
    ModuleOwner owner;
    owner.scope = scope;
    owner.pid = pid;
    owner.createTime = createTime;
    return owner;
}

// OwnerA：进程 A（当前会话）的所有者。
inline ModuleOwner OwnerA() {
    return MakeOwner(Scope::ProcessVirtual, kPidA, kCreateA);
}

// OwnerB：进程 B（预览进程）的所有者。
inline ModuleOwner OwnerB() {
    return MakeOwner(Scope::ProcessVirtual, kPidB, kCreateB);
}

// KernelOwner：内核目录的所有者 (内核, 0, 0)。
inline ModuleOwner KernelOwner() {
    return MakeOwner(Scope::KernelVirtual, 0U, 0ULL);
}

// 进程 A 的会话。默认 R3 通道、64 位。
inline MemoryTargetSession ProcessSession(
    const Channel channel = Channel::UserMode,
    const std::uint32_t bits = 64U) {
    MemoryTargetSession session;
    session.scope = Scope::ProcessVirtual;
    session.pid = kPidA;
    session.processCreateTime100ns = kCreateA;
    session.attachGeneration = 1U;
    session.channel = channel;
    session.addressBits = bits;
    return session;
}

// 内核会话：pid 与创建时间为 0。
inline MemoryTargetSession KernelSession(const Channel channel = Channel::StandardDriver) {
    MemoryTargetSession session;
    session.scope = Scope::KernelVirtual;
    session.channel = channel;
    session.addressBits = 64U;
    return session;
}

// 物理会话：pid 与创建时间为 0。
inline MemoryTargetSession PhysicalSession(const Channel channel = Channel::StandardDriver) {
    MemoryTargetSession session;
    session.scope = Scope::Physical;
    session.channel = channel;
    session.addressBits = 64U;
    return session;
}

// ------------------------------------------------------------
// 标准清单。
// ------------------------------------------------------------
inline std::vector<ModuleRecord> ProcessRecordsA() {
    std::vector<ModuleRecord> records;
    records.push_back(MakeRecord("client.dll", "C:\\Game\\client.dll", kClientBaseA, 0x2000000ULL));
    records.push_back(MakeRecord("ntdll.dll", "C:\\Windows\\System32\\ntdll.dll", kNtdllNativeBase));
    records.push_back(MakeRecord("ntdll.dll", "C:\\Windows\\SysWOW64\\ntdll.dll", kNtdllWowBase));
    records.push_back(MakeRecord("user32.dll", "C:\\Windows\\System32\\user32.dll", kUser32Base));
    records.push_back(MakeRecord("shared.dll", "C:\\Game\\shared.dll", kSharedBaseProcess));
    records.push_back(MakeRecord("top.dll", "C:\\Game\\top.dll", kTopBase));
    records.push_back(MakeRecord(kUtf8GameDll, std::string("C:\\Game\\") + kUtf8GameDll, 0x7FF900000000ULL));
    // many.dll 七份，基址 0x10000000 * 序号，路径 C:\Dir<序号>\many.dll。
    for (std::uint64_t index = 1; index <= 7U; ++index) {
        records.push_back(MakeRecord(
            "many.dll",
            "C:\\Dir" + std::to_string(index) + "\\many.dll",
            0x10000000ULL * index));
    }
    return records;
}

// 进程 B（预览进程）的清单：client.dll 同名但基址不同，另有一个 A 没有的 previewonly.dll。
inline std::vector<ModuleRecord> ProcessRecordsB() {
    std::vector<ModuleRecord> records;
    records.push_back(MakeRecord("client.dll", "D:\\Other\\client.dll", kClientBaseB));
    records.push_back(MakeRecord("previewonly.dll", "D:\\Other\\previewonly.dll", 0x7FF810000000ULL));
    return records;
}

inline std::vector<ModuleRecord> KernelRecords() {
    std::vector<ModuleRecord> records;
    records.push_back(MakeRecord("ntoskrnl.exe", "\\SystemRoot\\system32\\ntoskrnl.exe", kNtoskrnlBase));
    records.push_back(MakeRecord("CI.dll", "\\SystemRoot\\system32\\CI.dll", kCiBase));
    records.push_back(MakeRecord("shared.dll", "\\SystemRoot\\system32\\drivers\\shared.dll", kSharedBaseKernel));
    records.push_back(MakeRecord("foo.sys", "\\??\\C:\\Windows\\system32\\drivers\\foo.sys", kFooBase));
    records.push_back(MakeRecord("dup.sys", "\\SystemRoot\\system32\\drivers\\dup.sys", kDupDriversBase));
    records.push_back(MakeRecord("dup.sys", "\\SystemRoot\\system32\\dup.sys", kDupSystem32Base));
    return records;
}

// LoadDirectory：BeginLoad + Commit 一步到位，返回 Commit 是否被接受。
inline bool LoadDirectory(
    MemoryModuleDirectory& directory,
    const ModuleOwner& owner,
    const std::uint64_t ticket,
    std::vector<ModuleRecord> records) {
    directory.BeginLoad(owner, ticket);
    return directory.Commit(owner, ticket, std::move(records));
}

// Fixture：进程 A 目录与内核目录都已 Ready 的标准夹具。
// 目录里的记录被 Lookup::matches 以指针引用，所以夹具禁止拷贝。
struct Fixture {
    MemoryModuleDirectory process;
    MemoryModuleDirectory kernel;

    Fixture() {
        LoadDirectory(process, OwnerA(), 1U, ProcessRecordsA());
        LoadDirectory(kernel, KernelOwner(), 1U, KernelRecords());
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
};

// ------------------------------------------------------------
// 假指针读取器：记录每一次调用，行为完全由测试预设。
// ------------------------------------------------------------
class FakePointerReader final : public IPointerReader {
public:
    // 地址 -> 该地址处读出的指针值；不在表里的地址读取失败。
    std::map<std::uint64_t, std::uint64_t> memory;
    // 读取失败时交给调用方的说明文本。
    std::string failureText;
    // 调用计数与实参记录。
    int calls = 0;
    std::vector<std::uint64_t> addresses;
    std::vector<std::uint32_t> widths;
    std::vector<MemoryTargetSession> sessions;

    bool ReadPointer(
        const MemoryTargetSession& session,
        const std::uint64_t address,
        const std::uint32_t widthBytes,
        std::uint64_t& valueOut,
        std::string& failureOut) override {
        ++calls;
        addresses.push_back(address);
        widths.push_back(widthBytes);
        sessions.push_back(session);
        valueOut = 0U;
        failureOut.clear();
        const auto found = memory.find(address);
        if (found == memory.end()) {
            failureOut = failureText;
            return false;
        }
        valueOut = found->second;
        return true;
    }
};

// MakeReader：带一条标准指针链的假读取器。
// 0x1000 -> 0x2000 -> 0x3000 -> 0x4000；0x1008 -> 0x7777；
// 0x6000 -> 0x100000000（超 32 位）；0x6008 -> 0xFFFFFFFF（32 位上限）；
// 0x9000 -> 0xFFFFF80000001000（内核半区地址）。
inline FakePointerReader MakeReader() {
    FakePointerReader reader;
    reader.memory[0x1000ULL] = 0x2000ULL;
    reader.memory[0x2000ULL] = 0x3000ULL;
    reader.memory[0x3000ULL] = 0x4000ULL;
    reader.memory[0x1008ULL] = 0x7777ULL;
    reader.memory[0x6000ULL] = 0x100000000ULL;
    reader.memory[0x6008ULL] = 0xFFFFFFFFULL;
    reader.memory[0x9000ULL] = 0xFFFFF80000001000ULL;
    return reader;
}

// ------------------------------------------------------------
// 断言辅助：失败标签里带上输入与实际结果，一眼看出哪条、得到了什么。
// ------------------------------------------------------------
inline std::wstring Widen(const std::string& text) {
    std::wstring wide;
    for (const char character : text) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (byte >= 0x20U && byte < 0x7FU) {
            wide.push_back(static_cast<wchar_t>(byte));
        } else {
            wide.push_back(L'?');
        }
    }
    return wide;
}

inline std::wstring Describe(const AddressEval& eval) {
    std::wostringstream stream;
    stream << L"{ok=" << (eval.expr.ok ? 1 : 0)
           << L" value=0x" << std::hex << eval.expr.value << std::dec
           << L" err=" << static_cast<int>(eval.expr.error)
           << L" issue=" << static_cast<int>(eval.issue)
           << L" detail='" << Widen(eval.detail) << L"'"
           << L" matches=" << eval.matchCount
           << L" inSpace=" << (eval.inScopeSpace ? 1 : 0)
           << L" suggest=" << (eval.suggestScope.has_value()
                   ? static_cast<int>(*eval.suggestScope) : -1)
           << L"}";
    return stream.str();
}

inline std::wstring EvalLabel(const wchar_t* what, const std::string& text, const AddressEval& eval) {
    std::wostringstream stream;
    stream << L"session resolver: " << what << L" [" << Widen(text) << L"] got " << Describe(eval);
    return stream.str();
}

// 套件入口声明：Eval 那一半检查定义在 SessionAddressResolverTests.Eval.cpp，
// 由 RunMemwbSessionResolverTests 调用。
void RunMemwbSessionResolverEvalChecks(KswordTests::Suite& suite);

} // namespace memwb_wpb_test
