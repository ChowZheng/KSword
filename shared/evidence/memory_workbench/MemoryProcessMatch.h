#pragma once

// ============================================================
// MemoryProcessMatch.h
// 作用：
// - 把目标条"目标进程"输入框里的文本匹配成候选进程：纯数字按 PID，否则按进程名/显示文本/子串。
// - 解决的缺陷：旧 findDriverMemoryProcessComboMatch 在同名多开（如 QQ 一开就是十个同名进程）
//   时静默取**第一个**命中，选错之后表现为"地址读不到"，排查会被整个带偏到内存读取上去。
//   这里多个命中一律返回 Multiple 并列出全部 PID，绝不替用户挑一个。
//
// 冻结接口摘要（后续 E-K 各包依赖，改动须经主会话批准）：
//   struct ProcessCandidate { uint32_t pid; std::string name; }   一个候选进程（name 为 UTF-8）。
//   enum class ProcessMatchKind     Empty / NotFound / Unique / Multiple / BadPid。
//   enum class ProcessMatchBy       None / Pid / ExactName / DisplayText / Substring：哪一级命中了。
//   struct ProcessMatchResult { kind, by, pid, candidates }
//                                   Unique 时 pid=命中的 PID 且 candidates 恰一项；
//                                   Multiple 时 pid=0 且 candidates 为全部命中、按 PID 升序；
//                                   其余 pid=0、candidates 为空。
//   std::string ProcessDisplayText(const ProcessCandidate&)       显示文本 "name [PID:pid]"。
//   ProcessMatchResult MatchProcessText(std::string_view text, const std::vector<ProcessCandidate>&)
//
// 匹配规则（先命中先返回；每一级命中 0 个才进入下一级）：
//   0. 文本去掉前后空白（ASCII 空白、U+00A0、U+3000）；为空 -> Empty（界面据此表示"跟随 Dock"）。
//   1. 纯十进制数字，或 0x/0X 加至少一位十六进制数字 -> 按 PID：
//        值为 0 或超过 32 位 -> BadPid；否则在候选里精确查 PID，找到 Unique，找不到 NotFound。
//        数字文本**不会**再按进程名匹配（"7" 不会去撞名叫 7 的进程）。
//        不带 0x 的十六进制样子（如 "4d2"）、带正负号的数字都不是数字，走名字匹配。
//   2. 精确进程名（ASCII 大小写不敏感）。
//   3. 精确显示文本（同样 ASCII 大小写不敏感），形如 "chrome.exe [PID:1234]"。
//   4. 子串：文本是进程名或显示文本的子串（ASCII 大小写不敏感）。
//   每一级：命中 1 个 -> Unique；命中多个 -> Multiple；0 个 -> 下一级。
//
// 规则：
// - 大小写折叠只处理 ASCII：A-Z 折成 a-z；非 ASCII 字节（UTF-8 汉字等）原样逐字节比较，
//   所以 "É" 与 "é" 不等价，测试钉住这一点。
// - 名字里的 '+'、'.'、'[' 等字符没有任何特殊含义，不做"模块+偏移"之类的解析。
// - pid 为 0 的候选一律忽略（系统空闲进程不可作为目标）；同一个 PID 在候选里重复出现只算一个进程
//   （保留第一次出现的那条），不构成 Multiple。
// - Multiple 的 candidates 按 PID 升序，保证界面列表与测试都是确定的。
// - 本模块不拼任何用户可见文案；界面层按 kind 翻译。
// - 仅使用标准库，不包含 Windows.h，不包含任何 Qt 头。输入用 UTF-8，由 Qt 层 toUtf8() 转换。
// ============================================================

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ksword::memwb
{
    // ProcessCandidate：一个候选进程。
    struct ProcessCandidate
    {
        // pid：进程号。为 0 的候选在匹配时被忽略。
        std::uint32_t pid = 0;
        // name：进程名（UTF-8），如 "chrome.exe"。
        std::string name;
    };

    // ProcessMatchKind：匹配结论的种类。数值固定。
    enum class ProcessMatchKind : std::uint32_t
    {
        // Empty：文本为空或只有空白。界面据此表示"跟随 Dock"。
        Empty = 0,
        // NotFound：没有任何候选命中（含数字 PID 不在候选里）。
        NotFound = 1,
        // Unique：恰好命中一个进程。
        Unique = 2,
        // Multiple：命中多个进程，必须由用户进一步指定，绝不取第一个。
        Multiple = 3,
        // BadPid：文本是数字，但值为 0 或超过 32 位，不可能是 PID。
        BadPid = 4,
    };

    // ProcessMatchBy：由哪一级规则命中。数值固定。
    enum class ProcessMatchBy : std::uint32_t
    {
        // None：没有命中（Empty/NotFound/BadPid）。
        None = 0,
        // Pid：数字文本按 PID 命中。
        Pid = 1,
        // ExactName：精确进程名命中。
        ExactName = 2,
        // DisplayText：精确显示文本命中。
        DisplayText = 3,
        // Substring：子串命中。
        Substring = 4,
    };

    // ProcessMatchResult：MatchProcessText 的结论。默认值是"空文本"的安全初值。
    struct ProcessMatchResult
    {
        // kind：结论种类。
        ProcessMatchKind kind = ProcessMatchKind::Empty;
        // by：由哪一级规则命中；没有命中时为 None。
        ProcessMatchBy by = ProcessMatchBy::None;
        // pid：Unique 时是命中的 PID，其余为 0。
        std::uint32_t pid = 0;
        // candidates：Unique 时恰一项；Multiple 时是全部命中、按 PID 升序；其余为空。
        std::vector<ProcessCandidate> candidates;
    };

    // ProcessDisplayText：候选进程的显示文本，固定为 "进程名 [PID:十进制 PID]"。
    // 传入：candidate 候选进程。传出：显示文本。
    // 用途：目标条的补全列表必须显示同一个格式，这样用户从列表里复制/选中的文本能被精确命中。
    std::string ProcessDisplayText(const ProcessCandidate& candidate);

    // MatchProcessText：把输入文本匹配到候选进程，规则见文件头。
    // 传入：text 用户输入（UTF-8）；candidates 当前可见的候选进程（可含重复与 pid 为 0 的项）。
    // 传出：ProcessMatchResult。不修改 candidates；Empty/NotFound/BadPid 时 by=None、pid=0、candidates 为空
    // （保持安全初值，调用方不会误取到一个"看似有效"的 PID）。
    ProcessMatchResult MatchProcessText(std::string_view text, const std::vector<ProcessCandidate>& candidates);
}
