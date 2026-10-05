// ============================================================
// MemoryByteSearch.cpp
// 作用：
// - 实现 MemoryByteSearch.h 声明的查找引擎：十六进制/文本模式解析、内置静态数据源、
//   分块扫描（Find 与 CountMatches 共用同一个分块扫描器）。
// - 语义、统计口径与错误位置规则全部写在头文件顶部，这里只写实现思路。
// ============================================================

#include "MemoryByteSearch.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace ksword::memwb
{
namespace
{
    // ------------------------------ 常量 ------------------------------
    // kWildcardNibble：半字节解析结果里"通配 ?"的标记值（真实数位是 0..15）。
    constexpr int kWildcardNibble = 16;
    // kInvalidNibble：半字节解析结果里"不是十六进制数位也不是 ?"的标记值。
    constexpr int kInvalidNibble = -1;
    // kFoldMask：忽略 ASCII 大小写用的掩码，清掉 bit5（0x20）。
    constexpr std::uint8_t kFoldMask = 0xDF;
    // kHexDigits：描述文本用的大写十六进制数位表。
    constexpr char kHexDigits[] = "0123456789ABCDEF";

    // ------------------------------ 解析：通用小工具 ------------------------------
    // Fail：登记一个解析错误并返回 false，让调用处可以一行 return。
    // 传入：errorOut 错误输出；code 错误码；position 出错的字节偏移。
    bool Fail(ParseError& errorOut, ParseErrorCode code, std::size_t position)
    {
        errorOut.code = code;
        errorOut.position = position;
        return false;
    }

    // IsPatternSeparator：十六进制文本里的分隔符（空白与逗号）。
    bool IsPatternSeparator(char c)
    {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f' || c == ',';
    }

    // NibbleOf：把一个字符转成半字节值。
    // 传出：0..15 为数位；kWildcardNibble 为 '?'；kInvalidNibble 为其它字符。
    int NibbleOf(char c)
    {
        if (c == '?')
        {
            return kWildcardNibble;
        }
        // 小写字母先折成大写，再在数位表前 16 项里找位置。
        const char upper = (c >= 'a' && c <= 'f') ? static_cast<char>(c - 'a' + 'A') : c;
        for (int value = 0; value < 16; ++value)
        {
            if (kHexDigits[value] == upper)
            {
                return value;
            }
        }
        return kInvalidNibble;
    }

    // IsAsciiLetter：是否为 ASCII 字母 A-Z / a-z。
    bool IsAsciiLetter(std::uint8_t value)
    {
        return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
    }

    // HasHexPrefix：text[index] 处是否是 0x/0X 前缀（'0' 紧跟 x 或 X）。
    bool HasHexPrefix(std::string_view text, std::size_t index)
    {
        return text[index] == '0'
            && index + 1 < text.size()
            && (text[index + 1] == 'x' || text[index + 1] == 'X');
    }

    // AppendNibblePair：把一对半字节拼成模式的一个字节。
    // 传入：pattern 被追加的模式；high/low 高低半字节（0..15 或 kWildcardNibble）。
    // 通配的半字节在 bytes 里填 0，在 mask 里清掉对应的四位。
    void AppendNibblePair(SearchPattern& pattern, int high, int low)
    {
        const bool highWild = (high == kWildcardNibble);
        const bool lowWild = (low == kWildcardNibble);
        const int highValue = highWild ? 0 : high;
        const int lowValue = lowWild ? 0 : low;
        const int maskValue = (highWild ? 0x00 : 0xF0) | (lowWild ? 0x00 : 0x0F);
        pattern.bytes.push_back(static_cast<std::uint8_t>((highValue << 4) | lowValue));
        pattern.mask.push_back(static_cast<std::uint8_t>(maskValue));
    }

    // DescribeHex：把十六进制模式规范化成 "4D 5A ?? A?" 形式的展示文本。
    // 传入：pattern 解析出的模式（掩码只会是 FF/F0/0F/00）。
    std::string DescribeHex(const SearchPattern& pattern)
    {
        std::string text;
        for (std::size_t i = 0; i < pattern.bytes.size(); ++i)
        {
            // 字节之间用空格隔开；半字节被掩掉就写 '?'，否则写该半字节的大写数位。
            text += (i == 0) ? "" : " ";
            text.push_back(((pattern.mask[i] & 0xF0) == 0) ? '?' : kHexDigits[pattern.bytes[i] >> 4]);
            text.push_back(((pattern.mask[i] & 0x0F) == 0) ? '?' : kHexDigits[pattern.bytes[i] & 0x0F]);
        }
        return text;
    }

    // ------------------------------ 解析：文本模式 ------------------------------
    // DecodeUtf8：严格解一个 UTF-8 码点。
    // 传入：text 全文；index 当前码点首字节偏移。
    // 传出：成功时 index 前进到下一个码点，codePointOut 为码点，返回 true；
    //       失败（孤立续字节、截断、过长编码、代理码点、超过 U+10FFFF）返回 false。
    bool DecodeUtf8(std::string_view text, std::size_t& index, std::uint32_t& codePointOut)
    {
        const std::uint8_t lead = static_cast<std::uint8_t>(text[index]);
        if (lead < 0x80)
        {
            codePointOut = lead;
            ++index;
            return true;
        }

        // 孤立续字节（80..BF）、C0/C1（恒为过长编码）、F5 以上（超出 Unicode）直接拒绝。
        if (lead < 0xC2 || lead > 0xF4)
        {
            return false;
        }

        // 按首字节判断后续字节数；首字节里剩下的低位是码点高位（掩码 0x1F/0x0F/0x07），
        // minimum 是该长度能表示的最小码点，低于它就是过长编码。
        const std::size_t extraBytes = (lead >= 0xF0) ? 3 : ((lead >= 0xE0) ? 2 : 1);
        std::uint32_t codePoint = lead & (0x7Fu >> (extraBytes + 1));
        const std::uint32_t minimum = (extraBytes == 1) ? 0x80u : ((extraBytes == 2) ? 0x800u : 0x10000u);
        if (text.size() - index <= extraBytes)
        {
            return false;  // 序列被截断
        }

        // 逐个吃续字节（必须形如 10xxxxxx），把低 6 位并进码点。
        for (std::size_t i = 1; i <= extraBytes; ++i)
        {
            const std::uint8_t next = static_cast<std::uint8_t>(text[index + i]);
            if ((next & 0xC0) != 0x80)
            {
                return false;
            }
            codePoint = (codePoint << 6) | (next & 0x3Fu);
        }

        // 过长编码、代理码点、超出 Unicode 上限都不是合法 UTF-8。
        if (codePoint < minimum || codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF))
        {
            return false;
        }
        codePointOut = codePoint;
        index += extraBytes + 1;
        return true;
    }

    // AppendByte：给模式追加一个字节。
    // 传入：value 字节值；foldCase 为 true 且是 ASCII 字母时存大写并用 0xDF 掩码，
    //       否则精确匹配（@ [ ` { 等相邻符号不是字母，不折叠）。
    void AppendByte(SearchPattern& pattern, std::uint8_t value, bool foldCase)
    {
        if (foldCase && IsAsciiLetter(value))
        {
            pattern.bytes.push_back(static_cast<std::uint8_t>(value & kFoldMask));
            pattern.mask.push_back(kFoldMask);
            return;
        }
        pattern.bytes.push_back(value);
        pattern.mask.push_back(0xFF);
    }

    // AppendUtf16Unit：给模式追加一个 UTF-16 小端码元（先低字节后高字节）。
    // 只有高字节为 0 的码元，其低字节才可能是 ASCII 字母而参与折叠；高字节永远精确。
    void AppendUtf16Unit(SearchPattern& pattern, std::uint32_t unit, bool foldCase)
    {
        const std::uint8_t low = static_cast<std::uint8_t>(unit & 0xFF);
        const std::uint8_t high = static_cast<std::uint8_t>((unit >> 8) & 0xFF);
        AppendByte(pattern, low, foldCase && high == 0);
        AppendByte(pattern, high, false);
    }

    // AppendCodePoint：把一个码点按目标编码追加到模式。
    // 传入：pattern 被追加的模式；codePoint 已校验的码点；rawUtf8 该码点在输入里的原字节；
    //       encoding 目标编码；foldCase 是否折叠 ASCII 字母。
    void AppendCodePoint(
        SearchPattern& pattern,
        std::uint32_t codePoint,
        std::string_view rawUtf8,
        TextEncoding encoding,
        bool foldCase)
    {
        if (encoding == TextEncoding::Utf16Le)
        {
            if (codePoint < 0x10000)
            {
                AppendUtf16Unit(pattern, codePoint, foldCase);
                return;
            }
            // 辅助平面：减去 0x10000 后高 10 位进高代理、低 10 位进低代理。
            const std::uint32_t offset = codePoint - 0x10000;
            AppendUtf16Unit(pattern, 0xD800 + (offset >> 10), false);
            AppendUtf16Unit(pattern, 0xDC00 + (offset & 0x3FF), false);
            return;
        }

        // UTF-8：码点已经过严格校验，原字节就是规范编码，直接逐字节追加；
        // 多字节序列的字节都 >= 0x80，不是 ASCII 字母，AppendByte 自然按精确匹配处理。
        for (const char raw : rawUtf8)
        {
            AppendByte(pattern, static_cast<std::uint8_t>(raw), foldCase);
        }
    }

    // ------------------------------ 匹配器 ------------------------------
    // CompiledPattern：预处理后的模式，匹配热路径只读它。
    struct CompiledPattern
    {
        // want：bytes & mask，匹配时只需 (data & mask) == want。
        std::vector<std::uint8_t> want;
        // mask：与 want 等长的掩码。
        std::vector<std::uint8_t> mask;
        // length：模式字节数 L（至少为 1）。
        std::size_t length = 0;
        // allExact：是否每个掩码都是 0xFF（纯字面量，可用 memcmp）。
        bool allExact = false;
        // anchor：第一个掩码为 0xFF 的下标，用来做 memchr 定位；没有则等于 length。
        std::size_t anchor = 0;
    };

    // CompilePattern：校验并预处理模式。
    // 传入：pattern 调用方给的模式；传出：compiled 预处理结果；
    // 返回 false 表示模式非法（为空或 bytes/mask 长度不等）。
    bool CompilePattern(const SearchPattern& pattern, CompiledPattern& compiled)
    {
        if (pattern.bytes.empty() || pattern.bytes.size() != pattern.mask.size())
        {
            return false;
        }
        compiled.length = pattern.bytes.size();
        compiled.mask = pattern.mask;
        compiled.want.resize(compiled.length);
        compiled.allExact = true;
        compiled.anchor = compiled.length;

        // 逐字节预先相与，并找出第一个精确字节作锚点。
        for (std::size_t i = 0; i < compiled.length; ++i)
        {
            compiled.want[i] = static_cast<std::uint8_t>(pattern.bytes[i] & pattern.mask[i]);
            if (pattern.mask[i] != 0xFF)
            {
                compiled.allExact = false;
            }
            else if (compiled.anchor == compiled.length)
            {
                compiled.anchor = i;
            }
        }
        return true;
    }

    // MatchAt：data 开头处的 L 个字节是否匹配模式（调用方保证 L 个字节可读且有效）。
    bool MatchAt(const CompiledPattern& pattern, const std::uint8_t* data)
    {
        if (pattern.allExact)
        {
            return std::memcmp(data, pattern.want.data(), pattern.length) == 0;
        }
        for (std::size_t i = 0; i < pattern.length; ++i)
        {
            if ((data[i] & pattern.mask[i]) != pattern.want[i])
            {
                return false;
            }
        }
        return true;
    }

    // ScanForward：在 data 里按起点从小到大找命中，逐字节步进（允许重叠命中）。
    // 传入：pattern 预处理模式；data 缓冲；lo/hi 候选起点下标闭区间
    //       （调用方保证 data[lo .. hi+L-1] 全部可读且有效）；visit 命中回调。
    // 传出：返回 false 表示 visit 要求停止，true 表示区间扫完。
    template <typename Visit>
    bool ScanForward(const CompiledPattern& pattern, const std::uint8_t* data, std::size_t lo, std::size_t hi, Visit& visit)
    {
        const bool hasAnchor = pattern.anchor < pattern.length;
        const std::uint8_t anchorByte = hasAnchor ? pattern.want[pattern.anchor] : 0;
        std::size_t start = lo;
        while (start <= hi)
        {
            // 有精确字节时先 memchr 跳到锚点字节相等的位置；没有任何精确字节
            // （全通配/纯半字节通配）则每个位置都是候选。
            std::size_t candidate = start;
            if (hasAnchor)
            {
                const void* hit = std::memchr(data + start + pattern.anchor, anchorByte, hi - start + 1);
                if (hit == nullptr)
                {
                    return true;
                }
                candidate = static_cast<std::size_t>(static_cast<const std::uint8_t*>(hit) - data) - pattern.anchor;
            }
            if (MatchAt(pattern, data + candidate) && !visit(candidate))
            {
                return false;
            }
            start = candidate + 1;  // 逐字节步进：下一个候选紧挨着这一个，允许重叠命中
        }
        return true;
    }

    // ScanBackward：在 data 里按起点从大到小找命中，参数与返回值同 ScanForward。
    template <typename Visit>
    bool ScanBackward(const CompiledPattern& pattern, const std::uint8_t* data, std::size_t lo, std::size_t hi, Visit& visit)
    {
        const bool hasAnchor = pattern.anchor < pattern.length;
        const std::uint8_t anchorByte = hasAnchor ? pattern.want[pattern.anchor] : 0;

        // 从 hi 向 lo 递减；用 while + 先减的写法避免无符号下标在 0 处回绕。
        std::size_t start = hi + 1;
        while (start > lo)
        {
            --start;
            if (hasAnchor && data[start + pattern.anchor] != anchorByte)
            {
                continue;
            }
            if (MatchAt(pattern, data + start) && !visit(start))
            {
                return false;
            }
        }
        return true;
    }

    // ScanEnd：一段扫描结束的原因。
    enum class ScanEnd
    {
        Completed = 0,  // 整段扫完
        Stopped,        // 回调要求停止（Find 命中或 Count 到达上限）
        Cancelled,      // 取消标志被置位
    };

    // ChunkScanner：分块扫描器，Find 与 CountMatches 共用。
    // 用法：构造后可对若干个"候选起点区间"依次调用 ScanPhase，统计累加。
    class ChunkScanner
    {
    public:
        // 构造：source 数据源；pattern 预处理模式；cancelFlag 取消标志（可空）；
        // chunkBytes 调用方设定的块大小（会被夹取，保证每块至少含一个候选起点）。
        ChunkScanner(
            IByteSource& source,
            const CompiledPattern& pattern,
            const std::atomic<bool>* cancelFlag,
            std::uint64_t chunkBytes)
            : source_(source)
            , pattern_(pattern)
            , cancelFlag_(cancelFlag)
            , chunkStarts_(EffectiveChunkBytes(chunkBytes, pattern.length) - (pattern.length - 1))
        {
        }

        // scannedBytes：累计已检查且自身字节真实读到的候选起点数。
        std::uint64_t scannedBytes = 0;
        // skippedBytes：累计已检查但自身字节读不到的候选起点数。
        std::uint64_t skippedBytes = 0;

        // ScanPhase：在候选起点闭区间 [startLo, startHi] 上按方向分块扫描。
        // 传入：startLo/startHi 候选起点区间（调用方保证 startLo<=startHi 且整个窗口在范围内）；
        //       direction 方向；visit(address)->bool 命中回调，返回 false 表示停止。
        // 传出：见 ScanEnd。全程用"剩余个数-1"做比较，64 位边界上不溢出。
        template <typename Visit>
        ScanEnd ScanPhase(std::uint64_t startLo, std::uint64_t startHi, SearchDirection direction, Visit&& visit)
        {
            const bool forward = (direction == SearchDirection::Forward);
            // cursor：Forward 时是下一块第一个候选起点；Backward 时是下一块最后一个候选起点。
            std::uint64_t cursor = forward ? startLo : startHi;

            while (true)
            {
                // 取消标志在每块读取之前检查（含第一块）。
                if (cancelFlag_ != nullptr && cancelFlag_->load(std::memory_order_relaxed))
                {
                    return ScanEnd::Cancelled;
                }

                // 本块候选起点数：剩余不足一块就取全部剩余。remainingMinusOne 不会溢出。
                const std::uint64_t remainingMinusOne = forward ? (startHi - cursor) : (cursor - startLo);
                const bool finalChunk = remainingMinusOne < chunkStarts_;
                const std::uint64_t count = finalChunk ? (remainingMinusOne + 1) : chunkStarts_;
                const std::uint64_t address = forward ? cursor : (cursor - count + 1);

                // 读取窗口 = 本块候选起点 + 模式尾部 L-1 字节（与下一块重叠的部分）。
                LoadChunk(address, count + (pattern_.length - 1));

                // bridge：把缓冲下标换成地址交给 visit，并记住最后一次命中的下标用于统计。
                std::size_t lastIndex = 0;
                auto bridge = [&](std::size_t index)
                {
                    lastIndex = index;
                    return visit(address + index);
                };
                const std::size_t countSize = static_cast<std::size_t>(count);
                if (!SearchBuffer(countSize, direction, bridge))
                {
                    // 被叫停：只统计到命中位置为止。
                    if (forward)
                    {
                        AccountRegion(0, lastIndex + 1);
                    }
                    else
                    {
                        AccountRegion(lastIndex, countSize);
                    }
                    return ScanEnd::Stopped;
                }
                AccountRegion(0, countSize);
                if (finalChunk)
                {
                    return ScanEnd::Completed;
                }
                cursor = forward ? (cursor + count) : (cursor - count);
            }
        }

    private:
        // LoadChunk：读一块并规整状态。数据源违约（缓冲比请求短）按不可读处理。
        void LoadChunk(std::uint64_t address, std::uint64_t length)
        {
            readLength_ = static_cast<std::size_t>(length);
            status_ = source_.Read(address, length, bytes_, valid_);
            if (status_ == ReadStatus::Unreadable)
            {
                return;
            }
            if (bytes_.size() < readLength_ || (status_ == ReadStatus::Partial && valid_.size() < readLength_))
            {
                status_ = ReadStatus::Unreadable;
            }
        }

        // AccountRegion：把缓冲里候选起点下标 [from, to) 计入统计。
        // Ok 全算 scanned；Unreadable 全算 skipped；Partial 按每个位置自身的有效位拆分。
        void AccountRegion(std::size_t from, std::size_t to)
        {
            if (to <= from)
            {
                return;
            }
            // validCount：区间内自身字节真实读到的位置数。Ok 全部，Unreadable 为零，
            // Partial 数有效位；其余位置计入 skipped。
            std::uint64_t validCount = to - from;
            if (status_ == ReadStatus::Unreadable)
            {
                validCount = 0;
            }
            else if (status_ == ReadStatus::Partial)
            {
                validCount = static_cast<std::uint64_t>(std::count_if(
                    valid_.data() + from, valid_.data() + to, [](std::uint8_t flag) { return flag != 0; }));
            }
            scannedBytes += validCount;
            skippedBytes += (to - from) - validCount;
        }

        // RunSpan：在缓冲下标闭区间 [lo, hi] 的候选起点上按方向搜索。
        template <typename Visit>
        bool RunSpan(std::size_t lo, std::size_t hi, SearchDirection direction, Visit& visit)
        {
            if (direction == SearchDirection::Forward)
            {
                return ScanForward(pattern_, bytes_.data(), lo, hi, visit);
            }
            return ScanBackward(pattern_, bytes_.data(), lo, hi, visit);
        }

        // SearchBuffer：在当前缓冲的 [0, count) 个候选起点上搜索，返回 false 表示被叫停。
        // Ok 整块一次搜；Unreadable 无事可做；Partial 先按有效字节切成极大连续段，
        // 每段内只考虑"整个窗口都落在该段里"的起点，保证匹配不跨越不可读字节。
        template <typename Visit>
        bool SearchBuffer(std::size_t count, SearchDirection direction, Visit& visit)
        {
            if (status_ == ReadStatus::Ok)
            {
                return RunSpan(0, count - 1, direction, visit);
            }
            if (status_ == ReadStatus::Unreadable)
            {
                return true;
            }

            // runs_：每项是一段可搜的候选起点下标闭区间 [lo, hi]。
            runs_.clear();
            std::size_t index = 0;
            while (index < readLength_)
            {
                if (valid_[index] == 0)
                {
                    ++index;
                    continue;
                }
                std::size_t end = index;
                while (end < readLength_ && valid_[end] != 0)
                {
                    ++end;
                }
                // 连续段 [index, end)：窗口放得下才有候选。读取窗口恰好是 count+L-1 字节，
                // 所以段内最大起点 end-L 自然不超过本块的候选范围（<= count-1），无需再夹取。
                if (end - index >= pattern_.length)
                {
                    runs_.emplace_back(index, end - pattern_.length);
                }
                index = end;
            }

            // Forward 按段升序、Backward 按段降序，保证回调顺序与方向一致。
            if (direction == SearchDirection::Backward)
            {
                std::reverse(runs_.begin(), runs_.end());
            }
            for (const auto& run : runs_)
            {
                if (!RunSpan(run.first, run.second, direction, visit))
                {
                    return false;
                }
            }
            return true;
        }

        // source_：数据源。
        IByteSource& source_;
        // pattern_：预处理模式。
        const CompiledPattern& pattern_;
        // cancelFlag_：取消标志，可为空。
        const std::atomic<bool>* cancelFlag_ = nullptr;
        // chunkStarts_：每块的候选起点数 = 夹取后的块大小 - (L-1)，至少为 1。
        std::uint64_t chunkStarts_ = 1;
        // bytes_/valid_：当前块的字节与有效位，跨块复用容量。
        std::vector<std::uint8_t> bytes_;
        std::vector<std::uint8_t> valid_;
        // status_：当前块的读取状态（已规整）。
        ReadStatus status_ = ReadStatus::Unreadable;
        // readLength_：当前块请求的字节数。
        std::size_t readLength_ = 0;
        // runs_：Partial 块切出的可搜起点区间，跨块复用容量。
        std::vector<std::pair<std::size_t, std::size_t>> runs_;
    };

    // PhaseInterval：Find 的一个扫描阶段，候选起点闭区间 [lo, hi]。
    struct PhaseInterval
    {
        // valid：该阶段是否非空。
        bool valid = false;
        // lo/hi：候选起点闭区间。
        std::uint64_t lo = 0;
        std::uint64_t hi = 0;
    };
}

// ------------------------------ 解析器 ------------------------------
bool ParseHexPattern(std::string_view text, SearchPattern& patternOut, ParseError& errorOut)
{
    // 先把两个输出清成初值：失败路径不会留下上一次的残留。
    patternOut = SearchPattern{};
    errorOut = ParseError{};

    SearchPattern parsed;
    std::size_t index = 0;

    // 每轮循环处理一个记号（可选前缀 + 若干半字节），分隔符直接跳过。
    while (index < text.size())
    {
        if (IsPatternSeparator(text[index]))
        {
            ++index;
            continue;
        }

        // 0x 前缀必须紧跟半字节，"0x"、"0x 4D" 都是孤立前缀，位置指向前缀的 '0'。
        const std::size_t tokenStart = index;
        if (HasHexPrefix(text, index))
        {
            index += 2;
            if (index >= text.size() || IsPatternSeparator(text[index]))
            {
                return Fail(errorOut, ParseErrorCode::BadPattern, tokenStart);
            }
        }

        // 记号内的半字节两两配对；任何非法字符立即报告它的偏移。
        std::size_t nibbleCount = 0;
        int highNibble = 0;
        while (index < text.size() && !IsPatternSeparator(text[index]))
        {
            const int nibble = NibbleOf(text[index]);
            if (nibble == kInvalidNibble)
            {
                return Fail(errorOut, ParseErrorCode::BadPattern, index);
            }
            if ((nibbleCount % 2) == 0)
            {
                highNibble = nibble;
            }
            else
            {
                AppendNibblePair(parsed, highNibble, nibble);
            }
            ++nibbleCount;
            ++index;
        }

        // 半字节数为奇数：最后一个半字节没有搭档，位置指向它。
        if ((nibbleCount % 2) != 0)
        {
            return Fail(errorOut, ParseErrorCode::BadPattern, index - 1);
        }
    }

    if (parsed.bytes.empty())
    {
        return Fail(errorOut, ParseErrorCode::Empty, 0);
    }
    parsed.description = DescribeHex(parsed);
    patternOut = std::move(parsed);
    return true;
}

bool ParseTextPattern(
    std::string_view text,
    TextEncoding encoding,
    bool caseInsensitive,
    SearchPattern& patternOut,
    ParseError& errorOut)
{
    patternOut = SearchPattern{};
    errorOut = ParseError{};
    if (text.empty())
    {
        return Fail(errorOut, ParseErrorCode::Empty, 0);
    }

    // 逐码点解码并按目标编码追加；非法 UTF-8 报告出错码点的首字节偏移。
    SearchPattern parsed;
    std::size_t index = 0;
    while (index < text.size())
    {
        const std::size_t codePointStart = index;
        std::uint32_t codePoint = 0;
        if (!DecodeUtf8(text, index, codePoint))
        {
            return Fail(errorOut, ParseErrorCode::BadPattern, codePointStart);
        }
        AppendCodePoint(parsed, codePoint, text.substr(codePointStart, index - codePointStart), encoding, caseInsensitive);
    }

    // 展示文本：编码标签（/i 表示忽略大小写）加原文本。
    parsed.description = (encoding == TextEncoding::Utf8) ? "utf8" : "utf16le";
    parsed.description += caseInsensitive ? "/i:" : ":";
    parsed.description.append(text.data(), text.size());
    patternOut = std::move(parsed);
    return true;
}

// ------------------------------ 公共函数 ------------------------------
SearchResult Find(
    IByteSource& source,
    const SearchPattern& pattern,
    const SearchRange& range,
    std::uint64_t startAddress,
    SearchDirection direction,
    bool wrap,
    const std::atomic<bool>* cancelFlag,
    std::uint64_t chunkBytes)
{
    SearchResult result;
    CompiledPattern compiled;
    if (!CompilePattern(pattern, compiled) || range.first > range.last)
    {
        result.invalidArguments = true;
        return result;
    }

    // 范围放不下一个完整模式：没有任何候选起点，也不是错误。
    const std::uint64_t tailLength = compiled.length - 1;
    if (range.last - range.first < tailLength)
    {
        return result;
    }
    const std::uint64_t lastStart = range.last - tailLength;  // 最大的合法匹配起点

    // 把"方向 + 起点 + 是否回绕"换算成至多两个候选起点区间（先 first 后 second）。
    // 只用比较与加一减一（且都已确认不会越界），64 位边界上不溢出。
    PhaseInterval first;
    PhaseInterval second;
    if (direction == SearchDirection::Forward)
    {
        if (startAddress <= lastStart)
        {
            first = { true, (std::max)(startAddress, range.first), lastStart };
        }
        if (wrap && startAddress > range.first)
        {
            second = { true, range.first, (std::min)(startAddress - 1, lastStart) };
        }
    }
    else
    {
        if (startAddress >= range.first)
        {
            first = { true, range.first, (std::min)(startAddress, lastStart) };
        }
        if (wrap && startAddress < lastStart)
        {
            second = { true, (startAddress < range.first) ? range.first : (startAddress + 1), lastStart };
        }
    }

    // 命中回调：记录地址并要求停止（Find 只要第一个命中）。
    std::uint64_t matchAddress = 0;
    auto visit = [&matchAddress](std::uint64_t address)
    {
        matchAddress = address;
        return false;
    };

    ChunkScanner scanner(source, compiled, cancelFlag, chunkBytes);
    ScanEnd end = ScanEnd::Completed;
    if (first.valid)
    {
        end = scanner.ScanPhase(first.lo, first.hi, direction, visit);
    }
    if (end == ScanEnd::Completed && second.valid)
    {
        result.wrapped = true;  // 进入回绕段就如实置位，哪怕最后没找到
        end = scanner.ScanPhase(second.lo, second.hi, direction, visit);
    }

    result.found = (end == ScanEnd::Stopped);
    result.address = result.found ? matchAddress : 0;
    result.cancelled = (end == ScanEnd::Cancelled);
    result.scannedBytes = scanner.scannedBytes;
    result.skippedUnreadableBytes = scanner.skippedBytes;
    return result;
}

CountResult CountMatches(
    IByteSource& source,
    const SearchPattern& pattern,
    const SearchRange& range,
    std::uint64_t cap,
    const std::atomic<bool>* cancelFlag,
    std::uint64_t chunkBytes)
{
    CountResult result;
    CompiledPattern compiled;
    if (!CompilePattern(pattern, compiled) || range.first > range.last)
    {
        result.invalidArguments = true;
        return result;
    }

    // 上限为 0：计数一开始就"达到上限"，一个字节都不扫。
    if (cap == 0)
    {
        result.capped = true;
        return result;
    }
    const std::uint64_t tailLength = compiled.length - 1;
    if (range.last - range.first < tailLength)
    {
        return result;
    }

    // 每个命中计一次；到达 cap 就叫停并标记 capped。
    auto visit = [&result, cap](std::uint64_t)
    {
        ++result.count;
        if (result.count >= cap)
        {
            result.capped = true;
            return false;
        }
        return true;
    };
    ChunkScanner scanner(source, compiled, cancelFlag, chunkBytes);
    const ScanEnd end = scanner.ScanPhase(range.first, range.last - tailLength, SearchDirection::Forward, visit);
    result.cancelled = (end == ScanEnd::Cancelled);
    result.scannedBytes = scanner.scannedBytes;
    result.skippedUnreadableBytes = scanner.skippedBytes;
    return result;
}
}
