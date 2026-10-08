#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Byte-oriented registry codecs. No Windows API or Qt dependency: callers keep
// the original bytes and only encode after an explicit, validated user edit.
namespace ks::registry
{
    constexpr std::uint32_t TypeNone = 0;
    constexpr std::uint32_t TypeString = 1;
    constexpr std::uint32_t TypeExpandString = 2;
    constexpr std::uint32_t TypeBinary = 3;
    constexpr std::uint32_t TypeDword = 4;
    constexpr std::uint32_t TypeDwordBigEndian = 5;
    constexpr std::uint32_t TypeMultiString = 7;
    constexpr std::uint32_t TypeQword = 11;

    // Failure never returns a partial decode/encode or changes an output.
    // canonical reports exact conventional termination, not merely readability.
    bool DecodeString(std::string_view bytes, std::u16string* text, bool* canonical = nullptr);
    bool EncodeString(std::u16string_view text, std::string* bytes);
    bool DecodeMultiString(std::string_view bytes, std::vector<std::u16string>* items,
        bool* canonical = nullptr);
    bool EncodeMultiString(const std::vector<std::u16string>& items, std::string* bytes);

    // Decimal accepts digits only. Hex additionally accepts an optional 0x prefix.
    // Signs, embedded whitespace, overflow and invalid digit sequences fail.
    bool ParseUnsigned(std::string_view text, unsigned base, unsigned bits,
        std::uint64_t* value);
    bool DecodeUnsigned(std::string_view bytes, unsigned width, std::uint64_t* value,
        bool bigEndian = false);
    bool EncodeUnsigned(std::uint64_t value, unsigned width, std::string* bytes,
        bool bigEndian = false);
}
