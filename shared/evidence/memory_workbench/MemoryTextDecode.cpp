#include "MemoryTextDecode.h"

#include <algorithm>

namespace ksword::memwb
{
    namespace
    {
        bool IsScalar(const std::uint32_t value)
        {
            return value <= 0x10FFFFU && (value < 0xD800U || value > 0xDFFFU);
        }

        std::size_t ExpectedCodePageLength(const MemoryTextEncoding encoding, const std::uint8_t* bytes,
            const std::size_t available)
        {
            if (bytes[0] < 0x80U) return 1;
            if (encoding == MemoryTextEncoding::Gb18030 && available > 1
                && bytes[0] >= 0x81U && bytes[0] <= 0xFEU && bytes[1] >= 0x30U && bytes[1] <= 0x39U)
                return 4;
            if (encoding == MemoryTextEncoding::Gbk || encoding == MemoryTextEncoding::Gb18030 || encoding == MemoryTextEncoding::Big5)
                return bytes[0] >= 0x81U && bytes[0] <= 0xFEU ? 2U : 1U;
            return 1;
        }
    }

    TextDecodeResult DecodeMemoryText(const std::vector<std::uint8_t>& bytes,
        const std::vector<std::uint8_t>& stateMask, const TextDecodeOptions& options)
    {
        TextDecodeResult result;
        result.effectiveEncoding = options.encoding == MemoryTextEncoding::Auto ? options.autoFallback : options.encoding;
        if (result.effectiveEncoding == MemoryTextEncoding::Auto) result.effectiveEncoding = MemoryTextEncoding::Utf8;
        if (bytes.size() != stateMask.size()) return result;
        result.validInput = true;
        result.glyphs.reserve(bytes.size());
        const auto readable = [&stateMask](const std::size_t offset, const std::size_t length) {
            for (std::size_t n = 0; n < length; ++n) if (stateMask[offset + n] != 1U) return false;
            return true;
        };
        const auto incompleteKind = [&stateMask](const std::size_t offset, const std::size_t length) {
            auto kind = TextGlyphKind::Incomplete;
            for (std::size_t n = 0; n < length; ++n)
            {
                if (stateMask[offset + n] == 2U) return TextGlyphKind::Loading;
                if (stateMask[offset + n] != 1U) kind = TextGlyphKind::Unreadable;
            }
            return kind;
        };
        const auto append = [&result](const std::size_t offset, const std::size_t length,
            const std::uint32_t scalar, const TextGlyphKind kind) {
            result.glyphs.push_back({offset, length, scalar, kind});
        };
        std::size_t position = 0;
        if (options.recognizeBom && !options.oddLeadingByte)
        {
            MemoryTextEncoding bomEncoding = MemoryTextEncoding::Auto;
            std::size_t bomLength = 0;
            if (bytes.size() >= 3 && readable(0, 3) && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF)
            { bomEncoding = MemoryTextEncoding::Utf8; bomLength = 3; }
            else if (bytes.size() >= 2 && readable(0, 2) && bytes[0] == 0xFF && bytes[1] == 0xFE)
            { bomEncoding = MemoryTextEncoding::Utf16LE; bomLength = 2; }
            else if (bytes.size() >= 2 && readable(0, 2) && bytes[0] == 0xFE && bytes[1] == 0xFF)
            { bomEncoding = MemoryTextEncoding::Utf16BE; bomLength = 2; }
            if (bomLength != 0 && (options.encoding == MemoryTextEncoding::Auto || result.effectiveEncoding == bomEncoding))
            {
                result.bomDetected = true;
                result.effectiveEncoding = bomEncoding;
                append(0, bomLength, 0xFEFFU, TextGlyphKind::Bom);
                position = bomLength;
            }
        }
        if (position == 0 && options.oddLeadingByte
            && (result.effectiveEncoding == MemoryTextEncoding::Utf16LE || result.effectiveEncoding == MemoryTextEncoding::Utf16BE)
            && !bytes.empty())
        {
            append(0, 1, 0, stateMask[0] == 0 ? TextGlyphKind::Unreadable
                : stateMask[0] == 2 ? TextGlyphKind::Loading : TextGlyphKind::Incomplete);
            position = 1;
        }
        while (position < bytes.size())
        {
            const bool utf16 = result.effectiveEncoding == MemoryTextEncoding::Utf16LE || result.effectiveEncoding == MemoryTextEncoding::Utf16BE;
            if (stateMask[position] != 1U && !utf16)
            {
                append(position, 1, 0, stateMask[position] == 2U ? TextGlyphKind::Loading : TextGlyphKind::Unreadable);
                ++position;
                continue;
            }
            const std::size_t available = bytes.size() - position;
            const std::uint8_t lead = bytes[position];
            std::size_t length = 1;
            std::uint32_t scalar = lead;
            TextGlyphKind kind = TextGlyphKind::Character;
            if (result.effectiveEncoding == MemoryTextEncoding::Utf8)
            {
                if (lead < 0x80U) length = 1;
                else if ((lead & 0xE0U) == 0xC0U) { length = 2; scalar = lead & 0x1FU; }
                else if ((lead & 0xF0U) == 0xE0U) { length = 3; scalar = lead & 0x0FU; }
                else if ((lead & 0xF8U) == 0xF0U) { length = 4; scalar = lead & 0x07U; }
                else kind = TextGlyphKind::Invalid;
                if (length > available)
                {
                    bool continuationValid = true;
                    for (std::size_t n = 1; n < available; ++n)
                        if (stateMask[position + n] == 1U && (bytes[position + n] & 0xC0U) != 0x80U)
                            continuationValid = false;
                    if (!continuationValid) { length = 1; kind = TextGlyphKind::Invalid; }
                    else
                    {
                        length = available;
                        kind = incompleteKind(position, length);
                    }
                }
                else if (!readable(position, length))
                {
                    kind = TextGlyphKind::Unreadable;
                    for (std::size_t n = 0; n < length; ++n)
                        if (stateMask[position + n] == 2U) kind = TextGlyphKind::Loading;
                }
                else if (kind == TextGlyphKind::Character && length > 1)
                {
                    for (std::size_t n = 1; n < length; ++n)
                    {
                        if ((bytes[position + n] & 0xC0U) != 0x80U)
                        { length = 1; kind = TextGlyphKind::Invalid; break; }
                        scalar = (scalar << 6U) | (bytes[position + n] & 0x3FU);
                    }
                    const std::uint32_t minimum = length == 2 ? 0x80U : length == 3 ? 0x800U : length == 4 ? 0x10000U : 0U;
                    if (kind == TextGlyphKind::Character && (!IsScalar(scalar) || scalar < minimum))
                        kind = TextGlyphKind::Invalid;
                }
            }
            else if (result.effectiveEncoding == MemoryTextEncoding::Utf16LE || result.effectiveEncoding == MemoryTextEncoding::Utf16BE)
            {
                length = 2;
                const bool little = result.effectiveEncoding == MemoryTextEncoding::Utf16LE;
                const auto unit = [&bytes, little](const std::size_t at) -> std::uint32_t {
                    return little ? static_cast<std::uint32_t>(bytes[at]) | (static_cast<std::uint32_t>(bytes[at + 1]) << 8U)
                        : (static_cast<std::uint32_t>(bytes[at]) << 8U) | bytes[at + 1];
                };
                if (available < 2) { length = available; kind = incompleteKind(position, length); }
                else if (!readable(position, 2))
                    kind = stateMask[position] == 2U || stateMask[position + 1] == 2U ? TextGlyphKind::Loading : TextGlyphKind::Unreadable;
                else
                {
                    scalar = unit(position);
                    if (scalar >= 0xD800U && scalar <= 0xDBFFU)
                    {
                        if (available < 4) { length = available; kind = incompleteKind(position, length); }
                        else if (!readable(position + 2, 2))
                        {
                            length = 4;
                            kind = stateMask[position + 2] == 2U || stateMask[position + 3] == 2U
                                ? TextGlyphKind::Loading : TextGlyphKind::Unreadable;
                        }
                        else
                        {
                            const std::uint32_t low = unit(position + 2);
                            if (low < 0xDC00U || low > 0xDFFFU) kind = TextGlyphKind::Invalid;
                            else { scalar = 0x10000U + ((scalar - 0xD800U) << 10U) + (low - 0xDC00U); length = 4; }
                        }
                    }
                    else if (scalar >= 0xDC00U && scalar <= 0xDFFFU) kind = TextGlyphKind::Invalid;
                }
            }
            else if (result.effectiveEncoding == MemoryTextEncoding::Ascii)
            {
                if (lead > 0x7FU) kind = TextGlyphKind::Invalid;
            }
            else
            {
                length = ExpectedCodePageLength(result.effectiveEncoding, bytes.data() + position, available);
                // ACP can also be UTF-8 or another multibyte page: the adapter
                // chooses its full character length from the readable prefix.
                std::size_t readablePrefix = 0;
                while (readablePrefix < std::min<std::size_t>(4, available)
                    && stateMask[position + readablePrefix] == 1U) ++readablePrefix;
                TextCodePageResult decoded;
                if (options.codePageDecoder)
                    decoded = options.codePageDecoder(result.effectiveEncoding, bytes.data() + position, readablePrefix);
                if (!decoded.ok && decoded.consumed > 0 && decoded.consumed <= 4)
                    length = decoded.consumed;
                if (decoded.ok && decoded.consumed > 0 && decoded.consumed <= readablePrefix && IsScalar(decoded.scalar))
                { length = decoded.consumed; scalar = decoded.scalar; }
                else if (length > available) { length = available; kind = incompleteKind(position, length); }
                else if (!readable(position, length))
                {
                    kind = TextGlyphKind::Unreadable;
                    for (std::size_t n = 0; n < length; ++n)
                        if (stateMask[position + n] == 2U) kind = TextGlyphKind::Loading;
                }
                else { length = 1; kind = TextGlyphKind::Invalid; }
            }
            append(position, length, scalar, kind);
            position += length;
        }
        return result;
    }
}
