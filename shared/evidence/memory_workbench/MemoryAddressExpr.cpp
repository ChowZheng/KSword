// ============================================================
// MemoryAddressExpr.cpp
// 作用：
// - 实现 MemoryAddressExpr.h 声明的地址表达式解析与求值。
// - 结构分两段：先把整串文本解析成"项"的列表（语法错误全在这一段暴露，不碰
//   resolver），再对项列表求值（这一段才会调用 resolver）。
// ============================================================

#include "MemoryAddressExpr.h"

#include "../NumericTextParse.h"

#include <limits>
#include <utility>
#include <vector>

namespace ksword::memwb
{
namespace
{
    // kMaxU64：64 位无符号数的上限，求和溢出判断用它做界。
    constexpr std::uint64_t kMaxU64 = (std::numeric_limits<std::uint64_t>::max)();

    // kMaxU32：32 位无符号数的上限，32 位指针宽度下检查 resolver 返回值用。
    constexpr std::uint64_t kMaxU32 = 0xFFFFFFFFULL;

    // TermKind：一个"项"的种类。
    enum class TermKind
    {
        Number,  // 十六进制数字
        Module,  // 模块名
        Deref,   // 方括号解引用
    };

    // Term：解析后的一个项。解析阶段填好，求值阶段只读。
    struct Term
    {
        // kind：项的种类。
        TermKind kind = TermKind::Number;
        // number：Number 项的值。
        std::uint64_t number = 0;
        // name：Module 项的模块名（已去引号、已剪空白）。
        std::string name;
        // inner：Deref 项的内部表达式（若干项，求值时相加）。
        std::vector<Term> inner;
        // position：项在原始输入中的起始字节偏移。
        std::size_t position = 0;
        // text：出错时报告的记号文本；Number 为数字记号，Module 为模块名，
        // Deref 为含方括号的整段文本。
        std::string text;
    };

    // Failure：解析或求值失败的原因，最终搬进 ExprResult。
    struct Failure
    {
        // error：失败原因。
        ExprError error = ExprError::None;
        // position：出错字节偏移。
        std::size_t position = 0;
        // detail：出错记号文本。
        std::string detail;
    };

    // IsSpaceChar：是否空白。与 NumericTextTrim 使用同一组空白字符，避免两处判断不一致。
    bool IsSpaceChar(const char character)
    {
        return character == ' ' || character == '\t' || character == '\r'
            || character == '\n' || character == '\f' || character == '\v';
    }

    // IsHexDigitChar：是否十六进制数位。直接复用数值解析模块的数位表，不另写一份。
    bool IsHexDigitChar(const char character)
    {
        return ksword::evidence::NumericTextDigitValue(character, 16) >= 0;
    }

    // IsSeparatorChar：是否数位分隔符（调试器粘贴格式的反引号与下划线）。
    bool IsSeparatorChar(const char character)
    {
        return character == '`' || character == '_';
    }

    // IsQuoteChar：是否模块名引号（双引号或单引号）。
    bool IsQuoteChar(const char character)
    {
        return character == '"' || character == '\'';
    }

    // IsTokenTerminator：无引号记号在遇到这些字符时结束。
    // 空白、加号、方括号、引号都是结构字符，不能出现在无引号记号里。
    bool IsTokenTerminator(const char character)
    {
        return IsSpaceChar(character) || character == '+' || character == '['
            || character == ']' || IsQuoteChar(character);
    }

    // StripHexPrefix：去掉一个开头的 0x/0X 前缀，返回数位部分。没有前缀则原样返回。
    std::string_view StripHexPrefix(const std::string_view token)
    {
        if (token.size() >= 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X'))
        {
            return token.substr(2);
        }
        return token;
    }

    // IsNumericCharset：整个记号（去掉 0x 前缀后）是否只由十六进制数位与分隔符组成。
    // 注意这只判字符集，不判格式：`0x`、`1__2` 字符集合格，格式不合格，
    // 它们要走数字解析并得到 BadNumber，而不是被当成模块名。
    bool IsNumericCharset(const std::string_view token)
    {
        const std::string_view body = StripHexPrefix(token);
        for (const char character : body)
        {
            if (!IsHexDigitChar(character) && !IsSeparatorChar(character))
            {
                return false;
            }
        }
        return true;
    }

    // ParseNumberToken：解析一个数字记号。
    // 传入：token 含可选 0x 前缀、数位与分隔符的记号。
    // 传出：valueOut 成功时的值；errorOut 失败时是 BadNumber 或 Overflow。
    bool ParseNumberToken(
        const std::string_view token,
        std::uint64_t& valueOut,
        ExprError& errorOut)
    {
        valueOut = 0;
        errorOut = ExprError::BadNumber;
        const std::string_view body = StripHexPrefix(token);
        if (body.empty())
        {
            // 只有 0x 前缀没有数位：不能退化成 0，0 本身是合法地址。
            return false;
        }

        // 逐字符校验分隔符位置，同时收集纯数位。分隔符只在两个数位之间合法：
        // previousWasDigit 为假时遇到分隔符，说明它在开头或紧跟另一个分隔符。
        std::string digits;
        digits.reserve(body.size());
        bool previousWasDigit = false;
        for (const char character : body)
        {
            if (IsSeparatorChar(character))
            {
                if (!previousWasDigit)
                {
                    return false;
                }
                previousWasDigit = false;
                continue;
            }
            if (!IsHexDigitChar(character))
            {
                return false;
            }
            digits.push_back(character);
            previousWasDigit = true;
        }
        if (!previousWasDigit)
        {
            // 以分隔符结尾。
            return false;
        }

        // 到这里 digits 非空且全是十六进制数位，唯一可能的失败原因就是超过 64 位。
        // 数位累加复用 ParseNumericText（地址语义，默认十六进制，溢出判失败不回绕）。
        const ksword::evidence::NumericTextParseResult parsed = ksword::evidence::ParseNumericText(
            digits,
            ksword::evidence::NumericTextDefaultRadix::Hexadecimal);
        if (!parsed.ok)
        {
            errorOut = ExprError::Overflow;
            return false;
        }
        valueOut = parsed.value;
        errorOut = ExprError::None;
        return true;
    }

    // IsSubtractionLike：含 '-' 的无引号记号是不是"数字减法"。
    // 判据：以 '-' 切开，所有非空片段都是数字字符集（0x100-10、100-、ab-cd）。
    // 只要有一个非空片段含非数字字符（api-ms-win-...、7-zip.dll），就是模块名。
    // 没有任何非空片段（"-"、"--"）按减法处理，因为它们不可能是模块名。
    bool IsSubtractionLike(const std::string_view token)
    {
        std::size_t pieceStart = 0;
        while (pieceStart <= token.size())
        {
            std::size_t pieceEnd = token.find('-', pieceStart);
            if (pieceEnd == std::string_view::npos)
            {
                pieceEnd = token.size();
            }
            const std::string_view piece = token.substr(pieceStart, pieceEnd - pieceStart);
            if (!piece.empty() && !IsNumericCharset(piece))
            {
                return false;
            }
            pieceStart = pieceEnd + 1;
        }
        return true;
    }

    // Parser：把表达式文本解析成项列表的递归下降解析器。
    // 解析阶段不接触 resolver，任何语法错误都在这里暴露。
    class Parser
    {
    public:
        // 构造：绑定要解析的文本（调用期间文本必须保持有效）。
        explicit Parser(const std::string_view text)
            : text_(text)
        {
        }

        // Parse：解析整串文本。
        // 传出：terms 顶层项列表；failure 失败时的原因。返回 false 表示失败。
        bool Parse(std::vector<Term>& terms, Failure& failure)
        {
            SkipSpaces();
            if (AtEnd())
            {
                failure.error = ExprError::Empty;
                failure.position = 0;
                failure.detail.clear();
                return false;
            }
            // 顶层表达式只会停在输入末尾或一个多余的右括号上。
            if (!ParseExpression(0, terms))
            {
                failure = failure_;
                return false;
            }
            if (!AtEnd())
            {
                Fail(ExprError::BadSyntax, pos_, std::string(1, text_[pos_]));
                failure = failure_;
                return false;
            }
            return true;
        }

    private:
        // text_：被解析的原始文本。
        std::string_view text_;
        // pos_：当前读到的字节偏移。
        std::size_t pos_ = 0;
        // moduleSeen_：整个表达式里是否已经出现过模块项（含括号内），模块最多一次。
        bool moduleSeen_ = false;
        // failure_：最近一次失败的原因。
        Failure failure_;

        // AtEnd：是否读到输入末尾。
        bool AtEnd() const
        {
            return pos_ >= text_.size();
        }

        // SkipSpaces：跳过当前位置起的连续空白。
        void SkipSpaces()
        {
            while (!AtEnd() && IsSpaceChar(text_[pos_]))
            {
                ++pos_;
            }
        }

        // Fail：记录失败原因并返回 false，方便调用处 `return Fail(...)`。
        bool Fail(const ExprError error, const std::size_t position, std::string detail)
        {
            failure_.error = error;
            failure_.position = position;
            failure_.detail = std::move(detail);
            return false;
        }

        // ScanBareTokenEnd：从 start 起找无引号记号的结束位置（不含）。
        std::size_t ScanBareTokenEnd(const std::size_t start) const
        {
            std::size_t end = start;
            while (end < text_.size() && !IsTokenTerminator(text_[end]))
            {
                ++end;
            }
            return end;
        }

        // FailUnexpected：当前位置是"一项结束后不该出现"的字符，报 BadSyntax。
        // 详情取这个字符起的整个记号；若它本身是结构字符则只取这一个字符。
        bool FailUnexpected()
        {
            const std::size_t end = ScanBareTokenEnd(pos_);
            const std::size_t length = (end > pos_) ? (end - pos_) : 1U;
            return Fail(ExprError::BadSyntax, pos_, std::string(text_.substr(pos_, length)));
        }

        // ParseExpression：解析"项 ( '+' 项 )*"，停在输入末尾或右括号上（不消费右括号）。
        // 传入：depth 当前方括号嵌套层数；传出：terms 追加解析出的项。
        bool ParseExpression(const int depth, std::vector<Term>& terms)
        {
            for (;;)
            {
                SkipSpaces();
                const bool isFirst = terms.empty();
                Term term;
                if (!ParseTerm(depth, isFirst, term))
                {
                    return false;
                }
                terms.push_back(std::move(term));

                // 一项之后只允许：结束、右括号、加号。别的字符（含减号、第二个记号）
                // 一律是语法错误。
                SkipSpaces();
                if (AtEnd())
                {
                    return true;
                }
                const char next = text_[pos_];
                if (next == ']')
                {
                    return true;
                }
                if (next != '+')
                {
                    return FailUnexpected();
                }
                ++pos_;
            }
        }

        // ParseTerm：解析一个项。isFirst 表示它是所在表达式的第一项（模块项只能在这里）。
        bool ParseTerm(const int depth, const bool isFirst, Term& term)
        {
            if (AtEnd())
            {
                // 缺项：尾随加号、`[`之后什么都没有。位置指向输入末尾。
                return Fail(ExprError::BadSyntax, text_.size(), std::string());
            }
            const char first = text_[pos_];
            if (first == '[')
            {
                return ParseDeref(depth, term);
            }
            if (IsQuoteChar(first))
            {
                return ParseQuotedModule(isFirst, term);
            }
            if (first == '+' || first == ']')
            {
                // 项的位置上出现了分隔符：前导加号、连续加号、空括号都是这里。
                return Fail(ExprError::BadSyntax, pos_, std::string(1, first));
            }
            return ParseBareToken(isFirst, term);
        }

        // ParseDeref：解析 '[' 表达式 ']'。
        bool ParseDeref(const int depth, Term& term)
        {
            // 嵌套深度上限：第 kMaxAddressExprDerefDepth+1 层 '[' 直接拒绝，
            // 递归深度因此有硬上限。
            if (depth >= kMaxAddressExprDerefDepth)
            {
                return Fail(ExprError::BadSyntax, pos_, "[");
            }
            const std::size_t openPos = pos_;
            ++pos_;
            term.kind = TermKind::Deref;
            term.position = openPos;
            if (!ParseExpression(depth + 1, term.inner))
            {
                return false;
            }
            if (AtEnd())
            {
                // 内部表达式走到末尾也没遇到右括号：报在对应的 '[' 上。
                return Fail(ExprError::BadSyntax, openPos, "[");
            }
            ++pos_;
            term.text = std::string(text_.substr(openPos, pos_ - openPos));
            return true;
        }

        // ParseQuotedModule：解析带引号的模块名。引号内不转义，不含加号限制。
        bool ParseQuotedModule(const bool isFirst, Term& term)
        {
            const std::size_t start = pos_;
            const char quote = text_[pos_];
            const std::size_t closePos = text_.find(quote, start + 1);
            if (closePos == std::string_view::npos)
            {
                // 引号没有闭合：报在开引号上，详情取到末尾的整段。
                return Fail(ExprError::BadSyntax, start, std::string(text_.substr(start)));
            }
            const std::string_view inner = ksword::evidence::NumericTextTrim(
                text_.substr(start + 1, closePos - start - 1));
            if (inner.empty())
            {
                return Fail(
                    ExprError::BadSyntax,
                    start,
                    std::string(text_.substr(start, closePos + 1 - start)));
            }
            pos_ = closePos + 1;
            term.kind = TermKind::Module;
            term.position = start;
            term.name = std::string(inner);
            term.text = term.name;
            return AcceptModule(isFirst, term);
        }

        // ParseBareToken：解析无引号记号，按"数字 / 减法 / 模块名"分类。
        bool ParseBareToken(const bool isFirst, Term& term)
        {
            const std::size_t start = pos_;
            const std::size_t end = ScanBareTokenEnd(start);
            const std::string_view token = text_.substr(start, end - start);
            pos_ = end;

            // 整串都是十六进制字符：一律按数字，哪怕它同时是个合法的模块名（abc）。
            if (IsNumericCharset(token))
            {
                std::uint64_t value = 0;
                ExprError error = ExprError::None;
                if (!ParseNumberToken(token, value, error))
                {
                    return Fail(error, start, std::string(token));
                }
                term.kind = TermKind::Number;
                term.number = value;
                term.position = start;
                term.text = std::string(token);
                return true;
            }

            // 含 '-' 且各片段都是数字：这是想写减法，明确拒绝而不是当模块名。
            const std::size_t minusIndex = token.find('-');
            if (minusIndex != std::string_view::npos && IsSubtractionLike(token))
            {
                return Fail(ExprError::BadSyntax, start + minusIndex, std::string(token));
            }

            term.kind = TermKind::Module;
            term.position = start;
            term.name = std::string(token);
            term.text = term.name;
            return AcceptModule(isFirst, term);
        }

        // AcceptModule：模块项的位置规则——整个表达式最多一个，且必须是所在层的第一项。
        bool AcceptModule(const bool isFirst, const Term& term)
        {
            if (moduleSeen_ || !isFirst)
            {
                return Fail(ExprError::BadSyntax, term.position, term.text);
            }
            moduleSeen_ = true;
            return true;
        }
    };

    // Evaluator：对已解析的项列表求值，唯一会调用 resolver 的地方。
    class Evaluator
    {
    public:
        // 构造：绑定指针宽度与 resolver（可为空）。
        Evaluator(const std::uint32_t widthBytes, IAddressExprResolver* const resolver)
            : widthBytes_(widthBytes)
            , resolver_(resolver)
        {
        }

        // EvaluateTerms：把一组项相加。任何一步溢出判 Overflow，绝不回绕。
        // 传出：valueOut 成功时的和。返回 false 时原因在 failure()。
        bool EvaluateTerms(const std::vector<Term>& terms, std::uint64_t& valueOut)
        {
            std::uint64_t sum = 0;
            for (const Term& term : terms)
            {
                std::uint64_t value = 0;
                if (!EvaluateTerm(term, value))
                {
                    return false;
                }
                // 先判溢出再相加。回绕后的值同样是个合法地址，会被照单读下去。
                if (sum > kMaxU64 - value)
                {
                    return Fail(ExprError::Overflow, term);
                }
                sum += value;
            }
            valueOut = sum;
            return true;
        }

        // failure：最近一次失败的原因。
        const Failure& failure() const
        {
            return failure_;
        }

        // usedModule：求值过程中是否解析过模块项。
        bool usedModule() const
        {
            return usedModule_;
        }

        // moduleName：解析过的模块名。
        const std::string& moduleName() const
        {
            return moduleName_;
        }

        // usedDeref：求值过程中是否做过解引用。
        bool usedDeref() const
        {
            return usedDeref_;
        }

    private:
        // widthBytes_：解引用读取的指针宽度。
        std::uint32_t widthBytes_ = 0;
        // resolver_：外部能力，可能为空。
        IAddressExprResolver* resolver_ = nullptr;
        // failure_：最近一次失败的原因。
        Failure failure_;
        // usedModule_：是否解析过模块项。
        bool usedModule_ = false;
        // moduleName_：解析过的模块名。
        std::string moduleName_;
        // usedDeref_：是否做过解引用。
        bool usedDeref_ = false;

        // Fail：记录失败，位置与详情取自出错的项。
        bool Fail(const ExprError error, const Term& term)
        {
            failure_.error = error;
            failure_.position = term.position;
            failure_.detail = term.text;
            return false;
        }

        // EvaluateTerm：求单个项的值。
        bool EvaluateTerm(const Term& term, std::uint64_t& valueOut)
        {
            if (term.kind == TermKind::Number)
            {
                valueOut = term.number;
                return true;
            }
            if (term.kind == TermKind::Module)
            {
                return EvaluateModule(term, valueOut);
            }
            return EvaluateDeref(term, valueOut);
        }

        // EvaluateModule：经 resolver 查模块基址。
        bool EvaluateModule(const Term& term, std::uint64_t& valueOut)
        {
            if (resolver_ == nullptr)
            {
                return Fail(ExprError::NeedsProcess, term);
            }
            std::uint64_t base = 0;
            switch (resolver_->LookupModule(term.name, base))
            {
            case ModuleLookup::Found:
                valueOut = base;
                usedModule_ = true;
                moduleName_ = term.name;
                return true;
            case ModuleLookup::Ambiguous:
                return Fail(ExprError::AmbiguousModule, term);
            case ModuleLookup::NeedsProcess:
                return Fail(ExprError::NeedsProcess, term);
            case ModuleLookup::NotFound:
            default:
                // 枚举之外的返回值也按"找不到"处理：拒绝，不猜。
                return Fail(ExprError::UnknownModule, term);
            }
        }

        // EvaluateDeref：先求内部表达式，再经 resolver 读指针。
        bool EvaluateDeref(const Term& term, std::uint64_t& valueOut)
        {
            if (resolver_ == nullptr)
            {
                return Fail(ExprError::NeedsProcess, term);
            }
            // 宽度非法时不能读，也不必先去求内部表达式（那会白白调用 resolver）。
            if (widthBytes_ != 4U && widthBytes_ != 8U)
            {
                return Fail(ExprError::DerefFailed, term);
            }
            std::uint64_t address = 0;
            if (!EvaluateTerms(term.inner, address))
            {
                return false;
            }
            std::uint64_t pointer = 0;
            if (!resolver_->ReadPointer(address, widthBytes_, pointer))
            {
                return Fail(ExprError::DerefFailed, term);
            }
            // 32 位宽度下 resolver 必须零扩展返回；高位非零说明它违约，
            // 拿去当地址只会读到别处，所以拒绝。
            if (widthBytes_ == 4U && pointer > kMaxU32)
            {
                return Fail(ExprError::DerefFailed, term);
            }
            valueOut = pointer;
            usedDeref_ = true;
            return true;
        }
    };

    // MakeFailureResult：把失败原因装成 ExprResult，除错误三元组外全部保持初值。
    ExprResult MakeFailureResult(const Failure& failure)
    {
        ExprResult result;
        result.ok = false;
        result.value = 0;
        result.error = failure.error;
        result.detail = failure.detail;
        result.errorPosition = failure.position;
        return result;
    }
}

ExprResult EvaluateAddressExpr(
    const std::string_view text,
    const std::uint32_t widthBytes,
    IAddressExprResolver* const resolver)
{
    // 第一段：整体解析。语法错误在这里就返回，resolver 还没被碰过。
    Parser parser(text);
    std::vector<Term> terms;
    Failure parseFailure;
    if (!parser.Parse(terms, parseFailure))
    {
        return MakeFailureResult(parseFailure);
    }

    // 第二段：求值。只有走到这里才可能调用 resolver。
    Evaluator evaluator(widthBytes, resolver);
    std::uint64_t value = 0;
    if (!evaluator.EvaluateTerms(terms, value))
    {
        return MakeFailureResult(evaluator.failure());
    }

    ExprResult result;
    result.ok = true;
    result.value = value;
    result.error = ExprError::None;
    result.usedModule = evaluator.usedModule();
    result.moduleName = evaluator.moduleName();
    result.usedDeref = evaluator.usedDeref();
    return result;
}

bool ParsePlainAddress(const std::string_view text, std::uint64_t& value)
{
    // 失败时 value 恒为 0：先清零，成功再覆盖。
    value = 0;
    const std::string_view trimmed = ksword::evidence::NumericTextTrim(text);
    if (trimmed.empty())
    {
        return false;
    }
    std::uint64_t parsed = 0;
    ExprError error = ExprError::None;
    if (!ParseNumberToken(trimmed, parsed, error))
    {
        return false;
    }
    value = parsed;
    return true;
}
}
