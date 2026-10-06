// MemoryDiffOverlay.cpp
// 作用：暂存编辑叠加层的"基线载入"与"只读查询"部分。
// 补丁的改写（Stage / Discard / AcceptWrite）在 MemoryDiffOverlay.Patches.cpp。

#include "MemoryDiffOverlay.h"

#include <iterator>
#include <utility>

namespace ksword::memwb
{
    // 构造：记下暂存总量上限，其余成员取头文件里的默认初值。
    // 入参：maxPendingBytes 暂存总量上限。
    MemoryDiffOverlay::MemoryDiffOverlay(std::uint64_t maxPendingBytes)
        : maxPendingBytes_(maxPendingBytes)
    {
    }

    // 判断区间终点是否能用 uint64 表示。
    // 入参：address 起始地址；length 字节数。
    // 返回：address + length <= uint64 最大值 时为 true。
    bool MemoryDiffOverlay::RangeRepresentable(std::uint64_t address, std::uint64_t length)
    {
        // address + length > 最大值 等价于 length > 最大值 - address，
        // 后者的减法永远不会回绕，所以不会被溢出骗过去。
        return length <= kMemoryDiffOverlayAddressMax - address;
    }

    // 判断 address 是否落在当前基线窗口内。
    // 入参：address 绝对地址。
    // 返回：窗口内为 true；没有基线时恒为 false。
    bool MemoryDiffOverlay::InWindow(std::uint64_t address) const
    {
        // 没有基线时窗口为空。
        if (!hasBaseline_)
        {
            return false;
        }

        // 先比较起点再做减法，避免 address < baseAddress_ 时回绕。
        if (address < baseAddress_)
        {
            return false;
        }

        // 偏移小于窗口长度才算在内。
        return address - baseAddress_ < baseline_.size();
    }

    // 载入基线的共用实现。
    // 入参：见头文件 LoadBaseline；keepPatches 为 true 时保留暂存补丁。
    // 返回：Ok 或拒绝原因；拒绝时在改任何成员之前就返回。
    BaselineLoadStatus MemoryDiffOverlay::InstallBaseline(
        const std::string& identityKey,
        std::uint64_t baseAddress,
        std::vector<std::uint8_t> bytes,
        std::vector<std::uint8_t> validMask,
        bool keepPatches)
    {
        // 先做全部校验：掩码长度不符、窗口终点溢出都直接拒绝，此时尚未改动任何状态。
        if (validMask.size() != bytes.size())
        {
            return BaselineLoadStatus::MaskLengthMismatch;
        }

        const std::uint64_t length = static_cast<std::uint64_t>(bytes.size());
        if (!RangeRepresentable(baseAddress, length))
        {
            return BaselineLoadStatus::AddressOverflow;
        }

        // 判断是不是"同一目标同一窗口"的重读：身份、基址、长度三者必须全部相同。
        const bool sameTarget = hasBaseline_
            && identityKey == identityKey_
            && baseAddress == baseAddress_
            && bytes.size() == baseline_.size();

        // 身份串是否变了（首次载入没有旧身份，补丁本来就是空的，不算变）。
        const bool identityChanged = hasBaseline_ && identityKey != identityKey_;

        // 同一目标：被顶替下来的旧基线成为"上次读取"；否则上次读取作废。
        if (sameTarget)
        {
            previous_ = std::move(baseline_);
            previousMask_ = std::move(validMask_);
            hasPrevious_ = true;
        }
        else
        {
            previous_.clear();
            previousMask_.clear();
            hasPrevious_ = false;
        }

        // 装入新基线。
        identityKey_ = identityKey;
        baseAddress_ = baseAddress;
        baseline_ = std::move(bytes);
        validMask_ = std::move(validMask);
        hasBaseline_ = true;

        // "自己写入"标记只对紧随其后的那一次读取有意义，新读取一到就清掉，
        // 之后目标上再出现的差异才是真正的外部变化。
        selfWritten_.assign(baseline_.size(), 0);

        // 新目标 / 新快照语义要连补丁一起清空；重读语义保留补丁。
        // 例外：重读时身份串变了（换进程、换通道、重新附加）就已经不是同一目标，
        // 补丁是按绝对地址存的，留着会被写进另一个目标，所以同样清空。
        if (!keepPatches || identityChanged)
        {
            DiscardAll();
        }

        return BaselineLoadStatus::Ok;
    }

    // 载入新目标 / 新快照的基线，清空补丁。
    BaselineLoadStatus MemoryDiffOverlay::LoadBaseline(
        const std::string& identityKey,
        std::uint64_t baseAddress,
        std::vector<std::uint8_t> bytes,
        std::vector<std::uint8_t> validMask)
    {
        return InstallBaseline(identityKey, baseAddress, std::move(bytes), std::move(validMask), false);
    }

    // 同一目标重读，保留补丁。
    BaselineLoadStatus MemoryDiffOverlay::RefreshBaseline(
        const std::string& identityKey,
        std::uint64_t baseAddress,
        std::vector<std::uint8_t> bytes,
        std::vector<std::uint8_t> validMask)
    {
        return InstallBaseline(identityKey, baseAddress, std::move(bytes), std::move(validMask), true);
    }

    // 找到覆盖 address 的补丁块。
    // 入参：address 绝对地址；offsetInBlock 输出块内偏移，没找到时置 0。
    // 返回：块的迭代器；没有覆盖它的块时返回 patches_.end()。
    std::map<std::uint64_t, DiffBlock>::const_iterator MemoryDiffOverlay::FindPatch(
        std::uint64_t address,
        std::uint64_t& offsetInBlock) const
    {
        offsetInBlock = 0;

        // 第一个起点大于 address 的块的前一个，才可能覆盖 address。
        const auto next = patches_.upper_bound(address);
        if (next == patches_.begin())
        {
            return patches_.end();
        }

        // 候选块起点 <= address，再看 address 有没有落在它的长度之内。
        const auto candidate = std::prev(next);
        const std::uint64_t offset = address - candidate->first;
        if (offset >= candidate->second.after.size())
        {
            return patches_.end();
        }

        offsetInBlock = offset;
        return candidate;
    }

    // 基线里的原值。
    std::optional<std::uint8_t> MemoryDiffOverlay::BaselineByte(std::uint64_t address) const
    {
        // 窗口外没有基线字节。
        if (!InWindow(address))
        {
            return std::nullopt;
        }

        // 窗口内但没读到，同样没有。
        const std::uint64_t index = address - baseAddress_;
        if (validMask_[static_cast<std::size_t>(index)] == 0)
        {
            return std::nullopt;
        }

        return baseline_[static_cast<std::size_t>(index)];
    }

    // 上次读取里的值。
    std::optional<std::uint8_t> MemoryDiffOverlay::PreviousByte(std::uint64_t address) const
    {
        // 没有上次读取，或它与当前基线尺寸不符：不提供任何比对依据。
        if (!hasPrevious_ || previous_.size() != baseline_.size())
        {
            return std::nullopt;
        }

        if (!InWindow(address))
        {
            return std::nullopt;
        }

        const std::uint64_t index = address - baseAddress_;
        if (previousMask_[static_cast<std::size_t>(index)] == 0)
        {
            return std::nullopt;
        }

        return previous_[static_cast<std::size_t>(index)];
    }

    // 单字节变化种类，优先级见头文件。
    ByteChangeKind MemoryDiffOverlay::ChangeKind(std::uint64_t address) const
    {
        const std::optional<std::uint8_t> current = BaselineByte(address);

        // 1. Pending：有补丁且补丁值不同于基线；基线未知时当作不同。
        std::uint64_t offsetInBlock = 0;
        const auto patch = FindPatch(address, offsetInBlock);
        if (patch != patches_.end())
        {
            const std::uint8_t staged = patch->second.after[static_cast<std::size_t>(offsetInBlock)];
            if (!current.has_value() || *current != staged)
            {
                return ByteChangeKind::Pending;
            }
        }

        // 2. SelfWritten：自己刚写入的，优先于外部变化，否则每次写入都会被报成篡改。
        if (InWindow(address) && selfWritten_[static_cast<std::size_t>(address - baseAddress_)] != 0)
        {
            return ByteChangeKind::SelfWritten;
        }

        // 3. Unreadable：基线里没有这个字节。
        if (!current.has_value())
        {
            return ByteChangeKind::Unreadable;
        }

        // 4. ExternalChange：上次读取存在且两边都真实读到、取值不同。
        const std::optional<std::uint8_t> previous = PreviousByte(address);
        if (previous.has_value() && *previous != *current)
        {
            return ByteChangeKind::ExternalChange;
        }

        return ByteChangeKind::Unchanged;
    }

    // 叠加后的单字节取值：补丁优先，其次基线。
    std::optional<std::uint8_t> MemoryDiffOverlay::EffectiveByte(std::uint64_t address) const
    {
        std::uint64_t offsetInBlock = 0;
        const auto patch = FindPatch(address, offsetInBlock);
        if (patch != patches_.end())
        {
            return patch->second.after[static_cast<std::size_t>(offsetInBlock)];
        }

        return BaselineByte(address);
    }

    // 取叠加后的视图。
    // 入参：address 起始绝对地址；length 字节数。
    // 返回：MaterializedBytes；请求溢出或超过防御上限时 ok 为 false。
    MaterializedBytes MemoryDiffOverlay::Materialize(std::uint64_t address, std::uint64_t length) const
    {
        MaterializedBytes result;

        // 先拒绝不合法的请求：区间溢出，或长度大到会让分配失败。
        if (!RangeRepresentable(address, length) || length > kMemoryDiffOverlayMaxMaterializeBytes)
        {
            return result;
        }

        // 输出先全部置为"无效的 0"，后面只把真正有来源的位置改成有效。
        result.ok = true;
        result.bytes.assign(static_cast<std::size_t>(length), 0);
        result.validMask.assign(static_cast<std::size_t>(length), 0);
        if (length == 0)
        {
            return result;
        }

        const std::uint64_t endExclusive = address + length;

        // 第一层：基线与请求区间的交集。没读到的字节保持无效，不拿缓冲里的垃圾值充数。
        if (hasBaseline_)
        {
            const std::uint64_t windowEnd = baseAddress_ + baseline_.size();
            const std::uint64_t lo = address > baseAddress_ ? address : baseAddress_;
            const std::uint64_t hi = endExclusive < windowEnd ? endExclusive : windowEnd;
            for (std::uint64_t at = lo; at < hi; ++at)
            {
                const std::size_t source = static_cast<std::size_t>(at - baseAddress_);
                if (validMask_[source] == 0)
                {
                    continue;
                }

                const std::size_t target = static_cast<std::size_t>(at - address);
                result.bytes[target] = baseline_[source];
                result.validMask[target] = 1;
            }
        }

        // 第二层：补丁覆盖在上面，覆盖处视为有效。
        // 从可能压住 address 的那个块开始，向后走到起点不小于区间终点为止。
        auto patch = patches_.upper_bound(address);
        if (patch != patches_.begin())
        {
            --patch;
        }

        for (; patch != patches_.end() && patch->first < endExclusive; ++patch)
        {
            // 第一个候选块可能整个在请求区间之前：此时 hi <= lo，下面的循环一次也不会执行。
            const DiffBlock& block = patch->second;
            const std::uint64_t blockEnd = block.address + block.after.size();
            const std::uint64_t lo = block.address > address ? block.address : address;
            const std::uint64_t hi = blockEnd < endExclusive ? blockEnd : endExclusive;
            for (std::uint64_t at = lo; at < hi; ++at)
            {
                const std::size_t target = static_cast<std::size_t>(at - address);
                result.bytes[target] = block.after[static_cast<std::size_t>(at - block.address)];
                result.validMask[target] = 1;
            }
        }

        return result;
    }

    // 按地址升序列出补丁块副本。std::map 本身就是升序。
    std::vector<DiffBlock> MemoryDiffOverlay::DiffBlocks() const
    {
        std::vector<DiffBlock> blocks;
        blocks.reserve(patches_.size());
        for (const auto& entry : patches_)
        {
            blocks.push_back(entry.second);
        }

        return blocks;
    }

    // 是否存在暂存补丁。
    bool MemoryDiffOverlay::HasPendingPatches() const
    {
        return !patches_.empty();
    }

    // 暂存补丁总字节数。
    std::uint64_t MemoryDiffOverlay::PendingByteCount() const
    {
        return pendingBytes_;
    }

    // 是否已载入过基线。
    bool MemoryDiffOverlay::HasBaseline() const
    {
        return hasBaseline_;
    }

    // 是否存在上次读取。
    bool MemoryDiffOverlay::HasPreviousRead() const
    {
        return hasPrevious_;
    }

    // 当前基线身份串。
    const std::string& MemoryDiffOverlay::IdentityKey() const
    {
        return identityKey_;
    }

    // 当前窗口起始地址。
    std::uint64_t MemoryDiffOverlay::BaseAddress() const
    {
        return baseAddress_;
    }

    // 当前窗口字节数。
    std::uint64_t MemoryDiffOverlay::BaselineSize() const
    {
        return static_cast<std::uint64_t>(baseline_.size());
    }

    // 本实例的暂存总量上限。
    std::uint64_t MemoryDiffOverlay::MaxPendingBytes() const
    {
        return maxPendingBytes_;
    }
}
