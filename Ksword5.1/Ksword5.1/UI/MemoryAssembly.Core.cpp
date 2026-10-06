#include "MemoryAssembly.Core.h"

#include <Zydis.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstring>
#include <limits>
#include <map>
#include <string_view>
#include <utility>

namespace ks::ui::detail
{
namespace
{
    constexpr std::size_t kMaximumSource = 64U * 1024U;
    constexpr std::size_t kMaximumLines = 4096U;
    constexpr std::size_t kMaximumOutput = 64U * 1024U;
    using Symbols = std::map<std::string, std::uint64_t>;

    struct Failure
    {
        AssemblyError code = AssemblyError::None;
        std::string detail;
        bool set(AssemblyError error, std::string token = {})
        {
            code = error;
            detail = std::move(token);
            return false;
        }
    };

    std::string trim(std::string text)
    {
        const auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
        const auto start = std::find_if_not(text.begin(), text.end(), isSpace);
        const auto end = std::find_if_not(text.rbegin(), text.rend(), isSpace).base();
        return start < end ? std::string(start, end) : std::string();
    }

    std::string lower(std::string text)
    {
        for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    }

    bool identifier(const std::string& text)
    {
        if (text.empty() || !(std::isalpha(static_cast<unsigned char>(text.front()))
            || text.front() == '_')) return false;
        return std::all_of(text.begin(), text.end(), [](unsigned char c) {
            return std::isalnum(c) != 0 || c == '_';
        });
    }

    const std::map<std::string, ZydisRegister>& registers()
    {
        static const auto table = [] {
            std::map<std::string, ZydisRegister> result;
            for (int i = 1; i <= ZYDIS_REGISTER_MAX_VALUE; ++i)
            {
                const auto reg = static_cast<ZydisRegister>(i);
                if (const auto* name = ZydisRegisterGetString(reg)) result.emplace(name, reg);
            }
            for (int i = 0; i < 8; ++i)
                result.emplace("st(" + std::to_string(i) + ")",
                    static_cast<ZydisRegister>(ZYDIS_REGISTER_ST0 + i));
            return result;
        }();
        return table;
    }

    ZydisRegister findRegister(const std::string& text)
    {
        const auto found = registers().find(text);
        return found == registers().end() ? ZYDIS_REGISTER_NONE : found->second;
    }

    const std::map<std::string, ZydisMnemonic>& mnemonics()
    {
        static const auto table = [] {
            std::map<std::string, ZydisMnemonic> result;
            for (int i = 1; i <= ZYDIS_MNEMONIC_MAX_VALUE; ++i)
            {
                const auto mnemonic = static_cast<ZydisMnemonic>(i);
                if (const auto* name = ZydisMnemonicGetString(mnemonic)) result.emplace(name, mnemonic);
            }
            const std::pair<const char*, const char*> aliases[] = {
                {"je", "jz"}, {"jne", "jnz"}, {"jc", "jb"}, {"jnae", "jb"},
                {"jnc", "jnb"}, {"jae", "jnb"}, {"jna", "jbe"}, {"jnbe", "ja"},
                {"jpe", "jp"}, {"jpo", "jnp"}, {"jnge", "jl"}, {"jge", "jnl"},
                {"jng", "jle"}, {"jg", "jnle"}, {"sal", "shl"}, {"retn", "ret"},
                {"loopz", "loope"}, {"loopnz", "loopne"}, {"wait", "fwait"}, {"xlatb", "xlat"}
            };
            for (const auto& alias : aliases)
            {
                const auto found = result.find(alias.second);
                if (found != result.end()) result.emplace(alias.first, found->second);
            }
            const std::pair<const char*, const char*> conditions[] = {
                {"e", "z"}, {"ne", "nz"}, {"c", "b"}, {"nae", "b"},
                {"nc", "nb"}, {"ae", "nb"}, {"na", "be"}, {"a", "nbe"},
                {"pe", "p"}, {"po", "np"}, {"nge", "l"}, {"ge", "nl"},
                {"ng", "le"}, {"g", "nle"}
            };
            for (const auto* prefix : {"cmov", "set"})
                for (const auto& condition : conditions)
                {
                    const auto found = result.find(std::string(prefix) + condition.second);
                    if (found != result.end()) result.emplace(std::string(prefix) + condition.first, found->second);
                }
            return result;
        }();
        return table;
    }

    struct Magnitude
    {
        std::uint64_t value = 0;
        bool negative = false;
    };

    bool addMagnitude(Magnitude& accumulator, Magnitude term, Failure& failure)
    {
        if (accumulator.negative == term.negative)
        {
            if (term.value > std::numeric_limits<std::uint64_t>::max() - accumulator.value)
                return failure.set(AssemblyError::InvalidNumber);
            accumulator.value += term.value;
        }
        else if (accumulator.value >= term.value) accumulator.value -= term.value;
        else
        {
            accumulator.value = term.value - accumulator.value;
            accumulator.negative = term.negative;
        }
        if (!accumulator.value) accumulator.negative = false;
        return true;
    }

    bool numericAtom(const std::string& text, std::uint64_t& value)
    {
        if (text.empty()) return false;
        std::string_view digits(text);
        int base = 16;
        if (digits.size() > 2 && digits.substr(0, 2) == "0x") { digits.remove_prefix(2); base = 16; }
        else if (digits.size() > 2 && digits.substr(0, 2) == "0d") { digits.remove_prefix(2); base = 10; }
        else if (digits.size() > 1 && digits.back() == 'h') { digits.remove_suffix(1); base = 16; }
        if (digits.empty()) return false;
        const auto converted = std::from_chars(digits.data(), digits.data() + digits.size(), value, base);
        return converted.ec == std::errc() && converted.ptr == digits.data() + digits.size();
    }

    // Split additive expressions without accepting double signs or trailing garbage.
    bool terms(const std::string& expression, std::vector<std::pair<bool, std::string>>& output,
        Failure& failure)
    {
        // compact 保留运算符结构；空白只能出现在词法单元之间，不能拼接数字或标识符。
        std::string compact;
        bool separated = false;
        const auto isOperator = [](char character) {
            return character == '+' || character == '-' || character == '*';
        };
        for (const auto character : expression)
        {
            if (std::isspace(static_cast<unsigned char>(character)))
            {
                separated = true;
                continue;
            }
            // separated 表示刚跨过空白：两个相邻原子必须由显式运算符分隔。
            if (separated && !compact.empty()
                && !isOperator(compact.back()) && !isOperator(character))
            {
                return failure.set(AssemblyError::InvalidOperands, expression);
            }
            compact.push_back(character);
            separated = false;
        }
        if (compact.empty()) return failure.set(AssemblyError::InvalidOperands);
        std::size_t pos = 0;
        while (pos < compact.size())
        {
            bool negative = false;
            if (compact[pos] == '+' || compact[pos] == '-') negative = compact[pos++] == '-';
            const auto begin = pos;
            while (pos < compact.size() && compact[pos] != '+' && compact[pos] != '-') ++pos;
            if (begin == pos) return failure.set(AssemblyError::InvalidOperands, expression);
            output.emplace_back(negative, compact.substr(begin, pos - begin));
        }
        return true;
    }

    bool constant(const std::string& expression, const Symbols& symbols, bool estimating,
        std::uint64_t runtimeAddress, std::uint64_t& value, bool& usesLabel, Failure& failure)
    {
        std::vector<std::pair<bool, std::string>> parts;
        if (!terms(expression, parts, failure)) return false;
        Magnitude total;
        for (const auto& part : parts)
        {
            Magnitude term;
            term.negative = part.first;
            const auto found = symbols.find(part.second);
            if (found != symbols.end())
            {
                usesLabel = true;
                term.value = estimating ? runtimeAddress : found->second;
            }
            else if (!numericAtom(part.second, term.value))
            {
                if (identifier(part.second)) return failure.set(AssemblyError::UnknownLabel, part.second);
                return failure.set(AssemblyError::InvalidNumber, part.second);
            }
            if (!addMagnitude(total, term, failure)) return false;
        }
        if (total.negative && total.value > (std::uint64_t{1} << 63))
            return failure.set(AssemblyError::InvalidNumber, expression);
        value = total.negative ? std::uint64_t{0} - total.value : total.value;
        return true;
    }

    std::int64_t signedBits(std::uint64_t value)
    {
        std::int64_t result;
        std::memcpy(&result, &value, sizeof(result));
        return result;
    }

    ZydisInstructionAttributes segmentPrefix(const std::string& text)
    {
        if (text == "cs") return ZYDIS_ATTRIB_HAS_SEGMENT_CS;
        if (text == "ss") return ZYDIS_ATTRIB_HAS_SEGMENT_SS;
        if (text == "ds") return ZYDIS_ATTRIB_HAS_SEGMENT_DS;
        if (text == "es") return ZYDIS_ATTRIB_HAS_SEGMENT_ES;
        if (text == "fs") return ZYDIS_ATTRIB_HAS_SEGMENT_FS;
        if (text == "gs") return ZYDIS_ATTRIB_HAS_SEGMENT_GS;
        return 0;
    }

    struct ParsedRequest
    {
        ZydisEncoderRequest request{};
        int relativeMemory = -1;
        std::uint64_t rawDisplacement = 0;
        std::vector<int> unsizedMemory;
    };

    bool memoryOperand(std::string text, ZydisEncoderOperand& operand,
        ParsedRequest& parsed, int operandIndex, const Symbols& symbols,
        bool estimating, std::uint64_t runtimeAddress, Failure& failure)
    {
        const auto open = text.find('[');
        if (open == std::string::npos || text.back() != ']'
            || text.find('[', open + 1) != std::string::npos
            || text.find(']') != text.size() - 1)
            return failure.set(AssemblyError::InvalidMemory, text);
        auto prefix = trim(text.substr(0, open));
        auto expression = trim(text.substr(open + 1, text.size() - open - 2));
        ZydisInstructionAttributes segment = 0;
        const auto colon = prefix.find(':');
        if (colon != std::string::npos)
        {
            if (colon != prefix.size() - 1) return failure.set(AssemblyError::InvalidMemory, prefix);
            auto preceding = trim(prefix.substr(0, colon));
            const auto separator = preceding.find_last_of(" \t");
            auto name = separator == std::string::npos ? preceding : preceding.substr(separator + 1);
            segment = segmentPrefix(name);
            if (!segment) return failure.set(AssemblyError::InvalidMemory, name);
            prefix = separator == std::string::npos ? std::string() : trim(preceding.substr(0, separator));
        }
        if (prefix.size() > 4 && prefix.substr(prefix.size() - 4) == " ptr")
            prefix = trim(prefix.substr(0, prefix.size() - 4));
        const std::map<std::string, ZyanU16> sizes = {
            {"byte", 1}, {"word", 2}, {"dword", 4}, {"qword", 8}, {"tbyte", 10},
            {"xmmword", 16}, {"oword", 16}, {"ymmword", 32}, {"zmmword", 64}
        };
        operand.type = ZYDIS_OPERAND_TYPE_MEMORY;
        if (!prefix.empty())
        {
            const auto found = sizes.find(prefix);
            if (found == sizes.end()) return failure.set(AssemblyError::InvalidMemory, prefix);
            operand.mem.size = found->second;
        }
        else parsed.unsizedMemory.push_back(operandIndex);
        if (segment)
        {
            if ((parsed.request.prefixes & ZYDIS_ATTRIB_HAS_SEGMENT)
                && !(parsed.request.prefixes & segment))
                return failure.set(AssemblyError::InvalidMemory, text);
            parsed.request.prefixes |= segment;
        }
        bool forceRelative = false;
        bool forceAbsolute = false;
        if (expression.rfind("rel ", 0) == 0) { forceRelative = true; expression = trim(expression.substr(4)); }
        else if (expression.rfind("abs ", 0) == 0) { forceAbsolute = true; expression = trim(expression.substr(4)); }
        std::vector<std::pair<bool, std::string>> parts;
        if (!terms(expression, parts, failure)) return false;
        std::string displacement;
        for (const auto& part : parts)
        {
            const auto star = part.second.find('*');
            auto reg = findRegister(part.second);
            std::uint64_t scale = 1;
            bool scaled = star != std::string::npos;
            if (scaled)
            {
                reg = findRegister(part.second.substr(0, star));
                if (reg == ZYDIS_REGISTER_NONE
                    || !numericAtom(part.second.substr(star + 1), scale)
                    || !(scale == 1 || scale == 2 || scale == 4 || scale == 8))
                    return failure.set(AssemblyError::InvalidMemory, part.second);
            }
            if (reg != ZYDIS_REGISTER_NONE)
            {
                const auto kind = ZydisRegisterGetClass(reg);
                if (part.first || !(kind == ZYDIS_REGCLASS_GPR16 || kind == ZYDIS_REGCLASS_GPR32
                    || kind == ZYDIS_REGCLASS_GPR64 || reg == ZYDIS_REGISTER_RIP || reg == ZYDIS_REGISTER_EIP))
                    return failure.set(AssemblyError::InvalidMemory, part.second);
                if (!scaled && operand.mem.base == ZYDIS_REGISTER_NONE) operand.mem.base = reg;
                else if (operand.mem.index == ZYDIS_REGISTER_NONE
                    && reg != ZYDIS_REGISTER_RIP && reg != ZYDIS_REGISTER_EIP)
                {
                    operand.mem.index = reg;
                    operand.mem.scale = static_cast<ZyanU8>(scale);
                }
                else return failure.set(AssemblyError::InvalidMemory, part.second);
            }
            else
            {
                if (!displacement.empty() || part.first) displacement += part.first ? '-' : '+';
                displacement += part.second;
            }
        }
        bool usesLabel = false;
        std::uint64_t numericDisplacement = 0;
        if (!displacement.empty()
            && !constant(displacement, symbols, estimating, runtimeAddress, numericDisplacement, usesLabel, failure))
            return false;
        const bool hasRegisters = operand.mem.base != ZYDIS_REGISTER_NONE
            || operand.mem.index != ZYDIS_REGISTER_NONE;
        if ((operand.mem.index == ZYDIS_REGISTER_RSP || operand.mem.index == ZYDIS_REGISTER_ESP)
            && operand.mem.scale == 1 && operand.mem.base != ZYDIS_REGISTER_NONE
            && operand.mem.base != ZYDIS_REGISTER_RIP && operand.mem.base != ZYDIS_REGISTER_EIP)
            std::swap(operand.mem.base, operand.mem.index);
        const bool x64 = parsed.request.machine_mode == ZYDIS_MACHINE_MODE_LONG_64;
        if ((forceRelative || forceAbsolute) && hasRegisters)
            return failure.set(AssemblyError::InvalidMemory, text);
        if (forceRelative && (!x64 || segment)) return failure.set(AssemblyError::InvalidMemory, text);
        if (operand.mem.base == ZYDIS_REGISTER_RIP || operand.mem.base == ZYDIS_REGISTER_EIP)
        {
            if (operand.mem.index != ZYDIS_REGISTER_NONE || !x64
                || numericDisplacement != static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(numericDisplacement))))
                return failure.set(AssemblyError::InvalidMemory, text);
            parsed.relativeMemory = operandIndex;
            parsed.rawDisplacement = numericDisplacement;
        }
        else if (!hasRegisters && x64 && !forceAbsolute && !segment)
            operand.mem.base = ZYDIS_REGISTER_RIP;
        else if (!hasRegisters && !x64 && numericDisplacement > std::numeric_limits<std::uint32_t>::max())
            return failure.set(AssemblyError::AddressOverflow, text);
        if (!hasRegisters && operand.mem.base == ZYDIS_REGISTER_NONE
            && numericDisplacement <= std::numeric_limits<std::uint32_t>::max())
        {
            // Zydis stores absolute disp32 values as sign-extended integers.
            // The hint retains the intended 32-bit address, including its high bit.
            parsed.request.address_size_hint = ZYDIS_ADDRESS_SIZE_HINT_32;
            const auto low = static_cast<std::uint32_t>(numericDisplacement);
            std::int32_t signedLow;
            std::memcpy(&signedLow, &low, sizeof(signedLow));
            numericDisplacement = static_cast<std::uint64_t>(static_cast<std::int64_t>(signedLow));
        }
        operand.mem.displacement = signedBits(numericDisplacement);
        return true;
    }

    bool parseInstruction(std::string text, ParsedRequest& parsed, const Symbols& symbols,
        bool estimating, std::uint64_t runtimeAddress, bool x64, Failure& failure)
    {
        parsed.request.machine_mode = x64 ? ZYDIS_MACHINE_MODE_LONG_64 : ZYDIS_MACHINE_MODE_LEGACY_32;
        const std::map<std::string, ZydisInstructionAttributes> prefixes = {
            {"lock", ZYDIS_ATTRIB_HAS_LOCK}, {"rep", ZYDIS_ATTRIB_HAS_REP},
            {"repe", ZYDIS_ATTRIB_HAS_REPE}, {"repz", ZYDIS_ATTRIB_HAS_REPE},
            {"repne", ZYDIS_ATTRIB_HAS_REPNE}, {"repnz", ZYDIS_ATTRIB_HAS_REPNE},
            {"bnd", ZYDIS_ATTRIB_HAS_BND}, {"xacquire", ZYDIS_ATTRIB_HAS_XACQUIRE},
            {"xrelease", ZYDIS_ATTRIB_HAS_XRELEASE}, {"notrack", ZYDIS_ATTRIB_HAS_NOTRACK}
        };
        std::string mnemonic;
        for (;;)
        {
            const auto end = text.find_first_of(" \t");
            mnemonic = text.substr(0, end);
            text = end == std::string::npos ? std::string() : trim(text.substr(end + 1));
            const auto prefix = prefixes.find(mnemonic);
            if (prefix == prefixes.end()) break;
            if (parsed.request.prefixes & prefix->second)
                return failure.set(AssemblyError::InvalidOperands, mnemonic);
            parsed.request.prefixes |= prefix->second;
        }
        const auto found = mnemonics().find(mnemonic);
        if (found == mnemonics().end()) return failure.set(AssemblyError::UnknownMnemonic, mnemonic);
        parsed.request.mnemonic = found->second;
        std::vector<std::string> operands;
        if (!text.empty())
        {
            std::size_t start = 0;
            int depth = 0;
            for (std::size_t i = 0; i <= text.size(); ++i)
            {
                if (i < text.size() && text[i] == '[') ++depth;
                if (i < text.size() && text[i] == ']') --depth;
                if (depth < 0 || depth > 1) return failure.set(AssemblyError::InvalidMemory, text);
                if (i == text.size() || (text[i] == ',' && depth == 0))
                {
                    auto token = trim(text.substr(start, i - start));
                    if (token.empty() || operands.size() == ZYDIS_ENCODER_MAX_OPERANDS)
                        return failure.set(AssemblyError::InvalidOperands, text);
                    operands.push_back(std::move(token));
                    start = i + 1;
                }
            }
            if (depth) return failure.set(AssemblyError::InvalidMemory, text);
        }
        parsed.request.operand_count = static_cast<ZyanU8>(operands.size());
        for (std::size_t i = 0; i < operands.size(); ++i)
        {
            auto token = operands[i];
            auto& operand = parsed.request.operands[i];
            if (token.rfind("short ", 0) == 0 || token.rfind("near ", 0) == 0)
            {
                if (i) return failure.set(AssemblyError::InvalidOperands, token);
                const bool shortBranch = token.rfind("short ", 0) == 0;
                parsed.request.branch_type = shortBranch ? ZYDIS_BRANCH_TYPE_SHORT : ZYDIS_BRANCH_TYPE_NEAR;
                parsed.request.branch_width = shortBranch ? ZYDIS_BRANCH_WIDTH_8 : ZYDIS_BRANCH_WIDTH_32;
                token = trim(token.substr(shortBranch ? 6 : 5));
            }
            const auto reg = findRegister(token);
            if (reg != ZYDIS_REGISTER_NONE)
            {
                operand.type = ZYDIS_OPERAND_TYPE_REGISTER;
                operand.reg.value = reg;
            }
            else if (token.find('[') != std::string::npos)
            {
                if (!memoryOperand(token, operand, parsed, static_cast<int>(i), symbols,
                    estimating, runtimeAddress, failure)) return false;
            }
            else
            {
                if (token.find_first_of("{}:$\"'") != std::string::npos)
                    return failure.set(AssemblyError::UnsupportedSyntax, token);
                operand.type = ZYDIS_OPERAND_TYPE_IMMEDIATE;
                bool usesLabel = false;
                if (!constant(token, symbols, estimating, runtimeAddress, operand.imm.u, usesLabel, failure))
                    return false;
            }
        }
        // Do not accidentally choose an indirect far transfer from ambiguous memory sizes.
        if ((parsed.request.mnemonic == ZYDIS_MNEMONIC_CALL || parsed.request.mnemonic == ZYDIS_MNEMONIC_JMP)
            && parsed.request.operand_count == 1
            && parsed.request.operands[0].type != ZYDIS_OPERAND_TYPE_IMMEDIATE
            && parsed.request.branch_type == ZYDIS_BRANCH_TYPE_NONE)
            parsed.request.branch_type = ZYDIS_BRANCH_TYPE_NEAR;
        // Intel formatting omits the default width for stack operands and near
        // indirect transfers. Preserve that implicit architectural width when
        // users copy those instructions back into the assembly editor.
        if ((parsed.request.mnemonic == ZYDIS_MNEMONIC_CALL
            || parsed.request.mnemonic == ZYDIS_MNEMONIC_JMP
            || parsed.request.mnemonic == ZYDIS_MNEMONIC_PUSH
            || parsed.request.mnemonic == ZYDIS_MNEMONIC_POP)
            && parsed.request.operand_count == 1
            && parsed.request.operands[0].type == ZYDIS_OPERAND_TYPE_MEMORY
            && parsed.request.operands[0].mem.size == 0)
        {
            parsed.request.operands[0].mem.size = x64 ? 8 : 4;
            parsed.unsizedMemory.clear();
        }
        return true;
    }

    bool encodeRequest(ParsedRequest parsed, std::uint64_t runtimeAddress,
        std::vector<std::uint8_t>& bytes)
    {
        std::array<std::uint8_t, ZYDIS_MAX_INSTRUCTION_LENGTH> buffer{};
        ZyanUSize length = buffer.size();
        auto request = parsed.request;
        if (request.machine_mode == ZYDIS_MACHINE_MODE_LEGACY_32)
        {
            const std::string mnemonic = ZydisMnemonicGetString(request.mnemonic);
            const bool relativeBranch = mnemonic.front() == 'j' || mnemonic.rfind("loop", 0) == 0
                || request.mnemonic == ZYDIS_MNEMONIC_CALL || request.mnemonic == ZYDIS_MNEMONIC_XBEGIN;
            if (relativeBranch)
            {
                for (ZyanU8 i = 0; i < request.operand_count; ++i)
                {
                    auto& operand = request.operands[i];
                    if (operand.type != ZYDIS_OPERAND_TYPE_IMMEDIATE) continue;
                    if (operand.imm.u > std::numeric_limits<std::uint32_t>::max()) return false;
                    // The x86 instruction pointer wraps at 32 bits. Present an
                    // equivalent nearby 64-bit VA to the absolute-address encoder.
                    const auto lowDelta = static_cast<std::uint32_t>(operand.imm.u - runtimeAddress);
                    std::int32_t signedDelta;
                    std::memcpy(&signedDelta, &lowDelta, sizeof(signedDelta));
                    operand.imm.u = runtimeAddress
                        + static_cast<std::uint64_t>(static_cast<std::int64_t>(signedDelta));
                }
            }
        }
        if (parsed.relativeMemory >= 0)
            request.operands[parsed.relativeMemory].mem.displacement = signedBits(runtimeAddress);
        if (!ZYAN_SUCCESS(ZydisEncoderEncodeInstructionAbsolute(&request, buffer.data(), &length, runtimeAddress)))
            return false;
        if (parsed.relativeMemory >= 0)
        {
            if (runtimeAddress > std::numeric_limits<std::uint64_t>::max() - length) return false;
            request = parsed.request;
            // Unsigned modular addition preserves negative displacements and kernel VAs.
            request.operands[parsed.relativeMemory].mem.displacement =
                signedBits(runtimeAddress + length + parsed.rawDisplacement);
            length = buffer.size();
            if (!ZYAN_SUCCESS(ZydisEncoderEncodeInstructionAbsolute(&request, buffer.data(), &length, runtimeAddress)))
                return false;
        }
        bytes.assign(buffer.begin(), buffer.begin() + length);
        return true;
    }

    bool encodeInstruction(const std::string& text, const Symbols& symbols, bool estimating,
        std::uint64_t runtimeAddress, bool x64, std::vector<std::uint8_t>& output, Failure& failure)
    {
        ParsedRequest parsed;
        if (!parseInstruction(text, parsed, symbols, estimating, runtimeAddress, x64, failure)) return false;
        if (parsed.unsizedMemory.empty())
        {
            if (encodeRequest(parsed, runtimeAddress, output)) return true;
            return failure.set(AssemblyError::InvalidInstruction, text);
        }
        if (parsed.unsizedMemory.size() > 2) return failure.set(AssemblyError::InvalidOperands, text);
        constexpr std::array<ZyanU16, 8> sizes{1, 2, 4, 8, 10, 16, 32, 64};
        std::vector<std::vector<std::uint8_t>> choices;
        const auto count = parsed.unsizedMemory.size() == 1 ? sizes.size() : sizes.size() * sizes.size();
        for (std::size_t i = 0; i < count; ++i)
        {
            auto candidate = parsed;
            candidate.request.operands[candidate.unsizedMemory[0]].mem.size = sizes[i % sizes.size()];
            if (candidate.unsizedMemory.size() == 2)
                candidate.request.operands[candidate.unsizedMemory[1]].mem.size = sizes[i / sizes.size()];
            std::vector<std::uint8_t> bytes;
            if (encodeRequest(candidate, runtimeAddress, bytes)
                && std::find(choices.begin(), choices.end(), bytes) == choices.end()) choices.push_back(std::move(bytes));
        }
        if (choices.empty()) return failure.set(AssemblyError::InvalidInstruction, text);
        if (choices.size() != 1) return failure.set(AssemblyError::AmbiguousMemorySize, text);
        output = std::move(choices.front());
        return true;
    }

    struct Statement
    {
        std::string label;
        std::string text;
        int line = 0;
        std::size_t size = 0;
    };

    IntelAssemblyResult failed(const Failure& failure, int line)
    {
        IntelAssemblyResult result;
        result.error = failure.code;
        result.detail = failure.detail;
        result.errorLine = line;
        return result;
    }
}

IntelAssemblyResult assembleIntel(const std::string& source, std::uint64_t baseAddress, bool x64)
{
    Failure failure;
    if (source.size() > kMaximumSource)
    {
        failure.set(AssemblyError::SourceTooLarge);
        return failed(failure, 0);
    }
    if (!x64 && baseAddress > std::numeric_limits<std::uint32_t>::max())
    {
        failure.set(AssemblyError::AddressOverflow);
        return failed(failure, 0);
    }
    std::vector<Statement> statements;
    Symbols symbols;
    std::size_t start = 0;
    int line = 0;
    for (std::size_t i = 0; i <= source.size(); ++i)
    {
        if (i < source.size() && source[i] != '\n') continue;
        ++line;
        if (line > static_cast<int>(kMaximumLines))
        {
            failure.set(AssemblyError::SourceTooLarge);
            return failed(failure, line);
        }
        auto text = lower(trim(source.substr(start, i - start)));
        start = i + 1;
        const auto comment = text.find(';');
        if (comment != std::string::npos) text = trim(text.substr(0, comment));
        if (text.empty()) continue;
        Statement statement;
        statement.line = line;
        // Only a colon before the mnemonic's first whitespace defines a label.
        const auto colon = text.find(':');
        const auto whitespace = text.find_first_of(" \t[");
        if (colon != std::string::npos && (whitespace == std::string::npos || colon < whitespace))
        {
            statement.label = text.substr(0, colon);
            if (!identifier(statement.label) || findRegister(statement.label) != ZYDIS_REGISTER_NONE)
            {
                failure.set(AssemblyError::InvalidLabel, statement.label);
                return failed(failure, line);
            }
            if (!symbols.emplace(statement.label, baseAddress).second)
            {
                failure.set(AssemblyError::DuplicateLabel, statement.label);
                return failed(failure, line);
            }
            text = trim(text.substr(colon + 1));
        }
        statement.text = std::move(text);
        statements.push_back(std::move(statement));
    }
    if (statements.empty() || std::none_of(statements.begin(), statements.end(), [](const Statement& statement) {
        return !statement.text.empty();
    }))
    {
        failure.set(AssemblyError::EmptySource);
        return failed(failure, 0);
    }
    // First estimate encodes symbolic addresses at the current instruction to avoid
    // rejecting forward short branches before their true offsets are known.
    std::uint64_t estimatedAddress = baseAddress;
    for (auto& statement : statements)
    {
        if (statement.text.empty()) continue;
        std::vector<std::uint8_t> bytes;
        if (!encodeInstruction(statement.text, symbols, true, estimatedAddress, x64, bytes, failure))
            return failed(failure, statement.line);
        statement.size = bytes.size();
        const auto maximum = x64 ? std::numeric_limits<std::uint64_t>::max()
            : std::numeric_limits<std::uint32_t>::max();
        if (bytes.size() > maximum - estimatedAddress && &statement != &statements.back())
        {
            failure.set(AssemblyError::AddressOverflow);
            return failed(failure, statement.line);
        }
        estimatedAddress += bytes.size();
    }
    for (int pass = 0; pass < 32; ++pass)
    {
        std::uint64_t address = baseAddress;
        std::size_t total = 0;
        for (const auto& statement : statements)
        {
            if (!statement.label.empty()) symbols[statement.label] = address;
            const auto maximum = x64 ? std::numeric_limits<std::uint64_t>::max()
                : std::numeric_limits<std::uint32_t>::max();
            if (statement.size && statement.size - 1 > maximum - address)
            {
                failure.set(AssemblyError::AddressOverflow);
                return failed(failure, statement.line);
            }
            if (statement.size > maximum - address && &statement != &statements.back())
            {
                failure.set(AssemblyError::AddressOverflow);
                return failed(failure, statement.line);
            }
            address += statement.size;
            total += statement.size;
            if (total > kMaximumOutput)
            {
                failure.set(AssemblyError::OutputTooLarge);
                return failed(failure, statement.line);
            }
        }
        IntelAssemblyResult result;
        result.bytes.reserve(total);
        address = baseAddress;
        bool changed = false;
        for (auto& statement : statements)
        {
            if (statement.text.empty()) continue;
            std::vector<std::uint8_t> bytes;
            if (!encodeInstruction(statement.text, symbols, false, address, x64, bytes, failure))
                return failed(failure, statement.line);
            changed |= statement.size != bytes.size();
            statement.size = bytes.size();
            address += bytes.size();
            result.bytes.insert(result.bytes.end(), bytes.begin(), bytes.end());
            if (result.bytes.size() > kMaximumOutput)
            {
                failure.set(AssemblyError::OutputTooLarge);
                return failed(failure, statement.line);
            }
        }
        if (!changed)
        {
            result.success = true;
            return result;
        }
    }
    failure.set(AssemblyError::UnstableLabels);
    return failed(failure, 0);
}
}
