// ============================================================
// wpK1_tests.MappingPointer.cpp
// 作用：WorkbenchServicesMapping 里"指针读取"三个函数的逐分支断言：
//       IsPointerReadAllowed（解引用准入）、DecodePointerLittleEndian（小端解码）、
//       MapPointerRead（端口读取结果 -> PointerReadResult）。
// 重点：Partial / Unreadable / Failed 三种非 Ok 状态绝不能拼出指针值；Ok 但字节数不对也不行。
// ============================================================

#include "wpK1_common.h"

#include "../../../Ksword5.1/Ksword5.1/MemoryDock/WorkbenchServicesMapping.h"

#include <cstdint>
#include <string>
#include <vector>

namespace svc = ksword::memwb_services_detail;

namespace
{
    // MakeRead：构造一个端口读取结果。
    ksword::memwb::IoReadResult MakeRead(
        const ksword::memwb::IoReadStatus status,
        const std::vector<std::uint8_t>& data,
        const std::string& failure)
    {
        ksword::memwb::IoReadResult read;
        read.status = status;
        read.data = data;
        read.failure = failure;
        return read;
    }

    // TestPointerReadAllowed：3 个范围 x 4 个通道的全表。
    void TestPointerReadAllowed()
    {
        using ksword::memwb::Channel;
        using ksword::memwb::Scope;

        // 物理范围：任何通道都不允许。
        WPK1_CHECK(!svc::IsPointerReadAllowed(Scope::Physical, Channel::UserMode));
        WPK1_CHECK(!svc::IsPointerReadAllowed(Scope::Physical, Channel::StandardDriver));
        WPK1_CHECK(!svc::IsPointerReadAllowed(Scope::Physical, Channel::Hvm));
        WPK1_CHECK(!svc::IsPointerReadAllowed(Scope::Physical, Channel::Ddma));

        // 磁盘传输通道：任何范围都不允许（每次解引用都会改写磁盘暂存扇区）。
        WPK1_CHECK(!svc::IsPointerReadAllowed(Scope::ProcessVirtual, Channel::Ddma));
        WPK1_CHECK(!svc::IsPointerReadAllowed(Scope::KernelVirtual, Channel::Ddma));

        // 其余六个组合允许。
        WPK1_CHECK(svc::IsPointerReadAllowed(Scope::ProcessVirtual, Channel::UserMode));
        WPK1_CHECK(svc::IsPointerReadAllowed(Scope::ProcessVirtual, Channel::StandardDriver));
        WPK1_CHECK(svc::IsPointerReadAllowed(Scope::ProcessVirtual, Channel::Hvm));
        WPK1_CHECK(svc::IsPointerReadAllowed(Scope::KernelVirtual, Channel::UserMode));
        WPK1_CHECK(svc::IsPointerReadAllowed(Scope::KernelVirtual, Channel::StandardDriver));
        WPK1_CHECK(svc::IsPointerReadAllowed(Scope::KernelVirtual, Channel::Hvm));
    }

    // TestDecodePointer：小端解码、宽度与长度校验。
    void TestDecodePointer()
    {
        std::uint64_t value = 0xDEADBEEFU;

        // 8 字节：第 0 字节是最低位。
        WPK1_CHECK(svc::DecodePointerLittleEndian({1, 2, 3, 4, 5, 6, 7, 8}, 8U, &value));
        WPK1_CHECK(value == 0x0807060504030201ULL);

        // 4 字节：零扩展到 64 位，高 32 位必须是 0（即使之前 value 里有残留）。
        value = 0xFFFFFFFFFFFFFFFFULL;
        WPK1_CHECK(svc::DecodePointerLittleEndian({0xAA, 0xBB, 0xCC, 0xDD}, 4U, &value));
        WPK1_CHECK(value == 0xDDCCBBAAULL);

        // 高位字节为 0xFF 的 8 字节（典型内核地址）不被符号扩展破坏。
        WPK1_CHECK(svc::DecodePointerLittleEndian({0x00, 0x10, 0x34, 0x12, 0x00, 0xF8, 0xFF, 0xFF}, 8U, &value));
        WPK1_CHECK(value == 0xFFFFF80012341000ULL);

        // 字节数与宽度不符：失败，且输出清零（不留残值）。
        value = 0x1234U;
        WPK1_CHECK(!svc::DecodePointerLittleEndian({1, 2, 3, 4, 5, 6, 7}, 8U, &value));
        WPK1_CHECK(value == 0U);
        value = 0x1234U;
        WPK1_CHECK(!svc::DecodePointerLittleEndian({1, 2, 3, 4, 5}, 4U, &value));
        WPK1_CHECK(value == 0U);
        value = 0x1234U;
        WPK1_CHECK(!svc::DecodePointerLittleEndian({}, 8U, &value));
        WPK1_CHECK(value == 0U);

        // 宽度只接受 4 与 8。
        value = 0x1234U;
        WPK1_CHECK(!svc::DecodePointerLittleEndian({1, 2, 3}, 3U, &value));
        WPK1_CHECK(value == 0U);
        WPK1_CHECK(!svc::DecodePointerLittleEndian({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}, 16U, &value));
        WPK1_CHECK(!svc::DecodePointerLittleEndian({}, 0U, &value));

        // 输出指针为空：安全返回 false。
        WPK1_CHECK(!svc::DecodePointerLittleEndian({1, 2, 3, 4}, 4U, nullptr));
    }

    // TestMapPointerReadOk：Ok 状态——只有字节数恰好等于宽度才成功。
    void TestMapPointerReadOk()
    {
        // 8 字节指针读成功。
        const ks::ui::PointerReadResult ok8 = svc::MapPointerRead(
            MakeRead(ksword::memwb::IoReadStatus::Ok, {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x00}, ""), 8U);
        WPK1_CHECK(ok8.ok);
        WPK1_CHECK(ok8.value == 0x0070605040302010ULL);
        WPK1_CHECK(ok8.failure.empty());

        // 4 字节指针读成功（32 位目标）。
        const ks::ui::PointerReadResult ok4 = svc::MapPointerRead(
            MakeRead(ksword::memwb::IoReadStatus::Ok, {0x78, 0x56, 0x34, 0x12}, ""), 4U);
        WPK1_CHECK(ok4.ok);
        WPK1_CHECK(ok4.value == 0x12345678ULL);

        // Ok 却字节数不对（端口违反契约）：失败，value 恒为 0，失败说明带期望与实际字节数。
        const ks::ui::PointerReadResult wrongCount = svc::MapPointerRead(
            MakeRead(ksword::memwb::IoReadStatus::Ok, {1, 2, 3, 4}, ""), 8U);
        WPK1_CHECK(!wrongCount.ok);
        WPK1_CHECK(wrongCount.value == 0U);
        WPK1_CHECK(wrongCount.failure.find("expected 8") != std::string::npos);
        WPK1_CHECK(wrongCount.failure.find("got 4") != std::string::npos);

        // Ok 且 data 为空：同样失败。
        const ks::ui::PointerReadResult emptyOk = svc::MapPointerRead(
            MakeRead(ksword::memwb::IoReadStatus::Ok, {}, ""), 8U);
        WPK1_CHECK(!emptyOk.ok);
        WPK1_CHECK(emptyOk.value == 0U);
    }

    // TestMapPointerReadNotOk：Partial / Unreadable / Failed / 未知状态——绝不拼指针。
    void TestMapPointerReadNotOk()
    {
        // Partial：data 是真实前缀（这里 5 字节），不得拿去拼指针。
        const ks::ui::PointerReadResult partial = svc::MapPointerRead(
            MakeRead(ksword::memwb::IoReadStatus::Partial, {1, 2, 3, 4, 5}, "tail not copied"), 8U);
        WPK1_CHECK(!partial.ok);
        WPK1_CHECK(partial.value == 0U);
        WPK1_CHECK(partial.failure.find("only 5 of 8") != std::string::npos);
        WPK1_CHECK(partial.failure.find("tail not copied") != std::string::npos);

        // Unreadable：目标本身读不到。
        const ks::ui::PointerReadResult unreadable = svc::MapPointerRead(
            MakeRead(ksword::memwb::IoReadStatus::Unreadable, {}, "win32=299"), 8U);
        WPK1_CHECK(!unreadable.ok);
        WPK1_CHECK(unreadable.value == 0U);
        WPK1_CHECK(unreadable.failure.find("unreadable") != std::string::npos);
        WPK1_CHECK(unreadable.failure.find("win32=299") != std::string::npos);

        // Failed：通道自身失败——说明里要能看出是"通道"问题而不是"目标不可读"。
        const ks::ui::PointerReadResult failed = svc::MapPointerRead(
            MakeRead(ksword::memwb::IoReadStatus::Failed, {}, "driver not loaded"), 8U);
        WPK1_CHECK(!failed.ok);
        WPK1_CHECK(failed.value == 0U);
        WPK1_CHECK(failed.failure.find("channel failed") != std::string::npos);
        WPK1_CHECK(failed.failure.find("driver not loaded") != std::string::npos);
        WPK1_CHECK(failed.failure.find("unreadable") == std::string::npos);

        // 端口没给细节串：说明里不出现多余的 ": "。
        const ks::ui::PointerReadResult bare = svc::MapPointerRead(
            MakeRead(ksword::memwb::IoReadStatus::Failed, {}, ""), 8U);
        WPK1_CHECK(bare.failure == "pointer read channel failed");

        // 即使 Partial/Unreadable/Failed 携带了恰好 8 字节的 data（异常端口），也不采信。
        const ks::ui::PointerReadResult sneaky = svc::MapPointerRead(
            MakeRead(ksword::memwb::IoReadStatus::Partial, {1, 2, 3, 4, 5, 6, 7, 8}, ""), 8U);
        WPK1_CHECK(!sneaky.ok);
        WPK1_CHECK(sneaky.value == 0U);
        const ks::ui::PointerReadResult sneakyFailed = svc::MapPointerRead(
            MakeRead(ksword::memwb::IoReadStatus::Failed, {1, 2, 3, 4, 5, 6, 7, 8}, "x"), 8U);
        WPK1_CHECK(!sneakyFailed.ok);
        WPK1_CHECK(sneakyFailed.value == 0U);

        // 枚举越界值：按失败处理，说明里标明未知状态。
        const ks::ui::PointerReadResult unknown = svc::MapPointerRead(
            MakeRead(static_cast<ksword::memwb::IoReadStatus>(99), {1, 2, 3, 4, 5, 6, 7, 8}, ""), 8U);
        WPK1_CHECK(!unknown.ok);
        WPK1_CHECK(unknown.value == 0U);
        WPK1_CHECK(unknown.failure.find("unknown status") != std::string::npos);
    }
}

namespace wpK1_test
{
    void RunMappingPointerTests()
    {
        TestPointerReadAllowed();
        TestDecodePointer();
        TestMapPointerReadOk();
        TestMapPointerReadNotOk();
    }
}
