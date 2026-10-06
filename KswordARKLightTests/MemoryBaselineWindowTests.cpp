// 基线窗口（shared/evidence/memory_workbench/MemoryBaselineWindow.h）的离线测试。
//
// 为什么这个模块值得一整套穷举断言：它属于**选错了不会报错**的那一类。
// 窗口选错之后，叠加层照样载入、画布照样上色，只是出现三种静默的坏结果：
//   * 窗口里混进了没读到的页 -> 那些字节被当成真实基线，编辑时"写前复核"拿零去比；
//   * 窗口越过了插入点所在页 / 地址空间边界 -> 后续写入落到根本没读过的地方，
//     或者 base + length 回绕，叠加层按"窗口之外"拒绝一切编辑；
//   * 滚动也重算窗口 -> RefreshBaseline 把"自己写入"标记反复清掉，用户刚写下的字节
//     立刻被当成外部篡改涂成青色，或者 base 一变"上次读取"全丢、青色永远不亮。
//
// 断言原则与 NumericTextParseTests.cpp 一致：
//   * 期望的 base / length 全部手算写死（含居中裁剪时左右各取几页），绝不从被测函数反算；
//   * 边界两侧都测（页内首字节/末字节、上限 -1/恰好/+1、地址空间边界内外各一字节）；
//   * 该被拒绝的输入必须被显式拒绝，且失败时 base 与 length 保持 0 的安全初值；
//   * 最后用一个与实现完全不同写法的暴力参照做随机对拍，并分别在低地址区与
//     地址空间顶端各跑一遍，让"终点不溢出"这条规则被真正走到。

#include "TestSupport.h"

#include "MemoryBaselineWindowTestSupport.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <set>

namespace {

using namespace MemwbBaselineTests;

using ksword::memwb::AddressSpaceBounds;
using ksword::memwb::BaselineRefeedDecision;
using ksword::memwb::BaselineSpan;
using ksword::memwb::BaselineSpanStatus;
using ksword::memwb::BaselineWindowPolicy;
using ksword::memwb::BaselineWindowRecord;
using ksword::memwb::DecideBaselineRefeed;
using ksword::memwb::kBaselineWindowDefaultMaxPages;
using ksword::memwb::kBaselineWindowDefaultPageSize;
using ksword::memwb::SelectBaselineSpan;

// ------------------------------------------------------------
// 一、常量、默认值与 BaselineSpan 的小方法。
// ------------------------------------------------------------
void TestConstantsAndDefaults(KswordTests::Suite& suite) {
    suite.expect(kBaselineWindowDefaultPageSize == 4096ULL, L"window: the default page size is 4096");
    suite.expect(kBaselineWindowDefaultMaxPages == 256ULL, L"window: the default (tentative) page limit is 256");

    // 默认构造的跨度是"无效的空跨度"：失败路径上忘记赋值的调用方拿到的是空。
    const BaselineSpan blank;
    suite.expect(blank.status == BaselineSpanStatus::InvalidArgument,
        L"window: a default span is InvalidArgument, not Ok");
    suite.expect(blank.base == 0 && blank.length == 0 && blank.IsEmpty(),
        L"window: a default span is empty at base 0");
    suite.expect(!blank.Contains(0) && !blank.Contains(kMax64),
        L"window: an empty span contains no address, including 0 and the maximum");

    const AddressSpaceBounds defaultBounds;
    suite.expect(defaultBounds.lowest == 0 && defaultBounds.highest == kMax64,
        L"window: default bounds cover the whole uint64 space");
    const BaselineWindowPolicy defaultPolicy;
    suite.expect(defaultPolicy.pageSize == 4096ULL && defaultPolicy.maxPages == 256ULL,
        L"window: the default policy is 4096-byte pages, 256 pages");

    // Contains / End：[base, base + length) 左闭右开，两侧各验一字节。
    BaselineSpan span;
    span.status = BaselineSpanStatus::Ok;
    span.base = 0x1000;
    span.length = 0x2000;
    suite.expect(span.Contains(0x1000) && span.Contains(0x2FFF),
        L"window: Contains accepts the first and the last byte");
    suite.expect(!span.Contains(0xFFF) && !span.Contains(0x3000),
        L"window: Contains rejects one byte before and one byte after");
    suite.expect(span.End() == 0x3000ULL && !span.IsEmpty(), L"window: End is base + length");

    // 紧贴地址空间顶端的跨度：Contains 不能因为 base + length 回绕而误判。
    BaselineSpan top;
    top.status = BaselineSpanStatus::Ok;
    top.base = 0xFFFFFFFFFFFFE000ULL;
    top.length = 0x1000;
    suite.expect(top.Contains(0xFFFFFFFFFFFFEFFFULL) && !top.Contains(0xFFFFFFFFFFFFF000ULL),
        L"window: Contains is correct for a span just below the top of the space");
    suite.expect(!top.Contains(0) && !top.Contains(0x1000),
        L"window: a span near the top does not wrap around to contain low addresses");
}

// ------------------------------------------------------------
// 二、插入点所在页：必须包含、必须已落定、页内位置无关。
// ------------------------------------------------------------
void TestInsertionPage(KswordTests::Suite& suite) {
    const PageSet single = MakePages({ 0x7FF612345000ULL });

    // 页内任意位置（首字节、中间、末字节）都选到同一页。
    ExpectSpan(suite, Select4K(0x7FF612345000ULL, single, 256), BaselineSpanStatus::Ok,
        0x7FF612345000ULL, 0x1000, L"window: the first byte of a page selects that page");
    ExpectSpan(suite, Select4K(0x7FF612345678ULL, single, 256), BaselineSpanStatus::Ok,
        0x7FF612345000ULL, 0x1000, L"window: a middle byte selects the page, base is page aligned");
    ExpectSpan(suite, Select4K(0x7FF612345FFFULL, single, 256), BaselineSpanStatus::Ok,
        0x7FF612345000ULL, 0x1000, L"window: the last byte of a page selects that page");

    // 页边界两侧：多一个字节就是邻页，邻页没落定就是空。
    ExpectSpan(suite, Select4K(0x7FF612346000ULL, single, 256), BaselineSpanStatus::InsertionPageNotSettled,
        0, 0, L"window: the first byte of the next page (not settled) gives an empty span");
    ExpectSpan(suite, Select4K(0x7FF612344FFFULL, single, 256), BaselineSpanStatus::InsertionPageNotSettled,
        0, 0, L"window: the last byte of the previous page (not settled) gives an empty span");

    // 插入点页没落定，即使两侧邻页都落定了也不能退而求其次。
    const PageSet gap = MakePages({ 0x10000ULL, 0x12000ULL });
    ExpectSpan(suite, Select4K(0x11000ULL, gap, 256), BaselineSpanStatus::InsertionPageNotSettled, 0, 0,
        L"window: an unsettled insertion page is not replaced by a settled neighbour");
    ExpectSpan(suite, Select4K(0x11FFFULL, gap, 256), BaselineSpanStatus::InsertionPageNotSettled, 0, 0,
        L"window: the same holds for the last byte of the unsettled page");

    // 空集合：什么页都没落定。
    ExpectSpan(suite, Select4K(0x10000ULL, PageSet{}, 256), BaselineSpanStatus::InsertionPageNotSettled, 0, 0,
        L"window: with no settled page at all the span is empty");

    // 第 0 页不特殊：它未落定时同样被拒绝，哪怕别的页已落定。
    ExpectSpan(suite, Select4K(0x10, MakePages({ 0x1000ULL }), 256), BaselineSpanStatus::InsertionPageNotSettled, 0, 0,
        L"window: an unsettled page 0 is rejected like any other page");

    // 地址 0 所在的第 0 页：下界处没有向下的越界。
    ExpectSpan(suite, Select4K(0, MakePages({ 0 }), 256), BaselineSpanStatus::Ok, 0, 0x1000,
        L"window: page 0 is selectable and the left side does not underflow");
    ExpectSpan(suite, Select4K(0xFFF, MakePages({ 0 }), 256), BaselineSpanStatus::Ok, 0, 0x1000,
        L"window: the last byte of page 0 selects page 0");
}

// ------------------------------------------------------------
// 三、连续性：只含围绕插入点页的连续一段，绝不跨过未落定页。
// ------------------------------------------------------------
void TestContiguity(KswordTests::Suite& suite) {
    // 0x10000 0x11000 连续，0x12000 缺，0x13000 0x14000 0x15000 连续。
    const PageSet pages = MakePages({ 0x10000ULL, 0x11000ULL, 0x13000ULL, 0x14000ULL, 0x15000ULL });

    ExpectSpan(suite, Select4K(0x11800ULL, pages, 256), BaselineSpanStatus::Ok, 0x10000ULL, 0x2000,
        L"window: the left run of two pages stops at the gap");
    ExpectSpan(suite, Select4K(0x10000ULL, pages, 256), BaselineSpanStatus::Ok, 0x10000ULL, 0x2000,
        L"window: the first page of the left run selects the whole left run");
    ExpectSpan(suite, Select4K(0x14000ULL, pages, 256), BaselineSpanStatus::Ok, 0x13000ULL, 0x3000,
        L"window: the right run of three pages is selected as one span");
    ExpectSpan(suite, Select4K(0x13FFFULL, pages, 256), BaselineSpanStatus::Ok, 0x13000ULL, 0x3000,
        L"window: the last byte of the first right-run page selects the same run");
    ExpectSpan(suite, Select4K(0x12000ULL, pages, 256), BaselineSpanStatus::InsertionPageNotSettled, 0, 0,
        L"window: the gap page itself is not selectable");

    // 隔了一页的两个孤页不会被连起来。
    const PageSet isolated = MakePages({ 0x10000ULL, 0x12000ULL });
    ExpectSpan(suite, Select4K(0x10000ULL, isolated, 256), BaselineSpanStatus::Ok, 0x10000ULL, 0x1000,
        L"window: pages separated by one missing page are not joined (left)");
    ExpectSpan(suite, Select4K(0x12000ULL, isolated, 256), BaselineSpanStatus::Ok, 0x12000ULL, 0x1000,
        L"window: pages separated by one missing page are not joined (right)");
}

// ------------------------------------------------------------
// 四、页数上限：恰好 / 少一页 / 多一页，居中裁剪与让位。
// ------------------------------------------------------------
void TestMaxPagesClipping(KswordTests::Suite& suite) {
    // 16 个连续页：0x10000 .. 0x1F000，第 i 页起点 0x10000 + i * 0x1000。
    const PageSet run = MakeRun(0x10000ULL, 16, 4096);

    // 偶数上限：左侧取 (4-1)/2 = 1 页，右侧取 2 页。第 8 页起点 0x18000。
    ExpectSpan(suite, Select4K(0x18000ULL, run, 4), BaselineSpanStatus::Ok, 0x17000ULL, 0x4000,
        L"window: limit 4 around page 8 takes 1 left + 2 right pages");
    // 奇数上限：两侧各 2 页。
    ExpectSpan(suite, Select4K(0x18000ULL, run, 5), BaselineSpanStatus::Ok, 0x16000ULL, 0x5000,
        L"window: limit 5 around page 8 takes 2 left + 2 right pages");
    ExpectSpan(suite, Select4K(0x18000ULL, run, 3), BaselineSpanStatus::Ok, 0x17000ULL, 0x3000,
        L"window: limit 3 around page 8 takes 1 left + 1 right page");
    ExpectSpan(suite, Select4K(0x18000ULL, run, 2), BaselineSpanStatus::Ok, 0x18000ULL, 0x2000,
        L"window: limit 2 takes 0 left + 1 right page (even limits favour the right)");
    ExpectSpan(suite, Select4K(0x18000ULL, run, 1), BaselineSpanStatus::Ok, 0x18000ULL, 0x1000,
        L"window: limit 1 is exactly the insertion page");

    // 靠近左端：左侧不够，名额让给右侧。
    ExpectSpan(suite, Select4K(0x10000ULL, run, 4), BaselineSpanStatus::Ok, 0x10000ULL, 0x4000,
        L"window: at the left end the unused left quota goes to the right (pages 0-3)");
    ExpectSpan(suite, Select4K(0x11000ULL, run, 4), BaselineSpanStatus::Ok, 0x10000ULL, 0x4000,
        L"window: one page from the left end still yields pages 0-3");
    // 靠近右端：右侧不够，名额让给左侧。第 15 页起点 0x1F000，第 14 页起点 0x1E000。
    ExpectSpan(suite, Select4K(0x1F000ULL, run, 4), BaselineSpanStatus::Ok, 0x1C000ULL, 0x4000,
        L"window: at the right end the unused right quota goes to the left (pages 12-15)");
    ExpectSpan(suite, Select4K(0x1E000ULL, run, 4), BaselineSpanStatus::Ok, 0x1C000ULL, 0x4000,
        L"window: one page from the right end still yields pages 12-15");

    // 上限等于 / 大于 / 小于连续段长度（16 页）的两侧。
    ExpectSpan(suite, Select4K(0x18000ULL, run, 16), BaselineSpanStatus::Ok, 0x10000ULL, 0x10000,
        L"window: limit equal to the run length selects the whole run");
    ExpectSpan(suite, Select4K(0x18000ULL, run, 17), BaselineSpanStatus::Ok, 0x10000ULL, 0x10000,
        L"window: limit one above the run length also selects the whole run");
    ExpectSpan(suite, Select4K(0x18000ULL, run, 100), BaselineSpanStatus::Ok, 0x10000ULL, 0x10000,
        L"window: a large limit never invents pages");
    ExpectSpan(suite, Select4K(0x10000ULL, run, 15), BaselineSpanStatus::Ok, 0x10000ULL, 0xF000,
        L"window: limit 15 at the left end keeps pages 0-14");
    ExpectSpan(suite, Select4K(0x1F000ULL, run, 15), BaselineSpanStatus::Ok, 0x11000ULL, 0xF000,
        L"window: limit 15 at the right end keeps pages 1-15");
    ExpectSpan(suite, Select4K(0x18000ULL, run, kMax64), BaselineSpanStatus::Ok, 0x10000ULL, 0x10000,
        L"window: the largest possible limit does not overflow limit - 1");

    // 默认上限 256 页，连续段 600 页（起点 0x100000，终点 0x358000）。
    const PageSet big = MakeRun(0x100000ULL, 600, 4096);
    // 插入点页序号 300，起点 0x22C000：左 127 页、右 128 页。
    ExpectSpan(suite, Select4K(0x22C123ULL, big, 256), BaselineSpanStatus::Ok, 0x1AD000ULL, 0x100000,
        L"window: 256 pages centred on page 300 start at 0x1AD000");
    // 插入点页序号 10，起点 0x10A000：左只有 10 页，其余 245 页给右侧。
    ExpectSpan(suite, Select4K(0x10A000ULL, big, 256), BaselineSpanStatus::Ok, 0x100000ULL, 0x100000,
        L"window: near the left end of a long run the window starts at the run start");
    // 插入点页序号 595，起点 0x100000 + 0x253000 = 0x353000：右只有 4 页，左取 251 页，
    // 起点 0x353000 - 251 * 0x1000 = 0x258000，终点 0x358000 恰是连续段终点。
    ExpectSpan(suite, Select4K(0x353000ULL, big, 256), BaselineSpanStatus::Ok, 0x258000ULL, 0x100000,
        L"window: near the right end of a long run the window ends at the run end");

    // 连续段恰好 256 页与 257 页：上限的两侧。
    const PageSet exact = MakeRun(0x100000ULL, 256, 4096);
    ExpectSpan(suite, Select4K(0x180000ULL, exact, 256), BaselineSpanStatus::Ok, 0x100000ULL, 0x100000,
        L"window: a run of exactly 256 pages is returned whole");
    const PageSet over = MakeRun(0x100000ULL, 257, 4096);
    ExpectSpan(suite, Select4K(0x100000ULL, over, 256), BaselineSpanStatus::Ok, 0x100000ULL, 0x100000,
        L"window: a 257-page run at its first page is clipped to pages 0-255");
    ExpectSpan(suite, Select4K(0x200000ULL, over, 256), BaselineSpanStatus::Ok, 0x101000ULL, 0x100000,
        L"window: a 257-page run at its last page is clipped to pages 1-256");
}

// ------------------------------------------------------------
// 五、页大小：非 4096 的页、非 2 的幂的页、1 字节页。
// ------------------------------------------------------------
void TestOtherPageSizes(KswordTests::Suite& suite) {
    const AddressSpaceBounds all = MakeBounds(0, kMax64);

    // 16 字节页：0x100 0x110 0x120 连续，插入点 0x11F 落在中间页。
    ExpectSpan(suite,
        SelectBaselineSpan(0x11F, MakePages({ 0x100ULL, 0x110ULL, 0x120ULL }), all, MakePolicy(16, 256)),
        BaselineSpanStatus::Ok, 0x100ULL, 0x30, L"window: 16-byte pages join into one 0x30-byte span");

    // 512 字节页（0x200）：页序号 1、2 相邻，序号 4 孤立。
    ExpectSpan(suite,
        SelectBaselineSpan(0x5FF, MakePages({ 0x200ULL, 0x400ULL, 0x800ULL }), all, MakePolicy(512, 256)),
        BaselineSpanStatus::Ok, 0x200ULL, 0x400, L"window: 512-byte pages 1 and 2 join, page 4 stays out");

    // 100 字节页（非 2 的幂）：页算术用除法与乘法，不靠掩码。
    const PageSet hundreds = MakePages({ 0, 100ULL, 200ULL, 400ULL });
    ExpectSpan(suite, SelectBaselineSpan(150, hundreds, all, MakePolicy(100, 256)),
        BaselineSpanStatus::Ok, 0, 300, L"window: 100-byte pages around byte 150 span 0..300");
    ExpectSpan(suite, SelectBaselineSpan(399, hundreds, all, MakePolicy(100, 256)),
        BaselineSpanStatus::InsertionPageNotSettled, 0, 0,
        L"window: byte 399 is in the unsettled page starting at 300");
    ExpectSpan(suite, SelectBaselineSpan(400, hundreds, all, MakePolicy(100, 256)),
        BaselineSpanStatus::Ok, 400ULL, 100, L"window: byte 400 starts the isolated page");

    // 1 字节页：最末字节 0xFF..FF 终点会回绕，所以不可选；它的前一个字节可选。
    const PageSet bytes = MakePages({ 5ULL, 6ULL, 7ULL, 9ULL, kMax64 - 1, kMax64 });
    ExpectSpan(suite, SelectBaselineSpan(6, bytes, all, MakePolicy(1, 256)),
        BaselineSpanStatus::Ok, 5ULL, 3, L"window: 1-byte pages 5,6,7 join into a 3-byte span");
    ExpectSpan(suite, SelectBaselineSpan(kMax64 - 1, bytes, all, MakePolicy(1, 256)),
        BaselineSpanStatus::Ok, kMax64 - 1, 1,
        L"window: the byte before the maximum is selectable, its end is exactly the maximum");
    ExpectSpan(suite, SelectBaselineSpan(kMax64, bytes, all, MakePolicy(1, 256)),
        BaselineSpanStatus::InsertionOutsideSpace, 0, 0,
        L"window: the very last byte is never selectable, even if it is settled");
}

// ------------------------------------------------------------
// 六、地址空间边界：整页才算数，两侧各验一字节。
// ------------------------------------------------------------
void TestBounds(KswordTests::Suite& suite) {
    const BaselineWindowPolicy policy = MakePolicy(4096, 256);

    // 用户态上界：最高字节 0x7FFFFFFEFFFF，最后一个合法页起点 0x7FFFFFFEF000。
    const AddressSpaceBounds user = MakeBounds(0x10000ULL, 0x7FFFFFFEFFFFULL);
    const PageSet topPages = MakePages({ 0x7FFFFFFEE000ULL, 0x7FFFFFFEF000ULL, 0x7FFFFFFF0000ULL });
    ExpectSpan(suite, SelectBaselineSpan(0x7FFFFFFEF123ULL, topPages, user, policy),
        BaselineSpanStatus::Ok, 0x7FFFFFFEE000ULL, 0x2000,
        L"window: a settled page above the upper bound is not pulled into the window");
    ExpectSpan(suite, SelectBaselineSpan(0x7FFFFFFEFFFFULL, topPages, user, policy),
        BaselineSpanStatus::Ok, 0x7FFFFFFEE000ULL, 0x2000,
        L"window: the highest addressable byte is selectable");
    ExpectSpan(suite, SelectBaselineSpan(0x7FFFFFFF0000ULL, topPages, user, policy),
        BaselineSpanStatus::InsertionOutsideSpace, 0, 0,
        L"window: one byte above the upper bound is outside the space even if settled");

    // 下界：0x10000 恰是最低页起点，0xF000 页在边界之外。
    const PageSet lowPages = MakePages({ 0xF000ULL, 0x10000ULL, 0x11000ULL });
    ExpectSpan(suite, SelectBaselineSpan(0x10000ULL, lowPages, user, policy),
        BaselineSpanStatus::Ok, 0x10000ULL, 0x2000,
        L"window: a settled page below the lower bound is not pulled into the window");
    ExpectSpan(suite, SelectBaselineSpan(0xFFFFULL, lowPages, user, policy),
        BaselineSpanStatus::InsertionOutsideSpace, 0, 0,
        L"window: one byte below the lower bound is outside the space");

    // 下界落在页中间：那一页只有半页，不选。
    const AddressSpaceBounds midLow = MakeBounds(0x10800ULL, kMax64);
    const PageSet three = MakePages({ 0x10000ULL, 0x11000ULL, 0x12000ULL });
    ExpectSpan(suite, SelectBaselineSpan(0x10900ULL, three, midLow, policy),
        BaselineSpanStatus::InsertionOutsideSpace, 0, 0,
        L"window: a page that only half fits above the lower bound is not selectable");
    ExpectSpan(suite, SelectBaselineSpan(0x10800ULL, three, midLow, policy),
        BaselineSpanStatus::InsertionOutsideSpace, 0, 0,
        L"window: even the lowest addressable byte is rejected when its page is partial");
    ExpectSpan(suite, SelectBaselineSpan(0x11000ULL, three, midLow, policy),
        BaselineSpanStatus::Ok, 0x11000ULL, 0x2000,
        L"window: the next whole page is selectable and the partial page is excluded");

    // 上界落在页中间：上界 0x12FFE 少一个字节，0x12000 页是半页；上界 0x12FFF 则整页。
    const PageSet twoPages = MakePages({ 0x11000ULL, 0x12000ULL });
    ExpectSpan(suite, SelectBaselineSpan(0x11800ULL, twoPages, MakeBounds(0, 0x12FFEULL), policy),
        BaselineSpanStatus::Ok, 0x11000ULL, 0x1000,
        L"window: a page one byte short of the upper bound is excluded");
    ExpectSpan(suite, SelectBaselineSpan(0x12500ULL, twoPages, MakeBounds(0, 0x12FFEULL), policy),
        BaselineSpanStatus::InsertionOutsideSpace, 0, 0,
        L"window: inserting into that partial page is rejected");
    ExpectSpan(suite, SelectBaselineSpan(0x11800ULL, twoPages, MakeBounds(0, 0x12FFFULL), policy),
        BaselineSpanStatus::Ok, 0x11000ULL, 0x2000,
        L"window: with the upper bound at the page end the page is whole and included");

    // 边界里恰好一页、差一个字节一页、半页。
    const PageSet one = MakePages({ 0x10000ULL });
    ExpectSpan(suite, SelectBaselineSpan(0x10000ULL, one, MakeBounds(0x10000ULL, 0x10FFFULL), policy),
        BaselineSpanStatus::Ok, 0x10000ULL, 0x1000,
        L"window: bounds that hold exactly one whole page select that page");
    ExpectSpan(suite, SelectBaselineSpan(0x10000ULL, one, MakeBounds(0x10000ULL, 0x10FFEULL), policy),
        BaselineSpanStatus::InsertionOutsideSpace, 0, 0,
        L"window: bounds one byte shorter than a page hold no selectable page");
    ExpectSpan(suite, SelectBaselineSpan(0x10900ULL, one, MakeBounds(0x10800ULL, 0x10FFFULL), policy),
        BaselineSpanStatus::InsertionOutsideSpace, 0, 0,
        L"window: bounds holding only half a page hold no selectable page");

    // 下界 0、上界 0xFFF：第 0 页，不能在下界处下溢。
    ExpectSpan(suite, SelectBaselineSpan(0, MakePages({ 0 }), MakeBounds(0, 0xFFF), policy),
        BaselineSpanStatus::Ok, 0, 0x1000, L"window: bounds [0, 0xFFF] hold exactly page 0");

    // 内核半区下界：低于分界的页不选。
    const AddressSpaceBounds kernel = MakeBounds(0xFFFF800000000000ULL, kMax64);
    const PageSet splitPages = MakePages(
        { 0xFFFF7FFFFFFFF000ULL, 0xFFFF800000000000ULL, 0xFFFF800000001000ULL });
    ExpectSpan(suite, SelectBaselineSpan(0xFFFF800000000010ULL, splitPages, kernel, policy),
        BaselineSpanStatus::Ok, 0xFFFF800000000000ULL, 0x2000,
        L"window: the page below the kernel split is not pulled into the window");
    ExpectSpan(suite, SelectBaselineSpan(0xFFFF7FFFFFFFF800ULL, splitPages, kernel, policy),
        BaselineSpanStatus::InsertionOutsideSpace, 0, 0,
        L"window: an address below the kernel split is outside the kernel space");
}

// ------------------------------------------------------------
// 七、溢出：贴近 UINT64_MAX 时 base + length 不回绕，最末一页永不入选。
// ------------------------------------------------------------
void TestOverflowNearTop(KswordTests::Suite& suite) {
    const BaselineWindowPolicy policy = MakePolicy(4096, 256);
    const AddressSpaceBounds kernel = MakeBounds(0xFFFF800000000000ULL, kMax64);
    const PageSet topPages = MakePages(
        { 0xFFFFFFFFFFFFD000ULL, 0xFFFFFFFFFFFFE000ULL, 0xFFFFFFFFFFFFF000ULL });

    // 最末一页（终点恰为 2^64）不入选，所以窗口终点是 0xFFFFFFFFFFFFF000，可表示。
    const BaselineSpan below = SelectBaselineSpan(0xFFFFFFFFFFFFE123ULL, topPages, kernel, policy);
    ExpectSpan(suite, below, BaselineSpanStatus::Ok, 0xFFFFFFFFFFFFD000ULL, 0x2000,
        L"window: near the top the last page is excluded and the span ends at 0xFFFFFFFFFFFFF000");
    suite.expect(below.End() == 0xFFFFFFFFFFFFF000ULL && below.End() > below.base,
        L"window: the end of a span near the top did not wrap around");

    // 插入点落在最末一页：该页不可选，返回越界（与叠加层"最末字节不可编辑"一致）。
    ExpectSpan(suite, SelectBaselineSpan(0xFFFFFFFFFFFFF800ULL, topPages, kernel, policy),
        BaselineSpanStatus::InsertionOutsideSpace, 0, 0,
        L"window: an insertion point in the very last page is rejected");
    ExpectSpan(suite, SelectBaselineSpan(0xFFFFFFFFFFFFF000ULL, topPages, kernel, policy),
        BaselineSpanStatus::InsertionOutsideSpace, 0, 0,
        L"window: the first byte of the very last page is rejected");
    ExpectSpan(suite, SelectBaselineSpan(kMax64, topPages, kernel, policy),
        BaselineSpanStatus::InsertionOutsideSpace, 0, 0,
        L"window: the maximum address is rejected");
    ExpectSpan(suite, SelectBaselineSpan(0xFFFFFFFFFFFFEFFFULL, topPages, kernel, policy),
        BaselineSpanStatus::Ok, 0xFFFFFFFFFFFFD000ULL, 0x2000,
        L"window: the last byte of the second-to-last page is still selectable");

    // 下界向上取整本身会溢出：lowest 在最末一页中间，没有任何整页。
    ExpectSpan(suite, SelectBaselineSpan(kMax64, topPages, MakeBounds(0xFFFFFFFFFFFFF800ULL, kMax64), policy),
        BaselineSpanStatus::InsertionOutsideSpace, 0, 0,
        L"window: rounding the lower bound up near the top does not overflow");
    ExpectSpan(suite, SelectBaselineSpan(0xFFFFFFFFFFFFF900ULL, topPages,
        MakeBounds(0xFFFFFFFFFFFFF001ULL, kMax64), policy),
        BaselineSpanStatus::InsertionOutsideSpace, 0, 0,
        L"window: a lower bound one byte into the last page leaves no whole page");

    // 页大小为 2^63：只有第 0 页（[0, 2^63)）合法，第 1 页终点是 2^64，不可表示。
    const BaselineWindowPolicy huge = MakePolicy(0x8000000000000000ULL, 256);
    const PageSet hugePages = MakePages({ 0, 0x8000000000000000ULL });
    ExpectSpan(suite, SelectBaselineSpan(5, hugePages, MakeBounds(0, kMax64), huge),
        BaselineSpanStatus::Ok, 0, 0x8000000000000000ULL,
        L"window: a 2^63-byte page 0 is selected and its length does not overflow");
    ExpectSpan(suite, SelectBaselineSpan(0x8000000000000005ULL, hugePages, MakeBounds(0, kMax64), huge),
        BaselineSpanStatus::InsertionOutsideSpace, 0, 0,
        L"window: the second 2^63-byte page ends at 2^64 and is excluded");
}

// ------------------------------------------------------------
// 八、参数不合法：显式拒绝且输出保持空。
// ------------------------------------------------------------
void TestInvalidArguments(KswordTests::Suite& suite) {
    const PageSet pages = MakePages({ 0x10000ULL });

    ExpectSpan(suite, SelectBaselineSpan(0x10000ULL, pages, MakeBounds(0, kMax64), MakePolicy(0, 256)),
        BaselineSpanStatus::InvalidArgument, 0, 0, L"window: page size 0 is rejected (no divide by zero)");
    ExpectSpan(suite, SelectBaselineSpan(0x10000ULL, pages, MakeBounds(0, kMax64), MakePolicy(4096, 0)),
        BaselineSpanStatus::InvalidArgument, 0, 0, L"window: a page limit of 0 is rejected");
    ExpectSpan(suite, SelectBaselineSpan(0x10000ULL, pages, MakeBounds(0x2000, 0x1FFF), MakePolicy(4096, 256)),
        BaselineSpanStatus::InvalidArgument, 0, 0, L"window: lowest above highest is rejected");

    // lowest == highest 是合法区间（单字节空间），只是 4096 字节的页装不下。
    ExpectSpan(suite, SelectBaselineSpan(0x10000ULL, pages, MakeBounds(0x10000ULL, 0x10000ULL), MakePolicy(4096, 256)),
        BaselineSpanStatus::InsertionOutsideSpace, 0, 0,
        L"window: a one-byte space is valid input but holds no 4096-byte page");
    ExpectSpan(suite,
        SelectBaselineSpan(0x10000ULL, MakePages({ 0x10000ULL }), MakeBounds(0x10000ULL, 0x10000ULL), MakePolicy(1, 256)),
        BaselineSpanStatus::Ok, 0x10000ULL, 1, L"window: a one-byte space holds exactly one 1-byte page");

    // 无效参数的优先级高于"没落定"：同样的空集合，参数错就报参数错。
    ExpectSpan(suite, SelectBaselineSpan(0x10000ULL, PageSet{}, MakeBounds(0, kMax64), MakePolicy(0, 256)),
        BaselineSpanStatus::InvalidArgument, 0, 0, L"window: invalid parameters are reported before settledness");
}

// ------------------------------------------------------------
// 九、已落定集合里的"非页对齐"元素一律不认。
// ------------------------------------------------------------
void TestUnalignedEntriesIgnored(KswordTests::Suite& suite) {
    // 只有一个不对齐的元素：它不是任何一页的起点。
    ExpectSpan(suite, Select4K(0x10800ULL, MakePages({ 0x10800ULL }), 256),
        BaselineSpanStatus::InsertionPageNotSettled, 0, 0,
        L"window: an unaligned entry does not settle the page that contains it");

    // 对齐页旁边的不对齐元素不能充当邻页。
    const PageSet mixed = MakePages({ 0x10000ULL, 0x11800ULL, 0x12001ULL });
    ExpectSpan(suite, Select4K(0x10000ULL, mixed, 256), BaselineSpanStatus::Ok, 0x10000ULL, 0x1000,
        L"window: unaligned entries do not extend the span");
    ExpectSpan(suite, Select4K(0x11800ULL, mixed, 256), BaselineSpanStatus::InsertionPageNotSettled, 0, 0,
        L"window: the page of an unaligned entry stays unsettled");
}

// ------------------------------------------------------------
// 十、确定性：同输入同输出，与集合的构造顺序无关，输入不被修改。
// ------------------------------------------------------------
void TestDeterminism(KswordTests::Suite& suite) {
    PageSet forward;
    for (std::uint64_t index = 0; index < 40; ++index) {
        forward.insert(0x40000ULL + index * 4096ULL);
    }
    PageSet backward;
    for (std::uint64_t index = 40; index > 0; --index) {
        backward.insert(0x40000ULL + (index - 1) * 4096ULL);
    }
    const PageSet snapshot = forward;

    const BaselineSpan first = Select4K(0x4A123ULL, forward, 8);
    const BaselineSpan second = Select4K(0x4A123ULL, forward, 8);
    const BaselineSpan reversed = Select4K(0x4A123ULL, backward, 8);

    // 插入点页序号 10，上限 8：左 (8-1)/2 = 3 页、右 4 页，起点 0x40000 + 7 * 0x1000 = 0x47000。
    ExpectSpan(suite, first, BaselineSpanStatus::Ok, 0x47000ULL, 0x8000,
        L"window: the determinism fixture selects pages 7-14");
    suite.expect(second.status == first.status && second.base == first.base && second.length == first.length,
        L"window: two calls with the same input return the same span");
    suite.expect(reversed.status == first.status && reversed.base == first.base && reversed.length == first.length,
        L"window: the result does not depend on how the settled set was built");
    suite.expect(forward == snapshot, L"window: the settled set is not modified by selection");
}

// ------------------------------------------------------------
// 十一、重算判定：脏了才重算，滚动不重算。
// ------------------------------------------------------------
void TestRefeedDecision(KswordTests::Suite& suite) {
    const BaselineWindowPolicy policy = MakePolicy(4096, 256);
    // 窗口 [0x10000, 0x14000) 共 4 页，来源代次 7。
    BaselineWindowRecord record;
    record.hasWindow = true;
    record.base = 0x10000ULL;
    record.length = 0x4000;
    record.sourceRevision = 7;
    const PageSet settled = MakeRun(0x10000ULL, 4, 4096);

    // 没有窗口：必须重算，记录里的其他字段无关。
    BaselineWindowRecord none = record;
    none.hasWindow = false;
    suite.expect(DecideBaselineRefeed(none, 0x12345ULL, 7, settled, policy) == BaselineRefeedDecision::Recompute,
        L"refeed: no window means recompute");

    // 一切如常：Keep。
    suite.expect(DecideBaselineRefeed(record, 0x12345ULL, 7, settled, policy) == BaselineRefeedDecision::Keep,
        L"refeed: nothing changed means keep");

    // 窗口内滚动（含首字节、末字节）不重算；窗口外一字节就重算。
    suite.expect(DecideBaselineRefeed(record, 0x10000ULL, 7, settled, policy) == BaselineRefeedDecision::Keep,
        L"refeed: an anchor on the first window byte is still inside");
    suite.expect(DecideBaselineRefeed(record, 0x13FFFULL, 7, settled, policy) == BaselineRefeedDecision::Keep,
        L"refeed: an anchor on the last window byte is still inside");
    suite.expect(DecideBaselineRefeed(record, 0x14000ULL, 7, settled, policy) == BaselineRefeedDecision::Recompute,
        L"refeed: an anchor one byte past the window end recomputes");
    suite.expect(DecideBaselineRefeed(record, 0xFFFFULL, 7, settled, policy) == BaselineRefeedDecision::Recompute,
        L"refeed: an anchor one byte before the window start recomputes");
    suite.expect(DecideBaselineRefeed(record, 0, 7, settled, policy) == BaselineRefeedDecision::Recompute
        && DecideBaselineRefeed(record, kMax64, 7, settled, policy) == BaselineRefeedDecision::Recompute,
        L"refeed: anchors at the two extremes of the space recompute");

    // 来源代次变了（重读）：位置不变，原位重喂。任何不同都算，含回绕。
    suite.expect(DecideBaselineRefeed(record, 0x12345ULL, 8, settled, policy)
        == BaselineRefeedDecision::RefreshSameSpan,
        L"refeed: a newer source revision refeeds the same span");
    suite.expect(DecideBaselineRefeed(record, 0x12345ULL, 6, settled, policy)
        == BaselineRefeedDecision::RefreshSameSpan,
        L"refeed: any different source revision refeeds, not only a larger one");
    BaselineWindowRecord wrapped = record;
    wrapped.sourceRevision = kMax64;
    suite.expect(DecideBaselineRefeed(wrapped, 0x12345ULL, 0, settled, policy)
        == BaselineRefeedDecision::RefreshSameSpan,
        L"refeed: a source revision that wrapped to 0 still differs from the recorded maximum");

    // 判定顺序：锚点出窗口压过来源变化；页缺失压过来源变化。
    suite.expect(DecideBaselineRefeed(record, 0x20000ULL, 8, settled, policy) == BaselineRefeedDecision::Recompute,
        L"refeed: leaving the window outranks a source change");
    PageSet missingFirst = settled;
    missingFirst.erase(0x10000ULL);
    suite.expect(DecideBaselineRefeed(record, 0x12345ULL, 8, missingFirst, policy)
        == BaselineRefeedDecision::Recompute,
        L"refeed: a missing window page outranks a source change");

    // 窗口内每一页单独缺失一次，都要重算（首页、中间页、末页都覆盖）。
    for (std::uint64_t index = 0; index < 4; ++index) {
        PageSet holed = settled;
        holed.erase(0x10000ULL + index * 4096ULL);
        suite.expect(DecideBaselineRefeed(record, 0x12345ULL, 7, holed, policy)
            == BaselineRefeedDecision::Recompute,
            L"refeed: any single unsettled page inside the window recomputes");
    }

    // 窗口外多出来的已落定相邻页不触发扩张：不重算。
    PageSet extra = settled;
    extra.insert(0xF000ULL);
    extra.insert(0x14000ULL);
    extra.insert(0x15000ULL);
    suite.expect(DecideBaselineRefeed(record, 0x12345ULL, 7, extra, policy) == BaselineRefeedDecision::Keep,
        L"refeed: extra settled pages next to the window do not trigger growth");
    suite.expect(DecideBaselineRefeed(record, 0x12345ULL, 8, extra, policy)
        == BaselineRefeedDecision::RefreshSameSpan,
        L"refeed: extra neighbours do not turn a plain refeed into a recompute either");

    // 页数上限：窗口 4 页，上限 4 恰好放得下，上限 3 放不下。
    suite.expect(DecideBaselineRefeed(record, 0x12345ULL, 7, settled, MakePolicy(4096, 4))
        == BaselineRefeedDecision::Keep,
        L"refeed: a window exactly at the page limit is kept");
    suite.expect(DecideBaselineRefeed(record, 0x12345ULL, 7, settled, MakePolicy(4096, 3))
        == BaselineRefeedDecision::Recompute,
        L"refeed: a window one page over a lowered limit recomputes");

    // 策略不合法：不除零，交给选取函数去报错。
    suite.expect(DecideBaselineRefeed(record, 0x12345ULL, 7, settled, MakePolicy(0, 256))
        == BaselineRefeedDecision::Recompute,
        L"refeed: page size 0 recomputes instead of dividing by zero");
    suite.expect(DecideBaselineRefeed(record, 0x12345ULL, 7, settled, MakePolicy(4096, 0))
        == BaselineRefeedDecision::Recompute,
        L"refeed: a page limit of 0 recomputes");
}

// ------------------------------------------------------------
// 十二、来路不明的记录一律重算，不在坏数据上循环。
// ------------------------------------------------------------
void TestRefeedMalformedRecords(KswordTests::Suite& suite) {
    const BaselineWindowPolicy policy = MakePolicy(4096, 256);
    const PageSet settled = MakeRun(0x10000ULL, 4, 4096);

    BaselineWindowRecord zeroLength;
    zeroLength.hasWindow = true;
    zeroLength.base = 0x10000ULL;
    zeroLength.length = 0;
    suite.expect(DecideBaselineRefeed(zeroLength, 0x10000ULL, 0, settled, policy)
        == BaselineRefeedDecision::Recompute,
        L"refeed: a record with a window flag but length 0 recomputes");

    BaselineWindowRecord oddLength = zeroLength;
    oddLength.length = 0x1800;
    suite.expect(DecideBaselineRefeed(oddLength, 0x10000ULL, 0, settled, policy)
        == BaselineRefeedDecision::Recompute,
        L"refeed: a length that is not a page multiple recomputes");

    BaselineWindowRecord oddBase = zeroLength;
    oddBase.base = 0x10800ULL;
    oddBase.length = 0x4000;
    suite.expect(DecideBaselineRefeed(oddBase, 0x11000ULL, 0, settled, policy)
        == BaselineRefeedDecision::Recompute,
        L"refeed: a base that is not page aligned recomputes");

    // 终点回绕：[0xFFFFFFFFFFFFE000, +0x2000) 的终点是 2^64，不可表示。
    BaselineWindowRecord wraps = zeroLength;
    wraps.base = 0xFFFFFFFFFFFFE000ULL;
    wraps.length = 0x2000;
    const PageSet topSettled = MakePages({ 0xFFFFFFFFFFFFE000ULL, 0xFFFFFFFFFFFFF000ULL });
    suite.expect(DecideBaselineRefeed(wraps, 0xFFFFFFFFFFFFE100ULL, 0, topSettled, policy)
        == BaselineRefeedDecision::Recompute,
        L"refeed: a record whose end would wrap past 2^64 recomputes");

    // 同一位置缩短一页（终点 0xFFFFFFFFFFFFF000，可表示）则是正常记录。
    BaselineWindowRecord fine = wraps;
    fine.length = 0x1000;
    suite.expect(DecideBaselineRefeed(fine, 0xFFFFFFFFFFFFEFFFULL, 0, topSettled, policy)
        == BaselineRefeedDecision::Keep,
        L"refeed: the same record shortened to a representable end is kept");
}

// ------------------------------------------------------------
// 十三、记录的 Set / Reset，以及"滚动不重算、重读原位重喂"的完整过程。
// ------------------------------------------------------------
void TestRecordLifecycle(KswordTests::Suite& suite) {
    const BaselineWindowPolicy policy = MakePolicy(4096, 256);
    const AddressSpaceBounds all = MakeBounds(0, kMax64);
    const PageSet settled = MakeRun(0x10000ULL, 8, 4096);

    BaselineWindowRecord record;
    suite.expect(!record.hasWindow && record.base == 0 && record.length == 0 && record.sourceRevision == 0,
        L"record: a new record holds no window");

    // 选取 -> 记录 -> 判定：这是调用方的标准流程。
    const BaselineSpan span = SelectBaselineSpan(0x13000ULL, settled, all, policy);
    ExpectSpan(suite, span, BaselineSpanStatus::Ok, 0x10000ULL, 0x8000,
        L"record: the eight-page fixture selects the whole run");
    record.Set(span, 41);
    suite.expect(record.hasWindow && record.base == 0x10000ULL && record.length == 0x8000
        && record.sourceRevision == 41,
        L"record: Set copies base, length and the source revision");

    // 滚动（锚点在窗口内换位置）反复判定都是 Keep：不会反复重建清掉自己写入标记。
    bool allKeep = true;
    for (std::uint64_t anchor = 0x10000ULL; anchor < 0x18000ULL; anchor += 0x400) {
        if (DecideBaselineRefeed(record, anchor, 41, settled, policy) != BaselineRefeedDecision::Keep) {
            allKeep = false;
        }
    }
    suite.expect(allKeep, L"record: scrolling anywhere inside the window never asks for a rebuild");

    // 重读：来源代次 +1 -> 原位重喂，base/length 不变，所以"上次读取"得以保留。
    suite.expect(DecideBaselineRefeed(record, 0x13000ULL, 42, settled, policy)
        == BaselineRefeedDecision::RefreshSameSpan,
        L"record: a reread asks for a refeed of the same span");
    record.Set(span, 42);
    suite.expect(record.base == 0x10000ULL && record.length == 0x8000 && record.sourceRevision == 42,
        L"record: refeeding the same span keeps base and length");
    suite.expect(DecideBaselineRefeed(record, 0x13000ULL, 42, settled, policy) == BaselineRefeedDecision::Keep,
        L"record: after the refeed is recorded the window is clean again");

    // 滚出窗口 -> 重算 -> 新窗口。
    suite.expect(DecideBaselineRefeed(record, 0x18000ULL, 42, settled, policy) == BaselineRefeedDecision::Recompute,
        L"record: scrolling out of the window asks for a recompute");

    // 传入空跨度（选取失败）等价于 Reset，不会留下"有窗口但长度为 0"。
    const BaselineSpan failed = SelectBaselineSpan(0x30000ULL, settled, all, policy);
    ExpectSpan(suite, failed, BaselineSpanStatus::InsertionPageNotSettled, 0, 0,
        L"record: an unsettled insertion page yields the empty fixture span");
    record.Set(failed, 99);
    suite.expect(!record.hasWindow && record.base == 0 && record.length == 0 && record.sourceRevision == 0,
        L"record: Set with an empty span resets the record");

    record.Set(span, 5);
    record.Reset();
    suite.expect(!record.hasWindow && record.base == 0 && record.length == 0 && record.sourceRevision == 0,
        L"record: Reset clears every field");
}

// ------------------------------------------------------------
// 十四、随机对拍：暴力参照 + 规格性质，低地址区与地址空间顶端各一遍。
// ------------------------------------------------------------

void TestRandomAgainstOracle(KswordTests::Suite& suite) {
    RunRandomRegion(suite, 0x1000ULL, 20260310ULL, 6000,
        L"random: selection matches the brute-force oracle in the low region");
    // 区域 [0xFFFFFFFFFFFFFC00, 2^64)：最末一页的终点是 2^64，会走到"终点不可表示"分支。
    RunRandomRegion(suite, 0xFFFFFFFFFFFFFC00ULL, 20260311ULL, 6000,
        L"random: selection matches the brute-force oracle at the top of the space");
}

// RunRandomDecision：随机检查判定函数与"逐条件独立复述"的参照一致。
void TestRandomRefeedDecision(KswordTests::Suite& suite) {
    constexpr std::uint64_t kPage = 16;
    Rng rng(20260312ULL);
    int mismatches = 0;
    int keepCount = 0;
    int refeedCount = 0;
    int recomputeCount = 0;

    for (int iteration = 0; iteration < 8000; ++iteration) {
        // 随机窗口：起点 0x100 起 0..9 页，长度 1..6 页；偶尔造坏记录。
        BaselineWindowRecord record;
        record.hasWindow = rng.Below(10) != 0;
        record.base = 0x100ULL + rng.Below(10) * kPage + (rng.Below(8) == 0 ? 3ULL : 0ULL);
        record.length = (1 + rng.Below(6)) * kPage + (rng.Below(8) == 0 ? 5ULL : 0ULL);
        record.sourceRevision = rng.Below(3);

        PageSet settled;
        for (std::uint64_t page = 0; page < 20; ++page) {
            if (rng.Below(100) < 92) {
                settled.insert(0x100ULL + page * kPage);
            }
        }
        const std::uint64_t anchor = 0xF0ULL + rng.Below(0x120);
        const std::uint64_t currentRevision = rng.Below(3);
        const std::uint64_t maxPages = 1 + rng.Below(8);

        // 参照：按文件头的判定顺序逐条复述，页检查用最朴素的逐字节步进。
        BaselineRefeedDecision expected = BaselineRefeedDecision::Keep;
        const bool malformed = record.length == 0 || record.length % kPage != 0 || record.base % kPage != 0;
        if (!record.hasWindow || malformed || record.length / kPage > maxPages
            || anchor < record.base || anchor >= record.base + record.length) {
            expected = BaselineRefeedDecision::Recompute;
        } else {
            bool allSettled = true;
            for (std::uint64_t address = record.base; address < record.base + record.length; address += kPage) {
                if (settled.count(address) == 0) {
                    allSettled = false;
                }
            }
            if (!allSettled) {
                expected = BaselineRefeedDecision::Recompute;
            } else if (record.sourceRevision != currentRevision) {
                expected = BaselineRefeedDecision::RefreshSameSpan;
            }
        }

        const BaselineRefeedDecision actual = DecideBaselineRefeed(
            record, anchor, currentRevision, settled, MakePolicy(kPage, maxPages));
        if (actual != expected) {
            ++mismatches;
        }
        keepCount += actual == BaselineRefeedDecision::Keep ? 1 : 0;
        refeedCount += actual == BaselineRefeedDecision::RefreshSameSpan ? 1 : 0;
        recomputeCount += actual == BaselineRefeedDecision::Recompute ? 1 : 0;
    }

    suite.expect(mismatches == 0, L"random: the refeed decision matches the condition-by-condition oracle");
    // 三种结论都必须真的出现过，否则对拍没有覆盖到它。
    suite.expect(keepCount > 100 && refeedCount > 100 && recomputeCount > 100,
        L"random: every refeed decision was exercised many times");
}

} // namespace

int RunMemwbBaselineWindowTests() {
    KswordTests::Suite suite(L"MEMWB baseline window");
    TestConstantsAndDefaults(suite);
    TestInsertionPage(suite);
    TestContiguity(suite);
    TestMaxPagesClipping(suite);
    TestOtherPageSizes(suite);
    TestBounds(suite);
    TestOverflowNearTop(suite);
    TestInvalidArguments(suite);
    TestUnalignedEntriesIgnored(suite);
    TestDeterminism(suite);
    TestRefeedDecision(suite);
    TestRefeedMalformedRecords(suite);
    TestRecordLifecycle(suite);
    TestRandomAgainstOracle(suite);
    TestRandomRefeedDecision(suite);
    suite.report();
    return suite.failures();
}
