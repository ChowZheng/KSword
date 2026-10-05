#pragma once

// ============================================================
// WorkbenchIoMapping.h
// 作用：
// - M-1（主会话审核拆分）：把 WorkbenchIoPorts.cpp/.Kernel.cpp 里最容易出错
//   的"结果映射"收进这一个独立文件：MapFacadeReadOutcome、
//   MapFacadeWriteOutcome、MapStandardDriverVirtualRead 三个纯函数。
// - 三者都是**纯函数**：只读取 ksword::memory_backend::AccessOutcome 或
//   ksword::ark::VirtualMemoryReadResult 的字段取值，翻译成 Qt-free 的
//   IoReadResult/IoWriteResult；不发起任何 Win32 调用，不调用门面
//   （ksword::memory_backend::readPhysical/writeVirtual/...）或驱动客户端
//   （ksword::ark::DriverClient 的任何方法）。正因为"纯"，才能用手工构造的
//   结构体离线穷举每一个分支，不需要真的打开进程、打开驱动句柄——
//   tools/memwb_ui/memwb_ui_tests.IoMapping.cpp 就是这样测它的。
// - WorkbenchIoPorts.cpp（真实调门面的那一跳）与 WorkbenchIoPorts.Kernel.cpp
//   （真实调驱动客户端的那一跳）只负责"问一次"，问完把原始结果转手交给这里
//   的函数翻译，不在各自文件里各写一份判据。
// - 依据：docs/内存工作台Phase3集成设计.md 第 0 节第 16/17 条；本次任务下发
//   的【已核实的后端事实】FACTS 2/4/5 条（与 WorkbenchIoPorts.h 共用同一份
//   编号）。
//
// ============================================================
// 冻结接口摘要（本文件新增的三个纯函数；改名或改语义须先同步通知）
// ============================================================
//   namespace ksword::memwb_ports_detail
//   IoReadResult  MapFacadeReadOutcome(const AccessOutcome&)
//   IoWriteResult MapFacadeWriteOutcome(const AccessOutcome&)
//   IoReadResult  MapStandardDriverVirtualRead(const VirtualMemoryReadResult&)
//
// ============================================================
// R-1 修复记录（主会话审核发现，2026-10）
// ============================================================
// MapStandardDriverVirtualRead 原来在 readStatus==OK 时直接把 data 整段搬进
// 结果报 Ok，没有核对 data.size() 是否真的等于 requestedBytes。真实驱动如果
// 因为响应缓冲不足等原因把响应截断，会出现"readStatus==OK 但 data 偏短"这种
// 协议上不该有、但调用方必须能分辨的组合——旧实现会把这份偏短的数据当成
// "整段都读到了"直接报 Ok，上层（MemoryPageReader）按 Ok 分支把前缀之后的
// 内容也标成 Valid，等于伪造了一段从未真实读到的数据。现在按长度分三支：
//   data.size()==requestedBytes            -> Ok；
//   data.size()<requestedBytes 且非空       -> Partial（data 即真实前缀，
//                                              failure 说明"响应被截断"）；
//   data 为空                               -> Failed（协议层异常响应，不是
//                                              "目标不可读"，不能报 Unreadable
//                                              也不能报 Ok，Ok 要求
//                                              data.size()==length 这个契约
//                                              满足不了）；
//   data.size()>requestedBytes（防御性，协议不应出现）仍按 Ok 处理但裁到
//   requestedBytes，保持 IoReadResult 的契约。
// PARTIAL_COPY 分支本来就不看 data 与 requestedBytes 的长度关系，只看
// data 是否非空，因此"PARTIAL_COPY 且 data.size()>=requestedBytes"这个协议上
// 矛盾的组合保持原样报 Partial，不在这次修复范围内上调成 Ok。
// ============================================================

#include "MemoryAccessBackend.h"
#include "../ArkDriverClient/ArkDriverTypes.h"
#include "../../../shared/evidence/memory_workbench/MemoryIoPort.h"

namespace ksword::memwb_ports_detail
{
    // MapFacadeReadOutcome：
    // - 作用：把门面 AccessOutcome 映射为 IoReadResult，对应 FACTS 第 4 条。
    // - 判据（按顺序）：
    //     1. scratchAreaDirty 为真 -> Failed（暂存区未还原，必须停止后续
    //        读取；这一条优先于下面的 ok/partial 判断，因为暂存区弄脏时
    //        outcome.ok 仍可能是 true，不能被当作成功）。
    //     2. ok 且非 partial -> Ok。
    //     3. (ok 且 partial) 或 (!ok 且 data 非空) -> Partial，data 即前缀。
    //     4. 其余（!ok 且 data 为空） -> Unreadable。
    //   "磁盘传输会话不可用"这一条不在本函数里判断——它必须在调用门面之前
    //   作为前置闸门判断（见 WorkbenchIoPort::ReadViaFacade），因为门面返回
    //   的 AccessOutcome 无法区分"会话不可用"与"目标本身读不到"，事后已经
    //   看不出来了。
    // - 私有页表窗口（Hvm）未走成直接窗口时，门面会 ok=true 并在
    //   failureText 里带"回退"注记；这段文本在 Ok/Partial 分支原样保留，
    //   是注记不是错误（P-1：MemoryPageReader 会把它聚合进 PageReadResult::note）。
    // - 调用方法：传入 ksword::memory_backend 门面 readPhysical/readVirtual
    //   等调用的原始返回值。
    ksword::memwb::IoReadResult MapFacadeReadOutcome(
        const ksword::memory_backend::AccessOutcome& outcome);

    // MapFacadeWriteOutcome：
    // - 作用：把门面 AccessOutcome 映射为 IoWriteResult，对应 FACTS 第 5 条。
    // - 全部字段都是忠实搬运：forceRequired->needsApproval；scratchDirty、
    //   lostUpdateWindow、partial、bytesDone、failureText 原样复制。
    // - rolledBack 恒为 false：门面的物理/虚拟写从不自动回滚失败的写入
    //   （物理写按设计保持旧行为，失败不回滚；虚拟写同理），只有经
    //   WorkbenchKernelMutationPort 的分步事务才会真正回滚。
    // - 调用方法：传入 ksword::memory_backend 门面 writePhysical/writeVirtual
    //   等调用的原始返回值。
    ksword::memwb::IoWriteResult MapFacadeWriteOutcome(
        const ksword::memory_backend::AccessOutcome& outcome);

    // MapStandardDriverVirtualRead：
    // - 作用：把"标准驱动通道、不带 ZERO_FILL_UNREADABLE 标志"的虚拟内存读取
    //   结果映射为 IoReadResult；被 WorkbenchIoPort::Read 的标准驱动分支与
    //   WorkbenchKernelMutationPort::ReadBack 共用——两者发起的是同一种调用
    //   （区别只在 processId 与 KERNEL_ADDRESS 标志的取值），映射规则理应
    //   只有一份，不在两个文件里各写一遍。
    // - 判据（FACTS 第 2 条，已用 process_memory.c 第 890-915 行核实；长度
    //   三支见本文件头 R-1 修复记录）：
    //     io.ok==false                              -> Failed（通道通信失败）
    //     readStatus==OK 且 data.size()==requestedBytes -> Ok
    //     readStatus==OK 但 data 偏短且非空             -> Partial（R-1）
    //     readStatus==OK 但 data 为空                   -> Failed（R-1）
    //     readStatus==PARTIAL_COPY 且 data 非空          -> Partial（data 为
    //                                                      真实前缀，不看
    //                                                      长度与 requestedBytes
    //                                                      的关系）
    //     readStatus==PARTIAL_COPY 但 data 为空          -> Unreadable（防御性
    //                                                      兜底，理论上不会
    //                                                      出现，但不能把空
    //                                                      前缀说成"部分成功"）
    //     readStatus==COPY_FAILED（bytesRead==0）        -> Unreadable（目标
    //                                                      不可读）
    //     其余 readStatus（协议不匹配/范围被拒等）         -> Failed
    // - 调用方法：传入 ksword::ark::DriverClient::readVirtualMemory 的原始
    //   返回值。
    ksword::memwb::IoReadResult MapStandardDriverVirtualRead(
        const ksword::ark::VirtualMemoryReadResult& driverResult);
}
