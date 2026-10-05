// 十六进制虚拟视图模型的页缓存部分（shared/evidence/memory_workbench/HexViewport.h）的离线测试：
// 字节四态、来源代次守卫、LRU 淘汰、预取规划、地址空间上沿，以及命中率计数基准。
// 入口 RunMemwbViewportCacheTests 由 HexViewportTests.cpp 的 RunMemwbViewportTests 汇总调用。
//
// 为什么页缓存必须被穷举：它同样属于**算错了不会报错**的一类。
//   * 陈旧的异步结果被当成新数据收下：界面照样画、字节照样显示，显示的却是上一个目标的内容；
//   * 不可读被当成 0：用户会把"读不到"看成"这里就是全零"，这是篡改检测里最糟的误导；
//   * 预取把可见页挤出缓存：画面反复闪回"未加载"，看起来像是读取慢，实则是规划错了。
//
// 断言原则与 NumericTextParseTests.cpp 一致：期望值手算写死、边界两侧都测、拒绝路径显式测，
// 并且每次拒绝之后都回头确认缓存状态**完全没变**（拒绝不能留下半截改动）。
//
// 命中率基准是"计数断言"，不是计时断言：只数请求了几页、淘汰了几页、命中/未命中几次，
// 所以在任何机器上结果都确定。

#include "TestSupport.h"

#include "../shared/evidence/memory_workbench/HexViewport.h"

#include <cstdint>
#include <vector>

// 本文件的入口，声明与 HexViewportTests.cpp 中的一致。
void RunMemwbViewportCacheTests(KswordTests::Suite& suite);

namespace {

using ksword::memwb::HexViewport;
using State = HexViewport::ByteState;
using Result = HexViewport::PageResult;

// kTop：uint64 最大值，也是地址空间的最后一个地址。
constexpr std::uint64_t kTop = 0xFFFFFFFFFFFFFFFFULL;

// kPage：一页的字节数，测试里写死 4096，与被测常量分开写以便单独核对。
constexpr std::uint64_t kPage = 4096ULL;

// MakeBytes：造一页测试数据。第 i 个字节 = (i*7 + 3 + seed) mod 256。
// 手算样例（seed=0）：偏移 0 -> 0x03，偏移 1 -> 0x0A，偏移 100 -> 703 mod 256 = 191 = 0xBF。
std::vector<std::uint8_t> MakeBytes(std::uint32_t seed) {
    std::vector<std::uint8_t> bytes(4096U);
    for (std::uint32_t index = 0; index < 4096U; ++index) {
        bytes[index] = static_cast<std::uint8_t>((index * 7U + 3U + seed) % 256U);
    }
    return bytes;
}

// FullMask：整页都读到了的掩码。
std::vector<std::uint8_t> FullMask() {
    return std::vector<std::uint8_t>(4096U, static_cast<std::uint8_t>(1));
}

// Range：构造一段页范围（距离字段对 Mark* 无意义，置 0）。
HexViewport::FetchRange Range(std::uint64_t firstPageStart, std::uint64_t pageCount) {
    HexViewport::FetchRange range;
    range.firstPageStart = firstPageStart;
    range.pageCount = pageCount;
    return range;
}

// Plan：规划结果的简写，便于逐条比较（起点、页数、距离）。
bool PlanEntryIs(const HexViewport::FetchRange& entry, std::uint64_t start, std::uint64_t count, std::uint64_t distance) {
    return entry.firstPageStart == start && entry.pageCount == count && entry.distancePages == distance;
}

// Insert：把整页有效数据插入缓存，seed 决定内容。
Result Insert(HexViewport& view, std::uint64_t pageStart, std::uint32_t seed, std::uint64_t revision) {
    return view.InsertPage(pageStart, MakeBytes(seed), FullMask(), revision);
}

// ------------------------------------------------------------
// 一、字节四态：NotLoaded / Pending / Unreadable / Valid 必须彼此区分。
// ------------------------------------------------------------
void TestByteStates(KswordTests::Suite& suite) {
    // 0x10000..0x1FFFF = 16 页，页起点 0x10000、0x11000、...、0x1F000。
    HexViewport view(0x10000, 0x1FFFF);

    // 初始：什么都没读过。值必须是 0 且状态是 NotLoaded——但调用方只能看状态。
    HexViewport::ByteLookup first = view.LookupByte(0x10000);
    suite.expect(first.state == State::NotLoaded && first.value == 0, L"memwb cache: a fresh view has nothing loaded");

    // 登记在途：只有那一页变成 Pending，邻页仍然是 NotLoaded。
    suite.expect(view.MarkInFlight(Range(0x10000, 1)) == Result::Accepted, L"memwb cache: marking a page in flight is accepted");
    suite.expect(view.LookupByte(0x10000).state == State::Pending && view.LookupByte(0x10FFF).state == State::Pending,
        L"memwb cache: every byte of an in-flight page is pending");
    suite.expect(view.LookupByte(0x11000).state == State::NotLoaded, L"memwb cache: the next page is still not loaded");
    suite.expect(view.IsPageInFlight(0x10000) && view.InFlightPageCount() == 1U, L"memwb cache: in-flight bookkeeping");

    // 结果回来：变成 Valid，值按手算。偏移 0 -> 0x03，偏移 1 -> 0x0A，偏移 100(0x64) -> 0xBF。
    suite.expect(Insert(view, 0x10000, 0, 0) == Result::Accepted, L"memwb cache: a fresh page is accepted");
    first = view.LookupByte(0x10000);
    suite.expect(first.state == State::Valid && first.value == 0x03, L"memwb cache: offset 0 holds 0x03");
    suite.expect(view.LookupByte(0x10001).value == 0x0A && view.LookupByte(0x10064).value == 0xBF,
        L"memwb cache: offsets 1 and 100 hold 0x0A and 0xBF");
    suite.expect(view.LookupByte(0x10064).state == State::Valid, L"memwb cache: offset 100 is valid");
    suite.expect(!view.IsPageInFlight(0x10000) && view.InFlightPageCount() == 0U && view.IsPageCached(0x10000),
        L"memwb cache: inserting a page clears its in-flight mark");

    // 部分读：掩码为 0 的字节是 Unreadable，值必须是 0——哪怕底层缓冲里那个位置有非零数据。
    // seed=1：偏移 9 的底层字节 = 9*7+3+1 = 67 = 0x43，偏移 10 = 74 = 0x4A。
    std::vector<std::uint8_t> partialMask = FullMask();
    for (std::size_t offset = 0; offset < 10U; ++offset) {
        partialMask[offset] = 0;
    }
    suite.expect(view.InsertPage(0x11000, MakeBytes(1), partialMask, 0) == Result::Accepted,
        L"memwb cache: a partially readable page is accepted");
    const HexViewport::ByteLookup hole = view.LookupByte(0x11009);
    suite.expect(hole.state == State::Unreadable && hole.value == 0,
        L"memwb cache: a masked-out byte is unreadable and does not leak the buffer's 0x43");
    const HexViewport::ByteLookup edge = view.LookupByte(0x1100A);
    suite.expect(edge.state == State::Valid && edge.value == 0x4A,
        L"memwb cache: the first unmasked byte is valid (0x4A)");
    suite.expect(view.LookupByte(0x11000).state == State::Unreadable, L"memwb cache: offset 0 of the partial page is unreadable");

    // 整页不可读：与"还没读"必须是不同的状态。
    suite.expect(view.MarkUnreadable(Range(0x12000, 1), 0) == Result::Accepted, L"memwb cache: marking a page unreadable is accepted");
    suite.expect(view.LookupByte(0x12000).state == State::Unreadable && view.LookupByte(0x12FFF).state == State::Unreadable,
        L"memwb cache: an unreadable page is unreadable at both ends");
    suite.expect(view.LookupByte(0x12000).value == 0, L"memwb cache: an unreadable byte reports value 0");
    suite.expect(view.LookupByte(0x13000).state == State::NotLoaded && view.LookupByte(0x12000).state != State::NotLoaded,
        L"memwb cache: unreadable is distinguishable from not loaded");
    suite.expect(view.IsPageCached(0x12000), L"memwb cache: an unreadable page occupies a cache slot");

    // 不可读覆盖有效页：旧内容不能在"读不到"的页上继续显示。
    suite.expect(view.MarkUnreadable(Range(0x10000, 1), 0) == Result::Accepted
        && view.LookupByte(0x10000).state == State::Unreadable && view.LookupByte(0x10000).value == 0,
        L"memwb cache: marking a valid page unreadable hides the old bytes");
    suite.expect(Insert(view, 0x10000, 0, 0) == Result::Accepted && view.LookupByte(0x10000).state == State::Valid,
        L"memwb cache: a later successful read replaces the unreadable mark");

    // 空间之外：不是"未加载"的等待状态，也绝不是数据。
    suite.expect(view.LookupByte(0x0FFFF).state == State::NotLoaded && view.LookupByte(0x20000).state == State::NotLoaded,
        L"memwb cache: addresses outside the space are never data");
    suite.expect(view.LookupByte(0x20000).value == 0 && view.PeekByte(0x0FFFF).value == 0,
        L"memwb cache: outside addresses report value 0");

    // 已缓存的页不会被登记在途降级。
    suite.expect(view.MarkInFlight(Range(0x10000, 1)) == Result::Accepted && view.LookupByte(0x10000).state == State::Valid
        && !view.IsPageInFlight(0x10000),
        L"memwb cache: marking a cached page in flight does not downgrade it");

    // 撤销在途：回到 NotLoaded，并且 PlanFetch 会重新请求它。
    suite.expect(view.MarkInFlight(Range(0x14000, 1)) == Result::Accepted && view.LookupByte(0x14000).state == State::Pending,
        L"memwb cache: setup in-flight page for cancellation");
    suite.expect(view.CancelInFlight(Range(0x14000, 1)) == Result::Accepted && view.LookupByte(0x14000).state == State::NotLoaded
        && !view.IsPageInFlight(0x14000),
        L"memwb cache: cancelling an in-flight mark returns the page to not loaded");
}

// ------------------------------------------------------------
// 二、来源代次守卫：陈旧（和超前）的异步结果必须被拒绝。
// ------------------------------------------------------------
void TestStaleRevision(KswordTests::Suite& suite) {
    HexViewport view(0x10000, 0x1FFFF);
    suite.expect(view.SourceRevision() == 0ULL, L"memwb cache: the initial source revision is 0");

    // 换到代次 5 之后，代次 4、6、0 都与当前不符：旧的和超前的一视同仁地拒绝，缓存保持为空。
    view.InvalidateAll(5);
    suite.expect(view.SourceRevision() == 5ULL, L"memwb cache: InvalidateAll updates the revision");
    suite.expect(Insert(view, 0x10000, 0, 4) == Result::RejectedStaleRevision,
        L"memwb cache: a result from the previous revision is rejected");
    suite.expect(Insert(view, 0x10000, 0, 6) == Result::RejectedStaleRevision,
        L"memwb cache: a result from a future revision is rejected too");
    suite.expect(Insert(view, 0x10000, 0, 0) == Result::RejectedStaleRevision,
        L"memwb cache: a result from the original revision is rejected");
    suite.expect(view.CachedPageCount() == 0U && view.LookupByte(0x10000).state == State::NotLoaded,
        L"memwb cache: rejected stale results leave the cache empty");
    suite.expect(view.Stats().staleRejections == 3ULL, L"memwb cache: three stale writes were counted");
    suite.expect(Insert(view, 0x10000, 0, 5) == Result::Accepted && view.LookupByte(0x10000).value == 0x03,
        L"memwb cache: a result from the current revision is accepted");

    // 换目标：在途登记与已缓存内容全部清空，旧代次的结果晚到也进不来。
    suite.expect(view.MarkInFlight(Range(0x11000, 1)) == Result::Accepted, L"memwb cache: setup an in-flight page");
    view.InvalidateAll(6);
    suite.expect(view.CachedPageCount() == 0U && view.InFlightPageCount() == 0U,
        L"memwb cache: InvalidateAll drops cached pages and in-flight marks");
    suite.expect(view.LookupByte(0x10000).state == State::NotLoaded && view.LookupByte(0x11000).state == State::NotLoaded,
        L"memwb cache: nothing from the old target stays visible");
    suite.expect(Insert(view, 0x11000, 7, 5) == Result::RejectedStaleRevision,
        L"memwb cache: the old target's late result is dropped");
    suite.expect(view.LookupByte(0x11000).state == State::NotLoaded && !view.IsPageCached(0x11000),
        L"memwb cache: the dropped late result is not visible");

    // 陈旧的"读取失败"同样不能生效：它不能把新读到的页标成不可读。
    suite.expect(Insert(view, 0x11000, 7, 6) == Result::Accepted, L"memwb cache: the new target's page is accepted");
    suite.expect(view.MarkUnreadable(Range(0x11000, 1), 5) == Result::RejectedStaleRevision
        && view.LookupByte(0x11000).state == State::Valid,
        L"memwb cache: a stale failure cannot mark a freshly read page unreadable");
    suite.expect(view.MarkUnreadable(Range(0x11000, 1), 7) == Result::RejectedStaleRevision,
        L"memwb cache: a future-revision failure is rejected as well");
}

// ------------------------------------------------------------
// 三、非法写入的显式拒绝：每一种拒绝之后缓存都必须原样不动。
// ------------------------------------------------------------
void TestRejections(KswordTests::Suite& suite) {
    HexViewport view(0x10000, 0x1FFFF);
    suite.expect(Insert(view, 0x10010, 0, 0) == Result::RejectedMisaligned, L"memwb cache: an unaligned page start is rejected");
    suite.expect(Insert(view, 0x20000, 0, 0) == Result::RejectedOutsideSpace, L"memwb cache: a page after the space is rejected");
    suite.expect(Insert(view, 0x0F000, 0, 0) == Result::RejectedOutsideSpace, L"memwb cache: a page before the space is rejected");
    suite.expect(view.InsertPage(0x10000, std::vector<std::uint8_t>(4095U), FullMask(), 0) == Result::RejectedBadSize,
        L"memwb cache: a 4095-byte buffer is rejected");
    suite.expect(view.InsertPage(0x10000, MakeBytes(0), std::vector<std::uint8_t>(4097U, 1), 0) == Result::RejectedBadSize,
        L"memwb cache: a 4097-byte mask is rejected");
    suite.expect(view.InsertPage(0x10000, std::vector<std::uint8_t>(), std::vector<std::uint8_t>(), 0) == Result::RejectedBadSize,
        L"memwb cache: empty buffers are rejected");
    suite.expect(view.CachedPageCount() == 0U && view.Stats().staleRejections == 0ULL,
        L"memwb cache: bad arguments leave the cache untouched and are not counted as stale");

    // 空间首尾不与页边界对齐：边界页只要与空间相交就被接受，空间之外的字节仍然不是数据。
    HexViewport edge(0x10800, 0x11FFF);
    suite.expect(Insert(edge, 0x10000, 0, 0) == Result::Accepted, L"memwb cache: a boundary page straddling the start is accepted");
    suite.expect(edge.LookupByte(0x10800).state == State::Valid && edge.LookupByte(0x10800).value == 0x03,
        L"memwb cache: the first in-space byte of the boundary page is valid (offset 0x800 -> 0x03)");
    suite.expect(edge.LookupByte(0x107FF).state == State::NotLoaded,
        L"memwb cache: the byte just before the space is not data even though its page is cached");

    // 页范围校验：对齐、页数 0、页数超过容量、越过空间末页、起点在空间之外。
    HexViewport ranges(0x10000, 0x1FFFF);
    suite.expect(ranges.MarkInFlight(Range(0x10010, 1)) == Result::RejectedMisaligned, L"memwb cache: unaligned range rejected");
    suite.expect(ranges.MarkInFlight(Range(0x10000, 0)) == Result::RejectedBadSize, L"memwb cache: a zero-page range is rejected");
    suite.expect(ranges.MarkInFlight(Range(0x10000, 257)) == Result::RejectedBadSize,
        L"memwb cache: a range larger than the cache capacity (256) is rejected");
    suite.expect(ranges.MarkInFlight(Range(0x1F000, 2)) == Result::RejectedOutsideSpace,
        L"memwb cache: a range running past the last page is rejected");
    suite.expect(ranges.MarkInFlight(Range(0x20000, 1)) == Result::RejectedOutsideSpace,
        L"memwb cache: a range after the space is rejected");
    suite.expect(ranges.InFlightPageCount() == 0U, L"memwb cache: rejected ranges register nothing");
    suite.expect(ranges.MarkInFlight(Range(0x1F000, 1)) == Result::Accepted && ranges.MarkInFlight(Range(0x1E000, 2)) == Result::Accepted,
        L"memwb cache: ranges ending exactly at the last page are accepted");
    suite.expect(ranges.InFlightPageCount() == 2U, L"memwb cache: pages 0x1E000 and 0x1F000 are in flight (0x1F000 registered twice)");

    // 容量边界：容量 4 的视图，4 页合法、5 页拒绝。
    HexViewport tiny(0x10000, 0x1FFFF, 16, 4);
    suite.expect(tiny.MarkInFlight(Range(0x10000, 4)) == Result::Accepted && tiny.MarkInFlight(Range(0x10000, 5)) == Result::RejectedBadSize,
        L"memwb cache: the range limit is the cache capacity, on both sides");

    // 整体失败原子：第二页越界时第一页也不能被标成不可读。
    HexViewport atomic(0x10000, 0x1FFFF);
    suite.expect(atomic.MarkUnreadable(Range(0x1F000, 2), 0) == Result::RejectedOutsideSpace
        && atomic.LookupByte(0x1F000).state == State::NotLoaded && atomic.CachedPageCount() == 0U,
        L"memwb cache: a rejected unreadable range marks none of its pages");
}

// ------------------------------------------------------------
// 四、LRU 淘汰：容量 3，逐步核对淘汰次序。
// ------------------------------------------------------------
void TestLruEviction(KswordTests::Suite& suite) {
    const std::uint64_t pageA = 0x10000ULL;  // 页 A 的起始地址
    const std::uint64_t pageB = 0x11000ULL;  // 页 B 的起始地址
    const std::uint64_t pageC = 0x12000ULL;  // 页 C 的起始地址
    const std::uint64_t pageD = 0x13000ULL;  // 页 D 的起始地址
    const std::uint64_t pageE = 0x14000ULL;  // 页 E 的起始地址
    const std::uint64_t pageF = 0x15000ULL;  // 页 F 的起始地址
    HexViewport view(0x10000, 0x1FFFF, 16, 3);  // 容量只有 3 页的视图，用来观察淘汰次序
    suite.expect(view.MaxCachedPages() == 3U, L"memwb cache: the capacity can be overridden for tests");

    // 先装满：次序就是插入次序，没有淘汰。
    suite.expect(Insert(view, pageA, 0, 0) == Result::Accepted && Insert(view, pageB, 1, 0) == Result::Accepted
        && Insert(view, pageC, 2, 0) == Result::Accepted, L"memwb cache: three pages fit");
    suite.expect(view.CachedPageStartsLeastRecentFirst() == std::vector<std::uint64_t>({ pageA, pageB, pageC })
        && view.Stats().evictions == 0ULL, L"memwb cache: the eviction order starts as insertion order");

    // 读 A 命中：A 变成最近使用，次序 [B, C, A]。
    suite.expect(view.LookupByte(pageA).state == State::Valid
        && view.CachedPageStartsLeastRecentFirst() == std::vector<std::uint64_t>({ pageB, pageC, pageA }),
        L"memwb cache: a hit refreshes the page to most recently used");

    // 只读版本不刷新；未命中的查询也不改变次序。
    static_cast<void>(view.PeekByte(pageB));
    static_cast<void>(view.LookupByte(pageD));
    suite.expect(view.CachedPageStartsLeastRecentFirst() == std::vector<std::uint64_t>({ pageB, pageC, pageA }),
        L"memwb cache: peeking and missing do not change the eviction order");

    // 插入 D：容量满，淘汰最久未用的 B（不是插入最早的 A，因为 A 刚被读过）。
    suite.expect(Insert(view, pageD, 3, 0) == Result::Accepted && !view.IsPageCached(pageB)
        && view.CachedPageStartsLeastRecentFirst() == std::vector<std::uint64_t>({ pageC, pageA, pageD }),
        L"memwb cache: the least recently used page (B) is evicted, not the oldest inserted (A)");
    suite.expect(view.LookupByte(pageB).state == State::NotLoaded && view.Stats().evictions == 1ULL,
        L"memwb cache: an evicted page reads as not loaded, and the eviction is counted");

    // 替换已有页：原地更新，不淘汰任何页，并成为最近使用。
    suite.expect(Insert(view, pageA, 9, 0) == Result::Accepted && view.Stats().evictions == 1ULL
        && view.CachedPageCount() == 3U
        && view.CachedPageStartsLeastRecentFirst() == std::vector<std::uint64_t>({ pageC, pageD, pageA }),
        L"memwb cache: replacing a cached page evicts nothing and refreshes it");
    suite.expect(view.LookupByte(pageA).value == 0x0C, L"memwb cache: the replaced page holds the new content (3+9 = 12)");

    // 插入 E：淘汰 C。
    suite.expect(Insert(view, pageE, 4, 0) == Result::Accepted && !view.IsPageCached(pageC)
        && view.CachedPageStartsLeastRecentFirst() == std::vector<std::uint64_t>({ pageD, pageA, pageE })
        && view.Stats().evictions == 2ULL, L"memwb cache: inserting E evicts C");

    // 不可读页同样占名额也参与淘汰：标 D 不可读(D 变最近) -> [A, E, D]；再标新页 F 不可读，淘汰 A。
    suite.expect(view.MarkUnreadable(Range(pageD, 1), 0) == Result::Accepted
        && view.CachedPageStartsLeastRecentFirst() == std::vector<std::uint64_t>({ pageA, pageE, pageD }),
        L"memwb cache: marking an existing page unreadable refreshes it");
    suite.expect(view.MarkUnreadable(Range(pageF, 1), 0) == Result::Accepted && !view.IsPageCached(pageA)
        && view.CachedPageStartsLeastRecentFirst() == std::vector<std::uint64_t>({ pageE, pageD, pageF })
        && view.Stats().evictions == 3ULL, L"memwb cache: a new unreadable page takes a slot and evicts A");

    // 命中已知不可读页也算命中并刷新：读 E -> [D, F, E]。
    suite.expect(view.LookupByte(pageE).state == State::Valid
        && view.CachedPageStartsLeastRecentFirst() == std::vector<std::uint64_t>({ pageD, pageF, pageE }),
        L"memwb cache: hits keep refreshing recency");
    suite.expect(view.LookupByte(pageD).state == State::Unreadable
        && view.CachedPageStartsLeastRecentFirst() == std::vector<std::uint64_t>({ pageF, pageE, pageD }),
        L"memwb cache: a hit on an unreadable page refreshes it too");

    // 在途登记不占缓存名额：登记 3 页在途，缓存页数不变、也不引发淘汰。
    suite.expect(view.MarkInFlight(Range(0x16000, 3)) == Result::Accepted && view.CachedPageCount() == 3U
        && view.Stats().evictions == 3ULL, L"memwb cache: in-flight marks do not consume cache slots");
}

// ------------------------------------------------------------
// 五、预取规划：去重、夹取、按距离排序。
// 空间 0x10000..0x10FFFF = 1 MiB = 256 页，页 k 起点 = 0x10000 + k*0x1000；行宽 16 时每页 256 行。
// ------------------------------------------------------------
void TestPlanFetch(KswordTests::Suite& suite) {
    HexViewport view(0x10000, 0x10FFFF);
    suite.expect(view.RowCount() == 65536ULL, L"memwb cache: 1 MiB at width 16 is 65536 rows");

    // (a) 视口在最顶：行 0..15 = 0x10000..0x100FF，只落在页 0；预取 2 页 -> 页 1、2。
    const auto planA = view.PlanFetch(0, 16, 2);
    suite.expect(planA.size() == 2U && PlanEntryIs(planA[0], 0x10000, 1, 0) && PlanEntryIs(planA[1], 0x11000, 2, 1),
        L"memwb cache: a viewport on page 0 plans the visible page then two prefetch pages");

    // (b) 视口在中间：行 2760..2859，起点 0x10000+2760*16 = 页 10 内，末行落在页 11；预取 2 页 -> 页 8..13。
    // 可见页[10,11]距离 0；后方 [12,13] 距离 1；前方 [8,9] 距离 1；距离相同时向后的在前。
    const auto planB = view.PlanFetch(2760, 100, 2);
    suite.expect(planB.size() == 3U && PlanEntryIs(planB[0], 0x1A000, 2, 0) && PlanEntryIs(planB[1], 0x1C000, 2, 1)
        && PlanEntryIs(planB[2], 0x18000, 2, 1),
        L"memwb cache: visible pages first, then the forward prefetch, then the backward prefetch");

    // (c) 页 11 已缓存、页 9 已在途：可见页只剩 [10]；后方 [12,13]；前方只剩 [8]，距离 10-8 = 2。
    HexViewport busy(0x10000, 0x10FFFF);
    suite.expect(Insert(busy, 0x1B000, 0, 0) == Result::Accepted && busy.MarkInFlight(Range(0x19000, 1)) == Result::Accepted,
        L"memwb cache: setup for de-duplication");
    const auto planC = busy.PlanFetch(2760, 100, 2);
    suite.expect(planC.size() == 3U && PlanEntryIs(planC[0], 0x1A000, 1, 0) && PlanEntryIs(planC[1], 0x1C000, 2, 1)
        && PlanEntryIs(planC[2], 0x18000, 1, 2),
        L"memwb cache: cached and in-flight pages are not requested again");

    // (d) 页 12 已缓存：后方只剩 [13]，距离 13-11 = 2；前方 [8,9] 距离 1。近的先于远的，与方向无关。
    HexViewport behind(0x10000, 0x10FFFF);
    suite.expect(Insert(behind, 0x1C000, 0, 0) == Result::Accepted, L"memwb cache: setup for nearest-first ordering");
    const auto planD = behind.PlanFetch(2760, 100, 2);
    suite.expect(planD.size() == 3U && PlanEntryIs(planD[0], 0x1A000, 2, 0) && PlanEntryIs(planD[1], 0x18000, 2, 1)
        && PlanEntryIs(planD[2], 0x1D000, 1, 2),
        L"memwb cache: a nearer backward run is planned before a farther forward run");

    // (e) 夹取：顶部之前没有页可预取；底部之后同理。
    const auto planE = view.PlanFetch(0, 16, 5);
    suite.expect(planE.size() == 2U && PlanEntryIs(planE[0], 0x10000, 1, 0) && PlanEntryIs(planE[1], 0x11000, 5, 1),
        L"memwb cache: prefetch before the first page is clamped away");
    const auto planF = view.PlanFetch(65520, 16, 5);
    suite.expect(planF.size() == 2U && PlanEntryIs(planF[0], 0x10F000, 1, 0) && PlanEntryIs(planF[1], 0x10A000, 5, 1),
        L"memwb cache: prefetch past the last page is clamped away (pages 250..254 before page 255)");
    const auto planG = view.PlanFetch(65530, 1000, 0);
    suite.expect(planG.size() == 1U && PlanEntryIs(planG[0], 0x10F000, 1, 0),
        L"memwb cache: visible rows past the end are clamped to the last row");

    // (f) 无需读取的输入：起始行越界、可见行数为 0。
    suite.expect(view.PlanFetch(65536, 16, 2).empty() && view.PlanFetch(kTop, 16, 2).empty(),
        L"memwb cache: a first row past the end plans nothing");
    suite.expect(view.PlanFetch(0, 0, 2).empty(), L"memwb cache: zero visible rows plan nothing");
    suite.expect(view.PlanFetch(65535, 1, 0).size() == 1U, L"memwb cache: the very last row plans its page");

    // (g) 不预取：只有可见页。
    const auto planH = view.PlanFetch(2760, 100, 0);
    suite.expect(planH.size() == 1U && PlanEntryIs(planH[0], 0x1A000, 2, 0), L"memwb cache: zero prefetch plans only the visible pages");

    // (h) PlanFetch 是纯查询：不登记在途。
    suite.expect(view.InFlightPageCount() == 0U, L"memwb cache: planning does not mark anything in flight");

    // (i) 去重闭环：登记计划里的每一段后再规划为空；换代次后又回到原计划。
    for (const HexViewport::FetchRange& range : planA) {
        suite.expect(view.MarkInFlight(range) == Result::Accepted, L"memwb cache: planned ranges are valid to mark");
    }
    suite.expect(view.PlanFetch(0, 16, 2).empty(), L"memwb cache: once marked in flight, the same plan is empty");
    view.InvalidateAll(1);
    const auto planAgain = view.PlanFetch(0, 16, 2);
    suite.expect(planAgain == planA, L"memwb cache: after invalidation the original plan comes back");

    // (j) 已知不可读的页不再请求（否则会无限重试）：页 1 标不可读后，后方只剩页 2，距离 2。
    HexViewport unreadable(0x10000, 0x10FFFF);
    suite.expect(unreadable.MarkUnreadable(Range(0x11000, 1), 0) == Result::Accepted, L"memwb cache: setup unreadable page");
    const auto planJ = unreadable.PlanFetch(0, 16, 2);
    suite.expect(planJ.size() == 2U && PlanEntryIs(planJ[0], 0x10000, 1, 0) && PlanEntryIs(planJ[1], 0x12000, 1, 2),
        L"memwb cache: a known-unreadable page is not requested again");

    // (k) 空间起止不页对齐：0x10800..0x12FFF，页 0x10000 起点早于首地址，仍要读（读取方只取空间内的部分）。
    HexViewport ragged(0x10800, 0x12FFF);
    suite.expect(ragged.RowCount() == 640ULL, L"memwb cache: 0x10800..0x12FFF is 640 rows");
    const auto planK1 = ragged.PlanFetch(0, 1, 0);
    suite.expect(planK1.size() == 1U && PlanEntryIs(planK1[0], 0x10000, 1, 0),
        L"memwb cache: the first page starts before the space and is still planned from its page start");
    const auto planK2 = ragged.PlanFetch(639, 1, 5);
    suite.expect(planK2.size() == 2U && PlanEntryIs(planK2[0], 0x12000, 1, 0) && PlanEntryIs(planK2[1], 0x10000, 2, 1),
        L"memwb cache: the last page plans two pages before it and nothing after");
}

// ------------------------------------------------------------
// 六、预取受缓存容量约束：规划的总页数不能超过容量，否则会把可见页自己挤出去。
// ------------------------------------------------------------
void TestPlanFetchCapacity(KswordTests::Suite& suite) {
    HexViewport big(0x10000, 0x10FFFF);

    // 预取页数取 UINT64_MAX：不溢出，总页数恰好等于容量 256（也就是整个 1 MiB）。
    const auto hugeTop = big.PlanFetch(0, 16, kTop);
    suite.expect(hugeTop.size() == 2U && PlanEntryIs(hugeTop[0], 0x10000, 1, 0) && PlanEntryIs(hugeTop[1], 0x11000, 255, 1),
        L"memwb cache: an enormous prefetch from the top plans exactly the whole 256-page space");
    const auto hugeMiddle = big.PlanFetch(2760, 100, kTop);
    std::uint64_t totalPages = 0;
    for (const HexViewport::FetchRange& range : hugeMiddle) {
        totalPages += range.pageCount;
    }
    suite.expect(totalPages == 256ULL, L"memwb cache: an enormous prefetch from the middle still stays within the capacity");

    // 容量 4、可见页 [10,11]：剩余名额 2，向后 1 页、向前 1 页 -> 窗口 [9..12]。
    HexViewport four(0x10000, 0x10FFFF, 16, 4);
    const auto planEven = four.PlanFetch(2760, 100, 100);
    suite.expect(planEven.size() == 3U && PlanEntryIs(planEven[0], 0x1A000, 2, 0) && PlanEntryIs(planEven[1], 0x1C000, 1, 1)
        && PlanEntryIs(planEven[2], 0x19000, 1, 1),
        L"memwb cache: a full cache budget is split between the two sides");

    // 容量 5：剩余名额 3，向后多分 1 页 -> 后 2 页、前 1 页 -> 窗口 [9..13]。
    HexViewport five(0x10000, 0x10FFFF, 16, 5);
    const auto planOdd = five.PlanFetch(2760, 100, 100);
    suite.expect(planOdd.size() == 3U && PlanEntryIs(planOdd[0], 0x1A000, 2, 0) && PlanEntryIs(planOdd[1], 0x1C000, 2, 1)
        && PlanEntryIs(planOdd[2], 0x19000, 1, 1),
        L"memwb cache: an odd leftover budget favours the forward side");

    // 容量 4、视口在最顶：前方没有页，名额全部给后方 -> 窗口 [0..3]。
    const auto planTop = four.PlanFetch(0, 16, 100);
    suite.expect(planTop.size() == 2U && PlanEntryIs(planTop[0], 0x10000, 1, 0) && PlanEntryIs(planTop[1], 0x11000, 3, 1),
        L"memwb cache: the budget one side cannot use goes to the other side");

    // 容量 4、视口在最底：对称，名额全部给前方 -> 页 252..255。
    const auto planBottom = four.PlanFetch(65520, 16, 100);
    suite.expect(planBottom.size() == 2U && PlanEntryIs(planBottom[0], 0x10F000, 1, 0) && PlanEntryIs(planBottom[1], 0x10C000, 3, 1),
        L"memwb cache: at the bottom the whole leftover budget goes backwards (pages 252..254)");

    // 可见页本身占满容量：容量 2、可见 [10,11] -> 只取这两页，不预取；容量 1 -> 只取页 10。
    HexViewport two(0x10000, 0x10FFFF, 16, 2);
    const auto planTwo = two.PlanFetch(2760, 100, 5);
    suite.expect(planTwo.size() == 1U && PlanEntryIs(planTwo[0], 0x1A000, 2, 0),
        L"memwb cache: visible pages that exactly fill the cache get no prefetch");
    HexViewport one(0x10000, 0x10FFFF, 16, 1);
    const auto planOne = one.PlanFetch(2760, 100, 5);
    suite.expect(planOne.size() == 1U && PlanEntryIs(planOne[0], 0x1A000, 1, 0),
        L"memwb cache: visible pages beyond the capacity are truncated to the capacity");
}

// ------------------------------------------------------------
// 七、地址空间两端的页：UINT64_MAX 所在页的"结束地址 + 1"会溢出，必须不出事。
// ------------------------------------------------------------
void TestPagesAtAddressSpaceEnds(KswordTests::Suite& suite) {
    // 高端：最后两页 0xFFFFFFFFFFFFE000 与 0xFFFFFFFFFFFFF000，行宽 16，共 512 行。
    HexViewport top(0xFFFFFFFFFFFFE000ULL, kTop);
    suite.expect(top.RowCount() == 512ULL, L"memwb cache: the last 8 KiB of the address space is 512 rows");
    const auto planTop = top.PlanFetch(0, 10, 3);
    suite.expect(planTop.size() == 2U && PlanEntryIs(planTop[0], 0xFFFFFFFFFFFFE000ULL, 1, 0)
        && PlanEntryIs(planTop[1], 0xFFFFFFFFFFFFF000ULL, 1, 1),
        L"memwb cache: prefetch at the top of the address space stops at the last page");
    const auto planLast = top.PlanFetch(511, 5, 0);
    suite.expect(planLast.size() == 1U && PlanEntryIs(planLast[0], 0xFFFFFFFFFFFFF000ULL, 1, 0),
        L"memwb cache: the last row plans the last page");

    // 最后一页可以插入、读取：偏移 0xFFF 的字节 = 4095*7+3 = 28668，28668 mod 256 = 252 = 0xFC。
    suite.expect(Insert(top, 0xFFFFFFFFFFFFF000ULL, 0, 0) == Result::Accepted, L"memwb cache: the last page of the address space is accepted");
    suite.expect(top.LookupByte(kTop).state == State::Valid && top.LookupByte(kTop).value == 0xFC,
        L"memwb cache: UINT64_MAX reads the last byte of the last page (0xFC)");
    suite.expect(top.LookupByte(0xFFFFFFFFFFFFF000ULL).value == 0x03, L"memwb cache: the first byte of the last page is 0x03");
    suite.expect(top.LookupByte(0xFFFFFFFFFFFFEFFFULL).state == State::NotLoaded,
        L"memwb cache: the byte just before the last page is not loaded");

    // 范围校验在上沿不溢出：整个空间 2 页合法；从最后一页起 2 页越界。
    suite.expect(top.MarkInFlight(Range(0xFFFFFFFFFFFFE000ULL, 2)) == Result::Accepted,
        L"memwb cache: a range covering the last two pages is accepted");
    suite.expect(top.MarkInFlight(Range(0xFFFFFFFFFFFFF000ULL, 2)) == Result::RejectedOutsideSpace,
        L"memwb cache: a range running past the end of the address space is rejected, not wrapped");
    suite.expect(top.MarkUnreadable(Range(0xFFFFFFFFFFFFF000ULL, 1), 0) == Result::Accepted
        && top.LookupByte(kTop).state == State::Unreadable,
        L"memwb cache: the last page can be marked unreadable");

    // 低端：0..0x1FFF，页 0 与页 0x1000。
    HexViewport low(0, 0x1FFF);
    suite.expect(Insert(low, 0, 0, 0) == Result::Accepted && low.LookupByte(0).value == 0x03,
        L"memwb cache: page 0 is accepted and address 0 reads back");
    suite.expect(low.MarkInFlight(Range(0, 2)) == Result::Accepted && low.MarkInFlight(Range(0, 3)) == Result::RejectedOutsideSpace,
        L"memwb cache: ranges at the bottom are validated on both sides");
    suite.expect(HexViewport::PageStartOf(0x1FFF) == 0x1000ULL && HexViewport::PageStartOf(kTop) == 0xFFFFFFFFFFFFF000ULL,
        L"memwb cache: PageStartOf aligns down without overflow");
}

// ------------------------------------------------------------
// 八、计数器：命中/未命中只统计 LookupByte，PeekByte 与空间外地址不计。
// ------------------------------------------------------------
void TestStats(KswordTests::Suite& suite) {
    HexViewport view(0x10000, 0x1FFFF);
    static_cast<void>(view.LookupByte(0x10000));      // 未加载：未命中 1
    suite.expect(view.MarkInFlight(Range(0x10000, 1)) == Result::Accepted, L"memwb cache: stats setup");
    static_cast<void>(view.LookupByte(0x10000));      // 在途：未命中 2
    suite.expect(Insert(view, 0x10000, 0, 0) == Result::Accepted, L"memwb cache: stats setup page");
    static_cast<void>(view.LookupByte(0x10000));      // 命中 1
    suite.expect(view.MarkUnreadable(Range(0x11000, 1), 0) == Result::Accepted, L"memwb cache: stats setup unreadable");
    static_cast<void>(view.LookupByte(0x11000));      // 不可读页也是命中 2
    static_cast<void>(view.LookupByte(0x20000));      // 空间之外：不计
    static_cast<void>(view.PeekByte(0x10000));        // 只读：不计
    suite.expect(view.Stats().hits == 2ULL && view.Stats().misses == 2ULL && view.Stats().evictions == 0ULL,
        L"memwb cache: two hits and two misses were counted; peeks and outside addresses were not");

    view.ResetStats();
    suite.expect(view.Stats().hits == 0ULL && view.Stats().misses == 0ULL && view.CachedPageCount() == 2U,
        L"memwb cache: resetting the counters keeps the cache");
}

// ------------------------------------------------------------
// 九、命中率基准（计数断言，不是计时断言）。
// 空间：0x10000000 起 1 MiB，256 页，行宽 16，每页 256 行；页 k 起点 = 0x10000000 + k*0x1000。
// FrameSim 模拟绘制层 + 异步读取方：每帧按"查字节 / 规划 / 登记在途 / 交付结果"的次序驱动视图。
// ------------------------------------------------------------
constexpr std::uint64_t kBenchFirst = 0x10000000ULL;
constexpr std::uint64_t kBenchBytes = 0x100000ULL;

class FrameSim {
public:
    // visibleRows：每帧可见行数；prefetch：预取页数；synchronous：为真则读取在同一帧内立即完成，
    // 为假则结果推迟一帧才到（模拟异步延迟）。
    FrameSim(HexViewport& view, std::uint64_t visibleRows, std::uint64_t prefetch, bool synchronous)
        : view_(view), visibleRows_(visibleRows), prefetch_(prefetch), synchronous_(synchronous) {}

    // Frame：以 firstRow 为首行驱动一帧。
    void Frame(std::uint64_t firstRow) {
        if (synchronous_) {
            PlanAndMark(firstRow);
            Deliver();
            Paint(firstRow);
        } else {
            Deliver();
            Paint(firstRow);
            PlanAndMark(firstRow);
        }
        ++frames_;
    }

    // pagesRequested：累计向"读取方"请求过的页数。
    std::uint64_t pagesRequested() const { return pagesRequested_; }

    // frames：已驱动的帧数。
    std::uint64_t frames() const { return frames_; }

    // wrongValues：绘制时发现的"状态是 Valid 但值与生成公式不符"的次数，必须为 0。
    std::uint64_t wrongValues() const { return wrongValues_; }

    // lookupsPerFrame：每帧查询的字节数 = 可见行数 * 2（每行查首尾两列）。
    std::uint64_t lookupsPerFrame() const { return visibleRows_ * 2ULL; }

private:
    // 页内容生成公式：seed = 页号低 8 位，字节 = (偏移*7 + 3 + seed) mod 256。
    static std::uint32_t SeedOf(std::uint64_t pageStart) {
        return static_cast<std::uint32_t>((pageStart / kPage) & 0xFFULL);
    }

    // PlanAndMark：规划并登记在途，把请求排进待交付队列。
    void PlanAndMark(std::uint64_t firstRow) {
        for (const HexViewport::FetchRange& range : view_.PlanFetch(firstRow, visibleRows_, prefetch_)) {
            if (view_.MarkInFlight(range) == Result::Accepted) {
                pending_.push_back(range);
                pagesRequested_ += range.pageCount;
            }
        }
    }

    // Deliver：把待交付的请求全部以当前代次插入缓存。
    void Deliver() {
        for (const HexViewport::FetchRange& range : pending_) {
            for (std::uint64_t index = 0; index < range.pageCount; ++index) {
                const std::uint64_t pageStart = range.firstPageStart + index * kPage;
                view_.InsertPage(pageStart, MakeBytes(SeedOf(pageStart)), FullMask(), view_.SourceRevision());
            }
        }
        pending_.clear();
    }

    // Paint：逐可见行查首尾两列；Valid 的字节核对生成公式。
    void Paint(std::uint64_t firstRow) {
        for (std::uint64_t row = firstRow; row < firstRow + visibleRows_; ++row) {
            for (const std::uint64_t column : { 0ULL, 15ULL }) {
                const std::uint64_t address = kBenchFirst + row * 16ULL + column;
                const HexViewport::ByteLookup found = view_.LookupByte(address);
                const std::uint64_t pageStart = address - (address % kPage);
                const std::uint32_t offset = static_cast<std::uint32_t>(address - pageStart);
                const std::uint8_t expected = static_cast<std::uint8_t>((offset * 7U + 3U + SeedOf(pageStart)) % 256U);
                if (found.state == State::Valid && found.value != expected) {
                    ++wrongValues_;
                }
            }
        }
    }

    HexViewport& view_;                              // 被驱动的视图
    std::uint64_t visibleRows_;                      // 每帧可见行数
    std::uint64_t prefetch_;                         // 预取页数
    bool synchronous_;                               // 读取是否同帧完成
    std::vector<HexViewport::FetchRange> pending_;   // 已登记在途、尚未交付的请求
    std::uint64_t pagesRequested_ = 0;               // 累计请求页数
    std::uint64_t frames_ = 0;                       // 已驱动帧数
    std::uint64_t wrongValues_ = 0;                  // 值核对失败次数
};

void TestCacheHitBenchmark(KswordTests::Suite& suite) {
    // ---- A：默认容量 256，预取 2，可见 16 行(256 字节)，每帧下滚 16 行，读取延迟一帧 ----
    // 帧 f 的可见页 = f/16。前进 4096 帧（首行 0,16,...,65520）覆盖整个 1 MiB，再后退 4095 帧回到 0。
    HexViewport viewA(kBenchFirst, kBenchFirst + kBenchBytes - 1);
    FrameSim simA(viewA, 16, 2, false);
    for (std::uint64_t firstRow = 0; firstRow <= 65520ULL; firstRow += 16ULL) {
        simA.Frame(firstRow);
    }
    // 前进一遍：每页只请求一次（共 256 页），不淘汰；只有第一帧未命中（16 行 * 2 列 = 32 次），之后全部命中。
    suite.expect(simA.frames() == 4096ULL && simA.pagesRequested() == 256ULL,
        L"memwb cache bench: a 1 MiB sequential scroll requests each of the 256 pages exactly once");
    suite.expect(viewA.Stats().evictions == 0ULL && viewA.CachedPageCount() == 256U,
        L"memwb cache bench: the whole 1 MiB fits in the default cache without eviction");
    suite.expect(viewA.Stats().misses == 32ULL && viewA.Stats().hits == 4096ULL * 32ULL - 32ULL,
        L"memwb cache bench: with a 2-page lead only the very first frame misses (32 lookups)");
    suite.expect(simA.wrongValues() == 0ULL, L"memwb cache bench: every valid byte matches the generated content");

    // 往返：再回头滚一遍（4095 帧）：缓存已全驻留，不再请求任何页、不再淘汰、不再未命中。
    for (std::uint64_t firstRow = 65504ULL;; firstRow -= 16ULL) {
        simA.Frame(firstRow);
        if (firstRow == 0) {
            break;
        }
    }
    suite.expect(simA.frames() == 4096ULL + 4095ULL && simA.pagesRequested() == 256ULL,
        L"memwb cache bench: scrolling back requests no additional page");
    suite.expect(viewA.Stats().misses == 32ULL && viewA.Stats().evictions == 0ULL
        && viewA.Stats().hits == (4096ULL + 4095ULL) * 32ULL - 32ULL,
        L"memwb cache bench: scrolling back is 100% hits");
    suite.expect(simA.wrongValues() == 0ULL, L"memwb cache bench: the way back reads correct bytes too");

    // ---- B：容量 4，不预取，可见 256 行(正好一页)，读取同帧完成 ----
    // 前进 256 帧：256 页各请求一次，前 4 页占空槽，其余 252 页每次淘汰一页。
    // 后退 255 帧（页 254..0）：缓存里留着最后插入的 252..255，页 254、253、252 命中；
    // 页 251..0 共 252 页每页都要重读并各淘汰一页。合计请求 256+252 = 508 页，淘汰 252+252 = 504 页。
    HexViewport viewB(kBenchFirst, kBenchFirst + kBenchBytes - 1, 16, 4);
    FrameSim simB(viewB, 256, 0, true);
    for (std::uint64_t page = 0; page < 256ULL; ++page) {
        simB.Frame(page * 256ULL);
    }
    suite.expect(simB.pagesRequested() == 256ULL && viewB.Stats().evictions == 252ULL,
        L"memwb cache bench: a forward pass through 256 pages with a 4-page cache evicts 252 pages");
    suite.expect(viewB.Stats().misses == 0ULL && viewB.Stats().hits == 256ULL * simB.lookupsPerFrame(),
        L"memwb cache bench: synchronous reads make every paint a hit");
    for (std::uint64_t page = 254ULL;; --page) {
        simB.Frame(page * 256ULL);
        if (page == 0) {
            break;
        }
    }
    suite.expect(simB.pagesRequested() == 508ULL && viewB.Stats().evictions == 504ULL,
        L"memwb cache bench: scrolling back re-reads 252 pages because the 4-page cache kept only the last four");
    const std::vector<std::uint64_t> expectedOrder = {
        kBenchFirst + 3ULL * kPage, kBenchFirst + 2ULL * kPage, kBenchFirst + 1ULL * kPage, kBenchFirst };
    suite.expect(viewB.CachedPageStartsLeastRecentFirst() == expectedOrder,
        L"memwb cache bench: after scrolling back the cache holds pages 3,2,1,0 with page 0 most recent");
    suite.expect(simB.wrongValues() == 0ULL, L"memwb cache bench: the small cache never serves wrong bytes");

    // ---- C：循环访问 5 页，容量 4 对容量 5 ----
    // LRU 的经典病态：工作集比容量大 1，循环访问时每次都未命中。3 轮 * 5 页 = 15 次请求，淘汰 15-4 = 11 次。
    // 容量 5 恰好装下：只请求 5 次，没有淘汰。
    HexViewport viewC4(kBenchFirst, kBenchFirst + kBenchBytes - 1, 16, 4);
    FrameSim simC4(viewC4, 256, 0, true);
    HexViewport viewC5(kBenchFirst, kBenchFirst + kBenchBytes - 1, 16, 5);
    FrameSim simC5(viewC5, 256, 0, true);
    for (int cycle = 0; cycle < 3; ++cycle) {
        for (std::uint64_t page = 0; page < 5ULL; ++page) {
            simC4.Frame(page * 256ULL);
            simC5.Frame(page * 256ULL);
        }
    }
    suite.expect(simC4.pagesRequested() == 15ULL && viewC4.Stats().evictions == 11ULL,
        L"memwb cache bench: cycling over 5 pages with a 4-page LRU cache misses every time");
    suite.expect(simC5.pagesRequested() == 5ULL && viewC5.Stats().evictions == 0ULL,
        L"memwb cache bench: cycling over 5 pages with a 5-page cache reads each page once");
}

}  // namespace

void RunMemwbViewportCacheTests(KswordTests::Suite& suite) {
    TestByteStates(suite);
    TestStaleRevision(suite);
    TestRejections(suite);
    TestLruEviction(suite);
    TestPlanFetch(suite);
    TestPlanFetchCapacity(suite);
    TestPagesAtAddressSpaceEnds(suite);
    TestStats(suite);
    TestCacheHitBenchmark(suite);
}
