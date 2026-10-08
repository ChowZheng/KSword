#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace ksword::memwb
{
    // Values 0..2 are the established workbench settings values.
    enum class MemoryTextEncoding : int
    {
        Ansi = 0, Utf8 = 1, Utf16LE = 2, Utf16BE = 3,
        Gbk = 4, Gb18030 = 5, Big5 = 6, Ascii = 7, Auto = 8
    };

    enum class TextGlyphKind { Character, Bom, Invalid, Unreadable, Loading, Incomplete };

    struct TextGlyph
    {
        std::size_t offset = 0;
        std::size_t byteLength = 0;
        std::uint32_t scalar = 0;
        TextGlyphKind kind = TextGlyphKind::Character;
    };

    struct TextCodePageResult
    {
        bool ok = false;
        std::uint32_t scalar = 0;
        std::size_t consumed = 0;
    };

    // The adapter must strictly reject invalid input, never silently replace it.
    using TextCodePageDecoder = std::function<TextCodePageResult(
        MemoryTextEncoding, const std::uint8_t*, std::size_t)>;

    struct TextDecodeOptions
    {
        MemoryTextEncoding encoding = MemoryTextEncoding::Utf8;
        MemoryTextEncoding autoFallback = MemoryTextEncoding::Utf8;
        TextCodePageDecoder codePageDecoder;
        // UTF-16 pairing is relative to the explicitly selected stream origin,
        // rather than to the absolute address or the current display row.
        bool oddLeadingByte = false;
        bool recognizeBom = true;
    };

    struct TextDecodeResult
    {
        MemoryTextEncoding effectiveEncoding = MemoryTextEncoding::Utf8;
        bool bomDetected = false;
        bool validInput = false;
        std::vector<TextGlyph> glyphs;
    };

    // stateMask: 1=readable, 0=unreadable, 2=not loaded/in flight.
    // Every input byte belongs to exactly one glyph, including BOM and errors.
    TextDecodeResult DecodeMemoryText(const std::vector<std::uint8_t>& bytes,
        const std::vector<std::uint8_t>& stateMask, const TextDecodeOptions& options);
}
