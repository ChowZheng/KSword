// MemoryKernelMutation.h 的实现。只负责"怎么切片、怎么校验每一步、什么时候
// 回滚"，不碰任何真实 I/O（全部经 IKernelMutationPort 完成），因此可以用脚本化
// 假端口离线穷举测试。实现细节全部对应文件头"算法"一节逐条展开，改动前请先
// 回去确认没有偏离那一节描述的行为。

#include "MemoryKernelMutation.h"

#include <algorithm>
#include <limits>

namespace ksword::memwb
{
    namespace
    {
        // CommittedSlice：一片已经通过 Prepare 校验、因此必须纳入回滚考虑的记录。
        // 即使这一片后面的 DryRun/Force/ReadBack 失败了，它也会出现在这里——
        // 因为 Prepare 已经在后端创建了事务，必须尝试把它收回去。
        struct CommittedSlice
        {
            // transactionId：Prepare 返回的事务号，Rollback 要带着它。
            std::uint64_t transactionId = 0;
            // address：这一片的起始地址。
            std::uint64_t address = 0;
            // expectedBefore：这一片的写前字节，回滚核对时用它比对回读结果。
            std::vector<std::uint8_t> expectedBefore;
            // reachedForceCommit：这一片是否真的调用过 ForceCommit（第 5
            // 步），不看调用结果成不成功——只要调用过，就说明这一片存在
            // "可能已经落地"的风险，必须参与 rolledBack 的判定；从未调用过
            // （DryRun 就失败了）的片，目标上理应还是写前字节，不该把它计
            // 入"已经回滚过东西"。
            bool reachedForceCommit = false;
        };

        // RangeOverflows：判断 address+length 是否超过 uint64 上限。
        // 调用方法：address、length 均为无符号 64 位；返回 true 表示这段范围
        // 无法用 [address, address+length) 表示，调用方必须整体拒绝。
        bool RangeOverflows(const std::uint64_t address, const std::uint64_t length)
        {
            constexpr std::uint64_t kMaxAddress = (std::numeric_limits<std::uint64_t>::max)();
            return length > 0 && address > kMaxAddress - length;
        }

        // BytesEqualPrefix：判断 data 的前 length 字节是否与 expected（整段）逐
        // 字节相等；data 长度不足 length 时直接判不等。
        bool BytesEqualPrefix(
            const std::vector<std::uint8_t>& data,
            const std::uint64_t length,
            const std::vector<std::uint8_t>& expected)
        {
            if (data.size() < static_cast<std::size_t>(length)
                || expected.size() != static_cast<std::size_t>(length))
            {
                return false;
            }
            return std::equal(expected.begin(), expected.end(), data.begin());
        }

        // RunRollback：对 committed 列表按逆序逐条核对/回滚，把结果汇总进
        // result。即使 committed 为空，这个函数也能安全调用（两个循环体都
        // 不会执行一次），调用方不需要单独判空。
        //
        // 每一片的处理顺序复刻旧编排（MemoryDock.DriverMemoryRw.cpp 第
        // 1660-1708 行）：先回读一次，已经等于 expectedBefore 就直接算
        // "已恢复"，不调用 Rollback；不相等才调用 Rollback（返回值不被
        // 信任，只认紧跟着的第二次回读）。这与旧实现"无条件先 Rollback
        // 再回读"不同——旧实现对一个本来就没被改动过的片也会白白发一次
        // Rollback IOCTL。
        void RunRollback(
            IKernelMutationPort& port,
            const std::vector<CommittedSlice>& committed,
            KernelMutationResult& result)
        {
            // anyReachedForceCommit：回滚列表里是否至少有一片真的调用过
            // ForceCommit。这是判定整体 rolledBack 的前提——参见文件头
            // 算法第 6 步与 KernelMutationResult::rolledBack 的字段注释。
            bool anyReachedForceCommit = false;
            for (const CommittedSlice& slice : committed)
            {
                if (slice.reachedForceCommit)
                {
                    anyReachedForceCommit = true;
                    break;
                }
            }

            for (auto iterator = committed.rbegin(); iterator != committed.rend(); ++iterator)
            {
                const CommittedSlice& slice = *iterator;
                const std::uint64_t sliceLength =
                    static_cast<std::uint64_t>(slice.expectedBefore.size());

                // 第一次回读：目标可能从未被真正改动过（ForceCommit 没被
                // 调用，或调用了但其实没落地），这种情况下目标本来就等于
                // expectedBefore，不必再多发一次 Rollback。
                IoReadResult check = port.ReadBack(slice.address, sliceLength);
                bool restored = check.status == IoReadStatus::Ok
                    && BytesEqualPrefix(check.data, sliceLength, slice.expectedBefore);

                if (!restored)
                {
                    // 确实不一样，才需要真的回滚。Rollback 的返回值不被
                    // 信任：它只是"后端认为自己回滚成功了"，真正的事实来自
                    // 紧接着这一次独立回读。
                    (void)port.Rollback(slice.transactionId);
                    check = port.ReadBack(slice.address, sliceLength);
                    restored = check.status == IoReadStatus::Ok
                        && BytesEqualPrefix(check.data, sliceLength, slice.expectedBefore);
                }

                if (restored)
                {
                    result.rollbackVerifiedBytes += sliceLength;
                }
                else
                {
                    ++result.rollbackFailedCount;
                    result.bytesDone += sliceLength;
                }
            }

            // 只有"确实有片到过 ForceCommit 阶段、且逐片核对全部通过"才算
            // 真正的"已回滚"；committed 为空或没有任何片到过 ForceCommit 时
            // anyReachedForceCommit 恒为假，这里天然算出 false。
            result.rolledBack = anyReachedForceCommit && result.rollbackFailedCount == 0;
        }
    } // namespace

    KernelMutationResult WriteKernelBytes(
        IKernelMutationPort& port,
        const std::uint64_t address,
        const std::vector<std::uint8_t>& bytes,
        const KernelMutationReadBeforeFn& readBefore)
    {
        KernelMutationResult result;

        // 空写入：什么都没做，安全地报告成功。
        if (bytes.empty())
        {
            result.ok = true;
            return result;
        }

        if (RangeOverflows(address, static_cast<std::uint64_t>(bytes.size())))
        {
            result.failure = "kernel mutation address range exceeds the 64-bit address space";
            return result;
        }

        std::vector<CommittedSlice> committed;
        std::size_t offset = 0;
        bool failed = false;

        while (offset < bytes.size() && !failed)
        {
            const std::uint64_t sliceLength = (std::min<std::uint64_t>)(
                kKernelMutationSliceBytes,
                static_cast<std::uint64_t>(bytes.size() - offset));
            const std::uint64_t sliceAddress = address + static_cast<std::uint64_t>(offset);
            const std::vector<std::uint8_t> after(
                bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                bytes.begin() + static_cast<std::ptrdiff_t>(offset) + static_cast<std::ptrdiff_t>(sliceLength));

            // 第 1 步：取这一片的写前快照。拿不到完整长度的真实前缀就不能继续
            // 往下 Prepare——那样等于拿着不可信的 expectedBefore 去骗后端。
            const IoReadResult beforeRead = readBefore(sliceAddress, sliceLength);
            if (beforeRead.status != IoReadStatus::Ok
                || beforeRead.data.size() != static_cast<std::size_t>(sliceLength))
            {
                result.failure = "kernel mutation could not obtain a trusted before-snapshot for this slice";
                failed = true;
                break;
            }
            const std::vector<std::uint8_t> expectedBefore = beforeRead.data;

            // 第 2 步：Prepare，并对返回值做四项校验（状态/事务号/长度/before 一致）。
            const MutationPrepareResult prepared = port.Prepare(sliceAddress, after, expectedBefore);
            const bool prepareValid = prepared.ok
                && prepared.transactionId != 0
                && BytesEqualPrefix(prepared.beforeBytes, sliceLength, expectedBefore);
            if (!prepareValid)
            {
                result.failure = prepared.failure.empty()
                    ? std::string("kernel mutation prepare result failed validation "
                        "(status/transaction id/length/before mismatch)")
                    : "kernel mutation prepare failed: " + prepared.failure;
                failed = true;
                break;
            }

            // 第 3 步：校验通过，必须记入回滚列表——哪怕接下来的步骤失败。
            CommittedSlice slice;
            slice.transactionId = prepared.transactionId;
            slice.address = sliceAddress;
            slice.expectedBefore = expectedBefore;
            committed.push_back(std::move(slice));

            // 第 4 步：DryRunCommit。
            const MutationStepResult dryRun = port.DryRunCommit(prepared.transactionId);
            if (!dryRun.ok)
            {
                result.failure = dryRun.failure.empty()
                    ? std::string("kernel mutation dry-run commit failed")
                    : "kernel mutation dry-run commit failed: " + dryRun.failure;
                failed = true;
                break;
            }

            // 第 5 步：ForceCommit。调用之前先标记"这一片到过 ForceCommit
            // 阶段"——不管接下来调用成功与否都要标记，因为失败的提交也可能
            // 已经部分落地（回滚判定 RunRollback 靠这个字段知道要不要把
            // 这一片计入 rolledBack 的判定范围）。成功后立即回读核对与
            // after 一致。
            committed.back().reachedForceCommit = true;
            const MutationStepResult forced = port.ForceCommit(prepared.transactionId);
            if (!forced.ok)
            {
                result.failure = forced.failure.empty()
                    ? std::string("kernel mutation force commit failed")
                    : "kernel mutation force commit failed: " + forced.failure;
                failed = true;
                break;
            }

            const IoReadResult verify = port.ReadBack(sliceAddress, sliceLength);
            const bool verified = verify.status == IoReadStatus::Ok
                && BytesEqualPrefix(verify.data, sliceLength, after);
            if (!verified)
            {
                result.failure = "kernel mutation verify read-back did not match the written bytes";
                failed = true;
                break;
            }

            offset += static_cast<std::size_t>(sliceLength);
        }

        if (!failed)
        {
            result.ok = true;
            result.bytesDone = static_cast<std::uint64_t>(bytes.size());
            return result;
        }

        // 第 6 步：失败路径，按逆序回滚已记录的片并逐片核对。
        RunRollback(port, committed, result);
        return result;
    }
}
