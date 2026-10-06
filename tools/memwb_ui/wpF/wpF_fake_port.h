#pragma once

// ============================================================
// wpF_fake_port.h
// 作用：
// - WP-F（int3 补丁）离屏验证夹具专用的假端口，实现 ksword::memwb::IMemoryIoPort。
// - 真实链路是 Int3Controller -> Int3PatchLedger -> MemoryPatchByteStore -> IMemoryIoPort，
//   本夹具链接**真实的** MemoryPatchByteStore（shared/evidence/memory_workbench），只在
//   最底层换成这个假端口，这样"FORCE_REQUIRED 不自动强制、approved 恒为 false"这类断言
//   验证的是生产代码的真实行为，不是夹具自己编的逻辑。
// - 只属于本夹具目录，不是仓库共享文件，可以随意改。
// ============================================================

#include "../../../shared/evidence/memory_workbench/MemoryIoPort.h"
#include "../../../shared/evidence/memory_workbench/MemoryTargetSession.h"

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace wpf_test
{
    // FakeMemoryIoPort：用一个地址到字节的映射模拟"目标内存"，并提供多个故障注入开关。
    // 非线程安全——夹具全程单线程调用，与真实实现"每次按 PID 开句柄"的线程安全要求无关。
    class FakeMemoryIoPort final : public ksword::memwb::IMemoryIoPort
    {
    public:
        // memory：模拟的目标内存；未出现过的地址视为"从未写入"，Read 时按 Unreadable 处理。
        std::unordered_map<std::uint64_t, std::uint8_t> memory;

        // failAllReads：Read 恒返回 Failed（通道自身故障，不是"目标不可读"）。
        bool failAllReads = false;
        // failReadOnCall：仅第 N 次读取失败，用于安装后的回读故障。
        int failReadOnCall = 0;
        // unreadableAllReads：Read 恒返回 Unreadable（一个字节都没读到，原因在目标本身）。
        bool unreadableAllReads = false;
        // nextReadOverride + overrideAtReadCall：第 overrideAtReadCall 次 1 字节 Read（调用计数
        // 从 1 开始）返回 nextReadOverride 而不是 memory 里的真实值，用它模拟"写入后回读到了
        // 别的值"（VerifyFailed）。必须按调用序号而不是"下一次"——int3 安装先读一次原字节、
        // 写入、再读一次验证，这两次都是 1 字节 Read，覆盖值必须只命中第二次（验证那次），
        // 否则会把"原字节"本身读错，连带推翻整条检查顺序。用后自动失效（overrideAtReadCall 清 0）。
        std::optional<std::uint8_t> nextReadOverride;
        int overrideAtReadCall = 0;

        // requireApproval：为真时 Write 要求 approved==true，否则返回 needsApproval 且不写入
        // 任何字节——用它模拟标准驱动通道的 FORCE_REQUIRED。
        bool requireApproval = false;
        // failAllWrites：Write 恒失败（非 approval 原因，模拟通道写故障）。
        bool failAllWrites = false;
        // failWriteOnCall：仅第 N 次写入失败且不改字节，用于安装失败后的回滚故障。
        int failWriteOnCall = 0;

        // 调用计数：供测试断言"路由被拒绝时端口调用次数恒为 0"一类判据。
        int readCalls = 0;
        int writeCalls = 0;
        // approvedTrueCount / approvedFalseCount：Write 收到的 approved 参数分别为真/假的
        // 次数。int3 补丁零摩擦决策下，经由 MemoryPatchByteStore 的写入 approvedTrueCount
        // 必须恒为 0。
        int approvedTrueCount = 0;
        int approvedFalseCount = 0;

        // Limits：int3 补丁每次只读写 1 字节，这里给的上限对它无意义，固定返回一个占位值。
        ksword::memwb::IoLimits Limits(const ksword::memwb::MemoryTargetSession&) const override
        {
            return ksword::memwb::IoLimits{1ULL, 1ULL};
        }

        // Read：规则见各开关的注释；默认行为是如实读取 memory。
        ksword::memwb::IoReadResult Read(
            const ksword::memwb::MemoryTargetSession&,
            const std::uint64_t address,
            const std::uint64_t length) override
        {
            ++readCalls;
            ksword::memwb::IoReadResult result;

            if (failAllReads || (failReadOnCall != 0 && readCalls == failReadOnCall))
            {
                result.status = ksword::memwb::IoReadStatus::Failed;
                return result;
            }
            if (unreadableAllReads)
            {
                result.status = ksword::memwb::IoReadStatus::Unreadable;
                return result;
            }
            if (nextReadOverride.has_value() && length == 1ULL && readCalls == overrideAtReadCall)
            {
                result.status = ksword::memwb::IoReadStatus::Ok;
                result.data.push_back(*nextReadOverride);
                nextReadOverride.reset();
                overrideAtReadCall = 0;
                return result;
            }

            // 逐字节拼前缀，遇到第一个没写过的地址就停。
            std::vector<std::uint8_t> bytes;
            for (std::uint64_t offset = 0; offset < length; ++offset)
            {
                const auto it = memory.find(address + offset);
                if (it == memory.end())
                {
                    break;
                }
                bytes.push_back(it->second);
            }
            if (bytes.size() == length)
            {
                result.status = ksword::memwb::IoReadStatus::Ok;
                result.data = std::move(bytes);
            }
            else if (!bytes.empty())
            {
                result.status = ksword::memwb::IoReadStatus::Partial;
                result.data = std::move(bytes);
            }
            else
            {
                result.status = ksword::memwb::IoReadStatus::Unreadable;
            }
            return result;
        }

        // Write：requireApproval 且未同意时拒绝且不写任何字节；其余情形按 failAllWrites
        // 决定是否真的落地到 memory。
        ksword::memwb::IoWriteResult Write(
            const ksword::memwb::MemoryTargetSession&,
            const std::uint64_t address,
            const std::vector<std::uint8_t>& bytes,
            const bool approved) override
        {
            ++writeCalls;
            approved ? ++approvedTrueCount : ++approvedFalseCount;

            ksword::memwb::IoWriteResult result;
            if (requireApproval && !approved)
            {
                result.needsApproval = true;
                return result;
            }
            if (failAllWrites || (failWriteOnCall != 0 && writeCalls == failWriteOnCall))
            {
                return result;
            }
            for (std::size_t index = 0; index < bytes.size(); ++index)
            {
                memory[address + index] = bytes[index];
            }
            result.ok = true;
            result.bytesDone = bytes.size();
            return result;
        }
    };
}
