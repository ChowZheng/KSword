#pragma once

#include <cstdint>
#include <limits>
#include <string_view>

namespace Ksword::Evidence {

// Accept addresses pasted from debuggers while rejecting ambiguous or wrapped input.
inline bool ParseHexAddress(std::string_view text, std::uint64_t& value) noexcept
{
    const auto isSpace = [](const char ch) {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
    };
    while (!text.empty() && isSpace(text.front())) { text.remove_prefix(1); }
    while (!text.empty() && isSpace(text.back())) { text.remove_suffix(1); }
    if (text.size() >= 2 && text.front() == '0' &&
        (text[1] == 'x' || text[1] == 'X')) { text.remove_prefix(2); }
    if (text.empty()) { return false; }

    std::uint64_t parsed = 0;
    bool previousDigit = false;
    for (const char ch : text)
    {
        if (ch == '`' || ch == '_')
        {
            if (!previousDigit) { return false; }
            previousDigit = false;
            continue;
        }
        unsigned int digit = 0;
        if (ch >= '0' && ch <= '9') { digit = static_cast<unsigned int>(ch - '0'); }
        else if (ch >= 'a' && ch <= 'f') { digit = static_cast<unsigned int>(ch - 'a' + 10); }
        else if (ch >= 'A' && ch <= 'F') { digit = static_cast<unsigned int>(ch - 'A' + 10); }
        else { return false; }
        if (parsed > ((std::numeric_limits<std::uint64_t>::max)() - digit) / 16)
        {
            return false;
        }
        parsed = parsed * 16 + digit;
        previousDigit = true;
    }
    if (!previousDigit) { return false; }
    value = parsed;
    return true;
}

} // namespace Ksword::Evidence
