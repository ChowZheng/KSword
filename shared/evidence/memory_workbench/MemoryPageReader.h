#pragma once

// ============================================================
// MemoryPageReader.h
// 作用：
// - 把"给定一段页范围，去 IMemoryIoPort 上读出来"这件事的全部策略收在一处：
//   按端口上限分块、块内遇到部分读/不可读按页二分继续、遇到通道失败或暂存区
//   弄脏立即整体停止、取消标志每次 Read 之前检查。调用方（WorkbenchPageProvider，
//   后续工作包）只负责把结果喂回 HexViewport 的页缓存，不自己判断"要不要重试"。
// - 纯 C++20 标准库实现，不依赖 Qt、不依赖 Win32，可以用脚本化假端口
//   （KswordARKLightTests/MemoryIoTestSupport.h）完整测试"不探测、不重读"
//   这类必须靠调用序列才能钉住的行为。
//
// ============================================================
// 冻结接口摘要
// ============================================================
//   enum class PageState{Valid,PartiallyValid,Unreadable,NotAttempted}
//   struct PageRecord{pageStart,bytes(4096),valid(4096),state}
//   struct PageReadResult{pages,failure,channelFailed,cancelled,
//                          scratchAreaDirty,readModifyWriteWindow,portCalls,
//                          note,unreadableReason}
//   ReadPages(port, session, firstPageStart, pageCount, cancel) -> PageReadResult
//
// ============================================================
// 算法（调研确认的行为，必须严格如此，不得"优化"成别的顺序）
// ============================================================
// 1. 按 port.Limits(session).maxReadBytes 把 [firstPageStart, firstPageStart +
//    pageCount*4096) 切成若干"块"：每块是整页的倍数，且至少 1 页；
//    maxReadBytes==0（不限）时整段range是一块。块的边界只在切分时算一次，
//    之后块内的续读绝不会超出本块原定的边界去借用下一块的配额。
// 2. 对每一块：发起一次 Read 覆盖块的（当前未处理）剩余部分，按返回状态处理：
//      Ok         -> 整块（剩余部分）标为 Valid，块处理完毕；
//      Partial    -> data 的前缀字节标 Valid；前缀结束所在的那一页，从前缀结束点
//                    到页尾标 Unreadable（不去探测页内还剩多少能读，不重读这一页）；
//                    从**下一页**起，对块里剩下的部分再发一次 Read，继续本步骤；
//      Unreadable -> 本次 Read 请求的**第一页**整页标 Unreadable；从下一页起对
//                    块里剩下的部分再发一次 Read，继续本步骤；
//      Failed     -> 当前位置到**整段 range 末尾**（本块剩余部分 + 后面所有块）
//                    全部标为 NotAttempted，记录 failure，整体停止（不再处理
//                    后面的块）。
//    每次 Read 的 readModifyWriteWindow 都累加进结果（逻辑或），不影响是否停止。
//    每次 Read 之后检查 scratchAreaDirty：为真则累加进结果，并整体停止（当前
//    位置之后、含本块剩余与后面所有块，全部 NotAttempted），因为暂存区没还原
//    时继续读只会扩大污染。
//    注记聚合（P-1，供状态条解释"这次读取发生了什么"）：Ok/Partial 结果里
//    非空的 failure 文本是"注记"而不是错误（例如私有页表窗口回退到普通路径
//    时门面带的"已回退"说明），按出现顺序去重后累加进 note，最多保留 3 条，
//    用"；"连接；Unreadable 结果里第一个非空 failure 文本记进
//    unreadableReason（只取第一个，供状态条解释"为什么这页是 ??"）；Failed
//    的 failure 只写顶层 result.failure，不混入 note——那是整体停止的原因，
//    不是"顺便搬运的说明"。
// 3. 每次发起 Read 之前先检查 cancel：非空且已置位，则当前位置到整段 range
//    末尾全部 NotAttempted、cancelled=true，整体停止（这次不会再发起 Read）。
// 4. 地址溢出保护：若 [firstPageStart, firstPageStart + pageCount*4096) 的末尾
//    会超过 2^64-1（即发生回绕），整体按 Failed 处理且**不枚举任何页**
//    （pages 为空、channelFailed=true），因为回绕后的页起始地址本身就是假的，
//    不能把假地址放进结果里。
// 5. pageCount==0 时什么都不做，返回"什么都没发生"的空结果（pages 为空，
//    其余字段都是默认的安全初值）。
//
// 页记录的四种状态与"字节是否可信"严格绑定：
//   Valid         这一页全部 4096 字节都真实读到，bytes/valid 全部有意义；
//   PartiallyValid这一页部分字节真实读到（valid[i]==1 才可信），其余为 0/0；
//   Unreadable    这一页一个字节都没读到，bytes 全 0、valid 全 0；
//   NotAttempted  这一页从未发起过读取（取消/通道失败/溢出导致），bytes 全 0、
//                 valid 全 0——与 Unreadable 的区别只在"有没有问过"，调用方
//                 展示文案不同，但两者都绝不能把 bytes 当真实数据。
//
// 线程模型：非线程安全。调用方在读线程里单线程调用（设计文档第 3 节不变式 5：
// 读线程是一条串行池，只持值），结果值类型整体搬回 UI 线程使用。
// C++20、Qt-free、Win32-free。
// ============================================================

#include "HexViewport.h"
#include "MemoryIoPort.h"
#include "MemoryTargetSession.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace ksword::memwb
{
    // PageState：一页里字节是否可信，含义见文件头。
    enum class PageState
    {
        // Valid：整页 4096 字节全部真实读到。
        Valid,
        // PartiallyValid：页内部分字节真实读到，其余是占位 0。
        PartiallyValid,
        // Unreadable：问过了，一个字节都没读到。
        Unreadable,
        // NotAttempted：从未问过（取消 / 通道失败 / 地址溢出）。
        NotAttempted
    };

    // PageRecord：一页的读取结果。bytes 与 valid 恒为 HexViewport::kPageBytes
    // （4096）长；valid[i]==0 的位置 bytes[i] 恒为 0，调用方不得把它当数据显示。
    struct PageRecord
    {
        // pageStart：本页起始地址（页对齐，等于 firstPageStart 加若干个 4096）。
        std::uint64_t pageStart = 0;
        // bytes：页内字节，长度固定 4096；无效位置恒为 0。
        std::vector<std::uint8_t> bytes;
        // valid：页内每字节一个 0/1 有效位，长度固定 4096。
        std::vector<std::uint8_t> valid;
        // state：本页整体状态，见 PageState。
        PageState state = PageState::NotAttempted;
    };

    // PageReadResult：ReadPages 的完整结果。默认值是"什么都没发生"的安全初值。
    struct PageReadResult
    {
        // pages：按地址升序排列的每一页记录；地址溢出时为空。
        std::vector<PageRecord> pages;
        // failure：channelFailed 为真时的失败原因细节串（英文）；其余情形为空。
        std::string failure;
        // channelFailed：因 Failed 状态或地址范围溢出而整体停止。
        bool channelFailed = false;
        // cancelled：因 cancel 标志置位而整体停止。
        bool cancelled = false;
        // scratchAreaDirty：任一次 Read 报告过暂存区被弄脏；一旦为真，后续
        // 读取已整体停止。
        bool scratchAreaDirty = false;
        // readModifyWriteWindow：任一次 Read 报告过读改写窗口，累加（不触发停止）。
        bool readModifyWriteWindow = false;
        // portCalls：本次调用里实际发起的 IMemoryIoPort::Read 调用次数。
        std::uint64_t portCalls = 0;
        // note：Ok/Partial 结果里出现过的非空 failure 文本（搬运注记，不是
        // 错误），按出现顺序去重后用"；"连接，最多 3 条；没有任何注记时为空。
        // 典型来源：私有页表窗口未走成直接窗口、回退到普通路径时门面带的
        // "已回退"说明——调用方据此知道这次读取"已经不再是独立的那一条路径
        // 了"，而不是把这份说明直接当失败处理。
        std::string note;
        // unreadableReason：第一次遇到 Unreadable 结果时的非空 failure 文本；
        // 没有遇到过、或遇到的那次 failure 为空时保持空串。只取第一个，供
        // 状态条解释"为什么这一页是 ??"，不需要也不聚合后续同类原因。
        std::string unreadableReason;
    };

    // ReadPages：读取 [firstPageStart, firstPageStart + pageCount*4096) 这一段
    // 页范围，算法见文件头。
    // 调用方法：在读线程里调用；firstPageStart 必须页对齐（4096 的整数倍），
    // 调用方（HexViewport::PlanFetch 产出的 FetchRange）已经保证这一点，本函数
    // 不再做这项校验。
    // 传入：port 真实或假的 I/O 端口；session 当前目标会话；firstPageStart 起始
    //       页地址；pageCount 页数（可以为 0）；cancel 可为空，非空时每次
    //       发起读取前都会检查一次。
    // 传出：PageReadResult，见上。
    PageReadResult ReadPages(
        IMemoryIoPort& port,
        const MemoryTargetSession& session,
        std::uint64_t firstPageStart,
        std::uint64_t pageCount,
        const std::atomic<bool>* cancel);
}
