// 统一地址簿 v1 文本格式的离线测试（写出、往返、解析、拒绝）。
// 条目管理 / 地址解析 / 轮询策略的用例在 MemoryAddressBookTests.cpp，本文件并入同一个套件。
//
// 持久化最怕两件事，下面的用例分别钉死：
//   * 写出再读回不是同一份数据（备注里的制表符 / 换行 / 反斜杠 / emoji 被吃掉或错位）；
//   * 读到一个损坏的文件时，把内存里用户已有的书签污染了一半，却报告"成功"或只报一个模糊的错误。
//
// 拒绝用例的结构是固定的：标题 + 两条合法行 + 一条被改坏的行。
//   这样既能断言"出错行号 = 坏行的行号"，也能抓出"边解析边写入"的实现——它会在
//   报错前已经把前两条写进了 out。

#include "MemoryAddressBookTests.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {

using MemwbAddressBookTestSupport::Draft;
using MemwbAddressBookTestSupport::IdsOf;
using MemwbAddressBookTestSupport::kU64Max;
using MemwbAddressBookTestSupport::SameEntry;
using MemwbAddressBookTestSupport::SameList;
using ksword::memwb::AddressEntry;
using ksword::memwb::AddressFilter;
using ksword::memwb::DeserializeError;
using ksword::memwb::DeserializeResult;
using ksword::memwb::EntryKind;
using ksword::memwb::MemoryAddressBook;
using ksword::memwb::ValueType;

using Ids = std::vector<std::uint64_t>;

// 合法的 v1 标题行（含 LF）。
const std::string kHeader = "KSWORD-ADDRESS-BOOK 1\n";

// Fields：一条条目行的 8 个字段，默认值全部合法；用例只改其中一个字段来制造单一缺陷。
struct Fields {
    std::string id = "5";
    std::string kind = "bookmark";
    std::string valueType = "u32";
    std::string target = "game.exe";
    std::string module = "game.dll";
    std::string rva = "0x10";
    std::string absolute = "0x0";
    std::string note = "n";
};

// Row：把 Fields 拼成一行（含 LF），制表符分隔。这是手写文件的脚手架，不是被测逻辑。
std::string Row(const Fields& f) {
    return f.id + "\t" + f.kind + "\t" + f.valueType + "\t" + f.target + "\t" + f.module + "\t"
        + f.rva + "\t" + f.absolute + "\t" + f.note + "\n";
}

// GoodRow：默认合法字段、仅 id 不同的一行。
std::string GoodRow(const char* id) {
    Fields fields;
    fields.id = id;
    return Row(fields);
}

// TextWithBadRow：标题 + id 1、2 两条合法行 + 一条坏行 = 坏行在第 4 行。
std::string TextWithBadRow(const std::string& badRow) {
    return kHeader + GoodRow("1") + GoodRow("2") + badRow;
}

// MakePopulatedBook：造一个"用户已有数据"的簿：id 1、2 在册，id 3 加了又删，下一个 id 是 4。
MemoryAddressBook MakePopulatedBook() {
    MemoryAddressBook book;
    book.Add(Draft(EntryKind::Bookmark, "keep.exe", "keep.dll", 0x99, 0, "precious", ValueType::U64));
    book.Add(Draft(EntryKind::Watch, "keep.exe", "", 0, 0x5000, "also precious", ValueType::F32));
    const std::uint64_t scratch = book.Add(Draft(EntryKind::Search, "keep.exe", "", 0, 0x6000, ""));
    book.Remove(scratch);
    return book;
}

// Label：给"同一类缺陷的第 N 个取值"生成区分开的断言标签。
std::wstring Label(const wchar_t* base, const std::size_t index) {
    return std::wstring(base) + L" #" + std::to_wstring(index);
}

// ExpectRejected：对一段文本断言 Deserialize 被拒绝，并同时检查出错行号、错误码、
// 诊断文本非空、以及传入的"已有数据"簿保持原样（条目、顺序、下一个 id 都不变）。
void ExpectRejected(
    KswordTests::Suite& suite,
    const std::wstring& label,
    const std::string& text,
    const std::size_t expectedLine,
    const DeserializeError expectedCode) {
    MemoryAddressBook out = MakePopulatedBook();
    const std::vector<AddressEntry> before = out.List();
    const std::uint64_t nextBefore = out.NextId();

    const DeserializeResult result = MemoryAddressBook::Deserialize(text, out);
    suite.expect(!result.ok, (label + L": is rejected").c_str());
    suite.expect(result.errorLine == expectedLine, (label + L": reports the offending line").c_str());
    suite.expect(result.errorCode == expectedCode, (label + L": reports the right error code").c_str());
    suite.expect(!result.errorText.empty(), (label + L": carries a diagnostic text").c_str());
    suite.expect(SameList(out.List(), before) && out.Size() == before.size() && out.NextId() == nextBefore,
        (label + L": leaves the existing book untouched").c_str());
}

// RejectFieldValues：把 Fields 的某一个字段依次换成一组坏值，每个都应在第 4 行以 code 被拒绝。
void RejectFieldValues(
    KswordTests::Suite& suite,
    const wchar_t* label,
    std::string Fields::*member,
    const std::initializer_list<const char*> badValues,
    const DeserializeError code) {
    std::size_t index = 0;
    for (const char* value : badValues) {
        Fields fields;
        fields.*member = value;
        ExpectRejected(suite, Label(label, index), TextWithBadRow(Row(fields)), 4, code);
        ++index;
    }
}

// ------------------------------------------------------------
// 一、枚举记号：它们是文件格式的一部分，必须逐个钉死。
// ------------------------------------------------------------
void TestEnumTokens(KswordTests::Suite& suite) {
    struct KindToken {
        EntryKind kind;
        const char* token;
    };
    const KindToken kinds[] = {
        { EntryKind::Search, "search" },
        { EntryKind::Bookmark, "bookmark" },
        { EntryKind::Watch, "watch" },
    };
    for (const KindToken& item : kinds) {
        EntryKind parsed = EntryKind::Search;
        suite.expect(std::string(ksword::memwb::EntryKindName(item.kind)) == item.token,
            L"tokens: each kind is written with its fixed lowercase token");
        suite.expect(ksword::memwb::ParseEntryKind(item.token, parsed) && parsed == item.kind,
            L"tokens: each kind token parses back to the same kind");
    }

    struct TypeToken {
        ValueType type;
        const char* token;
    };
    const TypeToken types[] = {
        { ValueType::Hex8, "hex8" }, { ValueType::U8, "u8" }, { ValueType::U16, "u16" },
        { ValueType::U32, "u32" }, { ValueType::U64, "u64" }, { ValueType::I8, "i8" },
        { ValueType::I16, "i16" }, { ValueType::I32, "i32" }, { ValueType::I64, "i64" },
        { ValueType::F32, "f32" }, { ValueType::F64, "f64" },
    };
    for (const TypeToken& item : types) {
        ValueType parsed = ValueType::Hex8;
        suite.expect(std::string(ksword::memwb::ValueTypeName(item.type)) == item.token,
            L"tokens: each value type is written with its fixed lowercase token");
        // 先把 parsed 放在一个与目标不同的值上，避免"本来就是它"造成误通过。
        parsed = (item.type == ValueType::F64) ? ValueType::U8 : ValueType::F64;
        suite.expect(ksword::memwb::ParseValueType(item.token, parsed) && parsed == item.type,
            L"tokens: each value type token parses back to the same type");
    }

    // 越界枚举写成 "unknown"；解析失败不改动输出参数；记号区分大小写、不容忍多余字符。
    suite.expect(std::string(ksword::memwb::EntryKindName(static_cast<EntryKind>(9))) == "unknown"
            && std::string(ksword::memwb::ValueTypeName(static_cast<ValueType>(99))) == "unknown",
        L"tokens: out-of-range enum values are named unknown");
    EntryKind keptKind = EntryKind::Watch;
    for (const char* bad : { "Search", "WATCH", "", "bookmark ", "unknown", "book" }) {
        suite.expect(!ksword::memwb::ParseEntryKind(bad, keptKind) && keptKind == EntryKind::Watch,
            L"tokens: a malformed kind token fails and leaves the output alone");
    }
    ValueType keptType = ValueType::I16;
    for (const char* bad : { "U32", "", "hex", "u128", "f32 ", "float" }) {
        suite.expect(!ksword::memwb::ParseValueType(bad, keptType) && keptType == ValueType::I16,
            L"tokens: a malformed value type token fails and leaves the output alone");
    }
}

// ------------------------------------------------------------
// 二、写出：完整文本手算写死。
// ------------------------------------------------------------
void TestSerializeExactText(KswordTests::Suite& suite) {
    // 空簿只有标题行。
    const MemoryAddressBook empty;
    suite.expect(empty.Serialize() == "KSWORD-ADDRESS-BOOK 1\n",
        L"serialize: an empty book is just the header line");

    // 两条条目，第二条备注里有 制表符 / 换行 / 反斜杠（a TAB b LF c \ d）。
    MemoryAddressBook book;
    book.Add(Draft(EntryKind::Bookmark, "game.exe", "game.dll", 0x1234, 0, "hello", ValueType::U32));
    book.Add(Draft(EntryKind::Watch, "game.exe", "", 0, 0x7FF6A0001000ULL, "a\tb\nc\\d", ValueType::F32));
    const std::string expected =
        "KSWORD-ADDRESS-BOOK 1\n"
        "1\tbookmark\tu32\tgame.exe\tgame.dll\t0x1234\t0x0\thello\n"
        "2\twatch\tf32\tgame.exe\t\t0x0\t0x7ff6a0001000\ta\\tb\\nc\\\\d\n";
    suite.expect(book.Serialize() == expected,
        L"serialize: the exact v1 text for a module entry and an escaped absolute entry");

    // 十六进制写法：小写、不补零、0 写成 0x0；回车被转义成 \r。
    MemoryAddressBook hexBook;
    hexBook.Add(Draft(EntryKind::Search, "t", "", 0, 0xABC, ""));
    hexBook.Add(Draft(EntryKind::Search, "t", "", 0, 0x10, "x\ry"));
    hexBook.Add(Draft(EntryKind::Search, "t", "m", kU64Max, 0, ""));
    const std::string hexExpected =
        "KSWORD-ADDRESS-BOOK 1\n"
        "1\tsearch\thex8\tt\t\t0x0\t0xabc\t\n"
        "2\tsearch\thex8\tt\t\t0x0\t0x10\tx\\ry\n"
        "3\tsearch\thex8\tt\tm\t0xffffffffffffffff\t0x0\t\n";
    suite.expect(hexBook.Serialize() == hexExpected,
        L"serialize: addresses are minimal lowercase hex and a carriage return is escaped");
}

// ------------------------------------------------------------
// 三、往返：特殊字符无损。
// ------------------------------------------------------------
void TestEscapeRoundTrip(KswordTests::Suite& suite) {
    // 汉字"中文备注"与 emoji 用十六进制转义写出 UTF-8 字节，避免依赖源文件编码；
    // 转义序列后面紧跟的字面量用字符串拼接隔开，防止十六进制转义吞掉后面的字符。
    const std::string chinese = "\xE4\xB8\xAD\xE6\x96\x87" "\xE5\xA4\x87\xE6\xB3\xA8";
    const std::string emoji = "\xF0\x9F\x98\x80";
    const std::string zwjFamily = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9";
    const std::vector<std::string> samples = {
        "",
        "plain",
        "tab\there",
        "line1\nline2",
        "cr\rlf\n",
        "back\\slash",
        "\\t",                    // 反斜杠加字母 t，不是制表符：读回来必须还是两个字符
        "\\n",
        "\\\\t",                  // 两个反斜杠加 t
        "ends with backslash\\",
        "\\",
        "\\\\",
        "\t\n\r\\",
        "  spaced  ",
        chinese,
        emoji,
        zwjFamily + " " + chinese + "\t" + emoji,
        std::string("nul\0inside", 10),
    };

    std::size_t index = 0;
    for (const std::string& sample : samples) {
        // 条目 1：三个字符串字段都带样本；条目 2：无模块（moduleName 为空）。
        MemoryAddressBook book;
        AddressEntry withModule = Draft(EntryKind::Bookmark, "", "", 0x42, 0, "", ValueType::U16);
        withModule.targetKey = sample;
        withModule.moduleName = "mod" + sample;
        withModule.note = sample;
        AddressEntry withoutModule = Draft(EntryKind::Watch, "", "", 0, 0x7000, "", ValueType::I64);
        withoutModule.targetKey = sample;
        withoutModule.note = sample;
        book.Add(withModule);
        book.Add(withoutModule);

        // 转义之后正文里不能再有裸换行 / 制表符：恰好 3 个 LF（标题 + 2 行），每行恰好 7 个制表符。
        const std::string text = book.Serialize();
        suite.expect(std::count(text.begin(), text.end(), '\n') == 3,
            Label(L"round trip: escaped text keeps exactly one LF per line", index).c_str());
        suite.expect(std::count(text.begin(), text.end(), '\t') == 14,
            Label(L"round trip: escaped text keeps exactly seven tabs per entry line", index).c_str());
        suite.expect(text.find('\r') == std::string::npos,
            Label(L"round trip: no raw carriage return is ever written", index).c_str());

        MemoryAddressBook loaded;
        const DeserializeResult result = MemoryAddressBook::Deserialize(text, loaded);
        suite.expect(result.ok, Label(L"round trip: the written text loads back", index).c_str());
        suite.expect(SameList(loaded.List(), book.List()),
            Label(L"round trip: every field survives byte for byte", index).c_str());
        ++index;
    }
}

// ------------------------------------------------------------
// 四、往返：整本簿（id 间隙、所有 kind / valueType、极端地址）。
// ------------------------------------------------------------
void TestWholeBookRoundTrip(KswordTests::Suite& suite) {
    // 11 个 valueType 各一条，kind 轮流取三种；末尾两条放极端地址。
    const ValueType allTypes[] = {
        ValueType::Hex8, ValueType::U8, ValueType::U16, ValueType::U32, ValueType::U64, ValueType::I8,
        ValueType::I16, ValueType::I32, ValueType::I64, ValueType::F32, ValueType::F64,
    };
    const EntryKind kindCycle[] = { EntryKind::Search, EntryKind::Bookmark, EntryKind::Watch };
    MemoryAddressBook book;
    std::size_t counter = 0;
    for (const ValueType type : allTypes) {
        book.Add(Draft(kindCycle[counter % 3], "game.exe", "", 0, 0x1000 + counter, "n", type));
        ++counter;
    }
    book.Add(Draft(EntryKind::Bookmark, "game.exe", "", 0, kU64Max, "max abs"));
    book.Add(Draft(EntryKind::Bookmark, "game.exe", "m.dll", kU64Max, 0, "max rva"));

    // 删掉两条制造 id 间隙：id 2 与 id 5。最大 id 仍是 13。
    book.Remove(2);
    book.Remove(5);

    const std::string text = book.Serialize();
    MemoryAddressBook loaded;
    const DeserializeResult result = MemoryAddressBook::Deserialize(text, loaded);
    suite.expect(result.ok, L"whole book: the written text loads back");
    suite.expect(loaded.Size() == 11U && SameList(loaded.List(), book.List()),
        L"whole book: ids with gaps, order, kinds, types and extreme addresses all survive");
    suite.expect(loaded.Serialize() == text,
        L"whole book: writing the loaded book reproduces the exact same text");
    suite.expect(loaded.NextId() == 14ULL && book.NextId() == 14ULL,
        L"whole book: the next id after loading is the highest id plus one");
    suite.expect(loaded.Add(Draft(EntryKind::Search, "t", "", 0, 1, "")) == 14ULL,
        L"whole book: the loaded book keeps handing out fresh ids");
}

// ------------------------------------------------------------
// 五、写出时过滤：只保存 Bookmark / Watch，id 原样保留。
// ------------------------------------------------------------
void TestSerializeFilter(KswordTests::Suite& suite) {
    MemoryAddressBook book;
    book.Add(Draft(EntryKind::Search, "a.exe", "", 0, 0x10, "temp"));
    book.Add(Draft(EntryKind::Bookmark, "a.exe", "", 0, 0x20, "keep"));
    book.Add(Draft(EntryKind::Watch, "b.exe", "", 0, 0x30, "watch"));

    AddressFilter saved;
    saved.kinds = { EntryKind::Bookmark, EntryKind::Watch };
    const std::string expected =
        "KSWORD-ADDRESS-BOOK 1\n"
        "2\tbookmark\thex8\ta.exe\t\t0x0\t0x20\tkeep\n"
        "3\twatch\thex8\tb.exe\t\t0x0\t0x30\twatch\n";
    suite.expect(book.Serialize(saved) == expected,
        L"serialize filter: the temporary search entry is left out and ids are preserved");

    AddressFilter onlyB;
    onlyB.targetKey = std::string("b.exe");
    suite.expect(book.Serialize(onlyB) ==
            "KSWORD-ADDRESS-BOOK 1\n3\twatch\thex8\tb.exe\t\t0x0\t0x30\twatch\n",
        L"serialize filter: a target filter writes only that target");

    // 过滤后的文本读回来：id 保持 2、3，下一个 id 是 4。
    MemoryAddressBook loaded;
    suite.expect(MemoryAddressBook::Deserialize(expected, loaded).ok,
        L"serialize filter: the filtered text is a valid book");
    suite.expect(IdsOf(loaded.List()) == Ids({ 2, 3 }) && loaded.NextId() == 4ULL,
        L"serialize filter: loaded ids stay 2 and 3 and the next id is 4");
}

// ------------------------------------------------------------
// 六、拒绝：每类损坏输入都要被拒绝、报对行号、且 out 不变。
// ------------------------------------------------------------
void TestRejectedHeaders(KswordTests::Suite& suite) {
    ExpectRejected(suite, L"header: empty input", "", 1, DeserializeError::EmptyInput);
    ExpectRejected(suite, L"header: blank first line", "\n", 1, DeserializeError::BadHeader);
    ExpectRejected(suite, L"header: wrong magic", "HELLO 1\n", 1, DeserializeError::BadHeader);
    ExpectRejected(suite, L"header: no version", "KSWORD-ADDRESS-BOOK\n", 1, DeserializeError::BadHeader);
    ExpectRejected(suite, L"header: magic and space but no version", "KSWORD-ADDRESS-BOOK \n", 1,
        DeserializeError::BadHeader);
    ExpectRejected(suite, L"header: lowercase magic", "ksword-address-book 1\n", 1,
        DeserializeError::BadHeader);

    // 魔数对、版本不是 1：一律是"未知版本"。
    const char* const versions[] = { "3", "0", "10", "01", "1 ", "1.0", "v1" };
    std::size_t index = 0;
    for (const char* version : versions) {
        const std::string text = std::string("KSWORD-ADDRESS-BOOK ") + version + "\n";
        ExpectRejected(suite, Label(L"header: unknown version", index), text, 1,
            DeserializeError::UnsupportedVersion);
        ++index;
    }

    // 没有行尾 LF：标题行被截断。
    ExpectRejected(suite, L"header: no trailing LF", "KSWORD-ADDRESS-BOOK 1", 1,
        DeserializeError::UnterminatedLine);
}

void TestRejectedTruncationAndLineEndings(KswordTests::Suite& suite) {
    // 最后一条条目缺行尾 LF：行号是那条的行号（第 3 行）。
    const std::string noFinalLf = kHeader + GoodRow("1") + "2\tbookmark\tu32\tgame.exe\tgame.dll\t0x10\t0x0\tn";
    ExpectRejected(suite, L"truncation: last entry has no LF", noFinalLf, 3, DeserializeError::UnterminatedLine);

    // 在一行中间被截断。
    const std::string cutMidLine = kHeader + GoodRow("1") + GoodRow("2").substr(0, 12);
    ExpectRejected(suite, L"truncation: file cut in the middle of a line", cutMidLine, 3,
        DeserializeError::UnterminatedLine);

    // 整个文件被转成 CRLF：第 1 行就拒绝，而不是悄悄让每条备注末尾多一个回车。
    const std::string crlfHeader = "KSWORD-ADDRESS-BOOK 1\r\n" + GoodRow("1");
    ExpectRejected(suite, L"line ending: CRLF header", crlfHeader, 1, DeserializeError::RawCarriageReturn);

    // 只有后面某一行带 CR：行号指向它（第 3 行）。
    std::string crlfRow = GoodRow("2");
    crlfRow.insert(crlfRow.size() - 1, "\r");
    ExpectRejected(suite, L"line ending: CRLF on a later entry", kHeader + GoodRow("1") + crlfRow, 3,
        DeserializeError::RawCarriageReturn);

    // 字段中间的裸回车。
    Fields midCr;
    midCr.note = "a\rb";
    ExpectRejected(suite, L"line ending: raw CR inside a note", TextWithBadRow(Row(midCr)), 4,
        DeserializeError::RawCarriageReturn);
}

void TestRejectedFieldCounts(KswordTests::Suite& suite) {
    // 7 个字段（缺备注）。
    ExpectRejected(suite, L"fields: seven fields",
        TextWithBadRow("5\tbookmark\tu32\tgame.exe\tgame.dll\t0x10\t0x0\n"), 4,
        DeserializeError::WrongFieldCount);

    // 9 个字段（多一个制表符，或末尾多一个）。
    ExpectRejected(suite, L"fields: nine fields (extra field)",
        TextWithBadRow("5\tbookmark\tu32\tgame.exe\tgame.dll\t0x10\t0x0\tn\textra\n"), 4,
        DeserializeError::WrongFieldCount);
    ExpectRejected(suite, L"fields: nine fields (trailing tab)",
        TextWithBadRow("5\tbookmark\tu32\tgame.exe\tgame.dll\t0x10\t0x0\tn\t\n"), 4,
        DeserializeError::WrongFieldCount);

    // 空行、没有制表符的一行：都是 1 个字段。
    ExpectRejected(suite, L"fields: blank line in the middle", TextWithBadRow("\n"), 4,
        DeserializeError::WrongFieldCount);
    ExpectRejected(suite, L"fields: a line with no tabs", TextWithBadRow("justonefield\n"), 4,
        DeserializeError::WrongFieldCount);
    // 空行夹在合法行之间：行号是空行自己。
    ExpectRejected(suite, L"fields: blank line before a valid entry",
        kHeader + "\n" + GoodRow("1"), 2, DeserializeError::WrongFieldCount);
}

void TestRejectedFieldValues(KswordTests::Suite& suite) {
    // id：非数字、空、零、带符号、带空白、小数、十六进制、上限哨兵、溢出，一个都不能放过。
    RejectFieldValues(suite, L"id", &Fields::id,
        { "abc", "", "0", "-1", "+1", " 1", "1 ", "1.5", "0x1", "18446744073709551615",
          "18446744073709551616", "99999999999999999999" },
        DeserializeError::BadId);

    // kind / valueType：区分大小写、不容忍空白、空串。
    RejectFieldValues(suite, L"kind", &Fields::kind,
        { "Search", "SEARCH", "foo", "", "bookmark ", "watching" }, DeserializeError::UnknownKind);
    RejectFieldValues(suite, L"value type", &Fields::valueType,
        { "u128", "U32", "", "hex", "f32 ", "double" }, DeserializeError::UnknownValueType);

    // 地址：无 0x、0x 后无数位、非十六进制字符、17 位（越界）、负号、内部空白、大写 0X。
    const std::initializer_list<const char*> badAddresses = {
        "1234", "0x", "0xg", "0x12345678901234567", "0x10000000000000000", "-0x1", "0x 1", "0X10", "",
        "0x+1", "0x1_0", "0x00000000000000010",
    };
    RejectFieldValues(suite, L"rva", &Fields::rva, badAddresses, DeserializeError::BadAddress);
    RejectFieldValues(suite, L"absolute", &Fields::absolute, badAddresses, DeserializeError::BadAddress);

    // 转义：未知转义、末尾孤立反斜杠，三个字符串字段各测一遍。
    RejectFieldValues(suite, L"note escape", &Fields::note,
        { "a\\q", "trail\\", "\\0", "\\u0041", "\\ " }, DeserializeError::BadEscape);
    RejectFieldValues(suite, L"target escape", &Fields::target, { "\\x", "t\\" }, DeserializeError::BadEscape);
    RejectFieldValues(suite, L"module escape", &Fields::module, { "m\\", "\\z.dll" }, DeserializeError::BadEscape);
}

void TestRejectedInconsistencyAndDuplicates(KswordTests::Suite& suite) {
    // 有模块名却带非零绝对地址；无模块名却带非零 rva：互相矛盾。
    Fields moduleWithAbsolute;
    moduleWithAbsolute.absolute = "0x10";
    ExpectRejected(suite, L"consistency: module entry with an absolute address",
        TextWithBadRow(Row(moduleWithAbsolute)), 4, DeserializeError::InconsistentAddress);
    Fields plainWithRva;
    plainWithRva.module = "";
    plainWithRva.rva = "0x10";
    ExpectRejected(suite, L"consistency: module-less entry with an rva",
        TextWithBadRow(Row(plainWithRva)), 4, DeserializeError::InconsistentAddress);

    // 重复 id：报第二次出现的行号；前导零写法按数值判重。
    ExpectRejected(suite, L"duplicate: same id as line 3", TextWithBadRow(GoodRow("2")), 4,
        DeserializeError::DuplicateId);
    ExpectRejected(suite, L"duplicate: same id as line 2", TextWithBadRow(GoodRow("1")), 4,
        DeserializeError::DuplicateId);
    ExpectRejected(suite, L"duplicate: leading zeros do not make a different id",
        TextWithBadRow(GoodRow("02")), 4, DeserializeError::DuplicateId);
}

// ------------------------------------------------------------
// 七、成功路径：顺序、下一个 id、替换语义、边界。
// ------------------------------------------------------------
void TestAcceptedInputs(KswordTests::Suite& suite) {
    // 文件里 id 乱序（5、3、9）：按文件顺序保留，下一个 id = 最大 id + 1 = 10。
    Fields first;
    first.id = "5";
    first.kind = "search";
    first.valueType = "hex8";
    first.target = "x.exe";
    first.module = "";
    first.rva = "0x0";
    first.absolute = "0x401000";
    first.note = "";
    Fields second;
    second.id = "3";
    Fields third;
    third.id = "9";
    third.kind = "watch";
    third.valueType = "f64";
    third.target = "";
    third.module = "";
    third.rva = "0x0";
    third.absolute = "0xFFFFFFFFFFFFFFFF";
    third.note = "w";

    MemoryAddressBook fresh;
    const DeserializeResult result =
        MemoryAddressBook::Deserialize(kHeader + Row(first) + Row(second) + Row(third), fresh);
    suite.expect(result.ok && result.errorLine == 0U && result.errorCode == DeserializeError::None
            && result.errorText.empty(),
        L"accept: a good file reports success with no error fields");
    suite.expect(IdsOf(fresh.List()) == Ids({ 5, 3, 9 }),
        L"accept: entries keep file order even when ids are not ascending");
    suite.expect(fresh.NextId() == 10ULL, L"accept: the next id is the highest id in the file plus one");
    suite.expect(fresh.Add(Draft(EntryKind::Search, "t", "", 0, 1, "")) == 10ULL,
        L"accept: the next Add hands out exactly that id");

    // 逐字段核对三条（手算期望值）。
    const auto entry5 = fresh.Find(5);
    suite.expect(entry5.has_value() && entry5->kind == EntryKind::Search && entry5->valueType == ValueType::Hex8
            && entry5->targetKey == "x.exe" && entry5->moduleName.empty() && entry5->rva == 0ULL
            && entry5->absoluteAddress == 0x401000ULL && entry5->note.empty(),
        L"accept: an entry with empty module and empty note is read field by field");
    const auto entry3 = fresh.Find(3);
    suite.expect(entry3.has_value() && entry3->kind == EntryKind::Bookmark && entry3->valueType == ValueType::U32
            && entry3->moduleName == "game.dll" && entry3->rva == 0x10ULL && entry3->absoluteAddress == 0ULL,
        L"accept: a module entry is read field by field");
    const auto entry9 = fresh.Find(9);
    suite.expect(entry9.has_value() && entry9->kind == EntryKind::Watch && entry9->valueType == ValueType::F64
            && entry9->targetKey.empty() && entry9->absoluteAddress == kU64Max && entry9->note == "w",
        L"accept: uppercase hex digits and the maximum address are read correctly");

    // 替换语义：载入前已有 id 1、2（下一个 id 为 4）；文件只含 id 1。
    // 旧的 id 2 消失；下一个 id 仍是 4（不回退到 2，曾经发出过的号段不重现）。
    MemoryAddressBook used = MakePopulatedBook();
    Fields replacement;
    replacement.id = "1";
    replacement.target = "new.exe";
    suite.expect(MemoryAddressBook::Deserialize(kHeader + Row(replacement), used).ok,
        L"accept: loading into a used book succeeds");
    suite.expect(IdsOf(used.List()) == Ids({ 1 }) && used.Find(1)->targetKey == "new.exe",
        L"accept: loading replaces the previous contents");
    suite.expect(!used.Find(2).has_value(), L"accept: an entry that is not in the file is gone");
    suite.expect(used.NextId() == 4ULL, L"accept: loading never lowers the next id below what was handed out");

    // 文件里的最大 id 超过已发出的号段：下一个 id 以文件为准。
    MemoryAddressBook other = MakePopulatedBook();
    Fields big;
    big.id = "50";
    suite.expect(MemoryAddressBook::Deserialize(kHeader + Row(big), other).ok && other.NextId() == 51ULL,
        L"accept: a file with a higher id moves the next id past it");

    // 只有标题行：合法，清空簿，下一个 id 不变。
    MemoryAddressBook cleared = MakePopulatedBook();
    suite.expect(MemoryAddressBook::Deserialize(kHeader, cleared).ok && cleared.Size() == 0U
            && cleared.NextId() == 4ULL,
        L"accept: a header-only file empties the book and keeps the id counter");
}

void TestAcceptedBoundaries(KswordTests::Suite& suite) {
    // id 上限：UINT64_MAX - 1 是最大的合法 id；载入后号段用尽，Add 返回 0 而不是回绕复用。
    Fields topId;
    topId.id = "18446744073709551614";
    MemoryAddressBook exhausted;
    suite.expect(MemoryAddressBook::Deserialize(kHeader + Row(topId), exhausted).ok,
        L"boundary: the largest legal id is accepted");
    suite.expect(exhausted.NextId() == ksword::memwb::kAddressBookIdLimit,
        L"boundary: the id counter stops at the limit sentinel");
    suite.expect(exhausted.Add(Draft(EntryKind::Search, "t", "", 0, 1, "")) == 0ULL && exhausted.Size() == 1U,
        L"boundary: an exhausted id range makes Add fail instead of wrapping");

    // 前导零：id 与 16 位以内的十六进制都允许（按数值解释）。
    Fields padded;
    padded.id = "007";
    padded.rva = "0x0000000000000010";
    MemoryAddressBook book;
    suite.expect(MemoryAddressBook::Deserialize(kHeader + Row(padded), book).ok,
        L"boundary: leading zeros are accepted in id and in a 16-digit hex");
    suite.expect(book.Find(7).has_value() && book.Find(7)->rva == 0x10ULL,
        L"boundary: leading zeros do not change the value");

    // 16 位全 F 是合法的最大值（上面已验证 17 位被拒绝，两侧都有）。
    Fields maxRva;
    maxRva.rva = "0xFFFFFFFFFFFFFFFF";
    MemoryAddressBook maxBook;
    suite.expect(MemoryAddressBook::Deserialize(kHeader + Row(maxRva), maxBook).ok
            && maxBook.Find(5)->rva == kU64Max,
        L"boundary: a 16-digit all-F rva is accepted as UINT64_MAX");

    // 全空字段（目标、模块、备注都为空）：末尾的空字段不能被当成"字段数不够"。
    MemoryAddressBook blank;
    const std::string blankText = kHeader + "1\tsearch\thex8\t\t\t0x0\t0x1000\t\n";
    suite.expect(MemoryAddressBook::Deserialize(blankText, blank).ok && blank.Size() == 1U,
        L"boundary: empty target, module and note still make a valid entry line");

    // 转义解码：反斜杠加 t 是制表符；两个反斜杠加 t 是"反斜杠 + 字母 t"，不是制表符。
    Fields escapes;
    escapes.note = "a\\tb";
    Fields doubled;
    doubled.id = "6";
    doubled.note = "\\\\t";
    MemoryAddressBook escaped;
    suite.expect(MemoryAddressBook::Deserialize(kHeader + Row(escapes) + Row(doubled), escaped).ok,
        L"boundary: escaped notes load");
    suite.expect(escaped.Find(5)->note == "a\tb", L"boundary: backslash-t decodes to a tab");
    suite.expect(escaped.Find(6)->note == "\\t" && escaped.Find(6)->note.size() == 2U,
        L"boundary: double backslash then t decodes to a backslash and the letter t");
}

void TestPointerV2Serialization(KswordTests::Suite& suite) {
    using MemwbAddressBookTestSupport::PointerDraft;
    const std::string header = "KSWORD-ADDRESS-BOOK 2\n";
    MemoryAddressBook book;
    auto pointer = PointerDraft();
    pointer.pointerChain->offsets = {0, (std::numeric_limits<std::int64_t>::min)(),
        (std::numeric_limits<std::int64_t>::max)()};
    const auto pointerId = book.Add(pointer);
    book.Add(Draft(EntryKind::Watch, "ordinary", "", 0, 0x5000, "normal"));
    const auto text = book.Serialize();
    suite.expect(text.starts_with(header), L"pointer v2: mixed book upgrades to version 2");
    suite.expect(text.find("0x0,-0x8000000000000000,0x7fffffffffffffff") != std::string::npos,
        L"pointer v2: signed hexadecimal extremes are written without overflow");
    suite.expect(text.find("C:\\\\Game\\\\game.exe\tC:\\\\Game\\\\game.dll") != std::string::npos,
        L"pointer v2: full paths use existing lossless escaping");
    suite.expect(text.find("normal\t\t\t\t\t\t\t\t\n") != std::string::npos,
        L"pointer v2: ordinary rows append eight empty fixed fields");
    MemoryAddressBook loaded;
    suite.expect(MemoryAddressBook::Deserialize(text, loaded).ok && SameList(book.List(), loaded.List())
        && loaded.NextId() == book.NextId(), L"pointer v2: mixed entries and full definitions roundtrip");
    suite.expect(loaded.Serialize() == text, L"pointer v2: canonical serialization is stable");
    AddressFilter onlyWatch;
    onlyWatch.kinds = {EntryKind::Watch};
    suite.expect(book.Serialize(onlyWatch).starts_with(kHeader), L"pointer v2: a plain filtered export retains v1");
    suite.expect(book.Remove(pointerId) && book.Serialize().starts_with(kHeader),
        L"pointer v2: removing the last pointer restores legacy output");

    auto escaped = PointerDraft();
    escaped.note = "a\tb\nc\r\\note";
    escaped.pointerChain->processPath += "\t\n\r";
    escaped.pointerChain->modulePath += "\\t";
    escaped.pointerChain->moduleFileSize = (std::numeric_limits<std::int64_t>::max)();
    escaped.pointerChain->moduleFileTime = (std::numeric_limits<std::int64_t>::max)();
    escaped.pointerChain->moduleSize = kU64Max;
    escaped.pointerChain->pointerSize = 4;
    escaped.pointerChain->offsets.resize(16, -1);
    MemoryAddressBook escaping;
    escaping.Add(escaped);
    suite.expect(MemoryAddressBook::Deserialize(escaping.Serialize(), loaded).ok
        && SameList(escaping.List(), loaded.List()),
        L"pointer v2: escaped strings, positive metadata limits, width 4 and maximum depth roundtrip");

    MemoryAddressBook migrated;
    suite.expect(MemoryAddressBook::Deserialize(kHeader + GoodRow("5"), migrated).ok,
        L"pointer v2: preexisting v1 address book loads");
    suite.expect(migrated.SetPointerChain(5, *pointer.pointerChain, "game.dll", 0x100)
        && migrated.Serialize().starts_with(header) && migrated.Find(5)->valueType == ValueType::U32
        && migrated.Find(5)->note == "n", L"pointer v2: editing a v1 bookmark migrates without losing metadata");

    using Columns = std::vector<std::string>;
    const Columns validFields = {"5", "bookmark", "u32", "game.exe", "game.dll", "0x100", "0x0", "n",
        "pointer", "C:\\\\Game\\\\game.exe", "C:\\\\Game\\\\game.dll", "16384", "32768", "123456789", "8", "0x0,-0x10"};
    const auto row = [](const Columns& fields) {
        std::string result;
        for (std::size_t i = 0; i < fields.size(); ++i) { if (i) result += '\t'; result += fields[i]; }
        return result + '\n';
    };
    std::string normal = GoodRow("1");
    normal.pop_back();
    normal += "\t\t\t\t\t\t\t\t\n";
    std::size_t rejectionCase = 0;
    const auto reject = [&](const std::string& badText, const DeserializeError expected, std::size_t line = 3) {
        MemoryAddressBook existing;
        existing.Add(PointerDraft());
        existing.Add(Draft(EntryKind::Watch, "keep", "", 0, 0xABCD, "precious"));
        const auto before = existing.List();
        const auto next = existing.NextId();
        const auto result = MemoryAddressBook::Deserialize(badText, existing);
        const auto label = L"pointer v2: corrupt input case " + std::to_wstring(++rejectionCase);
        suite.expect(!result.ok && result.errorCode == expected && result.errorLine == line && !result.errorText.empty(),
            (label + L" reports exact error and line").c_str());
        suite.expect(SameList(before, existing.List()) && existing.NextId() == next,
            (label + L" preserves existing pointer entries and id counter").c_str());
    };
    const auto corruptField = [&](std::size_t index, const std::string& replacement,
                                  DeserializeError error = DeserializeError::BadPointerChain) {
        auto fields = validFields;
        fields[index] = replacement;
        reject(header + normal + row(fields), error);
    };
    for (const auto* kind : {"search", "watch"}) corruptField(1, kind);
    corruptField(4, "", DeserializeError::InconsistentAddress);
    corruptField(5, "0x4000");
    corruptField(5, "0x3ff9");
    corruptField(6, "0x1", DeserializeError::InconsistentAddress);
    corruptField(8, "chain");
    corruptField(8, "");
    for (const auto index : {9U, 10U}) {
        for (const auto* path : {"", "game.exe", "C:game.exe", "\\\\q"})
            corruptField(index, path);
        corruptField(index, "C:\\q", DeserializeError::BadEscape);
        corruptField(index, "C:/" + std::string(32766, 'x'));
    }
    for (const auto index : {11U, 12U, 13U})
        for (const auto* value : {"0", "-1", "+1", " 1", "1.0", "18446744073709551616"}) corruptField(index, value);
    corruptField(12, "9223372036854775808");
    corruptField(13, "9223372036854775808");
    for (const auto* width : {"0", "3", "16", "08", "8.0"}) corruptField(14, width);
    for (const auto* offsets : {"", "0", "-0x8000000000000001", "0x8000000000000000", "0xg", "0x1,",
        ",0x1", "0x1,,0x2", "0x1, 0x2", "0x00000000000000000", "0x1_0"}) corruptField(15, offsets);
    std::string deep = "0x0";
    for (int i = 1; i < 17; ++i) deep += ",0x0";
    corruptField(15, deep);
    corruptField(15, std::string(321, '0'));
    auto shortRow = validFields;
    shortRow.pop_back();
    reject(header + normal + row(shortRow), DeserializeError::WrongFieldCount);
    auto tooMany = validFields;
    tooMany.push_back("extra");
    reject(header + normal + row(tooMany), DeserializeError::WrongFieldCount);
    auto truncated = header + normal + row(validFields);
    truncated.pop_back();
    reject(truncated, DeserializeError::UnterminatedLine);
    reject(header + std::string(ksword::memwb::kAddressBookV2TextLimit, 'x'), DeserializeError::InputTooLarge, 1);
    suite.expect(MemoryAddressBook::Deserialize(header, loaded).ok && loaded.Size() == 0,
        L"pointer v2: an empty v2 book is valid");
}

} // namespace

// RunMemwbAddressBookSerializeTests：序列化相关用例的总入口，由 RunMemwbAddressBookTests 调用。
void RunMemwbAddressBookSerializeTests(KswordTests::Suite& suite) {
    TestEnumTokens(suite);
    TestSerializeExactText(suite);
    TestEscapeRoundTrip(suite);
    TestWholeBookRoundTrip(suite);
    TestSerializeFilter(suite);
    TestRejectedHeaders(suite);
    TestRejectedTruncationAndLineEndings(suite);
    TestRejectedFieldCounts(suite);
    TestRejectedFieldValues(suite);
    TestRejectedInconsistencyAndDuplicates(suite);
    TestAcceptedInputs(suite);
    TestAcceptedBoundaries(suite);
    TestPointerV2Serialization(suite);
}
