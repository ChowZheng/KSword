#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "CliArgs.h"

// 旧版 CLI 文本与字节工具：定长协议字段解码、十六进制解析、数字解析与调试打印。
// 输入：命令行文本、文件路径，以及固定缓冲区指针与容量。
// 处理：只做字符串/字节层面的转换与格式校验，不读写任何 KSWORD_ARK_* 协议包的其余字段。
// 返回：无行为；定义见 CliText.cpp。
//
// 本文件收的是纯转换一族：readFileBytes、parseHexBytes、loadBytesFromHexOrFile、
// fixedAnsi、fixedWide、fixedUtf16、copyWideToFixed、parseU32、parseU64、parsePidListText、
// hex64、hexdump、sameToken、isHelpToken、containsIgnoreCase。
// 输出风格类辅助（dumpWideText、printResponseBanner 等）故意留在 KswordCLI.cpp。
namespace ks::cli::legacy
{
    // readFileBytes loads a whole file into memory for blob-style IOCTL inputs.
    // Inputs: path and an upper size bound to reject oversized protocol blobs.
    // Processing: opens the file in binary mode and validates the actual length.
    // Returns: byte vector on success; throws on IO or size violations.
    std::vector<std::uint8_t> readFileBytes(const std::wstring& path, std::size_t maxBytes);

    // parseHexBytes accepts either 0x-prefixed numbers or raw hex byte strings.
    // Inputs: text token containing bytes like "DE AD BE EF" or "0x1234".
    // Processing: strips separators and decodes two hex digits per byte.
    // Returns: decoded bytes; throws on malformed input.
    std::vector<std::uint8_t> parseHexBytes(const std::wstring& text);

    // fixedAnsi copies a fixed char array into std::string safely.
    // Inputs: text points at protocol storage; maxBytes is the field capacity.
    // Processing: scans until NUL or capacity, whichever comes first.
    // Returns: a possibly empty string without embedded protocol padding.
    std::string fixedAnsi(const char* text, std::size_t maxBytes);

    // fixedWide copies a fixed wchar_t array into std::wstring safely.
    // Inputs: text points at protocol storage; maxChars is the field capacity.
    // Processing: scans until NUL or capacity, whichever comes first.
    // Returns: a possibly empty wide string without trailing padding.
    std::wstring fixedWide(const wchar_t* text, std::size_t maxChars);

    // fixedUtf16 copies unsigned-short UTF-16 protocol fields into std::wstring.
    // Inputs: text points at UTF-16 code units; maxChars bounds the scan.
    // Processing: casts code units to wchar_t, matching Windows UTF-16 wchar_t.
    // Returns: a std::wstring suitable for console output on Windows.
    std::wstring fixedUtf16(const unsigned short* text, std::size_t maxChars);

    // copyWideToFixed writes a command-line string into a protocol wchar_t field.
    // Inputs: destination/capacity are the protocol field; source is user text.
    // Processing: truncates to leave room for NUL and clears the destination first.
    // Returns: no value; destination is always NUL terminated when capacity > 0.
    void copyWideToFixed(wchar_t* destination, std::size_t capacity, const std::wstring& source);

    // parseU32 parses a decimal or 0x-prefixed unsigned 32-bit number.
    // Inputs: token is a command-line argument; name is used in error text.
    // Processing: wcstoull is used so both decimal and hex are accepted.
    // Returns: parsed value, or throws invalid_argument/out_of_range.
    std::uint32_t parseU32(const wchar_t* token, const char* name);

    // parseU64 parses a decimal or 0x-prefixed unsigned 64-bit number.
    // Inputs: token is a command-line argument; name is used in error text.
    // Processing: wcstoull is used to preserve native Windows argument encoding.
    // Returns: parsed value, or throws invalid_argument.
    std::uint64_t parseU64(const wchar_t* token, const char* name);

    // parsePidListText converts a delimiter-separated PID list into unique PID values.
    // Inputs: text accepts decimal or 0x-prefixed numbers separated by comma, semicolon, pipe, or whitespace.
    // Processing: PID zero is ignored and duplicates are removed while preserving first-seen order.
    // Returns: normalized PID vector; throws when a token is malformed or the protocol maximum is exceeded.
    std::vector<std::uint32_t> parsePidListText(const std::wstring& text);

    // hex64 formats a 64-bit integer as a fixed-width hexadecimal value.
    // Inputs: value is the integer to render.
    // Processing: uses an isolated stringstream to avoid mutating global cout state.
    // Returns: wide string in 0xNNNN form.
    std::wstring hex64(std::uint64_t value);

    // hexdump prints raw bytes in a stable offset+ASCII layout.
    // Inputs: source bytes and an optional row width.
    // Processing: emits to stdout only; suitable for debugging variable payloads.
    // Returns: no value.
    void hexdump(const std::uint8_t* data, std::size_t size, std::size_t width = 16U);

    // loadBytesFromHexOrFile loads write payload bytes from --hex or --data-file.
    // Inputs: parsed args and the option names to consult.
    // Processing: enforces maxBytes and optional mandatory payload presence.
    // Returns: decoded bytes or throws on invalid/missing combinations.
    std::vector<std::uint8_t> loadBytesFromHexOrFile(
        const NamedArgs& args,
        const wchar_t* hexKey,
        const wchar_t* fileKey,
        std::size_t maxBytes,
        bool required);

    // sameToken compares an argv token with a metadata token.
    // Inputs: value may be null; expected is a static command token.
    // Processing: uses exact case-sensitive comparison to match dispatch behavior.
    // Returns: true when both tokens contain the same text.
    bool sameToken(const wchar_t* value, const wchar_t* expected);

    // isHelpToken reports whether a token requests CLI help.
    // Inputs: raw argv token, possibly null.
    // Processing: accepts the same spellings that dispatch historically treated as help.
    // Returns: true for help, --help, -h, or /?.
    bool isHelpToken(const wchar_t* token);

    // containsIgnoreCase 作用：在 CLI 文本投影中做宽字符包含匹配。
    // 输入：haystack 为待搜索文本，needle 为目标片段。
    // 处理：逐字符转小写后比较，避免不同 R0 detail 大小写导致漏报。
    // 返回：命中返回 true；needle 为空时按“不过滤”返回 true。
    bool containsIgnoreCase(const std::wstring& haystack, const wchar_t* needle);
}
