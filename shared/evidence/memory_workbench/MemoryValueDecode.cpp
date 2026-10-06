// ============================================================
// MemoryValueDecode.cpp
// 作用：
// - 实现 MemoryValueDecode.h 声明的数据解释器逻辑：字节 -> 行（DecodeAll）与
//   文本 -> 字节（EncodeValue）。
// - 公历换算自己实现（按 400/100/4/1 年周期拆天数），不使用 gmtime/localtime。
// ============================================================

#include "MemoryValueDecode.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cstdio>
#include <limits>
#include <system_error>

namespace ksword::memwb
{
namespace
{
    // ------------------------------ 常量 ------------------------------
    // kAsciiMaxChars：ascii 行最多取的字节数。
    constexpr std::size_t kAsciiMaxChars = 32;
    // kUtf16MaxUnits：utf16 行最多取的码元数（字符起点位置的上限）。
    constexpr std::size_t kUtf16MaxUnits = 16;
    // kOutOfRangeText：时间行内容不合法时的展示文本。
    constexpr const char* kOutOfRangeText = "out of range";
    // kSecondsPerDay：一天的秒数。
    constexpr std::int64_t kSecondsPerDay = 86400;
    // kTicksPerSecond：FILETIME 每秒的 100ns 刻度数。
    constexpr std::uint64_t kTicksPerSecond = 10000000ULL;
    // kTicksPerDay：FILETIME 每天的刻度数（864000000000）。
    constexpr std::uint64_t kTicksPerDay = kTicksPerSecond * 86400ULL;
    // kDayIndexUnixEpoch：1970-01-01 距 0001-01-01 的天数（公历外推）。
    constexpr std::int64_t kDayIndexUnixEpoch = 719162;
    // kDayIndexFiletimeEpoch：1601-01-01 距 0001-01-01 的天数。
    constexpr std::int64_t kDayIndexFiletimeEpoch = 584388;
    // kMaxDayIndex：9999-12-31 距 0001-01-01 的天数，日期范围的上界。
    constexpr std::int64_t kMaxDayIndex = 3652058;
    // kMinUnixSeconds：0001-01-01 00:00:00 的 Unix 秒数，time_t64 的范围下界。
    constexpr std::int64_t kMinUnixSeconds = -kDayIndexUnixEpoch * kSecondsPerDay;
    // kMaxUnixSeconds：9999-12-31 23:59:59 的 Unix 秒数，time_t64 的范围上界。
    constexpr std::int64_t kMaxUnixSeconds =
        (kMaxDayIndex - kDayIndexUnixEpoch + 1) * kSecondsPerDay - 1;
    // kMaxFiletimeTicks：9999-12-31 23:59:59.9999999 的 FILETIME，filetime 的范围上界。
    constexpr std::uint64_t kMaxFiletimeTicks =
        static_cast<std::uint64_t>(kMaxDayIndex - kDayIndexFiletimeEpoch + 1) * kTicksPerDay - 1;
    // 派生常量与独立记下的字面量互相核对：任何一边算错都在编译期暴露。
    static_assert(kMinUnixSeconds == -62135596800LL, "0001-01-01 in unix seconds");
    static_assert(kMaxUnixSeconds == 253402300799LL, "9999-12-31 23:59:59 in unix seconds");
    static_assert(kMaxFiletimeTicks == 2650467743999999999ULL, "9999-12-31 end in FILETIME");

    // ------------------------------ 通用小工具 ------------------------------
    // MakeRow：造一个只带 label 的空行（不可用、内容合法）。传入 label，传出初值行。
    DecodedRow MakeRow(const char* label)
    {
        DecodedRow row;
        row.label = label;
        return row;
    }

    // ReadUnsigned：按字节序把 size（1..8）个字节读成无符号数。
    // 传入：bytes 起点（调用方保证至少 size 字节）；size 字节数；order 字节序。传出：零扩展的数值。
    std::uint64_t ReadUnsigned(const std::uint8_t* bytes, std::size_t size, ByteOrder order)
    {
        // value：从最高有效字节开始逐字节累加的结果。
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < size; ++i)
        {
            // index：第 i 个最高有效字节在内存里的下标；小端时最高位在末尾。
            const std::size_t index = (order == ByteOrder::Little) ? (size - 1 - i) : i;
            value = (value << 8) | bytes[index];
        }
        return value;
    }

    // SignExtend：把 size 字节的无符号位模式按补码解释成有符号数。
    // 用"异或符号位再减符号位"实现，全程无符号模运算，没有未定义行为。
    std::int64_t SignExtend(std::uint64_t raw, std::size_t size)
    {
        // signBit：该宽度的符号位。
        const std::uint64_t signBit = 1ULL << (size * 8 - 1);
        return static_cast<std::int64_t>((raw ^ signBit) - signBit);
    }

    // HexText：数值输出成补零到 digits 位的十六进制文本（不带 0x）；upper 决定大小写。
    std::string HexText(std::uint64_t value, std::size_t digits, bool upper)
    {
        // table：数位表；text：先填满 '0'，再从低位往高位覆盖。
        const char* const table = upper ? "0123456789ABCDEF" : "0123456789abcdef";
        std::string text(digits, '0');
        for (std::size_t i = 0; i < digits; ++i)
        {
            text[digits - 1 - i] = table[value & 0xF];
            value >>= 4;
        }
        return text;
    }

    // DecimalText：整数的十进制文本（最短，无补零）。
    template <typename IntegerT>
    std::string DecimalText(IntegerT value)
    {
        // buffer：to_chars 的输出缓冲，64 位整数最多 20 个字符，留足余量。
        char buffer[32];
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
        return std::string(buffer, result.ptr);
    }

    // ------------------------------ 整数、浮点、指针行 ------------------------------
    // IntWidth：一种整数宽度及其有符号/无符号两个 label。
    struct IntWidth
    {
        // size：字节数；signedLabel / unsignedLabel：两行的英文键。
        std::size_t size;
        const char* signedLabel;
        const char* unsignedLabel;
    };

    // kIntWidths：按固定顺序排列的四种宽度，决定行顺序 i8 u8 i16 u16 ... u64。
    constexpr IntWidth kIntWidths[] = {
        { 1, "i8", "u8" }, { 2, "i16", "u16" }, { 4, "i32", "u32" }, { 8, "i64", "u64" },
    };

    // AppendIntegerRows：追加 8 个整数行（每种宽度先有符号后无符号）。
    // 传入：rows 输出；bytes/length 字节窗；order 字节序。
    void AppendIntegerRows(
        std::vector<DecodedRow>& rows,
        const std::uint8_t* bytes,
        std::size_t length,
        ByteOrder order)
    {
        for (const IntWidth& width : kIntWidths)
        {
            DecodedRow signedRow = MakeRow(width.signedLabel);
            DecodedRow unsignedRow = MakeRow(width.unsignedLabel);
            if (length >= width.size)
            {
                // raw：这一宽度的位模式；hex：补零到 2*size 位的大写位模式，两行共用。
                const std::uint64_t raw = ReadUnsigned(bytes, width.size, order);
                const std::string hex = " (0x" + HexText(raw, width.size * 2, true) + ")";
                // signedText / unsignedText：两种解释的十进制。
                const std::string signedText = DecimalText(SignExtend(raw, width.size));
                const std::string unsignedText = DecimalText(raw);
                signedRow.available = true;
                signedRow.text = signedText + hex;
                signedRow.copyText = signedText;
                unsignedRow.available = true;
                unsignedRow.text = unsignedText + hex;
                unsignedRow.copyText = unsignedText;
            }
            rows.push_back(std::move(signedRow));
            rows.push_back(std::move(unsignedRow));
        }
    }

    // FloatText：按位模式生成浮点文本，特殊值只看位模式、不做任何浮点运算。
    // 模板参数：FloatT 浮点类型；BitsT 同宽无符号类型。传入 bits 位模式。
    template <typename FloatT, typename BitsT>
    std::string FloatText(BitsT bits)
    {
        // 位域布局由类型推出：尾数位数 = digits - 1，指数位数 = 总位数 - 符号位 - 尾数位数。
        constexpr int kMantissaBits = std::numeric_limits<FloatT>::digits - 1;
        constexpr int kExponentBits = static_cast<int>(sizeof(FloatT)) * 8 - 1 - kMantissaBits;
        // mantissa / exponent / negative：尾数、指数与符号位。
        const BitsT mantissa = bits & ((BitsT{ 1 } << kMantissaBits) - 1);
        const BitsT exponent = (bits >> kMantissaBits) & ((BitsT{ 1 } << kExponentBits) - 1);
        const bool negative = ((bits >> (kMantissaBits + kExponentBits)) & 1) != 0;
        // 指数全 1：尾数非零是 NaN（任何符号与载荷都显示 NaN），否则是无穷。
        if (exponent == ((BitsT{ 1 } << kExponentBits) - 1))
        {
            return (mantissa != 0) ? "NaN" : (negative ? "-Inf" : "Inf");
        }
        // 指数与尾数全 0：正负零，显式写出，不依赖具体标准库对负零的取舍。
        if (exponent == 0 && mantissa == 0)
        {
            return negative ? "-0" : "0";
        }
        // 其余是有限数：最短往返表示（缓冲 64 足够容纳 f64 的最长输出）。
        char buffer[64];
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), std::bit_cast<FloatT>(bits));
        return std::string(buffer, result.ptr);
    }

    // BuildFloatRow：生成 f32 或 f64 行。传入 label、字节窗与字节序。
    template <typename FloatT, typename BitsT>
    DecodedRow BuildFloatRow(
        const char* label,
        const std::uint8_t* bytes,
        std::size_t length,
        ByteOrder order)
    {
        DecodedRow row = MakeRow(label);
        if (length < sizeof(BitsT))
        {
            return row;
        }
        row.available = true;
        row.text = FloatText<FloatT, BitsT>(static_cast<BitsT>(ReadUnsigned(bytes, sizeof(BitsT), order)));
        row.copyText = row.text;
        return row;
    }

    // BuildPointerRow：生成 ptr 行。宽度只允许 4/8；字节不足或宽度非法时不会调用 namer。
    DecodedRow BuildPointerRow(
        const std::uint8_t* bytes,
        std::size_t length,
        ByteOrder order,
        std::uint32_t pointerWidthBytes,
        IPointerNamer* namer)
    {
        DecodedRow row = MakeRow("ptr");
        // size：指针字节数。
        const std::size_t size = pointerWidthBytes;
        if ((pointerWidthBytes != 4 && pointerWidthBytes != 8) || length < size)
        {
            return row;
        }
        // address：解出的指针值（32 位时零扩展）。
        const std::uint64_t address = ReadUnsigned(bytes, size, order);
        row.available = true;
        row.copyText = "0x" + HexText(address, size * 2, true);
        row.text = row.copyText;
        if (namer != nullptr)
        {
            // description：namer 给出的描述，进来时必须为空；未命中或为空都不追加。
            std::string description;
            if (namer->Describe(address, description) && !description.empty())
            {
                row.text += " (" + description + ")";
            }
        }
        return row;
    }

    // ------------------------------ 公历换算与时间行 ------------------------------
    // IsLeapYear：公历闰年规则：4 年一闰，逢百不闰，逢四百又闰（1900 非闰、2000 闰、
    // 2100 非闰、2400 闰）。
    bool IsLeapYear(int year)
    {
        return (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0);
    }

    // DayIndexToDate：把"自 0001-01-01 起的天数"（0..kMaxDayIndex）换成年月日。
    // 做法：依次按 400 年（146097 天）、百年（36524 天，周期末的百年多一天）、4 年
    // （1461 天）、1 年（365 天，闰年最后一天多一天）拆分；末两级的商需要封顶。
    void DayIndexToDate(std::int64_t dayIndex, int& year, int& month, int& day)
    {
        // cycles400：整 400 年周期数；rest：每级扣除之后剩余的天数。
        const std::int64_t cycles400 = dayIndex / 146097;
        std::int64_t rest = dayIndex % 146097;
        // cycles100：百年数。封顶 3：周期最后一天（逢四百的闰年末）会让商落到 4。
        const std::int64_t cycles100 = std::min<std::int64_t>(rest / 36524, 3);
        rest -= cycles100 * 36524;
        // cycles4：百年内的 4 年组数；cycles1：整年数，同样封顶 3（闰年的第 366 天）。
        const std::int64_t cycles4 = rest / 1461;
        rest -= cycles4 * 1461;
        const std::int64_t cycles1 = std::min<std::int64_t>(rest / 365, 3);
        rest -= cycles1 * 365;
        year = static_cast<int>(1 + 400 * cycles400 + 100 * cycles100 + 4 * cycles4 + cycles1);
        // 此时 rest 是年内零起点的天序号，按该年各月长度逐月扣减得到月与日。
        static const int kMonthDays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
        int monthIndex = 0;
        while (monthIndex < 11)
        {
            // monthLength：当月天数，闰年二月 29 天。
            const std::int64_t monthLength = kMonthDays[monthIndex] + ((monthIndex == 1 && IsLeapYear(year)) ? 1 : 0);
            if (rest < monthLength)
            {
                break;
            }
            rest -= monthLength;
            ++monthIndex;
        }
        month = monthIndex + 1;
        day = static_cast<int>(rest) + 1;
    }

    // FormatMoment：格式化一个时刻。
    // 传入：dayIndex 自 0001-01-01 的天数；secondOfDay 当天内秒序号；fraction 非空时作为
    // 小数秒接在秒后；iso 为真时用 'T' 分隔并以 "Z" 结尾（复制用），否则用空格与 " UTC"（展示用）。
    std::string FormatMoment(std::int64_t dayIndex, std::int64_t secondOfDay, const std::string& fraction, bool iso)
    {
        int year = 1;
        int month = 1;
        int day = 1;
        DayIndexToDate(dayIndex, year, month, day);
        // buffer：日期时间主体；年份恒为 4 位补零，其余各段 2 位补零。
        char buffer[40];
        std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d%c%02d:%02d:%02d", year, month, day,
            iso ? 'T' : ' ', static_cast<int>(secondOfDay / 3600),
            static_cast<int>((secondOfDay % 3600) / 60), static_cast<int>(secondOfDay % 60));
        std::string text = buffer;
        if (!fraction.empty())
        {
            text += "." + fraction;
        }
        text += iso ? "Z" : " UTC";
        return text;
    }

    // MarkOutOfRange：把时间行标成内容非法；rawText 是原始数值的十进制，供复制。
    void MarkOutOfRange(DecodedRow& row, const std::string& rawText)
    {
        row.available = true;
        row.valid = false;
        row.text = kOutOfRangeText;
        row.copyText = rawText;
    }

    // BuildUnixTimeRow：生成 time_t32（size 4）或 time_t64（size 8）行。
    DecodedRow BuildUnixTimeRow(std::size_t size, const std::uint8_t* bytes, std::size_t length, ByteOrder order)
    {
        DecodedRow row = MakeRow(size == 4 ? "time_t32" : "time_t64");
        if (length < size)
        {
            return row;
        }
        // seconds：有符号 Unix 秒数。
        const std::int64_t seconds = SignExtend(ReadUnsigned(bytes, size, order), size);
        if (seconds < kMinUnixSeconds || seconds > kMaxUnixSeconds)
        {
            MarkOutOfRange(row, DecimalText(seconds));
            return row;
        }
        // 向下取整的除法：C++ 的 / 与 % 对负数向零截断，1970 年之前的秒数余数为负，
        // 要手工借一天，否则会差一天。
        std::int64_t days = seconds / kSecondsPerDay;
        std::int64_t secondOfDay = seconds % kSecondsPerDay;
        if (secondOfDay < 0)
        {
            secondOfDay += kSecondsPerDay;
            days -= 1;
        }
        row.available = true;
        row.text = FormatMoment(days + kDayIndexUnixEpoch, secondOfDay, std::string(), false);
        row.copyText = FormatMoment(days + kDayIndexUnixEpoch, secondOfDay, std::string(), true);
        return row;
    }

    // BuildFiletimeRow：生成 filetime 行。0（未设置哨兵）与超过 9999 年末的值判内容非法。
    DecodedRow BuildFiletimeRow(const std::uint8_t* bytes, std::size_t length, ByteOrder order)
    {
        DecodedRow row = MakeRow("filetime");
        if (length < 8)
        {
            return row;
        }
        // ticks：自 1601-01-01 的 100ns 刻度数（无符号）。
        const std::uint64_t ticks = ReadUnsigned(bytes, 8, order);
        if (ticks == 0 || ticks > kMaxFiletimeTicks)
        {
            MarkOutOfRange(row, DecimalText(ticks));
            return row;
        }
        // 整天数换日期；当天剩余刻度拆成秒与 7 位小数。
        const std::int64_t dayIndex = static_cast<std::int64_t>(ticks / kTicksPerDay) + kDayIndexFiletimeEpoch;
        const std::uint64_t rest = ticks % kTicksPerDay;
        const std::int64_t secondOfDay = static_cast<std::int64_t>(rest / kTicksPerSecond);
        // fractionDigits：秒内刻度数的十进制；fraction：补零到 7 位后的小数部分。
        const std::string fractionDigits = DecimalText(rest % kTicksPerSecond);
        const std::string fraction = std::string(7 - fractionDigits.size(), '0') + fractionDigits;
        row.available = true;
        row.text = FormatMoment(dayIndex, secondOfDay, fraction, false);
        row.copyText = FormatMoment(dayIndex, secondOfDay, fraction, true);
        return row;
    }

    // ------------------------------ GUID 与字符串行 ------------------------------
    // FormatGuid：把 16 字节按 Windows 内存布局格式化成 8-4-4-4-12（不含花括号）。
    // Data1/Data2/Data3 按小端读，Data4 的 8 个字节按内存顺序输出；upper 决定大小写。
    std::string FormatGuid(const std::uint8_t* bytes, bool upper)
    {
        std::string text = HexText(ReadUnsigned(bytes, 4, ByteOrder::Little), 8, upper);
        text += "-" + HexText(ReadUnsigned(bytes + 4, 2, ByteOrder::Little), 4, upper);
        text += "-" + HexText(ReadUnsigned(bytes + 6, 2, ByteOrder::Little), 4, upper);
        // Data4 的 8 个字节分成 2 + 6 两组：第 8 字节前（与 Data3 之间）和第 10 字节前各有连字符。
        for (std::size_t i = 8; i < 16; ++i)
        {
            text += ((i == 8 || i == 10) ? "-" : "") + HexText(bytes[i], 2, upper);
        }
        return text;
    }

    // BuildGuidRow：生成 guid 行（固定小端字段布局，不随 ByteOrder 变化）。
    DecodedRow BuildGuidRow(const std::uint8_t* bytes, std::size_t length)
    {
        DecodedRow row = MakeRow("guid");
        if (length < 16)
        {
            return row;
        }
        row.available = true;
        row.text = FormatGuid(bytes, false);
        row.copyText = "{" + FormatGuid(bytes, true) + "}";
        return row;
    }

    // BuildAsciiRow：生成 ascii 行。取到首个不可打印字节（含 NUL）或 32 字节为止。
    DecodedRow BuildAsciiRow(const std::uint8_t* bytes, std::size_t length)
    {
        DecodedRow row = MakeRow("ascii");
        // limit：最多检查的字节数；count：连续可打印字节（0x20..0x7E）的个数。
        const std::size_t limit = std::min(length, kAsciiMaxChars);
        std::size_t count = 0;
        while (count < limit && bytes[count] >= 0x20 && bytes[count] <= 0x7E)
        {
            ++count;
        }
        if (count == 0)
        {
            return row;
        }
        row.available = true;
        row.text.assign(reinterpret_cast<const char*>(bytes), count);
        row.copyText = row.text;
        return row;
    }

    // AppendUtf8：把一个 Unicode 标量值（非代理项、不超过 U+10FFFF）追加成 UTF-8 字节。
    void AppendUtf8(std::string& out, std::uint32_t codePoint)
    {
        // extra：续字节个数，由码点范围决定；前导字节的高位标记按 extra 选取。
        const int extra = codePoint < 0x80 ? 0 : (codePoint < 0x800 ? 1 : (codePoint < 0x10000 ? 2 : 3));
        static const std::uint32_t kLeadMarks[4] = { 0x00, 0xC0, 0xE0, 0xF0 };
        out.push_back(static_cast<char>(kLeadMarks[extra] | (codePoint >> (6 * extra))));
        for (int i = extra - 1; i >= 0; --i)
        {
            out.push_back(static_cast<char>(0x80 | ((codePoint >> (6 * i)) & 0x3F)));
        }
    }

    // BuildUtf16Row：生成 utf16 行（UTF-16LE，固定小端，不随 ByteOrder 变化）。
    // 孤立代理项输出 U+FFFD 并令 valid=false；至少一个字符才 available。
    DecodedRow BuildUtf16Row(const std::uint8_t* bytes, std::size_t length)
    {
        DecodedRow row = MakeRow("utf16");
        // units：窗口里完整的码元数（奇数多出的字节忽略）；window：字符起点的上限。
        const std::size_t units = length / 2;
        const std::size_t window = std::min(units, kUtf16MaxUnits);
        // unitAt：读第 index 个小端码元。
        const auto unitAt = [bytes](std::size_t index) {
            return static_cast<std::uint32_t>(bytes[index * 2] | (bytes[index * 2 + 1] << 8));
        };
        // text：输出的 UTF-8；valid：是否出现过孤立代理项；index：当前码元下标。
        std::string text;
        bool valid = true;
        std::size_t index = 0;
        while (index < window)
        {
            const std::uint32_t unit = unitAt(index);
            if (unit == 0)
            {
                break;
            }
            // codePoint：本字符的标量值；consumed：本字符占用的码元数。
            std::uint32_t codePoint = unit;
            std::size_t consumed = 1;
            if (unit >= 0xD800 && unit <= 0xDBFF && index + 1 < units && (unitAt(index + 1) & 0xFC00) == 0xDC00)
            {
                // 高代理项后紧跟低代理项：合成补充平面字符。起点在窗口内的代理对即使低代理项
                // 落在窗口之外也补全，不把一个完整字符拆坏。
                codePoint = 0x10000 + ((unit - 0xD800) << 10) + (unitAt(index + 1) - 0xDC00);
                consumed = 2;
            }
            else if (unit >= 0xD800 && unit <= 0xDFFF)
            {
                // 孤立的高或低代理项：不能编码成 UTF-8，用替换字符占位并标记非法。
                codePoint = 0xFFFD;
                valid = false;
            }
            AppendUtf8(text, codePoint);
            index += consumed;
        }
        if (text.empty())
        {
            return row;
        }
        row.available = true;
        row.valid = valid;
        row.text = text;
        row.copyText = text;
        return row;
    }

    // ------------------------------ 文本 -> 字节 ------------------------------
    // TypeKind：可编码类型的大类，决定解析与范围检查的方式。
    enum class TypeKind
    {
        Signed,
        Unsigned,
        Float,
        Pointer,
    };

    // EncodeType：一种可编码类型。name 精确匹配；size 字节数，Pointer 的宽度由调用方给定故为 0。
    struct EncodeType
    {
        const char* name;
        TypeKind kind;
        std::size_t size;
    };

    // kEncodeTypes：全部可编码类型。
    constexpr EncodeType kEncodeTypes[] = {
        { "i8", TypeKind::Signed, 1 }, { "u8", TypeKind::Unsigned, 1 },
        { "i16", TypeKind::Signed, 2 }, { "u16", TypeKind::Unsigned, 2 },
        { "i32", TypeKind::Signed, 4 }, { "u32", TypeKind::Unsigned, 4 },
        { "i64", TypeKind::Signed, 8 }, { "u64", TypeKind::Unsigned, 8 },
        { "f32", TypeKind::Float, 4 }, { "f64", TypeKind::Float, 8 },
        { "ptr", TypeKind::Pointer, 0 },
    };

    // TrimBlanks：剪掉两端的空格、制表、回车、换行。
    std::string_view TrimBlanks(std::string_view text)
    {
        const std::size_t first = text.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos)
        {
            return std::string_view();
        }
        return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    }

    // ParseInteger：把文本解析成整数类型（Signed/Unsigned/Pointer）的位模式。
    // 传入：text 已剪空白；kind 大类；size 字节数。传出：patternOut 为该宽度的位模式（负数为补码）。
    EncodeStatus ParseInteger(std::string_view text, TypeKind kind, std::size_t size, std::uint64_t& patternOut)
    {
        // negative：是否有前导 '-'；body：去掉符号后的数字体；base：带 0x/0X 前缀为 16，否则 10。
        bool negative = false;
        std::string_view body = text;
        if (!body.empty() && body.front() == '-')
        {
            negative = true;
            body.remove_prefix(1);
        }
        int base = 10;
        if (body.size() >= 2 && body[0] == '0' && (body[1] == 'x' || body[1] == 'X'))
        {
            base = 16;
            body.remove_prefix(2);
        }
        // magnitude：数值的绝对值。from_chars 对无符号目标不接受任何符号字符，
        // 所以 "+1"、"--1"、"-" 都在这里被当成语法错误。
        std::uint64_t magnitude = 0;
        const char* const last = body.data() + body.size();
        const auto parsed = std::from_chars(body.data(), last, magnitude, base);
        // 先判语法（无数位、尾部多余字符）再判 64 位溢出：带尾巴的超长数字是语法错误。
        if (body.empty() || parsed.ec == std::errc::invalid_argument || parsed.ptr != last)
        {
            return EncodeStatus::BadNumber;
        }
        if (parsed.ec == std::errc::result_out_of_range)
        {
            return EncodeStatus::Overflow;
        }
        // 正负两个方向各自的绝对值上限：有符号为 2^(bits-1)-1 与 2^(bits-1)；
        // 无符号与指针为全宽上限与 0（只有 "-0" 能过）。
        const std::size_t bits = size * 8;
        const std::uint64_t positiveMax = (kind == TypeKind::Signed)
            ? ((1ULL << (bits - 1)) - 1)
            : ((bits == 64) ? ~0ULL : ((1ULL << bits) - 1));
        const std::uint64_t negativeMax = (kind == TypeKind::Signed) ? (positiveMax + 1) : 0;
        if (magnitude > (negative ? negativeMax : positiveMax))
        {
            return EncodeStatus::OutOfRange;
        }
        // 负数取无符号模运算的补码，再只保留目标宽度（范围已保证这一步无损）。
        patternOut = negative ? (0ULL - magnitude) : magnitude;
        if (bits < 64)
        {
            patternOut &= (1ULL << bits) - 1;
        }
        return EncodeStatus::Ok;
    }

    // MantissaHasNonzeroDigit：十进制浮点文本的尾数部分（'e'/'E' 之前）是否含 1..9，
    // 用来识别"文本明明非零、解析结果却是 0"的下溢。
    bool MantissaHasNonzeroDigit(std::string_view text)
    {
        for (const char c : text)
        {
            if (c == 'e' || c == 'E')
            {
                break;
            }
            if (c >= '1' && c <= '9')
            {
                return true;
            }
        }
        return false;
    }

    // ParseFloatBits：把文本解析成 f32/f64 的位模式。模板参数 FloatT/BitsT 同 FloatText。
    // 接受十进制与科学计数法，以及不区分大小写的 nan、inf、infinity（可带前导 '-'）。
    template <typename FloatT, typename BitsT>
    EncodeStatus ParseFloatBits(std::string_view text, std::uint64_t& patternOut)
    {
        // negative：是否有前导 '-'；body：去掉符号后的部分；kSignBit：符号位。
        bool negative = false;
        std::string_view body = text;
        if (!body.empty() && body.front() == '-')
        {
            negative = true;
            body.remove_prefix(1);
        }
        constexpr BitsT kSignBit = BitsT{ 1 } << (sizeof(BitsT) * 8 - 1);
        // word：小写化的 body，用来匹配特殊值单词。
        std::string word(body);
        for (char& c : word)
        {
            c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
        }
        if (word == "inf" || word == "infinity" || word == "nan")
        {
            const FloatT special = (word == "nan") ? std::numeric_limits<FloatT>::quiet_NaN()
                                                   : std::numeric_limits<FloatT>::infinity();
            patternOut = std::bit_cast<BitsT>(special) | (negative ? kSignBit : 0);
            return EncodeStatus::Ok;
        }
        // 首个非符号字符必须是数字或小数点：挡住 from_chars 顺带接受的 nan(...) 之类，
        // 它们在各标准库上的取舍不一。
        if (body.empty() || !((body.front() >= '0' && body.front() <= '9') || body.front() == '.'))
        {
            return EncodeStatus::BadNumber;
        }
        FloatT value = 0;
        const char* const last = text.data() + text.size();
        const auto parsed = std::from_chars(text.data(), last, value);
        if (parsed.ec == std::errc::invalid_argument || parsed.ptr != last)
        {
            return EncodeStatus::BadNumber;
        }
        // 上溢（以及部分标准库上的下溢）报 result_out_of_range；另一些标准库把下溢静默成 0。
        // 文本含非零数位而结果为 0 同样拒绝，不能让 "1e-50" 悄悄变成 0 写进去。
        if (parsed.ec == std::errc::result_out_of_range || (value == 0 && MantissaHasNonzeroDigit(text)))
        {
            return EncodeStatus::OutOfRange;
        }
        patternOut = std::bit_cast<BitsT>(value);
        return EncodeStatus::Ok;
    }

    // WritePattern：把 size 字节的位模式按字节序写进 bytesOut。
    void WritePattern(std::uint64_t pattern, std::size_t size, ByteOrder order, std::vector<std::uint8_t>& bytesOut)
    {
        bytesOut.assign(size, 0);
        for (std::size_t i = 0; i < size; ++i)
        {
            // index：第 i 个低位字节落在的下标；小端低位在前，大端低位在后。
            const std::size_t index = (order == ByteOrder::Little) ? i : (size - 1 - i);
            bytesOut[index] = static_cast<std::uint8_t>((pattern >> (8 * i)) & 0xFF);
        }
    }
} // namespace

std::vector<DecodedRow> DecodeAll(
    const std::uint8_t* bytes,
    std::size_t length,
    ByteOrder order,
    std::uint32_t pointerWidthBytes,
    IPointerNamer* namer)
{
    // 空指针当作没有字节，保证后面所有读取都有数据撑腰。
    if (bytes == nullptr)
    {
        length = 0;
    }
    // rows：按固定顺序累积的结果。
    std::vector<DecodedRow> rows;
    rows.reserve(kDecodedRowCount);
    AppendIntegerRows(rows, bytes, length, order);
    rows.push_back(BuildFloatRow<float, std::uint32_t>("f32", bytes, length, order));
    rows.push_back(BuildFloatRow<double, std::uint64_t>("f64", bytes, length, order));
    rows.push_back(BuildPointerRow(bytes, length, order, pointerWidthBytes, namer));
    rows.push_back(BuildFiletimeRow(bytes, length, order));
    rows.push_back(BuildUnixTimeRow(4, bytes, length, order));
    rows.push_back(BuildUnixTimeRow(8, bytes, length, order));
    rows.push_back(BuildGuidRow(bytes, length));
    rows.push_back(BuildAsciiRow(bytes, length));
    rows.push_back(BuildUtf16Row(bytes, length));
    return rows;
}

EncodeStatus EncodeValue(
    std::string_view type,
    std::string_view text,
    ByteOrder order,
    std::uint32_t pointerWidthBytes,
    std::vector<std::uint8_t>& bytesOut)
{
    // 每次调用先清空：失败路径保证 bytesOut 为空，不留上一次的残余。
    bytesOut.clear();
    // info：按名字查到的类型描述；查不到返回 UnknownType（先于任何文本检查）。
    const EncodeType* info = nullptr;
    for (const EncodeType& candidate : kEncodeTypes)
    {
        if (type == candidate.name)
        {
            info = &candidate;
            break;
        }
    }
    if (info == nullptr)
    {
        return EncodeStatus::UnknownType;
    }
    // size：本次编码的字节数；ptr 取调用方给的宽度，只允许 4 或 8。
    std::size_t size = info->size;
    if (info->kind == TypeKind::Pointer)
    {
        if (pointerWidthBytes != 4 && pointerWidthBytes != 8)
        {
            return EncodeStatus::UnknownType;
        }
        size = pointerWidthBytes;
    }
    // pattern：解析出的位模式；status：解析结果，失败直接返回，bytesOut 保持为空。
    const std::string_view trimmed = TrimBlanks(text);
    std::uint64_t pattern = 0;
    EncodeStatus status = EncodeStatus::Ok;
    if (info->kind != TypeKind::Float)
    {
        status = ParseInteger(trimmed, info->kind, size, pattern);
    }
    else if (size == 8)
    {
        status = ParseFloatBits<double, std::uint64_t>(trimmed, pattern);
    }
    else
    {
        status = ParseFloatBits<float, std::uint32_t>(trimmed, pattern);
    }
    if (status != EncodeStatus::Ok)
    {
        return status;
    }
    WritePattern(pattern, size, order, bytesOut);
    return EncodeStatus::Ok;
}
}
