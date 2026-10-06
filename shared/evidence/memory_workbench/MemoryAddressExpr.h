#pragma once

// ============================================================
// MemoryAddressExpr.h
// 作用：
// - 内存工作台统一的"地址表达式"解析与求值器，取代现状 4 套互相矛盾的地址语法。
//   纯 C++20 标准库实现，不依赖 Qt、不依赖 Win32，可在离线套件里直接穷举测试。
//
// 为什么必须单独成层并被穷举测试：
// - 这类解析器属于"算错了不会报错"的一类。解析成另一个数后界面照样显示、驱动
//   照样去读，只是读到了别处。已经踩过的坑：纯数字串 `1233` 被按十进制解析成
//   0x4D1，安静地跳到另一个地址；基址加偏移回绕后得到的同样是个合法地址。
// - 所以本层的三条铁律是：默认十六进制、溢出判失败绝不回绕、失败时 value 恒为 0。
//
// 文法（表达式 = 项 ( '+' 项 )*）：
//   项 = 数字 | 模块名 | '[' 表达式 ']'
//
//   数字：
//     * `0x` / `0X` 前缀或无前缀，一律十六进制；`1233` 就是 0x1233，绝不是十进制。
//     * 可含 '`' 与 '_' 分隔（调试器粘贴格式，如 fffff800`12345678）。分隔符
//       只在数位之间合法：首尾、连续、紧跟 0x 之后都是 BadNumber。
//     * 数字解析复用 ksword::evidence::ParseNumericText（地址默认十六进制、
//       溢出判失败），本模块不另写一套数位累加。
//
//   模块名：
//     * 带引号（双引号或单引号）的任意文本。引号内不做转义（Windows 路径含反斜杠），
//       内容两端空白会被剪掉，剪完为空是 BadSyntax。引号内可含 '+'。
//     * 或者含有非十六进制字符的无引号记号，如 client.dll、ntdll.dll、
//       api-ms-win-core-file-l1-1-0.dll、C:\Windows\System32\ntdll.dll。
//     * 无引号记号只要"整串都是十六进制字符（含 0x 前缀与分隔符）"，一律按数字
//       解释。所以名叫 abc 的模块必须写成 "abc"；这是刻意的、被测试钉死的。
//       判据只看字符集：`0x`、`1__2` 字符集合格但格式不合格，得到 BadNumber，
//       而不是被当成模块名；`0x12g4`、`abc.dll` 含非十六进制字符，是模块名。
//     * 一个表达式里模块项最多出现一次，并且必须是它所在那一层（顶层或某对方括号
//       内）的第一项；违反判 BadSyntax。
//     * 模块通过调用方注入的 IAddressExprResolver 解析，本模块不接触任何进程。
//
//   解引用 '[' 表达式 ']'：
//     * 先求内部表达式的值，再经 ReadPointer 按 widthBytes（32 位目标 4、64 位
//       目标 8）读出指针值，作为这一项的值。读取失败判 DerefFailed。
//     * 支持嵌套，深度上限 kMaxAddressExprDerefDepth（4），超限判 BadSyntax。
//
//   不支持减法：
//     * 模块名里合法地含 '-'（api-ms-win-...），所以 '-' 不能当运算符。无引号
//       记号含 '-' 且其各个非空片段全是数字（如 0x100-10、100-、ab-cd），明确判
//       BadSyntax，绝不悄悄当成模块名；7-zip.dll 这类有非数字片段的才是模块名。
//       取舍：`client.dll-10` 与带连字符的真模块名无法区分，按模块名整体交给
//       resolver（查不到就是 UnknownModule，详情里能看到整个记号）。
//
//   求值：
//     * 所有项相加，任何一步溢出判 Overflow，绝不回绕。
//     * 先整体解析再求值：任何语法错误都在第一次调用 resolver 之前被发现，
//       因此坏输入不会触发一次多余的内存读取；纯数字表达式永远不调用 resolver。
//
// 错误报告：
// - 只返回错误码、出错记号文本与出错字节偏移，不拼接面向用户的句子，界面层翻译。
// - 失败时除 error/detail/errorPosition 外，ExprResult 的全部字段都是初值
//   （value==0、usedModule==false、usedDeref==false、moduleName 为空）。
//
// 偏移规则：errorPosition 是出错记号在**原始输入**里的字节偏移（含前导空白）。
// 缺少右括号时指向对应的 '['；缺少项（如尾随加号）时指向输入末尾；
// 数字减法指向第一个 '-'。
// 详情规则：detail 对数字是数字记号原文，对模块项是模块名（已去引号），
// 对解引用项是含方括号的整段文本（求值失败时指向最内层出错的那一项）。
//
// 测试：KswordARKLightTests/MemoryAddressExprTests.cpp（套件入口 RunMemwbAddressExprTests，
// 套件名 "MEMWB addr expr"）+ MemoryAddressExprTests.Resolver.cpp（需要 resolver 的那
// 一半，由入口调用）+ MemoryAddressExprTestSupport.h（假 resolver 与断言辅助）。
// ============================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ksword::memwb
{
    // kMaxAddressExprDerefDepth：方括号解引用的最大嵌套层数。
    // 4 层足够覆盖常见的多级指针链，同时给递归一个硬上限，坏输入无法压爆栈。
    inline constexpr int kMaxAddressExprDerefDepth = 4;

    // ModuleLookup：模块名查询结果。
    // Found 命中且 baseOut 有效；NotFound 没有这个模块；Ambiguous 重名，调用方
    // 应要求用户给完整路径；NeedsProcess 当前没有可查询的进程上下文。
    enum class ModuleLookup
    {
        Found = 0,
        NotFound,
        Ambiguous,
        NeedsProcess,
    };

    // ExprError：求值失败的原因。None 仅在成功时出现。
    enum class ExprError
    {
        None = 0,
        Empty,            // 输入为空或只有空白
        BadNumber,        // 数字记号格式非法（分隔符位置不对、0x 后没有数位）
        Overflow,         // 数字或求和超出 64 位，不回绕
        UnknownModule,    // 模块名查不到
        AmbiguousModule,  // 模块名重名，需要完整路径
        NeedsProcess,     // 需要进程上下文而没有（resolver 为空或模块查询要求进程）
        BadSyntax,        // 文法错误（含减法、模块位置不对、括号不配对、嵌套过深）
        DerefFailed,      // 解引用读取指针失败
    };

    // IAddressExprResolver：求值时需要的外部能力，由调用方注入。
    // 这样本模块不依赖任何进程/驱动，测试里用假实现即可统计调用次数。
    class IAddressExprResolver
    {
    public:
        // 虚析构：允许经基类指针销毁实现。
        virtual ~IAddressExprResolver() = default;

        // LookupModule：按名字查模块基址。
        // 传入：name 模块名（已去引号并剪掉两端空白，大小写保持原样）。
        // 传出：baseOut 仅当返回 Found 时有意义；返回值见 ModuleLookup。
        virtual ModuleLookup LookupModule(const std::string& name, std::uint64_t& baseOut) = 0;

        // ReadPointer：在目标地址空间读一个指针。
        // 传入：address 读取地址；widthBytes 指针宽度（4 或 8）。
        // 传出：valueOut 零扩展后的指针值（宽度为 4 时必须不超过 0xFFFFFFFF）；
        //       返回 false 表示读取失败。
        virtual bool ReadPointer(
            std::uint64_t address,
            std::uint32_t widthBytes,
            std::uint64_t& valueOut) = 0;
    };

    // ExprResult：一次求值的完整结果。
    struct ExprResult
    {
        // ok：是否成功。
        bool ok = false;
        // value：求值结果；失败时恒为 0。
        std::uint64_t value = 0;
        // error：失败原因；成功时为 None。
        ExprError error = ExprError::None;
        // detail：出错的记号文本（不含面向用户的句子）；成功时为空。
        std::string detail;
        // errorPosition：出错处在原始输入中的字节偏移；成功时为 0。
        std::size_t errorPosition = 0;
        // usedModule：表达式是否含模块项（仅成功时可能为 true）。
        bool usedModule = false;
        // moduleName：所用模块名（已去引号）；没用模块时为空。
        std::string moduleName;
        // usedDeref：表达式是否含解引用项（仅成功时可能为 true）。
        bool usedDeref = false;
    };

    // EvaluateAddressExpr：解析并求值一个地址表达式。
    // 传入：
    //   text       表达式文本（UTF-8），两端与记号之间的空白被忽略；
    //   widthBytes 解引用时读取的指针宽度，只允许 4 或 8（其它值遇到解引用判
    //              DerefFailed，且不会调用 ReadPointer；纯数字/模块表达式不受影响）；
    //   resolver   外部能力；为空指针时遇到模块项或解引用项返回 NeedsProcess。
    // 传出：ExprResult，失败时 value 为 0 且 error 指明原因。
    ExprResult EvaluateAddressExpr(
        std::string_view text,
        std::uint32_t widthBytes,
        IAddressExprResolver* resolver);

    // ParsePlainAddress：只解析"纯数字"地址（0x/裸十六进制，可含 ` 与 _ 分隔），
    // 供不需要模块与解引用的场景使用。两端空白被剪掉；含加号、方括号、模块名、
    // 内部空格、溢出等一律返回 false。
    // 传出：value 成功时为解析结果；失败时恒为 0（即使调用前有残留值）。
    bool ParsePlainAddress(std::string_view text, std::uint64_t& value);
}
