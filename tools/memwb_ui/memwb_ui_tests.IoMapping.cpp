// memwb_ui_tests.IoMapping.cpp
// 作用：M-1（主会话审核拆分）的离线验证——覆盖
// Ksword5.1/Ksword5.1/MemoryDock/WorkbenchIoMapping.h 三个纯映射函数的每一个
// 分支：MapFacadeReadOutcome（门面读结果 -> IoReadResult）、
// MapFacadeWriteOutcome（门面写结果 -> IoWriteResult）、
// MapStandardDriverVirtualRead（标准驱动虚拟读 -> IoReadResult，含 R-1 的
// 长度三支修复）。三个函数都是纯函数（不碰 Win32、不调门面或驱动），本文件
// 只手工构造 AccessOutcome / VirtualMemoryReadResult 喂给它们，不需要真的
// 打开进程或驱动句柄，因此可以和 HexCanvas 的离屏夹具一起用 MSVC + Qt 链接
// 成同一个测试程序。
//
// 为什么要把这三个函数的分支逐条摆出来：它们是真实端口里"状态翻译"最容易
// 出错的一跳——翻译表漏一行、顺序颠倒一个判断，错误只会表现成"明明读到了
// 数据却显示 ??"或者"明明失败了却显示空白"，不会抛异常，必须靠手算期望值
// 逐分支断言才能钉住。

#include "memwb_ui_common.h"
#include "memwb_ui_tests.IoMapping.h"

#include "../../Ksword5.1/Ksword5.1/MemoryDock/WorkbenchIoMapping.h"

#include <QByteArray>
#include <QString>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace memwb_test
{
    namespace
    {
        using ksword::memory_backend::AccessOutcome;
        using ksword::ark::VirtualMemoryReadResult;
        using ksword::memwb::IoReadStatus;
        using ksword::memwb::IoWriteResult;

        // BytesToQByteArray：std::vector<uint8_t> -> QByteArray，测试里手算
        // 期望数据时少写几个转换。传入：bytes 任意字节序列（可为空）。
        QByteArray BytesToQByteArray(const std::vector<std::uint8_t>& bytes)
        {
            if (bytes.empty())
            {
                return QByteArray();
            }
            return QByteArray(reinterpret_cast<const char*>(bytes.data()),
                static_cast<qsizetype>(bytes.size()));
        }

        // MakePattern8：生成长度为 length、起始值 seed 的确定性字节序列，与
        // 其余 memwb 测试文件的 MakePattern 同一手法，纯粹是换一个无歧义的
        // 名字避免跟 memwb_ui_common.h 里按 QByteArray 签名的 MakePattern
        // 互相遮蔽。
        std::vector<std::uint8_t> MakePattern8(std::uint8_t seed, std::size_t length)
        {
            std::vector<std::uint8_t> data(length);
            for (std::size_t index = 0; index < length; ++index)
            {
                data[index] = static_cast<std::uint8_t>(seed + static_cast<std::uint8_t>(index));
            }
            return data;
        }

        // ============================================================
        // 一、MapFacadeReadOutcome：门面 AccessOutcome -> IoReadResult。
        // ============================================================
        void TestMapFacadeReadOutcome()
        {
            // 分支 1：scratchAreaDirty 优先于 ok——即使 ok=true 且 partial=
            // false（正常情况下该判 Ok），只要 scratchDirty 为真，就必须是
            // Failed。这是判据顺序本身的回归锁：翻译表如果把这一条挪到
            // ok/partial 判断之后，这里会立刻报错。
            {
                AccessOutcome outcome;
                outcome.ok = true;
                outcome.partial = false;
                outcome.scratchDirty = true;
                outcome.failureText = QStringLiteral("暂存扇区未还原");
                outcome.data = BytesToQByteArray(MakePattern8(0x10, 8));

                const auto result = ksword::memwb_ports_detail::MapFacadeReadOutcome(outcome);
                CHECK(result.status == IoReadStatus::Failed);
                CHECK(result.failure == "暂存扇区未还原");
                CHECK(result.scratchAreaDirty);
                CHECK(result.data.empty());
            }

            // 分支 2：Ok 带注记保留——ok=true、partial=false、scratchDirty=
            // false，但 failureText 非空（私有页表窗口回退一类的说明）：这段
            // 文本必须原样搬进 result.failure，不能因为"成功了"就被清空。
            {
                const std::vector<std::uint8_t> payload = MakePattern8(0x20, 16);
                AccessOutcome outcome;
                outcome.ok = true;
                outcome.partial = false;
                outcome.scratchDirty = false;
                outcome.lostUpdateWindow = true;
                outcome.failureText = QStringLiteral("已回退到普通路径");
                outcome.data = BytesToQByteArray(payload);

                const auto result = ksword::memwb_ports_detail::MapFacadeReadOutcome(outcome);
                CHECK(result.status == IoReadStatus::Ok);
                CHECK(result.data == payload);
                CHECK(result.failure == "已回退到普通路径");
                // flags 搬运：readModifyWriteWindow 必须跟着原样搬过来，
                // 与 status 判到哪一支无关。
                CHECK(result.readModifyWriteWindow);
                CHECK(!result.scratchAreaDirty);
            }

            // 分支 3a：ok 且 partial -> Partial，data 即真实前缀。
            {
                const std::vector<std::uint8_t> prefix = MakePattern8(0x30, 5);
                AccessOutcome outcome;
                outcome.ok = true;
                outcome.partial = true;
                outcome.data = BytesToQByteArray(prefix);

                const auto result = ksword::memwb_ports_detail::MapFacadeReadOutcome(outcome);
                CHECK(result.status == IoReadStatus::Partial);
                CHECK(result.data == prefix);
            }

            // 分支 3b：!ok 且 data 非空 -> 同样是 Partial，data 即真实前缀。
            // 这一支与 3a 走的是不同的布尔组合，必须单独测，不能靠 3a 顶替。
            {
                const std::vector<std::uint8_t> prefix = MakePattern8(0x40, 3);
                AccessOutcome outcome;
                outcome.ok = false;
                outcome.partial = false; // 故意保持 false，验证"!ok+有数据"单独成立 Partial。
                outcome.data = BytesToQByteArray(prefix);
                outcome.failureText = QStringLiteral("读了一部分就断了");

                const auto result = ksword::memwb_ports_detail::MapFacadeReadOutcome(outcome);
                CHECK(result.status == IoReadStatus::Partial);
                CHECK(result.data == prefix);
                CHECK(result.failure == "读了一部分就断了");
            }

            // 分支 4：!ok 且 data 为空 -> Unreadable。
            {
                AccessOutcome outcome;
                outcome.ok = false;
                outcome.data = QByteArray();
                outcome.failureText = QStringLiteral("目标不可读");

                const auto result = ksword::memwb_ports_detail::MapFacadeReadOutcome(outcome);
                CHECK(result.status == IoReadStatus::Unreadable);
                CHECK(result.failure == "目标不可读");
                CHECK(result.data.empty());
            }
        }

        // ============================================================
        // 二、MapFacadeWriteOutcome：门面 AccessOutcome -> IoWriteResult。
        // 这个方向没有状态分支，只有"搬运对不对"：逐字段核对，并单独钉死
        // forceRequired->needsApproval 的改名与 rolledBack 恒为 false。
        // ============================================================
        void TestMapFacadeWriteOutcome()
        {
            // 分支 1：forceRequired -> needsApproval（改名搬运）。
            {
                AccessOutcome outcome;
                outcome.ok = false;
                outcome.forceRequired = true;
                outcome.failureText = QStringLiteral("需要用户确认后带强制标志重试");

                const IoWriteResult result = ksword::memwb_ports_detail::MapFacadeWriteOutcome(outcome);
                CHECK(result.needsApproval);
                CHECK(!result.ok);
                CHECK(result.failure == "需要用户确认后带强制标志重试");
            }

            // 分支 2：rolledBack 恒为 false——即便 outcome 本身"看起来很成功"，
            // 门面从不自动回滚，这里也绝不能被意外置真。
            {
                AccessOutcome outcome;
                outcome.ok = true;
                outcome.bytesDone = 64;

                const IoWriteResult result = ksword::memwb_ports_detail::MapFacadeWriteOutcome(outcome);
                CHECK(!result.rolledBack);
            }

            // 分支 3：各字段忠实搬运——一次性把六个字段都设成互不相同、容易
            // 串位的值，逐一核对没有哪两个字段被写反。
            {
                AccessOutcome outcome;
                outcome.ok = true;
                outcome.partial = true;
                outcome.bytesDone = 4096;
                outcome.forceRequired = false;
                outcome.scratchDirty = true;
                outcome.lostUpdateWindow = true;
                outcome.failureText = QStringLiteral("部分写入，暂存区也脏了");

                const IoWriteResult result = ksword::memwb_ports_detail::MapFacadeWriteOutcome(outcome);
                CHECK(result.ok);
                CHECK(result.partial);
                CHECK(result.bytesDone == 4096);
                CHECK(!result.needsApproval);
                CHECK(result.scratchAreaDirty);
                CHECK(result.readModifyWriteWindow);
                CHECK(result.failure == "部分写入，暂存区也脏了");
            }
        }

        // MakeDriverRead：构造一条"IO 通信本身成功"的 VirtualMemoryReadResult，
        // 调用方法：传入 readStatus、requestedBytes、data；io.ok 固定为 true，
        // 其余字段取默认值——下面每个分支只关心这三个字段的组合。
        VirtualMemoryReadResult MakeDriverRead(
            const std::uint32_t readStatus,
            const std::uint32_t requestedBytes,
            const std::vector<std::uint8_t>& data)
        {
            VirtualMemoryReadResult result;
            result.io.ok = true;
            result.readStatus = readStatus;
            result.requestedBytes = requestedBytes;
            result.data = data;
            return result;
        }

        // ============================================================
        // 三、MapStandardDriverVirtualRead：标准驱动虚拟读 -> IoReadResult，
        // 覆盖 io 失败、四种 readStatus、以及 R-1 新增的"OK 但长度不对"两支。
        // ============================================================
        void TestMapStandardDriverVirtualRead()
        {
            // 分支 1：io.ok==false -> Failed（通道通信失败，与 readStatus 无关）。
            {
                VirtualMemoryReadResult driverResult;
                driverResult.io.ok = false;
                driverResult.io.message = "句柄已失效";
                driverResult.readStatus = KSWORD_ARK_MEMORY_READ_STATUS_OK; // 故意给一个"看起来成功"的状态，验证 io.ok 优先。

                const auto result = ksword::memwb_ports_detail::MapStandardDriverVirtualRead(driverResult);
                CHECK(result.status == IoReadStatus::Failed);
                CHECK(result.failure.find("句柄已失效") != std::string::npos);
            }

            // 分支 2：OK 且 data.size()==requestedBytes -> Ok。
            {
                const std::vector<std::uint8_t> data = MakePattern8(0x50, 16);
                const VirtualMemoryReadResult driverResult = MakeDriverRead(
                    KSWORD_ARK_MEMORY_READ_STATUS_OK, 16U, data);

                const auto result = ksword::memwb_ports_detail::MapStandardDriverVirtualRead(driverResult);
                CHECK(result.status == IoReadStatus::Ok);
                CHECK(result.data.size() == 16);
                CHECK(result.data == data);
            }

            // 分支 3（R-1）：OK 但 data 偏短且非空 -> Partial，data 即真实
            // 前缀，不能当成"整段都读到了"。
            {
                const std::vector<std::uint8_t> shortData = MakePattern8(0x60, 10);
                const VirtualMemoryReadResult driverResult = MakeDriverRead(
                    KSWORD_ARK_MEMORY_READ_STATUS_OK, 16U, shortData); // 请求 16，实际只带回 10。

                const auto result = ksword::memwb_ports_detail::MapStandardDriverVirtualRead(driverResult);
                CHECK(result.status == IoReadStatus::Partial);
                CHECK(result.data.size() == 10);
                CHECK(result.data == shortData);
                CHECK(!result.failure.empty());
            }

            // 分支 4（R-1）：OK 但 data 为空 -> Failed（不是 Unreadable，也
            // 不能是 Ok——Ok 要求 data.size()==length，这里满足不了）。
            {
                const VirtualMemoryReadResult driverResult = MakeDriverRead(
                    KSWORD_ARK_MEMORY_READ_STATUS_OK, 16U, std::vector<std::uint8_t>());

                const auto result = ksword::memwb_ports_detail::MapStandardDriverVirtualRead(driverResult);
                CHECK(result.status == IoReadStatus::Failed);
                CHECK(result.data.empty());
            }

            // 分支 5：PARTIAL_COPY 且 data 非空 -> Partial，data 即前缀。
            {
                const std::vector<std::uint8_t> prefix = MakePattern8(0x70, 4);
                const VirtualMemoryReadResult driverResult = MakeDriverRead(
                    KSWORD_ARK_MEMORY_READ_STATUS_PARTIAL_COPY, 16U, prefix);

                const auto result = ksword::memwb_ports_detail::MapStandardDriverVirtualRead(driverResult);
                CHECK(result.status == IoReadStatus::Partial);
                CHECK(result.data == prefix);
            }

            // 分支 6：PARTIAL_COPY 但 data 为空 -> Unreadable（防御性兜底，
            // 协议上不应出现，但不能把空前缀说成"部分成功"）。
            {
                const VirtualMemoryReadResult driverResult = MakeDriverRead(
                    KSWORD_ARK_MEMORY_READ_STATUS_PARTIAL_COPY, 16U, std::vector<std::uint8_t>());

                const auto result = ksword::memwb_ports_detail::MapStandardDriverVirtualRead(driverResult);
                CHECK(result.status == IoReadStatus::Unreadable);
            }

            // 分支 7：COPY_FAILED -> Unreadable（目标不可读，bytesRead==0）。
            {
                const VirtualMemoryReadResult driverResult = MakeDriverRead(
                    KSWORD_ARK_MEMORY_READ_STATUS_COPY_FAILED, 16U, std::vector<std::uint8_t>());

                const auto result = ksword::memwb_ports_detail::MapStandardDriverVirtualRead(driverResult);
                CHECK(result.status == IoReadStatus::Unreadable);
            }

            // 分支 8：其它 readStatus（协议不匹配/范围被拒等）-> Failed。
            // 用 RANGE_REJECTED 举一例，含义是"驱动自己拒绝了这个范围"。
            {
                const VirtualMemoryReadResult driverResult = MakeDriverRead(
                    KSWORD_ARK_MEMORY_READ_STATUS_RANGE_REJECTED, 16U, std::vector<std::uint8_t>());

                const auto result = ksword::memwb_ports_detail::MapStandardDriverVirtualRead(driverResult);
                CHECK(result.status == IoReadStatus::Failed);
            }

            // 分支 9（防御性，R-1 文档里一并说明）：OK 但 data 比 requestedBytes
            // 还长——仍按 Ok 处理，但必须裁到 requestedBytes，保持
            // IoReadResult 的"Ok 时 data.size()==length"契约。
            {
                const std::vector<std::uint8_t> longData = MakePattern8(0x80, 20);
                const VirtualMemoryReadResult driverResult = MakeDriverRead(
                    KSWORD_ARK_MEMORY_READ_STATUS_OK, 16U, longData); // 请求 16，实际带回 20。

                const auto result = ksword::memwb_ports_detail::MapStandardDriverVirtualRead(driverResult);
                CHECK(result.status == IoReadStatus::Ok);
                CHECK(result.data.size() == 16);
                CHECK(std::equal(result.data.begin(), result.data.end(), longData.begin()));
            }
        }
    } // namespace

    // RunIoMappingTests：按声明顺序跑完三组分支测试，断言计入共享计数器。
    void RunIoMappingTests()
    {
        TestMapFacadeReadOutcome();
        TestMapFacadeWriteOutcome();
        TestMapStandardDriverVirtualRead();
    }
}
