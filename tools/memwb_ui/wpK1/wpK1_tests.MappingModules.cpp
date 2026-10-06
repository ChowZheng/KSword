// ============================================================
// wpK1_tests.MappingModules.cpp
// 作用：WorkbenchServicesMapping 里"模块记录映射 / 进程模块快照判定 / 内核模块失败说明"
//       三组纯函数的逐分支断言。所有输入都是手工构造的，不触碰系统。
// ============================================================

#include "wpK1_common.h"

#include "../../../Ksword5.1/Ksword5.1/MemoryDock/WorkbenchServicesMapping.h"

#include <cstdint>
#include <string>
#include <vector>

namespace svc = ksword::memwb_services_detail;

namespace
{
    // MakeRaw：构造一条原始模块字段，缩短测试代码。
    svc::RawModuleInfo MakeRaw(
        const std::string& name,
        const std::string& fullPath,
        const std::uint64_t base,
        const std::uint64_t size)
    {
        svc::RawModuleInfo raw;
        raw.name = name;
        raw.fullPath = fullPath;
        raw.base = base;
        raw.size = size;
        return raw;
    }

    // TestFileNameFromPath：路径取最后一个分量，两种分隔符都认。
    void TestFileNameFromPath()
    {
        WPK1_CHECK(svc::FileNameFromPath("C:\\Windows\\System32\\ntdll.dll") == "ntdll.dll");
        // 内核模块的 NT 路径。
        WPK1_CHECK(svc::FileNameFromPath("\\SystemRoot\\system32\\CI.dll") == "CI.dll");
        WPK1_CHECK(svc::FileNameFromPath("\\??\\C:\\drivers\\x.sys") == "x.sys");
        // 正斜杠与混合分隔符。
        WPK1_CHECK(svc::FileNameFromPath("a/b/c.exe") == "c.exe");
        WPK1_CHECK(svc::FileNameFromPath("a\\b/c.exe") == "c.exe");
        // 没有分隔符：原样返回。
        WPK1_CHECK(svc::FileNameFromPath("plain.dll") == "plain.dll");
        // 以分隔符结尾：最后一个分量为空。
        WPK1_CHECK(svc::FileNameFromPath("dir\\").empty());
        WPK1_CHECK(svc::FileNameFromPath("").empty());
    }

    // TestMapRawModule：单条映射的每个拒绝分支与补名规则。
    void TestMapRawModule()
    {
        ksword::memwb::ModuleRecord record;

        // 正常：四个字段原样带过去。
        WPK1_CHECK(svc::MapRawModule(MakeRaw("a.dll", "C:\\x\\a.dll", 0x1000U, 0x200U), &record));
        WPK1_CHECK(record.name == "a.dll");
        WPK1_CHECK(record.fullPath == "C:\\x\\a.dll");
        WPK1_CHECK(record.base == 0x1000U);
        WPK1_CHECK(record.size == 0x200U);

        // 基址为 0 = 无效记录：返回 false，且输出保持不变（用哨兵值证明没有被改写）。
        ksword::memwb::ModuleRecord untouched;
        untouched.name = "sentinel";
        untouched.base = 0x77U;
        WPK1_CHECK(!svc::MapRawModule(MakeRaw("b.dll", "C:\\b.dll", 0U, 1U), &untouched));
        WPK1_CHECK(untouched.name == "sentinel");
        WPK1_CHECK(untouched.base == 0x77U);

        // 名字为空：从完整路径取最后一个分量。
        ksword::memwb::ModuleRecord filled;
        WPK1_CHECK(svc::MapRawModule(MakeRaw("", "C:\\dir\\c.dll", 0x2000U, 0U), &filled));
        WPK1_CHECK(filled.name == "c.dll");
        WPK1_CHECK(filled.fullPath == "C:\\dir\\c.dll");

        // 名字有、路径空：保留（内核/某些枚举源可能只给名字）。
        ksword::memwb::ModuleRecord nameOnly;
        WPK1_CHECK(svc::MapRawModule(MakeRaw("only.sys", "", 0x3000U, 4U), &nameOnly));
        WPK1_CHECK(nameOnly.name == "only.sys");
        WPK1_CHECK(nameOnly.fullPath.empty());

        // 名字与路径都空：没有任何可查询的名字，拒绝，输出不变。
        ksword::memwb::ModuleRecord nameless;
        nameless.name = "keep";
        WPK1_CHECK(!svc::MapRawModule(MakeRaw("", "", 0x4000U, 4U), &nameless));
        WPK1_CHECK(nameless.name == "keep");

        // 名字空 + 路径以分隔符结尾（取不出文件名分量）：补名后仍为空，拒绝，输出不变。
        ksword::memwb::ModuleRecord trailing;
        trailing.name = "keep2";
        WPK1_CHECK(!svc::MapRawModule(MakeRaw("", "C:\\dir\\", 0x5000U, 4U), &trailing));
        WPK1_CHECK(trailing.name == "keep2");

        // 输出指针为空：安全返回 false，不崩溃。
        WPK1_CHECK(!svc::MapRawModule(MakeRaw("a.dll", "C:\\a.dll", 0x1000U, 1U), nullptr));

        // 大小与基址取 64 位全宽，不被截断。
        ksword::memwb::ModuleRecord wide;
        WPK1_CHECK(svc::MapRawModule(
            MakeRaw("w.sys", "\\w.sys", 0xFFFFF80012345000ULL, 0x1'0000'0000ULL), &wide));
        WPK1_CHECK(wide.base == 0xFFFFF80012345000ULL);
        WPK1_CHECK(wide.size == 0x1'0000'0000ULL);
    }

    // TestMapRawModules：批量映射——丢弃无效项、按基址升序稳定排序。
    void TestMapRawModules()
    {
        // 空输入 -> 空输出。
        WPK1_CHECK(svc::MapRawModules({}).empty());

        // 乱序输入 + 一条无效（基址 0）+ 一条无名：只剩有效项并按基址升序。
        const std::vector<svc::RawModuleInfo> input = {
            MakeRaw("c.dll", "C:\\c.dll", 0x3000U, 1U),
            MakeRaw("zero.dll", "C:\\zero.dll", 0U, 1U),
            MakeRaw("a.dll", "C:\\a.dll", 0x1000U, 1U),
            MakeRaw("", "", 0x9000U, 1U),
            MakeRaw("b.dll", "C:\\b.dll", 0x2000U, 1U),
        };
        const std::vector<ksword::memwb::ModuleRecord> sorted = svc::MapRawModules(input);
        WPK1_CHECK(sorted.size() == 3U);
        if (sorted.size() == 3U)
        {
            WPK1_CHECK(sorted[0].name == "a.dll");
            WPK1_CHECK(sorted[1].name == "b.dll");
            WPK1_CHECK(sorted[2].name == "c.dll");
        }

        // 基址相同的两条保持枚举顺序（稳定排序）。
        const std::vector<svc::RawModuleInfo> ties = {
            MakeRaw("first.dll", "C:\\first.dll", 0x1000U, 1U),
            MakeRaw("second.dll", "C:\\second.dll", 0x1000U, 1U),
            MakeRaw("third.dll", "C:\\third.dll", 0x1000U, 1U),
        };
        const std::vector<ksword::memwb::ModuleRecord> tieResult = svc::MapRawModules(ties);
        WPK1_CHECK(tieResult.size() == 3U);
        if (tieResult.size() == 3U)
        {
            WPK1_CHECK(tieResult[0].name == "first.dll");
            WPK1_CHECK(tieResult[1].name == "second.dll");
            WPK1_CHECK(tieResult[2].name == "third.dll");
        }

        // 同一输入两次调用得到逐项相同的结果（确定性）。
        const std::vector<ksword::memwb::ModuleRecord> again = svc::MapRawModules(input);
        WPK1_CHECK(again.size() == sorted.size());
        bool identical = again.size() == sorted.size();
        for (std::size_t index = 0U; identical && index < again.size(); ++index)
        {
            identical = again[index].name == sorted[index].name && again[index].base == sorted[index].base;
        }
        WPK1_CHECK(identical);
    }

    // TestJudgeProcessModuleSnapshot：成败只看诊断文本里的 "Module source:" 标记。
    void TestJudgeProcessModuleSnapshot()
    {
        // 两种来源标记都算成功，失败文本为空。
        const svc::ModuleSnapshotVerdict toolhelp =
            svc::JudgeProcessModuleSnapshot("Module source: Toolhelp", 10U);
        WPK1_CHECK(toolhelp.ok);
        WPK1_CHECK(toolhelp.failure.empty());
        WPK1_CHECK(svc::JudgeProcessModuleSnapshot("Module source: PSAPI fallback", 3U).ok);

        // 标记夹在其它诊断中间也算成功（诊断用 " | " 拼接）。
        WPK1_CHECK(svc::JudgeProcessModuleSnapshot(
            "CreateToolhelp32Snapshot(module) failed: x | Module source: PSAPI fallback", 2U).ok);

        // 空模块列表不等于失败：进程可以真的没有模块（标记在，count 为 0）。
        WPK1_CHECK(svc::JudgeProcessModuleSnapshot(
            "Module source: Toolhelp | Module enumeration succeeded but module count is 0.", 0U).ok);

        // 身份核验失败：没有标记，失败文本取诊断全文。
        const std::string identityText = "process identity changed (PID was reused); module snapshot skipped.";
        const svc::ModuleSnapshotVerdict reused = svc::JudgeProcessModuleSnapshot(identityText, 0U);
        WPK1_CHECK(!reused.ok);
        WPK1_CHECK(reused.failure == identityText);

        // 打不开进程。
        const svc::ModuleSnapshotVerdict denied = svc::JudgeProcessModuleSnapshot(
            "OpenProcess(for module identity) failed: access denied", 0U);
        WPK1_CHECK(!denied.ok);
        WPK1_CHECK(denied.failure.find("access denied") != std::string::npos);

        // 没有任何路径成功。
        WPK1_CHECK(!svc::JudgeProcessModuleSnapshot("No module enumeration path succeeded.", 0U).ok);

        // 模块列表非空但没有成功标记：仍判失败（不能凭"有记录"推断成功）。
        WPK1_CHECK(!svc::JudgeProcessModuleSnapshot("something else", 5U).ok);

        // 诊断为空（协议外）：判失败并给一句带模块数的兜底说明，不返回空的失败原因。
        const svc::ModuleSnapshotVerdict empty = svc::JudgeProcessModuleSnapshot("", 7U);
        WPK1_CHECK(!empty.ok);
        WPK1_CHECK(!empty.failure.empty());
        WPK1_CHECK(empty.failure.find("7") != std::string::npos);

        // 标记大小写敏感且要求完整前缀："module source:" 小写不算（诊断文本是程序生成的固定串）。
        WPK1_CHECK(!svc::JudgeProcessModuleSnapshot("module source: toolhelp", 1U).ok);
    }

    // TestKernelModuleFailure：内核模块失败说明与状态名映射。
    void TestKernelModuleFailure()
    {
        // 状态名：四个已知值与越界值。
        WPK1_CHECK(std::string(svc::KernelModuleQueryStatusName(0U)) == "Ok");
        WPK1_CHECK(std::string(svc::KernelModuleQueryStatusName(1U)) == "ApiUnavailable");
        WPK1_CHECK(std::string(svc::KernelModuleQueryStatusName(2U)) == "LengthQueryFailed");
        WPK1_CHECK(std::string(svc::KernelModuleQueryStatusName(3U)) == "SnapshotFailed");
        WPK1_CHECK(std::string(svc::KernelModuleQueryStatusName(4U)) == "Unknown");
        WPK1_CHECK(std::string(svc::KernelModuleQueryStatusName(0xFFFFFFFFU)) == "Unknown");

        // 失败说明：状态名、8 位大写十六进制 NTSTATUS（带前导零）、所需字节数。
        WPK1_CHECK(svc::FormatKernelModuleFailure("SnapshotFailed", 0xC0000004U, 123U) ==
            "kernel module query failed: status=SnapshotFailed, ntstatus=0xC0000004, requiredBytes=123");
        WPK1_CHECK(svc::FormatKernelModuleFailure("LengthQueryFailed", 0x5U, 0U) ==
            "kernel module query failed: status=LengthQueryFailed, ntstatus=0x00000005, requiredBytes=0");

        // 状态名为空指针或空串：写 Unknown，不崩溃。
        WPK1_CHECK(svc::FormatKernelModuleFailure(nullptr, 1U, 2U).find("status=Unknown,") != std::string::npos);
        WPK1_CHECK(svc::FormatKernelModuleFailure("", 1U, 2U).find("status=Unknown,") != std::string::npos);
    }
}

namespace wpK1_test
{
    void RunMappingModuleTests()
    {
        TestFileNameFromPath();
        TestMapRawModule();
        TestMapRawModules();
        TestJudgeProcessModuleSnapshot();
        TestKernelModuleFailure();
    }
}
