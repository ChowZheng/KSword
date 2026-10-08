#include "RegistryValueCodec.h"

#include <limits>
#include <utility>

namespace ks::registry
{
    namespace
    {
        bool ValidUtf16(std::u16string_view text)
        {
            for (std::size_t i = 0; i < text.size(); ++i)
            {
                const char16_t ch = text[i];
                if (ch >= 0xd800 && ch <= 0xdbff)
                {
                    if (++i >= text.size() || text[i] < 0xdc00 || text[i] > 0xdfff)
                        return false;
                }
                else if (ch >= 0xdc00 && ch <= 0xdfff)
                    return false;
            }
            return true;
        }

        bool DecodeUtf16(std::string_view bytes, std::u16string* text)
        {
            if (!text || bytes.size() % 2 != 0)
                return false;
            std::u16string result;
            result.reserve(bytes.size() / 2);
            for (std::size_t i = 0; i < bytes.size(); i += 2)
            {
                const auto lo = static_cast<unsigned char>(bytes[i]);
                const auto hi = static_cast<unsigned char>(bytes[i + 1]);
                result.push_back(static_cast<char16_t>(lo | (static_cast<unsigned>(hi) << 8)));
            }
            if (!ValidUtf16(result))
                return false;
            *text = std::move(result);
            return true;
        }

        void AppendUtf16(std::string* bytes, std::u16string_view text)
        {
            for (const char16_t ch : text)
            {
                bytes->push_back(static_cast<char>(ch & 0xff));
                bytes->push_back(static_cast<char>((ch >> 8) & 0xff));
            }
        }
    }

    bool DecodeString(std::string_view bytes, std::u16string* text, bool* canonical)
    {
        if (!text)
            return false;
        std::u16string result;
        if (!DecodeUtf16(bytes, &result))
            return false;
        const std::size_t originalSize = result.size();
        while (!result.empty() && result.back() == 0)
            result.pop_back();
        if (result.find(char16_t(0)) != std::u16string::npos)
            return false;
        if (canonical)
            *canonical = originalSize == result.size() + 1;
        *text = std::move(result);
        return true;
    }

    bool EncodeString(std::u16string_view text, std::string* bytes)
    {
        if (!bytes || text.find(char16_t(0)) != std::u16string_view::npos || !ValidUtf16(text))
            return false;
        std::string result;
        AppendUtf16(&result, text);
        result.append(2, '\0');
        *bytes = std::move(result);
        return true;
    }

    bool DecodeMultiString(std::string_view bytes, std::vector<std::u16string>* items,
        bool* canonical)
    {
        if (!items)
            return false;
        std::u16string text;
        if (!DecodeUtf16(bytes, &text))
            return false;
        std::vector<std::u16string> result;
        std::size_t cursor = 0;
        std::size_t terminator = text.size();
        while (cursor < text.size())
        {
            const auto end = text.find(char16_t(0), cursor);
            if (end == cursor)
            {
                terminator = cursor;
                for (std::size_t i = cursor; i < text.size(); ++i)
                    if (text[i] != 0)
                        return false;
                break;
            }
            if (end == std::u16string::npos)
            {
                result.push_back(text.substr(cursor));
                cursor = text.size();
            }
            else
            {
                result.push_back(text.substr(cursor, end - cursor));
                cursor = end + 1;
            }
        }
        if (canonical)
        {
            *canonical = result.empty()
                ? text.size() == 2 && text[0] == 0 && text[1] == 0
                : terminator < text.size() && terminator + 1 == text.size();
        }
        *items = std::move(result);
        return true;
    }

    bool EncodeMultiString(const std::vector<std::u16string>& items, std::string* bytes)
    {
        if (!bytes)
            return false;
        std::string result;
        for (const auto& item : items)
        {
            if (item.empty() || item.find(char16_t(0)) != std::u16string::npos || !ValidUtf16(item))
                return false;
            AppendUtf16(&result, item);
            result.append(2, '\0');
        }
        result.append(items.empty() ? 4 : 2, '\0');
        *bytes = std::move(result);
        return true;
    }

    bool ParseUnsigned(std::string_view text, unsigned base, unsigned bits, std::uint64_t* value)
    {
        if (!value || (base != 10 && base != 16) || (bits != 32 && bits != 64))
            return false;
        if (base == 16 && text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
            text.remove_prefix(2);
        if (text.empty())
            return false;
        const std::uint64_t maximum = bits == 32
            ? UINT64_C(0xffffffff) : (std::numeric_limits<std::uint64_t>::max)();
        std::uint64_t result = 0;
        for (const char ch : text)
        {
            unsigned digit;
            if (ch >= '0' && ch <= '9')
                digit = static_cast<unsigned>(ch - '0');
            else if (ch >= 'a' && ch <= 'f')
                digit = static_cast<unsigned>(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F')
                digit = static_cast<unsigned>(ch - 'A' + 10);
            else
                return false;
            if (digit >= base || result > (maximum - digit) / base)
                return false;
            result = result * base + digit;
        }
        *value = result;
        return true;
    }

    bool DecodeUnsigned(std::string_view bytes, unsigned width, std::uint64_t* value,
        bool bigEndian)
    {
        if (!value || (width != 4 && width != 8) || bytes.size() != width)
            return false;
        std::uint64_t result = 0;
        for (unsigned i = 0; i < width; ++i)
        {
            const unsigned shift = (bigEndian ? width - i - 1 : i) * 8;
            result |= std::uint64_t(static_cast<unsigned char>(bytes[i])) << shift;
        }
        *value = result;
        return true;
    }

    bool EncodeUnsigned(std::uint64_t value, unsigned width, std::string* bytes, bool bigEndian)
    {
        if (!bytes || (width != 4 && width != 8) || (width == 4 && value > UINT64_C(0xffffffff)))
            return false;
        std::string result(width, '\0');
        for (unsigned i = 0; i < width; ++i)
        {
            const unsigned shift = (bigEndian ? width - i - 1 : i) * 8;
            result[i] = static_cast<char>((value >> shift) & 0xff);
        }
        *bytes = std::move(result);
        return true;
    }
}
