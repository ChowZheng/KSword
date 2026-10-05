// 内存工作台进程文本匹配（shared/evidence/memory_workbench/MemoryProcessMatch.h）的离线测试。
//
// 为什么这个模块值得一整套穷举断言：它属于**判错了不会报错**的那一类。
// 真实发生过的缺陷：旧 findDriverMemoryProcessComboMatch 在同名多开（QQ 一开就是十个同名
// 进程）时静默取第一个命中。选错之后的表现是"地址读不到"，而读取代码其实完全正常，
// 排查会被整个带偏到内存读取上去。具体到本模块有五处：
//   * 多个命中被收口成第一个（本模块存在的理由）；
//   * 纯数字被当成十六进制，或 "0x" 前缀被当成十进制，读到另一个进程；
//   * 超过 32 位的数字被截断成一个"碰巧存在"的 PID；
//   * 数字文本又去按名字匹配，撞上一个叫 "7" 的进程；
//   * 同一个 PID 在候选里重复出现被误报成 Multiple，或 PID 为 0 的候选被选中。
//
// 断言原则与 NumericTextParseTests.cpp 一致：
//   * 期望值手算写死（含十六进制 0x64 = 100、32 位边界 4294967295/4294967296）；
//   * 边界两侧都测（32 位上限、U+3000 与相邻的 U+3001、NBSP 与相邻的 U+00A1）；
//   * 该被拒绝的输入必须被显式拒绝，所有失败结论的 pid/by/candidates 都保持安全初值；
//   * 每一级匹配规则的优先顺序（精确名 > 显示文本 > 子串）各用一个反例钉住。

#include "TestSupport.h"

#include "../shared/evidence/memory_workbench/MemoryProcessMatch.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

using ksword::memwb::MatchProcessText;
using ksword::memwb::ProcessCandidate;
using ksword::memwb::ProcessDisplayText;
using ksword::memwb::ProcessMatchBy;
using ksword::memwb::ProcessMatchKind;
using ksword::memwb::ProcessMatchResult;

// Cand：造一个候选进程。传入：pid 与名字（UTF-8）。传出：候选。
ProcessCandidate Cand(const std::uint32_t pid, const char* name) {
    ProcessCandidate candidate;
    candidate.pid = pid;
    candidate.name = name;
    return candidate;
}

// MainList：多数测试共用的候选列表。故意把 PID 排成乱序（200 在 100 之前、402 在 400 之前），
// 这样"按 PID 升序列出"不会碰巧被输入顺序满足。
std::vector<ProcessCandidate> MainList() {
    return {
        Cand(200, "chrome.exe"),
        Cand(4, "System"),
        Cand(100, "chrome.exe"),
        Cand(150, "Chrome.exe"),
        Cand(300, "notepad.exe"),
        Cand(301, "notepad++.exe"),
        Cand(402, "QQ.exe"),
        Cand(400, "QQ.exe"),
        Cand(401, "QQ.exe"),
        Cand(1234, "svchost.exe"),
    };
}

// PidsOf：取结果里候选的 PID 序列，便于整串比较。
std::vector<std::uint32_t> PidsOf(const ProcessMatchResult& result) {
    std::vector<std::uint32_t> pids;
    for (const ProcessCandidate& candidate : result.candidates) {
        pids.push_back(candidate.pid);
    }
    return pids;
}

// IsFailureShape：失败类结论（Empty/NotFound/BadPid）必须保持安全初值：by=None、pid=0、候选为空。
bool IsFailureShape(const ProcessMatchResult& result) {
    return result.by == ProcessMatchBy::None && result.pid == 0U && result.candidates.empty();
}

// ------------------------------------------------------------
// 一、枚举数值、默认值、显示文本格式。
// ------------------------------------------------------------
void TestEnumsDefaultsAndDisplayText(KswordTests::Suite& suite) {
    suite.expect(static_cast<std::uint32_t>(ProcessMatchKind::Empty) == 0U, L"match: Empty is 0");
    suite.expect(static_cast<std::uint32_t>(ProcessMatchKind::NotFound) == 1U, L"match: NotFound is 1");
    suite.expect(static_cast<std::uint32_t>(ProcessMatchKind::Unique) == 2U, L"match: Unique is 2");
    suite.expect(static_cast<std::uint32_t>(ProcessMatchKind::Multiple) == 3U, L"match: Multiple is 3");
    suite.expect(static_cast<std::uint32_t>(ProcessMatchKind::BadPid) == 4U, L"match: BadPid is 4");
    suite.expect(static_cast<std::uint32_t>(ProcessMatchBy::None) == 0U, L"match: By None is 0");
    suite.expect(static_cast<std::uint32_t>(ProcessMatchBy::Pid) == 1U, L"match: By Pid is 1");
    suite.expect(static_cast<std::uint32_t>(ProcessMatchBy::ExactName) == 2U, L"match: By ExactName is 2");
    suite.expect(static_cast<std::uint32_t>(ProcessMatchBy::DisplayText) == 3U, L"match: By DisplayText is 3");
    suite.expect(static_cast<std::uint32_t>(ProcessMatchBy::Substring) == 4U, L"match: By Substring is 4");

    // 默认结论是"空文本"的安全初值。
    const ProcessMatchResult fresh;
    suite.expect(fresh.kind == ProcessMatchKind::Empty && IsFailureShape(fresh),
        L"match: a default result is Empty with no pid and no candidates");
    const ProcessCandidate blank;
    suite.expect(blank.pid == 0U && blank.name.empty(), L"match: a default candidate is empty");

    // 显示文本格式："名字 [PID:十进制]"，手写整串。
    suite.expect(ProcessDisplayText(Cand(1234, "chrome.exe")) == "chrome.exe [PID:1234]",
        L"display: the format is name, space, [PID:decimal]");
    suite.expect(ProcessDisplayText(Cand(4294967295U, "x")) == "x [PID:4294967295]",
        L"display: the largest pid is written in full");
    suite.expect(ProcessDisplayText(Cand(5, "")) == " [PID:5]", L"display: an empty name keeps the separator");
    suite.expect(ProcessDisplayText(Cand(7, "a b+c.exe")) == "a b+c.exe [PID:7]",
        L"display: special characters in the name pass through unchanged");
}

// ------------------------------------------------------------
// 二、空文本：Empty，与候选是否为空无关。
// ------------------------------------------------------------
void TestEmptyText(KswordTests::Suite& suite) {
    const std::vector<ProcessCandidate> list = MainList();
    const std::string_view blanks[] = {
        "", " ", "   ", "\t", "\r\n", " \t\r\n\v\f ",
        "\xE3\x80\x80",              // U+3000 全角空格
        "\xC2\xA0",                  // U+00A0 不换行空格
        " \xE3\x80\x80\xC2\xA0\t ",  // 混合
    };
    for (const std::string_view blank : blanks) {
        const ProcessMatchResult result = MatchProcessText(blank, list);
        suite.expect(result.kind == ProcessMatchKind::Empty && IsFailureShape(result),
            L"empty: whitespace-only text is Empty with a safe result");
    }

    // 候选列表为空时空文本仍是 Empty，不是 NotFound。
    suite.expect(MatchProcessText("", std::vector<ProcessCandidate>()).kind == ProcessMatchKind::Empty,
        L"empty: empty text with no candidates is still Empty");
    suite.expect(MatchProcessText("  ", std::vector<ProcessCandidate>()).kind == ProcessMatchKind::Empty,
        L"empty: blank text with no candidates is still Empty");
}

// ------------------------------------------------------------
// 三、数字文本：十进制 PID、0x 十六进制、32 位边界。
// ------------------------------------------------------------
void TestNumericText(KswordTests::Suite& suite) {
    const std::vector<ProcessCandidate> list = MainList();

    // 十进制：100 命中第一个 chrome（PID 100），带出名字与"按 PID 命中"。
    ProcessMatchResult result = MatchProcessText("100", list);
    suite.expect(result.kind == ProcessMatchKind::Unique && result.pid == 100U && result.by == ProcessMatchBy::Pid,
        L"pid: decimal 100 finds pid 100 by pid");
    suite.expect(result.candidates.size() == 1U && result.candidates[0].pid == 100U
            && result.candidates[0].name == "chrome.exe",
        L"pid: the unique result carries exactly the matched candidate");

    // 前后空白与前导零不影响值；很长的前导零也不算溢出。
    suite.expect(MatchProcessText("  100  ", list).pid == 100U, L"pid: surrounding whitespace is ignored");
    suite.expect(MatchProcessText("0100", list).pid == 100U, L"pid: a leading zero is ignored");
    suite.expect(MatchProcessText("0000000000000000000000000000100", list).pid == 100U,
        L"pid: a very long run of leading zeros is not an overflow");
    suite.expect(MatchProcessText("4", list).pid == 4U, L"pid: the System pid 4 is found");

    // 找不到：NotFound，不会按名字再试一次。
    result = MatchProcessText("99", list);
    suite.expect(result.kind == ProcessMatchKind::NotFound && IsFailureShape(result),
        L"pid: a pid that is not in the candidates is NotFound");

    // 十进制与十六进制的分歧：64 是十进制 64，0x64 才是十进制 100——这就是原缺陷的形状。
    std::vector<ProcessCandidate> radix = { Cand(64, "decimal64.exe"), Cand(100, "hex64.exe") };
    result = MatchProcessText("64", radix);
    suite.expect(result.kind == ProcessMatchKind::Unique && result.pid == 64U,
        L"pid: bare 64 is decimal 64, never hex 0x64");
    result = MatchProcessText("0x64", radix);
    suite.expect(result.kind == ProcessMatchKind::Unique && result.pid == 100U && result.by == ProcessMatchBy::Pid,
        L"pid: 0x64 is hex, which is 100");
    suite.expect(MatchProcessText("0X64", radix).pid == 100U, L"pid: the 0X prefix works too");
    suite.expect(MatchProcessText("0xFFFFFFFF", { Cand(4294967295U, "max.exe") }).pid == 4294967295U,
        L"pid: 0xFFFFFFFF is the largest valid pid");
    suite.expect(MatchProcessText("0x00000000000000000000064", radix).pid == 100U,
        L"pid: leading zeros after 0x are not an overflow");
    suite.expect(MatchProcessText("0xabc", { Cand(2748, "hexletters.exe") }).pid == 2748U,
        L"pid: lowercase hex digits parse, 0xabc is 2748");
    suite.expect(MatchProcessText("0xABC", { Cand(2748, "hexletters.exe") }).pid == 2748U,
        L"pid: uppercase hex digits parse too");

    // 32 位边界：4294967295 合法（有候选则命中，没有则 NotFound）；4294967296 是 BadPid。
    const std::vector<ProcessCandidate> maxList = { Cand(4294967295U, "max.exe") };
    suite.expect(MatchProcessText("4294967295", maxList).kind == ProcessMatchKind::Unique,
        L"pid: 4294967295 is a valid pid and is found");
    suite.expect(MatchProcessText("4294967295", list).kind == ProcessMatchKind::NotFound,
        L"pid: 4294967295 absent from the candidates is NotFound, not BadPid");
    result = MatchProcessText("4294967296", maxList);
    suite.expect(result.kind == ProcessMatchKind::BadPid && IsFailureShape(result),
        L"pid: 4294967296 is one past 32 bits and is BadPid, never truncated to 0");
    suite.expect(MatchProcessText("4294967297", { Cand(1, "one.exe") }).kind == ProcessMatchKind::BadPid,
        L"pid: 4294967297 is BadPid and is not wrapped around to pid 1");
    suite.expect(MatchProcessText("99999999999999999999999999999999", list).kind == ProcessMatchKind::BadPid,
        L"pid: a huge decimal number is BadPid");
    suite.expect(MatchProcessText("0x100000000", maxList).kind == ProcessMatchKind::BadPid,
        L"pid: 0x100000000 is one past 32 bits and is BadPid");
    suite.expect(MatchProcessText("0x1FFFFFFFF", { Cand(0xFFFFFFFFU, "max.exe") }).kind == ProcessMatchKind::BadPid,
        L"pid: 0x1FFFFFFFF is BadPid and is not truncated to 0xFFFFFFFF");
    suite.expect(MatchProcessText("0xFFFFFFFFFFFFFFFFFFFFFFFF", list).kind == ProcessMatchKind::BadPid,
        L"pid: a huge hex number is BadPid");
    std::string longDigits(1000, '9');
    suite.expect(MatchProcessText(longDigits, list).kind == ProcessMatchKind::BadPid,
        L"pid: a thousand digits is BadPid without overflow trouble");

    // 零不是 PID：BadPid（含各种写法）。
    suite.expect(MatchProcessText("0", list).kind == ProcessMatchKind::BadPid, L"pid: 0 is BadPid");
    suite.expect(MatchProcessText("000", list).kind == ProcessMatchKind::BadPid, L"pid: 000 is BadPid");
    suite.expect(MatchProcessText("0x0", list).kind == ProcessMatchKind::BadPid, L"pid: 0x0 is BadPid");
    suite.expect(MatchProcessText("0x000", list).kind == ProcessMatchKind::BadPid, L"pid: 0x000 is BadPid");
}

// ------------------------------------------------------------
// 四、不是数字的文本：走名字匹配，不会被当成 PID。
// ------------------------------------------------------------
void TestNotNumbers(KswordTests::Suite& suite) {
    const std::vector<ProcessCandidate> list = MainList();

    // 这些都不是数字，应走名字匹配；候选里没有对应名字，所以是 NotFound 且 by=None。
    const std::string_view notNumbers[] = {
        "0x",          // 只有前缀没有数位
        "0X",
        "0xZZ",        // 前缀后不是十六进制数位
        "0x64g",
        "4d2",         // 十六进制样子但没有 0x 前缀
        "1e3",
        "+100",        // 带符号
        "-100",
        "1 00",        // 内部有空格
        "12abc",
        "100.",        // 带小数点
        "\xEF\xBC\x91\xEF\xBC\x90\xEF\xBC\x90",  // 全角数字 １００
        "\xD9\xA3",                              // 阿拉伯-印度数字 ٣
    };
    for (const std::string_view text : notNumbers) {
        const ProcessMatchResult result = MatchProcessText(text, list);
        suite.expect(result.kind == ProcessMatchKind::NotFound && IsFailureShape(result),
            L"not number: text that is not a numeric literal is matched as a name and not found");
    }

    // 带符号/小数点/十六进制样子的文本即使恰好有同名进程，也走名字匹配而非 PID。
    suite.expect(MatchProcessText("+100", { Cand(100, "pid100.exe"), Cand(7, "+100") }).pid == 7U,
        L"not number: +100 matches the process named +100, not pid 100");
    suite.expect(MatchProcessText("4d2", { Cand(1234, "x.exe"), Cand(8, "4d2") }).pid == 8U,
        L"not number: bare 4d2 is a name, it is not hex 0x4d2 = 1234");

    // 数字文本永远不再按名字匹配：叫 "7" 的进程不会被 "7" 命中。
    const ProcessMatchResult digitName = MatchProcessText("7", { Cand(9, "7"), Cand(10, "seven.exe") });
    suite.expect(digitName.kind == ProcessMatchKind::NotFound && IsFailureShape(digitName),
        L"not number: digits are only ever a pid, they never match a process named 7");
    suite.expect(MatchProcessText("9", { Cand(9, "7"), Cand(10, "seven.exe") }).pid == 9U,
        L"not number: the same digits found as a pid when the pid exists");
    suite.expect(MatchProcessText("0", { Cand(0, "0"), Cand(5, "zero") }).kind == ProcessMatchKind::BadPid,
        L"not number: a process named 0 with pid 0 cannot be selected by 0 either");
}

// ------------------------------------------------------------
// 四·补、数字文本的数位边界：用 1..5000 的稠密候选，任何"把边界字符多算成数位"的实现
// 都会解析出一个真实存在的 PID 而被抓住。
// ------------------------------------------------------------
void TestNumericLiteralBoundaries(KswordTests::Suite& suite) {
    // 稠密候选：PID 1..5000，名字 "procN"。名字里不含冒号、斜杠、反引号等边界字符，
    // 所以下面这些非数字文本在名字匹配里也不会命中。
    std::vector<ProcessCandidate> dense;
    for (std::uint32_t pid = 1; pid <= 5000U; ++pid) {
        dense.push_back(Cand(pid, ("proc" + std::to_string(pid)).c_str()));
    }

    // 紧邻合法数位两侧的字符都不是数位：'/' 在 '0' 之前，':' 在 '9' 之后，
    // '@' 在 'A' 之前，'G' 在 'F' 之后，'`' 在 'a' 之前，'g' 在 'f' 之后。
    const std::string_view neighbours[] = {
        "1/", "/1", "9/", "1:", "9:", "0x1@", "0x1G", "0x1g", "0x1`", "0x:", "0x/", "0x@", "0x`",
    };
    for (const std::string_view text : neighbours) {
        const ProcessMatchResult result = MatchProcessText(text, dense);
        suite.expect(result.kind == ProcessMatchKind::NotFound && IsFailureShape(result),
            L"digits: a character next to a valid digit is not a digit");
    }

    // 合法数位的两端都被接受：0 与 9、a/A 与 f/F。
    suite.expect(MatchProcessText("10", dense).pid == 10U, L"digits: 0 is a digit (pid 10)");
    suite.expect(MatchProcessText("9", dense).pid == 9U, L"digits: 9 is a digit (pid 9)");
    suite.expect(MatchProcessText("0x1f", dense).pid == 31U, L"digits: lowercase f is a hex digit (0x1f = 31)");
    suite.expect(MatchProcessText("0x1F", dense).pid == 31U, L"digits: uppercase F is a hex digit (0x1F = 31)");
    suite.expect(MatchProcessText("0x1a", dense).pid == 26U, L"digits: lowercase a is a hex digit (0x1a = 26)");
    suite.expect(MatchProcessText("0x1A", dense).pid == 26U, L"digits: uppercase A is a hex digit (0x1A = 26)");
    suite.expect(MatchProcessText("0xaF", dense).pid == 175U, L"digits: mixed case hex works (0xaF = 175)");
    suite.expect(MatchProcessText("0x9", dense).pid == 9U, L"digits: a single hex digit works");
    suite.expect(MatchProcessText("0x1388", dense).pid == 5000U, L"digits: 0x1388 is 5000");
    suite.expect(MatchProcessText("0x1389", dense).kind == ProcessMatchKind::NotFound,
        L"digits: 0x1389 is 5001, which is not in the candidates");
}

// ------------------------------------------------------------
// 五·补、ASCII 大小写折叠的边界：A/Z 两端被折叠，紧邻的 '@' '[' '`' '{' 不被折叠。
// ------------------------------------------------------------
void TestAsciiFoldBoundaries(KswordTests::Suite& suite) {
    const std::vector<ProcessCandidate> list = {
        Cand(50, "AZ.exe"), Cand(51, "a@.exe"), Cand(52, "a[.exe"), Cand(53, "a`.exe"), Cand(54, "a{.exe"),
    };

    // A 与 Z 两个端点都被折叠。
    suite.expect(MatchProcessText("az.EXE", list).pid == 50U, L"fold: A and Z both fold to lower case");
    suite.expect(MatchProcessText("aZ.Exe", list).pid == 50U, L"fold: a mixed spelling of AZ matches");

    // 紧邻端点的字符不被折叠：'@'(0x40) 与 '`'(0x60) 不等价，'['(0x5B) 与 '{'(0x7B) 不等价。
    // 如果折叠区间多算了一个字符，下面某一条会从 Unique 变成 Multiple 或指错 PID。
    ProcessMatchResult result = MatchProcessText("a@.exe", list);
    suite.expect(result.kind == ProcessMatchKind::Unique && result.pid == 51U,
        L"fold: @ is not folded onto the grave accent");
    result = MatchProcessText("a`.exe", list);
    suite.expect(result.kind == ProcessMatchKind::Unique && result.pid == 53U,
        L"fold: the grave accent is not folded onto @");
    result = MatchProcessText("a[.exe", list);
    suite.expect(result.kind == ProcessMatchKind::Unique && result.pid == 52U,
        L"fold: [ is not folded onto {");
    result = MatchProcessText("a{.exe", list);
    suite.expect(result.kind == ProcessMatchKind::Unique && result.pid == 54U,
        L"fold: { is not folded onto [");
}

// ------------------------------------------------------------
// 五、精确名：大小写折叠、同名多开 Multiple 且按 PID 升序、优先于子串。
// ------------------------------------------------------------
void TestExactName(KswordTests::Suite& suite) {
    const std::vector<ProcessCandidate> list = MainList();

    // 单个同名：Unique，by=ExactName。
    ProcessMatchResult result = MatchProcessText("notepad.exe", list);
    suite.expect(result.kind == ProcessMatchKind::Unique && result.pid == 300U && result.by == ProcessMatchBy::ExactName,
        L"name: notepad.exe is unique by exact name and does not also match notepad++.exe");
    suite.expect(MatchProcessText("System", list).pid == 4U, L"name: System is found by exact name");
    suite.expect(MatchProcessText("system", list).pid == 4U, L"name: exact name is case-insensitive for ASCII");
    suite.expect(MatchProcessText("SYSTEM", list).pid == 4U, L"name: upper case input also matches");

    // 同名多开：chrome.exe 三个（含大小写不同的 Chrome.exe）-> Multiple，全部列出、按 PID 升序，
    // 绝不取第一个；pid 字段保持 0。
    result = MatchProcessText("chrome.exe", list);
    suite.expect(result.kind == ProcessMatchKind::Multiple && result.by == ProcessMatchBy::ExactName,
        L"name: same-name processes are Multiple, never the first one");
    suite.expect(result.pid == 0U, L"name: a Multiple result carries no pid");
    suite.expect(PidsOf(result) == std::vector<std::uint32_t>({ 100U, 150U, 200U }),
        L"name: Multiple lists every match sorted by pid ascending, case variants included");
    suite.expect(PidsOf(MatchProcessText("CHROME.EXE", list)) == std::vector<std::uint32_t>({ 100U, 150U, 200U }),
        L"name: the upper case spelling lists the same three");
    suite.expect(PidsOf(MatchProcessText("  chrome.exe\t", list)) == std::vector<std::uint32_t>({ 100U, 150U, 200U }),
        L"name: surrounding whitespace does not change the list");

    // QQ 三开：输入顺序是 402、400、401，输出必须是 400、401、402。
    result = MatchProcessText("qq.exe", list);
    suite.expect(result.kind == ProcessMatchKind::Multiple && result.by == ProcessMatchBy::ExactName
            && PidsOf(result) == std::vector<std::uint32_t>({ 400U, 401U, 402U }),
        L"name: three QQ processes are listed in pid order regardless of input order");
    suite.expect(result.candidates[0].name == "QQ.exe", L"name: the listed candidates keep their original names");

    // 精确名优先于子串："a.exe" 精确命中 PID 1，虽然 "ba.exe" 也包含它。
    const std::vector<ProcessCandidate> overlap = { Cand(2, "ba.exe"), Cand(1, "a.exe") };
    result = MatchProcessText("a.exe", overlap);
    suite.expect(result.kind == ProcessMatchKind::Unique && result.pid == 1U && result.by == ProcessMatchBy::ExactName,
        L"name: an exact name wins over a substring match");

    // 输入的是子串、不是精确名时才走子串：同一份 overlap，"a." 两个都包含 -> Multiple。
    result = MatchProcessText("a.", overlap);
    suite.expect(result.kind == ProcessMatchKind::Multiple && result.by == ProcessMatchBy::Substring
            && PidsOf(result) == std::vector<std::uint32_t>({ 1U, 2U }),
        L"name: a shared substring is Multiple and sorted by pid");
}

// ------------------------------------------------------------
// 六、显示文本与子串：两级的优先顺序与各自的命中范围。
// ------------------------------------------------------------
void TestDisplayTextAndSubstring(KswordTests::Suite& suite) {
    const std::vector<ProcessCandidate> list = MainList();

    // 精确显示文本能把同名多开里的某一个精确选出来。
    ProcessMatchResult result = MatchProcessText("QQ.exe [PID:401]", list);
    suite.expect(result.kind == ProcessMatchKind::Unique && result.pid == 401U && result.by == ProcessMatchBy::DisplayText,
        L"display: the exact display text picks one of several same-name processes");
    result = MatchProcessText("  chrome.exe [PID:100] ", list);
    suite.expect(result.kind == ProcessMatchKind::Unique && result.pid == 100U && result.by == ProcessMatchBy::DisplayText,
        L"display: surrounding whitespace is ignored");
    result = MatchProcessText("CHROME.EXE [pid:150]", list);
    suite.expect(result.kind == ProcessMatchKind::Unique && result.pid == 150U && result.by == ProcessMatchBy::DisplayText,
        L"display: the display text is also case-insensitive for ASCII");

    // 显示文本的前缀不是精确显示文本：落到子串，三个 QQ 的显示文本都包含 "[PID:40"。
    result = MatchProcessText("QQ.exe [PID:40", list);
    suite.expect(result.kind == ProcessMatchKind::Multiple && result.by == ProcessMatchBy::Substring
            && PidsOf(result) == std::vector<std::uint32_t>({ 400U, 401U, 402U }),
        L"display: a prefix of the display text is a substring match listing all three");

    // 子串：进程名片段。
    result = MatchProcessText("note", list);
    suite.expect(result.kind == ProcessMatchKind::Multiple && result.by == ProcessMatchBy::Substring
            && PidsOf(result) == std::vector<std::uint32_t>({ 300U, 301U }),
        L"substring: note matches both notepad processes");
    result = MatchProcessText("PAD++", list);
    suite.expect(result.kind == ProcessMatchKind::Unique && result.pid == 301U && result.by == ProcessMatchBy::Substring,
        L"substring: PAD++ is a case-insensitive fragment unique to notepad++.exe");
    result = MatchProcessText("svch", list);
    suite.expect(result.kind == ProcessMatchKind::Unique && result.pid == 1234U && result.by == ProcessMatchBy::Substring,
        L"substring: a unique fragment is Unique and reports Substring");

    // 片段位于显示文本的最末尾（含最后一个字符）：滑动窗口的最后一个起点也必须被检查。
    result = MatchProcessText("[PID:401]", list);
    suite.expect(result.kind == ProcessMatchKind::Unique && result.pid == 401U && result.by == ProcessMatchBy::Substring,
        L"substring: a fragment at the very end of the display text matches");

    // "exe" 命中所有带扩展名的进程，System 没有；全部按 PID 升序，共 9 个。
    result = MatchProcessText("exe", list);
    suite.expect(result.kind == ProcessMatchKind::Multiple && result.by == ProcessMatchBy::Substring,
        L"substring: exe matches many processes");
    suite.expect(PidsOf(result) == std::vector<std::uint32_t>({ 100U, 150U, 200U, 300U, 301U, 400U, 401U, 402U, 1234U }),
        L"substring: exe lists nine processes in pid order and leaves out System");

    // "PID" 命中所有显示文本（因为都含 "[PID:"），10 个候选全部列出。
    result = MatchProcessText("pid", list);
    suite.expect(result.kind == ProcessMatchKind::Multiple && result.candidates.size() == 10U,
        L"substring: a fragment of the display text matches every candidate");

    // 没有任何命中：NotFound，by=None。
    result = MatchProcessText("firefox", list);
    suite.expect(result.kind == ProcessMatchKind::NotFound && IsFailureShape(result),
        L"substring: an unknown name is NotFound");

    // 优先级反例一：精确名压过精确显示文本。pid 2 的进程名恰好叫 "x [PID:1]"，
    // 而 pid 1 的显示文本也是 "x [PID:1]"；精确名一级先命中，结果是 pid 2。
    const std::vector<ProcessCandidate> trap = { Cand(1, "x"), Cand(2, "x [PID:1]") };
    result = MatchProcessText("x [PID:1]", trap);
    suite.expect(result.kind == ProcessMatchKind::Unique && result.pid == 2U && result.by == ProcessMatchBy::ExactName,
        L"priority: an exact name wins over an exact display text");

    // 优先级反例二：精确显示文本压过子串。pid 2 的显示文本 "k [PID:1]x [PID:2]" 也包含输入，
    // 但精确显示文本一级先命中，只返回 pid 1，不是 Multiple。
    const std::vector<ProcessCandidate> trap2 = { Cand(1, "k"), Cand(2, "k [PID:1]x") };
    result = MatchProcessText("k [PID:1]", trap2);
    suite.expect(result.kind == ProcessMatchKind::Unique && result.pid == 1U && result.by == ProcessMatchBy::DisplayText,
        L"priority: an exact display text wins over a substring match");
}

// ------------------------------------------------------------
// 七、特殊字符：'+' 等没有任何特殊含义；内部空白原样保留。
// ------------------------------------------------------------
void TestSpecialCharacters(KswordTests::Suite& suite) {
    const std::vector<ProcessCandidate> list = {
        Cand(7, "c++.exe"), Cand(8, "my app.exe"), Cand(9, "a[b].exe"), Cand(10, "mod+10"), Cand(11, "dot.dot.exe"),
    };

    // '+' 不会被当成"模块+偏移"：整串精确名命中，片段按子串命中。
    ProcessMatchResult result = MatchProcessText("c++.exe", list);
    suite.expect(result.kind == ProcessMatchKind::Unique && result.pid == 7U && result.by == ProcessMatchBy::ExactName,
        L"special: c++.exe matches exactly, the plus signs are ordinary characters");
    result = MatchProcessText("c++", list);
    suite.expect(result.kind == ProcessMatchKind::Unique && result.pid == 7U && result.by == ProcessMatchBy::Substring,
        L"special: the fragment c++ matches by substring");
    result = MatchProcessText("mod+10", list);
    suite.expect(result.kind == ProcessMatchKind::Unique && result.pid == 10U && result.by == ProcessMatchBy::ExactName,
        L"special: a name shaped like module+offset is just a name");
    suite.expect(MatchProcessText("mod+11", list).kind == ProcessMatchKind::NotFound,
        L"special: a different offset after the plus is not found, nothing is evaluated");
    suite.expect(MatchProcessText("c+", list).pid == 7U, L"special: a single plus fragment matches by substring");

    // 内部空白原样保留：单空格命中，双空格不命中，子串可以含空格。
    suite.expect(MatchProcessText("my app.exe", list).pid == 8U, L"special: an inner space matches exactly");
    suite.expect(MatchProcessText("my  app.exe", list).kind == ProcessMatchKind::NotFound,
        L"special: two inner spaces do not collapse into one");
    suite.expect(MatchProcessText("my app", list).pid == 8U, L"special: a fragment with an inner space matches");

    // 方括号、点号：都是普通字符。
    suite.expect(MatchProcessText("a[b].exe", list).pid == 9U, L"special: brackets in a name are ordinary characters");
    suite.expect(MatchProcessText("a[b]", list).pid == 9U, L"special: a bracket fragment matches by substring");
    suite.expect(MatchProcessText("dot.dot", list).pid == 11U, L"special: dots are ordinary characters");
    suite.expect(MatchProcessText(".", list).kind == ProcessMatchKind::Multiple,
        L"special: a lone dot is a substring of several names and is Multiple");

    // 含 NUL 字节的输入不是进程名片段，也不能让匹配越界或崩溃。
    const std::string withNul("c++\0.exe", 8);
    suite.expect(MatchProcessText(withNul, list).kind == ProcessMatchKind::NotFound,
        L"special: text with an embedded NUL byte is simply not found");
}

// ------------------------------------------------------------
// 八、非 ASCII：只折叠 ASCII，其余字节原样比较。
// ------------------------------------------------------------
void TestNonAscii(KswordTests::Suite& suite) {
    // 工具 = E5 B7 A5 E5 85 B7；工具箱 = 工具 + E7 AE B1；É = C3 89；é = C3 A9。
    // 字面量一律拆成相邻的短串，避免十六进制转义吞掉后面的字母数字。
    const std::string tool = std::string("\xE5\xB7\xA5") + std::string("\xE5\x85\xB7");
    const std::string toolBox = tool + std::string("\xE7\xAE\xB1");
    const std::string upperE = std::string("\xC3\x89");
    const std::string lowerE = std::string("\xC3\xA9");
    const std::vector<ProcessCandidate> list = {
        Cand(500, (tool + ".exe").c_str()),
        Cand(501, (toolBox + ".exe").c_str()),
        Cand(600, (upperE + ".exe").c_str()),
        Cand(601, (upperE + "A.exe").c_str()),
    };

    // 精确名与子串都按字节比较。
    ProcessMatchResult result = MatchProcessText(tool + ".exe", list);
    suite.expect(result.kind == ProcessMatchKind::Unique && result.pid == 500U && result.by == ProcessMatchBy::ExactName,
        L"non-ascii: a CJK name matches exactly");
    result = MatchProcessText(toolBox, list);
    suite.expect(result.kind == ProcessMatchKind::Unique && result.pid == 501U && result.by == ProcessMatchBy::Substring,
        L"non-ascii: a CJK fragment unique to one name matches by substring");
    result = MatchProcessText(tool, list);
    suite.expect(result.kind == ProcessMatchKind::Multiple && PidsOf(result) == std::vector<std::uint32_t>({ 500U, 501U }),
        L"non-ascii: a CJK fragment shared by two names is Multiple in pid order");

    // 非 ASCII 字母不折叠：É 与 é 不等价。
    suite.expect(MatchProcessText(upperE + ".exe", list).pid == 600U,
        L"non-ascii: the same non-ascii letter matches");
    suite.expect(MatchProcessText(lowerE + ".exe", list).kind == ProcessMatchKind::NotFound,
        L"non-ascii: a different-case non-ascii letter does not match, only ASCII is folded");

    // 混合：非 ASCII 部分原样、ASCII 部分折叠。
    result = MatchProcessText(upperE + "a.EXE", list);
    suite.expect(result.kind == ProcessMatchKind::Unique && result.pid == 601U && result.by == ProcessMatchBy::ExactName,
        L"non-ascii: the ascii part of a mixed name is folded while the other bytes are compared as they are");
}

// ------------------------------------------------------------
// 九、空白的边界：U+3000/NBSP 被去掉，相邻码点与残缺序列不被去掉。
// ------------------------------------------------------------
void TestWhitespaceBoundaries(KswordTests::Suite& suite) {
    const std::vector<ProcessCandidate> list = MainList();

    // 各种空白包住名字：结果与没有空白一致。
    const std::string_view wrapped[] = {
        "\xE3\x80\x80System\xE3\x80\x80",           // 全角空格
        "\xC2\xA0System\xC2\xA0",                   // 不换行空格
        "\t\xE3\x80\x80 System \xC2\xA0\n",         // 混合
        "\xE3\x80\x80\xE3\x80\x80System",           // 连续多个全角空格
        "System\r\n",
    };
    for (const std::string_view text : wrapped) {
        const ProcessMatchResult result = MatchProcessText(text, list);
        suite.expect(result.kind == ProcessMatchKind::Unique && result.pid == 4U,
            L"whitespace: ascii, NBSP and ideographic spaces around a name are trimmed");
    }
    suite.expect(MatchProcessText("\xE3\x80\x80" "100\xC2\xA0", list).pid == 100U,
        L"whitespace: the same trimming applies to a numeric pid");

    // 相邻但不是空白的码点：U+3001（E3 80 81）与 U+00A1（C2 A1）不被去掉，名字因此对不上。
    suite.expect(MatchProcessText("System\xE3\x80\x81", list).kind == ProcessMatchKind::NotFound,
        L"whitespace: U+3001 next to U+3000 is not whitespace");
    suite.expect(MatchProcessText("System\xC2\xA1", list).kind == ProcessMatchKind::NotFound,
        L"whitespace: U+00A1 next to U+00A0 is not whitespace");
    suite.expect(MatchProcessText("\xE3\x80\x81System", list).kind == ProcessMatchKind::NotFound,
        L"whitespace: a leading U+3001 is not trimmed");

    // 残缺的 UTF-8 序列（缺字节）不是空白，也不能把名字的字节误剥掉。
    suite.expect(MatchProcessText("System\xE3\x80", list).kind == ProcessMatchKind::NotFound,
        L"whitespace: a truncated ideographic space is not trimmed");
    suite.expect(MatchProcessText("System\xE3", list).kind == ProcessMatchKind::NotFound,
        L"whitespace: a lone lead byte is not trimmed");
    suite.expect(MatchProcessText("System\xC2", list).kind == ProcessMatchKind::NotFound,
        L"whitespace: a lone C2 byte is not trimmed");

    // 名字内部的全角空格不被去掉：只有前后才修剪。
    const std::vector<ProcessCandidate> inner = { Cand(40, "a\xE3\x80\x80" "b.exe") };
    suite.expect(MatchProcessText("a\xE3\x80\x80" "b.exe", inner).pid == 40U,
        L"whitespace: an inner ideographic space is part of the name");
}

// ------------------------------------------------------------
// 十、候选列表的清洗：重复 PID 只算一个、pid 为 0 的忽略。
// ------------------------------------------------------------
void TestCandidateHygiene(KswordTests::Suite& suite) {
    // 同一个 PID 重复出现（刷新竞态）：只算一个进程，不是 Multiple，保留第一次出现的那条。
    const std::vector<ProcessCandidate> dup = { Cand(10, "dup.exe"), Cand(10, "dup.exe"), Cand(11, "other.exe") };
    ProcessMatchResult result = MatchProcessText("dup.exe", dup);
    suite.expect(result.kind == ProcessMatchKind::Unique && result.pid == 10U && result.candidates.size() == 1U,
        L"hygiene: a duplicated pid is one process, not Multiple");
    result = MatchProcessText("10", dup);
    suite.expect(result.kind == ProcessMatchKind::Unique && result.pid == 10U && result.candidates.size() == 1U,
        L"hygiene: a duplicated pid found by number is still one candidate");

    // 同一 PID、不同名字：按 PID 去重保留第一条；子串命中两条时仍只算一个进程。
    const std::vector<ProcessCandidate> renamed = { Cand(20, "first.exe"), Cand(20, "second.exe") };
    result = MatchProcessText("exe", renamed);
    suite.expect(result.kind == ProcessMatchKind::Unique && result.pid == 20U
            && result.candidates.size() == 1U && result.candidates[0].name == "first.exe",
        L"hygiene: the same pid under two names collapses to the first entry");
    suite.expect(MatchProcessText("second.exe", renamed).pid == 20U,
        L"hygiene: the later name still finds the same process");

    // pid 为 0 的候选一律忽略：名字、显示文本、子串都选不到它。
    const std::vector<ProcessCandidate> idle = { Cand(0, "Idle.exe"), Cand(5, "other.exe") };
    suite.expect(MatchProcessText("Idle.exe", idle).kind == ProcessMatchKind::NotFound,
        L"hygiene: a pid 0 candidate is never selected by exact name");
    suite.expect(MatchProcessText("idle", idle).kind == ProcessMatchKind::NotFound,
        L"hygiene: a pid 0 candidate is never selected by substring");
    suite.expect(MatchProcessText("Idle.exe [PID:0]", idle).kind == ProcessMatchKind::NotFound,
        L"hygiene: a pid 0 candidate is never selected by display text");
    suite.expect(MatchProcessText("exe", idle).pid == 5U, L"hygiene: pid 0 is left out of a substring listing");

    // 空名字的候选：只能经显示文本片段命中。
    const std::vector<ProcessCandidate> nameless = { Cand(20, ""), Cand(21, "x.exe") };
    result = MatchProcessText("[PID:20]", nameless);
    suite.expect(result.kind == ProcessMatchKind::Unique && result.pid == 20U && result.by == ProcessMatchBy::Substring,
        L"hygiene: a nameless candidate is reachable through its display text");

    // 空候选列表：非空文本 NotFound；数字 NotFound；0 仍是 BadPid。
    const std::vector<ProcessCandidate> none;
    suite.expect(MatchProcessText("chrome", none).kind == ProcessMatchKind::NotFound, L"hygiene: no candidates, a name");
    suite.expect(MatchProcessText("100", none).kind == ProcessMatchKind::NotFound, L"hygiene: no candidates, a pid");
    suite.expect(MatchProcessText("0", none).kind == ProcessMatchKind::BadPid,
        L"hygiene: no candidates does not turn BadPid into NotFound");
}

// ------------------------------------------------------------
// 十一、结果形状：失败结论保持安全初值，Unique 与 Multiple 的字段约定。
// ------------------------------------------------------------
void TestResultShapes(KswordTests::Suite& suite) {
    const std::vector<ProcessCandidate> list = MainList();

    // 每种失败结论的形状。
    suite.expect(IsFailureShape(MatchProcessText("", list)), L"shape: Empty has a safe shape");
    suite.expect(IsFailureShape(MatchProcessText("nothing-like-this", list)), L"shape: NotFound has a safe shape");
    suite.expect(IsFailureShape(MatchProcessText("4294967296", list)), L"shape: BadPid has a safe shape");
    suite.expect(IsFailureShape(MatchProcessText("12345", list)), L"shape: a missing pid has a safe shape");

    // Unique：pid 与唯一候选一致。
    ProcessMatchResult unique = MatchProcessText("system", list);
    suite.expect(unique.candidates.size() == 1U && unique.candidates[0].pid == unique.pid,
        L"shape: Unique carries the pid and exactly that candidate");

    // Multiple：pid 为 0，候选至少两个且严格升序。
    ProcessMatchResult multiple = MatchProcessText("exe", list);
    bool ascending = multiple.candidates.size() >= 2U;
    for (std::size_t index = 1; index < multiple.candidates.size(); ++index) {
        if (multiple.candidates[index - 1].pid >= multiple.candidates[index].pid) {
            ascending = false;
        }
    }
    suite.expect(multiple.pid == 0U && ascending, L"shape: Multiple has pid 0 and strictly ascending candidates");

    // 超长文本：不崩、NotFound。
    const std::string huge(20000, 'a');
    suite.expect(MatchProcessText(huge, list).kind == ProcessMatchKind::NotFound,
        L"shape: very long text is simply not found");
}

} // namespace

int RunMemwbProcessMatchTests() {
    KswordTests::Suite suite(L"MEMWB process match");
    TestEnumsDefaultsAndDisplayText(suite);
    TestEmptyText(suite);
    TestNumericText(suite);
    TestNotNumbers(suite);
    TestNumericLiteralBoundaries(suite);
    TestAsciiFoldBoundaries(suite);
    TestExactName(suite);
    TestDisplayTextAndSubstring(suite);
    TestSpecialCharacters(suite);
    TestNonAscii(suite);
    TestWhitespaceBoundaries(suite);
    TestCandidateHygiene(suite);
    TestResultShapes(suite);
    suite.report();
    return suite.failures();
}
