#pragma once

// ============================================================
// MemoryKernelMutation.h
// 作用：
// - 对 IKernelMutationPort 复刻旧编排"Prepare -> DryRun -> Force -> ReadBack
//   ->（失败则 Rollback）"的分步字节事务流程（见内存工作台 Phase3 集成设计
//   第 0 节第 16 条、第 3 节不变式 7）。旧实现在 MemoryDock.DriverMemoryRw.cpp
//   的 driverApplyMemoryDiffFromUi 里按内核地址快照整批编排；本文件把它收敛成
//   一个与 Qt / Win32 无关的纯函数，供 MemoryIoByteStore（下一层）调用。
// - 用户已拍板的设计差异：旧版失败时整批回滚；新实现只保证"单次 Write 调用
//   内"按片回滚——已提交且回读验证通过的片不会被这次失败连带撤销，只有仍在
//   本次调用回滚范围内、且确实失败的片才会被尝试撤销。
//
// 为什么不直接把这段逻辑写进 MemoryIoByteStore：
// - 内核分步事务的失败点多（Prepare / DryRun / Force / ReadBack 四步，外加
//   回滚阶段的再次校验），独立成一个函数才能让每个失败点单独起一个假端口
//   脚本来穷举测试，不必每次都搭一整套 IByteStore 环境。
//
// 每片上限 64 字节：这是协议常量（shared/driver/KswordArkMutationIoctl.h 的
// KSWORD_ARK_MUTATION_MAX_BYTES），本文件不包含该头（它会拉 Windows 的
// CTL_CODE 宏，不是纯标准库），只在这里复述数值并作为 kKernelMutationSliceBytes
// 的来源说明；真正的协议校验发生在真实端口实现里，本文件只负责按这个上限切片。
//
// 为什么 Prepare 需要一个"读取当前字节"的回调而不是直接调用
// IKernelMutationPort::ReadBack：
// - 调用方（MemoryIoByteStore）通常已经在 Write 开始时为了算 expectedBefore
//   一次性读过了整段写入范围（走 IMemoryIoPort::Read，可能带着端口自己的
//   Limits 切块），没必要为每一片再单独发一次请求。回调的签名与
//   IKernelMutationPort::ReadBack 一致，调用方可以传入"切一段已经读到的缓冲区"
//   的轻量闭包，也可以真的转发到 ReadBack——本文件不关心实现，只关心这次
//   调用是否返回了这一片足够长度的真实前缀。
// - 回调返回非 Ok、或返回的数据不足一整片时，视为"这一片拿不到可信的写前
//   快照"，整体按失败处理（不会凭着不完整的 expectedBefore 去 Prepare）。
// - 这与 ForceCommit 之后的回读验证、以及回滚之后的复核验证完全不同：那两处
//   必须是对目标的一次**实时**读取（走 IKernelMutationPort::ReadBack），因为
//   它们要核对的是"刚刚写完/刚刚回滚完之后目标上真正有什么"，不能用写之前
//   缓存的快照顶替。本文件内部对这两处统一直接调用 port.ReadBack，不复用
//   传入的回调。
//
// ============================================================
// 冻结接口摘要
// ============================================================
//   constexpr kKernelMutationSliceBytes == 64
//   using KernelMutationReadBeforeFn = std::function<IoReadResult(uint64_t,uint64_t)>
//   struct KernelMutationResult{ok,bytesDone,rolledBack,rollbackVerifiedBytes,
//                                 rollbackFailedCount,failure}
//   WriteKernelBytes(IKernelMutationPort&, address, bytes, readBefore) -> KernelMutationResult
//
// ============================================================
// 算法（严格按此顺序，不得"优化"成别的顺序）
// ============================================================
// bytes 为空：什么都没做，返回 {ok=true, bytesDone=0, rolledBack=false}。
// 否则按 kKernelMutationSliceBytes（64 字节）把 bytes 切成若干片，按地址升序
// 逐片处理，任一片失败立即停止（不处理后面的片）：
//   1. 调用 readBefore(sliceAddress, sliceLength) 取得这一片的写前字节。
//      非 Ok，或 data.size() != sliceLength，视为失败（这一片未曾进入 Prepare，
//      不计入回滚列表），记录失败原因，跳到第 6 步。
//   2. Prepare(sliceAddress, after=这一片的新字节, expectedBefore=第1步的结果)。
//      校验：prep.ok、prep.transactionId!=0、prep.beforeBytes.size()>=片长、
//      prep.beforeBytes 的前 片长 字节与 expectedBefore 逐字节相等。四项任一
//      不满足都是"Prepare 返回校验不符"，未通过校验的这一片不计入回滚列表
//      （Prepare 没有产出一个我们信任的事务号），记录失败原因，跳到第 6 步。
//   3. 校验通过：把 {transactionId, sliceAddress, expectedBefore} 记入回滚列表
//      （即使后面的步骤失败，这一片也已经记进去了，因为 Prepare 本身已经
//      在后端创建了事务）。
//   4. DryRunCommit(transactionId)。失败则记录原因，跳到第 6 步。
//   5. ForceCommit(transactionId)。失败则记录原因，跳到第 6 步。
//      成功后 ReadBack(sliceAddress, sliceLength) 核对是否等于 after（长度与
//      逐字节都要相等）。不等则记录原因，跳到第 6 步。
//      全部通过：进入下一片（回到第 1 步，地址前移一片长度）。
//   6.（仅失败路径，复刻旧编排 MemoryDock.DriverMemoryRw.cpp 第 1653-1715 行
//      的顺序——那里对每一片先回读、已经等于写前字节就不调用回滚）按回滚
//      列表的**逆序**逐条处理：先调用 ReadBack(address, 片长) 核对目标是否
//      已经等于 expectedBefore；已经相等就直接计为"已恢复"，**不调用
//      Rollback**（这一片可能从未真正落地，调一次没意义的 Rollback 只会
//      多一次不必要的 R0 往返）；不相等才调用 Rollback(transactionId)，
//      不管它的返回值如何，紧接着再调用一次 ReadBack 独立核对。两种路径
//      核对通过都计入 rollbackVerifiedBytes，不通过都计入
//      rollbackFailedCount 并把这一片的字节数计入 bytesDone（"回滚后仍然
//      留在目标上的字节数"）。
//      记录每一片是否真的到过 ForceCommit 阶段（即第 5 步是否被调用过，
//      不看它成不成功——第 0 节第 16 条与本文件头都强调过，失败的提交也
//      可能已部分落地，所以"到过"本身就值得单独记）。
//      最终 rolledBack = （回滚列表里**至少有一片到过 ForceCommit 阶段**）
//      且 rollbackFailedCount==0；回滚列表为空、或列表里没有任何一片到过
//      ForceCommit（例如 Prepare 之后 DryRun 就失败了）时恒为 false——
//      因为这种失败从未真正改动过目标，谈不上"已回滚"，哪怕凑巧也调用了
//      一次 Rollback 并核对通过。
//
// 地址溢出：address + bytes.size() 超过 uint64 上限时，整体拒绝（ok=false，
// bytesDone=0，rolledBack=false），不发起任何调用。
//
// 线程模型：非线程安全，纯函数式使用；调用方在同一线程调用。
// C++20、Qt-free、Win32-free。
// ============================================================

#include "MemoryIoPort.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ksword::memwb
{
    // kKernelMutationSliceBytes：内核分步字节事务每一片的最大长度，数值取自
    // 协议常量 KSWORD_ARK_MUTATION_MAX_BYTES（见本文件头说明），不包含该头。
    inline constexpr std::uint64_t kKernelMutationSliceBytes = 64ULL;

    // KernelMutationReadBeforeFn：取得某一片写前字节的回调，签名与
    // IKernelMutationPort::ReadBack 一致。调用方法：
    // readBefore(sliceAddress, sliceLength) -> IoReadResult；status!=Ok 或
    // data 长度不足 sliceLength 都会被当作"这一片拿不到可信快照"处理。
    using KernelMutationReadBeforeFn =
        std::function<IoReadResult(std::uint64_t address, std::uint64_t length)>;

    // KernelMutationResult：WriteKernelBytes 的返回值。默认是最安全的
    // "什么都没发生"：ok=false（调用方不应把默认值当成功读），bytesDone=0。
    struct KernelMutationResult
    {
        // ok：全部片都 Prepare/DryRun/Force/ReadBack 通过，整段已落地。
        bool ok = false;
        // bytesDone：失败后仍然留在目标上的字节数；ok 为真时等于 bytes.size()，
        // 失败且已全部回滚核对通过时为 0，回滚有失败片时为那些片的字节数之和。
        std::uint64_t bytesDone = 0;
        // rolledBack：是否"至少有一片真的到过 ForceCommit 阶段（无论
        // ForceCommit 本身成功与否——失败的提交也可能已部分落地），且回滚
        // 列表里全部片回滚核对成功"。没有任何片进入过回滚列表、或进了列表
        // 但从未到过 ForceCommit（例如 Prepare 之后 DryRun 就失败）时恒为
        // 假——这种失败没有真正改动过目标，不存在"已回滚"这件事。
        bool rolledBack = false;
        // rollbackVerifiedBytes：回滚阶段里核对确认已恢复为写前字节的总字节数。
        std::uint64_t rollbackVerifiedBytes = 0;
        // rollbackFailedCount：回滚阶段里核对后仍不是写前字节的片数。
        std::uint64_t rollbackFailedCount = 0;
        // failure：触发整体失败的那一步的原因细节串（英文，细节串而非用户句子）；
        // ok 为真时恒为空。
        std::string failure;
    };

    // WriteKernelBytes：对一段内核虚拟地址字节执行分步字节事务，算法见文件头。
    // 调用方法：port 已绑定到目标驱动连接；address 起始内核虚拟地址；bytes 想
    // 写入的新字节（可以为空）；readBefore 取得写前快照的回调。
    // 传出：KernelMutationResult，见上。
    KernelMutationResult WriteKernelBytes(
        IKernelMutationPort& port,
        std::uint64_t address,
        const std::vector<std::uint8_t>& bytes,
        const KernelMutationReadBeforeFn& readBefore);
}
