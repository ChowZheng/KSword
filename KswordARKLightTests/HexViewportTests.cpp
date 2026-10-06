// 十六进制虚拟视图模型（shared/evidence/memory_workbench/HexViewport.h）的离线测试：
// 地址空间几何、滚动目标、选区。页缓存部分在 HexViewportCacheTests.cpp，由本文件的入口汇总。
//
// 为什么这个模型值得穷举：它属于**算错了不会报错**的一类。
//   * 行列与地址互转错一位，界面照样画，只是用户看到的"这个字节在这个地址"是假的，
//     后面所有的复制、书签、写入目标都跟着错；
//   * 地址在 2^64 附近一旦回绕，会变成一个落在空间里的小地址，被当成合法位置接收；
//   * 补空位若能被选中，复制出来的字节里就混进了不属于目标的地址。
//
// 断言原则与 NumericTextParseTests.cpp 一致：
//   * 期望值独立手算写死，绝不从被测函数反算；
//   * 边界两侧都测（对齐/补空位、空间首尾、2^64 回绕）；
//   * 该被拒绝的输入必须被显式拒绝，失败时输出保持安全初值；
//   * 同一套规则在 8/16/32/48/64 多种行宽下都要成立，尤其是 48（不是 2 的幂）。

#include "TestSupport.h"

#include "../shared/evidence/memory_workbench/HexViewport.h"

#include <cstdint>
#include <limits>
#include <optional>

// 页缓存、预取规划与命中率基准的测试，在 HexViewportCacheTests.cpp。
void RunMemwbViewportCacheTests(KswordTests::Suite& suite);

namespace {

using ksword::memwb::HexViewport;

// kTop：uint64 最大值，也是地址空间的最后一个地址。
constexpr std::uint64_t kTop = 0xFFFFFFFFFFFFFFFFULL;

// kInt64Min / kInt64Max：有符号增量的两个极端，用来确认移动不会溢出或回绕。
constexpr std::int64_t kInt64Min = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t kInt64Max = std::numeric_limits<std::int64_t>::max();

// Is：可选值存在且等于期望。
bool Is(const std::optional<std::uint64_t>& value, std::uint64_t expected) {
    return value.has_value() && *value == expected;
}

// IsSel：选区的锚点与插入点是否正好是期望值。
bool IsSel(const HexViewport& view, std::uint64_t anchor, std::uint64_t caret) {
    const HexViewport::Selection selection = view.GetSelection();
    return selection.anchor == anchor && selection.caret == caret;
}

// IsRange：选区闭区间是否正好是期望值。
bool IsRange(const HexViewport& view, std::uint64_t first, std::uint64_t last) {
    const std::optional<HexViewport::AddressRange> range = view.SelectedRange();
    return range.has_value() && range->first == first && range->last == last;
}

// ------------------------------------------------------------
// 一、构造与参数校验：非法参数必须被显式拒绝，视图退化为空。
// ------------------------------------------------------------
void TestConstruction(KswordTests::Suite& suite) {
    // 暂定常量按任务书钉死；Phase 1 基准定稿后需要同时改这里，这是有意的提醒。
    suite.expect(HexViewport::kPageBytes == 4096ULL, L"memwb viewport: page size constant is 4096 (provisional)");
    suite.expect(HexViewport::kMaxCachedPages == 256U, L"memwb viewport: max cached pages constant is 256 (provisional)");

    const HexViewport good(0x1000, 0x1FFF);
    suite.expect(good.Status() == HexViewport::InitStatus::Ok && good.IsValid(),
        L"memwb viewport: a well-formed range is accepted");
    suite.expect(good.BytesPerRow() == 16U, L"memwb viewport: default row width is 16");
    suite.expect(good.MaxCachedPages() == 256U, L"memwb viewport: default cache capacity is the constant");
    suite.expect(good.FirstAddress() == 0x1000ULL && good.LastAddress() == 0x1FFFULL,
        L"memwb viewport: the closed range is reported back unchanged");

    // 反向区间：被拒绝，不能悄悄交换两端。
    const HexViewport reversed(0x2000, 0x1FFF);
    suite.expect(reversed.Status() == HexViewport::InitStatus::InvalidRange && !reversed.IsValid(),
        L"memwb viewport: first > last is rejected");

    // 缓存容量 0：缓存不下任何页，拒绝。
    const HexViewport noCache(0, 0xFF, 16, 0);
    suite.expect(noCache.Status() == HexViewport::InitStatus::InvalidCacheCapacity,
        L"memwb viewport: a zero cache capacity is rejected");

    // 行宽：五个合法值之外的一律拒绝。两侧都测（相邻值、2 的幂但不在表里、极端值）。
    const std::uint32_t badWidths[] = { 0U, 1U, 7U, 9U, 15U, 17U, 24U, 40U, 47U, 49U, 63U, 65U, 128U, 0xFFFFFFFFU };
    for (const std::uint32_t width : badWidths) {
        const HexViewport view(0, 0xFF, width);
        suite.expect(view.Status() == HexViewport::InitStatus::InvalidBytesPerRow && !view.IsValid(),
            L"memwb viewport: an unsupported row width is rejected");
    }
    const std::uint32_t goodWidths[] = { 8U, 16U, 32U, 48U, 64U };
    for (const std::uint32_t width : goodWidths) {
        const HexViewport view(0, 0xFF, width);
        suite.expect(view.IsValid() && view.BytesPerRow() == width,
            L"memwb viewport: each supported row width is accepted");
        suite.expect(HexViewport::IsSupportedBytesPerRow(width),
            L"memwb viewport: IsSupportedBytesPerRow agrees with the constructor");
    }
    suite.expect(!HexViewport::IsSupportedBytesPerRow(0U) && !HexViewport::IsSupportedBytesPerRow(24U),
        L"memwb viewport: IsSupportedBytesPerRow rejects 0 and 24");

    // 无效视图必须"全空"：任何查询都不能返回看起来像数据的东西，任何写入都被拒。
    HexViewport invalid(0x2000, 0x1000);
    suite.expect(invalid.RowCount() == 0ULL, L"memwb viewport: an invalid view has zero rows");
    suite.expect(!invalid.RowStartAddress(0).has_value() && !invalid.AddressAt(0, 0).has_value(),
        L"memwb viewport: an invalid view maps no cell to an address");
    suite.expect(!invalid.RowOfAddress(0x1500).has_value() && !invalid.ContainsAddress(0x1500),
        L"memwb viewport: an invalid view contains no address");
    suite.expect(!invalid.SelectedRange().has_value() && !invalid.SetCaret(0x1500, false),
        L"memwb viewport: an invalid view has no selection and refuses SetCaret");
    suite.expect(!invalid.MoveCaretByBytes(1, false) && !invalid.SelectAll() && !invalid.MoveCaretHome(false),
        L"memwb viewport: an invalid view refuses every caret move");
    suite.expect(invalid.PlanFetch(0, 10, 2).empty(), L"memwb viewport: an invalid view plans no fetch");
    suite.expect(!invalid.ScrollToAddress(0x1500, HexViewport::ScrollAlign::Top, 0, 10).has_value(),
        L"memwb viewport: an invalid view has no scroll target");
    suite.expect(invalid.LookupByte(0x1500).state == HexViewport::ByteState::NotLoaded,
        L"memwb viewport: an invalid view never reports a byte as loaded");
}

// ------------------------------------------------------------
// 二、运行期改行宽：只接受五个合法值，拒绝时视图原样不动。
// ------------------------------------------------------------
void TestSetBytesPerRow(KswordTests::Suite& suite) {
    // 0x1000..0x10FF 共 256 字节：行宽 16 时 16 行，行宽 8 时 32 行，行宽 64 时 4 行。
    HexViewport view(0x1000, 0x10FF);
    suite.expect(view.RowCount() == 16ULL, L"memwb viewport: 256 bytes at width 16 make 16 rows");
    suite.expect(view.SetBytesPerRow(8U) && view.BytesPerRow() == 8U && view.RowCount() == 32ULL,
        L"memwb viewport: switching to width 8 doubles the row count");
    suite.expect(view.SetBytesPerRow(64U) && view.RowCount() == 4ULL,
        L"memwb viewport: switching to width 64 makes 4 rows");

    // 拒绝路径：返回 false 且行宽、行数都不变（仍是 64）。
    const std::uint32_t badWidths[] = { 0U, 12U, 24U, 100U };
    for (const std::uint32_t width : badWidths) {
        suite.expect(!view.SetBytesPerRow(width), L"memwb viewport: SetBytesPerRow rejects an unsupported width");
    }
    suite.expect(view.BytesPerRow() == 64U && view.RowCount() == 4ULL,
        L"memwb viewport: a rejected width change leaves the view untouched");

    // 无效视图连合法行宽也不接受。
    HexViewport invalid(5, 4);
    suite.expect(!invalid.SetBytesPerRow(16U), L"memwb viewport: an invalid view refuses even a supported width");

    // 改行宽必须按新行宽重新对齐第 0 行。起点选 0x1015（4117）：它在各行宽下向下对齐到不同的地址，
    //   行宽 16 -> 0x1010，行宽 32 -> 0x1000，行宽 48 -> 4117 - 4117%48(=37) = 4080 = 0xFF0。
    // 末地址 0x1054：行数 = (0x1054-起点)/行宽 + 1，分别为 (68/16=4)+1=5、(84/32=2)+1=3、(100/48=2)+1=3。
    HexViewport realign(0x1015, 0x1054);
    suite.expect(Is(realign.RowStartAddress(0), 0x1010ULL) && realign.RowCount() == 5ULL
        && !realign.AddressAt(0, 4).has_value() && Is(realign.AddressAt(0, 5), 0x1015ULL),
        L"memwb viewport: width 16 aligns 0x1015 down to 0x1010 (5 rows)");
    suite.expect(realign.SetBytesPerRow(32U) && Is(realign.RowStartAddress(0), 0x1000ULL) && realign.RowCount() == 3ULL
        && !realign.AddressAt(0, 20).has_value() && Is(realign.AddressAt(0, 21), 0x1015ULL),
        L"memwb viewport: switching to width 32 re-aligns row 0 to 0x1000 (3 rows)");
    suite.expect(realign.SetBytesPerRow(48U) && Is(realign.RowStartAddress(0), 0xFF0ULL) && realign.RowCount() == 3ULL
        && !realign.AddressAt(0, 36).has_value() && Is(realign.AddressAt(0, 37), 0x1015ULL),
        L"memwb viewport: switching to width 48 re-aligns row 0 to 0xFF0 (3 rows)");
    suite.expect(Is(realign.RowOfAddress(0x1054), 2ULL) && realign.ColumnOfAddress(0x1054) == 4U,
        L"memwb viewport: after width 48, 0x1054 is row 2 column 4 (0x1054-0xFF0 = 100 = 2*48+4)");
}

// ------------------------------------------------------------
// 三、对齐、补空位与行列互转（行宽 16）。
// ------------------------------------------------------------
void TestAlignmentAndPadding(KswordTests::Suite& suite) {
    // 对齐起点：0x1000..0x1FFF，4096 字节 = 256 行。
    const HexViewport aligned(0x1000, 0x1FFF);
    suite.expect(aligned.RowCount() == 256ULL, L"memwb viewport: an aligned 4 KiB range is 256 rows");
    suite.expect(Is(aligned.RowStartAddress(0), 0x1000ULL) && Is(aligned.RowStartAddress(255), 0x1FF0ULL),
        L"memwb viewport: first and last row starts are 0x1000 and 0x1FF0");
    suite.expect(!aligned.RowStartAddress(256).has_value(), L"memwb viewport: row 256 does not exist");
    suite.expect(Is(aligned.RowOfAddress(0x100F), 0ULL) && Is(aligned.RowOfAddress(0x1010), 1ULL),
        L"memwb viewport: the row boundary falls between 0x100F and 0x1010");
    suite.expect(Is(aligned.RowOfAddress(0x1FFF), 255ULL), L"memwb viewport: the last address is on the last row");
    suite.expect(aligned.ColumnOfAddress(0x1013).has_value() && *aligned.ColumnOfAddress(0x1013) == 3U,
        L"memwb viewport: 0x1013 is column 3");
    suite.expect(Is(aligned.AddressAt(1, 3), 0x1013ULL), L"memwb viewport: cell (1,3) is 0x1013");
    suite.expect(!aligned.AddressAt(0, 16).has_value(), L"memwb viewport: column 16 does not exist at width 16");
    suite.expect(!aligned.AddressAt(256, 0).has_value(), L"memwb viewport: row 256 has no cells");

    // 未对齐起点：0x1005..0x1034（48 字节）。第 0 行前 5 列(0x1000..0x1004)是补空位，
    // 最后一行 0x1030..0x103F 里只有前 5 列有效（到 0x1034）。手算：行 0..3 共 4 行。
    const HexViewport padded(0x1005, 0x1034);
    suite.expect(padded.RowCount() == 4ULL, L"memwb viewport: 0x1005..0x1034 spans 4 rows");
    suite.expect(Is(padded.RowStartAddress(0), 0x1000ULL),
        L"memwb viewport: row 0 starts at the aligned-down address, before the first byte");
    suite.expect(!padded.AddressAt(0, 4).has_value(), L"memwb viewport: column 4 of row 0 is leading padding");
    suite.expect(Is(padded.AddressAt(0, 5), 0x1005ULL), L"memwb viewport: column 5 of row 0 is the first byte");
    suite.expect(Is(padded.AddressAt(3, 4), 0x1034ULL), L"memwb viewport: column 4 of row 3 is the last byte");
    suite.expect(!padded.AddressAt(3, 5).has_value(), L"memwb viewport: column 5 of row 3 is trailing padding");
    suite.expect(!padded.RowOfAddress(0x1004).has_value() && !padded.ColumnOfAddress(0x1004).has_value(),
        L"memwb viewport: a padding address has no row or column");
    suite.expect(Is(padded.RowOfAddress(0x1005), 0ULL), L"memwb viewport: the first byte is on row 0");
    suite.expect(!padded.RowOfAddress(0x1035).has_value(), L"memwb viewport: an address past the end has no row");
    suite.expect(!padded.ContainsAddress(0x1004) && padded.ContainsAddress(0x1005)
        && padded.ContainsAddress(0x1034) && !padded.ContainsAddress(0x1035),
        L"memwb viewport: containment flips exactly at both ends of the range");

    // 每行的有效区间：首行去掉开头补位，末行去掉结尾补位，中间行是满的。
    const auto firstSpan = padded.RowValidSpan(0);
    const auto middleSpan = padded.RowValidSpan(2);
    const auto lastSpan = padded.RowValidSpan(3);
    suite.expect(firstSpan.has_value() && firstSpan->first == 0x1005ULL && firstSpan->last == 0x100FULL,
        L"memwb viewport: the first row's valid span skips the leading padding");
    suite.expect(middleSpan.has_value() && middleSpan->first == 0x1020ULL && middleSpan->last == 0x102FULL,
        L"memwb viewport: a middle row is fully valid");
    suite.expect(lastSpan.has_value() && lastSpan->first == 0x1030ULL && lastSpan->last == 0x1034ULL,
        L"memwb viewport: the last row's valid span stops at the last byte");
    suite.expect(!padded.RowValidSpan(4).has_value(), L"memwb viewport: a row past the end has no span");

    // 往返：每个有效地址 -> (行,列) -> 地址必须还是它自己；
    // 每个格子 -> 地址存在，当且仅当地址落在 [first,last]。共 4*16=64 个格子。
    bool roundTripOk = true;
    for (std::uint64_t address = 0x1005ULL; address <= 0x1034ULL; ++address) {
        const auto row = padded.RowOfAddress(address);
        const auto column = padded.ColumnOfAddress(address);
        if (!row.has_value() || !column.has_value() || !Is(padded.AddressAt(*row, *column), address)) {
            roundTripOk = false;
        }
    }
    suite.expect(roundTripOk, L"memwb viewport: address -> (row,column) -> address round-trips for every byte");
    bool cellsOk = true;
    for (std::uint64_t row = 0; row < 4ULL; ++row) {
        for (std::uint32_t column = 0; column < 16U; ++column) {
            const std::uint64_t raw = 0x1000ULL + row * 16ULL + column;
            const bool expectedPresent = raw >= 0x1005ULL && raw <= 0x1034ULL;
            if (padded.AddressAt(row, column).has_value() != expectedPresent) {
                cellsOk = false;
            }
        }
    }
    suite.expect(cellsOk, L"memwb viewport: a cell has an address exactly when it is inside the range");
}

// ------------------------------------------------------------
// 四、其它行宽：8、48（非 2 的幂）、64，起点各不对齐。
// ------------------------------------------------------------
void TestOtherRowWidths(KswordTests::Suite& suite) {
    // 行宽 8：0x1003..0x100C。行0 = 0x1000..0x1007（列3..7有效），行1 = 0x1008..0x100F（列0..4有效）。
    const HexViewport eight(0x1003, 0x100C, 8U);
    suite.expect(eight.RowCount() == 2ULL, L"memwb viewport: width 8 over 0x1003..0x100C is 2 rows");
    suite.expect(!eight.AddressAt(0, 2).has_value() && Is(eight.AddressAt(0, 3), 0x1003ULL),
        L"memwb viewport: width 8 leading padding ends before column 3");
    suite.expect(Is(eight.AddressAt(1, 4), 0x100CULL) && !eight.AddressAt(1, 5).has_value(),
        L"memwb viewport: width 8 trailing padding starts at column 5 of row 1");
    suite.expect(!eight.AddressAt(0, 8).has_value(), L"memwb viewport: column 8 does not exist at width 8");

    // 行宽 48：对齐是取模而不是掩码。0x1000 = 4096 = 48*85 + 16，所以第 0 行起点是 4080 = 0xFF0。
    // 0x1000..0x10FF：行 0 = 0xFF0..0x101F（列 0..15 是补位，列 16 起有效），
    // 最后一行 (0x10FF-0xFF0)/48 = 271/48 = 5，行 5 起点 0xFF0+240 = 0x10E0，有效到列 31（0x10FF）。
    const HexViewport fortyEight(0x1000, 0x10FF, 48U);
    suite.expect(fortyEight.RowCount() == 6ULL, L"memwb viewport: width 48 over 0x1000..0x10FF is 6 rows");
    suite.expect(Is(fortyEight.RowStartAddress(0), 0xFF0ULL),
        L"memwb viewport: width 48 aligns by modulo, not by mask (row 0 starts at 0xFF0)");
    suite.expect(!fortyEight.AddressAt(0, 15).has_value() && Is(fortyEight.AddressAt(0, 16), 0x1000ULL),
        L"memwb viewport: width 48 leading padding is columns 0..15");
    suite.expect(Is(fortyEight.AddressAt(5, 31), 0x10FFULL) && !fortyEight.AddressAt(5, 32).has_value(),
        L"memwb viewport: width 48 last row ends at column 31");
    suite.expect(Is(fortyEight.RowOfAddress(0x1030), 1ULL) && fortyEight.ColumnOfAddress(0x1030) == 16U,
        L"memwb viewport: 0x1030 is row 1 column 16 at width 48 (0x1030-0xFF0 = 64 = 48+16)");

    // 行宽 64：0x1046 向下对齐到 0x1040（0x1000 是 64 的倍数，0x46 = 64+6 → 余 6，起点 0x1040）。
    const HexViewport sixtyFour(0x1046, 0x1100, 64U);
    suite.expect(Is(sixtyFour.RowStartAddress(0), 0x1040ULL),
        L"memwb viewport: width 64 aligns 0x1046 down to 0x1040");
    suite.expect(!sixtyFour.AddressAt(0, 5).has_value() && Is(sixtyFour.AddressAt(0, 6), 0x1046ULL),
        L"memwb viewport: width 64 leading padding is columns 0..5");
}

// ------------------------------------------------------------
// 五、极端地址：靠近 0 与靠近 UINT64_MAX 时不得溢出、不得回绕。
// ------------------------------------------------------------
void TestExtremeAddresses(KswordTests::Suite& suite) {
    // 低端：只有地址 0 一个字节。
    const HexViewport zero(0, 0);
    suite.expect(zero.RowCount() == 1ULL && Is(zero.AddressAt(0, 0), 0ULL) && !zero.AddressAt(0, 1).has_value(),
        L"memwb viewport: a one-byte space at address 0 has exactly one cell");
    suite.expect(Is(zero.RowOfAddress(0), 0ULL) && !zero.RowOfAddress(1).has_value(),
        L"memwb viewport: address 1 is outside the one-byte space at 0");

    // 低端：0x3..0x14，第 0 行前 3 列是补位。
    const HexViewport low(3, 0x14);
    suite.expect(!low.AddressAt(0, 2).has_value() && Is(low.AddressAt(0, 3), 3ULL) && low.RowCount() == 2ULL,
        L"memwb viewport: a range starting at 3 pads columns 0..2 of row 0");

    // 高端：最后 16 字节，行宽 16：整行有效，最后一格就是 UINT64_MAX。
    const HexViewport topRow(0xFFFFFFFFFFFFFFF0ULL, kTop);
    suite.expect(topRow.RowCount() == 1ULL && Is(topRow.AddressAt(0, 15), kTop),
        L"memwb viewport: the last cell of the address space is UINT64_MAX");
    suite.expect(Is(topRow.RowOfAddress(kTop), 0ULL) && topRow.ColumnOfAddress(kTop) == 15U,
        L"memwb viewport: UINT64_MAX is row 0 column 15 of the top row");

    // 高端：行宽 64，起点 0xFFFFFFFFFFFFFFC5，行 0 起点 0xFFFFFFFFFFFFFFC0，列 0..4 是补位。
    const HexViewport top64(0xFFFFFFFFFFFFFFC5ULL, kTop, 64U);
    suite.expect(top64.RowCount() == 1ULL && !top64.AddressAt(0, 4).has_value()
        && Is(top64.AddressAt(0, 5), 0xFFFFFFFFFFFFFFC5ULL) && Is(top64.AddressAt(0, 63), kTop),
        L"memwb viewport: width 64 at the top edge pads 5 columns and ends at UINT64_MAX");
    const auto top64Span = top64.RowValidSpan(0);
    suite.expect(top64Span.has_value() && top64Span->first == 0xFFFFFFFFFFFFFFC5ULL && top64Span->last == kTop,
        L"memwb viewport: the top row's valid span is clipped only at the front");

    // 全空间：行宽 16 -> 2^60 行；行宽 8 -> 2^61 行。手算：2^64/16 = 2^60 = 0x1000000000000000。
    const HexViewport full16(0, kTop);
    suite.expect(full16.RowCount() == 0x1000000000000000ULL, L"memwb viewport: the full space at width 16 is 2^60 rows");
    suite.expect(Is(full16.RowStartAddress(0x0FFFFFFFFFFFFFFFULL), 0xFFFFFFFFFFFFFFF0ULL)
        && !full16.RowStartAddress(0x1000000000000000ULL).has_value(),
        L"memwb viewport: the last of 2^60 rows starts at 0xFFFFFFFFFFFFFFF0 and the next row does not exist");
    suite.expect(Is(full16.AddressAt(0x0FFFFFFFFFFFFFFFULL, 15), kTop)
        && Is(full16.RowOfAddress(kTop), 0x0FFFFFFFFFFFFFFFULL),
        L"memwb viewport: UINT64_MAX maps to the last row and back");
    const HexViewport full8(0, kTop, 8U);
    suite.expect(full8.RowCount() == 0x2000000000000000ULL, L"memwb viewport: the full space at width 8 is 2^61 rows");

    // 全空间、行宽 48：2^64 mod 48 = 16，所以最高一行（行号 q = (2^64-16)/48 = 384307168202282325，
    // 起点 0xFFFFFFFFFFFFFFF0）只有前 16 列落在 2^64 之内，列 16 起会越过 2^64。
    // 关键断言：AddressAt(q,16) 必须为空——若实现回绕，0xFFFFFFFFFFFFFFF0+16 会变成 0，
    // 而 0 恰好在空间里，会被当成合法地址悄悄返回。
    const HexViewport full48(0, kTop, 48U);
    const std::uint64_t lastRow48 = 384307168202282325ULL;
    suite.expect(full48.RowCount() == lastRow48 + 1ULL, L"memwb viewport: the full space at width 48 has q+1 rows");
    suite.expect(Is(full48.RowStartAddress(lastRow48), 0xFFFFFFFFFFFFFFF0ULL),
        L"memwb viewport: the top row at width 48 starts at 0xFFFFFFFFFFFFFFF0");
    suite.expect(Is(full48.AddressAt(lastRow48, 15), kTop), L"memwb viewport: width 48 top row column 15 is UINT64_MAX");
    suite.expect(!full48.AddressAt(lastRow48, 16).has_value(),
        L"memwb viewport: width 48 top row column 16 would be 2^64 and must not wrap to address 0");
    suite.expect(!full48.AddressAt(lastRow48, 47).has_value(),
        L"memwb viewport: width 48 top row column 47 is also beyond 2^64");
    suite.expect(!full48.AddressAt(lastRow48 + 1ULL, 0).has_value(), L"memwb viewport: there is no row after the top row");
    suite.expect(Is(full48.RowOfAddress(kTop), lastRow48) && full48.ColumnOfAddress(kTop) == 15U,
        L"memwb viewport: UINT64_MAX is row q column 15 at width 48");
    const auto top48Span = full48.RowValidSpan(lastRow48);
    suite.expect(top48Span.has_value() && top48Span->first == 0xFFFFFFFFFFFFFFF0ULL && top48Span->last == kTop,
        L"memwb viewport: the valid span of the top row at width 48 saturates at UINT64_MAX");

    // 行宽 48、起点 0xFFFFFFFFFFFFFF00（正好是 48 的倍数）：行 0..5 共 6 行，行 5 起点 0xFFFFFFFFFFFFFFF0。
    const HexViewport top48(0xFFFFFFFFFFFFFF00ULL, kTop, 48U);
    suite.expect(top48.RowCount() == 6ULL && Is(top48.AddressAt(5, 15), kTop) && !top48.AddressAt(5, 16).has_value(),
        L"memwb viewport: width 48 near the top has 6 rows and clips row 5 at column 16");

    // 靠近上沿的逐字节往返：最后 200 个地址，行宽 48 与 16 各一遍。
    bool topRoundTrip = true;
    for (std::uint64_t back = 0; back < 200ULL; ++back) {
        const std::uint64_t address = kTop - back;
        const auto row48 = full48.RowOfAddress(address);
        const auto column48 = full48.ColumnOfAddress(address);
        const auto row16 = full16.RowOfAddress(address);
        const auto column16 = full16.ColumnOfAddress(address);
        if (!row48.has_value() || !column48.has_value() || !Is(full48.AddressAt(*row48, *column48), address)) {
            topRoundTrip = false;
        }
        if (!row16.has_value() || !column16.has_value() || !Is(full16.AddressAt(*row16, *column16), address)) {
            topRoundTrip = false;
        }
    }
    suite.expect(topRoundTrip, L"memwb viewport: the last 200 addresses round-trip at widths 48 and 16");
}

// ------------------------------------------------------------
// 六、滚动目标：Center / Top / Nearest，夹取不滚出末尾空白。
// ------------------------------------------------------------
void TestScrollToAddress(KswordTests::Suite& suite) {
    // 0x1000..0x1FFF 共 256 行；可见 16 行时最大首行 = 256-16 = 240。
    const HexViewport view(0x1000, 0x1FFF);
    using Align = HexViewport::ScrollAlign;
    suite.expect(view.MaxFirstVisibleRow(16) == 240ULL && view.MaxFirstVisibleRow(1) == 255ULL
        && view.MaxFirstVisibleRow(256) == 0ULL && view.MaxFirstVisibleRow(300) == 0ULL
        && view.MaxFirstVisibleRow(0) == 256ULL,
        L"memwb viewport: the maximum first row is rowCount minus the visible rows, floored at 0");

    // 目标 0x1500 在第 (0x500)/16 = 80 行。
    suite.expect(Is(view.ScrollToAddress(0x1500, Align::Top, 0, 16), 80ULL), L"memwb viewport: Top puts row 80 first");
    suite.expect(Is(view.ScrollToAddress(0x1500, Align::Center, 0, 16), 72ULL),
        L"memwb viewport: Center with 16 visible rows puts row 80 at index 8 (first row 72)");
    suite.expect(Is(view.ScrollToAddress(0x1500, Align::Center, 0, 15), 73ULL),
        L"memwb viewport: Center with 15 visible rows uses half = 7 (first row 73)");

    // Nearest：下方 -> 底部对齐；上方 -> 顶部对齐；已可见 -> 不动；边界两侧各测一次。
    suite.expect(Is(view.ScrollToAddress(0x1500, Align::Nearest, 0, 16), 65ULL),
        L"memwb viewport: Nearest scrolls down to bottom-align row 80 (first row 65)");
    suite.expect(Is(view.ScrollToAddress(0x1500, Align::Nearest, 100, 16), 80ULL),
        L"memwb viewport: Nearest scrolls up to top-align row 80");
    suite.expect(Is(view.ScrollToAddress(0x1500, Align::Nearest, 75, 16), 75ULL),
        L"memwb viewport: Nearest does not move when the row is already visible");
    suite.expect(Is(view.ScrollToAddress(0x1500, Align::Nearest, 65, 16), 65ULL),
        L"memwb viewport: Nearest keeps the view when the row is exactly the last visible one");
    suite.expect(Is(view.ScrollToAddress(0x1510, Align::Nearest, 65, 16), 66ULL),
        L"memwb viewport: Nearest scrolls by one when the row is just below the last visible one");
    suite.expect(Is(view.ScrollToAddress(0x1500, Align::Nearest, 80, 16), 80ULL),
        L"memwb viewport: Nearest keeps the view when the row is the first visible one");
    suite.expect(Is(view.ScrollToAddress(0x1500, Align::Nearest, 81, 16), 80ULL),
        L"memwb viewport: Nearest scrolls up by one when the row is just above the first visible one");

    // 夹取：末尾不留空白、开头不下溢。
    suite.expect(Is(view.ScrollToAddress(0x1FF0, Align::Top, 0, 16), 240ULL),
        L"memwb viewport: Top on the last row is clamped to the maximum first row");
    suite.expect(Is(view.ScrollToAddress(0x1FF0, Align::Center, 0, 16), 240ULL),
        L"memwb viewport: Center on the last row is clamped (247 -> 240)");
    suite.expect(Is(view.ScrollToAddress(0x1FF0, Align::Nearest, 0, 16), 240ULL),
        L"memwb viewport: Nearest on the last row bottom-aligns at 240");
    suite.expect(Is(view.ScrollToAddress(0x1000, Align::Center, 0, 16), 0ULL),
        L"memwb viewport: Center on row 0 does not underflow");
    suite.expect(Is(view.ScrollToAddress(0x1080, Align::Center, 0, 16), 0ULL),
        L"memwb viewport: Center on row 8 (== half) gives first row 0");
    suite.expect(Is(view.ScrollToAddress(0x1090, Align::Center, 0, 16), 1ULL),
        L"memwb viewport: Center on row 9 (just past half) gives first row 1");
    suite.expect(Is(view.ScrollToAddress(0x1500, Align::Top, 0, 1000), 0ULL),
        L"memwb viewport: a viewport taller than the data always scrolls to row 0");
    suite.expect(Is(view.ScrollToAddress(0x1500, Align::Nearest, 1000, 16), 80ULL),
        L"memwb viewport: Nearest clamps a stale current row before comparing");

    // 拒绝路径：地址不属于空间、补空位、零行高都没有目标。
    suite.expect(!view.ScrollToAddress(0x2000, Align::Top, 0, 16).has_value()
        && !view.ScrollToAddress(0x0FFF, Align::Top, 0, 16).has_value(),
        L"memwb viewport: an address outside the space has no scroll target");
    suite.expect(!view.ScrollToAddress(0x1500, Align::Top, 0, 0).has_value(),
        L"memwb viewport: a zero-row viewport has no scroll target");
    const HexViewport padded(0x1005, 0x1034);
    suite.expect(!padded.ScrollToAddress(0x1002, Align::Top, 0, 2).has_value(),
        L"memwb viewport: a padding address has no scroll target");
}

// ------------------------------------------------------------
// 七、选区基础：扩选、反向、夹取、全选、面板。
// ------------------------------------------------------------
void TestSelectionBasics(KswordTests::Suite& suite) {
    HexViewport view(0x1000, 0x10FF);
    const HexViewport::Selection initial = view.GetSelection();
    suite.expect(initial.anchor == 0x1000ULL && initial.caret == 0x1000ULL && initial.pane == HexViewport::ActivePane::Hex,
        L"memwb viewport: a fresh view has its caret on the first byte in the hex pane");
    suite.expect(IsRange(view, 0x1000, 0x1000), L"memwb viewport: a lone caret is a one-byte inclusive range");

    // 点击：锚点与插入点一起走。
    suite.expect(view.SetCaret(0x1023, false) && IsSel(view, 0x1023, 0x1023),
        L"memwb viewport: a plain click collapses the selection onto the byte");

    // 扩选：锚点不动，范围含两端。
    suite.expect(view.SetCaret(0x1027, true) && IsSel(view, 0x1023, 0x1027) && IsRange(view, 0x1023, 0x1027),
        L"memwb viewport: shift-click keeps the anchor and includes both ends");

    // 反向选区：插入点越过锚点，范围仍是 [小,大]。
    suite.expect(view.SetCaret(0x101F, true) && IsSel(view, 0x1023, 0x101F) && IsRange(view, 0x101F, 0x1023),
        L"memwb viewport: a reversed selection reports an ordered inclusive range");

    // 夹取：越过末尾落在 0x10FF，返回 false 表示没有落在请求的地址。
    suite.expect(!view.SetCaret(0x2000, true) && IsSel(view, 0x1023, 0x10FF) && IsRange(view, 0x1023, 0x10FF),
        L"memwb viewport: extending past the end clamps to the last byte");
    suite.expect(!view.SetCaret(0x0500, false) && IsSel(view, 0x1000, 0x1000),
        L"memwb viewport: a click before the start clamps to the first byte");
    suite.expect(!view.SetCaret(kTop, false) && IsSel(view, 0x10FF, 0x10FF),
        L"memwb viewport: a click at UINT64_MAX clamps to the last byte, no wrap");
    suite.expect(view.SetCaret(0x1000, false) && view.SetCaret(0x10FF, false),
        L"memwb viewport: both ends of the space are valid exact carets");

    // 全选：锚点 = 首地址，插入点 = 末地址；再次全选没有变化。
    suite.expect(view.SetCaret(0x1050, false), L"memwb viewport: setup for select-all");
    suite.expect(view.SelectAll() && IsSel(view, 0x1000, 0x10FF) && IsRange(view, 0x1000, 0x10FF),
        L"memwb viewport: select-all spans the whole space");
    suite.expect(!view.SelectAll(), L"memwb viewport: a repeated select-all changes nothing");

    // 面板：切换在 Hex/Ascii 间往返，且地址操作不改变面板。
    HexViewport panes(0x1000, 0x10FF);
    suite.expect(panes.Pane() == HexViewport::ActivePane::Hex, L"memwb viewport: the pane starts as hex");
    suite.expect(panes.TogglePane() == HexViewport::ActivePane::Ascii && panes.Pane() == HexViewport::ActivePane::Ascii,
        L"memwb viewport: toggling hex gives ascii");
    suite.expect(panes.SetCaret(0x1010, false) && panes.MoveCaretByBytes(1, true) && panes.SelectAll()
        && panes.Pane() == HexViewport::ActivePane::Ascii,
        L"memwb viewport: selection changes do not switch the pane");
    suite.expect(panes.TogglePane() == HexViewport::ActivePane::Hex, L"memwb viewport: toggling ascii gives hex");
    panes.SetPane(HexViewport::ActivePane::Ascii);
    suite.expect(panes.GetSelection().pane == HexViewport::ActivePane::Ascii, L"memwb viewport: SetPane sets the pane");
}

// ------------------------------------------------------------
// 八、插入点移动：字节、行、页、Home/End，换行与夹取。
// ------------------------------------------------------------
void TestCaretMovement(KswordTests::Suite& suite) {
    // 0x1000..0x10FF，16 行，每行 16 字节。
    HexViewport view(0x1000, 0x10FF);

    // 按字节：越过行尾自动换行；到头不回绕（不会变成 0xFFFF...）。
    suite.expect(view.MoveCaretByBytes(1, false) && IsSel(view, 0x1001, 0x1001), L"memwb viewport: +1 byte moves right");
    suite.expect(view.MoveCaretByBytes(-1, false) && IsSel(view, 0x1000, 0x1000), L"memwb viewport: -1 byte moves left");
    suite.expect(!view.MoveCaretByBytes(-1, false) && IsSel(view, 0x1000, 0x1000),
        L"memwb viewport: moving left of the first byte stays put and reports no change");
    suite.expect(!view.MoveCaretByBytes(kInt64Min, false) && IsSel(view, 0x1000, 0x1000),
        L"memwb viewport: INT64_MIN bytes does not overflow or wrap");
    suite.expect(view.MoveCaretByBytes(kInt64Max, false) && IsSel(view, 0x10FF, 0x10FF),
        L"memwb viewport: INT64_MAX bytes clamps to the last byte");
    suite.expect(!view.MoveCaretByBytes(1, false) && IsSel(view, 0x10FF, 0x10FF),
        L"memwb viewport: moving right of the last byte stays put");
    suite.expect(view.SetCaret(0x100F, false) && view.MoveCaretByBytes(1, false) && IsSel(view, 0x1010, 0x1010),
        L"memwb viewport: moving right past a row end wraps to the next row's first column");
    suite.expect(view.MoveCaretByBytes(-1, false) && IsSel(view, 0x100F, 0x100F),
        L"memwb viewport: moving left past a row start wraps to the previous row's last column");
    suite.expect(view.MoveCaretByBytes(0x10, false) && IsSel(view, 0x101F, 0x101F),
        L"memwb viewport: +16 bytes moves exactly one row down");
    suite.expect(!view.MoveCaretByBytes(0, false), L"memwb viewport: a zero move changes nothing");

    // 扩选：锚点不动；不扩选的移动会让锚点重新跟上。
    suite.expect(view.SetCaret(0x1020, false) && view.MoveCaretByBytes(3, true) && IsSel(view, 0x1020, 0x1023),
        L"memwb viewport: shift-right extends from the anchor");
    suite.expect(view.MoveCaretByBytes(-5, true) && IsSel(view, 0x1020, 0x101E) && IsRange(view, 0x101E, 0x1020),
        L"memwb viewport: extending back across the anchor reverses the selection");
    suite.expect(view.MoveCaretByBytes(1, false) && IsSel(view, 0x101F, 0x101F),
        L"memwb viewport: a plain move collapses the selection");

    // 按行：保持列，夹取在首末行。0x1013 = 行 1 列 3。
    suite.expect(view.SetCaret(0x1013, false) && view.MoveCaretByRows(1, false) && IsSel(view, 0x1023, 0x1023),
        L"memwb viewport: down one row keeps the column");
    suite.expect(view.MoveCaretByRows(-1, false) && IsSel(view, 0x1013, 0x1013), L"memwb viewport: up one row keeps the column");
    suite.expect(view.MoveCaretByRows(-1, false) && IsSel(view, 0x1003, 0x1003), L"memwb viewport: up to the first row");
    suite.expect(!view.MoveCaretByRows(-1, false) && IsSel(view, 0x1003, 0x1003),
        L"memwb viewport: up from the first row stays put and reports no change");
    suite.expect(view.MoveCaretByRows(100, false) && IsSel(view, 0x10F3, 0x10F3),
        L"memwb viewport: a large downward move clamps to the last row");
    suite.expect(!view.MoveCaretByRows(1, false) && IsSel(view, 0x10F3, 0x10F3),
        L"memwb viewport: down from the last row stays put");
    suite.expect(view.MoveCaretByRows(kInt64Min, false) && IsSel(view, 0x1003, 0x1003),
        L"memwb viewport: INT64_MIN rows clamps to the first row");
    suite.expect(view.MoveCaretByRows(kInt64Max, false) && IsSel(view, 0x10F3, 0x10F3),
        L"memwb viewport: INT64_MAX rows clamps to the last row");
    suite.expect(view.SetCaret(0x1013, false) && view.MoveCaretByRows(1, true) && IsSel(view, 0x1013, 0x1023),
        L"memwb viewport: shift-down extends by a row");

    // 按页：一页 = visibleRows 行。0x1013 = 行 1；每页 4 行。
    suite.expect(view.SetCaret(0x1013, false) && view.MoveCaretByPages(1, 4, false) && IsSel(view, 0x1053, 0x1053),
        L"memwb viewport: page down moves by the visible row count (row 1 -> row 5)");
    suite.expect(view.MoveCaretByPages(-1, 4, false) && IsSel(view, 0x1013, 0x1013),
        L"memwb viewport: page up moves back by the visible row count");
    suite.expect(view.MoveCaretByPages(-1, 4, false) && IsSel(view, 0x1003, 0x1003),
        L"memwb viewport: page up near the top clamps to row 0");
    suite.expect(view.MoveCaretByPages(2, 4, false) && IsSel(view, 0x1083, 0x1083),
        L"memwb viewport: two pages down moves 8 rows");
    suite.expect(view.MoveCaretByPages(kInt64Max, 4, false) && IsSel(view, 0x10F3, 0x10F3),
        L"memwb viewport: INT64_MAX pages saturates and clamps to the last row");
    suite.expect(view.MoveCaretByPages(kInt64Min, 4, false) && IsSel(view, 0x1003, 0x1003),
        L"memwb viewport: INT64_MIN pages saturates and clamps to the first row");
    suite.expect(view.SetCaret(0x1013, false) && view.MoveCaretByPages(2, kTop, false) && IsSel(view, 0x10F3, 0x10F3),
        L"memwb viewport: a pages*rows product that overflows uint64 saturates instead of wrapping");
    suite.expect(view.SetCaret(0x1013, false) && !view.MoveCaretByPages(1, 0, false) && IsSel(view, 0x1013, 0x1013),
        L"memwb viewport: a page of zero rows is refused");
    suite.expect(!view.MoveCaretByPages(0, 4, false), L"memwb viewport: zero pages changes nothing");
    suite.expect(view.MoveCaretByPages(1, 4, true) && IsSel(view, 0x1013, 0x1053),
        L"memwb viewport: shift-page-down extends");

    // Home / End：行首行尾；已在位时不报变化；扩选保持锚点。
    suite.expect(view.SetCaret(0x1054, false) && view.MoveCaretHome(false) && IsSel(view, 0x1050, 0x1050),
        L"memwb viewport: Home goes to the row's first byte");
    suite.expect(!view.MoveCaretHome(false), L"memwb viewport: Home on the first column changes nothing");
    suite.expect(view.MoveCaretEnd(false) && IsSel(view, 0x105F, 0x105F), L"memwb viewport: End goes to the row's last byte");
    suite.expect(!view.MoveCaretEnd(false), L"memwb viewport: End on the last column changes nothing");
    suite.expect(view.SetCaret(0x1054, false) && view.MoveCaretEnd(true) && IsSel(view, 0x1054, 0x105F),
        L"memwb viewport: shift-End extends to the row end");
    suite.expect(view.MoveCaretHome(true) && IsSel(view, 0x1054, 0x1050) && IsRange(view, 0x1050, 0x1054),
        L"memwb viewport: shift-Home past the anchor reverses the selection");

    // 改行宽后移动按新行宽走：行宽 32 时向下一行 = +0x20。
    suite.expect(view.SetBytesPerRow(32U) && view.SetCaret(0x1013, false) && view.MoveCaretByRows(1, false)
        && IsSel(view, 0x1033, 0x1033),
        L"memwb viewport: row moves use the new width after SetBytesPerRow");
}

// ------------------------------------------------------------
// 九、补空位吸附：首行开头、末行结尾的补位不可成为插入点。
// ------------------------------------------------------------
void TestSelectionWithPadding(KswordTests::Suite& suite) {
    // 0x1005..0x1034：行 0 的 0x1000..0x1004 与行 3 的 0x1035..0x103F 是补位。
    HexViewport view(0x1005, 0x1034);
    suite.expect(IsSel(view, 0x1005, 0x1005), L"memwb viewport: the initial caret is the first valid byte, not the padding");

    // 点击补位：吸附到该行第一个有效地址（首行是 firstAddress），返回 false。
    suite.expect(!view.SetCaret(0x1002, false) && IsSel(view, 0x1005, 0x1005),
        L"memwb viewport: clicking leading padding snaps to the first valid byte");
    suite.expect(!view.SetCaret(0x1000, false) && IsSel(view, 0x1005, 0x1005),
        L"memwb viewport: clicking the very first padding cell snaps to the first valid byte");
    suite.expect(!view.SetCaret(0x1004, false) && IsSel(view, 0x1005, 0x1005),
        L"memwb viewport: clicking the last padding cell snaps to the first valid byte");
    suite.expect(view.SetCaret(0x1005, false), L"memwb viewport: the first valid byte is an exact caret");
    suite.expect(!view.SetCaret(0x1035, false) && IsSel(view, 0x1034, 0x1034),
        L"memwb viewport: clicking trailing padding clamps to the last byte");
    suite.expect(!view.SetCaret(0x103F, false) && IsSel(view, 0x1034, 0x1034),
        L"memwb viewport: clicking the last cell of the last row clamps to the last byte");

    // Home / End 不把插入点带进补位。
    suite.expect(view.SetCaret(0x1008, false) && view.MoveCaretHome(false) && IsSel(view, 0x1005, 0x1005),
        L"memwb viewport: Home on the first row stops at the first valid byte");
    suite.expect(view.MoveCaretEnd(false) && IsSel(view, 0x100F, 0x100F), L"memwb viewport: End on the first row is column 15");
    suite.expect(view.SetCaret(0x1032, false) && view.MoveCaretEnd(false) && IsSel(view, 0x1034, 0x1034),
        L"memwb viewport: End on the last row stops at the last valid byte");
    suite.expect(view.MoveCaretHome(false) && IsSel(view, 0x1030, 0x1030), L"memwb viewport: Home on the last row is column 0");

    // 按行移动落进补位：吸附。
    suite.expect(view.SetCaret(0x1013, false) && view.MoveCaretByRows(-1, false) && IsSel(view, 0x1005, 0x1005),
        L"memwb viewport: moving up into the leading padding snaps to the first valid byte");
    suite.expect(view.SetCaret(0x1010, false) && view.MoveCaretByRows(-1, false) && IsSel(view, 0x1005, 0x1005),
        L"memwb viewport: moving up from column 0 into the padding also snaps");
    suite.expect(view.SetCaret(0x1025, false) && view.MoveCaretByRows(1, false) && IsSel(view, 0x1034, 0x1034),
        L"memwb viewport: moving down into the trailing padding clamps to the last byte");
    suite.expect(view.SetCaret(0x1007, false) && view.MoveCaretByPages(1, 100, false) && IsSel(view, 0x1034, 0x1034),
        L"memwb viewport: page down far past the end clamps into the valid area");

    // 按字节移动不会走进补位。
    suite.expect(view.SetCaret(0x1005, false) && !view.MoveCaretByBytes(-1, false) && IsSel(view, 0x1005, 0x1005),
        L"memwb viewport: moving left of the first valid byte never enters the padding");
    suite.expect(view.MoveCaretByBytes(kInt64Max, false) && IsSel(view, 0x1034, 0x1034),
        L"memwb viewport: moving right clamps at the last valid byte");

    // 扩选进补位：选区范围只含有效地址；全选从 firstAddress 开始而不是 0x1000。
    suite.expect(view.SetCaret(0x1013, false) && view.MoveCaretByRows(-1, true) && IsSel(view, 0x1013, 0x1005)
        && IsRange(view, 0x1005, 0x1013),
        L"memwb viewport: a selection extended into the padding stops at the first valid byte");
    suite.expect(view.SelectAll() && IsRange(view, 0x1005, 0x1034),
        L"memwb viewport: select-all never includes the padding");
}

// ------------------------------------------------------------
// 十、选区在地址空间两端：2^64 附近不回绕，行宽 48 的越界列。
// ------------------------------------------------------------
void TestSelectionExtremes(KswordTests::Suite& suite) {
    // 全空间、行宽 48：最高一行起点 0xFFFFFFFFFFFFFFF0，只有前 16 列存在。
    HexViewport view(0, kTop, 48U);
    suite.expect(view.SetCaret(kTop, false), L"memwb viewport: UINT64_MAX is an exact caret");
    suite.expect(!view.MoveCaretByBytes(1, false) && IsSel(view, kTop, kTop),
        L"memwb viewport: moving right of UINT64_MAX does not wrap to 0");
    suite.expect(!view.MoveCaretByBytes(kInt64Max, false) && IsSel(view, kTop, kTop),
        L"memwb viewport: INT64_MAX bytes at UINT64_MAX does not wrap");
    suite.expect(!view.MoveCaretByRows(1, false) && IsSel(view, kTop, kTop),
        L"memwb viewport: moving down from the top row stays put");
    suite.expect(view.MoveCaretByRows(-1, false) && IsSel(view, 0xFFFFFFFFFFFFFFCFULL, 0xFFFFFFFFFFFFFFCFULL),
        L"memwb viewport: moving up from UINT64_MAX is 48 bytes lower (0xFFFFFFFFFFFFFFCF)");

    // Home / End：最高一行的行首是 0xFFFFFFFFFFFFFFF0，行尾是 UINT64_MAX（不是 2^64+31）。
    suite.expect(view.SetCaret(kTop, false) && view.MoveCaretHome(false) && IsSel(view, 0xFFFFFFFFFFFFFFF0ULL, 0xFFFFFFFFFFFFFFF0ULL),
        L"memwb viewport: Home on the top row is 0xFFFFFFFFFFFFFFF0");
    suite.expect(view.MoveCaretEnd(false) && IsSel(view, kTop, kTop),
        L"memwb viewport: End on the top row saturates at UINT64_MAX");

    // 从最高一行的列 3 向上再向下：向下停在原地（最后一行）。
    suite.expect(view.SetCaret(0xFFFFFFFFFFFFFFF3ULL, false) && !view.MoveCaretByRows(1, false),
        L"memwb viewport: down from the last row at column 3 stays put");
    suite.expect(view.MoveCaretByRows(-1, false) && IsSel(view, 0xFFFFFFFFFFFFFFC3ULL, 0xFFFFFFFFFFFFFFC3ULL),
        L"memwb viewport: up from the top row at column 3 keeps the column");

    // 关键：倒数第二行的列 20（地址 0xFFFFFFFFFFFFFFD4）向下，目标是最高一行的列 20，
    // 对应 2^64+4——不存在。若实现回绕会得到地址 4（它在空间里），插入点会跑到 4。
    suite.expect(view.SetCaret(0xFFFFFFFFFFFFFFD4ULL, false) && view.MoveCaretByRows(1, false)
        && IsSel(view, kTop, kTop),
        L"memwb viewport: moving down into a column beyond 2^64 clamps to UINT64_MAX, not to address 4");

    // 极大的行/页移动。
    suite.expect(view.SetCaret(0, false) && view.MoveCaretByRows(kInt64Max, false)
        && IsSel(view, 0xFFFFFFFFFFFFFFF0ULL, 0xFFFFFFFFFFFFFFF0ULL),
        L"memwb viewport: INT64_MAX rows from address 0 lands on the top row, column 0");
    suite.expect(view.SetCaret(0, false) && view.MoveCaretByPages(kInt64Max, kTop, false)
        && IsSel(view, 0xFFFFFFFFFFFFFFF0ULL, 0xFFFFFFFFFFFFFFF0ULL),
        L"memwb viewport: a saturated page move from address 0 lands on the top row");
    suite.expect(view.SetCaret(0, false) && !view.MoveCaretByBytes(-1, false) && !view.MoveCaretByRows(-1, false)
        && IsSel(view, 0, 0),
        L"memwb viewport: moving up/left from address 0 never wraps to the top");

    // 全选覆盖整个 2^64 空间。
    suite.expect(view.SelectAll() && IsRange(view, 0, kTop), L"memwb viewport: select-all covers the full 64-bit space");
}

// ------------------------------------------------------------
// 十一、选区只由选区方法改变：滚动、缓存、重读、改行宽都不碰它。
// ------------------------------------------------------------
void TestSelectionIndependentOfOtherOperations(KswordTests::Suite& suite) {
    HexViewport view(0x1000, 0x1FFF);
    suite.expect(view.SetCaret(0x1023, false) && view.SetCaret(0x1527, true), L"memwb viewport: setup selection");
    view.SetPane(HexViewport::ActivePane::Ascii);
    const HexViewport::Selection before = view.GetSelection();

    // 一连串"不该影响选区"的操作：算滚动目标、规划读取、登记在途、写缓存、换代次、改行宽。
    static_cast<void>(view.ScrollToAddress(0x1F00, HexViewport::ScrollAlign::Center, 0, 16));
    static_cast<void>(view.PlanFetch(0, 16, 2));
    HexViewport::FetchRange range;
    range.firstPageStart = 0x1000;
    range.pageCount = 1;
    static_cast<void>(view.MarkInFlight(range));
    static_cast<void>(view.LookupByte(0x1500));
    view.InvalidateAll(9);
    static_cast<void>(view.SetBytesPerRow(32U));
    suite.expect(view.GetSelection() == before,
        L"memwb viewport: scrolling, caching, invalidation and width changes leave the selection untouched");
}

}  // namespace

int RunMemwbViewportTests() {
    KswordTests::Suite suite(L"MEMWB viewport");
    TestConstruction(suite);
    TestSetBytesPerRow(suite);
    TestAlignmentAndPadding(suite);
    TestOtherRowWidths(suite);
    TestExtremeAddresses(suite);
    TestScrollToAddress(suite);
    TestSelectionBasics(suite);
    TestCaretMovement(suite);
    TestSelectionWithPadding(suite);
    TestSelectionExtremes(suite);
    TestSelectionIndependentOfOtherOperations(suite);
    RunMemwbViewportCacheTests(suite);
    suite.report();
    return suite.failures();
}
