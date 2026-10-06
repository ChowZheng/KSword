// ============================================================
// wpK1_tests.MappingMisc.cpp
// 作用：WorkbenchServicesMapping 里其余几组纯函数的逐分支断言：
//       候选进程取名、页保护徽章、反汇编/汇编后端结果判据、地址簿路径选择、
//       int3 补丁的通道准入与会话构造、模块表跳转要不要钉住预览进程（3b）。
// ============================================================

#include "wpK1_common.h"

#include "../../../Ksword5.1/Ksword5.1/MemoryDock/WorkbenchServicesMapping.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace svc = ksword::memwb_services_detail;

namespace
{
    // TestFindProcessName：按 pid 取名。
    void TestFindProcessName()
    {
        const std::vector<ksword::memwb::ProcessCandidate> candidates = {
            {4U, "System"},
            {100U, "first.exe"},
            {100U, "second.exe"},
            {0U, "Idle"},
        };
        WPK1_CHECK(svc::FindProcessName(candidates, 4U) == "System");
        // 同 pid 取第一个。
        WPK1_CHECK(svc::FindProcessName(candidates, 100U) == "first.exe");
        // 没有命中。
        WPK1_CHECK(svc::FindProcessName(candidates, 999U).empty());
        // pid 为 0 恒返回空串（即使候选里有 pid=0 的项）。
        WPK1_CHECK(svc::FindProcessName(candidates, 0U).empty());
        // 空列表。
        WPK1_CHECK(svc::FindProcessName({}, 4U).empty());
    }

    // BadgeCase：徽章测试表的一行。
    struct BadgeCase
    {
        std::uint32_t state;
        std::uint32_t protect;
        std::uint32_t type;
        const char* text;
        svc::BadgeRole role;
    };

    // TestProtectionBadge：页状态/保护/类型 -> 徽章文字与配色的全表。
    void TestProtectionBadge()
    {
        const std::uint32_t commit = svc::kMemCommit;
        const std::vector<BadgeCase> cases = {
            // 未提交区域：只给状态缩写，不追加类型。
            {svc::kMemFree, 0U, 0U, "FREE", svc::BadgeRole::Idle},
            {svc::kMemFree, svc::kPageReadWrite, svc::kMemImage, "FREE", svc::BadgeRole::Idle},
            {svc::kMemReserve, 0U, svc::kMemPrivate, "RSV", svc::BadgeRole::Idle},
            // 未知状态值。
            {0U, svc::kPageReadWrite, 0U, "?", svc::BadgeRole::Idle},
            {0x123U, svc::kPageReadWrite, 0U, "?", svc::BadgeRole::Idle},
            // 已提交页的八种基础保护。
            {commit, svc::kPageNoAccess, 0U, "NA", svc::BadgeRole::Idle},
            {commit, svc::kPageReadOnly, 0U, "R", svc::BadgeRole::Info},
            {commit, svc::kPageReadWrite, 0U, "RW", svc::BadgeRole::Success},
            {commit, svc::kPageWriteCopy, 0U, "RWC", svc::BadgeRole::Success},
            {commit, svc::kPageExecute, 0U, "X", svc::BadgeRole::Warning},
            {commit, svc::kPageExecuteRead, 0U, "RX", svc::BadgeRole::Warning},
            {commit, svc::kPageExecuteReadWrite, 0U, "RWX", svc::BadgeRole::Error},
            {commit, svc::kPageExecuteWriteCopy, 0U, "RWXC", svc::BadgeRole::Error},
            // 保护值为 0 / 未知基础值。
            {commit, 0U, 0U, "?", svc::BadgeRole::Idle},
            {commit, 0x03U, 0U, "?", svc::BadgeRole::Idle},
            // 高位修饰标志（NOCACHE 0x200、WRITECOMBINE 0x400）不影响基础保护的识别，也不算守护页。
            {commit, svc::kPageReadWrite | 0x200U, 0U, "RW", svc::BadgeRole::Success},
            {commit, svc::kPageExecuteRead | 0x400U, 0U, "RX", svc::BadgeRole::Warning},
            // 守护页：追加 +G；非红色提升到橙色，红色不降级。
            {commit, svc::kPageReadWrite | svc::kPageGuard, 0U, "RW+G", svc::BadgeRole::Warning},
            {commit, svc::kPageReadOnly | svc::kPageGuard, 0U, "R+G", svc::BadgeRole::Warning},
            {commit, svc::kPageNoAccess | svc::kPageGuard, 0U, "NA+G", svc::BadgeRole::Warning},
            {commit, svc::kPageExecuteReadWrite | svc::kPageGuard, 0U, "RWX+G", svc::BadgeRole::Error},
            // 区域类型缩写。
            {commit, svc::kPageExecuteRead, svc::kMemImage, "RX (IMG)", svc::BadgeRole::Warning},
            {commit, svc::kPageReadWrite, svc::kMemMapped, "RW (MAP)", svc::BadgeRole::Success},
            {commit, svc::kPageExecuteReadWrite, svc::kMemPrivate, "RWX (PRV)", svc::BadgeRole::Error},
            // 守护页与类型同时出现：先 +G 再类型。
            {commit, svc::kPageReadWrite | svc::kPageGuard, svc::kMemPrivate, "RW+G (PRV)", svc::BadgeRole::Warning},
            // 未知类型值不追加。
            {commit, svc::kPageReadWrite, 0x5U, "RW", svc::BadgeRole::Success},
        };

        for (const BadgeCase& entry : cases)
        {
            const svc::ProtectionBadge badge = svc::FormatProtectionBadge(entry.state, entry.protect, entry.type);
            const std::string note = "state=" + std::to_string(entry.state) +
                " protect=" + std::to_string(entry.protect) + " type=" + std::to_string(entry.type) +
                " got text=" + badge.text;
            WPK1_CHECK_NOTE(badge.text == entry.text, note);
            WPK1_CHECK_NOTE(badge.role == entry.role, note);
        }
    }

    // TestDecodeAcceptance：解码后端单行结果的接受判据真值表。
    void TestDecodeAcceptance()
    {
        // 基准：已解码、来自 Zydis、长度 3、可用 10 -> 接受。
        WPK1_CHECK(svc::AcceptDecodedRow(true, true, 3U, 10U));

        // 任一前置条件缺失就拒绝。
        WPK1_CHECK(!svc::AcceptDecodedRow(false, true, 3U, 10U));
        // 降级解码器的结果不接受（不区分指令边界）。
        WPK1_CHECK(!svc::AcceptDecodedRow(true, false, 3U, 10U));

        // 长度边界：0 拒绝（否则视图原地不动）；等于可用字节接受；超过可用字节拒绝。
        WPK1_CHECK(!svc::AcceptDecodedRow(true, true, 0U, 10U));
        WPK1_CHECK(svc::AcceptDecodedRow(true, true, 1U, 1U));
        WPK1_CHECK(svc::AcceptDecodedRow(true, true, 10U, 10U));
        WPK1_CHECK(!svc::AcceptDecodedRow(true, true, 11U, 10U));

        // 单条指令上限 15 字节：15 接受，16 拒绝（即使可用字节足够多）。
        WPK1_CHECK(svc::AcceptDecodedRow(true, true, 15U, 100U));
        WPK1_CHECK(!svc::AcceptDecodedRow(true, true, 16U, 100U));
        WPK1_CHECK(!svc::AcceptDecodedRow(true, true, 1000U, 100000U));

        // 可用字节为 0：任何长度都拒绝。
        WPK1_CHECK(!svc::AcceptDecodedRow(true, true, 1U, 0U));
        WPK1_CHECK(!svc::AcceptDecodedRow(true, true, 0U, 0U));

        // 常量与协议一致：单条指令 15 字节、汇编输出 64 KiB。
        WPK1_CHECK(svc::kMaxInstructionBytes == 15U);
        WPK1_CHECK(svc::kMaxAssembledBytes == 65536U);
    }

    // TestAssembleJudgement：汇编后端结果判据——失败（含异常成功）一律不返回机器码。
    void TestAssembleJudgement()
    {
        // 后端失败：无论带了多少字节都是 BackendFailed（调用方必须清空机器码）。
        WPK1_CHECK(svc::JudgeAssembleResult(false, 0U) == svc::AssembleRejection::BackendFailed);
        WPK1_CHECK(svc::JudgeAssembleResult(false, 1U) == svc::AssembleRejection::BackendFailed);
        WPK1_CHECK(svc::JudgeAssembleResult(false, 100000U) == svc::AssembleRejection::BackendFailed);

        // 成功却没有机器码：协议外，按失败。
        WPK1_CHECK(svc::JudgeAssembleResult(true, 0U) == svc::AssembleRejection::EmptyOutput);

        // 成功且有界：1 字节到 64 KiB 接受。
        WPK1_CHECK(svc::JudgeAssembleResult(true, 1U) == svc::AssembleRejection::None);
        WPK1_CHECK(svc::JudgeAssembleResult(true, 15U) == svc::AssembleRejection::None);
        WPK1_CHECK(svc::JudgeAssembleResult(true, 65536U) == svc::AssembleRejection::None);

        // 超过 64 KiB：拒绝，包括极端大值。
        WPK1_CHECK(svc::JudgeAssembleResult(true, 65537U) == svc::AssembleRejection::OutputTooLarge);
        WPK1_CHECK(svc::JudgeAssembleResult(true, static_cast<std::size_t>(-1)) == svc::AssembleRejection::OutputTooLarge);
    }

    // TestPathHelpers：JoinPath 与 ChooseWritableDirectory。
    void TestPathHelpers()
    {
        // JoinPath：只在尾部没有分隔符时补一个 '/'。
        WPK1_CHECK(svc::JoinPath("C:\\app\\style", "book.txt") == "C:\\app\\style/book.txt");
        WPK1_CHECK(svc::JoinPath("C:\\app\\style\\", "book.txt") == "C:\\app\\style\\book.txt");
        WPK1_CHECK(svc::JoinPath("C:/app/style/", "book.txt") == "C:/app/style/book.txt");
        WPK1_CHECK(svc::JoinPath("/data", "book.txt") == "/data/book.txt");
        // 任一为空：返回空串（调用方据此判定"没有路径"，不会拼出相对根目录的奇怪路径）。
        WPK1_CHECK(svc::JoinPath("", "book.txt").empty());
        WPK1_CHECK(svc::JoinPath("C:\\app", "").empty());
        WPK1_CHECK(svc::JoinPath("", "").empty());

        // ChooseWritableDirectory：按顺序选第一个可写的，之后不再询问。
        std::vector<std::string> asked;
        const std::function<bool(const std::string&)> secondOnly = [&asked](const std::string& dir) {
            asked.push_back(dir);
            return dir == "second";
        };
        WPK1_CHECK(svc::ChooseWritableDirectory({"first", "second", "third"}, secondOnly) == "second");
        // "first" 被问过且不可写，"second" 命中后立即停止，"third" 没被问。
        WPK1_CHECK(asked.size() == 2U);
        if (asked.size() == 2U)
        {
            WPK1_CHECK(asked[0] == "first");
            WPK1_CHECK(asked[1] == "second");
        }

        // 空串候选被跳过且不交给回调。
        asked.clear();
        WPK1_CHECK(svc::ChooseWritableDirectory({"", "", "only"}, [&asked](const std::string& dir) {
            asked.push_back(dir);
            return true;
        }) == "only");
        WPK1_CHECK(asked.size() == 1U);

        // 全部不可写 -> 空串；没有候选 -> 空串；回调为空 -> 空串（不崩溃）。
        WPK1_CHECK(svc::ChooseWritableDirectory({"a", "b"}, [](const std::string&) { return false; }).empty());
        WPK1_CHECK(svc::ChooseWritableDirectory({}, [](const std::string&) { return true; }).empty());
        WPK1_CHECK(svc::ChooseWritableDirectory({"a"}, std::function<bool(const std::string&)>()).empty());
        // 只有空串候选 -> 空串。
        WPK1_CHECK(svc::ChooseWritableDirectory({""}, [](const std::string&) { return true; }).empty());
    }

    // TestPatchSession：int3 补丁的通道准入与会话构造。
    void TestPatchSession()
    {
        using ksword::memwb::Channel;

        // 通道准入：只放行三条进程范围通道；磁盘传输与越界值拒绝。
        WPK1_CHECK(svc::IsPatchChannelAllowed(Channel::UserMode));
        WPK1_CHECK(svc::IsPatchChannelAllowed(Channel::StandardDriver));
        WPK1_CHECK(svc::IsPatchChannelAllowed(Channel::Hvm));
        WPK1_CHECK(!svc::IsPatchChannelAllowed(Channel::Ddma));
        WPK1_CHECK(!svc::IsPatchChannelAllowed(static_cast<Channel>(4)));
        WPK1_CHECK(!svc::IsPatchChannelAllowed(static_cast<Channel>(0xFFFFFFFFU)));

        // 会话构造：范围恒为进程虚拟、身份三项取自目标、通道取自参数。
        ksword::memwb::PatchTarget target;
        target.pid = 4242U;
        target.processCreateTime100ns = 133000000000000000ULL;
        target.attachGeneration = 7U;
        const ksword::memwb::MemoryTargetSession session = svc::BuildPatchSession(target, Channel::StandardDriver);
        WPK1_CHECK(session.scope == ksword::memwb::Scope::ProcessVirtual);
        WPK1_CHECK(session.pid == 4242U);
        WPK1_CHECK(session.processCreateTime100ns == 133000000000000000ULL);
        WPK1_CHECK(session.attachGeneration == 7U);
        WPK1_CHECK(session.channel == Channel::StandardDriver);
        WPK1_CHECK(session.ddmaGeneration == 0U);
        WPK1_CHECK(session.addressBits == 64U);
        // 构造出的会话自洽（进程范围且 pid 非零）。
        WPK1_CHECK(ksword::memwb::Validate(session) == ksword::memwb::SessionError::None);

        // 通道参数原样进入会话，不同通道得到不同身份键（还原必须用安装时的通道）。
        const ksword::memwb::MemoryTargetSession viaUser = svc::BuildPatchSession(target, Channel::UserMode);
        WPK1_CHECK(viaUser.channel == Channel::UserMode);
        WPK1_CHECK(ksword::memwb::IdentityKey(viaUser, 0U, 1U) != ksword::memwb::IdentityKey(session, 0U, 1U));

        // pid 为 0 的目标构造出的会话不自洽（工厂层会在此之前拒绝，这里钉住这条后备防线）。
        const ksword::memwb::PatchTarget zeroTarget;
        WPK1_CHECK(ksword::memwb::Validate(svc::BuildPatchSession(zeroTarget, Channel::UserMode)) ==
            ksword::memwb::SessionError::NeedsPid);
    }
}

namespace
{
    // TestModuleJumpPin（3b）：模块表双击模块基址时要不要钉住预览进程。
    // 模块表可以预览一个并未附加的进程，基址属于预览进程；预览进程不同于附加进程时必须钉住它
    // （连同创建时间用于核对进程实例），否则会把别人的模块基址当成附加进程里的地址去看。
    void TestModuleJumpPin()
    {
        constexpr std::uint64_t kCreateTime = 0x01DB000000000123ULL;

        // 预览的是另一个进程：钉住它，带上创建时间。
        svc::ModuleJumpPin pin = svc::DecideModuleJumpPin(200U, kCreateTime, 100U);
        WPK1_CHECK(pin.pid == 200U);
        WPK1_CHECK(pin.createTime100ns == kCreateTime);

        // 没有附加任何进程、只是在预览：同样要钉住预览进程（附加 pid 为 0 不能当成"相同"）。
        pin = svc::DecideModuleJumpPin(200U, kCreateTime, 0U);
        WPK1_CHECK(pin.pid == 200U);
        WPK1_CHECK(pin.createTime100ns == kCreateTime);

        // 预览的就是附加的进程：不钉住（跟随 Dock），创建时间也不带。
        pin = svc::DecideModuleJumpPin(100U, kCreateTime, 100U);
        WPK1_CHECK(pin.pid == 0U);
        WPK1_CHECK(pin.createTime100ns == 0U);

        // 模块缓存为空（pid 为 0）：没有可钉的目标，恒不钉住，不论附加了谁。
        pin = svc::DecideModuleJumpPin(0U, kCreateTime, 100U);
        WPK1_CHECK(pin.pid == 0U);
        WPK1_CHECK(pin.createTime100ns == 0U);
        pin = svc::DecideModuleJumpPin(0U, 0U, 0U);
        WPK1_CHECK(pin.pid == 0U);

        // 创建时间没取到（0）时照样钉住，只是不带可核对的时间。
        pin = svc::DecideModuleJumpPin(300U, 0U, 100U);
        WPK1_CHECK(pin.pid == 300U);
        WPK1_CHECK(pin.createTime100ns == 0U);
    }
}

namespace wpK1_test
{
    void RunMappingMiscTests()
    {
        TestFindProcessName();
        TestProtectionBadge();
        TestDecodeAcceptance();
        TestAssembleJudgement();
        TestPathHelpers();
        TestPatchSession();
        TestModuleJumpPin();
    }
}
