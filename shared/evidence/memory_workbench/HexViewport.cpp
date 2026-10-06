// HexViewport.cpp
// 作用：HexViewport 的构造、基本信息、行列几何与滚动目标计算。
// 页缓存在 HexViewport.Cache.cpp，选区在 HexViewport.Selection.cpp。
//
// 本文件所有地址算术都遵守同一条纪律：先比较、再加减。任何一次
// "起点 + 偏移" 都先确认不会超过 UINT64_MAX，绝不允许回绕成小地址。

#include "HexViewport.h"

#include <limits>

namespace ksword::memwb {

namespace {

// kMaxAddress：uint64 能表示的最大地址，用于溢出判定。
constexpr std::uint64_t kMaxAddress = std::numeric_limits<std::uint64_t>::max();

}  // namespace

// 构造。
// 用法：HexViewport view(0x1000, 0x1FFF, 16);
// 传入：闭区间两端、每行字节数、缓存页数上限。
// 传出：对象本身；参数非法时 Status() 不是 Ok，且视图为空（所有查询返回空/0）。
HexViewport::HexViewport(
    std::uint64_t firstAddress,
    std::uint64_t lastAddress,
    std::uint32_t bytesPerRow,
    std::size_t maxCachedPages)
    : status_(InitStatus::Ok),
      firstAddress_(firstAddress),
      lastAddress_(lastAddress),
      bytesPerRow_(bytesPerRow),
      maxCachedPages_(maxCachedPages),
      rowBase_(0),
      sourceRevision_(0),
      useTick_(0) {
    // 按固定顺序校验参数：先区间，再行宽，最后缓存容量。
    if (firstAddress > lastAddress) {
        status_ = InitStatus::InvalidRange;
    } else if (!IsSupportedBytesPerRow(bytesPerRow)) {
        status_ = InitStatus::InvalidBytesPerRow;
    } else if (maxCachedPages == 0) {
        status_ = InitStatus::InvalidCacheCapacity;
    }

    // 非法参数：把可能被除的成员归一到安全值，视图保持为空。
    // 这样即使某条路径忘了检查 IsValid()，也不会出现除零。
    if (status_ != InitStatus::Ok) {
        bytesPerRow_ = kDefaultBytesPerRow;
        maxCachedPages_ = kMaxCachedPages;
        selection_ = Selection{};
        return;
    }

    // 第 0 行起始地址 = firstAddress 向下对齐到行宽的倍数。
    // 取模得到的余数一定不大于 firstAddress，所以减法不会下溢。
    rowBase_ = firstAddress_ - (firstAddress_ % bytesPerRow_);

    // 初始插入点放在第一个有效字节上，有效视图任何时刻都有插入点。
    selection_.anchor = firstAddress_;
    selection_.caret = firstAddress_;
    selection_.pane = ActivePane::Hex;
}

// 构造结果。
HexViewport::InitStatus HexViewport::Status() const {
    return status_;
}

// 视图是否有效。
bool HexViewport::IsValid() const {
    return status_ == InitStatus::Ok;
}

// 地址空间起点。
std::uint64_t HexViewport::FirstAddress() const {
    return firstAddress_;
}

// 地址空间终点。
std::uint64_t HexViewport::LastAddress() const {
    return lastAddress_;
}

// 当前每行字节数。
std::uint32_t HexViewport::BytesPerRow() const {
    return bytesPerRow_;
}

// 缓存页数上限。
std::size_t HexViewport::MaxCachedPages() const {
    return maxCachedPages_;
}

// 是否是受支持的行宽。
// 传入：行宽；传出：8/16/32/48/64 之一为 true。
bool HexViewport::IsSupportedBytesPerRow(std::uint32_t bytesPerRow) {
    return bytesPerRow == 8U
        || bytesPerRow == 16U
        || bytesPerRow == 32U
        || bytesPerRow == 48U
        || bytesPerRow == 64U;
}

// 改行宽。
// 传入：新行宽；传出：是否接受。拒绝时视图保持原样。
bool HexViewport::SetBytesPerRow(std::uint32_t bytesPerRow) {
    // 无效视图或不受支持的行宽一律拒绝，不做任何改动。
    if (!IsValid() || !IsSupportedBytesPerRow(bytesPerRow)) {
        return false;
    }

    // 行宽变了，第 0 行起始地址要按新行宽重新对齐。
    bytesPerRow_ = bytesPerRow;
    rowBase_ = firstAddress_ - (firstAddress_ % bytesPerRow_);
    return true;
}

// 地址是否属于空间。
bool HexViewport::ContainsAddress(std::uint64_t address) const {
    return IsValid() && address >= firstAddress_ && address <= lastAddress_;
}

// 总行数。
// 最后一个有效地址所在行号 + 1。(last - rowBase) 不会下溢也不会超过 uint64，
// 除以至少为 8 的行宽后 +1 也不会溢出。
std::uint64_t HexViewport::RowCount() const {
    if (!IsValid()) {
        return 0;
    }
    return (lastAddress_ - rowBase_) / bytesPerRow_ + 1ULL;
}

// 某行的起始地址。
// 传入：行号；传出：已对齐的行起始地址，越界为空。
std::optional<std::uint64_t> HexViewport::RowStartAddress(std::uint64_t row) const {
    // 行号必须在 [0, RowCount) 内；在范围内时 row*bytesPerRow 不会超过 last-rowBase。
    if (row >= RowCount()) {
        return std::nullopt;
    }
    return rowBase_ + row * static_cast<std::uint64_t>(bytesPerRow_);
}

// 某行内属于地址空间的闭区间。
// 传入：行号；传出：去掉补空位后的 [first, last]，越界为空。
std::optional<HexViewport::AddressRange> HexViewport::RowValidSpan(std::uint64_t row) const {
    // start：该行的起始地址（已对齐，首行可能早于 firstAddress）。
    const std::optional<std::uint64_t> start = RowStartAddress(row);
    if (!start.has_value()) {
        return std::nullopt;
    }

    // 行尾地址 = 行首 + (行宽 - 1)；最高一行可能超过 2^64，此时饱和到最大地址，
    // 随后再被 lastAddress_ 夹住。
    // span：行首到行尾的距离；rowEnd：该行最后一格对应的地址（饱和后）。
    const std::uint64_t span = static_cast<std::uint64_t>(bytesPerRow_) - 1ULL;
    std::uint64_t rowEnd = kMaxAddress;
    if (span <= kMaxAddress - *start) {
        rowEnd = *start + span;
    }

    // 行首早于 firstAddress 的部分是首行开头的补空位，行尾晚于 lastAddress 的部分是末行结尾的补空位。
    // valid：去掉补空位后的有效区间。
    AddressRange valid;
    valid.first = (*start < firstAddress_) ? firstAddress_ : *start;
    valid.last = (rowEnd > lastAddress_) ? lastAddress_ : rowEnd;
    return valid;
}

// 地址所在行。
// 传入：地址；传出：行号，地址不属于空间（含补空位）为空。
std::optional<std::uint64_t> HexViewport::RowOfAddress(std::uint64_t address) const {
    if (!ContainsAddress(address)) {
        return std::nullopt;
    }
    return (address - rowBase_) / static_cast<std::uint64_t>(bytesPerRow_);
}

// 地址所在列。
// 传入：地址；传出：列号 0..bytesPerRow-1，地址不属于空间为空。
std::optional<std::uint32_t> HexViewport::ColumnOfAddress(std::uint64_t address) const {
    if (!ContainsAddress(address)) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>((address - rowBase_) % static_cast<std::uint64_t>(bytesPerRow_));
}

// 行列对应的地址。
// 传入：行号、列号；传出：地址，补空位/越界/超出 2^64 为空。
std::optional<std::uint64_t> HexViewport::AddressAt(std::uint64_t row, std::uint32_t column) const {
    // 列号必须小于行宽。
    if (column >= bytesPerRow_) {
        return std::nullopt;
    }

    // 行号越界（含无效视图，此时 RowCount 为 0）。
    // start：该行起始地址。
    const std::optional<std::uint64_t> start = RowStartAddress(row);
    if (!start.has_value()) {
        return std::nullopt;
    }

    // 先判会不会超过 2^64：行宽 48 时最高一行的后几列就在这里被挡住，
    // 绝不能让它回绕成接近 0 的地址再碰巧落进空间。
    // offset：列号（行内偏移）。
    const std::uint64_t offset = static_cast<std::uint64_t>(column);
    if (offset > kMaxAddress - *start) {
        return std::nullopt;
    }

    // 再判是否是补空位（首行开头在 firstAddress 之前，末行结尾在 lastAddress 之后）。
    // address：该格对应的地址。
    const std::uint64_t address = *start + offset;
    if (address < firstAddress_ || address > lastAddress_) {
        return std::nullopt;
    }
    return address;
}

// 可滚动的最大首行。
// 传入：可见行数；传出：RowCount - visibleRowCount，不足一屏为 0。
std::uint64_t HexViewport::MaxFirstVisibleRow(std::uint64_t visibleRowCount) const {
    // rowCount：总行数。
    const std::uint64_t rowCount = RowCount();
    if (rowCount > visibleRowCount) {
        return rowCount - visibleRowCount;
    }
    return 0;
}

// 计算让 address 可见所需的目标首行。
// 用法：auto top = view.ScrollToAddress(addr, ScrollAlign::Center, scrollBar.value(), visibleRows);
// 传入：目标地址、对齐方式、当前首行、可见行数；传出：目标首行，无法计算为空。
std::optional<std::uint64_t> HexViewport::ScrollToAddress(
    std::uint64_t address,
    ScrollAlign align,
    std::uint64_t currentFirstRow,
    std::uint64_t visibleRowCount) const {
    // 零行高的视口没有"滚到哪"可言；目标地址不属于空间同理。
    if (!IsValid() || visibleRowCount == 0) {
        return std::nullopt;
    }
    // targetRow：目标地址所在行。
    const std::optional<std::uint64_t> targetRow = RowOfAddress(address);
    if (!targetRow.has_value()) {
        return std::nullopt;
    }

    // 最大首行：目标首行不能让视口下方出现空白。
    const std::uint64_t maxFirstRow = MaxFirstVisibleRow(visibleRowCount);

    // result：按对齐方式先算出的目标首行（尚未夹取）。
    std::uint64_t result = *targetRow;
    if (align == ScrollAlign::Top) {
        // 顶部对齐：目标行就是首行。
        result = *targetRow;
    } else if (align == ScrollAlign::Center) {
        // 居中：首行 = 目标行 - 可见行数/2，不足则取 0（不能下溢）。
        // half：视口高度的一半（向下取整）。
        const std::uint64_t half = visibleRowCount / 2ULL;
        result = (*targetRow > half) ? (*targetRow - half) : 0ULL;
    } else {
        // 就近：先把当前首行夹进合法范围，再判断目标行在视口的上方、之内还是下方。
        // current：夹取后的当前首行。
        const std::uint64_t current = (currentFirstRow > maxFirstRow) ? maxFirstRow : currentFirstRow;
        if (*targetRow < current) {
            // 在上方：滚到顶部对齐。
            result = *targetRow;
        } else if (*targetRow - current >= visibleRowCount) {
            // 在下方（已经越过最后一个可见行）：滚到底部对齐。
            result = *targetRow - (visibleRowCount - 1ULL);
        } else {
            // 已经可见：不动。
            result = current;
        }
    }

    // 最后统一夹取，防止滚出末尾空白。
    return (result > maxFirstRow) ? maxFirstRow : result;
}

}  // namespace ksword::memwb
