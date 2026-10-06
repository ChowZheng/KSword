// 内存工作台数据解释器（shared/evidence/memory_workbench/MemoryValueDecode.h）的离线测试。
//
// 为什么这个模块值得一整套穷举断言：它属于**算错了不会报错**的那一类。
// 时间换算差一天、有符号数的补码错位、字节序反了、闰年规则漏掉逢四百，界面照样
// 显示一个像样的值，用户只会以为目标内存就是那样。旧控件还有一个结构性缺陷：
// 选区长度决定能看到什么类型。本模块恒返回固定的 17 行，字节不足的行标记为不可用。
//
// 断言原则与 NumericTextParseTests.cpp / MemoryAddressExprTests.cpp 一致：
//   * 期望值手算写死（日期常量另用 GNU date 与 .NET 的已知常量交叉核对过），
//     绝不从被测函数反算；
//   * 边界两侧都测（i8 的 127/128/-128/-129、闰年的 2 月 28/29/3 月 1、时间范围的
//     两端、字节不足逐长度 0..16、32 字节字符串上限的两侧）；
//   * 该被拒绝的输入必须被显式拒绝，并核对状态码，失败时 bytesOut 必须为空；
//   * 两种字节序、指针宽度 4 与 8、namer 命中与未命中各测一遍；
//   * 公历换算另用一套"逐日推进"的参考日历做大范围扫描，两套实现互相核对。

#include "TestSupport.h"

#include "../shared/evidence/memory_workbench/MemoryValueDecode.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

namespace {

using ksword::memwb::ByteOrder;
using ksword::memwb::DecodeAll;
using ksword::memwb::DecodedRow;
using ksword::memwb::EncodeStatus;
using ksword::memwb::EncodeValue;
using ksword::memwb::IPointerNamer;
using ksword::memwb::kDecodedRowCount;
using KswordTests::Suite;

using Bytes = std::vector<std::uint8_t>;
using Rows = std::vector<DecodedRow>;

constexpr ByteOrder kLe = ByteOrder::Little;
constexpr ByteOrder kBe = ByteOrder::Big;

// Widen：把说明文字转成宽字符；非 ASCII 字节显示成 '?'，免得失败信息本身转换出错。
std::wstring Widen(const std::string& text) {
    std::wstring wide;
    for (const char c : text) {
        wide.push_back((c >= 0x20 && c < 0x7F) ? static_cast<wchar_t>(c) : L'?');
    }
    return wide;
}

// Check：登记一条断言；what 是失败时展示的说明。
void Check(Suite& suite, bool condition, const std::string& what) {
    suite.expect(condition, (L"value decode: " + Widen(what)).c_str());
}

// B：用整数列表造字节串，让期望字节读起来像手抄的十六进制。
Bytes B(std::initializer_list<int> values) {
    Bytes out;
    for (const int value : values) {
        out.push_back(static_cast<std::uint8_t>(value));
    }
    return out;
}

// Rep：count 个相同字节。
Bytes Rep(int value, std::size_t count) {
    return Bytes(count, static_cast<std::uint8_t>(value));
}

// Le / Be：把数值的低 size 个字节按小端 / 大端排成字节串。
Bytes Le(std::uint64_t value, std::size_t size) {
    Bytes out(size, 0);
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF);
    }
    return out;
}

Bytes Be(std::uint64_t value, std::size_t size) {
    const Bytes out = Le(value, size);
    return Bytes(out.rbegin(), out.rend());
}

// Order：按字节序排字节，让表驱动测试一行覆盖两种字节序。
Bytes Order(ByteOrder order, std::uint64_t value, std::size_t size) {
    return order == kLe ? Le(value, size) : Be(value, size);
}

// U：有符号数转无符号位模式，供 Le/Be 写负数。
std::uint64_t U(std::int64_t value) {
    return static_cast<std::uint64_t>(value);
}

// Row：按 label 取行（拷贝返回，避免引用悬垂到临时 vector）；找不到返回一个哨兵行。
DecodedRow Row(const Rows& rows, const char* label) {
    for (const DecodedRow& row : rows) {
        if (row.label == label) {
            return row;
        }
    }
    DecodedRow missing;
    missing.label = "<missing>";
    return missing;
}

// Dec：解码的便捷包装，默认 8 字节指针、无 namer。
Rows Dec(const Bytes& bytes, ByteOrder order = kLe, std::uint32_t width = 8, IPointerNamer* namer = nullptr) {
    return DecodeAll(bytes, order, width, namer);
}

// RowCase：一条"字节 + 字节序 -> 某一行"的期望。copy 为空指针表示与 text 相同。
struct RowCase {
    Bytes bytes;
    ByteOrder order;
    const char* label;
    const char* text;
    const char* copy = nullptr;
};

// NamedSize：一个名字配一个字节数（字节不足测试的"所需字节"、往返测试的"类型宽度"共用）。
struct NamedSize {
    const char* name;
    std::size_t size;
};

// TypeText：一个类型名配一段文本，以及 ptr 用的宽度（其余类型忽略）。
struct TypeText {
    const char* type;
    const char* text;
    std::uint32_t width = 8;
};

// RunRowCases：逐条核对 available、valid、text、copyText。
void RunRowCases(Suite& suite, const std::string& group, const std::vector<RowCase>& cases) {
    for (const RowCase& item : cases) {
        const DecodedRow row = Row(Dec(item.bytes, item.order), item.label);
        const std::string want = item.copy != nullptr ? item.copy : item.text;
        const std::string name = group + " " + item.label + (item.order == kBe ? " BE" : " LE");
        Check(suite, row.available && row.valid, name + " is available and valid");
        Check(suite, row.text == item.text, name + " text [" + row.text + "] want [" + item.text + "]");
        Check(suite, row.copyText == want, name + " copy [" + row.copyText + "] want [" + want + "]");
    }
}

// 一、行的布局：恒为固定的 17 行、固定顺序，与输入长度无关。
void TestLayout(Suite& suite) {
    static const char* const kLabels[] = {
        "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "f32", "f64", "ptr",
        "filetime", "time_t32", "time_t64", "guid", "ascii", "utf16",
    };
    Check(suite, kDecodedRowCount == 17 && sizeof(kLabels) / sizeof(kLabels[0]) == 17, "row count constant is 17");
    // 空串、16 字节、64 字节，行数与顺序必须完全一致。
    for (const Bytes& input : { Bytes(), Rep(0, 16), Rep(0x41, 64) }) {
        const Rows rows = Dec(input);
        bool sameOrder = rows.size() == kDecodedRowCount;
        for (std::size_t i = 0; sameOrder && i < rows.size(); ++i) {
            sameOrder = rows[i].label == kLabels[i];
        }
        Check(suite, sameOrder, "fixed 17 rows in fixed order for length " + std::to_string(input.size()));
    }
    // 空指针当作没有字节：17 行全部不可用。
    const Rows fromNull = DecodeAll(static_cast<const std::uint8_t*>(nullptr), 5, kLe, 8);
    bool allEmpty = fromNull.size() == kDecodedRowCount;
    for (const DecodedRow& row : fromNull) {
        allEmpty = allEmpty && !row.available && row.valid && row.text.empty() && row.copyText.empty();
    }
    Check(suite, allEmpty, "null pointer is treated as zero bytes: 17 unavailable rows");
}

// 二、整数：手算样例、字节序、补码位模式、补零。
void TestIntegers(Suite& suite) {
    const Bytes seq = B({ 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF });
    const Bytes word = B({ 0x78, 0x56, 0x34, 0x12 });
    RunRowCases(suite, "integer", {
        // 需求给定的样例：78 56 34 12 小端 = 0x12345678，大端 = 0x78563412。
        { word, kLe, "u32", "305419896 (0x12345678)", "305419896" },
        { word, kBe, "u32", "2018915346 (0x78563412)", "2018915346" },
        { B({ 0xFF }), kLe, "i8", "-1 (0xFF)", "-1" }, { B({ 0xFF }), kLe, "u8", "255 (0xFF)", "255" },
        // 同一段 01 23 .. EF，小端与大端逐宽度对照（十进制用 shell 另行算过）。
        { seq, kLe, "i8", "1 (0x01)", "1" },
        { seq, kLe, "i16", "8961 (0x2301)", "8961" },
        { seq, kLe, "i32", "1732584193 (0x67452301)", "1732584193" },
        { seq, kLe, "i64", "-1167088121787636991 (0xEFCDAB8967452301)", "-1167088121787636991" },
        { seq, kLe, "u64", "17279655951921914625 (0xEFCDAB8967452301)", "17279655951921914625" },
        { seq, kBe, "i16", "291 (0x0123)", "291" },
        { seq, kBe, "i32", "19088743 (0x01234567)", "19088743" },
        { seq, kBe, "i64", "81985529216486895 (0x0123456789ABCDEF)", "81985529216486895" },
        { seq, kBe, "u64", "81985529216486895 (0x0123456789ABCDEF)", "81985529216486895" },
        // 同一字节对（FE FF）：小端 = 0xFFFE = -2，大端 = 0xFEFF = -257。
        { B({ 0xFE, 0xFF }), kLe, "i16", "-2 (0xFFFE)", "-2" },
        { B({ 0xFE, 0xFF }), kBe, "i16", "-257 (0xFEFF)", "-257" },
        // 十六进制恒补零到类型宽度，不是最短写法。
        { B({ 5 }), kLe, "u8", "5 (0x05)", "5" }, { B({ 1, 0 }), kLe, "u16", "1 (0x0001)", "1" },
        { Rep(0, 4), kLe, "u32", "0 (0x00000000)", "0" }, { Rep(0, 8), kLe, "u64", "0 (0x0000000000000000)", "0" },
        // 全 FF：有符号显示 -1，十六进制仍是同一个位模式。
        { Rep(0xFF, 8), kLe, "i16", "-1 (0xFFFF)", "-1" }, { Rep(0xFF, 8), kLe, "u16", "65535 (0xFFFF)", "65535" },
        { Rep(0xFF, 8), kLe, "i32", "-1 (0xFFFFFFFF)", "-1" }, { Rep(0xFF, 8), kLe, "u32", "4294967295 (0xFFFFFFFF)", "4294967295" },
        { Rep(0xFF, 8), kLe, "i64", "-1 (0xFFFFFFFFFFFFFFFF)", "-1" },
        { Rep(0xFF, 8), kLe, "u64", "18446744073709551615 (0xFFFFFFFFFFFFFFFF)", "18446744073709551615" },
        // 符号边界两侧：最大正数与最小负数。
        { B({ 0x7F }), kLe, "i8", "127 (0x7F)", "127" }, { B({ 0x80 }), kLe, "i8", "-128 (0x80)", "-128" },
        { B({ 0x80 }), kLe, "u8", "128 (0x80)", "128" }, { B({ 0xFF, 0x7F }), kLe, "i16", "32767 (0x7FFF)", "32767" },
        { B({ 0x00, 0x80 }), kLe, "i16", "-32768 (0x8000)", "-32768" }, { B({ 0x00, 0x80 }), kLe, "u16", "32768 (0x8000)", "32768" },
        { B({ 0xFF, 0xFF, 0xFF, 0x7F }), kLe, "i32", "2147483647 (0x7FFFFFFF)", "2147483647" },
        { B({ 0x00, 0x00, 0x00, 0x80 }), kLe, "i32", "-2147483648 (0x80000000)", "-2147483648" },
        { B({ 0x00, 0x00, 0x00, 0x80 }), kLe, "u32", "2147483648 (0x80000000)", "2147483648" },
        { B({ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F }), kLe, "i64", "9223372036854775807 (0x7FFFFFFFFFFFFFFF)", "9223372036854775807" },
        { B({ 0, 0, 0, 0, 0, 0, 0, 0x80 }), kLe, "i64", "-9223372036854775808 (0x8000000000000000)", "-9223372036854775808" },
        { B({ 0, 0, 0, 0, 0, 0, 0, 0x80 }), kLe, "u64", "9223372036854775808 (0x8000000000000000)", "9223372036854775808" },
    });
}

// FloatCase：一个位模式及其期望文本；每条都按小端与大端各排一遍字节。
struct FloatCase {
    std::uint64_t bits;
    const char* label;
    const char* text;
};

// 三、浮点：最短往返、特殊值只看位模式。
void TestFloats(Suite& suite) {
    // 字节级样例：需求给定的 f32 0x3F800000 与 f64 1.0 的字节。
    RunRowCases(suite, "float bytes", {
        { B({ 0x00, 0x00, 0x80, 0x3F }), kLe, "f32", "1" },
        { B({ 0x3F, 0x80, 0x00, 0x00 }), kBe, "f32", "1" },
        { B({ 0, 0, 0, 0, 0, 0, 0xF0, 0x3F }), kLe, "f64", "1" },
        { B({ 0x3F, 0xF0, 0, 0, 0, 0, 0, 0 }), kBe, "f64", "1" },
    });
    const FloatCase cases[] = {
        { 0x3DCCCCCDULL, "f32", "0.1" }, { 0xC0200000ULL, "f32", "-2.5" }, { 0x40490FDBULL, "f32", "3.1415927" },
        { 0x7F7FFFFFULL, "f32", "3.4028235e+38" }, { 0xFF7FFFFFULL, "f32", "-3.4028235e+38" }, { 1ULL, "f32", "1e-45" },
        { 0ULL, "f32", "0" }, { 0x80000000ULL, "f32", "-0" },
        // NaN：任何符号、任何载荷（含信号 NaN）都显示 NaN；无穷带符号。
        { 0x7FC00000ULL, "f32", "NaN" }, { 0xFFC00000ULL, "f32", "NaN" }, { 0x7F800001ULL, "f32", "NaN" },
        { 0x7F800000ULL, "f32", "Inf" }, { 0xFF800000ULL, "f32", "-Inf" },
        { 0x3FB999999999999AULL, "f64", "0.1" }, { 0xC004000000000000ULL, "f64", "-2.5" },
        { 0x400921FB54442D18ULL, "f64", "3.141592653589793" }, { 0x7FEFFFFFFFFFFFFFULL, "f64", "1.7976931348623157e+308" },
        { 1ULL, "f64", "5e-324" }, { 0ULL, "f64", "0" }, { 0x8000000000000000ULL, "f64", "-0" },
        { 0x7FF8000000000000ULL, "f64", "NaN" }, { 0xFFF8000000000000ULL, "f64", "NaN" }, { 0x7FF0000000000001ULL, "f64", "NaN" },
        { 0x7FF0000000000000ULL, "f64", "Inf" }, { 0xFFF0000000000000ULL, "f64", "-Inf" },
    };
    for (const ByteOrder order : { kLe, kBe }) {
        for (const FloatCase& item : cases) {
            const DecodedRow row = Row(Dec(Order(order, item.bits, std::string(item.label) == "f32" ? 4 : 8), order), item.label);
            const std::string name = std::string("float ") + item.label + (order == kBe ? " BE " : " LE ") + item.text;
            Check(suite, row.available && row.valid && row.text == item.text && row.copyText == item.text, name + " got [" + row.text + "]");
        }
    }
}

// IsoOf：把展示文本 "YYYY-MM-DD hh:mm:ss[.f] UTC" 变成复制文本 "YYYY-MM-DDThh:mm:ss[.f]Z"。
std::string IsoOf(const std::string& text) {
    std::string iso = text.substr(0, text.size() - 4);
    iso[10] = 'T';
    return iso + "Z";
}

// TimeCase：一个时间期望。rawDecimal 为空指针表示合法（text 是时间文本）；
// 否则该值应判内容非法，text 必为 "out of range"，copyText 必为 rawDecimal。
struct TimeCase {
    std::uint64_t raw;
    const char* text;
    const char* rawDecimal;
};

// RunTimeCases：每个值在小端与大端下各测一遍。
void RunTimeCases(Suite& suite, const char* label, std::size_t size, const std::vector<TimeCase>& cases) {
    for (const ByteOrder order : { kLe, kBe }) {
        for (const TimeCase& item : cases) {
            const DecodedRow row = Row(Dec(Order(order, item.raw, size), order), label);
            const bool valid = item.rawDecimal == nullptr;
            const std::string want = valid ? IsoOf(item.text) : item.rawDecimal;
            const std::string name = std::string(label) + (order == kBe ? " BE " : " LE ") + item.text;
            Check(suite, row.available && row.valid == valid, name + " availability/validity");
            Check(suite, row.text == item.text, name + " text [" + row.text + "]");
            Check(suite, row.copyText == want, name + " copy [" + row.copyText + "] want [" + want + "]");
        }
    }
}

// 四、FILETIME：范围、闰年边界、小数秒、0 哨兵。
void TestFiletime(Suite& suite) {
    // 这些刻度数都是 (Unix 秒 + 11644473600) * 10^7，Unix 秒另由 GNU date 核对。
    RunTimeCases(suite, "filetime", 8, {
        { 116444736000000000ULL, "1970-01-01 00:00:00.0000000 UTC", nullptr },
        { 132539328000000000ULL, "2021-01-01 00:00:00.0000000 UTC", nullptr },
        { 1ULL, "1601-01-01 00:00:00.0000001 UTC", nullptr },
        { 116444736001234567ULL, "1970-01-01 00:00:00.1234567 UTC", nullptr },
        { 116444736010000000ULL, "1970-01-01 00:00:01.0000000 UTC", nullptr },
        { 94405823999999999ULL, "1900-02-28 23:59:59.9999999 UTC", nullptr },
        { 94405824000000000ULL, "1900-03-01 00:00:00.0000000 UTC", nullptr },
        { 125962560000000000ULL, "2000-02-29 00:00:00.0000000 UTC", nullptr },
        { 125963424000000000ULL, "2000-03-01 00:00:00.0000000 UTC", nullptr },
        { 157520159990000000ULL, "2100-02-28 23:59:59.0000000 UTC", nullptr },
        { 157520160000000000ULL, "2100-03-01 00:00:00.0000000 UTC", nullptr },
        { 252191231990000000ULL, "2400-02-29 23:59:59.0000000 UTC", nullptr },
        // 范围上界：9999-12-31 23:59:59.9999999 合法，再多 1 个刻度即越界。
        { 2650467743999999999ULL, "9999-12-31 23:59:59.9999999 UTC", nullptr },
        { 2650467744000000000ULL, "out of range", "2650467744000000000" },
        { 0xFFFFFFFFFFFFFFFFULL, "out of range", "18446744073709551615" },
        // 0 是"未设置"哨兵，不显示成 1601-01-01。
        { 0ULL, "out of range", "0" },
    });
}

// 五、time_t32 / time_t64：Unix 秒、负数向下取整、闰年边界、范围。
void TestUnixTime(Suite& suite) {
    // 32 位：恒在范围内，两端是著名的 2038 与 1901 边界。
    RunTimeCases(suite, "time_t32", 4, {
        { 0, "1970-01-01 00:00:00 UTC", nullptr }, { 2147483647ULL, "2038-01-19 03:14:07 UTC", nullptr },
        { U(-1), "1969-12-31 23:59:59 UTC", nullptr }, { U(-2147483647LL - 1), "1901-12-13 20:45:52 UTC", nullptr },
        { 86399, "1970-01-01 23:59:59 UTC", nullptr }, { 3661, "1970-01-01 01:01:01 UTC", nullptr },
    });
    // 64 位：同一批边界，外加 32 位放不下的值与 0001/9999 两端。
    RunTimeCases(suite, "time_t64", 8, {
        { 0, "1970-01-01 00:00:00 UTC", nullptr }, { U(-1), "1969-12-31 23:59:59 UTC", nullptr },
        { U(-86400), "1969-12-31 00:00:00 UTC", nullptr }, { U(-86401), "1969-12-30 23:59:59 UTC", nullptr },
        { 86399, "1970-01-01 23:59:59 UTC", nullptr }, { 2147483648ULL, "2038-01-19 03:14:08 UTC", nullptr },
        { 1609459200ULL, "2021-01-01 00:00:00 UTC", nullptr },
        // 闰年：1600 闰、1900 非闰、2000 闰、2100 非闰、2400 闰。
        { U(-11670912001LL), "1600-02-29 23:59:59 UTC", nullptr }, { U(-11670912000LL), "1600-03-01 00:00:00 UTC", nullptr },
        { U(-2203891201LL), "1900-02-28 23:59:59 UTC", nullptr }, { U(-2203891200LL), "1900-03-01 00:00:00 UTC", nullptr },
        { 951782400ULL, "2000-02-29 00:00:00 UTC", nullptr }, { 951868799ULL, "2000-02-29 23:59:59 UTC", nullptr },
        { 951868800ULL, "2000-03-01 00:00:00 UTC", nullptr }, { 4107542399ULL, "2100-02-28 23:59:59 UTC", nullptr },
        { 4107542400ULL, "2100-03-01 00:00:00 UTC", nullptr }, { 13574649599ULL, "2400-02-29 23:59:59 UTC", nullptr },
        { 13574649600ULL, "2400-03-01 00:00:00 UTC", nullptr },
        // 范围两端：0001-01-01 与 9999-12-31 恰好合法，各越一秒即非法。
        { U(-62135596800LL), "0001-01-01 00:00:00 UTC", nullptr },
        { 253402300799ULL, "9999-12-31 23:59:59 UTC", nullptr },
        { U(-62135596801LL), "out of range", "-62135596801" },
        { 253402300800ULL, "out of range", "253402300800" },
        { U(-9223372036854775807LL - 1), "out of range", "-9223372036854775808" },
        { 9223372036854775807ULL, "out of range", "9223372036854775807" },
    });
}

// RefIsLeap：参考日历的闰年判定，写法与被测实现刻意不同（先判 400，再判 100）。
bool RefIsLeap(int year) {
    if (year % 400 == 0) {
        return true;
    }
    if (year % 100 == 0) {
        return false;
    }
    return year % 4 == 0;
}

// RefMonthLength：参考日历的月长度。
int RefMonthLength(int year, int month) {
    static const int kLength[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return (month == 2 && RefIsLeap(year)) ? 29 : kLength[month - 1];
}

// 六、公历扫描：逐日推进的参考日历与被测的周期拆分互相核对。
void TestCalendarSweep(Suite& suite) {
    // kTimeOfDay：每个采样日固定取 13:24:35；其余是 9999-12-31 与两个纪元距 0001-01-01 的天数。
    constexpr std::int64_t kTimeOfDay = 13 * 3600 + 24 * 60 + 35;
    constexpr std::int64_t kMaxDay = 3652058;
    constexpr std::int64_t kUnixDay = 719162;
    constexpr std::int64_t kFiletimeDay = 584388;
    // year/month/day：参考日历当前日期；samples：采样次数；firstFailure：第一处不一致。
    int year = 1;
    int month = 1;
    int day = 1;
    std::size_t samples = 0;
    std::string firstFailure;
    for (std::int64_t index = 0; index <= kMaxDay; ++index) {
        // 取样：全程每 61 天一次，每年末初，4 的倍数年的 2 月底到 3 月初，
        // 以及每个整百年的整个 2、3 月（逢百闰否的分界）。
        const bool sample = index % 61 == 0 || (month == 12 && day == 31) || (month == 1 && day == 1)
            || (year % 4 == 0 && ((month == 2 && day >= 28) || (month == 3 && day == 1)))
            || (year % 100 == 0 && (month == 2 || month == 3));
        if (sample) {
            ++samples;
            char prefix[32];
            std::snprintf(prefix, sizeof(prefix), "%04d-%02d-%02d", year, month, day);
            const std::string wantUnix = std::string(prefix) + " 13:24:35 UTC";
            const std::string gotUnix = Row(Dec(Le(U((index - kUnixDay) * 86400 + kTimeOfDay), 8)), "time_t64").text;
            if (gotUnix != wantUnix && firstFailure.empty()) {
                firstFailure = "time_t64 want " + wantUnix + " got " + gotUnix;
            }
            // FILETIME 的范围从 1601 年起，更早的日子只测 time_t64。
            const std::uint64_t ticks = static_cast<std::uint64_t>(index - kFiletimeDay) * 864000000000ULL
                + static_cast<std::uint64_t>(kTimeOfDay) * 10000000ULL + 1234567ULL;
            const std::string wantFile = std::string(prefix) + " 13:24:35.1234567 UTC";
            const std::string gotFile = Row(Dec(Le(ticks, 8)), "filetime").text;
            if (index >= kFiletimeDay && gotFile != wantFile && firstFailure.empty()) {
                firstFailure = "filetime want " + wantFile + " got " + gotFile;
            }
        }
        // 参考日历前进一天。
        if (++day > RefMonthLength(year, month)) {
            day = 1;
            if (++month > 12) {
                month = 1;
                ++year;
            }
        }
    }
    Check(suite, firstFailure.empty(), "calendar sweep first mismatch: " + firstFailure);
    // 参考日历走完恰好滚到 10000-01-01，证明 kMaxDay 与参考日历自洽，扫描覆盖了整个范围。
    Check(suite, year == 10000 && month == 1 && day == 1, "reference calendar ends at 10000-01-01");
    Check(suite, samples > 50000, "calendar sweep sampled a meaningful number of days");
}

// 七、GUID。
void TestGuid(Suite& suite) {
    const Bytes sample = B({ 0x33, 0x22, 0x11, 0x00, 0x55, 0x44, 0x77, 0x66, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF });
    const Bytes dispatch = B({ 0x00, 0x04, 0x02, 0x00, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46 });
    Bytes longer = sample;
    longer.insert(longer.end(), std::size_t{ 8 }, std::uint8_t{ 0x55 });
    RunRowCases(suite, "guid", {
        // Data1/2/3 小端、Data4 按字节序：需求给定的样例。
        { sample, kLe, "guid", "00112233-4455-6677-8899-aabbccddeeff", "{00112233-4455-6677-8899-AABBCCDDEEFF}" },
        // GUID 固定 Windows 内存布局，ByteOrder 不影响它；多于 16 字节只取前 16 个。
        { sample, kBe, "guid", "00112233-4455-6677-8899-aabbccddeeff", "{00112233-4455-6677-8899-AABBCCDDEEFF}" },
        { longer, kLe, "guid", "00112233-4455-6677-8899-aabbccddeeff", "{00112233-4455-6677-8899-AABBCCDDEEFF}" },
        { Rep(0, 16), kLe, "guid", "00000000-0000-0000-0000-000000000000", "{00000000-0000-0000-0000-000000000000}" },
        { Rep(0xFF, 16), kLe, "guid", "ffffffff-ffff-ffff-ffff-ffffffffffff", "{FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF}" },
        // IID_IDispatch {00020400-0000-0000-C000-000000000046}。
        { dispatch, kLe, "guid", "00020400-0000-0000-c000-000000000046", "{00020400-0000-0000-C000-000000000046}" },
    });
}

// 八、ascii 与 utf16。
void TestStrings(Suite& suite) {
    // ascii：取到首个 NUL / 不可打印字节 / 32 字节。字符串先放进命名变量，RowCase 只存指针。
    const std::string a31(31, 'A');
    const std::string a32(32, 'A');
    RunRowCases(suite, "ascii", {
        { B({ 'H', 'e', 'l', 'l', 'o', 0, 'j', 'u', 'n', 'k' }), kLe, "ascii", "Hello" },
        { B({ 'H', 'e', 'l', 'l', 'o' }), kLe, "ascii", "Hello" },
        { B({ ' ' }), kLe, "ascii", " " }, { B({ '~' }), kBe, "ascii", "~" },
        { B({ 'A', 'B', 0x7F, 'C' }), kLe, "ascii", "AB" }, { B({ 'A', 'B', 0x80, 'C' }), kLe, "ascii", "AB" },
        { B({ 'A', 0x09, 'B' }), kLe, "ascii", "A" }, { B({ 'A', 0x1F, 'B' }), kLe, "ascii", "A" },
        { Rep('A', 31), kLe, "ascii", a31.c_str() }, { Rep('A', 32), kLe, "ascii", a32.c_str() },
        { Rep('A', 33), kLe, "ascii", a32.c_str() }, { Rep('A', 80), kBe, "ascii", a32.c_str() },
    });
    // 没有任何可打印字节：不可用，而不是可用的空串。
    for (const Bytes& bytes : { Bytes(), B({ 0 }), B({ 0x1F, 'A' }), B({ 0x80, 'A' }), B({ 0x7F }) }) {
        const DecodedRow row = Row(Dec(bytes), "ascii");
        Check(suite, !row.available && row.text.empty() && row.copyText.empty(), "ascii with no printable first byte is unavailable");
    }
    // utf16：UTF-16LE 转 UTF-8。预期字节手算：U+4E2D = E4 B8 AD，U+00E9 = C3 A9，U+0800 = E0 A0 80，
    // U+0080 = C2 80，U+1F600 = F0 9F 98 80，U+10000 = F0 90 80 80，U+10FFFF = F4 8F BF BF。
    RunRowCases(suite, "utf16", {
        { B({ 'H', 0, 'i', 0, 0, 0, 'x', 0 }), kLe, "utf16", "Hi" }, { B({ 'H', 0, 'i', 0 }), kLe, "utf16", "Hi" },
        { B({ 'H', 0, 'i', 0 }), kBe, "utf16", "Hi" }, { B({ 'H', 0, 'i', 0, 0x41 }), kLe, "utf16", "Hi" },
        { B({ 0x2D, 0x4E }), kLe, "utf16", "\xE4\xB8\xAD" }, { B({ 0xE9, 0x00 }), kLe, "utf16", "\xC3\xA9" },
        { B({ 0x00, 0x08 }), kLe, "utf16", "\xE0\xA0\x80" }, { B({ 0x7F, 0x00, 0x80, 0x00 }), kLe, "utf16", "\x7F\xC2\x80" },
        { B({ 0x3D, 0xD8, 0x00, 0xDE }), kLe, "utf16", "\xF0\x9F\x98\x80" }, { B({ 0x00, 0xD8, 0x00, 0xDC }), kLe, "utf16", "\xF0\x90\x80\x80" },
        { B({ 0xFF, 0xDB, 0xFF, 0xDF }), kLe, "utf16", "\xF4\x8F\xBF\xBF" },
    });
    // 上限：前 16 个码元。20 个 'a' 只取 16 个；恰好 16 个取 16 个；15 个取 15 个。
    Bytes twenty;
    for (int i = 0; i < 20; ++i) {
        twenty.push_back('a');
        twenty.push_back(0);
    }
    const auto head = [&twenty](std::ptrdiff_t bytes) {
        return Bytes(twenty.begin(), twenty.begin() + bytes);
    };
    Check(suite, Row(Dec(twenty), "utf16").text == std::string(16, 'a'), "utf16 stops after 16 code units");
    Check(suite, Row(Dec(head(32)), "utf16").text == std::string(16, 'a'), "utf16 exactly 16 units");
    Check(suite, Row(Dec(head(30)), "utf16").text == std::string(15, 'a'), "utf16 with 15 units");
    // 起点在窗口最后一个位置（第 15 个码元）的代理对被补全；起点在第 16 个位置的不取。
    Bytes straddle = head(30);
    Bytes pastEnd = head(32);
    for (const int byte : { 0x3D, 0xD8, 0x00, 0xDE }) {
        straddle.push_back(static_cast<std::uint8_t>(byte));
        pastEnd.push_back(static_cast<std::uint8_t>(byte));
    }
    Check(suite, Row(Dec(straddle), "utf16").text == std::string(15, 'a') + "\xF0\x9F\x98\x80", "utf16 surrogate pair straddling the window is completed");
    Check(suite, Row(Dec(pastEnd), "utf16").text == std::string(16, 'a'), "utf16 pair starting past the window is not read");
    // 空与不足：首个码元为 NUL、长度 0、长度 1 都不可用。
    for (const Bytes& bytes : { Bytes(), B({ 'A' }), B({ 0, 0, 'A', 0 }) }) {
        const DecodedRow row = Row(Dec(bytes), "utf16");
        Check(suite, !row.available && row.valid && row.text.empty(), "utf16 without a character is unavailable");
    }
    // 孤立代理项：valid=false，输出 U+FFFD（EF BF BD），其余字符照常输出。
    const std::vector<RowCase> lones = {
        { B({ 0x00, 0xD8, 'A', 0 }), kLe, "utf16", "\xEF\xBF\xBD" "A" }, { B({ 0x00, 0xDC, 'A', 0 }), kLe, "utf16", "\xEF\xBF\xBD" "A" },
        { B({ 'A', 0, 0x00, 0xD8 }), kLe, "utf16", "A\xEF\xBF\xBD" }, { B({ 0x00, 0xDC, 0x00, 0xD8 }), kLe, "utf16", "\xEF\xBF\xBD\xEF\xBF\xBD" },
        { B({ 0x00, 0xD8, 0x00, 0xD8, 0x00, 0xDC }), kLe, "utf16", "\xEF\xBF\xBD\xF0\x90\x80\x80" },
    };
    for (const RowCase& item : lones) {
        const DecodedRow row = Row(Dec(item.bytes), "utf16");
        Check(suite, row.available && !row.valid && row.text == item.text && row.copyText == item.text && !row.Usable(), "utf16 lone surrogate is invalid with U+FFFD");
    }
    Check(suite, Row(Dec(B({ 'H', 0 })), "utf16").Usable(), "a valid available row is usable");
}

// FakeNamer：按预设表命中；记录每次调用的地址，并检查调用时 out 是否为空。
class FakeNamer final : public IPointerNamer {
public:
    // names：地址 -> 描述；表里没有的地址返回 false。
    std::map<std::uint64_t, std::string> names;
    // calls：每次 Describe 收到的地址；outWasDirty：是否有过 out 进来时不为空的调用。
    std::vector<std::uint64_t> calls;
    bool outWasDirty = false;

    bool Describe(std::uint64_t address, std::string& out) override {
        calls.push_back(address);
        outWasDirty = outWasDirty || !out.empty();
        const auto found = names.find(address);
        if (found == names.end()) {
            return false;
        }
        out = found->second;
        return true;
    }
};

// LyingNamer：返回 false 却往 out 里写了东西，指针行不得采用它。
class LyingNamer final : public IPointerNamer {
public:
    bool Describe(std::uint64_t, std::string& out) override {
        out = "junk+0x1";
        return false;
    }
};

// 九、指针：宽度 4 / 8、字节序、namer 命中与未命中。
void TestPointer(Suite& suite) {
    const Bytes user64 = B({ 0x00, 0x00, 0x40, 0x12, 0xF6, 0x7F, 0x00, 0x00 });
    FakeNamer namer;
    namer.names[0x7FF612400000ULL] = "app.exe+0x0";
    const DecodedRow hit = Row(Dec(user64, kLe, 8, &namer), "ptr");
    Check(suite, hit.available && hit.valid, "ptr 8 LE available");
    Check(suite, hit.text == "0x00007FF612400000 (app.exe+0x0)", "ptr 8 LE with namer hit: " + hit.text);
    Check(suite, hit.copyText == "0x00007FF612400000", "ptr copy text carries no description: " + hit.copyText);
    Check(suite, namer.calls == std::vector<std::uint64_t>{ 0x7FF612400000ULL } && !namer.outWasDirty, "namer called once with the decoded address and an empty out");
    // 没有 namer / namer 未命中 / namer 命中但描述为空 / namer 撒谎：都不追加描述。
    Check(suite, Row(Dec(user64), "ptr").text == "0x00007FF612400000", "ptr without namer has no suffix");
    FakeNamer miss;
    Check(suite, Row(Dec(user64, kLe, 8, &miss), "ptr").text == "0x00007FF612400000" && miss.calls.size() == 1, "namer miss adds nothing");
    FakeNamer empty;
    empty.names[0x7FF612400000ULL] = "";
    Check(suite, Row(Dec(user64, kLe, 8, &empty), "ptr").text == "0x00007FF612400000", "namer hit with empty text adds nothing");
    LyingNamer liar;
    Check(suite, Row(Dec(user64, kLe, 8, &liar), "ptr").text == "0x00007FF612400000", "namer returning false adds nothing even if it wrote out");
    // 大端 8 字节、内核地址。
    Check(suite, Row(Dec(B({ 0, 0, 0x7F, 0xF6, 0x12, 0x40, 0, 0 }), kBe), "ptr").text == "0x00007FF612400000", "ptr 8 BE");
    Check(suite, Row(Dec(B({ 0x00, 0x10, 0x00, 0x00, 0x80, 0xF8, 0xFF, 0xFF })), "ptr").text == "0xFFFFF88000001000", "ptr kernel address");
    // 32 位宽度只读前 4 个字节，尾随字节不参与：地址是 0x401000 而不是 8 字节读法。
    FakeNamer narrow;
    narrow.names[0x401000ULL] = "game.exe+0x1000";
    const DecodedRow w4 = Row(Dec(B({ 0x00, 0x10, 0x40, 0x00, 0xAA, 0xBB, 0xCC, 0xDD }), kLe, 4, &narrow), "ptr");
    Check(suite, w4.text == "0x00401000 (game.exe+0x1000)" && w4.copyText == "0x00401000", "ptr 4 LE reads only 4 bytes: " + w4.text);
    Check(suite, narrow.calls == std::vector<std::uint64_t>{ 0x401000ULL }, "namer sees the 4-byte address");
    Check(suite, Row(Dec(B({ 0x00, 0x40, 0x10, 0x00 }), kBe, 4), "ptr").text == "0x00401000", "ptr 4 BE");
    // 32 位指针零扩展，绝不符号扩展。
    FakeNamer high;
    Check(suite, Row(Dec(Rep(0xFF, 4), kLe, 4, &high), "ptr").text == "0xFFFFFFFF" && high.calls == std::vector<std::uint64_t>{ 0xFFFFFFFFULL }, "ptr 4 is zero-extended, never sign-extended");
    // 字节不足不碰 namer；宽度非法不可用且不碰 namer。
    FakeNamer idle;
    Check(suite, !Row(Dec(Rep(0x11, 7), kLe, 8, &idle), "ptr").available && !Row(Dec(Rep(0x11, 3), kLe, 4, &idle), "ptr").available, "ptr 8 needs 8 bytes, ptr 4 needs 4");
    for (const std::uint32_t width : { 0u, 1u, 2u, 3u, 5u, 16u }) {
        const DecodedRow row = Row(Dec(Rep(0x11, 32), kLe, width, &idle), "ptr");
        Check(suite, !row.available && row.text.empty(), "ptr with illegal width " + std::to_string(width) + " is unavailable");
    }
    Check(suite, idle.calls.empty(), "namer is never called for unavailable pointer rows");
}

// 十、字节不足：逐长度 0..16，两种字节序。
void TestShortInputs(Suite& suite) {
    static const NamedSize kNeeds[] = {
        { "i8", 1 }, { "u8", 1 }, { "i16", 2 }, { "u16", 2 }, { "i32", 4 }, { "u32", 4 }, { "i64", 8 }, { "u64", 8 },
        { "f32", 4 }, { "f64", 8 }, { "ptr", 8 }, { "filetime", 8 }, { "time_t32", 4 }, { "time_t64", 8 },
        { "guid", 16 }, { "ascii", 1 }, { "utf16", 2 },
    };
    for (const ByteOrder order : { kLe, kBe }) {
        for (std::size_t length = 0; length <= 16; ++length) {
            // 精确长度的向量：多读一个字节就是越界，配合 -fsanitize 能抓到。
            const Rows rows = Dec(Rep(0x41, length), order);
            for (const NamedSize& need : kNeeds) {
                const DecodedRow row = Row(rows, need.name);
                const std::string name = std::string(need.name) + " at length " + std::to_string(length);
                Check(suite, row.available == (length >= need.size), name + " availability");
                Check(suite, row.available || (row.text.empty() && row.copyText.empty() && row.valid), name + " unavailable row is empty and valid");
            }
        }
    }
    // 指针宽度 4 的需求是 4 字节。
    for (std::size_t length = 0; length <= 8; ++length) {
        Check(suite, Row(Dec(Rep(0x41, length), kLe, 4), "ptr").available == (length >= 4), "ptr 4 at length " + std::to_string(length));
    }
}

// EncodeCase：一条编码成功的期望（类型、文本、字节序、ptr 宽度、期望字节）。
struct EncodeCase {
    const char* type;
    const char* text;
    ByteOrder order;
    std::uint32_t width;
    Bytes bytes;
};

// 十一、Encode 的成功路径：每种类型、两种字节序、前缀与空白。
void TestEncodeSuccess(Suite& suite) {
    const std::vector<EncodeCase> cases = {
        { "i8", "-1", kLe, 8, B({ 0xFF }) }, { "i8", "127", kLe, 8, B({ 0x7F }) }, { "i8", "-128", kLe, 8, B({ 0x80 }) },
        { "i8", "0", kLe, 8, B({ 0 }) }, { "i8", "-0", kLe, 8, B({ 0 }) }, { "i8", "0x7F", kLe, 8, B({ 0x7F }) },
        { "i8", "-0x80", kLe, 8, B({ 0x80 }) }, { "i8", "  12 ", kLe, 8, B({ 0x0C }) }, { "i8", "\t-5\r\n", kLe, 8, B({ 0xFB }) },
        { "u8", "255", kLe, 8, B({ 0xFF }) }, { "u8", "0xFF", kLe, 8, B({ 0xFF }) }, { "u8", "0XfF", kLe, 8, B({ 0xFF }) },
        { "u8", "0", kLe, 8, B({ 0 }) }, { "u8", "-0", kLe, 8, B({ 0 }) }, { "u8", "007", kLe, 8, B({ 7 }) }, { "u8", "0x007", kLe, 8, B({ 7 }) },
        { "i16", "-2", kLe, 8, B({ 0xFE, 0xFF }) }, { "i16", "-2", kBe, 8, B({ 0xFF, 0xFE }) },
        { "i16", "32767", kLe, 8, B({ 0xFF, 0x7F }) }, { "i16", "-32768", kLe, 8, B({ 0x00, 0x80 }) },
        { "u16", "65535", kLe, 8, B({ 0xFF, 0xFF }) }, { "u16", "0x1234", kLe, 8, B({ 0x34, 0x12 }) }, { "u16", "0x1234", kBe, 8, B({ 0x12, 0x34 }) },
        { "i32", "-1", kLe, 8, Rep(0xFF, 4) }, { "i32", "2147483647", kLe, 8, B({ 0xFF, 0xFF, 0xFF, 0x7F }) },
        { "i32", "-2147483648", kLe, 8, B({ 0, 0, 0, 0x80 }) }, { "i32", "0x12345678", kLe, 8, B({ 0x78, 0x56, 0x34, 0x12 }) },
        { "i32", "0x12345678", kBe, 8, B({ 0x12, 0x34, 0x56, 0x78 }) }, { "u32", "4294967295", kLe, 8, Rep(0xFF, 4) },
        { "u32", "305419896", kLe, 8, B({ 0x78, 0x56, 0x34, 0x12 }) }, { "u32", "305419896", kBe, 8, B({ 0x12, 0x34, 0x56, 0x78 }) },
        { "i64", "-1", kLe, 8, Rep(0xFF, 8) }, { "i64", "9223372036854775807", kLe, 8, B({ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F }) },
        { "i64", "-9223372036854775808", kLe, 8, B({ 0, 0, 0, 0, 0, 0, 0, 0x80 }) },
        { "u64", "18446744073709551615", kLe, 8, Rep(0xFF, 8) },
        { "u64", "0x0123456789ABCDEF", kLe, 8, B({ 0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01 }) },
        { "u64", "0x0123456789ABCDEF", kBe, 8, B({ 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF }) },
        { "f32", "1", kLe, 8, B({ 0x00, 0x00, 0x80, 0x3F }) }, { "f32", "1", kBe, 8, B({ 0x3F, 0x80, 0x00, 0x00 }) },
        { "f32", "-2.5", kLe, 8, B({ 0x00, 0x00, 0x20, 0xC0 }) }, { "f32", "0.1", kLe, 8, B({ 0xCD, 0xCC, 0xCC, 0x3D }) },
        { "f32", "1e0", kLe, 8, B({ 0x00, 0x00, 0x80, 0x3F }) }, { "f32", "1E2", kLe, 8, B({ 0x00, 0x00, 0xC8, 0x42 }) },
        { "f32", ".5", kLe, 8, B({ 0x00, 0x00, 0x00, 0x3F }) }, { "f32", "5.", kLe, 8, B({ 0x00, 0x00, 0xA0, 0x40 }) },
        { "f32", "-0", kLe, 8, B({ 0, 0, 0, 0x80 }) }, { "f32", "0", kLe, 8, Rep(0, 4) }, { "f32", "0.000e10", kLe, 8, Rep(0, 4) },
        { "f32", "NaN", kLe, 8, B({ 0x00, 0x00, 0xC0, 0x7F }) }, { "f32", "-nan", kLe, 8, B({ 0x00, 0x00, 0xC0, 0xFF }) },
        { "f32", "Inf", kLe, 8, B({ 0x00, 0x00, 0x80, 0x7F }) }, { "f32", "-inf", kLe, 8, B({ 0x00, 0x00, 0x80, 0xFF }) },
        { "f32", "INFINITY", kLe, 8, B({ 0x00, 0x00, 0x80, 0x7F }) }, { "f32", "3.4028235e38", kLe, 8, B({ 0xFF, 0xFF, 0x7F, 0x7F }) },
        { "f32", "1e-45", kLe, 8, B({ 0x01, 0, 0, 0 }) }, { "u64", "0000000000000000000000000007", kLe, 8, B({ 7, 0, 0, 0, 0, 0, 0, 0 }) },
        { "f64", "1", kLe, 8, B({ 0, 0, 0, 0, 0, 0, 0xF0, 0x3F }) }, { "f64", "1", kBe, 8, B({ 0x3F, 0xF0, 0, 0, 0, 0, 0, 0 }) },
        { "f64", "0.1", kLe, 8, B({ 0x9A, 0x99, 0x99, 0x99, 0x99, 0x99, 0xB9, 0x3F }) },
        { "f64", "-2.5", kLe, 8, B({ 0, 0, 0, 0, 0, 0, 0x04, 0xC0 }) }, { "f64", "NaN", kLe, 8, B({ 0, 0, 0, 0, 0, 0, 0xF8, 0x7F }) },
        { "f64", "Inf", kLe, 8, B({ 0, 0, 0, 0, 0, 0, 0xF0, 0x7F }) }, { "f64", "-Inf", kLe, 8, B({ 0, 0, 0, 0, 0, 0, 0xF0, 0xFF }) },
        { "f64", "1.7976931348623157e308", kLe, 8, B({ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xEF, 0x7F }) }, { "f64", "5e-324", kLe, 8, B({ 1, 0, 0, 0, 0, 0, 0, 0 }) },
        // ptr 按给定宽度编码；无前缀按十进制，与整数一致。
        { "ptr", "0x7FF612400000", kLe, 8, B({ 0x00, 0x00, 0x40, 0x12, 0xF6, 0x7F, 0x00, 0x00 }) },
        { "ptr", "0x7FF612400000", kBe, 8, B({ 0x00, 0x00, 0x7F, 0xF6, 0x12, 0x40, 0x00, 0x00 }) },
        { "ptr", "0xFFFFF88000001000", kLe, 8, B({ 0x00, 0x10, 0x00, 0x00, 0x80, 0xF8, 0xFF, 0xFF }) },
        { "ptr", "0", kLe, 8, Rep(0, 8) }, { "ptr", "-0", kLe, 4, Rep(0, 4) }, { "ptr", "4096", kLe, 4, B({ 0x00, 0x10, 0x00, 0x00 }) },
        { "ptr", "0x401000", kLe, 4, B({ 0x00, 0x10, 0x40, 0x00 }) }, { "ptr", "0x401000", kBe, 4, B({ 0x00, 0x40, 0x10, 0x00 }) },
        { "ptr", "4294967295", kLe, 4, Rep(0xFF, 4) }, { "ptr", "18446744073709551615", kLe, 8, Rep(0xFF, 8) },
    };
    for (const EncodeCase& item : cases) {
        // 预置残余：成功必须整体替换，不是追加。
        Bytes out = Rep(9, 10);
        const EncodeStatus status = EncodeValue(item.type, item.text, item.order, item.width, out);
        const std::string name = std::string("encode ") + item.type + " [" + item.text + "]" + (item.order == kBe ? " BE" : " LE");
        Check(suite, status == EncodeStatus::Ok, name + " succeeds");
        Check(suite, out == item.bytes, name + " produces the exact bytes");
    }
}

// ExpectFail：失败必须返回指定状态码，且 bytesOut 被清空（预置残余证明不是碰巧为空）。
void ExpectFail(Suite& suite, const std::string& type, const std::string& text, EncodeStatus want, std::uint32_t width = 8) {
    Bytes out = B({ 1, 2, 3 });
    const EncodeStatus got = EncodeValue(type, text, kLe, width, out);
    const std::string name = "encode " + type + " [" + text + "] width " + std::to_string(width);
    Check(suite, got == want, name + " status " + std::to_string(static_cast<int>(got)) + " want " + std::to_string(static_cast<int>(want)));
    Check(suite, out.empty(), name + " leaves bytesOut empty");
}

// 十二、Encode 的拒绝路径：五种状态码各自显式测，边界两侧都测。
void TestEncodeRejections(Suite& suite) {
    // 未知类型：先于文本检查（坏文本也先报它）；filetime/guid/字符串类不支持编码。
    for (const char* type : { "", "i7", "I8", "u128", "f16", "filetime", "guid", "ascii", "utf16", "float", " i8", "i8 ", "int8", "time_t64" }) {
        ExpectFail(suite, type, "1", EncodeStatus::UnknownType);
    }
    ExpectFail(suite, "zz", "###", EncodeStatus::UnknownType);
    for (const std::uint32_t width : { 0u, 1u, 2u, 3u, 5u, 16u }) {
        ExpectFail(suite, "ptr", "1", EncodeStatus::UnknownType, width);
    }
    // 语法错误：整数与浮点各一份清单，对每个相关类型都跑。
    const std::string embeddedNul("1\0" "2", 3);
    const std::vector<std::string> badIntegers = {
        "", "   ", "-", "+1", "--1", "- 1", "1 2", "0x", "0X", "-0x", "0xg", "0x-1", "12a", "1.5", "1e3", "1_000", "1,000",
        "abc", "0x 12", "0b101", "1h", "\xEF\xBC\x91", embeddedNul, "99999999999999999999x",
    };
    for (const char* type : { "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "ptr" }) {
        for (const std::string& text : badIntegers) {
            ExpectFail(suite, type, text, EncodeStatus::BadNumber);
        }
    }
    const std::vector<std::string> badFloats = {
        "", " ", "-", "+1.5", "1.5x", "e5", "1e", "1e+", ".", "-.", "0x1p3", "1,5", "nan(abc)", "infinit", "in", "n",
        "--1", "1.2.3", "- 1", "inf1", "1 2", "\xEF\xBC\x91",
    };
    for (const char* type : { "f32", "f64" }) {
        for (const std::string& text : badFloats) {
            ExpectFail(suite, type, text, EncodeStatus::BadNumber);
        }
    }
    // Overflow：数值本身放不进 64 位，先于类型范围检查。
    for (const char* type : { "u64", "i64", "u8", "i8", "u32", "ptr" }) {
        for (const char* text : { "18446744073709551616", "0x10000000000000000", "-18446744073709551616", "99999999999999999999999" }) {
            ExpectFail(suite, type, text, EncodeStatus::Overflow);
        }
    }
    ExpectFail(suite, "ptr", "0x10000000000000000", EncodeStatus::Overflow, 4);
    // OutOfRange：语法合法、64 位内，但超出该类型；每个边界的另一侧在成功表里。
    const TypeText ranges[] = {
        { "i8", "128", 8 }, { "i8", "-129", 8 }, { "i8", "255", 8 }, { "i8", "0xFF", 8 }, { "i8", "256", 8 }, { "i8", "-0x81", 8 },
        { "i8", "1000", 8 }, { "u8", "256", 8 }, { "u8", "-1", 8 }, { "u8", "-0x1", 8 }, { "u8", "0x100", 8 },
        { "i16", "32768", 8 }, { "i16", "-32769", 8 }, { "i16", "65535", 8 }, { "u16", "65536", 8 }, { "u16", "-1", 8 },
        { "i32", "2147483648", 8 }, { "i32", "-2147483649", 8 }, { "i32", "4294967295", 8 }, { "u32", "4294967296", 8 },
        { "u32", "-1", 8 }, { "i64", "9223372036854775808", 8 }, { "i64", "-9223372036854775809", 8 },
        { "i64", "18446744073709551615", 8 }, { "u64", "-1", 8 }, { "u64", "-18446744073709551615", 8 },
        { "ptr", "0x100000000", 4 }, { "ptr", "4294967296", 4 }, { "ptr", "-1", 4 }, { "ptr", "-1", 8 },
        // 浮点：上溢、负上溢、以及非零数下溢成 0 都拒绝，不静默写成 0 或无穷。
        { "f32", "3.5e38", 8 }, { "f32", "-3.5e38", 8 }, { "f32", "1e39", 8 }, { "f32", "1e-50", 8 }, { "f32", "-1e-50", 8 },
        { "f32", "0.00000000000000000000000000000000000000000000001", 8 },
        { "f64", "1e999", 8 }, { "f64", "-1e999", 8 }, { "f64", "2e308", 8 }, { "f64", "1e-999", 8 },
    };
    for (const TypeText& item : ranges) {
        ExpectFail(suite, item.type, item.text, EncodeStatus::OutOfRange, item.width);
    }
}

// 十三、往返：Decode(Encode(x)) == x，Encode(Decode(b)) == b。
void TestRoundTrips(Suite& suite) {
    // 文本 -> 字节 -> 文本：规范十进制文本原样回来。
    const TypeText texts[] = {
        { "i8", "-128" }, { "i8", "-1" }, { "i8", "0" }, { "i8", "127" }, { "u8", "0" }, { "u8", "255" },
        { "i16", "-32768" }, { "i16", "32767" }, { "u16", "65535" }, { "i32", "-2147483648" }, { "i32", "2147483647" },
        { "u32", "4294967295" }, { "i64", "-9223372036854775808" }, { "i64", "9223372036854775807" },
        { "u64", "18446744073709551615" }, { "u64", "17279655951921914625" }, { "f32", "0.1" }, { "f32", "-2.5" },
        { "f32", "3.1415927" }, { "f64", "0.1" }, { "f64", "3.141592653589793" }, { "f64", "-0" }, { "f32", "-Inf" },
        { "f64", "Inf" }, { "f64", "1.7976931348623157e+308" }, { "f64", "5e-324" },
    };
    for (const ByteOrder order : { kLe, kBe }) {
        for (const TypeText& item : texts) {
            Bytes bytes;
            const EncodeStatus status = EncodeValue(item.type, item.text, order, 8, bytes);
            const std::string copy = Row(Dec(bytes, order), item.type).copyText;
            Check(suite, status == EncodeStatus::Ok && copy == item.text, std::string("round trip text ") + item.type + " [" + item.text + "] got [" + copy + "]");
        }
    }
    // 字节 -> 文本 -> 字节：整数宽度与指针宽度，用一批有代表性的位模式，每种字节序。
    const std::uint64_t patterns[] = { 0xEFCDAB8967452301ULL, ~0ULL, 0ULL, 0x8000000000000000ULL, 0x7FFFFFFFFFFFFFFFULL, 1ULL, 0x80ULL };
    const NamedSize widths[] = { { "i8", 1 }, { "u8", 1 }, { "i16", 2 }, { "u16", 2 }, { "i32", 4 }, { "u32", 4 }, { "i64", 8 }, { "u64", 8 } };
    for (const ByteOrder order : { kLe, kBe }) {
        for (const std::uint64_t pattern : patterns) {
            for (const NamedSize& width : widths) {
                const Bytes original = Order(order, pattern, width.size);
                const std::string copy = Row(Dec(original, order), width.name).copyText;
                Bytes encoded;
                Check(suite, EncodeValue(width.name, copy, order, 8, encoded) == EncodeStatus::Ok && encoded == original,
                    std::string("round trip bytes ") + width.name + " [" + copy + "]");
            }
            for (const std::uint32_t pointerWidth : { 4u, 8u }) {
                const Bytes original = Order(order, pattern, pointerWidth);
                const std::string copy = Row(Dec(original, order, pointerWidth), "ptr").copyText;
                Bytes encoded;
                Check(suite, EncodeValue("ptr", copy, order, pointerWidth, encoded) == EncodeStatus::Ok && encoded == original,
                    "round trip ptr width " + std::to_string(pointerWidth) + " [" + copy + "]");
            }
        }
    }
    // 浮点位模式：有限数、零、无穷必须逐位回来；NaN 只要求仍显示 NaN（载荷不保留）。
    const std::vector<std::uint64_t> bits32 = { 0x3F800000, 0x3DCCCCCD, 0x7F7FFFFF, 1, 0x007FFFFF, 0x00800000, 0x80000000, 0x7F800000, 0xFF800000 };
    const std::vector<std::uint64_t> bits64 = { 0x3FF0000000000000ULL, 0x3FB999999999999AULL, 0x7FEFFFFFFFFFFFFFULL, 1, 0x8000000000000000ULL, 0xFFF0000000000000ULL, 0x000FFFFFFFFFFFFFULL };
    for (const ByteOrder order : { kLe, kBe }) {
        for (const bool wide : { false, true }) {
            for (const std::uint64_t bits : wide ? bits64 : bits32) {
                const Bytes original = Order(order, bits, wide ? 8 : 4);
                const std::string copy = Row(Dec(original, order), wide ? "f64" : "f32").copyText;
                Bytes encoded;
                Check(suite, EncodeValue(wide ? "f64" : "f32", copy, order, 8, encoded) == EncodeStatus::Ok && encoded == original, "round trip float [" + copy + "]");
            }
            Bytes nan;
            Check(suite, EncodeValue(wide ? "f64" : "f32", "-NaN", order, 8, nan) == EncodeStatus::Ok
                && Row(Dec(nan, order), wide ? "f64" : "f32").text == "NaN", "NaN survives a float round trip as NaN");
        }
    }
}

} // namespace

int RunMemwbValueDecodeTests() {
    KswordTests::Suite suite(L"MEMWB value decode");
    TestLayout(suite);
    TestIntegers(suite);
    TestFloats(suite);
    TestFiletime(suite);
    TestUnixTime(suite);
    TestCalendarSweep(suite);
    TestGuid(suite);
    TestStrings(suite);
    TestPointer(suite);
    TestShortInputs(suite);
    TestEncodeSuccess(suite);
    TestEncodeRejections(suite);
    TestRoundTrips(suite);
    suite.report();
    return suite.failures();
}
