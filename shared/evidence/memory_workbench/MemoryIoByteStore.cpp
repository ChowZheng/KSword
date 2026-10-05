// MemoryIoByteStore.h 的实现。Read 与 Write 各自对应文件头的两张规则表，改动
// 前请先回去确认没有偏离那两节描述的行为。

#include "MemoryIoByteStore.h"
#include "MemoryKernelMutation.h"

#include <algorithm>
#include <utility>

namespace ksword::memwb
{
    MemoryIoByteStore::MemoryIoByteStore(
        IMemoryIoPort& port,
        const MemoryTargetSession& session,
        IKernelMutationPort* kernelMutationPort)
        : port_(port)
        , session_(session)
        , kernelMutationPort_(kernelMutationPort)
    {
    }

    bool MemoryIoByteStore::IsKernelMutationRoute(const std::uint64_t address) const
    {
        // 三个条件同时成立才算"内核范围+标准驱动通道"：只看地址数值会让物理
        // 地址（恰好落进内核高半区数值范围的概率虽然极低，但并非结构性不可能）
        // 被误判，所以额外要求 scope==KernelVirtual 与 channel==StandardDriver
        // 双重确认，三者缺一都走通用的"其余范围/通道"分支。
        return session_.channel == Channel::StandardDriver
            && session_.scope == Scope::KernelVirtual
            && IsKernelVirtualAddress(address);
    }

    AccessResult MemoryIoByteStore::Read(const std::uint64_t address, const std::uint64_t length)
    {
        AccessResult result;

        // 空读取天然成功，不发起任何端口调用。
        if (length == 0)
        {
            result.ok = true;
            return result;
        }

        const IoLimits limits = port_.Limits(session_);
        const std::uint64_t maxReadBytes = limits.maxReadBytes; // 0 = 不限

        std::vector<std::uint8_t> collected;
        collected.reserve(static_cast<std::size_t>(length));
        std::uint64_t offset = 0;

        while (offset < length)
        {
            const std::uint64_t remaining = length - offset;
            const std::uint64_t chunkLength =
                (maxReadBytes == 0) ? remaining : (std::min)(maxReadBytes, remaining);
            const std::uint64_t chunkAddress = address + offset;

            const IoReadResult outcome = port_.Read(session_, chunkAddress, chunkLength);
            // 每一块都要累加这两个标志，哪怕这一块本身让整体停止——暂存区弄脏
            // 或读改写窗口的事实不会因为后面不再读而消失。
            result.scratchAreaDirty = result.scratchAreaDirty || outcome.scratchAreaDirty;
            result.readModifyWriteWindow = result.readModifyWriteWindow || outcome.readModifyWriteWindow;

            if (outcome.status == IoReadStatus::Ok)
            {
                collected.insert(collected.end(), outcome.data.begin(), outcome.data.end());
                offset += chunkLength;
                continue;
            }

            if (outcome.status == IoReadStatus::Partial)
            {
                // data 是真实前缀：并入已收集的内容后立即停止，不再读后面的块。
                collected.insert(collected.end(), outcome.data.begin(), outcome.data.end());
                result.ok = true;
                result.partial = true;
                result.data = std::move(collected);
                result.bytesDone = static_cast<std::uint64_t>(result.data.size());
                return result;
            }

            // Unreadable / Failed：一个字节都不交给调用方，只报告原因。
            result.ok = false;
            result.failureText = outcome.failure;
            return result;
        }

        result.ok = true;
        result.data = std::move(collected);
        result.bytesDone = static_cast<std::uint64_t>(result.data.size());
        return result;
    }

    AccessResult MemoryIoByteStore::Write(
        const std::uint64_t address,
        const std::vector<std::uint8_t>& bytes,
        const bool explicitApproval)
    {
        // 空写入天然成功，不发起任何端口调用。
        if (bytes.empty())
        {
            AccessResult result;
            result.ok = true;
            return result;
        }

        if (IsKernelMutationRoute(address))
        {
            return WriteViaKernelMutation(address, bytes);
        }
        return WriteViaPort(address, bytes, explicitApproval);
    }

    AccessResult MemoryIoByteStore::WriteViaKernelMutation(
        const std::uint64_t address,
        const std::vector<std::uint8_t>& bytes)
    {
        AccessResult result;

        if (kernelMutationPort_ == nullptr)
        {
            // 没有端口，连 Read 都不必尝试——没有它分步事务根本跑不起来。
            result.failureText =
                "kernel virtual address write requires a kernel mutation port, but none is configured";
            return result;
        }

        // 先把整段写入范围的写前快照一次性读出来，交给 MemoryKernelMutation
        // 的回调去切片，避免每一片都单独发一次 IMemoryIoPort::Read。
        const AccessResult snapshot = Read(address, static_cast<std::uint64_t>(bytes.size()));
        if (!snapshot.ok)
        {
            result.failureText = snapshot.failureText.empty()
                ? std::string("kernel mutation write could not read a before-snapshot for the target range")
                : "kernel mutation write before-snapshot failed: " + snapshot.failureText;
            return result;
        }

        // snapshot.ok 为真时可能仍是 partial（data 只是前缀）；回调对"落在这份
        // 前缀之外"的切片一律报 Failed，交给 MemoryKernelMutation 按它自己的
        // "拿不到可信前置快照"失败路径处理，这里不臆造别的数据来源。
        const std::vector<std::uint8_t> beforeSnapshot = snapshot.data;
        const std::uint64_t validPrefixLength = static_cast<std::uint64_t>(beforeSnapshot.size());

        const KernelMutationReadBeforeFn readBefore =
            [&beforeSnapshot, validPrefixLength, address](
                const std::uint64_t sliceAddress, const std::uint64_t sliceLength) -> IoReadResult
        {
            IoReadResult outcome;
            const std::uint64_t sliceOffset = sliceAddress - address;
            if (sliceOffset + sliceLength <= validPrefixLength)
            {
                outcome.status = IoReadStatus::Ok;
                outcome.data.assign(
                    beforeSnapshot.begin() + static_cast<std::ptrdiff_t>(sliceOffset),
                    beforeSnapshot.begin() + static_cast<std::ptrdiff_t>(sliceOffset + sliceLength));
                return outcome;
            }
            outcome.status = IoReadStatus::Failed;
            outcome.failure = "kernel mutation write before-snapshot does not cover this slice";
            return outcome;
        };

        const KernelMutationResult mutationResult =
            WriteKernelBytes(*kernelMutationPort_, address, bytes, readBefore);

        // 直接搬运四个字段；这条路径不产生显式同意询问，needsExplicitApproval
        // 保持默认的 false——驱动内部固定带 FORCE|UI_CONFIRMED（旧行为）。
        result.ok = mutationResult.ok;
        result.bytesDone = mutationResult.bytesDone;
        result.rolledBack = mutationResult.rolledBack;
        result.failureText = mutationResult.failure;
        return result;
    }

    AccessResult MemoryIoByteStore::WriteViaPort(
        const std::uint64_t address,
        const std::vector<std::uint8_t>& bytes,
        const bool explicitApproval)
    {
        AccessResult result;

        const IoLimits limits = port_.Limits(session_);
        const std::uint64_t maxWriteBytes = limits.maxWriteBytes; // 0 = 不限

        std::uint64_t offset = 0;
        std::uint64_t totalDone = 0;
        std::string lastAnnotation; // 成功片里搬运的"回退/降级"一类注记文本。

        while (offset < bytes.size())
        {
            const std::uint64_t remaining = static_cast<std::uint64_t>(bytes.size()) - offset;
            const std::uint64_t chunkLength =
                (maxWriteBytes == 0) ? remaining : (std::min)(maxWriteBytes, remaining);
            const std::uint64_t chunkAddress = address + offset;
            const std::vector<std::uint8_t> chunk(
                bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                bytes.begin() + static_cast<std::ptrdiff_t>(offset) + static_cast<std::ptrdiff_t>(chunkLength));

            const IoWriteResult outcome = port_.Write(session_, chunkAddress, chunk, explicitApproval);
            // 只置位不清除：哪怕这一片之后整体停止，脏标记与窗口标记都已经是事实。
            result.scratchAreaDirty = result.scratchAreaDirty || outcome.scratchAreaDirty;
            result.readModifyWriteWindow = result.readModifyWriteWindow || outcome.readModifyWriteWindow;

            if (outcome.needsApproval)
            {
                // 约定：要求同意时端口必须尚未写入任何字节，所以 bytesDone 只
                // 计之前已经成功的片。
                result.ok = false;
                result.needsExplicitApproval = true;
                result.bytesDone = totalDone;
                result.failureText = outcome.failure;
                return result;
            }

            // ok 为真但 partial 也为真时，接口契约要求按失败处理（见
            // MemoryIoPort.h 的 IoWriteResult::partial 注释），不能当成"这一片
            // 写完了"继续切下一片。
            if (!outcome.ok || outcome.partial)
            {
                totalDone += outcome.bytesDone;
                result.ok = false;
                result.bytesDone = totalDone;
                result.partial = outcome.partial;
                result.rolledBack = outcome.rolledBack;
                result.failureText = outcome.failure;
                return result;
            }

            totalDone += outcome.bytesDone;
            if (!outcome.failure.empty())
            {
                lastAnnotation = outcome.failure;
            }
            offset += chunkLength;
        }

        result.ok = true;
        result.bytesDone = totalDone;
        result.failureText = lastAnnotation;
        return result;
    }
}
