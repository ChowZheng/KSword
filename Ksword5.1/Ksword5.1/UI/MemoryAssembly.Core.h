#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ks::ui::detail
{
    enum class AssemblyError
    {
        None,
        EmptySource,
        SourceTooLarge,
        InvalidLabel,
        DuplicateLabel,
        UnknownLabel,
        UnknownMnemonic,
        InvalidOperands,
        InvalidNumber,
        InvalidMemory,
        AmbiguousMemorySize,
        InvalidInstruction,
        AddressOverflow,
        OutputTooLarge,
        UnstableLabels,
        UnsupportedSyntax
    };

    struct IntelAssemblyResult
    {
        std::vector<std::uint8_t> bytes;
        AssemblyError error = AssemblyError::None;
        std::string detail;
        int errorLine = 0;
        bool success = false;
    };

    // A bounded Intel instruction parser backed by the bundled Zydis encoder.
    // Numbers are hexadecimal by default (CE style); 0x/h also mean hexadecimal,
    // and 0d explicitly selects decimal. Defined labels take precedence.
    // Branch operands are absolute VAs. [rip+disp] is a raw displacement;
    // [rel target] and unqualified x64 [target] are RIP-relative absolute VAs.
    // [abs target] and segment overrides retain absolute memory addressing.
    // Labels and ; comments are supported; assembler scripts/macros are not.
    IntelAssemblyResult assembleIntel(
        const std::string& source,
        std::uint64_t baseAddress,
        bool x64);
}
