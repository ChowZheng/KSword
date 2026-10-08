#include "../../shared/evidence/memory_workbench/MemoryTextDecode.h"
#include "../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchTextCodePages.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <random>

using namespace ksword::memwb;
namespace
{
    int checks = 0;
    void Check(const bool condition, const char* text)
    {
        ++checks;
        if (!condition) { std::cerr << "FAIL: " << text << '\n'; std::exit(1); }
    }
    TextDecodeResult Decode(const std::vector<std::uint8_t>& bytes, const MemoryTextEncoding encoding,
        std::vector<std::uint8_t> mask = {}, const bool odd = false)
    {
        if (mask.empty()) mask.assign(bytes.size(), 1);
        TextDecodeOptions options;
        options.encoding = encoding;
        options.oddLeadingByte = odd;
        options.codePageDecoder = ks::ui::DecodeWorkbenchCodePage;
        return DecodeMemoryText(bytes, mask, options);
    }
    void Coverage(const TextDecodeResult& result, const std::size_t length)
    {
        Check(result.validInput, "valid input accepted");
        std::size_t offset = 0;
        for (const auto& glyph : result.glyphs)
        {
            Check(glyph.offset == offset && glyph.byteLength > 0 && glyph.byteLength <= length - offset,
                "each byte belongs to exactly one bounded glyph");
            if (glyph.kind == TextGlyphKind::Character)
                Check(glyph.scalar <= 0x10FFFFU && (glyph.scalar < 0xD800U || glyph.scalar > 0xDFFFU), "character is Unicode scalar");
            offset += glyph.byteLength;
        }
        Check(offset == length, "glyphs cover all original bytes");
    }
}
int main()
{
    Check(static_cast<int>(MemoryTextEncoding::Ansi) == 0 && static_cast<int>(MemoryTextEncoding::Utf8) == 1
        && static_cast<int>(MemoryTextEncoding::Utf16LE) == 2, "persisted encoding values remain stable");
    const auto utf8 = Decode({0x41, 0xE4, 0xB8, 0xAD, 0xF0, 0x9F, 0x98, 0x80}, MemoryTextEncoding::Utf8);
    Coverage(utf8, 8);
    Check(utf8.glyphs.size() == 3 && utf8.glyphs[1].scalar == 0x4E2D && utf8.glyphs[1].offset == 1
        && utf8.glyphs[1].byteLength == 3 && utf8.glyphs[2].scalar == 0x1F600
        && utf8.glyphs[2].byteLength == 4, "CJK and emoji keep original byte spans");
    for (const std::size_t boundary : {16U, 4096U})
    {
        std::vector<std::uint8_t> bytes(boundary - 1, 'A');
        bytes.insert(bytes.end(), {0xE4, 0xB8, 0xAD, 'B'});
        const auto result = Decode(bytes, MemoryTextEncoding::Utf8);
        Check(result.glyphs[boundary - 1].scalar == 0x4E2D && result.glyphs.back().scalar == 'B',
            "row/page crossing is not a decoding boundary");
    }
    const auto le = Decode({0x41, 0, 0x3D, 0xD8, 0, 0xDE, 0x42, 0}, MemoryTextEncoding::Utf16LE);
    const auto be = Decode({0, 0x41, 0xD8, 0x3D, 0xDE, 0, 0, 0x42}, MemoryTextEncoding::Utf16BE);
    Check(le.glyphs.size() == 3 && be.glyphs.size() == 3 && le.glyphs[1].scalar == 0x1F600
        && be.glyphs[1].scalar == 0x1F600 && be.glyphs[1].byteLength == 4, "both UTF-16 orders compose surrogate pairs");
    const auto missing = Decode({0x41, 0, 0x42, 0, 0x43, 0}, MemoryTextEncoding::Utf16LE, {0, 1, 1, 1, 1, 1});
    Check(missing.glyphs.size() == 3 && missing.glyphs[0].kind == TextGlyphKind::Unreadable
        && missing.glyphs[1].scalar == 'B' && missing.glyphs[2].scalar == 'C', "unreadable UTF-16 byte does not shift following pairs");
    for (const auto encoding : {MemoryTextEncoding::Utf16LE, MemoryTextEncoding::Utf16BE})
        for (const std::uint8_t state : {0U, 1U, 2U})
        {
            const auto expected = state == 2U ? TextGlyphKind::Loading
                : state == 0U ? TextGlyphKind::Unreadable : TextGlyphKind::Incomplete;
            const auto oneByte = Decode({0x41}, encoding, {state});
            Check(oneByte.glyphs[0].kind == expected, "UTF-16 terminal byte retains its actual evidence state");
            const auto shortPair = Decode(encoding == MemoryTextEncoding::Utf16LE
                ? std::vector<std::uint8_t>{0, 0xD8, 0x41} : std::vector<std::uint8_t>{0xD8, 0, 0x41},
                encoding, {1, 1, state});
            Check(shortPair.glyphs[0].kind == expected && shortPair.glyphs[0].byteLength == 3,
                "UTF-16 partial surrogate retains unreadable/loading tail evidence");
        }
    const auto surrogate = Decode({0, 0xD8, 0x41, 0}, MemoryTextEncoding::Utf16LE);
    Check(surrogate.glyphs[0].kind == TextGlyphKind::Invalid && surrogate.glyphs[1].scalar == 'A', "unpaired surrogate preserves next character");
    const auto odd = Decode({0xFF, 0x41, 0}, MemoryTextEncoding::Utf16LE, {1, 1, 1}, true);
    Check(odd.glyphs[0].kind == TextGlyphKind::Incomplete && odd.glyphs[1].scalar == 'A', "odd window restores stream pairing");
    const auto trunc = Decode({0xE4, 0xB8}, MemoryTextEncoding::Utf8);
    const auto malformed = Decode({0xE4, 0x41}, MemoryTextEncoding::Utf8);
    const auto unread = Decode({0xE4, 0xB8, 0xAD}, MemoryTextEncoding::Utf8, {1, 0, 1});
    const auto loading = Decode({0xE4, 0xB8, 0xAD}, MemoryTextEncoding::Utf8, {1, 2, 1});
    Check(trunc.glyphs[0].kind == TextGlyphKind::Incomplete, "window end is incomplete, not invalid");
    Check(malformed.glyphs[0].kind == TextGlyphKind::Invalid && malformed.glyphs[1].scalar == 'A', "malformed tail is invalid, next ASCII retained");
    Check(unread.glyphs[0].kind == TextGlyphKind::Unreadable && loading.glyphs[0].kind == TextGlyphKind::Loading, "missing and loading distinguished");
    for (const auto& mask : std::vector<std::vector<std::uint8_t>>{{1, 2, 0}, {1, 0, 2}})
        Check(Decode({0xF0, 0x90, 0x80}, MemoryTextEncoding::Utf8, mask).glyphs[0].kind == TextGlyphKind::Loading,
            "truncated UTF-8 retains loading evidence regardless of mask order");
    for (const std::uint8_t state : {std::uint8_t{0}, std::uint8_t{2}})
        Check(Decode({0x90, 0x30, 0x81}, MemoryTextEncoding::Gb18030, {1, 1, state}).glyphs[0].kind
            == (state == 2 ? TextGlyphKind::Loading : TextGlyphKind::Unreadable),
            "truncated code-page scalar retains unreadable or loading tail evidence");
    for (const auto& invalid : std::vector<std::vector<std::uint8_t>>{
        {0xC1, 0xA1}, {0xE0, 0x81, 0xA1}, {0xED, 0xA0, 0x80}, {0xF4, 0x90, 0x80, 0x80}})
        Check(Decode(invalid, MemoryTextEncoding::Utf8).glyphs[0].kind == TextGlyphKind::Invalid, "invalid Unicode is not accepted");
    const auto bom = Decode({0xFE, 0xFF, 0, 0x41}, MemoryTextEncoding::Auto);
    Check(bom.bomDetected && bom.effectiveEncoding == MemoryTextEncoding::Utf16BE && bom.glyphs[0].kind == TextGlyphKind::Bom
        && bom.glyphs[0].byteLength == 2 && bom.glyphs[1].scalar == 'A', "BOM selects encoding and retains byte mapping");
    const auto manual = Decode({0xFE, 0xFF, 0, 0x41}, MemoryTextEncoding::Utf8);
    Check(!manual.bomDetected && manual.effectiveEncoding == MemoryTextEncoding::Utf8, "manual encoding takes priority over conflicting BOM");
    Check(!DecodeMemoryText({1}, {}, TextDecodeOptions{}).validInput, "mismatched masks rejected");
#ifdef _WIN32
    const auto gbk = Decode({0xD6, 0xD0, 0xCE, 0xC4}, MemoryTextEncoding::Gbk);
    const auto big5 = Decode({0xA4, 0xA4}, MemoryTextEncoding::Big5);
    const auto gb18030 = Decode({0x90, 0x30, 0x81, 0x30}, MemoryTextEncoding::Gb18030);
    Check(gbk.glyphs.size() == 2 && gbk.glyphs[0].scalar == 0x4E2D && gbk.glyphs[1].scalar == 0x6587, "Windows GBK strict adapter");
    Check(big5.glyphs.size() == 1 && big5.glyphs[0].scalar == 0x4E2D && big5.glyphs[0].byteLength == 2, "Windows Big5 strict adapter");
    Check(gb18030.glyphs.size() == 1 && gb18030.glyphs[0].scalar == 0x10000 && gb18030.glyphs[0].byteLength == 4, "Windows GB18030 supplementary scalar");
    Check(Decode({0x81}, MemoryTextEncoding::Gbk).glyphs[0].kind == TextGlyphKind::Incomplete, "DBCS lead at window end");
    Check(Decode({0x81, 0x20, 'A'}, MemoryTextEncoding::Gbk).glyphs.back().scalar == 'A', "invalid DBCS does not swallow following ASCII");
    Check(Decode({'A'}, MemoryTextEncoding::Ansi).glyphs[0].scalar == 'A', "system ANSI basic character");
#endif
    std::mt19937 random(0x4B5357U);
    for (int iteration = 0; iteration < 512; ++iteration)
    {
        std::vector<std::uint8_t> bytes(64), states(64);
        for (std::size_t i = 0; i < bytes.size(); ++i) { bytes[i] = static_cast<std::uint8_t>(random()); states[i] = static_cast<std::uint8_t>(random() % 3); }
        for (const auto encoding : {MemoryTextEncoding::Utf8, MemoryTextEncoding::Utf16LE, MemoryTextEncoding::Utf16BE,
            MemoryTextEncoding::Ascii, MemoryTextEncoding::Gbk, MemoryTextEncoding::Gb18030, MemoryTextEncoding::Big5})
            Coverage(Decode(bytes, encoding, states), bytes.size());
    }
    std::cout << "Memory text decode: " << checks << " checks passed\n";
}
