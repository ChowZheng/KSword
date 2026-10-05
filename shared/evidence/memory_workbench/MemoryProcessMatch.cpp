// ============================================================
// MemoryProcessMatch.cpp
// 作用：
// - 实现 MemoryProcessMatch.h 声明的进程文本匹配。
// - 全部是纯逻辑，不依赖 Qt 与 Win32；输入输出都是 UTF-8 字节串，大小写折叠只处理 ASCII。
// ============================================================

#include "MemoryProcessMatch.h"

#include <cstddef>
#include <map>

namespace ksword::memwb
{
    namespace
    {
        // kMaxPid：PID 的上限，32 位无符号最大值。超过它的数字文本不可能是 PID。
        constexpr std::uint64_t kMaxPid = 0xFFFFFFFFULL;

        // MatchStage：名字匹配的三级规则，按声明顺序依次尝试。
        enum class MatchStage
        {
            // ExactName：精确进程名。
            ExactName,
            // DisplayText：精确显示文本。
            DisplayText,
            // Substring：子串（进程名或显示文本包含输入）。
            Substring,
        };

        // NumberKind：数字文本的解析结论。
        enum class NumberKind
        {
            // NotNumber：不是数字文本，应走名字匹配。
            NotNumber,
            // Valid：合法的 32 位非零 PID。
            Valid,
            // OutOfRange：是数字，但值为 0 或超过 32 位。
            OutOfRange,
        };

        // NumberParse：ParseNumberText 的输出。
        struct NumberParse
        {
            // kind：解析结论。
            NumberKind kind = NumberKind::NotNumber;
            // value：kind 为 Valid 时的 PID 值，其余为 0。
            std::uint32_t value = 0;
        };

        // WhitespaceLengthAtFront：文本开头是不是一个空白单元，是则返回它的字节数。
        // 传入：text 待检查文本。传出：ASCII 空白 1；UTF-8 的 U+00A0（C2 A0）2；
        // UTF-8 的 U+3000（E3 80 80）3；否则 0。后两种是中文输入法/粘贴时常带进来的空白。
        std::size_t WhitespaceLengthAtFront(const std::string_view text) noexcept
        {
            if (text.empty())
            {
                return 0;
            }

            // ASCII 空白：空格、制表、换行、回车、垂直制表、换页。
            const unsigned char first = static_cast<unsigned char>(text[0]);
            const bool asciiSpace = (first == ' ') || (first == '\t') || (first == '\n')
                || (first == '\r') || (first == '\v') || (first == '\f');
            if (asciiSpace)
            {
                return 1;
            }

            // U+00A0 不换行空格。
            if (text.size() >= 2 && first == 0xC2U && static_cast<unsigned char>(text[1]) == 0xA0U)
            {
                return 2;
            }

            // U+3000 全角空格。
            if (text.size() >= 3 && first == 0xE3U && static_cast<unsigned char>(text[1]) == 0x80U
                && static_cast<unsigned char>(text[2]) == 0x80U)
            {
                return 3;
            }
            return 0;
        }

        // WhitespaceLengthAtBack：文本末尾是不是一个空白单元，是则返回它的字节数。规则同上，方向相反。
        std::size_t WhitespaceLengthAtBack(const std::string_view text) noexcept
        {
            if (text.empty())
            {
                return 0;
            }

            // ASCII 空白。
            const unsigned char last = static_cast<unsigned char>(text[text.size() - 1]);
            const bool asciiSpace = (last == ' ') || (last == '\t') || (last == '\n')
                || (last == '\r') || (last == '\v') || (last == '\f');
            if (asciiSpace)
            {
                return 1;
            }

            // U+00A0：末两字节是 C2 A0。
            if (text.size() >= 2 && static_cast<unsigned char>(text[text.size() - 2]) == 0xC2U && last == 0xA0U)
            {
                return 2;
            }

            // U+3000：末三字节是 E3 80 80。
            if (text.size() >= 3 && static_cast<unsigned char>(text[text.size() - 3]) == 0xE3U
                && static_cast<unsigned char>(text[text.size() - 2]) == 0x80U && last == 0x80U)
            {
                return 3;
            }
            return 0;
        }

        // TrimText：去掉前后空白。传入：text 原文。传出：去掉前后空白单元之后的视图（不拷贝）。
        std::string_view TrimText(std::string_view text) noexcept
        {
            // 先剥开头，再剥末尾；每次剥一个单元直到不再是空白。
            std::size_t frontLength = WhitespaceLengthAtFront(text);
            while (frontLength > 0)
            {
                text.remove_prefix(frontLength);
                frontLength = WhitespaceLengthAtFront(text);
            }

            std::size_t backLength = WhitespaceLengthAtBack(text);
            while (backLength > 0)
            {
                text.remove_suffix(backLength);
                backLength = WhitespaceLengthAtBack(text);
            }
            return text;
        }

        // FoldAscii：把 A-Z 折成 a-z，其余字节（含 UTF-8 的高位字节）原样返回。
        char FoldAscii(const char value) noexcept
        {
            if (value >= 'A' && value <= 'Z')
            {
                return static_cast<char>(value - 'A' + 'a');
            }
            return value;
        }

        // EqualsFolded：两个串在 ASCII 大小写折叠下是否相等。传入：left/right。传出：相等为 true。
        bool EqualsFolded(const std::string_view left, const std::string_view right) noexcept
        {
            if (left.size() != right.size())
            {
                return false;
            }
            for (std::size_t index = 0; index < left.size(); ++index)
            {
                if (FoldAscii(left[index]) != FoldAscii(right[index]))
                {
                    return false;
                }
            }
            return true;
        }

        // ContainsFolded：haystack 在 ASCII 大小写折叠下是否包含 needle。
        // 传入：haystack 被搜索串；needle 要找的串（调用方保证非空）。传出：包含为 true。
        bool ContainsFolded(const std::string_view haystack, const std::string_view needle) noexcept
        {
            if (needle.size() > haystack.size())
            {
                return false;
            }

            // 朴素的逐起点比较：进程名都很短，不值得上更复杂的算法。
            const std::size_t lastStart = haystack.size() - needle.size();
            for (std::size_t start = 0; start <= lastStart; ++start)
            {
                bool matched = true;
                for (std::size_t offset = 0; offset < needle.size(); ++offset)
                {
                    if (FoldAscii(haystack[start + offset]) != FoldAscii(needle[offset]))
                    {
                        matched = false;
                        break;
                    }
                }
                if (matched)
                {
                    return true;
                }
            }
            return false;
        }

        // DigitValue：把一个字符转成数位值。
        // 传入：value 字符；hex 为 true 时接受 a-f/A-F。传出：0..15；不是数位返回 -1。
        int DigitValue(const char value, const bool hex) noexcept
        {
            if (value >= '0' && value <= '9')
            {
                return value - '0';
            }
            if (hex && value >= 'a' && value <= 'f')
            {
                return value - 'a' + 10;
            }
            if (hex && value >= 'A' && value <= 'F')
            {
                return value - 'A' + 10;
            }
            return -1;
        }

        // ParseNumberText：判断文本是不是数字，并解析成 PID。
        // 传入：text 已去空白的文本（非空）。传出：NumberParse。
        // 文法：纯十进制数字；或 0x/0X 后跟至少一位十六进制数字。其它一律 NotNumber。
        NumberParse ParseNumberText(const std::string_view text) noexcept
        {
            NumberParse parsed;

            // 0x 前缀才是十六进制。
            const bool hex = (text.size() >= 2) && (text[0] == '0') && (text[1] == 'x' || text[1] == 'X');
            const std::size_t digitsStart = hex ? 2U : 0U;

            // 只有前缀没有数位（"0x"）不是数字。
            if (digitsStart >= text.size())
            {
                return parsed;
            }

            // 逐位累加。value 一旦超过 32 位上限就停止累加并记下 overflow，
            // 但仍要继续扫描后面的字符：只要有一个非数位字符，整串就不是数字。
            const std::uint64_t base = hex ? 16ULL : 10ULL;
            std::uint64_t value = 0;
            bool overflow = false;
            for (std::size_t index = digitsStart; index < text.size(); ++index)
            {
                const int digit = DigitValue(text[index], hex);
                if (digit < 0)
                {
                    return parsed;
                }
                if (!overflow)
                {
                    value = value * base + static_cast<std::uint64_t>(digit);
                    if (value > kMaxPid)
                    {
                        overflow = true;
                    }
                }
            }

            // 值为 0 或超过 32 位：是数字但不可能是 PID。
            if (overflow || value == 0ULL)
            {
                parsed.kind = NumberKind::OutOfRange;
                return parsed;
            }
            parsed.kind = NumberKind::Valid;
            parsed.value = static_cast<std::uint32_t>(value);
            return parsed;
        }

        // StageMatches：某一级规则下，候选是否命中输入。
        // 传入：stage 规则级别；needle 已去空白的输入；candidate 候选进程。传出：命中为 true。
        bool StageMatches(const MatchStage stage, const std::string_view needle, const ProcessCandidate& candidate)
        {
            switch (stage)
            {
            case MatchStage::ExactName:
                return EqualsFolded(candidate.name, needle);
            case MatchStage::DisplayText:
                return EqualsFolded(ProcessDisplayText(candidate), needle);
            case MatchStage::Substring:
                return ContainsFolded(candidate.name, needle)
                    || ContainsFolded(ProcessDisplayText(candidate), needle);
            }
            return false;
        }

        // CollectHits：收集某一级规则下的全部命中，按 PID 去重并升序。
        // 传入：stage 规则级别；needle 输入；candidates 候选列表。
        // 传出：以 PID 为键的有序表；同一 PID 出现多次只保留第一条，pid 为 0 的候选忽略。
        std::map<std::uint32_t, ProcessCandidate> CollectHits(
            const MatchStage stage,
            const std::string_view needle,
            const std::vector<ProcessCandidate>& candidates)
        {
            std::map<std::uint32_t, ProcessCandidate> hits;
            for (const ProcessCandidate& candidate : candidates)
            {
                if (candidate.pid == 0U)
                {
                    continue;
                }
                if (StageMatches(stage, needle, candidate))
                {
                    // emplace 在键已存在时不覆盖，因此保留的是第一次出现的那条。
                    hits.emplace(candidate.pid, candidate);
                }
            }
            return hits;
        }

        // FillFromHits：把命中表写进结果。
        // 传入：hits 命中表（非空）；by 命中的规则级别；result 被填写的结果。
        // 传出：1 个命中写成 Unique，多个写成 Multiple（按 PID 升序，pid 字段保持 0）。
        void FillFromHits(
            const std::map<std::uint32_t, ProcessCandidate>& hits,
            const ProcessMatchBy by,
            ProcessMatchResult& result)
        {
            result.by = by;
            for (const auto& entry : hits)
            {
                result.candidates.push_back(entry.second);
            }

            // 恰好一个命中才是 Unique，并带出它的 PID；多个绝不取第一个。
            if (hits.size() == 1U)
            {
                result.kind = ProcessMatchKind::Unique;
                result.pid = hits.begin()->first;
                return;
            }
            result.kind = ProcessMatchKind::Multiple;
        }
    }

    // ProcessDisplayText：固定格式 "name [PID:pid]"。
    std::string ProcessDisplayText(const ProcessCandidate& candidate)
    {
        std::string text = candidate.name;
        text.append(" [PID:");
        text.append(std::to_string(candidate.pid));
        text.push_back(']');
        return text;
    }

    // MatchProcessText：规则见头文件。
    ProcessMatchResult MatchProcessText(const std::string_view text, const std::vector<ProcessCandidate>& candidates)
    {
        // result：结论，默认就是 Empty/None/0/空，所有失败路径都保持这套安全初值之外的字段不动。
        ProcessMatchResult result;

        // 第一步：去前后空白，空串直接返回 Empty。
        const std::string_view needle = TrimText(text);
        if (needle.empty())
        {
            return result;
        }

        // 第二步：数字文本按 PID 处理，不再回头按名字匹配。
        const NumberParse number = ParseNumberText(needle);
        if (number.kind == NumberKind::OutOfRange)
        {
            result.kind = ProcessMatchKind::BadPid;
            return result;
        }
        if (number.kind == NumberKind::Valid)
        {
            // 在候选里精确查 PID；同一 PID 重复只算一个。
            for (const ProcessCandidate& candidate : candidates)
            {
                if (candidate.pid == number.value)
                {
                    result.kind = ProcessMatchKind::Unique;
                    result.by = ProcessMatchBy::Pid;
                    result.pid = candidate.pid;
                    result.candidates.push_back(candidate);
                    return result;
                }
            }
            result.kind = ProcessMatchKind::NotFound;
            return result;
        }

        // 第三步：名字匹配，三级规则依次尝试，哪一级有命中就在那一级收口。
        // 级别与规则的对应：精确名 -> ExactName，精确显示文本 -> DisplayText，子串 -> Substring。
        const MatchStage stages[] = { MatchStage::ExactName, MatchStage::DisplayText, MatchStage::Substring };
        const ProcessMatchBy reported[] = {
            ProcessMatchBy::ExactName, ProcessMatchBy::DisplayText, ProcessMatchBy::Substring
        };
        for (std::size_t index = 0; index < 3U; ++index)
        {
            const std::map<std::uint32_t, ProcessCandidate> hits = CollectHits(stages[index], needle, candidates);
            if (!hits.empty())
            {
                FillFromHits(hits, reported[index], result);
                return result;
            }
        }

        // 三级都没有命中。
        result.kind = ProcessMatchKind::NotFound;
        return result;
    }
}
