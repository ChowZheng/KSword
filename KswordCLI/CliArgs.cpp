#include "CliArgs.h"

#include "CliText.h"

#include <cwchar>
#include <stdexcept>

// 本文件实现 CliArgs.h 声明的参数解析一族，函数体逐字搬自 KswordCLI.cpp。
namespace ks::cli::legacy
{
    // parseNamedArgs collects positional tokens after the command family/subcommand.
    // Inputs: argc/argv plus the start index for first option token.
    // Processing: preserves ordering for positional file/blob paths and flags.
    // Returns: NamedArgs with a positional list; option helpers scan argv directly.
    NamedArgs parseNamedArgs(int argc, wchar_t* argv[], int startIndex)
    {
        NamedArgs args{};
        for (int index = startIndex; index < argc; ++index)
        {
            if (argv[index] == nullptr || argv[index][0] == L'\0')
            {
                continue;
            }

            if (argv[index][0] == L'-')
            {
                if (index + 1 < argc && argv[index + 1] != nullptr && argv[index + 1][0] != L'-')
                {
                    args.options[argv[index]] = argv[index + 1];
                    ++index;
                }
                else
                {
                    args.options[argv[index]] = L"";
                }
                continue;
            }

            args.positionals.emplace_back(argv[index]);
        }
        return args;
    }

    // getOptionText returns a named option value from parsed args.
    // Inputs: option map and key with leading dashes.
    // Processing: caller can use fallback when the option is absent.
    // Returns: pointer to the stable internal string or nullptr.
    const std::wstring* getOptionText(const NamedArgs& args, const wchar_t* key)
    {
        const auto it = args.options.find(key);
        return it == args.options.end() ? nullptr : &it->second;
    }

    // getOptionU32 parses an option value as unsigned 32-bit.
    // Inputs: parsed args, key, and default value.
    // Processing: falls back to default when the switch is absent.
    // Returns: parsed or default value.
    std::uint32_t getOptionU32(const NamedArgs& args, const wchar_t* key, std::uint32_t defaultValue)
    {
        const std::wstring* value = getOptionText(args, key);
        return value == nullptr ? defaultValue : parseU32(value->c_str(), "option");
    }

    // getOptionU64 parses an option value as unsigned 64-bit.
    // Inputs: parsed args, key, and default value.
    // Processing: falls back to default when the switch is absent.
    // Returns: parsed or default value.
    std::uint64_t getOptionU64(const NamedArgs& args, const wchar_t* key, std::uint64_t defaultValue)
    {
        const std::wstring* value = getOptionText(args, key);
        return value == nullptr ? defaultValue : parseU64(value->c_str(), "option");
    }

    // getOptionBool reports whether the option is present.
    // Inputs: parsed args and key.
    // Processing: presence-only flag; no value is consumed.
    // Returns: true when present.
    bool getOptionBool(const NamedArgs& args, const wchar_t* key)
    {
        return args.options.find(key) != args.options.end();
    }

    // requireOptionText fetches a mandatory named option.
    // Inputs: parsed args and key.
    // Processing: throws when the switch is absent.
    // Returns: option value reference.
    const std::wstring& requireOptionText(const NamedArgs& args, const wchar_t* key)
    {
        const std::wstring* value = getOptionText(args, key);
        std::string keyText;
        for (const wchar_t* cursor = key; *cursor; ++cursor) keyText.push_back(static_cast<char>(*cursor));
        if (value == nullptr)
        {
            throw std::invalid_argument("missing option " + keyText);
        }
        if (value->empty())
            throw std::invalid_argument("missing value for option " + keyText);
        return *value;
    }

    // requireOptionU32 parses a mandatory unsigned 32-bit option.
    // Inputs: parsed args and key.
    // Processing: throws when the switch is absent or malformed.
    // Returns: parsed value.
    std::uint32_t requireOptionU32(const NamedArgs& args, const wchar_t* key)
    {
        return parseU32(requireOptionText(args, key).c_str(), "option");
    }

    // requireOptionU64 parses a mandatory unsigned 64-bit option.
    // Inputs: parsed args and key.
    // Processing: throws when the switch is absent or malformed.
    // Returns: parsed value.
    std::uint64_t requireOptionU64(const NamedArgs& args, const wchar_t* key)
    {
        return parseU64(requireOptionText(args, key).c_str(), "option");
    }

    // hasOption reports whether a named switch is present in argv.
    // Inputs: option name includes the leading dashes, for example "--pid".
    // Processing: exact case-sensitive match against the command line.
    // Returns: true when the option exists.
    bool hasOption(int argc, wchar_t* argv[], const wchar_t* option)
    {
        for (int index = 0; index < argc; ++index)
        {
            if (argv[index] != nullptr && std::wcscmp(argv[index], option) == 0)
            {
                return true;
            }
        }
        return false;
    }

    // findOptionValue returns the next token after a named switch.
    // Inputs: argv and option name; if missing, returns nullptr.
    // Processing: scans for exact option match and returns following token.
    // Returns: pointer to the following argument or nullptr.
    const wchar_t* findOptionValue(int argc, wchar_t* argv[], const wchar_t* option)
    {
        for (int index = 0; index + 1 < argc; ++index)
        {
            if (argv[index] != nullptr && std::wcscmp(argv[index], option) == 0)
            {
                return argv[index + 1];
            }
        }
        return nullptr;
    }
}
