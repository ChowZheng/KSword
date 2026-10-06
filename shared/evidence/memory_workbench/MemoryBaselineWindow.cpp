// ============================================================
// MemoryBaselineWindow.cpp
// 作用：实现基线窗口的选取（SelectBaselineSpan）与重算判定（DecideBaselineRefeed）。
// 约定：全部在"页序号"空间里做算术（序号 = 地址 / 页大小），只有最后换算成地址时
//       才乘回页大小；序号空间里的比较与加减都不会回绕，这是不溢出的根本办法。
// ============================================================

#include "MemoryBaselineWindow.h"

namespace ksword::memwb
{
    namespace
    {
        // uint64 地址空间的最大值，只用来推导"终点（不含）必须可表示"这一条。
        constexpr std::uint64_t kAddressMax = 0xFFFFFFFFFFFFFFFFULL;

        // ValidPageRange：当前边界下"整页都合法"的页序号闭区间 [first, last]。
        struct ValidPageRange
        {
            // 是否存在至少一个合法页。
            bool any = false;
            // 第一个合法页的序号。any 为 false 时无意义。
            std::uint64_t first = 0;
            // 最后一个合法页的序号。any 为 false 时无意义。
            std::uint64_t last = 0;
        };

        // ComputeValidPageRange：由地址空间闭区间推出合法页序号区间。
        // 传入：bounds 已保证 lowest <= highest；pageSize 非零。
        // 传出：ValidPageRange。合法页须同时满足：
        //       - 起点 >= lowest（半页跨下边界的不选）；
        //       - 末字节 <= highest（半页跨上边界的不选）；
        //       - 末字节 <= 0xFF..FE（终点不含，必须能用 uint64 表示，叠加层的统一规则）。
        ValidPageRange ComputeValidPageRange(const AddressSpaceBounds& bounds, const std::uint64_t pageSize)
        {
            ValidPageRange range;

            // 末字节上界：既不超过 highest，也不超过 0xFF..FE。
            std::uint64_t effectiveHighest = bounds.highest;
            if (effectiveHighest > kAddressMax - 1)
            {
                effectiveHighest = kAddressMax - 1;
            }

            // 连一整页都装不下：没有合法页。先判断它，才能放心做下面的减法。
            if (effectiveHighest < pageSize - 1)
            {
                return range;
            }

            // 最后一个合法页：起点最大为 effectiveHighest - (pageSize - 1)，向下取整到页边界。
            const std::uint64_t lastStartLimit = effectiveHighest - (pageSize - 1);
            const std::uint64_t lastIndex = lastStartLimit / pageSize;

            // 第一个合法页：lowest 向上取整到页边界。余数非零说明 lowest 落在页中间，
            // 那一页是半页，取下一页。pageSize 为 1 时余数恒为 0，+1 不会溢出。
            std::uint64_t firstIndex = bounds.lowest / pageSize;
            if (bounds.lowest % pageSize != 0)
            {
                firstIndex += 1;
            }

            if (firstIndex > lastIndex)
            {
                return range;
            }
            range.any = true;
            range.first = firstIndex;
            range.last = lastIndex;
            return range;
        }

        // IsPageSettled：页序号 pageIndex 对应的页起点是否在已落定集合里。
        // 调用方保证 pageIndex 是合法页序号，所以 pageIndex * pageSize 不会回绕。
        bool IsPageSettled(
            const std::set<std::uint64_t>& settledPageStarts,
            const std::uint64_t pageIndex,
            const std::uint64_t pageSize)
        {
            return settledPageStarts.find(pageIndex * pageSize) != settledPageStarts.end();
        }
    }

    BaselineSpan SelectBaselineSpan(
        const std::uint64_t insertionAddress,
        const std::set<std::uint64_t>& settledPageStarts,
        const AddressSpaceBounds& bounds,
        const BaselineWindowPolicy& policy)
    {
        // 默认值就是"无效的空跨度"，所有早退路径都直接返回它或只改 status。
        BaselineSpan span;

        // 第一步：参数合法性。页大小为 0 会除零，页数上限为 0 无法容纳插入点页。
        if (policy.pageSize == 0 || policy.maxPages == 0 || bounds.lowest > bounds.highest)
        {
            return span;
        }
        const std::uint64_t pageSize = policy.pageSize;

        // 第二步：插入点所在页必须是合法页。序号区间判据同时覆盖了
        // "地址空间之外 / 半页 / 终点不可表示"三种情形。
        const ValidPageRange valid = ComputeValidPageRange(bounds, pageSize);
        const std::uint64_t insertionIndex = insertionAddress / pageSize;
        if (!valid.any || insertionIndex < valid.first || insertionIndex > valid.last)
        {
            span.status = BaselineSpanStatus::InsertionOutsideSpace;
            return span;
        }

        // 第三步：插入点所在页必须已落定，否则不选邻页，直接返回空。
        if (!IsPageSettled(settledPageStarts, insertionIndex, pageSize))
        {
            span.status = BaselineSpanStatus::InsertionPageNotSettled;
            return span;
        }

        // 第四步：向左、向右各自数出"紧挨着的、合法且已落定"的页数。
        // 每侧最多数 maxPages - 1 页：再多也装不进窗口，不必为遥远的页白做查找。
        const std::uint64_t maxExtraPages = policy.maxPages - 1;
        std::uint64_t availableLeft = 0;
        while (availableLeft < maxExtraPages
            && insertionIndex - availableLeft > valid.first
            && IsPageSettled(settledPageStarts, insertionIndex - availableLeft - 1, pageSize))
        {
            availableLeft += 1;
        }
        std::uint64_t availableRight = 0;
        while (availableRight < maxExtraPages
            && insertionIndex + availableRight < valid.last
            && IsPageSettled(settledPageStarts, insertionIndex + availableRight + 1, pageSize))
        {
            availableRight += 1;
        }

        // 第五步：以插入点页为中心分配名额。左侧心愿 (maxPages-1)/2 页，其余给右侧；
        // 一侧不够就把名额让给另一侧。三行依次是：左侧先取心愿值、右侧用剩余名额
        // 取满、左侧再用右侧没吃完的名额补足。
        const std::uint64_t leftWanted = maxExtraPages / 2;
        std::uint64_t leftTake = availableLeft < leftWanted ? availableLeft : leftWanted;
        const std::uint64_t rightRoom = maxExtraPages - leftTake;
        const std::uint64_t rightTake = availableRight < rightRoom ? availableRight : rightRoom;
        const std::uint64_t leftRoom = maxExtraPages - rightTake;
        leftTake = availableLeft < leftRoom ? availableLeft : leftRoom;

        // 第六步：换算成地址。起点与总页数都落在合法页区间内，乘页大小不会回绕，
        // 终点不超过 0xFF..FF（合法页的末字节不超过 0xFF..FE）。
        span.status = BaselineSpanStatus::Ok;
        span.base = (insertionIndex - leftTake) * pageSize;
        span.length = (leftTake + rightTake + 1) * pageSize;
        return span;
    }

    BaselineRefeedDecision DecideBaselineRefeed(
        const BaselineWindowRecord& record,
        const std::uint64_t anchorAddress,
        const std::uint64_t currentSourceRevision,
        const std::set<std::uint64_t>& settledPageStarts,
        const BaselineWindowPolicy& policy)
    {
        // 策略不合法时不在这里除零，交给 SelectBaselineSpan 去报 InvalidArgument。
        if (policy.pageSize == 0 || policy.maxPages == 0)
        {
            return BaselineRefeedDecision::Recompute;
        }
        const std::uint64_t pageSize = policy.pageSize;

        // 第一步：还没有窗口。
        if (!record.hasWindow)
        {
            return BaselineRefeedDecision::Recompute;
        }

        // 第二步：记录自相矛盾（长度为 0、不对齐、终点回绕）一律重算，
        // 不信任来路不明的记录，也免得下面的逐页循环在坏数据上回绕。
        const bool lengthBad = record.length == 0 || record.length % pageSize != 0;
        const bool baseBad = record.base % pageSize != 0;
        const bool wraps = record.length > kAddressMax - record.base;
        if (lengthBad || baseBad || wraps)
        {
            return BaselineRefeedDecision::Recompute;
        }

        // 第三步：窗口页数超过当前上限（上限被调小）。
        const std::uint64_t pageCount = record.length / pageSize;
        if (pageCount > policy.maxPages)
        {
            return BaselineRefeedDecision::Recompute;
        }

        // 第四步：锚点滚出了窗口。窗口内的滚动（含恰好在首字节、末字节）不重算。
        const bool anchorInside = anchorAddress >= record.base
            && anchorAddress - record.base < record.length;
        if (!anchorInside)
        {
            return BaselineRefeedDecision::Recompute;
        }

        // 第五步：窗口内任何一页不再落定（被换出缓存、被判陈旧），基线里那段字节已
        // 无法再从画布取全，必须重算。页数不超过 maxPages，循环有界。
        for (std::uint64_t index = 0; index < pageCount; ++index)
        {
            const std::uint64_t pageStart = record.base + index * pageSize;
            if (settledPageStarts.find(pageStart) == settledPageStarts.end())
            {
                return BaselineRefeedDecision::Recompute;
            }
        }

        // 第六步：位置没问题，只看来源代次。不同说明发生过重读，原位重喂；
        // 相同则什么都不做。窗口外多出来的相邻页在这里不起作用，扩张只发生在
        // 锚点滚出窗口、走第四步重算的时候。
        if (record.sourceRevision != currentSourceRevision)
        {
            return BaselineRefeedDecision::RefreshSameSpan;
        }
        return BaselineRefeedDecision::Keep;
    }
}
