// MemoryPageReader.h 的实现。只负责"怎么切块、怎么处理四种读取状态、什么时候
// 停止"，不碰任何真实 I/O（全部经 IMemoryIoPort 完成），因此可以用脚本化假端口
// 离线穷举测试。实现细节全部对应文件头"算法"一节逐条展开，改动前请先回去确认
// 没有偏离那一节描述的行为。

#include "MemoryPageReader.h"

#include <algorithm>
#include <limits>

namespace ksword::memwb
{
    namespace
    {
        // kPageBytes：本文件统一用这个别名代替到处重复的 HexViewport::kPageBytes，
        // 纯粹是少打几个字，数值与含义完全来自 HexViewport（页大小暂定值的唯一来源）。
        constexpr std::uint64_t kPageBytes = HexViewport::kPageBytes;

        // RangeOverflows：判断 [firstPageStart, firstPageStart + pageCount*kPageBytes)
        // 的末尾地址是否会超过 2^64-1（即发生无符号回绕）。
        // 调用方法：仅在 pageCount>=1 时调用；返回 true 表示这段范围不可表示，
        // 调用方必须整体拒绝，绝不能用回绕后的假地址继续计算。
        // 算法：先求"最后一页"能合法取到的最大起始地址 maxLastPageStart（使得该页
        // 的末字节恰好是 2^64-1），再看 firstPageStart 加上 (pageCount-1) 个整页
        // 之后是否仍不超过这个上限——全程只用减法与除法，不做可能溢出的乘法。
        bool RangeOverflows(std::uint64_t firstPageStart, std::uint64_t pageCount)
        {
            constexpr std::uint64_t kMaxAddress = (std::numeric_limits<std::uint64_t>::max)();
            // 最后一页起始地址最多能到这里，否则它的末字节（+kPageBytes-1）就超了。
            const std::uint64_t maxLastPageStart = kMaxAddress - (kPageBytes - 1);
            if (firstPageStart > maxLastPageStart)
            {
                return true;
            }
            // firstPageStart 之后，在不超过 maxLastPageStart 的前提下还能再放几页。
            const std::uint64_t maxAdditionalPages = (maxLastPageStart - firstPageStart) / kPageBytes;
            // 还需要 pageCount-1 个"额外页"（第一页本身已经由上面那条检查确认合法）。
            return (pageCount - 1) > maxAdditionalPages;
        }

        // MarkPageFullyValid：把 page 整页标为真实读到。
        // 传入：page 待写入的页记录；source 本次 Read 返回的原始字节；srcOffset 这
        //       一页对应的字节在 source 里的起始下标（source 可能覆盖好几页）。
        // 前置条件：source.size() >= srcOffset + kPageBytes，调用方负责保证。
        void MarkPageFullyValid(
            PageRecord& page,
            const std::vector<std::uint8_t>& source,
            std::size_t srcOffset)
        {
            std::copy(
                source.begin() + static_cast<std::ptrdiff_t>(srcOffset),
                source.begin() + static_cast<std::ptrdiff_t>(srcOffset + kPageBytes),
                page.bytes.begin());
            std::fill(page.valid.begin(), page.valid.end(), static_cast<std::uint8_t>(1));
            page.state = PageState::Valid;
        }

        // MarkPagePartial：把 page 的前 localValidBytes 个字节标为真实读到，其余
        // （已经是构造时的默认 0/0）保持不变。localValidBytes==0 时等价于整页不可读。
        // 传入：page 待写入的页记录；source/srcOffset 同上；localValidBytes 这一页
        //       内真实读到的字节数（< kPageBytes）。
        void MarkPagePartial(
            PageRecord& page,
            const std::vector<std::uint8_t>& source,
            std::size_t srcOffset,
            std::uint64_t localValidBytes)
        {
            const std::size_t validCount = static_cast<std::size_t>(localValidBytes);
            std::copy(
                source.begin() + static_cast<std::ptrdiff_t>(srcOffset),
                source.begin() + static_cast<std::ptrdiff_t>(srcOffset + validCount),
                page.bytes.begin());
            std::fill_n(page.valid.begin(), validCount, static_cast<std::uint8_t>(1));
            // 前缀长度为 0 等于这一页根本没读到任何东西，与 Unreadable 同一含义；
            // 否则是一页里真假混杂的 PartiallyValid。
            page.state = validCount > 0 ? PageState::PartiallyValid : PageState::Unreadable;
        }

        // MarkPageUnreadable：把 page 标为整页不可读。bytes/valid 的默认初值已经
        // 是全 0，这里只需要改状态，写法上仍显式清一遍，免得以后默认初值被改掉。
        void MarkPageUnreadable(PageRecord& page)
        {
            std::fill(page.bytes.begin(), page.bytes.end(), static_cast<std::uint8_t>(0));
            std::fill(page.valid.begin(), page.valid.end(), static_cast<std::uint8_t>(0));
            page.state = PageState::Unreadable;
        }

        // kMaxNoteParts：note 字段最多保留的注记条数，见文件头 P-1 一节。
        constexpr std::size_t kMaxNoteParts = 3;

        // AppendNote：把一段非空注记文本按"先出现先保留、去重、最多
        // kMaxNoteParts 条"的规则并入 noteParts。只应用于 Ok/Partial 结果的
        // failure 文本——Unreadable 走 unreadableReason、Failed 走顶层
        // result.failure，两者含义不同，不能混进这里。
        // 传入：noteParts 累积到目前为止的注记列表；text 本次要并入的文本
        //       （可能为空，为空直接忽略）。
        void AppendNote(std::vector<std::string>& noteParts, const std::string& text)
        {
            if (text.empty() || noteParts.size() >= kMaxNoteParts)
            {
                return;
            }
            if (std::find(noteParts.begin(), noteParts.end(), text) != noteParts.end())
            {
                // 同一段文本已经记过，不重复加——例如连续好几次 Read 都带着
                // 同一句"私有页表窗口已回退"，note 只需要出现一次。
                return;
            }
            noteParts.push_back(text);
        }

        // JoinWithSemicolon：把 parts 用中文分号"；"连接成一个字符串；parts 为
        // 空时返回空串。调用方法：ReadPages 返回前用它把 noteParts 折叠进
        // result.note。
        std::string JoinWithSemicolon(const std::vector<std::string>& parts)
        {
            std::string joined;
            for (std::size_t index = 0; index < parts.size(); ++index)
            {
                if (index > 0)
                {
                    joined += "；"; // 中文分号，与任务要求的连接符一致
                }
                joined += parts[index];
            }
            return joined;
        }
    } // namespace

    PageReadResult ReadPages(
        IMemoryIoPort& port,
        const MemoryTargetSession& session,
        std::uint64_t firstPageStart,
        std::uint64_t pageCount,
        const std::atomic<bool>* cancel)
    {
        PageReadResult result;

        // 规则 5：空请求什么都不做，连 Limits 都不问。
        if (pageCount == 0)
        {
            return result;
        }

        // 规则 4：地址溢出时整体拒绝，不枚举任何页、不发起任何 Read。
        if (RangeOverflows(firstPageStart, pageCount))
        {
            result.channelFailed = true;
            result.failure = "page range end exceeds the 64-bit address space";
            return result;
        }

        // 预先铺好整段范围的页记录，默认 NotAttempted（bytes/valid 全 0）；
        // 后面只会"覆盖"某些页，从不新增或删除条目，所以下标与页序号一一对应。
        const std::size_t totalPages = static_cast<std::size_t>(pageCount);
        result.pages.resize(totalPages);
        for (std::size_t index = 0; index < totalPages; ++index)
        {
            PageRecord& record = result.pages[index];
            record.pageStart = firstPageStart + static_cast<std::uint64_t>(index) * kPageBytes;
            record.bytes.assign(static_cast<std::size_t>(kPageBytes), 0);
            record.valid.assign(static_cast<std::size_t>(kPageBytes), 0);
            record.state = PageState::NotAttempted;
        }

        // 规则 1：按端口上限把整段范围切成块；每块至少 1 页。
        const IoLimits limits = port.Limits(session);
        std::uint64_t pagesPerBlock = pageCount;
        if (limits.maxReadBytes != 0)
        {
            const std::uint64_t computed = limits.maxReadBytes / kPageBytes;
            pagesPerBlock = computed == 0 ? 1 : computed;
        }

        // noteParts：P-1 新增的注记聚合累积区，循环结束后折叠进 result.note；
        // 不直接往 result.note 里拼接字符串，是因为去重/截断逻辑（AppendNote）
        // 要能看到"已经加过哪些"，用一个独立的 vector 比每次重新切分
        // result.note 更直接。
        std::vector<std::string> noteParts;

        bool stopEverything = false;
        std::uint64_t blockStart = 0; // 块起始页序号（相对 firstPageStart，单位：页）。
        while (!stopEverything && blockStart < pageCount)
        {
            const std::uint64_t blockEnd = (std::min)(blockStart + pagesPerBlock, pageCount);
            bool singlePageReads = false;

            // 规则 2：优先批量读取块里尚未处理的范围；跨页零字节失败后，
            // 本块改为每次一整页。所有请求都不越过当前 blockEnd。
            std::uint64_t cursor = blockStart;
            while (cursor < blockEnd)
            {
                // 规则 3：每次发起 Read 之前先看取消标志。
                if (cancel != nullptr && cancel->load())
                {
                    result.cancelled = true;
                    stopEverything = true;
                    break;
                }

                const std::uint64_t reqAddress = firstPageStart + cursor * kPageBytes;
                const std::uint64_t reqPages = singlePageReads ? 1 : blockEnd - cursor;
                const std::uint64_t reqLength = reqPages * kPageBytes;

                const IoReadResult outcome = port.Read(session, reqAddress, reqLength);
                ++result.portCalls;
                result.readModifyWriteWindow = result.readModifyWriteWindow || outcome.readModifyWriteWindow;

                if (outcome.status == IoReadStatus::Failed)
                {
                    // 当前位置到整段 range 末尾全部保持默认的 NotAttempted，不用
                    // 再逐页覆写——它们本来就是这个状态。
                    result.channelFailed = true;
                    result.failure = outcome.failure;
                    // 真实 DDMA 端口把"暂存扇区没能还原"映射成 Failed + scratchAreaDirty
                    // （见 WorkbenchIoMapping.cpp 的 MapFacadeReadOutcome），所以脏标记必须在
                    // 这个分支里也带出来：否则调用方的 DDMA 闩锁永远不会置位，后续请求还会继续
                    // 读暂存区。
                    if (outcome.scratchAreaDirty)
                    {
                        result.scratchAreaDirty = true;
                    }
                    stopEverything = true;
                    break;
                }

                if (outcome.status == IoReadStatus::Unreadable && outcome.data.empty() && reqPages > 1)
                {
                    // A failed multi-page read does not identify its failing page.
                    // ReadProcessMemory can return no prefix when a later page is
                    // inaccessible. Verify the remaining block one page at a time;
                    // never skip its first page or repeatedly rescan the same hole.
                    if (outcome.scratchAreaDirty)
                    {
                        result.scratchAreaDirty = true;
                        stopEverything = true;
                        break;
                    }
                    singlePageReads = true;
                    continue; // Re-enter the cancellation check before any fallback I/O.
                }

                std::uint64_t consumedPages = 0;
                if (outcome.status == IoReadStatus::Ok)
                {
                    // 当前请求真实读到；data 应当正好 reqLength 字节，
                    // 这里仍夹一下长度，防止端口违反契约时越界访问。
                    const std::uint64_t usable = (std::min<std::uint64_t>)(outcome.data.size(), reqLength);
                    const std::uint64_t fullPages = usable / kPageBytes;
                    for (std::uint64_t page = 0; page < fullPages; ++page)
                    {
                        MarkPageFullyValid(
                            result.pages[static_cast<std::size_t>(cursor + page)],
                            outcome.data,
                            static_cast<std::size_t>(page * kPageBytes));
                    }
                    consumedPages = reqPages;
                    // P-1：Ok 也可能带着非空 failure（搬运"回退/降级"一类的
                    // 注记，不是错误），并入 note。
                    AppendNote(noteParts, outcome.failure);
                }
                else if (outcome.status == IoReadStatus::Partial)
                {
                    // data 是真实前缀：先把整页都在前缀里的页标 Valid，再把前缀
                    // 结束所在的那一页标 PartiallyValid/Unreadable，然后从下一页
                    // 起把剩下的部分留给块内循环的下一轮 Read（不在这里继续读）。
                    const std::uint64_t prefixBytes = (std::min<std::uint64_t>)(outcome.data.size(), reqLength);
                    const std::uint64_t fullPages = prefixBytes / kPageBytes;
                    for (std::uint64_t page = 0; page < fullPages; ++page)
                    {
                        MarkPageFullyValid(
                            result.pages[static_cast<std::size_t>(cursor + page)],
                            outcome.data,
                            static_cast<std::size_t>(page * kPageBytes));
                    }
                    if (fullPages < reqPages)
                    {
                        const std::uint64_t localValid = prefixBytes - fullPages * kPageBytes;
                        MarkPagePartial(
                            result.pages[static_cast<std::size_t>(cursor + fullPages)],
                            outcome.data,
                            static_cast<std::size_t>(fullPages * kPageBytes),
                            localValid);
                        consumedPages = fullPages + 1;
                    }
                    else
                    {
                        // 防御性分支：前缀恰好等于整个请求长度（违反"比请求短"的
                        // 契约），按已经读完整块处理，不留下一个不存在的失败页。
                        consumedPages = reqPages;
                    }
                    // P-1：Partial 同样可能带着非空 failure 注记，规则与 Ok 相同。
                    AppendNote(noteParts, outcome.failure);
                }
                else // IoReadStatus::Unreadable
                {
                    // Single-page failure establishes this page's unreadability.
                    // Do not probe individual bytes or retry that confirmed page.
                    MarkPageUnreadable(result.pages[static_cast<std::size_t>(cursor)]);
                    consumedPages = 1;
                    // P-1：只记第一次遇到的 Unreadable 原因，供状态条解释"为什么
                    // 这一页是 ??"；不是注记，不进 noteParts。
                    if (result.unreadableReason.empty() && !outcome.failure.empty())
                    {
                        result.unreadableReason = outcome.failure;
                    }
                }

                cursor += consumedPages;

                // 规则 2 的 scratchAreaDirty 分支：这次已经读到的数据照常生效，
                // 但不再发起任何后续 Read（含本块剩下的部分与后面所有块）。
                if (outcome.scratchAreaDirty)
                {
                    result.scratchAreaDirty = true;
                    stopEverything = true;
                    break;
                }
            }

            blockStart = blockEnd;
        }

        // P-1：把累积的注记折叠成最终字符串；noteParts 为空时得到空串，
        // 与"没有任何注记"的默认值一致。
        result.note = JoinWithSemicolon(noteParts);
        return result;
    }
}
