// 内存工作台地址表达式（shared/evidence/memory_workbench/MemoryAddressExpr.h）的离线测试。
//
// 为什么这个解析器值得一整套穷举断言：它属于**算错了不会报错**的那一类。
// 解析成另一个数之后，界面照样显示、驱动照样去读，只是读到了别处。
// 仓库里真实踩过的坑：地址框里纯数字串 1233 被当十进制解析成 0x4D1，安静地跳到
// 另一个地址；基址加偏移回绕后得到的同样是个合法地址。统一解析器的三条铁律是
// 默认十六进制、溢出判失败绝不回绕、失败时 value 恒为 0，下面逐条钉死。
//
// 断言原则与 NumericTextParseTests.cpp 一致：
//   * 期望值手算写死，绝不从被测函数反算；
//   * 边界两侧都测（溢出的分界、嵌套深度的分界、32 位指针宽度的分界）；
//   * 该被拒绝的输入必须显式拒绝，并同时核对错误码、出错偏移和详情文本；
//   * 失败时 value 必须为 0，其余输出字段保持初值；
//   * 用假 resolver 记录调用次数，证明"纯数字不碰 resolver""语法错误不碰 resolver"。
//
// 本文件负责纯文本层面（数字、语法、纯数字地址、失败不变式）与套件入口；
// 需要 resolver 的模块与解引用检查在 MemoryAddressExprTests.Resolver.cpp。

#include "MemoryAddressExprTestSupport.h"

namespace {

using namespace memwb_expr_test;

using ksword::memwb::EvaluateAddressExpr;
using ksword::memwb::ExprError;
using ksword::memwb::ExprResult;
using ksword::memwb::ParsePlainAddress;

// 表里手写的长字面量，用字符数断言防止数错位数（16 个 F、17 位的 1 加十六个 0）。
// 表里手写的长字面量，用字符数断言防止数错位数（16 个 F、17 位的 1 加十六个 0）。
static_assert(sizeof("FFFFFFFFFFFFFFFF") == 17, "16 hex digits");
static_assert(sizeof("10000000000000000") == 18, "17 hex digits");
static_assert(sizeof("0x10000000000000000") == 20, "0x plus 17 hex digits");
static_assert(sizeof("8000000000000000") == 17, "16 hex digits");
static_assert(sizeof("8000000000000001") == 17, "16 hex digits");
static_assert(sizeof("FFFFFFFFFFFFFFF0") == 17, "16 hex digits");
static_assert(sizeof("FFFFFFFFFFFFFFFFF") == 18, "17 hex digits");
static_assert(sizeof("1`0000000000000000") == 19, "1, a backtick, 16 zeros");
static_assert(sizeof("FFFFFFFFFFFFFFFF+1") == 19, "16 hex digits, plus, one digit");
static_assert(sizeof("[FFFFFFFFFFFFFFFF+1]") == 21, "bracket, 16 hex digits, plus, one digit, bracket");
static_assert(sizeof("1+FFFFFFFFFFFFFFFF") == 19, "one digit, plus, 16 hex digits");
static_assert(sizeof("7FFFFFFFFFFFFFFF+8000000000000001") == 34, "two 16-digit operands");
static_assert(sizeof("ffffffff`ffffffff") == 18, "8 digits, a backtick, 8 digits");

// ------------------------------------------------------------
// 一、缺陷本身：纯数字串是十六进制，不是十进制。
// ------------------------------------------------------------
void TestBareDigitsAreHex(KswordTests::Suite& suite) {
    FakeResolver resolver = MakeResolver();
    // 手算：0x1233 = 4659；十进制 1233 = 0x4D1。
    const ExprResult hex1233 = Eval("1233", &resolver);
    ExpectOk(suite, L"bare 1233 is hex 0x1233", "1233", hex1233, 0x1233ULL);
    suite.expect(hex1233.value == 4659ULL, L"addr expr: 0x1233 is 4659 decimal");
    suite.expect(hex1233.value != 1233ULL && hex1233.value != 0x4D1ULL,
        L"addr expr: bare 1233 is never read as decimal 1233 (0x4D1)");
    // 报障里出现过的另一个值：0x222222 = 2236962，十进制 222222 = 0x3640E。
    const ExprResult hex222222 = Eval("222222", &resolver);
    ExpectOk(suite, L"bare 222222 is hex 0x222222", "222222", hex222222, 0x222222ULL);
    suite.expect(hex222222.value != 0x3640EULL,
        L"addr expr: bare 222222 is not the decimal reading 0x3640E");
    // 前缀不改变进制：带不带 0x 结果相同。
    ExpectOk(suite, L"0x1233 equals bare 1233", "0x1233", Eval("0x1233", &resolver), 0x1233ULL);
    ExpectOk(suite, L"0X1233 equals bare 1233", "0X1233", Eval("0X1233", &resolver), 0x1233ULL);
    // 纯数字永远不碰 resolver。
    suite.expect(resolver.Untouched(), L"addr expr: bare numbers never call the resolver");
}

// ------------------------------------------------------------
// 二、数字各形态（含分隔符合法位置）与求和。
// ------------------------------------------------------------
void TestNumberForms(KswordTests::Suite& suite) {
    const OkCase cases[] = {
        { "1233", 0x1233ULL },
        { "0x1233", 0x1233ULL },
        { "abcdef", 0xABCDEFULL },
        { "ABCDEF", 0xABCDEFULL },
        { "0xAbCdEf", 0xABCDEFULL },
        { "0", 0ULL },
        { "0x0", 0ULL },
        { "00000000", 0ULL },
        { "0000000000000000000000001233", 0x1233ULL },
        { "1", 1ULL },
        { "f", 15ULL },
        { "19", 0x19ULL },
        { "fffff80012345678", 0xFFFFF80012345678ULL },
        { "FFFFFFFFFFFFFFFF", kMax },
        { "0xFFFFFFFFFFFFFFFF", kMax },
        { "  1233  ", 0x1233ULL },
        { "\t1233\r\n", 0x1233ULL },
        // 反引号与下划线分隔：只在数位之间。
        { "fffff800`12345678", 0xFFFFF80012345678ULL },
        { "fffff800_12345678", 0xFFFFF80012345678ULL },
        { "0xfffff800`12345678", 0xFFFFF80012345678ULL },
        { "1_2_3", 0x123ULL },
        { "1`2`3", 0x123ULL },
        { "a`b", 0xABULL },
        { "ffffffff`ffffffff", kMax },
        // 求和：任意项数、空白可有可无。
        { "10+20", 0x30ULL },
        { "10+20+30", 0x60ULL },
        { " 10 + 20 ", 0x30ULL },
        { "1+2+3+4", 0xAULL },
        { "10+0", 0x10ULL },
        { "0+0", 0ULL },
        { "fffff800`00000000+1000", 0xFFFFF80000001000ULL },
        // 加法恰好顶到上限是合法的，差一就是溢出（见失败用例）。
        { "FFFFFFFFFFFFFFFF+0", kMax },
        { "7FFFFFFFFFFFFFFF+8000000000000000", kMax },
        { "8000000000000000+7FFFFFFFFFFFFFFF", kMax },
        { "FFFFFFFFFFFFFFF0+8+7", kMax },
        // 名字像十六进制的无引号记号一律是数字。
        { "abc", 0xABCULL },
        { "dead", 0xDEADULL },
        { "face", 0xFACEULL },
        { "beef", 0xBEEFULL },
        { "abc+10", 0xACCULL },
    };
    FakeResolver resolver = MakeResolver();
    for (const OkCase& item : cases) {
        const ExprResult result = Eval(item.text, &resolver);
        ExpectOk(suite, L"number form parses", item.text, result, item.value);
        suite.expect(!result.usedModule && !result.usedDeref && result.moduleName.empty(),
            L"addr expr: a pure number reports no module and no deref");
    }
    suite.expect(resolver.Untouched(),
        L"addr expr: pure-number expressions never call the resolver, even with one supplied");
}

// ------------------------------------------------------------
// 三、数字失败：格式非法与溢出，必须显式拒绝、绝不回绕。
// ------------------------------------------------------------
void TestNumberFailures(KswordTests::Suite& suite) {
    const FailCase cases[] = {
        // 只有前缀没有数位：不能退化成 0。
        { "0x", ExprError::BadNumber, 0, "0x" },
        { "0X", ExprError::BadNumber, 0, "0X" },
        // 分隔符不在两个数位之间：开头、结尾、连续、紧跟 0x。
        { "`", ExprError::BadNumber, 0, "`" },
        { "_", ExprError::BadNumber, 0, "_" },
        { "``", ExprError::BadNumber, 0, "``" },
        { "`1234", ExprError::BadNumber, 0, "`1234" },
        { "1234`", ExprError::BadNumber, 0, "1234`" },
        { "12``34", ExprError::BadNumber, 0, "12``34" },
        { "12`_34", ExprError::BadNumber, 0, "12`_34" },
        { "12_`34", ExprError::BadNumber, 0, "12_`34" },
        { "_1", ExprError::BadNumber, 0, "_1" },
        { "1_", ExprError::BadNumber, 0, "1_" },
        { "0x`12", ExprError::BadNumber, 0, "0x`12" },
        { "0x12`", ExprError::BadNumber, 0, "0x12`" },
        { "0x_", ExprError::BadNumber, 0, "0x_" },
        // 偏移指向出错记号本身，含前导空白与前面的项。
        { "  `12", ExprError::BadNumber, 2, "`12" },
        { "10+0x", ExprError::BadNumber, 3, "0x" },
        { "10+1__2", ExprError::BadNumber, 3, "1__2" },
        // 单个数字超过 64 位：16 位恰好合法，17 位溢出。
        { "10000000000000000", ExprError::Overflow, 0, "10000000000000000" },
        { "0x10000000000000000", ExprError::Overflow, 0, "0x10000000000000000" },
        { "FFFFFFFFFFFFFFFFF", ExprError::Overflow, 0, "FFFFFFFFFFFFFFFFF" },
        { "1`0000000000000000", ExprError::Overflow, 0, "1`0000000000000000" },
        { "  0x10000000000000000", ExprError::Overflow, 2, "0x10000000000000000" },
        { "10+10000000000000000", ExprError::Overflow, 3, "10000000000000000" },
        // 求和溢出：偏移指向"加不下"的那一项。
        { "FFFFFFFFFFFFFFFF+1", ExprError::Overflow, 17, "1" },
        { "1+FFFFFFFFFFFFFFFF", ExprError::Overflow, 2, "FFFFFFFFFFFFFFFF" },
        { "8000000000000000+8000000000000000", ExprError::Overflow, 17, "8000000000000000" },
        { "7FFFFFFFFFFFFFFF+8000000000000001", ExprError::Overflow, 17, "8000000000000001" },
        { "FFFFFFFFFFFFFFF0+8+8", ExprError::Overflow, 19, "8" },
    };
    for (const FailCase& item : cases) {
        FakeResolver resolver = MakeResolver();
        const ExprResult result = Eval(item.text, &resolver);
        ExpectFail(suite, L"number failure is reported exactly", item.text, result,
            item.error, item.position, item.detail);
        suite.expect(resolver.Untouched(),
            L"addr expr: a numeric failure never reaches the resolver");
    }
}

// ------------------------------------------------------------
// 四、空输入、语法错误与减法拒绝。
// ------------------------------------------------------------
void TestSyntaxFailures(KswordTests::Suite& suite) {
    const FailCase cases[] = {
        // 空与全空白：Empty，不是 BadSyntax，也不是 0。
        { "", ExprError::Empty, 0, "" },
        { " ", ExprError::Empty, 0, "" },
        { "\t\r\n ", ExprError::Empty, 0, "" },
        // 多余的加号：前导、尾随、连续。
        { "+", ExprError::BadSyntax, 0, "+" },
        { "+10", ExprError::BadSyntax, 0, "+" },
        { "10+", ExprError::BadSyntax, 3, "" },
        { "10 +", ExprError::BadSyntax, 4, "" },
        { "10++20", ExprError::BadSyntax, 3, "+" },
        { "10+ +20", ExprError::BadSyntax, 4, "+" },
        // 两项之间没有加号。
        { "10 20", ExprError::BadSyntax, 3, "20" },
        { "10 client.dll", ExprError::BadSyntax, 3, "client.dll" },
        { "\"abc\"def", ExprError::BadSyntax, 5, "def" },
        { "abc\"def\"", ExprError::BadSyntax, 3, "\"" },
        // 括号不配对与空括号。
        { "]", ExprError::BadSyntax, 0, "]" },
        { "1000]", ExprError::BadSyntax, 4, "]" },
        { "[1000]]", ExprError::BadSyntax, 6, "]" },
        { "[]", ExprError::BadSyntax, 1, "]" },
        { "[ ]", ExprError::BadSyntax, 2, "]" },
        { "[", ExprError::BadSyntax, 1, "" },
        { "[1000", ExprError::BadSyntax, 0, "[" },
        { "[[1000]", ExprError::BadSyntax, 0, "[" },
        { "[+1000]", ExprError::BadSyntax, 1, "+" },
        { "[1000]+", ExprError::BadSyntax, 7, "" },
        { "[1000][2000]", ExprError::BadSyntax, 6, "[" },
        { "[1000]1000", ExprError::BadSyntax, 6, "1000" },
        // 数字减法：模块名里合法含 '-'，所以减法必须明确拒绝，不能悄悄当模块名。
        { "0x100-10", ExprError::BadSyntax, 5, "0x100-10" },
        { "100-10", ExprError::BadSyntax, 3, "100-10" },
        { "0x100-0x10", ExprError::BadSyntax, 5, "0x100-0x10" },
        { "100-", ExprError::BadSyntax, 3, "100-" },
        { "-10", ExprError::BadSyntax, 0, "-10" },
        { "-", ExprError::BadSyntax, 0, "-" },
        { "ab-cd", ExprError::BadSyntax, 2, "ab-cd" },
        { "10+20-5", ExprError::BadSyntax, 5, "20-5" },
        { "0x100 - 10", ExprError::BadSyntax, 6, "-" },
        { "[1000]-8", ExprError::BadSyntax, 6, "-8" },
    };
    for (const FailCase& item : cases) {
        FakeResolver resolver = MakeResolver();
        const ExprResult result = Eval(item.text, &resolver);
        ExpectFail(suite, L"syntax failure is reported exactly", item.text, result,
            item.error, item.position, item.detail);
        suite.expect(resolver.Untouched(),
            L"addr expr: a syntax failure is found before any resolver call");
    }
}

// ------------------------------------------------------------
// 五、ParsePlainAddress：只认纯数字，且与表达式的数字规则一致。
// ------------------------------------------------------------
void TestParsePlainAddress(KswordTests::Suite& suite) {
    const OkCase accepted[] = {
        { "1233", 0x1233ULL },
        { "0x1233", 0x1233ULL },
        { " 1233 ", 0x1233ULL },
        { "abc", 0xABCULL },
        { "0", 0ULL },
        { "fffff800`12345678", 0xFFFFF80012345678ULL },
        { "fffff800_12345678", 0xFFFFF80012345678ULL },
        { "FFFFFFFFFFFFFFFF", kMax },
    };
    for (const OkCase& item : accepted) {
        std::uint64_t value = 0xDEADULL;
        const bool ok = ParsePlainAddress(item.text, value);
        suite.expect(ok && value == item.value,
            (std::wstring(L"addr expr: ParsePlainAddress accepts [") + Widen(item.text) + L"]").c_str());
    }

    const char* const rejected[] = {
        "", "   ", "client.dll", "10+20", "[1000]", "0x", "12 34", "1233h", "-1", "+1",
        "10000000000000000", "0x10000000000000000", "`12", "12`", "0x`12", "\"abc\"", "abc.dll",
    };
    for (const char* text : rejected) {
        // 预置一个非零残留，证明失败时被清成 0 而不是原样留着。
        std::uint64_t value = 0xDEADULL;
        const bool ok = ParsePlainAddress(text, value);
        suite.expect(!ok && value == 0ULL,
            (std::wstring(L"addr expr: ParsePlainAddress rejects and zeroes [") + Widen(text) + L"]").c_str());
    }

    // 与表达式求值在单个数字记号上必须完全一致：两处不许各有各的规则。
    const char* const tokens[] = {
        "1233", "0x1233", "abc", "fffff800`12345678", "0x", "`12", "12`", "1__2",
        "10000000000000000", "FFFFFFFFFFFFFFFF", "0", "12g4",
    };
    for (const char* text : tokens) {
        std::uint64_t plain = 0;
        const bool plainOk = ParsePlainAddress(text, plain);
        const ExprResult expr = Eval(text, nullptr);
        // "12g4" 在表达式里是模块名（需要进程），不是数字；其余必须同成同败同值。
        const bool agrees = (std::string(text) == "12g4")
            ? (!plainOk && !expr.ok)
            : (plainOk == expr.ok && plain == expr.value);
        suite.expect(agrees,
            (std::wstring(L"addr expr: plain parser and expression agree on [") + Widen(text) + L"]").c_str());
    }
}
// ------------------------------------------------------------
// 十一、resolver 为空：模块与解引用返回 NeedsProcess，纯数字照常。
// ------------------------------------------------------------
void TestNullResolver(KswordTests::Suite& suite) {
    ExpectFail(suite, L"dereference without a resolver", "[1000]", Eval("[1000]", nullptr),
        ExprError::NeedsProcess, 0, "[1000]");
    ExpectFail(suite, L"module without a resolver", "client.dll+10", Eval("client.dll+10", nullptr),
        ExprError::NeedsProcess, 0, "client.dll");
    ExpectFail(suite, L"quoted module without a resolver", "\"client.dll\"",
        Eval("\"client.dll\"", nullptr),
        ExprError::NeedsProcess, 0, "client.dll");
    ExpectFail(suite, L"bracketed module without a resolver", "[client.dll+10]",
        Eval("[client.dll+10]", nullptr), ExprError::NeedsProcess, 0, "[client.dll+10]");
    // 语法错误优先于"没有 resolver"：位置规则是文法问题，与有无进程无关。
    ExpectFail(suite, L"syntax error wins over a missing resolver", "10+client.dll",
        Eval("10+client.dll", nullptr), ExprError::BadSyntax, 3, "client.dll");
    // 纯数字不需要 resolver。
    ExpectOk(suite, L"plain number without a resolver", "1233", Eval("1233", nullptr), 0x1233ULL);
    ExpectOk(suite, L"plain sum without a resolver", "10+20", Eval("10+20", nullptr), 0x30ULL);
    ExpectOk(suite, L"separated number without a resolver", "fffff800`12345678",
        Eval("fffff800`12345678", nullptr), 0xFFFFF80012345678ULL);
}

// ------------------------------------------------------------
// 十二、先整体解析再求值：坏输入不会触发任何一次多余的 resolver 调用。
// ------------------------------------------------------------
void TestParseBeforeEvaluate(KswordTests::Suite& suite) {
    const char* const badInputs[] = {
        "[1000]+",             // 尾随加号
        "client.dll+",         // 尾随加号
        "[1000]+0x",           // 数字格式错在后面
        "[1000]+[1000]]",      // 多余右括号
        "[1000]+client.dll",   // 模块不在第一项
        "client.dll+[1000",    // 括号没闭合
        "[1000]+0x100-10",     // 减法
        "[client.dll]+[[[[[1]]]]]",  // 后面的项嵌套过深
    };
    for (const char* text : badInputs) {
        FakeResolver resolver = MakeResolver();
        const ExprResult result = Eval(text, &resolver);
        suite.expect(!result.ok && result.error != ExprError::None,
            (std::wstring(L"addr expr: malformed input fails [") + Widen(text) + L"]").c_str());
        suite.expect(resolver.Untouched(),
            (std::wstring(L"addr expr: malformed input makes no resolver call [") + Widen(text) + L"]").c_str());
    }

    // 纯数字表达式（成功与失败两种）整体一次都不调 resolver。
    FakeResolver numeric = MakeResolver();
    const char* const numericInputs[] = {
        "1233", "0x10+20", "fffff800`12345678", "FFFFFFFFFFFFFFFF+1", "0x", "100-10", "", "abc",
    };
    for (const char* text : numericInputs) {
        (void)Eval(text, &numeric);
    }
    suite.expect(numeric.Untouched(),
        L"addr expr: no pure-number expression, valid or not, ever calls the resolver");
}

// ------------------------------------------------------------
// 十三、失败不变式：value 恒为 0，其余输出字段保持初值，错误码不为 None。
// ------------------------------------------------------------
void TestFailureInvariants(KswordTests::Suite& suite) {
    const char* const failing[] = {
        "", "   ", "0x", "`12", "10000000000000000", "FFFFFFFFFFFFFFFF+1", "10+", "[1000",
        "1000]", "0x100-10", "10+client.dll", "client.dll+ntdll.dll", "nosuch.dll+10",
        "dupe.dll", "needproc.dll", "overflow.dll+1000", "[9999]", "[1000]+[9999]",
        "[[[[[1000]]]]]", "\"\"", "\"client.dll",
    };
    for (const char* text : failing) {
        FakeResolver resolver = MakeResolver();
        const ExprResult result = Eval(text, &resolver);
        suite.expect(!result.ok && result.error != ExprError::None,
            (std::wstring(L"addr expr: failing input reports an error code [") + Widen(text) + L"]").c_str());
        suite.expect(result.value == 0ULL && !result.usedModule && !result.usedDeref
            && result.moduleName.empty(),
            (std::wstring(L"addr expr: failing input leaves value and flags at their initial values [")
                + Widen(text) + L"]").c_str());
    }

    // 成功的结果：错误三元组为初值。
    FakeResolver resolver = MakeResolver();
    const ExprResult good = Eval("client.dll+10", &resolver);
    suite.expect(good.ok && good.error == ExprError::None && good.detail.empty()
        && good.errorPosition == 0,
        L"addr expr: a successful result carries no error, detail or position");
    // 成功值为 0 与失败值为 0 必须靠 ok 区分：基址为 0 的模块、字面量 0。
    resolver.modules["zero.dll"] = 0ULL;
    const ExprResult zeroModule = Eval("zero.dll", &resolver);
    suite.expect(zeroModule.ok && zeroModule.value == 0ULL && zeroModule.usedModule,
        L"addr expr: a module based at 0 is a success with value 0, distinguishable from failure");
    suite.expect(Eval("0", &resolver).ok, L"addr expr: literal 0 is a success");
}

} // namespace

int RunMemwbAddressExprTests() {
    KswordTests::Suite suite(L"MEMWB addr expr");
    TestBareDigitsAreHex(suite);
    TestNumberForms(suite);
    TestNumberFailures(suite);
    TestSyntaxFailures(suite);
    TestParsePlainAddress(suite);
    TestNullResolver(suite);
    TestParseBeforeEvaluate(suite);
    TestFailureInvariants(suite);
    memwb_expr_test::RunMemwbAddressExprResolverChecks(suite);
    suite.report();
    return suite.failures();
}
