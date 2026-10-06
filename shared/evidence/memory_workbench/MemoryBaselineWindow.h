#pragma once

// ============================================================
// MemoryBaselineWindow.h
// 作用：
// - 内存工作台"基线窗口"的纯函数式选取层。叠加层（MemoryDiffOverlay）的基线只能是
//   一段连续字节，而画布按 4 KiB 页懒读、页缓存有上限，所以必须先决定"把哪一段连续
//   已读字节喂给 RefreshBaseline"。本模块只回答两个问题：
//     1. 围绕插入点，应该喂哪一段？（SelectBaselineSpan）
//     2. 上次喂过的那一段，现在要不要重算 / 要不要重喂？（DecideBaselineRefeed）
// - 纯 C++20 标准库实现，不依赖 Qt、不依赖 Win32、不读时钟、不做任何 I/O。
//   同输入必同输出，所以可以用手算期望值与暴力对拍完整测试。
//
// ============================================================
// 冻结接口摘要（后续 WP-J 的 BaselineFeeder 依赖，改动须同步通知）
// ============================================================
//   kBaselineWindowDefaultPageSize   默认页大小 4096 字节
//   kBaselineWindowDefaultMaxPages   默认窗口页数上限 256（暂定，待渲染基准）
//   AddressSpaceBounds{lowest,highest}
//                                    地址空间闭区间 [lowest, highest]（含两端）
//   BaselineWindowPolicy{pageSize,maxPages}
//                                    页大小与页数上限
//   BaselineSpanStatus               Ok / InvalidArgument / InsertionOutsideSpace /
//                                    InsertionPageNotSettled
//   BaselineSpan{status,base,length} 选取结果；length 为 0 即空跨度；
//                                    IsEmpty()/End()/Contains(address)
//   SelectBaselineSpan(insertion, settledPageStarts, bounds, policy) -> BaselineSpan
//                                    围绕插入点选"已落定页"组成的连续跨度
//   BaselineWindowRecord{hasWindow,base,length,sourceRevision}
//                                    上次已喂给叠加层的窗口记录；Reset()/Set(span, rev)
//   BaselineRefeedDecision           Keep / RefreshSameSpan / Recompute
//   DecideBaselineRefeed(record, anchor, currentSourceRevision, settledPageStarts, policy)
//                                    -> BaselineRefeedDecision
//
// ============================================================
// 选取规则（SelectBaselineSpan）
// ============================================================
// 输入：
// - 插入点地址 insertionAddress：窗口围绕它选取，调用方可传插入点或视口中心。
// - 已落定页集合 settledPageStarts：每个元素是一页的**起始地址**，且该页已成功回填、
//   非陈旧（调用方负责，本模块不判断页的新旧）。只认页对齐的起点，不对齐的元素当作
//   不存在（保守：宁可少认一页，也不把没读到的字节喂成基线）。
// - 地址空间边界 bounds：闭区间。只有**整页**落在 [lowest, highest] 内的页才合法，
//   跨边界的半页一律不选（保守，同旧窗口"只含真实读到的字节"的精神）。
// - 策略 policy：页大小（必须非零）与页数上限（必须非零）。
//
// 输出（status 为 Ok 时）：
// - 含插入点所在页；该页未落定则返回空跨度（InsertionPageNotSettled），不会退而求其次
//   选邻页——插入点所在页没读到，那里就根本无从编辑。
// - base 与 length 都是页大小的整数倍（页对齐）。
// - 只含已落定页，且是围绕插入点页的**连续**一段，中间不跨越任何未落定页。
// - 页数不超过 maxPages；连续段更长时以插入点页为中心裁剪：左侧取
//   (maxPages-1)/2 页、右侧取其余页，偶数上限时右侧多一页；某一侧不够长时把
//   多出的名额让给另一侧，总页数仍取满 min(连续段长度, maxPages)。
// - 边界不溢出：叠加层要求 base + length 不超过 uint64 最大值（终点不含，必须能表示），
//   所以终点恰为 2^64 的那一页（地址空间最末一页）永远不被选入；插入点落在该页时
//   返回 InsertionOutsideSpace。这与叠加层"最末一个字节不可编辑"的既有保守边界一致。
//
// ============================================================
// 重算判定（DecideBaselineRefeed）
// ============================================================
// 背景：RefreshBaseline 每调用一次就清掉"自己写入"标记，base/长度一变就清掉"上次读取"
// （青色高亮的依据）。所以"重读"与"滚动"必须区分对待：
// - Recompute        必须重新 SelectBaselineSpan：尚无窗口、记录自相矛盾、锚点滚出窗口、
//                    窗口内有页不再落定、窗口页数超过当前上限。
// - RefreshSameSpan  位置不变，只是来源代次变了（重读）：用记录里同样的 base/length 重喂，
//                    "上次读取"得以保留，青色成立。
// - Keep             什么都不用做：锚点仍在窗口内的滚动不重算，窗口外多出来的相邻页也
//                    不触发扩张（扩张只发生在锚点滚出窗口时），因此不会把"自己写入"
//                    标记反复清掉。
// 判定顺序固定，先命中者胜出：无窗口 -> 记录自相矛盾 -> 页数超上限 -> 锚点出窗口
// -> 窗口内有页未落定 -> 来源代次不同 -> Keep。
// 调用方在"无在途读取请求"时才应用本判定（在途页暂不落定，会被误判为需要重算）。
// ============================================================

#include <cstdint>
#include <set>

namespace ksword::memwb
{
    // 默认页大小：与画布按页懒读的粒度一致。
    inline constexpr std::uint64_t kBaselineWindowDefaultPageSize = 4096;

    // 默认窗口页数上限。暂定值：256 页 = 1 MiB，与虚拟读取单次上限同量级；
    // 待渲染基准确认后可能调整，所以始终经 BaselineWindowPolicy 传入，不在算法里写死。
    inline constexpr std::uint64_t kBaselineWindowDefaultMaxPages = 256;

    // AddressSpaceBounds：当前目标的地址空间闭区间 [lowest, highest]。
    // 例：32 位进程 [0, 0xFFFFFFFF]；内核 [0xFFFF800000000000, 0xFFFFFFFFFFFFFFFF]。
    struct AddressSpaceBounds
    {
        // 最低可寻址字节。
        std::uint64_t lowest = 0;
        // 最高可寻址字节（含）。默认整个 uint64 空间。
        std::uint64_t highest = 0xFFFFFFFFFFFFFFFFULL;
    };

    // BaselineWindowPolicy：窗口选取的参数。
    struct BaselineWindowPolicy
    {
        // 页大小（字节），必须非零；窗口的 base 与 length 都是它的整数倍。
        std::uint64_t pageSize = kBaselineWindowDefaultPageSize;
        // 页数上限，必须非零。
        std::uint64_t maxPages = kBaselineWindowDefaultMaxPages;
    };

    // BaselineSpanStatus：一次选取的结果。非 Ok 时跨度一律为空。
    enum class BaselineSpanStatus
    {
        // 选取成功，跨度非空。
        Ok,
        // 参数不合法：pageSize 为 0、maxPages 为 0，或 lowest 大于 highest。
        InvalidArgument,
        // 插入点所在页不是合法页：地址空间之外、只有半页落在边界内，
        // 或终点无法用 uint64 表示（地址空间最末一页）。
        InsertionOutsideSpace,
        // 插入点所在页合法但尚未落定（未读到或已陈旧）。
        InsertionPageNotSettled
    };

    // BaselineSpan：选取结果。默认构造值是"无效的空跨度"（InvalidArgument），
    // 这样失败路径上忘记赋值的调用方拿到的是空而不是上一次的残留。
    struct BaselineSpan
    {
        // 选取结果。
        BaselineSpanStatus status = BaselineSpanStatus::InvalidArgument;
        // 跨度起始地址（页对齐）。空跨度时为 0。
        std::uint64_t base = 0;
        // 跨度字节数（页大小的整数倍）。0 表示空跨度。
        std::uint64_t length = 0;

        // 是否空跨度。
        bool IsEmpty() const
        {
            return length == 0;
        }

        // 终点（不含）。由 SelectBaselineSpan 产生的非空跨度保证 base + length 不回绕。
        std::uint64_t End() const
        {
            return base + length;
        }

        // address 是否落在 [base, base + length) 内；空跨度恒为 false，不会回绕。
        bool Contains(const std::uint64_t address) const
        {
            return length != 0 && address >= base && address - base < length;
        }
    };

    // BaselineWindowRecord：上次已经喂给叠加层的窗口。由调用方持有，每次
    // RefreshBaseline 成功后用 Set 记下，换目标时用 Reset 清掉。
    struct BaselineWindowRecord
    {
        // 是否已有窗口记录。
        bool hasWindow = false;
        // 窗口起始地址。
        std::uint64_t base = 0;
        // 窗口字节数。
        std::uint64_t length = 0;
        // 喂入时的来源代次（SessionRevisions::Source()）。
        std::uint64_t sourceRevision = 0;

        // 清空记录（换目标时调用）。
        void Reset()
        {
            hasWindow = false;
            base = 0;
            length = 0;
            sourceRevision = 0;
        }

        // 记下一次喂入。传入空跨度等价于 Reset，免得记录出"有窗口但长度为 0"的矛盾态。
        void Set(const BaselineSpan& span, const std::uint64_t revision)
        {
            if (span.IsEmpty())
            {
                Reset();
                return;
            }
            hasWindow = true;
            base = span.base;
            length = span.length;
            sourceRevision = revision;
        }
    };

    // BaselineRefeedDecision：DecideBaselineRefeed 的结论，含义见文件头"重算判定"。
    enum class BaselineRefeedDecision
    {
        // 窗口与来源都没变，不要调用 RefreshBaseline。
        Keep,
        // 位置不变，只需用记录里的 base/length 重喂新字节。
        RefreshSameSpan,
        // 必须重新 SelectBaselineSpan 再喂。
        Recompute
    };

    // SelectBaselineSpan：围绕插入点选取"已落定页"组成的连续跨度。
    // 调用方法：先 SelectBaselineSpan，status 为 Ok 才用 span.base/span.length
    //           去画布取字节并调用 RefreshBaseline，随后 record.Set(span, 来源代次)。
    // 传入：insertionAddress 插入点；settledPageStarts 已落定页的起始地址集合；
    //       bounds 地址空间闭区间；policy 页大小与页数上限。
    // 传出：BaselineSpan，规则见文件头；失败时 base/length 均为 0。
    BaselineSpan SelectBaselineSpan(
        std::uint64_t insertionAddress,
        const std::set<std::uint64_t>& settledPageStarts,
        const AddressSpaceBounds& bounds,
        const BaselineWindowPolicy& policy);

    // DecideBaselineRefeed：判断上次喂入的窗口现在该如何处理。
    // 调用方法：每次"脏"事件（锚点移动、页回填、来源代次变化）后、防抖到期且无在途
    //           读取请求时调用；按返回值决定 Keep / 原位重喂 / 重新选取。
    // 传入：record 上次喂入记录；anchorAddress 当前锚点（插入点或视口中心）；
    //       currentSourceRevision 当前来源代次；settledPageStarts 已落定页起始地址集合；
    //       policy 页大小与页数上限。
    // 传出：BaselineRefeedDecision，判定顺序见文件头。policy 不合法时一律返回 Recompute
    //       （让 SelectBaselineSpan 去报 InvalidArgument，而不是在这里除零）。
    BaselineRefeedDecision DecideBaselineRefeed(
        const BaselineWindowRecord& record,
        std::uint64_t anchorAddress,
        std::uint64_t currentSourceRevision,
        const std::set<std::uint64_t>& settledPageStarts,
        const BaselineWindowPolicy& policy);
}
