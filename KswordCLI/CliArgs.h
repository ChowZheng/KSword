#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// 旧版 CLI 参数解析：命名开关的收集与取值辅助。
// 输入：wmain 透传的 argc/argv，以及命令行中出现的开关名（含前导横线）。
// 处理：按符号分隔位置参数与开关值，不做语义校验，也不接触任何 IOCTL 协议结构。
// 返回：无行为；定义见 CliArgs.cpp。
//
// 本文件收的是纯参数解析一族：NamedArgs、parseNamedArgs、getOptionText/U32/U64/Bool、
// requireOptionText/U32/U64、hasOption、findOptionValue。它们无状态、无副作用（除按契约抛异常），
// 因此从 KswordCLI.cpp 抽出时调用点无需改动。
namespace ks::cli::legacy
{
    struct NamedArgs
    {
        std::vector<std::wstring> positionals;
        std::map<std::wstring, std::wstring> options;
    };

    // parseNamedArgs collects positional tokens after the command family/subcommand.
    // Inputs: argc/argv plus the start index for first option token.
    // Processing: preserves ordering for positional file/blob paths and flags.
    // Returns: NamedArgs with a positional list; option helpers scan argv directly.
    NamedArgs parseNamedArgs(int argc, wchar_t* argv[], int startIndex);

    // getOptionText returns a named option value from parsed args.
    // Inputs: option map and key with leading dashes.
    // Processing: caller can use fallback when the option is absent.
    // Returns: pointer to the stable internal string or nullptr.
    const std::wstring* getOptionText(const NamedArgs& args, const wchar_t* key);

    // getOptionU32 parses an option value as unsigned 32-bit.
    // Inputs: parsed args, key, and default value.
    // Processing: falls back to default when the switch is absent.
    // Returns: parsed or default value.
    std::uint32_t getOptionU32(const NamedArgs& args, const wchar_t* key, std::uint32_t defaultValue);

    // getOptionU64 parses an option value as unsigned 64-bit.
    // Inputs: parsed args, key, and default value.
    // Processing: falls back to default when the switch is absent.
    // Returns: parsed or default value.
    std::uint64_t getOptionU64(const NamedArgs& args, const wchar_t* key, std::uint64_t defaultValue);

    // getOptionBool reports whether the option is present.
    // Inputs: parsed args and key.
    // Processing: presence-only flag; no value is consumed.
    // Returns: true when present.
    bool getOptionBool(const NamedArgs& args, const wchar_t* key);

    // requireOptionText fetches a mandatory named option.
    // Inputs: parsed args and key.
    // Processing: throws when the switch is absent.
    // Returns: option value reference.
    const std::wstring& requireOptionText(const NamedArgs& args, const wchar_t* key);

    // requireOptionU32 parses a mandatory unsigned 32-bit option.
    // Inputs: parsed args and key.
    // Processing: throws when the switch is absent or malformed.
    // Returns: parsed value.
    std::uint32_t requireOptionU32(const NamedArgs& args, const wchar_t* key);

    // requireOptionU64 parses a mandatory unsigned 64-bit option.
    // Inputs: parsed args and key.
    // Processing: throws when the switch is absent or malformed.
    // Returns: parsed value.
    std::uint64_t requireOptionU64(const NamedArgs& args, const wchar_t* key);

    // hasOption reports whether a named switch is present in argv.
    // Inputs: option name includes the leading dashes, for example "--pid".
    // Processing: exact case-sensitive match against the command line.
    // Returns: true when the option exists.
    bool hasOption(int argc, wchar_t* argv[], const wchar_t* option);

    // findOptionValue returns the next token after a named switch.
    // Inputs: argv and option name; if missing, returns nullptr.
    // Processing: scans for exact option match and returns following token.
    // Returns: pointer to the following argument or nullptr.
    const wchar_t* findOptionValue(int argc, wchar_t* argv[], const wchar_t* option);
}
