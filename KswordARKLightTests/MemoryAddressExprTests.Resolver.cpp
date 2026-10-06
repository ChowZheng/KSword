// 内存工作台地址表达式的离线测试 —— 需要 resolver 的那一半：模块 + 偏移、带引号模块、
// "abc 是数字而 "abc" 是模块"的分道、模块失败、解引用、嵌套深度、解引用失败。
//
// 断言原则见 MemoryAddressExprTests.cpp 顶部：期望值手算写死；边界两侧都测；
// 失败要同时核对错误码/偏移/详情，且 value 为 0、其余字段为初值；
// 用假 resolver 的调用记录证明"解析阶段的错误不会触发任何 resolver 调用"。

#include "MemoryAddressExprTestSupport.h"

namespace memwb_expr_test {
namespace {

using ksword::memwb::EvaluateAddressExpr;
using ksword::memwb::ExprError;
using ksword::memwb::ExprResult;

// ------------------------------------------------------------
// 六、模块 + 偏移：现状语法的行为迁移（最后加号分隔 -> 逐项求和）。
// ------------------------------------------------------------
void TestModulePlusOffset(KswordTests::Suite& suite) {
    FakeResolver resolver = MakeResolver();
    // 手算：0x7FF600000000 + 0xC125D9 = 0x7FF600C125D9。
    const ExprResult plain = Eval("client.dll+C125D9", &resolver);
    ExpectOk(suite, L"module plus offset", "client.dll+C125D9", plain, 0x7FF600C125D9ULL);
    suite.expect(plain.usedModule && plain.moduleName == "client.dll" && !plain.usedDeref,
        L"addr expr: module result reports the module and no deref");
    suite.expect(resolver.lookupCalls == 1 && resolver.lookupNames[0] == "client.dll",
        L"addr expr: the resolver is asked once, with the exact module name");

    // 偏移无论带不带 0x 一律十六进制：1233 是 0x1233，不是十进制 1233（= 0x4D1）。
    ExpectOk(suite, L"offset with 0x", "client.dll+0xC125D9",
        Eval("client.dll+0xC125D9", &resolver), 0x7FF600C125D9ULL);
    ExpectOk(suite, L"offset with 0X", "client.dll+0XC125D9",
        Eval("client.dll+0XC125D9", &resolver), 0x7FF600C125D9ULL);
    const ExprResult hexOffset = Eval("client.dll+1233", &resolver);
    ExpectOk(suite, L"bare offset is hex", "client.dll+1233", hexOffset, 0x7FF600001233ULL);
    suite.expect(hexOffset.value != kClientBase + 0x4D1ULL,
        L"addr expr: a bare module offset is never read as decimal");
    // 现状要求必须带偏移；新文法里单独的模块名是合法的一项，值就是基址。
    ExpectOk(suite, L"module alone is its base", "client.dll", Eval("client.dll", &resolver), kClientBase);
    ExpectOk(suite, L"module plus zero", "client.dll+0", Eval("client.dll+0", &resolver), kClientBase);
    ExpectOk(suite, L"multi-term offset", "client.dll+10+20",
        Eval("client.dll+10+20", &resolver), kClientBase + 0x30ULL);

    // 空白宽容。
    ExpectOk(suite, L"spaces around plus", "  client.dll + C125D9  ",
        Eval("  client.dll + C125D9  ", &resolver), 0x7FF600C125D9ULL);
    ExpectOk(suite, L"tabs and newlines around plus", "client.dll\t+\r\n10",
        Eval("client.dll\t+\r\n10", &resolver), kClientBase + 0x10ULL);

    // 名字原样传给 resolver，不改大小写（大小写是否敏感是 resolver 的事）。
    FakeResolver caseResolver = MakeResolver();
    ExpectOk(suite, L"module name keeps its case", "Client.DLL+10",
        Eval("Client.DLL+10", &caseResolver), 0x3333010ULL);
    suite.expect(caseResolver.lookupNames.size() == 1 && caseResolver.lookupNames[0] == "Client.DLL",
        L"addr expr: the module name reaches the resolver unchanged");

    // 含 '-' 的真实模块名：只要有非数字片段就是模块，不是减法。
    ExpectOk(suite, L"api-set module name", "api-ms-win-core-file-l1-1-0.dll+10",
        Eval("api-ms-win-core-file-l1-1-0.dll+10", &resolver), 0x7FFB00000010ULL);
    ExpectOk(suite, L"digit-led module with hyphen", "7-zip.dll+10",
        Eval("7-zip.dll+10", &resolver), 0x6000010ULL);
    // UTF-8 模块名原样透传。
    ExpectOk(suite, L"utf-8 module name", "\xE6\xB8\xB8\xE6\x88\x8F.dll+10",
        Eval("\xE6\xB8\xB8\xE6\x88\x8F.dll+10", &resolver), 0x4444010ULL);
    // 无引号的完整路径：含反斜杠与冒号都不是结构字符。
    ExpectOk(suite, L"unquoted full path", "C:\\Windows\\System32\\ntdll.dll+10",
        Eval("C:\\Windows\\System32\\ntdll.dll+10", &resolver), kNtdllBase + 0x10ULL);
}

// ------------------------------------------------------------
// 七、带引号的模块名，以及"abc 是数字、"abc" 是模块"的分道。
// ------------------------------------------------------------
void TestQuotedModulesAndLaneSplit(KswordTests::Suite& suite) {
    FakeResolver resolver = MakeResolver();
    ExpectOk(suite, L"double-quoted module", "\"client.dll\"+10",
        Eval("\"client.dll\"+10", &resolver), kClientBase + 0x10ULL);
    ExpectOk(suite, L"single-quoted module", "'client.dll'+10",
        Eval("'client.dll'+10", &resolver), kClientBase + 0x10ULL);
    // 引号内两端空白被剪掉（保持现状行为）；中间空白保留。
    resolver.lookupNames.clear();
    ExpectOk(suite, L"quoted name is trimmed", "\" client.dll \"+10",
        Eval("\" client.dll \"+10", &resolver), kClientBase + 0x10ULL);
    suite.expect(resolver.lookupNames.size() == 1 && resolver.lookupNames[0] == "client.dll",
        L"addr expr: the quotes and their padding are stripped before lookup");
    ExpectOk(suite, L"quoted name with a space", "\"my module.dll\"+10",
        Eval("\"my module.dll\"+10", &resolver), 0x2222010ULL);
    // 引号里可以含加号与路径反斜杠。
    const ExprResult plusInName = Eval("\"a+b.dll\"+10", &resolver);
    ExpectOk(suite, L"plus inside quotes", "\"a+b.dll\"+10", plusInName, 0x1111010ULL);
    suite.expect(plusInName.moduleName == "a+b.dll",
        L"addr expr: a plus inside quotes belongs to the module name");
    ExpectOk(suite, L"quoted full path", "\"C:\\Windows\\System32\\ntdll.dll\"+10",
        Eval("\"C:\\Windows\\System32\\ntdll.dll\"+10", &resolver), kNtdllBase + 0x10ULL);

    // 分道：无引号的 abc 整串是合法十六进制，一律是数字 0xABC，哪怕有个叫 abc 的模块。
    FakeResolver lane = MakeResolver();
    ExpectOk(suite, L"unquoted abc is the number", "abc", Eval("abc", &lane), 0xABCULL);
    suite.expect(lane.lookupCalls == 0, L"addr expr: unquoted abc does not consult the resolver");
    // 带引号的 "abc" 才是模块 abc（基址 0xAAAA0000）。
    const ExprResult quoted = Eval("\"abc\"", &lane);
    ExpectOk(suite, L"quoted abc is the module", "\"abc\"", quoted, 0xAAAA0000ULL);
    suite.expect(quoted.usedModule && quoted.moduleName == "abc" && lane.lookupCalls == 1,
        L"addr expr: quoted abc goes through the module lane exactly once");
    ExpectOk(suite, L"single-quoted abc is the module", "'abc'+10",
        Eval("'abc'+10", &lane), 0xAAAA0010ULL);
    // 同理：纯数字名字的模块也必须加引号。
    ExpectOk(suite, L"unquoted 1233 stays the number", "1233", Eval("1233", &lane), 0x1233ULL);
    ExpectOk(suite, L"quoted 1233 is the module", "\"1233\"", Eval("\"1233\"", &lane), 0x5000000ULL);
    // 一个非十六进制字符就足以让无引号记号变成模块名（abc.dll 含 '.'、'l'）。
    FakeResolver dotted = MakeResolver();
    ExpectFail(suite, L"abc.dll is a module name, not a number", "abc.dll",
        Eval("abc.dll", &dotted), ExprError::UnknownModule, 0, "abc.dll");
    suite.expect(dotted.lookupCalls == 1, L"addr expr: abc.dll is looked up as a module");
    // 0x 前缀后含非十六进制字符：整串字符集不合格，同样走模块通道，而不是 BadNumber。
    FakeResolver prefixed = MakeResolver();
    ExpectFail(suite, L"0x12g4 is a module name by the charset rule", "0x12g4",
        Eval("0x12g4", &prefixed), ExprError::UnknownModule, 0, "0x12g4");
    suite.expect(prefixed.lookupCalls == 1, L"addr expr: 0x12g4 is looked up as a module");
    // 但引号不闭合、空名都是语法错误。
    const FailCase quoteErrors[] = {
        { "\"client.dll'+10", ExprError::BadSyntax, 0, "\"client.dll'+10" },
        { "\"client.dll", ExprError::BadSyntax, 0, "\"client.dll" },
        { "\"\"", ExprError::BadSyntax, 0, "\"\"" },
        { "\"   \"", ExprError::BadSyntax, 0, "\"   \"" },
        { "10+\"\"", ExprError::BadSyntax, 3, "\"\"" },
    };
    for (const FailCase& item : quoteErrors) {
        FakeResolver errorResolver = MakeResolver();
        ExpectFail(suite, L"quote error is a syntax error", item.text,
            Eval(item.text, &errorResolver), item.error, item.position, item.detail);
        suite.expect(errorResolver.Untouched(), L"addr expr: a quote error never reaches the resolver");
    }
}

// ------------------------------------------------------------
// 八、模块失败：找不到 / 重名 / 需要进程 / 越界返回值 / 位置规则 / 基址溢出。
// ------------------------------------------------------------
void TestModuleFailures(KswordTests::Suite& suite) {
    FakeResolver resolver = MakeResolver();
    ExpectFail(suite, L"unknown module", "nosuch.dll+10", Eval("nosuch.dll+10", &resolver),
        ExprError::UnknownModule, 0, "nosuch.dll");
    ExpectFail(suite, L"unknown module offset includes leading space", "  nosuch.dll+10",
        Eval("  nosuch.dll+10", &resolver), ExprError::UnknownModule, 2, "nosuch.dll");
    // 引号模块的 detail 是去引号后的名字，偏移指向开引号。
    ExpectFail(suite, L"unknown quoted module", "  \"no such.dll\"",
        Eval("  \"no such.dll\"", &resolver), ExprError::UnknownModule, 2, "no such.dll");
    ExpectFail(suite, L"ambiguous module", "dupe.dll+10", Eval("dupe.dll+10", &resolver),
        ExprError::AmbiguousModule, 0, "dupe.dll");
    ExpectFail(suite, L"module lookup needs a process", "needproc.dll+10",
        Eval("needproc.dll+10", &resolver), ExprError::NeedsProcess, 0, "needproc.dll");
    // 内核/子表达式里的需要进程：偏移指向内部那一项。
    FakeResolver nested = MakeResolver();
    ExpectFail(suite, L"needs-process module inside a dereference", "[needproc.dll+10]",
        Eval("[needproc.dll+10]", &nested), ExprError::NeedsProcess, 1, "needproc.dll");
    suite.expect(nested.readCalls == 0, L"addr expr: no pointer read happens after a lookup failure");

    // 'client.dll-10' 无法与带连字符的真模块名区分，按模块名处理并交给 resolver：钉死这个取舍。
    FakeResolver hyphen = MakeResolver();
    ExpectFail(suite, L"module-looking token with a hyphen is looked up whole", "client.dll-10",
        Eval("client.dll-10", &hyphen), ExprError::UnknownModule, 0, "client.dll-10");
    suite.expect(hyphen.lookupNames.size() == 1 && hyphen.lookupNames[0] == "client.dll-10",
        L"addr expr: the whole hyphenated token is the module name");

    // resolver 返回枚举之外的值：拒绝，且不采用它顺手写出的基址。
    FakeResolver garbage = MakeResolver();
    garbage.forceRawLookup = true;
    garbage.rawLookupValue = 99;
    ExpectFail(suite, L"out-of-range lookup result is rejected", "client.dll+10",
        Eval("client.dll+10", &garbage), ExprError::UnknownModule, 0, "client.dll");

    // 模块位置规则：最多一个，且必须是所在层的第一项。解析阶段拒绝，resolver 一次都没被问。
    const FailCase positions[] = {
        { "10+client.dll", ExprError::BadSyntax, 3, "client.dll" },
        { "client.dll+ntdll.dll", ExprError::BadSyntax, 11, "ntdll.dll" },
        { "client.dll+client.dll", ExprError::BadSyntax, 11, "client.dll" },
        { "client.dll+\"ntdll.dll\"", ExprError::BadSyntax, 11, "ntdll.dll" },
        { "10+\"client.dll\"", ExprError::BadSyntax, 3, "client.dll" },
        { "[client.dll]+[ntdll.dll]", ExprError::BadSyntax, 14, "ntdll.dll" },
        { "[client.dll]+ntdll.dll", ExprError::BadSyntax, 13, "ntdll.dll" },
        { "[10+client.dll]", ExprError::BadSyntax, 4, "client.dll" },
    };
    for (const FailCase& item : positions) {
        FakeResolver positionResolver = MakeResolver();
        ExpectFail(suite, L"module position rule", item.text, Eval(item.text, &positionResolver),
            item.error, item.position, item.detail);
        suite.expect(positionResolver.Untouched(),
            L"addr expr: a module position error is found before any resolver call");
    }

    // 基址加偏移的溢出边界：0xFFFFFFFFFFFFF000 + 0xFFF 恰为上限，+ 0x1000 回绕。
    FakeResolver edge = MakeResolver();
    ExpectOk(suite, L"base plus offset reaching the limit", "overflow.dll+FFF",
        Eval("overflow.dll+FFF", &edge), kMax);
    const ExprResult wrapped = Eval("overflow.dll+1000", &edge);
    ExpectFail(suite, L"base plus offset wraps", "overflow.dll+1000", wrapped,
        ExprError::Overflow, 13, "1000");
    suite.expect(wrapped.value == 0ULL && !wrapped.usedModule && wrapped.moduleName.empty(),
        L"addr expr: after a wrap failure the already-resolved module is not reported");
}

// ------------------------------------------------------------
// 九、解引用：求值、嵌套、深度上限。
// ------------------------------------------------------------
void TestDeref(KswordTests::Suite& suite) {
    FakeResolver resolver = MakeResolver();
    const ExprResult single = Eval("[1000]", &resolver);
    ExpectOk(suite, L"single dereference", "[1000]", single, 0x2000ULL);
    suite.expect(single.usedDeref && !single.usedModule,
        L"addr expr: a dereference reports usedDeref only");
    suite.expect(resolver.readAddresses.size() == 1 && resolver.readAddresses[0] == 0x1000ULL
        && resolver.readWidths[0] == 8U,
        L"addr expr: the pointer is read at the inner value with the given width");

    // 宽度由调用方传入，原样下传（32 位目标 4）。
    FakeResolver narrow = MakeResolver();
    ExpectOk(suite, L"dereference at width 4", "[1000]", Eval("[1000]", &narrow, 4U), 0x2000ULL);
    suite.expect(narrow.readWidths.size() == 1 && narrow.readWidths[0] == 4U,
        L"addr expr: widthBytes 4 reaches ReadPointer unchanged");

    // 解引用与加法组合，顺序无关。
    FakeResolver sums = MakeResolver();
    ExpectOk(suite, L"dereference plus offset", "[1000]+8", Eval("[1000]+8", &sums), 0x2008ULL);
    ExpectOk(suite, L"offset plus dereference", "8+[1000]", Eval("8+[1000]", &sums), 0x2008ULL);
    sums.readAddresses.clear();
    ExpectOk(suite, L"two dereferences", "[1000]+[1008]", Eval("[1000]+[1008]", &sums), 0x9777ULL);
    suite.expect(sums.readAddresses.size() == 2 && sums.readAddresses[0] == 0x1000ULL
        && sums.readAddresses[1] == 0x1008ULL,
        L"addr expr: sibling dereferences read left to right");
    sums.readAddresses.clear();
    ExpectOk(suite, L"inner sum", "[1000+8]", Eval("[1000+8]", &sums), 0x7777ULL);
    suite.expect(sums.readAddresses.size() == 1 && sums.readAddresses[0] == 0x1008ULL,
        L"addr expr: the inner expression is summed before the read");

    // 模块可以在括号里（它是括号内的第一项），模块名与解引用标志同时报告。
    FakeResolver withModule = MakeResolver();
    const ExprResult moduleDeref = Eval("[client.dll+10]", &withModule);
    ExpectOk(suite, L"module inside a dereference", "[client.dll+10]", moduleDeref, 0x500ULL);
    suite.expect(moduleDeref.usedModule && moduleDeref.usedDeref && moduleDeref.moduleName == "client.dll",
        L"addr expr: both the module and the dereference are reported");
    ExpectOk(suite, L"dereference then offset", "[client.dll+10]+8",
        Eval("[client.dll+10]+8", &withModule), 0x508ULL);
    ExpectOk(suite, L"offset then bracketed module", "8+[client.dll+10]",
        Eval("8+[client.dll+10]", &withModule), 0x508ULL);

    // 嵌套：先内后外，读取顺序 0x1000 -> 0x2000。
    FakeResolver nest2 = MakeResolver();
    ExpectOk(suite, L"nested dereference", "[[1000]]", Eval("[[1000]]", &nest2), 0x3000ULL);
    suite.expect(nest2.readAddresses.size() == 2 && nest2.readAddresses[0] == 0x1000ULL
        && nest2.readAddresses[1] == 0x2000ULL,
        L"addr expr: nested dereferences read the inner pointer first");
    // 深度恰为上限 4：0x1000 -> 0x2000 -> 0x3000 -> 0x4000 -> 0x5000。
    FakeResolver nest4 = MakeResolver();
    ExpectOk(suite, L"depth 4 is allowed", "[[[[1000]]]]", Eval("[[[[1000]]]]", &nest4), 0x5000ULL);
    suite.expect(nest4.readCalls == 4 && nest4.readAddresses[3] == 0x4000ULL,
        L"addr expr: four nested reads follow the pointer chain");
    // 深度 5：超限，BadSyntax 指向第五个 '['，一次都没读。
    FakeResolver nest5 = MakeResolver();
    ExpectFail(suite, L"depth 5 is refused", "[[[[[1000]]]]]", Eval("[[[[[1000]]]]]", &nest5),
        ExprError::BadSyntax, 4, "[");
    suite.expect(nest5.Untouched(), L"addr expr: an over-deep expression reads nothing");
    // 深度是嵌套层数而不是括号总数：同层并列任意多个都合法。
    FakeResolver wide = MakeResolver();
    ExpectOk(suite, L"six sibling dereferences", "[1000]+[1000]+[1000]+[1000]+[1000]+[1000]",
        Eval("[1000]+[1000]+[1000]+[1000]+[1000]+[1000]", &wide), 0xC000ULL);
    suite.expect(wide.readCalls == 6, L"addr expr: sibling dereferences do not count toward depth");
    // 第二项里才超限：偏移指向那一项的第五个 '['。
    FakeResolver late = MakeResolver();
    ExpectFail(suite, L"depth limit in a later term", "[[[[1]]]]+[[[[[1]]]]]",
        Eval("[[[[1]]]]+[[[[[1]]]]]", &late), ExprError::BadSyntax, 14, "[");
    suite.expect(late.Untouched(), L"addr expr: depth is checked before the first term is read");
}

// ------------------------------------------------------------
// 十、解引用失败：读取失败、宽度边界、宽度非法、溢出。
// ------------------------------------------------------------
void TestDerefFailures(KswordTests::Suite& suite) {
    FakeResolver resolver = MakeResolver();
    ExpectFail(suite, L"unreadable pointer", "[9999]", Eval("[9999]", &resolver),
        ExprError::DerefFailed, 0, "[9999]");
    ExpectFail(suite, L"unreadable pointer after an offset", "10+[9999]", Eval("10+[9999]", &resolver),
        ExprError::DerefFailed, 3, "[9999]");
    // 内层失败报内层；外层读不到报外层。
    ExpectFail(suite, L"inner read fails", "[[9999]]", Eval("[[9999]]", &resolver),
        ExprError::DerefFailed, 1, "[9999]");
    FakeResolver broken = MakeResolver();
    broken.memory.erase(0x2000ULL);
    ExpectFail(suite, L"outer read fails", "[[1000]]", Eval("[[1000]]", &broken),
        ExprError::DerefFailed, 0, "[[1000]]");
    suite.expect(broken.readCalls == 2, L"addr expr: the inner read ran before the outer failed");
    // 前一项成功、后一项失败：整体失败，usedDeref 回到初值。
    FakeResolver partial = MakeResolver();
    ExpectFail(suite, L"second dereference fails", "[1000]+[9999]", Eval("[1000]+[9999]", &partial),
        ExprError::DerefFailed, 7, "[9999]");
    suite.expect(partial.readCalls == 2, L"addr expr: both reads were attempted in order");

    // 32 位宽度边界：0xFFFFFFFF 合法，0x100000000 是 resolver 违约，拒绝；64 位下同一值合法。
    FakeResolver widthEdge = MakeResolver();
    ExpectOk(suite, L"width 4 accepts 0xFFFFFFFF", "[6008]", Eval("[6008]", &widthEdge, 4U), 0xFFFFFFFFULL);
    ExpectFail(suite, L"width 4 refuses a value above 32 bits", "[6000]",
        Eval("[6000]", &widthEdge, 4U), ExprError::DerefFailed, 0, "[6000]");
    ExpectOk(suite, L"width 8 accepts the same value", "[6000]",
        Eval("[6000]", &widthEdge, 8U), 0x100000000ULL);

    // 宽度只允许 4 或 8：其它值遇到解引用就拒绝，且不调用 ReadPointer，也不白查模块。
    const std::uint32_t badWidths[] = { 0U, 1U, 3U, 5U, 16U };
    for (const std::uint32_t width : badWidths) {
        FakeResolver widthResolver = MakeResolver();
        const ExprResult result = Eval("[client.dll+10]", &widthResolver, width);
        ExpectFail(suite, L"unsupported pointer width", "[client.dll+10]", result,
            ExprError::DerefFailed, 0, "[client.dll+10]");
        suite.expect(widthResolver.Untouched(),
            L"addr expr: an unusable width reads nothing and looks nothing up");
    }
    // 宽度只在解引用时才有意义：纯数字表达式不受影响。
    FakeResolver unaffected = MakeResolver();
    ExpectOk(suite, L"width is irrelevant without a dereference", "1000",
        Eval("1000", &unaffected, 0U), 0x1000ULL);
    ExpectOk(suite, L"width is irrelevant for a plain module", "client.dll+10",
        Eval("client.dll+10", &unaffected, 0U), kClientBase + 0x10ULL);

    // 解引用结果参与求和时同样判溢出：0x7000 处的值是 kMax。
    FakeResolver overflow = MakeResolver();
    ExpectOk(suite, L"dereferenced limit plus zero", "[7000]+0", Eval("[7000]+0", &overflow), kMax);
    ExpectFail(suite, L"dereferenced limit plus one wraps", "[7000]+1", Eval("[7000]+1", &overflow),
        ExprError::Overflow, 7, "1");
    // 内部表达式求和溢出时不发起读取。
    FakeResolver innerWrap = MakeResolver();
    ExpectFail(suite, L"inner sum wraps before the read", "[FFFFFFFFFFFFFFFF+1]",
        Eval("[FFFFFFFFFFFFFFFF+1]", &innerWrap), ExprError::Overflow, 18, "1");
    suite.expect(innerWrap.Untouched(), L"addr expr: a wrapped inner address is never read");
}

} // namespace

void RunMemwbAddressExprResolverChecks(KswordTests::Suite& suite) {
    TestModulePlusOffset(suite);
    TestQuotedModulesAndLaneSplit(suite);
    TestModuleFailures(suite);
    TestDeref(suite);
    TestDerefFailures(suite);
}

} // namespace memwb_expr_test
