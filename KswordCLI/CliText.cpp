#include "CliText.h"

#include "../shared/driver/KswordArkCallbackIoctl.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

// 本文件实现 CliText.h 声明的文本/字节工具，函数体逐字搬自 KswordCLI.cpp。
namespace ks::cli::legacy
{
    // readFileBytes loads a whole file into memory for blob-style IOCTL inputs.
    // Inputs: path and an upper size bound to reject oversized protocol blobs.
    // Processing: opens the file in binary mode and validates the actual length.
    // Returns: byte vector on success; throws on IO or size violations.
    std::vector<std::uint8_t> readFileBytes(const std::wstring& path, std::size_t maxBytes)
    {
        if (path.empty())
        {
            throw std::invalid_argument("file path");
        }

        HANDLE file = ::CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            throw std::runtime_error("open file");
        }

        LARGE_INTEGER size{};
        if (::GetFileSizeEx(file, &size) == FALSE)
        {
            ::CloseHandle(file);
            throw std::runtime_error("file size");
        }
        if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > maxBytes)
        {
            ::CloseHandle(file);
            throw std::out_of_range("file too large");
        }

        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size.QuadPart));
        DWORD readBytes = 0;
        if (!bytes.empty())
        {
            if (::ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &readBytes, nullptr) == FALSE)
            {
                ::CloseHandle(file);
                throw std::runtime_error("file read");
            }
            bytes.resize(readBytes);
        }
        ::CloseHandle(file);
        return bytes;
    }

    // parseHexBytes accepts either 0x-prefixed numbers or raw hex byte strings.
    // Inputs: text token containing bytes like "DE AD BE EF" or "0x1234".
    // Processing: strips separators and decodes two hex digits per byte.
    // Returns: decoded bytes; throws on malformed input.
    std::vector<std::uint8_t> parseHexBytes(const std::wstring& text)
    {
        std::wstring normalized = text;
        if (normalized.size() >= 2U && normalized[0] == L'0' && (normalized[1] == L'x' || normalized[1] == L'X'))
        {
            normalized.erase(0U, 2U);
        }

        std::wstring cleaned;
        cleaned.reserve(text.size());
        for (wchar_t ch : normalized)
        {
            if (std::iswxdigit(ch))
            {
                cleaned.push_back(static_cast<wchar_t>(std::towupper(ch)));
            }
        }

        if (cleaned.empty())
        {
            return {};
        }
        if ((cleaned.size() % 2U) != 0U)
        {
            throw std::invalid_argument("hex bytes");
        }

        std::vector<std::uint8_t> bytes;
        bytes.reserve(cleaned.size() / 2U);
        for (std::size_t index = 0U; index < cleaned.size(); index += 2U)
        {
            const wchar_t hi = cleaned[index];
            const wchar_t lo = cleaned[index + 1U];
            const auto nibble = [](wchar_t c) -> int {
                if (c >= L'0' && c <= L'9') return static_cast<int>(c - L'0');
                if (c >= L'A' && c <= L'F') return static_cast<int>(10 + (c - L'A'));
                return -1;
            };
            const int hiValue = nibble(hi);
            const int loValue = nibble(lo);
            if (hiValue < 0 || loValue < 0)
            {
                throw std::invalid_argument("hex bytes");
            }
            bytes.push_back(static_cast<std::uint8_t>((hiValue << 4) | loValue));
        }
        return bytes;
    }

    // fixedAnsi copies a fixed char array into std::string safely.
    // Inputs: text points at protocol storage; maxBytes is the field capacity.
    // Processing: scans until NUL or capacity, whichever comes first.
    // Returns: a possibly empty string without embedded protocol padding.
    std::string fixedAnsi(const char* text, std::size_t maxBytes)
    {
        if (text == nullptr || maxBytes == 0U)
        {
            return {};
        }

        std::size_t length = 0U;
        while (length < maxBytes && text[length] != '\0')
        {
            ++length;
        }
        return std::string(text, text + length);
    }

    // fixedWide copies a fixed wchar_t array into std::wstring safely.
    // Inputs: text points at protocol storage; maxChars is the field capacity.
    // Processing: scans until NUL or capacity, whichever comes first.
    // Returns: a possibly empty wide string without trailing padding.
    std::wstring fixedWide(const wchar_t* text, std::size_t maxChars)
    {
        if (text == nullptr || maxChars == 0U)
        {
            return {};
        }

        std::size_t length = 0U;
        while (length < maxChars && text[length] != L'\0')
        {
            ++length;
        }
        return std::wstring(text, text + length);
    }

    // fixedUtf16 copies unsigned-short UTF-16 protocol fields into std::wstring.
    // Inputs: text points at UTF-16 code units; maxChars bounds the scan.
    // Processing: casts code units to wchar_t, matching Windows UTF-16 wchar_t.
    // Returns: a std::wstring suitable for console output on Windows.
    std::wstring fixedUtf16(const unsigned short* text, std::size_t maxChars)
    {
        if (text == nullptr || maxChars == 0U)
        {
            return {};
        }

        std::wstring value;
        for (std::size_t index = 0U; index < maxChars && text[index] != 0U; ++index)
        {
            value.push_back(static_cast<wchar_t>(text[index]));
        }
        return value;
    }

    // copyWideToFixed writes a command-line string into a protocol wchar_t field.
    // Inputs: destination/capacity are the protocol field; source is user text.
    // Processing: truncates to leave room for NUL and clears the destination first.
    // Returns: no value; destination is always NUL terminated when capacity > 0.
    void copyWideToFixed(wchar_t* destination, std::size_t capacity, const std::wstring& source)
    {
        if (destination == nullptr || capacity == 0U)
        {
            return;
        }

        std::fill(destination, destination + capacity, L'\0');
        const std::size_t chars = std::min<std::size_t>(source.size(), capacity - 1U);
        if (chars != 0U)
        {
            std::copy(source.data(), source.data() + chars, destination);
        }
    }

    // parseU32 parses a decimal or 0x-prefixed unsigned 32-bit number.
    // Inputs: token is a command-line argument; name is used in error text.
    // Processing: wcstoull is used so both decimal and hex are accepted.
    // Returns: parsed value, or throws invalid_argument/out_of_range.
    std::uint32_t parseU32(const wchar_t* token, const char* name)
    {
        if (token == nullptr || token[0] == L'\0')
        {
            throw std::invalid_argument(name);
        }

        wchar_t* end = nullptr;
        const unsigned long long value = std::wcstoull(token, &end, 0);
        if (end == token || *end != L'\0')
        {
            throw std::invalid_argument(name);
        }
        if (value > std::numeric_limits<std::uint32_t>::max())
        {
            throw std::out_of_range(name);
        }
        return static_cast<std::uint32_t>(value);
    }

    // parseU64 parses a decimal or 0x-prefixed unsigned 64-bit number.
    // Inputs: token is a command-line argument; name is used in error text.
    // Processing: wcstoull is used to preserve native Windows argument encoding.
    // Returns: parsed value, or throws invalid_argument.
    std::uint64_t parseU64(const wchar_t* token, const char* name)
    {
        if (token == nullptr || token[0] == L'\0')
        {
            throw std::invalid_argument(name);
        }

        wchar_t* end = nullptr;
        const unsigned long long value = std::wcstoull(token, &end, 0);
        if (end == token || *end != L'\0')
        {
            throw std::invalid_argument(name);
        }
        return static_cast<std::uint64_t>(value);
    }

    // parsePidListText converts a delimiter-separated PID list into unique PID values.
    // Inputs: text accepts decimal or 0x-prefixed numbers separated by comma, semicolon, pipe, or whitespace.
    // Processing: PID zero is ignored and duplicates are removed while preserving first-seen order.
    // Returns: normalized PID vector; throws when a token is malformed or the protocol maximum is exceeded.
    std::vector<std::uint32_t> parsePidListText(const std::wstring& text)
    {
        std::vector<std::uint32_t> processIds;
        std::wstring token;

        const auto flushToken = [&]()
        {
            if (token.empty())
            {
                return;
            }

            const std::uint32_t processId = parseU32(token.c_str(), "pid list");
            token.clear();
            if (processId == 0U)
            {
                return;
            }
            if (std::find(processIds.begin(), processIds.end(), processId) != processIds.end())
            {
                return;
            }
            if (processIds.size() >= KSWORD_ARK_MINIFILTER_BYPASS_PID_MAX_COUNT)
            {
                throw std::out_of_range("too many pids");
            }
            processIds.push_back(processId);
        };

        for (const wchar_t ch : text)
        {
            if (ch == L',' || ch == L';' || ch == L'|' || std::iswspace(ch) != 0)
            {
                flushToken();
                continue;
            }
            token.push_back(ch);
        }
        flushToken();
        return processIds;
    }

    // hex64 formats a 64-bit integer as a fixed-width hexadecimal value.
    // Inputs: value is the integer to render.
    // Processing: uses an isolated stringstream to avoid mutating global cout state.
    // Returns: wide string in 0xNNNN form.
    std::wstring hex64(std::uint64_t value)
    {
        std::wostringstream stream;
        stream << L"0x" << std::hex << std::uppercase << value;
        return stream.str();
    }

    // hexdump prints raw bytes in a stable offset+ASCII layout.
    // Inputs: source bytes and an optional row width.
    // Processing: emits to stdout only; suitable for debugging variable payloads.
    // Returns: no value.
    void hexdump(const std::uint8_t* data, std::size_t size, std::size_t width)
    {
        if (data == nullptr || size == 0U)
        {
            std::wcout << L"(empty)\n";
            return;
        }

        for (std::size_t offset = 0U; offset < size; offset += width)
        {
            const std::size_t lineBytes = std::min(width, size - offset);
            std::wcout << std::hex << std::setw(8) << std::setfill(L'0') << offset << L": ";
            for (std::size_t index = 0U; index < width; ++index)
            {
                if (index < lineBytes)
                {
                    std::wcout << std::setw(2) << static_cast<unsigned int>(data[offset + index]) << L' ';
                }
                else
                {
                    std::wcout << L"   ";
                }
            }
            std::wcout << L"|";
            for (std::size_t index = 0U; index < lineBytes; ++index)
            {
                const unsigned char ch = data[offset + index];
                std::wcout << (ch >= 32U && ch < 127U ? static_cast<wchar_t>(ch) : L'.');
            }
            std::wcout << L"|\n" << std::dec << std::setfill(L' ');
        }
    }

    // loadBytesFromHexOrFile loads write payload bytes from --hex or --data-file.
    // Inputs: parsed args and the option names to consult.
    // Processing: enforces maxBytes and optional mandatory payload presence.
    // Returns: decoded bytes or throws on invalid/missing combinations.
    std::vector<std::uint8_t> loadBytesFromHexOrFile(
        const NamedArgs& args,
        const wchar_t* hexKey,
        const wchar_t* fileKey,
        std::size_t maxBytes,
        bool required)
    {
        const std::wstring* hexText = getOptionText(args, hexKey);
        const std::wstring* fileText = getOptionText(args, fileKey);
        if (hexText != nullptr && fileText != nullptr)
        {
            throw std::invalid_argument("mutually exclusive payload");
        }
        if (hexText == nullptr && fileText == nullptr)
        {
            if (required)
            {
                throw std::invalid_argument("payload");
            }
            return {};
        }

        std::vector<std::uint8_t> bytes =
            (hexText != nullptr) ? parseHexBytes(*hexText) : readFileBytes(*fileText, maxBytes);
        if (bytes.size() > maxBytes)
        {
            throw std::out_of_range("payload too large");
        }
        return bytes;
    }

    // sameToken compares an argv token with a metadata token.
    // Inputs: value may be null; expected is a static command token.
    // Processing: uses exact case-sensitive comparison to match dispatch behavior.
    // Returns: true when both tokens contain the same text.
    bool sameToken(const wchar_t* value, const wchar_t* expected)
    {
        return value != nullptr && expected != nullptr && std::wcscmp(value, expected) == 0;
    }

    // isHelpToken reports whether a token requests CLI help.
    // Inputs: raw argv token, possibly null.
    // Processing: accepts the same spellings that dispatch historically treated as help.
    // Returns: true for help, --help, -h, or /?.
    bool isHelpToken(const wchar_t* token)
    {
        return sameToken(token, L"help") ||
               sameToken(token, L"--help") ||
               sameToken(token, L"-h") ||
               sameToken(token, L"/?");
    }

    // containsIgnoreCase 作用：在 CLI 文本投影中做宽字符包含匹配。
    // 输入：haystack 为待搜索文本，needle 为目标片段。
    // 处理：逐字符转小写后比较，避免不同 R0 detail 大小写导致漏报。
    // 返回：命中返回 true；needle 为空时按“不过滤”返回 true。
    bool containsIgnoreCase(const std::wstring& haystack, const wchar_t* needle)
    {
        if (needle == nullptr || needle[0] == L'\0')
        {
            return true;
        }

        std::wstring loweredHaystack;
        loweredHaystack.reserve(haystack.size());
        for (const wchar_t ch : haystack)
        {
            loweredHaystack.push_back(static_cast<wchar_t>(std::towlower(ch)));
        }

        std::wstring loweredNeedle;
        for (const wchar_t* cursor = needle; *cursor != L'\0'; ++cursor)
        {
            loweredNeedle.push_back(static_cast<wchar_t>(std::towlower(*cursor)));
        }
        return loweredHaystack.find(loweredNeedle) != std::wstring::npos;
    }
}
