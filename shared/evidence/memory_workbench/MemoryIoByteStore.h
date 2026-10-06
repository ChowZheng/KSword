#pragma once

// ============================================================
// MemoryIoByteStore.h
// 作用：
// - 实现 MemoryWriteTransaction.h 里的 IByteStore，是写事务管线真正触碰目标
//   内存的那一跳：把"按 Limits 切块读/写、按通道挑选内核分步事务还是直接
//   端口写"这些策略集中在这一个类里，写事务本身对此一无所知。
// - Qt-free、Win32-free，只经 IMemoryIoPort / IKernelMutationPort 做 I/O，
//   因此可以用脚本化假端口完整测试"按通道走哪条路、聚合哪些标志位"。
//
// ============================================================
// 旧 driverApplyMemoryDiffFromUi 各分支 → 新代码位置对照表
// ============================================================
// 旧实现（MemoryDock.DriverMemoryRw.cpp，约 1045-1340 行为写前复核与分发，
// 约 1380-1720 行为内核分步事务编排）按快照类型把一次"应用差异"分成五路：
//
//   旧分支                          新代码位置                保持语义？有意差异
//   ------------------------------  ------------------------  --------------------------------
//   物理快照，4KB 切片，无事务       本类 Write 的"其余范围/     保持：仍是逐片 port.Write、
//   （standard/HVM 后端）           通道"分支，交给              失败即停、不回滚；端口内部
//                                   IMemoryIoPort::Write        按 Limits.maxWriteBytes 切片，
//                                                               上限由物理写端口自己的 4KB
//                                                               决定，本类不重复写死这个数。
//   UserMode / Hvm 后端，            同上"其余范围/通道"分支      保持：仍是逐片写 + 回读核对
//   虚拟地址（含内核地址），                                     （回读核对现在在写事务管线的
//   writeVirtual 直写                                           (g) 步统一做，本类不重复读）。
//   DDMA 后端整条独立通路            同上"其余范围/通道"分支      保持：DDMA 的 force 确认、
//   （翻译 VA→PA 再 DMA）                                       scratchDirty、lostUpdateWindow
//                                                               全部通过 IoWriteResult 的同名
//                                                               字段逐片聚合，调用方式不变。
//   标准驱动通道 + 内核虚拟地址，    本类 Write 顶部的内核分支，   保持核心步骤（Prepare→DryRun→
//   KSWORD_ARK_MUTATION_* 分步事务   转给 MemoryKernelMutation    Force→ReadBack→失败则 Rollback）；
//                                                                有意差异：旧版失败整批回滚，
//                                                                新版只保证"本次 Write 调用内"
//                                                                按片回滚（用户已拍板，见
//                                                                MemoryKernelMutation.h 头注）。
//   标准驱动通道 + 用户态虚拟地址，  同上"其余范围/通道"分支      保持：写→回读核对两步，
//   普通 writeVirtualMemory                                     FORCE_REQUIRED→needsApproval
//                                                                的映射与旧版 confirmForce... 的
//                                                                交互意图一致（本类只负责把
//                                                                needsApproval 如实报出，重试
//                                                                同一次写入由写事务管线负责）。
//   权限类失败提示（promptForPrivilegeFailure /                  不保持：界面提示属于用户确认，
//   promptForPrivilegeNtStatus）                                 由 UI 层（WorkbenchConfirmations
//                                                                等，未来工作包）负责，本类只
//                                                                把 failureText 原样搬运上去。
//
// ============================================================
// 冻结接口摘要
// ============================================================
//   class MemoryIoByteStore : IByteStore
//     构造(IMemoryIoPort&, const MemoryTargetSession&, IKernelMutationPort* = nullptr)
//     Read(address,length) -> AccessResult
//     Write(address,bytes,approved) -> AccessResult
//
// ============================================================
// Read 的切块与映射规则
// ============================================================
// - length==0：不发起任何调用，返回 {ok=true}（空读取天然成功）。
// - 按 port.Limits(session).maxReadBytes 切块（0=不限，视整段为一块），依次
//   调用 port.Read；每一块都把 scratchAreaDirty / readModifyWriteWindow 累加
//   进结果（只置位不清除，哪怕这一块本身失败）。
// - 某一块返回 Ok：把它的数据追加进已收集的前缀，继续读下一块（若还有）。
// - 某一块返回非 Ok（Partial / Unreadable / Failed）：立即停止，不再读后面
//   的块：
//     Partial    -> {ok=true, partial=true, data=目前收集到的全部前缀,
//                    bytesDone=前缀长度}；
//     Unreadable / Failed -> {ok=false, failureText=该块的失败原因}（data 为
//                    空——调用方只在 ok 或 partial 为真时才能信任 data）。
// - 全部块都是 Ok：{ok=true, data=完整长度, bytesDone=length}。
//
// ============================================================
// Write 的分发与映射规则
// ============================================================
// - bytes 为空：不发起任何调用，返回 {ok=true, bytesDone=0}。
// - "内核范围 + 标准驱动通道"（session.channel==StandardDriver 且
//   session.scope==KernelVirtual 且 IsKernelVirtualAddress(address) 为真；
//   三者同时要求是为了不让物理地址的数值恰好落进内核半区被误判——物理范围
//   本不该走这条路）：
//     - kernelMutationPort_ 为空 -> {ok=false, failureText="..."}，一次 I/O
//       都不发起（没有端口，连 Read 都不必尝试）。
//     - kernelMutationPort_ 非空 -> 先调用本类自己的 Read(address,bytes.size())
//       取得写前整段快照；若该次 Read 连 ok 都不是，直接把它的失败原因转成
//       整体失败返回（没有任何可信的 expectedBefore，不进入分步事务）。
//       否则把这份快照包成一个只读切片回调交给
//       MemoryKernelMutation::WriteKernelBytes；回调对"落在快照有效前缀内"
//       的切片返回真实字节，对"超出前缀"的切片返回 Failed（即 Partial 快照
//       之后的部分一律视为拿不到可信前置快照，由 MemoryKernelMutation 按它
//       自己的失败路径处理，不在这里强行拼别的来源）。
//       KernelMutationResult -> AccessResult：ok/bytesDone/rolledBack/
//       failureText 直接搬运，needsExplicitApproval 恒为 false（这条路径的
//       同意语义是驱动内部的 FORCE|UI_CONFIRMED，不经写事务的同意询问）。
// - 其余范围 / 通道：按 port.Limits(session).maxWriteBytes 切片（0=不限，
//   整段一片），依次调用 port.Write(session, 片地址, 片字节, approved)：
//     - 某片 needsApproval 为真 -> 立即停止；此前没有持久写入时返回
//       needsExplicitApproval=true，允许批准后重试整块；此前已写入时返回
//       partial=true 与已写字节数，不允许整块重试，宿主必须重读核对。
//     - 某片 ok 为假（且非 needsApproval）-> 停止，{ok=false,
//       bytesDone=此前成功片加当前片未回滚的实际字节；只有此前没有写入
//       且当前片已回滚，rolledBack 才为真，避免把局部回滚说成整体回滚。
//     - 某片成功 -> 累加 bytesDone，继续下一片；scratchAreaDirty /
//       readModifyWriteWindow 全程只置位不清除；该片的 failureText 若非空
//       （端口用来搬运"回退/降级"注记）保留为当前候选注记文本。
//   全部片成功 -> {ok=true, bytesDone=总字节数, scratchAreaDirty /
//   readModifyWriteWindow 聚合结果, failureText=最后一次非空的注记（若有，
//   否则为空）}。
// ============================================================

#include "MemoryIoPort.h"
#include "MemoryTargetSession.h"
#include "MemoryWriteTransaction.h"

#include <cstdint>
#include <vector>

namespace ksword::memwb
{
    // MemoryIoByteStore：写事务的真实读写落点，规则见文件头。
    class MemoryIoByteStore final : public IByteStore
    {
    public:
        // 构造。
        // 传入：port 真实或假的 I/O 端口；session 当前目标会话（只读引用，权威
        //       副本归调用方）；kernelMutationPort 内核分步字节事务端口，可为
        //       空——为空时"内核范围+标准驱动通道"的写入会直接失败并说明原因，
        //       不会退回到别的写法（不自动降级，呼应设计文档不变式 4）。
        // 引用/指针必须比本对象活得久。
        MemoryIoByteStore(
            IMemoryIoPort& port,
            const MemoryTargetSession& session,
            IKernelMutationPort* kernelMutationPort = nullptr);

        MemoryIoByteStore(const MemoryIoByteStore&) = delete;
        MemoryIoByteStore& operator=(const MemoryIoByteStore&) = delete;

        // Read：见文件头"Read 的切块与映射规则"。
        AccessResult Read(std::uint64_t address, std::uint64_t length) override;

        // Write：见文件头"Write 的分发与映射规则"。
        AccessResult Write(
            std::uint64_t address,
            const std::vector<std::uint8_t>& bytes,
            bool explicitApproval) override;

    private:
        // WriteViaKernelMutation：内核范围+标准驱动通道分支的实现，被 Write 调用。
        AccessResult WriteViaKernelMutation(std::uint64_t address, const std::vector<std::uint8_t>& bytes);

        // WriteViaPort：其余范围/通道分支的实现，被 Write 调用。
        AccessResult WriteViaPort(
            std::uint64_t address,
            const std::vector<std::uint8_t>& bytes,
            bool explicitApproval);

        // IsKernelMutationRoute：判断这次写入是否应该走内核分步事务分支。
        bool IsKernelMutationRoute(std::uint64_t address) const;

        // port_：真实或假的 I/O 端口。
        IMemoryIoPort& port_;
        // session_：当前目标会话（只读）。
        const MemoryTargetSession& session_;
        // kernelMutationPort_：内核分步字节事务端口，可为空。
        IKernelMutationPort* kernelMutationPort_;
    };
}
