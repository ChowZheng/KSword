// 内存工作台字节查找引擎（shared/evidence/memory_workbench/MemoryByteSearch.h）的离线测试。
//
// 查找属于"漏报不会报错"的一类：跨块边界漏一个命中、把不可读间隙当成 00 匹配、Next 到尾
// 悄悄回绕，界面上都只表现为"没找到"。旧控件的真实缺陷逐条钉在下面。断言原则：
//   * 期望值手算写死，绝不从被测函数反算；
//   * 核心不变式用对拍：分块大小逐值扫描，结果必须与独立的暴力参考实现完全一致
//     （found/address/wrapped 与 scanned/skipped 统计都比）；
//   * 数据源在不可读的洞里保留"看起来会命中"的真实字节，无视有效位的实现会立刻露馅；
//   * 每类拒绝路径都核对错误码与位置；性能只打印数字、不断言耗时。

#include "TestSupport.h"

#include "../shared/evidence/memory_workbench/MemoryByteSearch.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace ksword::memwb;
using Bytes = std::vector<std::uint8_t>;
using U64 = std::uint64_t;

// kMax：64 位地址上限；kDefaultChunk：默认块大小；g_suite：装置自身出错时登记断言用，入口设置。
constexpr U64 kMax = 0xFFFFFFFFFFFFFFFFULL;
constexpr U64 kDefaultChunk = kDefaultSearchChunkBytes;
KswordTests::Suite* g_suite = nullptr;

// ------------------------------------------------------------
// 小工具
// ------------------------------------------------------------
// B：整数列表转字节串，省去到处写 static_cast。
Bytes B(std::initializer_list<int> values) {
    Bytes result;
    for (const int value : values) {
        result.push_back(static_cast<std::uint8_t>(value));
    }
    return result;
}

// Hex/Txt：解析模式；解析失败登记一次断言失败（装置错误）。
SearchPattern Hex(const char* text) {
    SearchPattern pattern;
    ParseError error;
    g_suite->expect(ParseHexPattern(text, pattern, error), L"test setup: hex pattern must parse");
    return pattern;
}

SearchPattern Txt(const char* text, TextEncoding encoding = TextEncoding::Utf8, bool ignoreCase = false) {
    SearchPattern pattern;
    ParseError error;
    g_suite->expect(ParseTextPattern(text, encoding, ignoreCase, pattern, error), L"test setup: text pattern must parse");
    return pattern;
}

// Fwd/Bwd：Find 的方向简写（默认不回绕、默认块大小、无取消标志）。
SearchResult Fwd(IByteSource& source, const SearchPattern& pattern, SearchRange range, U64 start, bool wrap = false, U64 chunk = kDefaultChunk) {
    return Find(source, pattern, range, start, SearchDirection::Forward, wrap, nullptr, chunk);
}

SearchResult Bwd(IByteSource& source, const SearchPattern& pattern, SearchRange range, U64 start, bool wrap = false, U64 chunk = kDefaultChunk) {
    return Find(source, pattern, range, start, SearchDirection::Backward, wrap, nullptr, chunk);
}

// ExpectFind：一次断言核对 Find 的全部字段；不一致时把实际值打出来便于定位。
void ExpectFind(KswordTests::Suite& suite, const wchar_t* label, const SearchResult& got, bool found, U64 address, bool wrapped, U64 scanned, U64 skipped) {
    const bool ok = got.found == found && got.address == (found ? address : 0) && got.wrapped == wrapped
        && got.scannedBytes == scanned && got.skippedUnreadableBytes == skipped && !got.cancelled && !got.invalidArguments;
    suite.expect(ok, label);
    if (!ok) {
        std::wcerr << L"    actual: found=" << got.found << L" address=0x" << std::hex << got.address << std::dec << L" wrapped=" << got.wrapped << L" scanned=" << got.scannedBytes << L" skipped=" << got.skippedUnreadableBytes << L'\n';
    }
}

// Put：把 value 写进 data 的 at 偏移处，用来在零背景里摆放标记。
void Put(Bytes& data, std::size_t at, const Bytes& value) {
    for (std::size_t i = 0; i < value.size(); ++i) {
        data[at + i] = value[i];
    }
}

// CancelAfter：返回读取钩子，在第 n 次读取之后置取消标志。
std::function<void(int)> CancelAfter(std::atomic<bool>& flag, int n) {
    return [&flag, n](int reads) {
        if (reads == n) {
            flag.store(true);
        }
    };
}

// ------------------------------------------------------------
// 测试数据源：单块内存 + 任意个不可读的洞 + 读取钩子 + 请求契约检查。
// 洞里保留真实字节（不清零），无视有效位的实现会在洞里命中。
// ------------------------------------------------------------
struct GapSource final : IByteSource {
    GapSource(U64 baseAddress, Bytes content) : base(baseAddress), data(std::move(content)) {}
    // base/data：数据块起始地址与内容；holes：不可读的绝对地址闭区间。
    U64 base;
    Bytes data;
    std::vector<std::pair<U64, U64>> holes;
    // afterRead：每次读取完成后调用，参数是累计读取次数。
    std::function<void(int)> afterRead;
    // allowed：引擎只许请求这个范围里的地址；越界、零长度、区间溢出都记为违约。
    SearchRange allowed{ 0, kMax };
    bool contractViolated = false;
    int reads = 0;
    // Readable：该地址是否真实可读（独立的暴力判据，参考实现也用它）。
    bool Readable(U64 address) const {
        if (address < base || address - base >= data.size()) {
            return false;
        }
        for (const auto& hole : holes) {
            if (address >= hole.first && address <= hole.second) {
                return false;
            }
        }
        return true;
    }

    ReadStatus Read(U64 address, U64 length, Bytes& bytesOut, Bytes& validOut) override {
        ++reads;
        const U64 lastAddress = address + (length - 1);
        if (length == 0 || lastAddress < address || address < allowed.first || lastAddress > allowed.last) {
            contractViolated = true;
        }
        bytesOut.assign(static_cast<std::size_t>(length), 0);
        validOut.assign(static_cast<std::size_t>(length), 0);
        U64 validCount = 0;
        for (U64 i = 0; i < length; ++i) {
            const U64 at = address + i;
            if (at >= base && at - base < data.size()) {
                bytesOut[static_cast<std::size_t>(i)] = data[static_cast<std::size_t>(at - base)];
            }
            if (Readable(at)) {
                validOut[static_cast<std::size_t>(i)] = 1;
                ++validCount;
            }
        }
        if (afterRead) {
            afterRead(reads);
        }
        return validCount == length ? ReadStatus::Ok : (validCount == 0 ? ReadStatus::Unreadable : ReadStatus::Partial);
    }
};

// ------------------------------------------------------------
// 暴力参考实现：列出全部候选起点，再按"方向 + 两组"的朴素定义逐个检查。
// 与被测代码的区间算术写法完全不同，只共用 Readable。
// ------------------------------------------------------------
struct RefResult {
    bool found = false;
    U64 address = 0;
    bool wrapped = false;
    U64 scanned = 0;
    U64 skipped = 0;
};

bool RefMatch(const GapSource& mem, const SearchPattern& pattern, U64 start) {
    for (std::size_t i = 0; i < pattern.bytes.size(); ++i) {
        const U64 at = start + i;
        if (!mem.Readable(at)) {
            return false;
        }
        if ((mem.data[static_cast<std::size_t>(at - mem.base)] & pattern.mask[i]) != (pattern.bytes[i] & pattern.mask[i])) {
            return false;
        }
    }
    return true;
}

// RefCandidates：范围内全部合法起点（升序）；放不下模式则为空。
std::vector<U64> RefCandidates(const SearchPattern& pattern, const SearchRange& range) {
    std::vector<U64> candidates;
    const U64 tail = pattern.bytes.size() - 1;
    if (range.last - range.first >= tail) {
        for (U64 k = 0; k <= range.last - tail - range.first; ++k) {
            candidates.push_back(range.first + k);
        }
    }
    return candidates;
}

RefResult RefFind(const GapSource& mem, const SearchPattern& pattern, const SearchRange& range, U64 start,
                  SearchDirection direction, bool wrap) {
    // 两组：第一组是"从起点往搜索方向"的候选，第二组是起点另一侧（仅 wrap 时才扫）。
    std::vector<U64> groups[2];
    for (const U64 candidate : RefCandidates(pattern, range)) {
        const bool inFirst = (direction == SearchDirection::Forward) ? (candidate >= start) : (candidate <= start);
        groups[inFirst ? 0 : 1].push_back(candidate);
    }
    RefResult result;
    for (int g = 0; g < 2 && !result.found; ++g) {
        if (g == 1 && (!wrap || groups[1].empty())) {
            break;
        }
        result.wrapped = (g == 1);
        std::vector<U64> order = groups[g];
        if (direction == SearchDirection::Backward) {
            order.assign(groups[g].rbegin(), groups[g].rend());
        }
        for (const U64 candidate : order) {
            (mem.Readable(candidate) ? result.scanned : result.skipped) += 1;
            if (RefMatch(mem, pattern, candidate)) {
                result.found = true;
                result.address = candidate;
                break;
            }
        }
    }
    return result;
}

// ------------------------------------------------------------
// 一、十六进制解析：接受路径（旧缺陷：连写 4D5A9000 被判无效）
// ------------------------------------------------------------
void TestHexAccepted(KswordTests::Suite& suite) {
    struct Case { const char* text; Bytes bytes; Bytes mask; const char* description; };
    const Case cases[] = {
        { "4D5A9000", B({0x4D, 0x5A, 0x90, 0x00}), B({0xFF, 0xFF, 0xFF, 0xFF}), "4D 5A 90 00" },
        { "4D 5A 90 00", B({0x4D, 0x5A, 0x90, 0x00}), B({0xFF, 0xFF, 0xFF, 0xFF}), "4D 5A 90 00" },
        { "4d,5a,90,00", B({0x4D, 0x5A, 0x90, 0x00}), B({0xFF, 0xFF, 0xFF, 0xFF}), "4D 5A 90 00" },
        { "0x4D5A9000", B({0x4D, 0x5A, 0x90, 0x00}), B({0xFF, 0xFF, 0xFF, 0xFF}), "4D 5A 90 00" },
        { "0x4D 0X5a,0x90\t00\n\r\v\f", B({0x4D, 0x5A, 0x90, 0x00}), B({0xFF, 0xFF, 0xFF, 0xFF}), "4D 5A 90 00" },
        { "0x4D5A 90", B({0x4D, 0x5A, 0x90}), B({0xFF, 0xFF, 0xFF}), "4D 5A 90" },
        { "  ,, 4D  ,5A,, ", B({0x4D, 0x5A}), B({0xFF, 0xFF}), "4D 5A" },
        // 通配：?? 全通配，A? 只比高半字节，?B 只比低半字节；通配位的 bytes 一律置 0。
        { "?? 4D", B({0x00, 0x4D}), B({0x00, 0xFF}), "?? 4D" },
        { "A? ?B", B({0xA0, 0x0B}), B({0xF0, 0x0F}), "A? ?B" },
        { "4D??9?", B({0x4D, 0x00, 0x90}), B({0xFF, 0x00, 0xF0}), "4D ?? 9?" },
        { "a?bf ?F FF", B({0xA0, 0xBF, 0x0F, 0xFF}), B({0xF0, 0xFF, 0x0F, 0xFF}), "A? BF ?F FF" },
        { "0x??", B({0x00}), B({0x00}), "??" },
    };
    for (const Case& c : cases) {
        SearchPattern pattern;
        ParseError error;
        error.position = 99;
        const bool ok = ParseHexPattern(c.text, pattern, error);
        suite.expect(ok && error.code == ParseErrorCode::None && error.position == 0, L"hex parse: accepted input reports success and clears the error");
        suite.expect(pattern.bytes == c.bytes, L"hex parse: bytes match the hand-computed value");
        suite.expect(pattern.mask == c.mask, L"hex parse: mask matches the hand-computed value");
        suite.expect(pattern.description == c.description, L"hex parse: description is the normalized byte text");
    }
}

// ------------------------------------------------------------
// 二、十六进制解析：每类拒绝路径与位置（位置都是手数的原始输入字节偏移）
// ------------------------------------------------------------
void TestHexRejected(KswordTests::Suite& suite) {
    struct Case { std::string text; ParseErrorCode code; std::size_t position; };
    const ParseErrorCode empty = ParseErrorCode::Empty;
    const ParseErrorCode bad = ParseErrorCode::BadPattern;
    const Case cases[] = {
        { "", empty, 0 }, { "   \t\r\n", empty, 0 }, { ",, ,", empty, 0 },
        // 奇数个半字节：位置是落单的最后一个半字节。
        { "4D5", bad, 2 }, { "4D5A9", bad, 4 }, { "4", bad, 0 }, { "?", bad, 0 }, { "???", bad, 2 },
        { "4D 5", bad, 3 }, { "4D 5A 9", bad, 6 }, { "?? ?", bad, 3 }, { "0x4D5", bad, 4 },
        // 一个字节不许拆到两个记号里。
        { "4 D", bad, 0 }, { "4D5 5A", bad, 2 },
        // 非法字符：位置是该字符；同一输入多处错误时先扫到的先报。
        { "G0", bad, 0 }, { "4G", bad, 1 }, { "4D 5Z", bad, 4 }, { "4D-5A", bad, 2 }, { "4D;5A", bad, 2 },
        { "x4D", bad, 0 }, { "4D0x5A", bad, 3 }, { "4D5x", bad, 3 }, { "0xG", bad, 2 }, { "0x0x", bad, 3 },
        // 孤立的 0x 前缀：位置是前缀的 '0'。
        { "0x", bad, 0 }, { "0x 4D", bad, 0 }, { "4D 0x", bad, 3 }, { "4D 0x,5A", bad, 3 },
        // 非 ASCII 与控制字符：NBSP、全角逗号、BOM、内嵌 NUL。
        { "4D\xC2\xA0" "5A", bad, 2 }, { "4D\xEF\xBC\x8C" "5A", bad, 2 }, { "\xEF\xBB\xBF" "4D", bad, 0 },
        { std::string("4D\0" "5A", 5), bad, 2 },
    };
    for (const Case& c : cases) {
        // 输出参数预填残留值：失败后必须被清空，调用方忘了看返回值也不会拿到旧结果。
        SearchPattern pattern;
        pattern.bytes = B({1});
        pattern.mask = B({1});
        pattern.description = "stale";
        ParseError error;
        const bool ok = ParseHexPattern(c.text, pattern, error);
        const bool hit = !ok && error.code == c.code && error.position == c.position;
        suite.expect(hit, L"hex parse: rejected input reports the hand-counted code and position");
        suite.expect(pattern.bytes.empty() && pattern.mask.empty() && pattern.description.empty(),
            L"hex parse: a failed parse leaves the output pattern empty");
    }
}

// ------------------------------------------------------------
// 三、文本解析：编码、折叠、代理对（码点与 UTF-16 码元均手算）
// ------------------------------------------------------------
void TestTextAccepted(KswordTests::Suite& suite) {
    struct Case { std::string text; TextEncoding encoding; bool ignoreCase; Bytes bytes; Bytes mask; std::string description; };
    const TextEncoding u8 = TextEncoding::Utf8;
    const TextEncoding u16 = TextEncoding::Utf16Le;
    const std::string multiByte = "\xC2\x80\xC3\xA9\xDF\xBF\xE0\xA0\x80\xED\x9F\xBF\xEE\x80\x80\xE2\x82\xAC\xF0\x9F\x98\x80";
    const Case cases[] = {
        { "MZ", u8, false, B({0x4D, 0x5A}), B({0xFF, 0xFF}), "utf8:MZ" },
        // 忽略大小写：字母存大写、掩码 DF；数字不折叠。
        { "a1B", u8, true, B({0x41, 0x31, 0x42}), B({0xDF, 0xFF, 0xDF}), "utf8/i:a1B" },
        { "AZaz", u8, true, B({0x41, 0x5A, 0x41, 0x5A}), B({0xDF, 0xDF, 0xDF, 0xDF}), "utf8/i:AZaz" },
        // 字母两侧的符号 @ [ ` { 与字母只差 bit5 但不是字母，绝不能折叠。
        { "@[`{", u8, true, B({0x40, 0x5B, 0x60, 0x7B}), B({0xFF, 0xFF, 0xFF, 0xFF}), "utf8/i:@[`{" },
        // UTF-8 多字节，忽略大小写也全部精确；每种长度的最小/最大码点都在内：U+0080 U+00E9 U+07FF
        // U+0800 U+D7FF（代理区前一个）U+E000（代理区后一个）U+20AC U+1F600。
        { multiByte, u8, true, B({0xC2, 0x80, 0xC3, 0xA9, 0xDF, 0xBF, 0xE0, 0xA0, 0x80, 0xED, 0x9F, 0xBF, 0xEE, 0x80, 0x80, 0xE2, 0x82, 0xAC,
                                  0xF0, 0x9F, 0x98, 0x80}), Bytes(22, 0xFF), "utf8/i:" + multiByte },
        // UTF-16LE：每个码元先低字节后高字节。€ = U+20AC 不是 ASCII 字母，忽略大小写下仍精确。
        { "A", u16, false, B({0x41, 0x00}), B({0xFF, 0xFF}), "utf16le:A" },
        { "aZ", u16, true, B({0x41, 0x00, 0x5A, 0x00}), B({0xDF, 0xFF, 0xDF, 0xFF}), "utf16le/i:aZ" },
        { "\xE2\x82\xAC", u16, true, B({0xAC, 0x20}), B({0xFF, 0xFF}), "utf16le/i:\xE2\x82\xAC" },
        // Ł = U+0141：低字节 41 看着像 A，但高字节非零，不属于 ASCII，不得折叠。
        { "\xC5\x81", u16, true, B({0x41, 0x01}), B({0xFF, 0xFF}), "utf16le/i:\xC5\x81" },
        // 辅助平面走代理对。U+1F600：0x1F600-0x10000=0xF600，高 10 位 0x3D、低 10 位 0x200
        // -> D83D DE00 -> 3D D8 00 DE。U+1D49C：0xD49C，高 0x35、低 0x9C -> D835 DC9C -> 35 D8 9C DC。
        { "\xF0\x9F\x98\x80", u16, true, B({0x3D, 0xD8, 0x00, 0xDE}), Bytes(4, 0xFF), "utf16le/i:\xF0\x9F\x98\x80" },
        { "\xF0\x9D\x92\x9C", u16, false, B({0x35, 0xD8, 0x9C, 0xDC}), Bytes(4, 0xFF), "utf16le:\xF0\x9D\x92\x9C" },
        // 码点边界：U+FFFF 是最后一个单码元；U+10000 是 D800 DC00；U+10FFFF 是 DBFF DFFF。
        { "\xEF\xBF\xBF", u16, false, B({0xFF, 0xFF}), B({0xFF, 0xFF}), "utf16le:\xEF\xBF\xBF" },
        { "\xF0\x90\x80\x80", u16, false, B({0x00, 0xD8, 0x00, 0xDC}), Bytes(4, 0xFF), "utf16le:\xF0\x90\x80\x80" },
        { "\xF4\x8F\xBF\xBF", u16, false, B({0xFF, 0xDB, 0xFF, 0xDF}), Bytes(4, 0xFF), "utf16le:\xF4\x8F\xBF\xBF" },
    };
    for (const Case& c : cases) {
        SearchPattern pattern;
        ParseError error;
        const bool ok = ParseTextPattern(c.text, c.encoding, c.ignoreCase, pattern, error);
        suite.expect(ok && error.code == ParseErrorCode::None, L"text parse: accepted input reports success");
        suite.expect(pattern.bytes == c.bytes, L"text parse: bytes match the hand-computed encoding");
        suite.expect(pattern.mask == c.mask, L"text parse: mask matches (DF only on ASCII letters, high bytes exact)");
        suite.expect(pattern.description == c.description, L"text parse: description carries the encoding tag and the original text");
    }
}

void TestTextRejected(KswordTests::Suite& suite) {
    // 位置 = 出错码点的首字节偏移；两种目标编码共用同一套 UTF-8 校验。
    struct Case { std::string text; std::size_t position; };
    const Case cases[] = {
        { "ab\xFF", 2 }, { "a\x80", 1 }, { "a\xC3", 1 }, { "a\xC3\x28", 1 },
        { "\xC0\x80", 0 }, { "\xC1\xBF", 0 }, { "\xBF\xBF", 0 }, // 过长编码；首字节就是续字节
        { "\xE0\x80\x80", 0 }, { "\xF0\x80\x80\x80", 0 },    // 3/4 字节的过长编码
        { "\xED\xA0\x80", 0 }, { "\xED\xBF\xBF", 0 },         // U+D800 与 U+DFFF：代理区两端
        { "\xF4\x90\x80\x80", 0 }, { "\xF5\x80\x80\x80", 0 }, // 超过 U+10FFFF
        { "xy\xE2\x82", 2 },                                  // 末尾被截断的 3 字节序列
        { "\xC3\xA9\xFF", 2 },                                // 合法码点之后才出错
    };
    for (const TextEncoding encoding : { TextEncoding::Utf8, TextEncoding::Utf16Le }) {
        for (const Case& c : cases) {
            SearchPattern pattern;
            pattern.bytes = B({1});
            ParseError error;
            const bool ok = ParseTextPattern(c.text, encoding, false, pattern, error);
            suite.expect(!ok && error.code == ParseErrorCode::BadPattern && error.position == c.position,
                L"text parse: invalid UTF-8 is BadPattern at the first byte of the bad code point");
            suite.expect(pattern.bytes.empty() && pattern.mask.empty() && pattern.description.empty(),
                L"text parse: a failed parse leaves the output pattern empty");
        }
        SearchPattern pattern;
        ParseError error;
        suite.expect(!ParseTextPattern("", encoding, true, pattern, error) && error.code == ParseErrorCode::Empty
            && error.position == 0, L"text parse: empty text is Empty");
    }
}

// ------------------------------------------------------------
// 四、手算样例：命中、重叠、方向、回绕、统计口径
// ------------------------------------------------------------
void TestHandComputedFind(KswordTests::Suite& suite) {
    // 0x100 起 16 字节：下标 0:00 1:4D 2:5A 3:90 4:00 5:4D 6:5A 7:90 8:00 9-12:AA 13:00 14:4D 15:5A。
    // 模式 4D 5A 90 命中在下标 1、5；下标 14 的 4D 5A 后面没有 90。最大起点 0x10D，共 14 个候选。
    StaticByteSource source(0x100, B({0x00, 0x4D, 0x5A, 0x90, 0x00, 0x4D, 0x5A, 0x90, 0x00,
                                      0xAA, 0xAA, 0xAA, 0xAA, 0x00, 0x4D, 0x5A}));
    const SearchRange range{ 0x100, 0x10F };
    const SearchPattern mz = Hex("4D5A90");
    // Forward：起点含自身；统计只到命中位置为止。
    ExpectFind(suite, L"find fwd: from range start hits 0x101 after examining 2 starts", Fwd(source, mz, range, 0x100), true, 0x101, false, 2, 0);
    ExpectFind(suite, L"find fwd: start address is inclusive", Fwd(source, mz, range, 0x101), true, 0x101, false, 1, 0);
    ExpectFind(suite, L"find fwd: from 0x102 the next hit is 0x105", Fwd(source, mz, range, 0x102), true, 0x105, false, 4, 0);
    // 0x106..0x10D 共 8 个候选，没有命中；不回绕就是不回绕。
    ExpectFind(suite, L"find fwd: no wrap past the end reports not found and not wrapped", Fwd(source, mz, range, 0x106), false, 0, false, 8, 0);
    ExpectFind(suite, L"find fwd: wrap=true crosses the end, reports wrapped and finds 0x101", Fwd(source, mz, range, 0x106, true), true, 0x101, true, 10, 0);
    // Backward：起点含自身，起点上限被夹到最大起点 0x10D（0x10D..0x105 共 9 个）。
    ExpectFind(suite, L"find bwd: from the end finds 0x105", Bwd(source, mz, range, 0x10F), true, 0x105, false, 9, 0);
    ExpectFind(suite, L"find bwd: start address is inclusive", Bwd(source, mz, range, 0x105), true, 0x105, false, 1, 0);
    ExpectFind(suite, L"find bwd: from 0x104 the previous hit is 0x101", Bwd(source, mz, range, 0x104), true, 0x101, false, 4, 0);
    ExpectFind(suite, L"find bwd: nothing at or below 0x100 and no wrap", Bwd(source, mz, range, 0x100), false, 0, false, 1, 0);
    ExpectFind(suite, L"find bwd: wrap=true goes to the top and finds 0x105", Bwd(source, mz, range, 0x100, true), true, 0x105, true, 10, 0);
    // 回绕后整个范围扫完仍无命中：wrapped 为 true、found 为 false；90 90 的最大起点是 0x10E，共 15 个候选。
    ExpectFind(suite, L"find fwd: a wrapped full scan without a hit still reports wrapped", Fwd(source, Hex("90 90"), range, 0x108, true), false, 0, true, 15, 0);
    // 起点在范围之外：Forward 小于 first 按 first，不算回绕；大于最大起点则第一段为空。
    ExpectFind(suite, L"find fwd: start below the range begins at first without wrapping", Fwd(source, mz, range, 0x80), true, 0x101, false, 2, 0);
    ExpectFind(suite, L"find fwd: start past the last candidate finds nothing without wrap", Fwd(source, mz, range, 0x10E), false, 0, false, 0, 0);
    ExpectFind(suite, L"find fwd: start past the last candidate wraps to the first hit", Fwd(source, mz, range, 0x10E, true), true, 0x101, true, 2, 0);
    ExpectFind(suite, L"find bwd: start below the range with wrap searches from the top", Bwd(source, mz, range, 0x80, true), true, 0x105, true, 9, 0);
    // 解析器与查找端到端：连写的 HEX 找得到。4D 5A 90 00 在下标 1、5 都成立，先到下标 1。
    ExpectFind(suite, L"end-to-end: 4D5A9000 without spaces finds the 4D 5A 90 00 bytes", Fwd(source, Hex("4D5A9000"), range, 0x100), true, 0x101, false, 2, 0);
    // 重叠命中：AA AA AA 里 AA AA 的起点是 0 和 1，AA 的起点是 0、1、2；最大起点 0x201。
    StaticByteSource aaa(0x200, B({0xAA, 0xAA, 0xAA}));
    const SearchRange aaaRange{ 0x200, 0x202 };
    suite.expect(CountMatches(aaa, Hex("AA AA"), aaaRange, 100).count == 2, L"count: AA AA in AA AA AA has 2 overlapping hits");
    suite.expect(CountMatches(aaa, Hex("AA"), aaaRange, 100).count == 3, L"count: AA in AA AA AA has 3 hits");
    ExpectFind(suite, L"find fwd: overlapping hit at the second byte", Fwd(aaa, Hex("AA AA"), aaaRange, 0x201), true, 0x201, false, 1, 0);
    ExpectFind(suite, L"find fwd: no candidate start remains past the last overlapping hit", Fwd(aaa, Hex("AA AA"), aaaRange, 0x202), false, 0, false, 0, 0);
    ExpectFind(suite, L"find bwd: start past the last candidate is clamped to 0x201", Bwd(aaa, Hex("AA AA"), aaaRange, 0x202), true, 0x201, false, 1, 0);
    // 半字节通配：数据 5A 4B 7B 4C。?B 命中 4B、7B；4? 命中 4B、4C；4? ?B 只命中 4B 7B（下标 1）。
    StaticByteSource nibbles(0, B({0x5A, 0x4B, 0x7B, 0x4C}));
    const SearchRange nibbleRange{ 0, 3 };
    suite.expect(CountMatches(nibbles, Hex("?B"), nibbleRange, 100).count == 2, L"nibble: ?B matches 4B and 7B only");
    suite.expect(CountMatches(nibbles, Hex("4?"), nibbleRange, 100).count == 2, L"nibble: 4? matches 4B and 4C only");
    ExpectFind(suite, L"nibble: 4? ?B has exactly one hit at offset 1", Fwd(nibbles, Hex("4? ?B"), nibbleRange, 0), true, 1, false, 2, 0);
    suite.expect(CountMatches(nibbles, Hex("4? ?B"), nibbleRange, 100).count == 1, L"nibble: 4? ?B counts one hit");
}

// ------------------------------------------------------------
// 五、忽略大小写与 UTF-16
// ------------------------------------------------------------
void TestTextFind(KswordTests::Suite& suite) {
    // 下标：x0 x1 空2 H3 e4 l5 l6 o7 空8 h9 E10 L11 L12 O13 空14 H15 ... h21 e22 L23 L24 o25 空26 x27 x28。
    const std::string haystack = "xx Hello hELLO HELLO heLLo xx";
    StaticByteSource source(0x4000, Bytes(haystack.begin(), haystack.end()));
    const SearchRange range{ 0x4000, 0x4000 + haystack.size() - 1 };
    suite.expect(CountMatches(source, Txt("hello", TextEncoding::Utf8, true), range, 100).count == 4, L"text: ignore-case hello hits all four spellings");
    suite.expect(CountMatches(source, Txt("hello"), range, 100).count == 0, L"text: exact hello hits none of them");
    suite.expect(CountMatches(source, Txt("Hello"), range, 100).count == 1, L"text: exact Hello hits one");
    // 掩码 DF 只该折叠字母：` 与 @、[ 与 { 只差 bit5。数据下标 0:@ 1:A 2:[ 3:a 4:` 5:{。
    StaticByteSource symbols(0, B({0x40, 0x41, 0x5B, 0x61, 0x60, 0x7B}));
    const SearchRange symbolRange{ 0, 5 };
    ExpectFind(suite, L"text: ignore-case ` matches only 0x60, never @", Fwd(symbols, Txt("`", TextEncoding::Utf8, true), symbolRange, 0), true, 4, false, 5, 0);
    suite.expect(CountMatches(symbols, Txt("`", TextEncoding::Utf8, true), symbolRange, 100).count == 1, L"text: ignore-case ` has exactly one hit");
    suite.expect(CountMatches(symbols, Txt("[", TextEncoding::Utf8, true), symbolRange, 100).count == 1, L"text: ignore-case [ never matches {");
    // UTF-16：65 00(e)、45 00(E)、45 01(Ņ 的码元)。忽略大小写的 e 只该命中前两个。
    StaticByteSource wide(0, B({0x65, 0x00, 0x45, 0x00, 0x45, 0x01}));
    suite.expect(CountMatches(wide, Txt("e", TextEncoding::Utf16Le, true), { 0, 5 }, 100).count == 2,
        L"utf16: ignore-case e matches e and E but not the 45 01 code unit");
    // 代理对：x U+1F600 的 UTF-16LE = 78 00 3D D8 00 DE，命中在偏移 2（候选 0、1、2 共 3 个）。
    StaticByteSource emoji(0x80, B({0x78, 0x00, 0x3D, 0xD8, 0x00, 0xDE}));
    ExpectFind(suite, L"utf16: a surrogate pair is found as one 4-byte unit", Fwd(emoji, Txt("\xF0\x9F\x98\x80", TextEncoding::Utf16Le), { 0x80, 0x85 }, 0x80), true, 0x82, false, 3, 0);
}

// ------------------------------------------------------------
// 六、不可读间隙：跳过并计数，绝不当作 00，匹配不得跨越
// ------------------------------------------------------------
void TestUnreadableGaps(KswordTests::Suite& suite) {
    // 32 字节全 0，洞下标 [8,15]（洞里保留真实的 00，"把洞当 00"的实现会多出很多命中）。
    // 模式 00x4 的合法起点：下标 0..4（窗口落在 0..7）与 16..28（窗口落在 16..31）= 5 + 13 = 18 个。
    // 候选起点 0..28 共 29 个，自身字节在洞里的 8..15 共 8 个 -> scanned 21、skipped 8。
    const U64 base = 0x1000;
    GapSource gap(base, Bytes(32, 0));
    gap.holes.push_back({ base + 8, base + 15 });
    gap.allowed = { base, base + 31 };
    const SearchRange range{ base, base + 31 };
    const SearchPattern zeros = Hex("00 00 00 00");
    for (const U64 chunk : { U64(1), U64(4), U64(5), U64(8), U64(9), U64(13), kDefaultChunk }) {
        const CountResult count = CountMatches(gap, zeros, range, 1000, nullptr, chunk);
        suite.expect(count.count == 18 && !count.capped && count.scannedBytes == 21 && count.skippedUnreadableBytes == 8,
            L"gap: hits never use hole bytes and unreadable starts are counted (any chunk size)");
        // 从下标 5 向前：5,6,7 读到但窗口碰洞不命中（3），8..15 自身不可读（8），16 命中（1）-> scanned 4、skipped 8。
        ExpectFind(suite, L"gap: forward jumps over the hole to the first fully readable window", Fwd(gap, zeros, range, base + 5, false, chunk), true, base + 16, false, 4, 8);
        // 从下标 15 向后：15..8 不可读（8），7,6,5 窗口碰洞（3），4 命中（1）-> scanned 4、skipped 8。
        ExpectFind(suite, L"gap: backward from inside the hole finds the last window before it", Bwd(gap, zeros, range, base + 15, false, chunk), true, base + 4, false, 4, 8);
        ExpectFind(suite, L"gap: backward from the end finds the last readable window", Bwd(gap, zeros, range, base + 31, false, chunk), true, base + 28, false, 1, 0);
    }
    suite.expect(!gap.contractViolated, L"gap: the engine only requested addresses inside the range");
    // 不跨越：DE AD BE EF 写在下标 2、6、12；洞 [8,11] 盖住第二份的后两个字节，真实字节虽齐全也不能命中。
    Bytes content(32, 0);
    const Bytes marker = B({0xDE, 0xAD, 0xBE, 0xEF});
    for (const std::size_t at : { std::size_t(2), std::size_t(6), std::size_t(12) }) {
        Put(content, at, marker);
    }
    GapSource straddle(base, content);
    straddle.holes.push_back({ base + 8, base + 11 });
    const SearchPattern deadbeef = Hex("DE AD BE EF");
    suite.expect(CountMatches(straddle, deadbeef, range, 100, nullptr, 7).count == 2, L"gap: a marker straddling the hole is not a hit although its bytes exist");
    // 从下标 3 向前：3..7 读到不命中（5），8..11 不可读（4），12 命中（1）-> scanned 6、skipped 4。
    ExpectFind(suite, L"gap: forward skips the straddler and finds the marker after the hole", Fwd(straddle, deadbeef, range, base + 3, false, 7), true, base + 12, false, 6, 4);
    // 内置 StaticByteSource 的三种读取状态：块内 Ok、两端越出块 Partial（块外填 0 且无效）、块外 Unreadable。
    StaticByteSource block(0x10, B({1, 2, 3, 4}));
    Bytes bytes;
    Bytes valid;
    suite.expect(block.Read(0x10, 4, bytes, valid) == ReadStatus::Ok && bytes == B({1, 2, 3, 4}) && valid == B({1, 1, 1, 1}), L"static source: a read inside the block is Ok");
    suite.expect(block.Read(0x0E, 8, bytes, valid) == ReadStatus::Partial && bytes == B({0, 0, 1, 2, 3, 4, 0, 0}) && valid == B({0, 0, 1, 1, 1, 1, 0, 0}),
        L"static source: a read straddling both ends is Partial with the outside bytes zeroed and invalid");
    suite.expect(block.Read(0x20, 3, bytes, valid) == ReadStatus::Unreadable && valid == B({0, 0, 0}), L"static source: a read outside the block is Unreadable");
    // 块贴着 2^64 顶端：越过顶端的数据被截掉，请求越过顶端时饱和而不回绕到 0。
    StaticByteSource tail(kMax - 1, B({7, 8, 9, 10}));
    suite.expect(tail.Read(kMax - 1, 2, bytes, valid) == ReadStatus::Ok && bytes == B({7, 8}), L"static source: data past 2^64 is truncated");
    suite.expect(tail.Read(kMax, 4, bytes, valid) == ReadStatus::Partial && bytes == B({8, 0, 0, 0}) && valid == B({1, 0, 0, 0}),
        L"static source: a request running past 2^64 saturates instead of wrapping");
    // 数据源违约：缓冲比请求短，宁可当不可读也不能在没读到的字节上匹配。
    struct ShortSource final : IByteSource {
        bool shortValid = false;  // 为真：字节缓冲够长，但 Partial 的有效位向量只有一个元素
        ReadStatus Read(U64, U64 length, Bytes& bytesOut, Bytes& validOut) override {
            bytesOut.assign(shortValid ? static_cast<std::size_t>(length) : 1, 0);
            validOut.assign(1, 1);
            return shortValid ? ReadStatus::Partial : ReadStatus::Ok;
        }
    } shortSource;
    ExpectFind(suite, L"gap: a source returning a short buffer is treated as unreadable", Fwd(shortSource, Hex("00"), { 0, 9 }, 0), false, 0, false, 0, 10);
    shortSource.shortValid = true;
    ExpectFind(suite, L"gap: a Partial read with a short validity vector is treated as unreadable", Fwd(shortSource, Hex("00"), { 0, 9 }, 0), false, 0, false, 0, 10);
}

// ------------------------------------------------------------
// 七、取消标志
// ------------------------------------------------------------
void TestCancel(KswordTests::Suite& suite) {
    // 标记 DE AD 在下标 50；L=2、chunkBytes=8 -> 每块 7 个候选起点：块 1 起点 0..6，块 2 起点 7..13。
    Bytes content(64, 0);
    Put(content, 50, B({0xDE, 0xAD}));
    GapSource source(0x2000, content);
    const SearchRange range{ 0x2000, 0x203F };
    std::atomic<bool> cancel{ false };
    const SearchPattern marker = Hex("DE AD");
    // 标志在读取第 2 块之后置位：第 2 块照常处理完，第 3 块之前检查到取消。
    source.afterRead = CancelAfter(cancel, 2);
    const SearchResult cancelled = Find(source, marker, range, 0x2000, SearchDirection::Forward, false, &cancel, 8);
    suite.expect(cancelled.cancelled && !cancelled.found && !cancelled.wrapped && source.reads == 2
        && cancelled.scannedBytes == 14 && cancelled.skippedUnreadableBytes == 0,
        L"cancel: the flag is checked between chunks and the finished chunks stay counted");
    // 已经置位的标志：一次都不读。
    source.reads = 0;
    source.afterRead = nullptr;
    const SearchResult before = Find(source, marker, range, 0x2000, SearchDirection::Forward, false, &cancel, 8);
    suite.expect(before.cancelled && source.reads == 0 && before.scannedBytes == 0, L"cancel: a flag set before the call prevents any read");
    // CountMatches 同样可取消，count 保留已计数的部分：每块 4 个候选、3 块 = 12。
    cancel.store(false);
    GapSource countSource(0, Bytes(64, 0xAA));
    countSource.afterRead = CancelAfter(cancel, 3);
    const CountResult count = CountMatches(countSource, Hex("AA"), { 0, 63 }, 1000, &cancel, 4);
    suite.expect(count.cancelled && !count.capped && count.count == 12 && countSource.reads == 3,
        L"cancel: count stops between chunks and keeps the 3 finished chunks of 4 hits");
}

// ------------------------------------------------------------
// 八、64 位地址边界
// ------------------------------------------------------------
void TestAddressSpaceEdges(KswordTests::Suite& suite) {
    // 64 字节块贴着地址空间顶端：基址 2^64-64，范围到 UINT64_MAX。标记 DE AD BE EF 写在下标 0..3
    // 与 60..63，最大起点是下标 60（地址 UINT64_MAX-3），候选共 61 个。
    const U64 base = kMax - 63;
    Bytes content(64, 0);
    const Bytes marker = B({0xDE, 0xAD, 0xBE, 0xEF});
    Put(content, 0, marker);
    Put(content, 60, marker);
    GapSource top(base, content);
    const SearchRange range{ base, kMax };
    top.allowed = range;
    const SearchPattern pattern = Hex("DE AD BE EF");
    for (const U64 chunk : { U64(0), U64(1), U64(4), U64(5), U64(7), U64(8), U64(63), kDefaultChunk }) {
        ExpectFind(suite, L"edge: forward from the base hits the first marker at the top of the address space", Fwd(top, pattern, range, base, false, chunk), true, base, false, 1, 0);
        ExpectFind(suite, L"edge: forward reaches the marker ending at UINT64_MAX without overflow", Fwd(top, pattern, range, base + 1, false, chunk), true, kMax - 3, false, 60, 0);
        ExpectFind(suite, L"edge: start past the largest candidate finds nothing and does not wrap", Fwd(top, pattern, range, kMax - 2, false, chunk), false, 0, false, 0, 0);
        ExpectFind(suite, L"edge: start at UINT64_MAX with wrap goes back to the first marker", Fwd(top, pattern, range, kMax, true, chunk), true, base, true, 1, 0);
        ExpectFind(suite, L"edge: backward from UINT64_MAX is clamped and finds the last marker", Bwd(top, pattern, range, kMax, false, chunk), true, kMax - 3, false, 1, 0);
        ExpectFind(suite, L"edge: backward from just below the last marker walks 60 starts to the first", Bwd(top, pattern, range, kMax - 4, false, chunk), true, base, false, 60, 0);
        const CountResult count = CountMatches(top, pattern, range, 100, nullptr, chunk);
        suite.expect(count.count == 2 && !count.capped && count.scannedBytes == 61, L"edge: count over the top of the address space sees both markers");
    }
    suite.expect(!top.contractViolated, L"edge: no read request overflowed or left the range");
    // 单字节模式、范围正好是最后一个地址；范围起点是 0 时向后找不会因减一回绕到顶端。
    StaticByteSource last(kMax, B({0x7E}));
    ExpectFind(suite, L"edge: a one-byte range at UINT64_MAX is searchable", Fwd(last, Hex("7E"), { kMax, kMax }, kMax), true, kMax, false, 1, 0);
    StaticByteSource zero(0, B({0x7E, 0x00, 0x00, 0x7E}));
    ExpectFind(suite, L"edge: backward from 0 without a hit does not wrap to the top", Bwd(zero, Hex("00 7E"), { 0, 3 }, 0), false, 0, false, 1, 0);
    ExpectFind(suite, L"edge: backward from 0 with wrap continues from the top", Bwd(zero, Hex("00 7E"), { 0, 3 }, 0, true), true, 2, true, 2, 0);
    // 步进辅助：不溢出，到头返回 false 且不改输出。
    U64 next = 7;
    suite.expect(AdvanceSearchStart(5, SearchDirection::Forward, next) && next == 6, L"advance: forward adds one");
    suite.expect(AdvanceSearchStart(5, SearchDirection::Backward, next) && next == 4, L"advance: backward subtracts one");
    next = 7;  // 到头时输出必须保持不变，所以先放一个哨兵值。
    suite.expect(!AdvanceSearchStart(kMax, SearchDirection::Forward, next) && next == 7, L"advance: forward at UINT64_MAX reports no next start");
    suite.expect(!AdvanceSearchStart(0, SearchDirection::Backward, next) && next == 7, L"advance: backward at 0 reports no next start");
}

// ------------------------------------------------------------
// 九、参数、块大小夹取与上限
// ------------------------------------------------------------
void TestArgumentsAndCaps(KswordTests::Suite& suite) {
    StaticByteSource source(0, B({0xAA, 0xAA, 0xAA, 0x00}));
    const SearchRange range{ 0, 3 };
    SearchPattern empty;
    SearchPattern mismatched = Hex("AA AA");
    mismatched.mask.pop_back();
    suite.expect(Fwd(source, empty, range, 0).invalidArguments, L"args: an empty pattern is flagged invalid");
    suite.expect(Fwd(source, mismatched, range, 0).invalidArguments, L"args: a mask shorter than the bytes is flagged invalid");
    suite.expect(Fwd(source, Hex("AA"), { 3, 0 }, 0).invalidArguments, L"args: an inverted range is flagged invalid");
    suite.expect(CountMatches(source, empty, range, 5).invalidArguments, L"args: count flags an empty pattern too");
    // 块大小夹取到 [模式长度, 64 MiB]：0 与过小的值抬到模式长度，荒谬的大值压到上限。
    suite.expect(EffectiveChunkBytes(0, 4) == 4 && EffectiveChunkBytes(1, 4) == 4 && EffectiveChunkBytes(8, 4) == 8
        && EffectiveChunkBytes(5, 100) == 100 && EffectiveChunkBytes(U64(1) << 40, 4) == kMaxSearchChunkBytes,
        L"chunk size: clamped to [pattern length, 64 MiB]");
    // 范围放不下模式不是错误，只是没有候选起点。
    ExpectFind(suite, L"args: a pattern longer than the range finds nothing without an error", Fwd(source, Hex("AA AA AA AA AA"), range, 0), false, 0, false, 0, 0);
    // cap：到达就停并置 capped（"至少 cap 个"）；cap 为 0 一个字节都不扫。
    const CountResult capTwo = CountMatches(source, Hex("AA"), range, 2);
    suite.expect(capTwo.count == 2 && capTwo.capped && capTwo.scannedBytes == 2, L"cap: stopping at 2 only examines up to the second hit");
    const CountResult capFour = CountMatches(source, Hex("AA"), range, 4);
    suite.expect(capFour.count == 3 && !capFour.capped, L"cap: a cap above the real count is not reported as capped");
    GapSource untouched(0, Bytes(8, 0xAA));
    const CountResult capZero = CountMatches(untouched, Hex("AA"), { 0, 7 }, 0);
    suite.expect(capZero.count == 0 && capZero.capped && untouched.reads == 0, L"cap: a cap of 0 scans nothing");
}

// ------------------------------------------------------------
// 十、对拍：暴力参考实现 vs 被测引擎（含洞、范围越过数据块、地址空间顶端）
// ------------------------------------------------------------
void TestDifferential(KswordTests::Suite& suite) {
    struct Spec { const wchar_t* label; SearchPattern pattern; Bytes alphabet; };
    const Spec specs[] = {
        { L"diff: literal AA", Hex("AA"), B({0xAA, 0xBB}) },
        { L"diff: overlapping literal AA AA", Hex("AA AA"), B({0xAA, 0xAA, 0xAA, 0xBB}) },
        { L"diff: literal AA BB AA", Hex("AA BB AA"), B({0xAA, 0xBB}) },
        // 首字节是通配：memchr 锚点在下标 1，覆盖"锚点偏移"的换算。
        { L"diff: leading and interior wildcards ?? AA ?? AA", Hex("?? AA ?? AA"), B({0xAA, 0xBB, 0xCC}) },
        { L"diff: nibble wildcards A? ?B", Hex("A? ?B"), B({0xAA, 0xAB, 0xBB, 0xBA, 0x5B}) },
        { L"diff: ignore-case text ab", Txt("ab", TextEncoding::Utf8, true), B({0x41, 0x61, 0x42, 0x62, 0x78}) },
        { L"diff: all-wildcard ?? ??", Hex("?? ??"), B({0xAA, 0xBB}) },
        { L"diff: long literal 5x AA", Hex("AA AA AA AA AA"), B({0xAA, 0xAA, 0xAA, 0xBB}) },
        { L"diff: never-matching CC", Hex("CC"), B({0xAA, 0xBB}) },
        // 手工构造的非规范模式：通配位上 bytes 不为 0，判据 (data & mask) == (bytes & mask) 仍必须成立。
        { L"diff: non-canonical bytes under the mask", SearchPattern{ B({0xFF, 0x3C}), B({0xF0, 0x0F}), "manual" }, B({0xF3, 0xF0, 0x0C, 0x3C, 0xFF, 0x00}) },
        { L"diff: utf16 ignore-case a", Txt("a", TextEncoding::Utf16Le, true), B({0x41, 0x61, 0x00, 0x01}) },
    };
    std::uint32_t rng = 0x2545F491u;
    auto next = [&rng]() {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return rng;
    };
    // 块大小 0..8 逐值（0 与小于模式长度的值会被夹到模式长度）、16，以及默认值（n<=40 时即"一次性扫描"）。
    const U64 chunks[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 16, kDefaultChunk };
    for (const Spec& spec : specs) {
        int mismatches = 0;
        for (const std::size_t n : { std::size_t(1), std::size_t(2), std::size_t(3), std::size_t(6), std::size_t(17), std::size_t(40) }) {
            for (int baseKind = 0; baseKind < 3; ++baseKind) {
                // 三种基址：0（下边界）、0x1000、紧贴 2^64 的顶端。
                const U64 base = (baseKind == 0) ? 0 : ((baseKind == 1) ? 0x1000 : kMax - (n - 1));
                for (int variant = 0; variant < 2; ++variant) {
                    Bytes content;
                    for (std::size_t i = 0; i < n; ++i) {
                        content.push_back(spec.alphabet[next() % spec.alphabet.size()]);
                    }
                    GapSource source(base, content);
                    SearchRange range{ base, base + (n - 1) };
                    if (variant == 1) {
                        // 范围两端各越出数据块 3 字节（不溢出时），并随机挖两个洞。
                        range.first = (base >= 3) ? base - 3 : base;
                        range.last = (kMax - range.last >= 3) ? range.last + 3 : kMax;
                        for (int h = 0; h < 2; ++h) {
                            const U64 lo = base + next() % n;
                            const U64 len = 1 + next() % 4;
                            source.holes.push_back({ lo, (kMax - lo >= len - 1) ? lo + len - 1 : kMax });
                        }
                    }
                    source.allowed = range;
                    // 起点：范围内外各两格，外加 0 与 UINT64_MAX 两个极端。
                    std::vector<U64> starts = { 0, kMax };
                    for (int d = -2; d <= static_cast<int>(range.last - range.first) + 2; ++d) {
                        const bool underflow = d < 0 && range.first < static_cast<U64>(-d);
                        const bool overflow = d > 0 && kMax - range.first < static_cast<U64>(d);
                        if (!underflow && !overflow) {
                            starts.push_back(d < 0 ? range.first - static_cast<U64>(-d) : range.first + static_cast<U64>(d));
                        }
                    }
                    for (const U64 start : starts) {
                        for (int dir = 0; dir < 2; ++dir) {
                            for (int wrap = 0; wrap < 2; ++wrap) {
                                const SearchDirection direction = dir == 0 ? SearchDirection::Forward : SearchDirection::Backward;
                                const RefResult ref = RefFind(source, spec.pattern, range, start, direction, wrap != 0);
                                for (const U64 chunk : chunks) {
                                    const SearchResult got = Find(source, spec.pattern, range, start, direction, wrap != 0, nullptr, chunk);
                                    const bool same = got.found == ref.found && got.address == ref.address && got.wrapped == ref.wrapped
                                        && got.scannedBytes == ref.scanned && got.skippedUnreadableBytes == ref.skipped
                                        && !got.cancelled && !got.invalidArguments;
                                    if (!same && ++mismatches <= 3) {
                                        std::wcerr << L"    mismatch n=" << n << L" base=0x" << std::hex << base << L" start=0x" << start << std::dec
                                                   << L" variant=" << variant << L" dir=" << dir << L" wrap=" << wrap << L" chunk=" << chunk << L'\n';
                                    }
                                }
                            }
                        }
                    }
                    // 计数：重叠命中全计，统计与参考一致；cap=2 时到达即停。
                    const std::vector<U64> candidates = RefCandidates(spec.pattern, range);
                    U64 expectedHits = 0;
                    U64 expectedScanned = 0;
                    for (const U64 candidate : candidates) {
                        expectedHits += RefMatch(source, spec.pattern, candidate) ? 1 : 0;
                        expectedScanned += source.Readable(candidate) ? 1 : 0;
                    }
                    for (const U64 chunk : chunks) {
                        const CountResult all = CountMatches(source, spec.pattern, range, kMax, nullptr, chunk);
                        const CountResult two = CountMatches(source, spec.pattern, range, 2, nullptr, chunk);
                        const bool same = all.count == expectedHits && !all.capped && all.scannedBytes == expectedScanned
                            && all.skippedUnreadableBytes == candidates.size() - expectedScanned
                            && two.count == (expectedHits < 2 ? expectedHits : 2) && two.capped == (expectedHits >= 2);
                        mismatches += same ? 0 : 1;
                    }
                    // 引擎的读请求必须始终落在范围内、不溢出（违约计入不一致）。
                    mismatches += source.contractViolated ? 1 : 0;
                }
            }
        }
        suite.expect(mismatches == 0, spec.label);
    }
}

// ------------------------------------------------------------
// 十一、性能：16 MiB 随机缓冲，只打印数字，不断言耗时
// ------------------------------------------------------------
void TestPerformance(KswordTests::Suite& suite) {
    constexpr std::size_t kSize = std::size_t(16) << 20;
    Bytes data(kSize);
    std::uint64_t state = 0x9E3779B97F4A7C15ULL;
    for (std::size_t i = 0; i < kSize; ++i) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        data[i] = static_cast<std::uint8_t>(state >> 24);
    }
    // 打掉自然出现的 DE AD BE EF（改首字节，不会制造新的出现），保证完整扫描无命中。
    for (std::size_t i = 0; i + 4 <= kSize; ++i) {
        if (data[i] == 0xDE && data[i + 1] == 0xAD && data[i + 2] == 0xBE && data[i + 3] == 0xEF) {
            data[i] = 0xDF;
        }
    }
    const U64 base = 0x10000000;
    const SearchRange range{ base, base + kSize - 1 };
    StaticByteSource clean(base, data);
    const SearchPattern literal = Hex("DE AD BE EF");
    // 计时：含数据源拷贝开销（真实使用也要付）。rate 返回 MiB/s。
    using Clock = std::chrono::steady_clock;
    auto rate = [](Clock::time_point begin) {
        const double seconds = std::chrono::duration<double>(Clock::now() - begin).count();
        return (static_cast<double>(kSize) / (1024.0 * 1024.0)) / (seconds > 0 ? seconds : 1e-9);
    };
    auto begin = Clock::now();
    const SearchResult forward = Fwd(clean, literal, range, base);
    const double literalForward = rate(begin);
    begin = Clock::now();
    const SearchResult backward = Bwd(clean, literal, range, range.last);
    const double literalBackward = rate(begin);
    // 只有半字节通配、没有任何精确字节：走逐位置比较的最慢路径。
    begin = Clock::now();
    const CountResult nibble = CountMatches(clean, Hex("D? A? B? E?"), range, kMax);
    const double nibbleRate = rate(begin);
    ExpectFind(suite, L"perf: a full forward literal scan examines every candidate and finds nothing", forward, false, 0, false, kSize - 3, 0);
    ExpectFind(suite, L"perf: a full backward literal scan examines every candidate and finds nothing", backward, false, 0, false, kSize - 3, 0);
    suite.expect(nibble.scannedBytes == kSize - 3 && !nibble.capped, L"perf: the nibble-only full count examines every candidate");
    // 打入 3 处已知出现：首、默认块边界上（块 1 的最后一个窗口，字节落在与块 2 重叠的尾部）、末尾。
    const std::size_t boundary = static_cast<std::size_t>(kDefaultChunk) - 3;
    for (const std::size_t at : { std::size_t(0), boundary, kSize - 4 }) {
        Put(data, at, literal.bytes);
    }
    StaticByteSource planted(base, std::move(data));
    const CountResult planting = CountMatches(planted, literal, range, 100);
    suite.expect(planting.count == 3 && !planting.capped, L"perf: the fast path finds the 3 planted occurrences at the first, chunk-boundary and last offsets");
    ExpectFind(suite, L"perf: searching from just past the first planted hit lands on the chunk-boundary one", Fwd(planted, literal, range, base + 1), true, base + boundary, false, boundary, 0);

    const std::streamsize oldPrecision = std::wcout.precision();
    std::wcout << std::fixed << std::setprecision(1) << L"  MEMWB byte search perf (16 MiB random, MiB/s): literal4 forward "
               << literalForward << L", literal4 backward " << literalBackward << L", nibble-only count " << nibbleRate << L'\n';
    std::wcout.unsetf(std::ios::floatfield);
    std::wcout.precision(oldPrecision);
    KswordTests::Suite::clearIfBroken(std::wcout);
}

} // namespace

int RunMemwbByteSearchTests() {
    KswordTests::Suite suite(L"MEMWB byte search");
    g_suite = &suite;
    TestHexAccepted(suite);
    TestHexRejected(suite);
    TestTextAccepted(suite);
    TestTextRejected(suite);
    TestHandComputedFind(suite);
    TestTextFind(suite);
    TestUnreadableGaps(suite);
    TestCancel(suite);
    TestAddressSpaceEdges(suite);
    TestArgumentsAndCaps(suite);
    TestDifferential(suite);
    TestPerformance(suite);
    suite.report();
    g_suite = nullptr;
    return suite.failures();
}
