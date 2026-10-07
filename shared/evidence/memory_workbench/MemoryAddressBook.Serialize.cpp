// MemoryAddressBook.Serialize.cpp
// 地址簿 v1/v2 文本格式的写出与解析。格式定义见 MemoryAddressBook.h 文件头。

#include "MemoryAddressBook.h"

#include "../NumericTextParse.h"
#include "../PointerChain.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace ksword::memwb {

namespace {

// 标题魔数（不含版本）与完整的 v1 标题行。
constexpr std::string_view kHeaderMagic = "KSWORD-ADDRESS-BOOK";
constexpr std::string_view kHeaderLineV1 = "KSWORD-ADDRESS-BOOK 1";
constexpr std::string_view kHeaderLineV2 = "KSWORD-ADDRESS-BOOK 2";
// 每条条目行的字段数，以及地址字段允许的最大十六进制位数（16 位 = 64 位）。
constexpr std::size_t kEntryFieldCount = 8;
constexpr std::size_t kEntryFieldCountV2 = 16;
constexpr std::size_t kMaxHexDigits = 16;

// AppendEscaped：把一个字符串字段转义后追加到 out。
// 传入：目标缓冲；原始字段；传出：out 末尾追加转义结果。
// 只转义反斜杠、制表符、换行、回车；其余字节（含多字节 UTF-8）原样复制。
void AppendEscaped(std::string& out, const std::string_view text) {
    for (const char character : text) {
        switch (character) {
        case '\\':
            out += "\\\\";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        default:
            out.push_back(character);
            break;
        }
    }
}

// Unescape：AppendEscaped 的逆操作。
// 传入：转义后的字段；输出缓冲。传出：成功返回 true 且 out 为原始字段；
// 遇到非法反斜杠序列（含末尾孤立反斜杠）返回 false，此时 out 内容无意义。
bool Unescape(const std::string_view text, std::string& out) {
    out.clear();
    out.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char character = text[index];
        if (character != '\\') {
            out.push_back(character);
            continue;
        }

        // 遇到反斜杠：后面必须还有一个字符，且只能是 \ t n r 之一。
        if (index + 1 >= text.size()) {
            return false;
        }
        ++index;
        switch (text[index]) {
        case '\\':
            out.push_back('\\');
            break;
        case 't':
            out.push_back('\t');
            break;
        case 'n':
            out.push_back('\n');
            break;
        case 'r':
            out.push_back('\r');
            break;
        default:
            return false;
        }
    }
    return true;
}

// FormatHexAddress：把 64 位数写成 "0x" + 最短小写十六进制（0 写成 "0x0"）。
std::string FormatHexAddress(const std::uint64_t value) {
    // kDigits：十六进制数位表，下标即数位值，固定小写。
    static const char kDigits[] = "0123456789abcdef";

    // 从最低位往高位取，倒着收集，最后翻转成正常顺序。
    // reversedDigits：倒序收集到的数位字符。
    std::string reversedDigits;
    // remaining：还没取走的高位部分，每轮右移 4 位。
    std::uint64_t remaining = value;
    do {
        reversedDigits.push_back(kDigits[remaining & 0xFULL]);
        remaining >>= 4;
    } while (remaining != 0);

    // text：最终文本，"0x" 前缀 + 翻转后的数位。
    std::string text = "0x";
    text.append(reversedDigits.rbegin(), reversedDigits.rend());
    return text;
}

std::string FormatOffset(const std::int64_t value) {
    const auto magnitude = value < 0 ? static_cast<std::uint64_t>(-(value + 1)) + 1
                                     : static_cast<std::uint64_t>(value);
    return (value < 0 ? "-" : "") + FormatHexAddress(magnitude);
}

// ParseHexAddress：严格解析 "0x" + 1~16 位十六进制。
// 传入：字段文本；输出值。传出：成功返回 true；失败返回 false 且 valueOut 不变。
// 不接受空白、正负号、大写 0X、17 位及以上（哪怕前导零）。
bool ParseHexAddress(const std::string_view text, std::uint64_t& valueOut) {
    if (text.size() < 3 || text[0] != '0' || text[1] != 'x') {
        return false;
    }
    // digits：去掉 "0x" 之后的数位部分。
    const std::string_view digits = text.substr(2);
    if (digits.size() > kMaxHexDigits) {
        return false;
    }
    // parsed：解析出的值，全部合法才写回 valueOut。
    std::uint64_t parsed = 0;
    if (!ksword::evidence::NumericTextParseDigits(digits, 16, parsed)) {
        return false;
    }
    valueOut = parsed;
    return true;
}

bool ParseOffset(const std::string_view text, std::int64_t& valueOut) {
    auto unsignedText = text;
    if (!unsignedText.empty() && (unsignedText.front() == '-' || unsignedText.front() == '+'))
        unsignedText.remove_prefix(1);
    std::uint64_t magnitude = 0;
    return ParseHexAddress(unsignedText, magnitude) && ksword::pointer_chain::ParseOffset(text, valueOut);
}

bool ParsePositive(const std::string_view text, std::uint64_t& valueOut) {
    return ksword::evidence::NumericTextParseDigits(text, 10, valueOut) && valueOut > 0;
}

// ParseEntryId：严格解析十进制 id，且必须落在 1 <= id < kAddressBookIdLimit。
// 传入：字段文本；输出值。传出：成功返回 true；失败返回 false 且 idOut 不变。
bool ParseEntryId(const std::string_view text, std::uint64_t& idOut) {
    // parsed：解析出的 id，范围校验通过才写回 idOut。
    std::uint64_t parsed = 0;
    if (!ksword::evidence::NumericTextParseDigits(text, 10, parsed)) {
        return false;
    }
    if (parsed == 0 || parsed >= kAddressBookIdLimit) {
        return false;
    }
    idOut = parsed;
    return true;
}

// SplitTabs：按制表符切分一行，保留空字段（"a\t\tb" 得到 3 个字段）。
std::vector<std::string_view> SplitTabs(const std::string_view line) {
    // fields：切出的字段视图，不拷贝，生命周期与 line 相同。
    std::vector<std::string_view> fields;
    // start：当前字段的起始下标。
    std::size_t start = 0;
    while (true) {
        // tabPosition：下一个制表符的位置，npos 表示已经是最后一个字段。
        const std::size_t tabPosition = line.find('\t', start);
        if (tabPosition == std::string_view::npos) {
            fields.push_back(line.substr(start));
            break;
        }
        fields.push_back(line.substr(start, tabPosition - start));
        if (fields.size() > kEntryFieldCountV2) break;
        start = tabPosition + 1;
    }
    return fields;
}

// CheckHeaderLine：校验标题行（不含行尾 LF）。
// 魔数对、版本不是 "1"/"2" -> UnsupportedVersion；其它任何不符 -> BadHeader。
DeserializeError CheckHeaderLine(const std::string_view line) {
    if (line == kHeaderLineV1 || line == kHeaderLineV2) {
        return DeserializeError::None;
    }

    // 魔数后面紧跟一个空格与非空版本串，才有资格被叫做"未知版本"。
    // hasMagic：这一行是否具有"魔数 + 空格 + 非空版本"的形状。
    const bool hasMagic = line.size() > kHeaderMagic.size() + 1
        && line.substr(0, kHeaderMagic.size()) == kHeaderMagic
        && line[kHeaderMagic.size()] == ' ';
    if (hasMagic) {
        return DeserializeError::UnsupportedVersion;
    }
    return DeserializeError::BadHeader;
}

// ParseEntryLine：解析一条条目行（不含行尾 LF）。
// 传入：行文本；输出条目。传出：None 表示成功（entryOut 完整填好）；
// 否则返回具体的失败原因，entryOut 内容无意义。
// 校验顺序：字段数 -> id -> kind -> valueType -> rva -> absolute -> 三个字符串转义 -> 地址一致性。
DeserializeError ParseEntryLine(const std::string_view line, const bool version2, AddressEntry& entryOut) {
    // 字段数必须与版本相符；多/少一个制表符或空行都落在这里。
    // fields：这一行切出的字段视图。
    const std::vector<std::string_view> fields = SplitTabs(line);
    if (fields.size() != (version2 ? kEntryFieldCountV2 : kEntryFieldCount)) {
        return DeserializeError::WrongFieldCount;
    }

    // 前三个字段：id、kind、valueType。
    // entry：逐字段填充的临时条目，全部校验通过才整体交给 entryOut。
    AddressEntry entry;
    if (!ParseEntryId(fields[0], entry.id)) {
        return DeserializeError::BadId;
    }
    if (!ParseEntryKind(fields[1], entry.kind)) {
        return DeserializeError::UnknownKind;
    }
    if (!ParseValueType(fields[2], entry.valueType)) {
        return DeserializeError::UnknownValueType;
    }

    // 字段 5、6：两个地址，格式相同。
    if (!ParseHexAddress(fields[5], entry.rva)) {
        return DeserializeError::BadAddress;
    }
    if (!ParseHexAddress(fields[6], entry.absoluteAddress)) {
        return DeserializeError::BadAddress;
    }

    // 字段 3、4、7：三个转义字符串。
    if (!Unescape(fields[3], entry.targetKey)) {
        return DeserializeError::BadEscape;
    }
    if (!Unescape(fields[4], entry.moduleName)) {
        return DeserializeError::BadEscape;
    }
    if (!Unescape(fields[7], entry.note)) {
        return DeserializeError::BadEscape;
    }

    // 一致性：有模块名则绝对地址必须为 0；无模块名则 rva 必须为 0。
    // Add 写出的文件天然满足；不满足说明文件被改坏了，宁可拒绝也不替它选一个。
    if (!entry.moduleName.empty() && entry.absoluteAddress != 0) {
        return DeserializeError::InconsistentAddress;
    }
    if (entry.moduleName.empty() && entry.rva != 0) {
        return DeserializeError::InconsistentAddress;
    }
    if (version2) {
        if (fields[8].empty()) {
            for (std::size_t i = 9; i < fields.size(); ++i)
                if (!fields[i].empty()) return DeserializeError::BadPointerChain;
        } else {
            if (fields[8] != "pointer") return DeserializeError::BadPointerChain;
            PointerBookmarkDefinition definition;
            // Bound escaped inputs before allocating decoded path buffers.
            if (fields[9].size() > 65536 || fields[10].size() > 65536)
                return DeserializeError::BadPointerChain;
            if (!Unescape(fields[9], definition.processPath) || !Unescape(fields[10], definition.modulePath))
                return DeserializeError::BadEscape;
            std::uint64_t fileSize = 0, fileTime = 0;
            const auto signedMax = static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
            if (!ParsePositive(fields[11], definition.moduleSize) || !ParsePositive(fields[12], fileSize)
                || fileSize > signedMax || !ParsePositive(fields[13], fileTime) || fileTime > signedMax)
                return DeserializeError::BadPointerChain;
            definition.moduleFileSize = static_cast<std::int64_t>(fileSize);
            definition.moduleFileTime = static_cast<std::int64_t>(fileTime);
            if (fields[14] != "4" && fields[14] != "8") return DeserializeError::BadPointerChain;
            definition.pointerSize = fields[14] == "4" ? 4U : 8U;
            if (fields[15].empty() || fields[15].size() > ksword::pointer_chain::MaxDepth * 20)
                return DeserializeError::BadPointerChain;
            std::size_t start = 0;
            while (true) {
                if (definition.offsets.size() >= ksword::pointer_chain::MaxDepth)
                    return DeserializeError::BadPointerChain;
                const auto end = fields[15].find(',', start);
                const auto part = fields[15].substr(start, end == std::string_view::npos ? end : end - start);
                std::int64_t offset = 0;
                if (!ParseOffset(part, offset)) return DeserializeError::BadPointerChain;
                definition.offsets.push_back(offset);
                if (end == std::string_view::npos) break;
                start = end + 1;
            }
            entry.pointerChain = std::move(definition);
        }
    }
    entryOut = std::move(entry);
    return DeserializeError::None;
}

// MakeFailure：构造失败结果。传入行号与原因，传出填好文本的 DeserializeResult。
DeserializeResult MakeFailure(const std::size_t line, const DeserializeError code) {
    DeserializeResult result;
    result.ok = false;
    result.errorLine = line;
    result.errorCode = code;
    result.errorText = DeserializeErrorText(code);
    return result;
}

} // namespace

// ------------------------------------------------------------
// 写出
// ------------------------------------------------------------

std::string MemoryAddressBook::Serialize(const AddressFilter& filter) const {
    // 标题行先行；之后每个满足过滤条件的条目写一行，顺序即插入顺序。
    // text：累积的输出文本。
    std::string text;
    const auto entries = List(filter);
    const bool version2 = std::any_of(entries.begin(), entries.end(),
        [](const AddressEntry& entry) { return entry.pointerChain.has_value(); });
    text += version2 ? kHeaderLineV2 : kHeaderLineV1;
    text.push_back('\n');
    for (const AddressEntry& entry : entries) {
        // 字段顺序固定：id kind valueType targetKey moduleName rva absolute note。
        text += std::to_string(entry.id);
        text.push_back('\t');
        text += EntryKindName(entry.kind);
        text.push_back('\t');
        text += ValueTypeName(entry.valueType);
        text.push_back('\t');
        AppendEscaped(text, entry.targetKey);
        text.push_back('\t');
        AppendEscaped(text, entry.moduleName);
        text.push_back('\t');
        text += FormatHexAddress(entry.rva);
        text.push_back('\t');
        text += FormatHexAddress(entry.absoluteAddress);
        text.push_back('\t');
        AppendEscaped(text, entry.note);
        if (version2) {
            if (!entry.pointerChain) {
                text += "\t\t\t\t\t\t\t\t";
            } else {
                const auto& definition = *entry.pointerChain;
                text += "\tpointer\t";
                AppendEscaped(text, definition.processPath);
                text.push_back('\t');
                AppendEscaped(text, definition.modulePath);
                text.push_back('\t');
                text += std::to_string(definition.moduleSize);
                text.push_back('\t');
                text += std::to_string(definition.moduleFileSize);
                text.push_back('\t');
                text += std::to_string(definition.moduleFileTime);
                text.push_back('\t');
                text += std::to_string(definition.pointerSize);
                text.push_back('\t');
                for (std::size_t i = 0; i < definition.offsets.size(); ++i) {
                    if (i != 0) text.push_back(',');
                    text += FormatOffset(definition.offsets[i]);
                }
            }
        }
        text.push_back('\n');
    }
    return text;
}

// ------------------------------------------------------------
// 解析
// ------------------------------------------------------------

DeserializeResult MemoryAddressBook::Deserialize(
    const std::string_view text,
    MemoryAddressBook& out) {
    // 空输入连标题行都没有，行号记为 1。
    if (text.empty()) {
        return MakeFailure(1, DeserializeError::EmptyInput);
    }

    // 解析到临时簿里，全部通过才替换 out——失败路径上 out 一个字节都不会被碰。
    // loaded：解析中的临时簿，成功后才替换 out。
    MemoryAddressBook loaded;
    // maxId：目前见到的最大 id，决定载入后的下一个 id。
    std::uint64_t maxId = 0;
    // lineNumber：当前行号，从 1 开始，含标题行，出错时原样报告。
    std::size_t lineNumber = 0;
    // lineStart：当前行在 text 里的起始下标。
    std::size_t lineStart = 0;
    bool version2 = false;

    // 逐行处理。每行必须以 LF 结尾；找不到 LF 说明这是被截断的最后一行。
    while (lineStart < text.size()) {
        ++lineNumber;
        // newlinePosition：当前行行尾 LF 的位置。
        const std::size_t newlinePosition = text.find('\n', lineStart);
        if (newlinePosition == std::string_view::npos) {
            return MakeFailure(lineNumber, DeserializeError::UnterminatedLine);
        }
        // line：当前行的内容，不含行尾 LF。
        const std::string_view line = text.substr(lineStart, newlinePosition - lineStart);
        lineStart = newlinePosition + 1;

        // 裸回车：Serialize 从不写出它（字段里的回车被转义为 \r），所以它只可能来自
        // 文本模式转换。拒绝，而不是让每条备注悄悄多出一个回车。
        if (line.find('\r') != std::string_view::npos) {
            return MakeFailure(lineNumber, DeserializeError::RawCarriageReturn);
        }

        // 第 1 行是标题行。
        if (lineNumber == 1) {
            const DeserializeError headerError = CheckHeaderLine(line);
            if (headerError != DeserializeError::None) {
                return MakeFailure(lineNumber, headerError);
            }
            version2 = line == kHeaderLineV2;
            if (version2 && text.size() > kAddressBookV2TextLimit)
                return MakeFailure(lineNumber, DeserializeError::InputTooLarge);
            continue;
        }

        // 其余是条目行：解析、查重、入临时簿。
        // entry：这一行解析出的条目。
        AddressEntry entry;
        // lineError：这一行的解析结论，None 表示合法。
        const DeserializeError lineError = ParseEntryLine(line, version2, entry);
        if (lineError != DeserializeError::None) {
            return MakeFailure(lineNumber, lineError);
        }
        if (!ValidPointerChain(entry)) return MakeFailure(lineNumber, DeserializeError::BadPointerChain);
        if (loaded.entries_.count(entry.id) != 0) {
            return MakeFailure(lineNumber, DeserializeError::DuplicateId);
        }
        // entryId：先把 id 取出来，entry 下一行就被移动走了。
        const std::uint64_t entryId = entry.id;
        maxId = std::max(maxId, entryId);
        loaded.order_.push_back(entryId);
        loaded.entries_.emplace(entryId, std::move(entry));
    }

    // 下一个 id：必须大于文件里的最大 id；若 out 之前已经分配到更大的号段，则沿用，
    // 以免载入文件让 out 曾经发出过的 id 重新出现。maxId < 上限哨兵，所以 +1 不会回绕。
    loaded.nextId_ = std::max(out.nextId_, maxId + 1);
    out = std::move(loaded);

    // result：成功结果，errorLine 为 0、errorCode 为 None、errorText 为空。
    DeserializeResult result;
    result.ok = true;
    return result;
}

} // namespace ksword::memwb
