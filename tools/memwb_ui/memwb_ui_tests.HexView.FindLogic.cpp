// memwb_ui_tests.HexView.FindLogic.cpp
// 作用：查找条背后的纯逻辑层（HexFindSearch.h）的验证——不经过任何控件：
//   1) 模式解析与错误描述、字节偏移到字符序号的换算；
//   2) ByteArraySource 的读取语义（含贴着 64 位末端）；
//   3) RunSearch 对"朴素查找"的差分测试：随机数据 / 随机模式（含半字节通配）/ 随机起点 / 两个方向 / 回绕开关，
//      期望值由与被测代码无关的朴素实现按 MemoryByteSearch.h 文档语义独立推出；
//   4) HitsInRange 对朴素实现的差分测试：随机可见窗口，含起点在窗口之前、尾部延伸进窗口以及越出窗口的命中；
//   5) 起点越界、取消标志、非法参数、上限 cap。

#include "memwb_ui_hexview.h"

#include "../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/HexViewFormat.h"

#include <algorithm>
#include <atomic>
#include <random>

namespace memwb_test
{
    namespace
    {
        namespace hf = ks::ui::hexfind;
        using ksword::memwb::SearchDirection;
        using ksword::memwb::SearchPattern;

        // MakePattern2：由字节与掩码构造模式（跳过解析器，直接给差分测试用）。
        SearchPattern MakeSearchPattern(const std::vector<std::uint8_t>& bytes, const std::vector<std::uint8_t>& mask)
        {
            SearchPattern pattern;
            pattern.bytes = bytes;
            pattern.mask = mask;
            pattern.description = "test";
            return pattern;
        }

        // Expect：朴素推出的期望结果。
        struct Expect
        {
            bool found = false;         // 是否命中
            std::uint64_t address = 0;  // 命中起点
            bool wrapped = false;       // 是否进入回绕段
        };

        // ExpectedFind：按文档语义（MemoryByteSearch.h 第四节）由全部命中起点列表推出一次 Find 的结果。
        // 传入：全部命中起点（升序）；缓冲范围 [first,last]；起点 start（在范围内）；方向；是否回绕。
        // 传出：期望结果。
        Expect ExpectedFind(
            const std::vector<std::uint64_t>& matches,
            std::uint64_t first,
            std::uint64_t start,
            bool forward,
            bool wrap,
            std::uint64_t maxStart)
        {
            Expect expect;
            if (forward)
            {
                // 第一段：起点 >= start 的最小命中。
                for (const std::uint64_t candidate : matches)
                {
                    if (candidate >= std::max(start, first))
                    {
                        expect.found = true;
                        expect.address = candidate;
                        return expect;
                    }
                }
                // 第二段：只含 start 之前的候选；段为空时 wrapped 保持 false。
                if (wrap && start > first)
                {
                    expect.wrapped = true;
                    for (const std::uint64_t candidate : matches)
                    {
                        if (candidate < start)
                        {
                            expect.found = true;
                            expect.address = candidate;
                            return expect;
                        }
                    }
                }
                return expect;
            }

            // 向前找：第一段是起点 <= min(start, maxStart) 的最大命中。
            const std::uint64_t clamped = std::min(start, maxStart);
            for (auto iterator = matches.rbegin(); iterator != matches.rend(); ++iterator)
            {
                if (*iterator <= clamped)
                {
                    expect.found = true;
                    expect.address = *iterator;
                    return expect;
                }
            }
            // 第二段：从末端往回，只含 start 之后的候选。
            if (wrap && start < maxStart)
            {
                expect.wrapped = true;
                for (auto iterator = matches.rbegin(); iterator != matches.rend(); ++iterator)
                {
                    if (*iterator > start)
                    {
                        expect.found = true;
                        expect.address = *iterator;
                        return expect;
                    }
                }
            }
            return expect;
        }

        // 解析与错误描述。
        void TestParseAndDescribe()
        {
            namespace ms = ksword::memwb;
            SearchPattern pattern;
            ms::ParseError error;
            QByteArray utf8;

            // 十六进制成功：字节与掩码。
            CHECK(hf::ParsePattern(hf::Mode::Hex, QStringLiteral("4D ?? A?"), false, pattern, error, &utf8));
            CHECK(pattern.bytes.size() == 3 && pattern.mask == std::vector<std::uint8_t>({ 0xFF, 0x00, 0xF0 }));
            CHECK(pattern.bytes[0] == 0x4D);

            // 文本：UTF-8 / UTF-16LE，大小写开关决定掩码。
            CHECK(hf::ParsePattern(hf::Mode::TextUtf8, QStringLiteral("字a"), true, pattern, error, nullptr));
            CHECK(pattern.bytes == std::vector<std::uint8_t>({ 0xE5, 0xAD, 0x97, 0x61 }));
            CHECK(pattern.mask == std::vector<std::uint8_t>({ 0xFF, 0xFF, 0xFF, 0xFF }));
            CHECK(hf::ParsePattern(hf::Mode::TextUtf8, QStringLiteral("a"), false, pattern, error, nullptr));
            CHECK(pattern.mask == std::vector<std::uint8_t>({ 0xDF }));
            CHECK(hf::ParsePattern(hf::Mode::TextUtf16Le, QStringLiteral("字"), true, pattern, error, nullptr));
            CHECK(pattern.bytes == std::vector<std::uint8_t>({ 0x57, 0x5B }));

            // 失败：错误码与描述，位置换算成 1 起的字符序号。
            CHECK(!hf::ParsePattern(hf::Mode::Hex, QStringLiteral("4D5"), false, pattern, error, &utf8));
            CHECK(error.code == ms::ParseErrorCode::BadPattern && error.position == 2);
            CHECK(hf::DescribeParseError(error, utf8) == QStringLiteral("第 3 个字符处有误"));
            CHECK(!hf::ParsePattern(hf::Mode::Hex, QString(), false, pattern, error, &utf8));
            CHECK(error.code == ms::ParseErrorCode::Empty);
            CHECK(hf::DescribeParseError(error, utf8) == QStringLiteral("输入为空"));
            CHECK(pattern.bytes.empty() && pattern.mask.empty());

            // 字节偏移 -> 字符序号：含中文时两者不同。
            const QByteArray mixed = QStringLiteral("中ab").toUtf8();
            CHECK(ks::ui::hexview_format::CharIndexFromUtf8Offset(mixed, 0) == 0);
            CHECK(ks::ui::hexview_format::CharIndexFromUtf8Offset(mixed, 3) == 1);
            CHECK(ks::ui::hexview_format::CharIndexFromUtf8Offset(mixed, 4) == 2);
            CHECK(ks::ui::hexview_format::CharIndexFromUtf8Offset(mixed, 99) == 3);
            CHECK(ks::ui::hexview_format::AddressDigitsFor(0, 0xFFFFFFFFULL) == 8);
            CHECK(ks::ui::hexview_format::AddressDigitsFor(0, 0x100000000ULL) == 16);
            CHECK(ks::ui::hexview_format::AddressDigitsFor(0x100000000ULL, 0x100000001ULL) == 16);
            CHECK(ks::ui::hexview_format::PlainToolTip(QStringLiteral("<b>&")).contains(QStringLiteral("&lt;b&gt;&amp;")));
            CHECK(ks::ui::hexview_format::PlainToolTip(QString()).isEmpty());
        }

        // ByteArraySource 的读取语义。
        void TestByteArraySource()
        {
            namespace ms = ksword::memwb;
            const QByteArray data = MakePattern(16);
            std::vector<std::uint8_t> bytes;
            std::vector<std::uint8_t> valid;

            hf::ByteArraySource source(data, 0x100);
            CHECK(source.Read(0x104, 4, bytes, valid) == ms::ReadStatus::Ok);
            CHECK(bytes.size() == 4 && bytes[0] == static_cast<std::uint8_t>(data.at(4)));
            CHECK(source.Read(0x100, 0, bytes, valid) == ms::ReadStatus::Ok && bytes.empty());

            // 前部越界：Partial，越界部分无效。
            CHECK(source.Read(0xFE, 4, bytes, valid) == ms::ReadStatus::Partial);
            CHECK(valid == std::vector<std::uint8_t>({ 0, 0, 1, 1 }));
            CHECK(bytes[2] == static_cast<std::uint8_t>(data.at(0)));

            // 后部越界与完全不相交。
            CHECK(source.Read(0x10E, 4, bytes, valid) == ms::ReadStatus::Partial);
            CHECK(valid == std::vector<std::uint8_t>({ 1, 1, 0, 0 }));
            CHECK(source.Read(0x200, 4, bytes, valid) == ms::ReadStatus::Unreadable);
            CHECK(source.Read(0x10, 4, bytes, valid) == ms::ReadStatus::Unreadable);

            // 空数据源。
            hf::ByteArraySource empty(QByteArray(), 0);
            CHECK(empty.Read(0, 4, bytes, valid) == ms::ReadStatus::Unreadable);

            // 贴着 64 位末端：请求越过 2^64 时饱和，不回绕成小地址。
            const std::uint64_t base = 0xFFFFFFFFFFFFFFFCULL;
            hf::ByteArraySource tail(data.left(4), base);
            CHECK(tail.Read(0xFFFFFFFFFFFFFFFEULL, 10, bytes, valid) == ms::ReadStatus::Partial);
            CHECK(valid[0] == 1 && valid[1] == 1 && valid[2] == 0);
            CHECK(bytes[0] == static_cast<std::uint8_t>(data.at(2)) && bytes[1] == static_cast<std::uint8_t>(data.at(3)));
            CHECK(tail.Read(base, 4, bytes, valid) == ms::ReadStatus::Ok);
        }

        // RunSearch 对朴素实现的差分测试。
        void TestRunSearchDifferential()
        {
            std::mt19937 random(0x5EED1234U);
            const std::uint8_t alphabet[] = { 0xAA, 0xBB, 0xCC, 0x00 };
            const std::uint8_t masks[] = { 0xFF, 0xFF, 0xFF, 0xF0, 0x0F, 0x00 };
            int trials = 0;
            int mismatches = 0;
            int wrappedCount = 0;
            int notFoundCount = 0;
            for (int trial = 0; trial < 600; ++trial)
            {
                // 数据：长度 1..80，字母表只有 4 个值，保证命中很多且重叠。
                const int size = 1 + static_cast<int>(random() % 80);
                QByteArray data(size, '\0');
                for (int index = 0; index < size; ++index)
                {
                    data[index] = static_cast<char>(alphabet[random() % 4]);
                }

                // 基址：0、普通、贴着 64 位末端三种。
                const std::uint64_t bases[] = { 0ULL, 0x1000ULL, 0xFFFFFFFFFFFFFFFFULL - static_cast<std::uint64_t>(size) + 1ULL };
                const std::uint64_t base = bases[random() % 3];

                // 模式：长度 1..4，字节取自字母表（保证有命中），掩码随机。
                const int length = 1 + static_cast<int>(random() % 4);
                std::vector<std::uint8_t> patternBytes;
                std::vector<std::uint8_t> patternMask;
                QByteArray needle;
                QByteArray needleMask;
                for (int index = 0; index < length; ++index)
                {
                    const std::uint8_t byte = alphabet[random() % 4];
                    const std::uint8_t mask = masks[random() % 6];
                    patternBytes.push_back(static_cast<std::uint8_t>(byte & mask));
                    patternMask.push_back(mask);
                    needle.append(static_cast<char>(byte & mask));
                    needleMask.append(static_cast<char>(mask));
                }
                const SearchPattern pattern = MakeSearchPattern(patternBytes, patternMask);

                // 全部命中的起点（朴素）、起点（范围内）、方向、回绕。
                const std::vector<std::uint64_t> matches = NaiveMatches(data, base, needle, needleMask);
                const std::uint64_t last = base + static_cast<std::uint64_t>(size) - 1ULL;
                const std::uint64_t start = base + (random() % static_cast<std::uint64_t>(size));
                const bool forward = (random() % 2) == 0;
                const bool wrap = (random() % 4) != 0;
                const std::uint64_t maxStart = (static_cast<std::uint64_t>(size) >= static_cast<std::uint64_t>(length))
                    ? last - (static_cast<std::uint64_t>(length) - 1ULL)
                    : base;

                // 范围放不下一个模式：按文档"无结果"，也不进入回绕段。
                const bool fits = static_cast<std::uint64_t>(size) >= static_cast<std::uint64_t>(length);
                const Expect expect = fits ? ExpectedFind(matches, base, start, forward, wrap, maxStart) : Expect();

                const hf::Outcome actual = hf::RunSearch(
                    data, base, pattern, start, forward ? SearchDirection::Forward : SearchDirection::Backward, wrap, nullptr);
                ++trials;
                const bool same = !actual.invalid && !actual.cancelled
                    && actual.found == expect.found
                    && (!expect.found || actual.address == expect.address)
                    && actual.wrapped == expect.wrapped;
                if (!same)
                {
                    ++mismatches;
                    if (mismatches <= 3)
                    {
                        CHECK_NOTE(false, QStringLiteral("trial %1 size=%2 len=%3 start=%4 fwd=%5 wrap=%6 expect(found=%7 addr=%8 wrapped=%9) actual(found=%10 addr=%11 wrapped=%12)")
                            .arg(trial).arg(size).arg(length).arg(start - base).arg(forward).arg(wrap)
                            .arg(expect.found).arg(expect.address - base).arg(expect.wrapped)
                            .arg(actual.found).arg(actual.address - base).arg(actual.wrapped));
                    }
                }
                wrappedCount += expect.wrapped ? 1 : 0;
                notFoundCount += expect.found ? 0 : 1;
            }
            CHECK(mismatches == 0);
            CHECK(trials == 600);

            // 覆盖度：回绕与未找到两个分支都被随机试验真正走到（防止差分测试其实只走了平凡路径）。
            CHECK_NOTE(wrappedCount > 40, QString::number(wrappedCount));
            CHECK_NOTE(notFoundCount > 40, QString::number(notFoundCount));
        }

        // HitsInRange 对朴素实现的差分测试：与窗口有交集的全部命中，含重叠。
        void TestHitsInRangeDifferential()
        {
            std::mt19937 random(0xC0FFEE11U);
            const std::uint8_t alphabet[] = { 0xAA, 0xBB, 0x00 };
            int mismatches = 0;
            int straddleBefore = 0;
            int straddleAfter = 0;
            for (int trial = 0; trial < 800; ++trial)
            {
                const int size = 1 + static_cast<int>(random() % 120);
                QByteArray data(size, '\0');
                for (int index = 0; index < size; ++index)
                {
                    data[index] = static_cast<char>(alphabet[random() % 3]);
                }
                const std::uint64_t bases[] = { 0ULL, 0x1000ULL, 0xFFFFFFFFFFFFFFFFULL - static_cast<std::uint64_t>(size) + 1ULL };
                const std::uint64_t base = bases[random() % 3];

                const int length = 1 + static_cast<int>(random() % 5);
                QByteArray needle;
                QByteArray needleMask;
                std::vector<std::uint8_t> patternBytes;
                std::vector<std::uint8_t> patternMask;
                for (int index = 0; index < length; ++index)
                {
                    const std::uint8_t byte = alphabet[random() % 3];
                    const std::uint8_t mask = (random() % 5 == 0) ? 0x00 : 0xFF;
                    needle.append(static_cast<char>(byte & mask));
                    needleMask.append(static_cast<char>(mask));
                    patternBytes.push_back(static_cast<std::uint8_t>(byte & mask));
                    patternMask.push_back(mask);
                }

                // 可见窗口：起点与终点都在缓冲内（含单字节窗口）。
                std::uint64_t visibleFirst = base + (random() % static_cast<std::uint64_t>(size));
                std::uint64_t visibleLast = base + (random() % static_cast<std::uint64_t>(size));
                if (visibleFirst > visibleLast)
                {
                    std::swap(visibleFirst, visibleLast);
                }

                // 期望：与窗口有交集的全部命中 [m, m+L-1]，按起点升序。
                std::vector<hf::AddressRange> expected;
                for (const std::uint64_t match : NaiveMatches(data, base, needle, needleMask))
                {
                    const std::uint64_t matchLast = match + static_cast<std::uint64_t>(length) - 1ULL;
                    if (match <= visibleLast && matchLast >= visibleFirst)
                    {
                        hf::AddressRange range;
                        range.first = match;
                        range.last = matchLast;
                        expected.push_back(range);
                        straddleBefore += (match < visibleFirst) ? 1 : 0;
                        straddleAfter += (matchLast > visibleLast) ? 1 : 0;
                    }
                }
                const std::vector<hf::AddressRange> actual = hf::HitsInRange(
                    data, base, MakeSearchPattern(patternBytes, patternMask), visibleFirst, visibleLast, 100000);
                if (actual != expected)
                {
                    ++mismatches;
                    if (mismatches <= 3)
                    {
                        CHECK_NOTE(false, QStringLiteral("trial %1 size=%2 len=%3 window=[%4,%5] expected=%6 actual=%7")
                            .arg(trial).arg(size).arg(length).arg(visibleFirst - base).arg(visibleLast - base)
                            .arg(expected.size()).arg(actual.size()));
                    }
                }
            }
            CHECK(mismatches == 0);

            // 覆盖度：起点在窗口之前、尾部越出窗口之后的命中都被随机试验覆盖到。
            CHECK_NOTE(straddleBefore > 30, QString::number(straddleBefore));
            CHECK_NOTE(straddleAfter > 30, QString::number(straddleAfter));
        }

        // 内存孔洞不能被零字节或通配符命中；上下方向、跨孔模式和高亮共用真实掩码。
        void TestUnreadableMemoryMask()
        {
            // data/mask：第 1、5 字节是未读占位 00，其余是已确认读到的数据。
            const QByteArray data = QByteArray::fromHex("0000004142004142");
            const QByteArray mask = QByteArray::fromHex("0100010101000101");
            const std::uint64_t base = 0x100;
            const SearchPattern zero = MakeSearchPattern({ 0x00 }, { 0xFF });
            const SearchPattern zeroPair = MakeSearchPattern({ 0x00, 0x00 }, { 0xFF, 0xFF });
            const SearchPattern anyPair = MakeSearchPattern({ 0x00, 0x00 }, { 0x00, 0x00 });
            const SearchPattern letters = MakeSearchPattern({ 0x41, 0x42 }, { 0xFF, 0xFF });

            hf::Outcome outcome = hf::RunSearch(data, base, zero, base + 1, SearchDirection::Forward, false, nullptr, mask);
            CHECK(outcome.found && outcome.address == base + 2);
            outcome = hf::RunSearch(data, base, zero, base + 5, SearchDirection::Backward, false, nullptr, mask);
            CHECK(outcome.found && outcome.address == base + 2);
            CHECK(!hf::RunSearch(data, base, zeroPair, base, SearchDirection::Forward, true, nullptr, mask).found);
            CHECK(hf::RunSearch(data, base, letters, base + 4, SearchDirection::Forward, false, nullptr, mask).address == base + 6);
            CHECK(hf::HitsInRange(data, base, anyPair, base, base + 7, 100, mask).size() == 3);
            CHECK(hf::HitsInRange(data, base, zeroPair, base, base + 7, 100, mask).empty());
            // 空掩码保留完整文件缓冲行为；错误长度的非空掩码必须拒绝而非退回“全有效”。
            CHECK(hf::RunSearch(data, base, zeroPair, base, SearchDirection::Forward, false, nullptr).found);
            CHECK(hf::RunSearch(data, base, zero, base, SearchDirection::Forward, false, nullptr, mask.left(2)).invalid);
            CHECK(hf::HitsInRange(data, base, zero, base, base + 7, 100, mask.left(2)).empty());

            hf::ByteArraySource source(data, base, mask);
            std::vector<std::uint8_t> bytes; // 输出实际数据，未读字节仍由 valid 明确标识
            std::vector<std::uint8_t> valid; // 输出可读性，搜索不得忽略
            CHECK(source.Read(base, 3, bytes, valid) == ksword::memwb::ReadStatus::Partial);
            CHECK(valid == std::vector<std::uint8_t>({ 1, 0, 1 }));
            CHECK(source.Read(base + 1, 1, bytes, valid) == ksword::memwb::ReadStatus::Unreadable);
            CHECK(source.Read(base + 3, 2, bytes, valid) == ksword::memwb::ReadStatus::Ok);
            CHECK(hf::RunSearch(data, 0xFFFFFFFFFFFFFFFCULL, zero, base, SearchDirection::Forward, true, nullptr, mask).invalid);
        }

        // 起点越界、取消标志、非法参数、上限。
        void TestEdgesAndLimits()
        {
            const QByteArray data = QByteArray::fromHex("00112233445566778899");
            const SearchPattern pattern = MakeSearchPattern({ 0x44, 0x55 }, { 0xFF, 0xFF });
            const std::uint64_t base = 0x100;

            // 起点小于范围首地址：向前找按首地址处理，不算回绕。
            hf::Outcome outcome = hf::RunSearch(data, base, pattern, 0x10, SearchDirection::Forward, false, nullptr);
            CHECK(outcome.found && outcome.address == 0x104 && !outcome.wrapped);

            // 起点大于最大起点：向后找且不回绕 -> 未找到；回绕 -> 从头找到并标注 wrapped。
            outcome = hf::RunSearch(data, base, pattern, 0x200, SearchDirection::Forward, false, nullptr);
            CHECK(!outcome.found && !outcome.wrapped);
            outcome = hf::RunSearch(data, base, pattern, 0x200, SearchDirection::Forward, true, nullptr);
            CHECK(outcome.found && outcome.address == 0x104 && outcome.wrapped);

            // 向前找：起点大于最大起点按最大起点处理。
            outcome = hf::RunSearch(data, base, pattern, 0x300, SearchDirection::Backward, false, nullptr);
            CHECK(outcome.found && outcome.address == 0x104 && !outcome.wrapped);

            // 取消标志已置位：立即返回 cancelled，不命中。
            std::atomic<bool> cancel(true);
            outcome = hf::RunSearch(data, base, pattern, base, SearchDirection::Forward, true, &cancel);
            CHECK(outcome.cancelled && !outcome.found);

            // 非法参数：空数据、空模式、掩码长度不等。
            CHECK(hf::RunSearch(QByteArray(), base, pattern, base, SearchDirection::Forward, true, nullptr).invalid);
            CHECK(hf::RunSearch(data, base, SearchPattern(), base, SearchDirection::Forward, true, nullptr).invalid);
            CHECK(hf::RunSearch(data, base, MakeSearchPattern({ 0x44, 0x55 }, { 0xFF }), base, SearchDirection::Forward, true, nullptr).invalid);

            // 模式比数据长：未找到，不崩溃。
            const SearchPattern longPattern = MakeSearchPattern(std::vector<std::uint8_t>(32, 0x44), std::vector<std::uint8_t>(32, 0xFF));
            outcome = hf::RunSearch(data, base, longPattern, base, SearchDirection::Forward, true, nullptr);
            CHECK(!outcome.found && !outcome.invalid);

            // HitsInRange：cap 限制数量；全通配模式每个位置都命中；窗口在缓冲之外为空；非法窗口为空。
            const SearchPattern anyByte = MakeSearchPattern({ 0x00 }, { 0x00 });
            CHECK(hf::HitsInRange(data, base, anyByte, base, base + 9, 100).size() == 10);
            CHECK(hf::HitsInRange(data, base, anyByte, base, base + 9, 4).size() == 4);
            CHECK(hf::HitsInRange(data, base, anyByte, base, base + 9, 0).empty());
            CHECK(hf::HitsInRange(data, base, anyByte, base + 50, base + 60, 100).empty());
            CHECK(hf::HitsInRange(data, base, anyByte, base + 5, base + 2, 100).empty());
            CHECK(hf::HitsInRange(QByteArray(), base, anyByte, base, base + 9, 100).empty());
        }
    }

    // 查找纯逻辑验证入口。
    void RunHexViewFindLogicTests()
    {
        TestParseAndDescribe();
        TestByteArraySource();
        TestRunSearchDifferential();
        TestHitsInRangeDifferential();
        TestUnreadableMemoryMask();
        TestEdgesAndLimits();
    }
}
