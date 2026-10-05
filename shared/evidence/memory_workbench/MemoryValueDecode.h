#pragma once

// ============================================================
// MemoryValueDecode.h
// 作用：
// - 内存工作台"数据解释器"的逻辑层：把光标处的一段字节解释成一整列固定的类型行
//   （整数、浮点、指针、时间、GUID、字符串），并提供反方向的写入编码
//   （把用户在解释器里键入的文本编码回字节）。
//   纯 C++20 标准库实现，不依赖 Qt、不依赖 Win32，可在离线套件里直接穷举测试。
//
// 为什么要有这个模块：
// - 旧控件的解释器由"选区长度"决定能看到什么类型：选 4 个字节才出现 u32，选 8
//   个字节才出现 f64，用户必须先猜对选区才能看到想要的解释。本模块改成"跟随光标"：
//   调用方给出光标处起始的一窗字节（建议至少 32 字节），本模块恒返回固定的 17 行，
//   字节不足的行标记为不可用，而不是不出现。
// - 这类转换属于"算错了不会报错"的一类：时间换算差一天、有符号数补码错位、字节序
//   反了，界面照样显示一个像样的值。所以公历换算自己实现并被穷举测试钉死，
//   不使用 gmtime/localtime（32 位 time_t 与各平台差异会让结果随环境漂移）。
//
// 行（DecodedRow）与固定顺序（kDecodedRowCount = 17）：
//   i8 u8 i16 u16 i32 u32 i64 u64 f32 f64 ptr filetime time_t32 time_t64 guid ascii utf16
//   label 是英文固定键（界面按它取本地化名称，不得翻译、不得改动）。
//   * 整数：text = 十进制 + 空格 + 括号内 0x 十六进制，例如 "-1 (0xFF)"、
//     "305419896 (0x12345678)"；十六进制恒为"类型宽度补零的大写位模式"，有符号数
//     显示其补码位模式（i8 的 -1 是 0xFF，不是 -0x1）；copyText = 仅十进制。
//   * f32/f64：std::to_chars 的最短往返表示（如 0.1 显示 "0.1"，不会显示
//     0.10000000149011612）；NaN（含任何符号/载荷）显示 "NaN"，正负无穷显示 "Inf"
//     与 "-Inf"，负零显示 "-0"；copyText 与 text 相同。判据只看位模式，
//     不对信号 NaN 做任何浮点运算。
//   * ptr：宽度由 pointerWidthBytes 决定（4 或 8，其它值该行不可用），text =
//     "0x" + 补零到 2*宽度 位的大写十六进制；若 namer 非空且 Describe 返回 true
//     且文本非空，则追加 " (" + 描述 + ")"；copyText 只含十六进制，不含描述。
//     字节不足时不会调用 namer。
//   * filetime：8 字节，100ns 自 1601-01-01 UTC。范围限定为 1601..9999 年；
//     值 0 是各类结构里通用的"未设置"哨兵，显示成 1601-01-01 会误导，所以与超过
//     9999-12-31 23:59:59.9999999 的值一起判 valid=false。
//     text = "YYYY-MM-DD hh:mm:ss.fffffff UTC"，copyText = ISO 8601
//     "YYYY-MM-DDThh:mm:ss.fffffffZ"。
//   * time_t32 / time_t64：有符号 4/8 字节，自 1970-01-01 UTC 的秒数。范围限定为
//     公历 0001-01-01 00:00:00 .. 9999-12-31 23:59:59；32 位恒在范围内，64 位超出
//     判 valid=false。text = "YYYY-MM-DD hh:mm:ss UTC"，copyText =
//     "YYYY-MM-DDThh:mm:ssZ"。年份恒为 4 位补零。
//   * 时间行 valid=false 时：text 为 "out of range"，copyText 为原始数值的十进制
//     （filetime 无符号、time_t 有符号），这样用户仍能复制出底层数值。
//   * guid：16 字节，Data1/Data2/Data3 按小端、Data4 按字节顺序（Windows 内存布局）；
//     text = 8-4-4-4-12 小写十六进制，不含花括号；copyText = "{...}" 大写。
//   * ascii：从起点取到首个 NUL、首个不可打印字节（可打印 = 0x20..0x7E）或满 32 字节，
//     至少 1 个可打印字节才 available；text 与 copyText 都是该串本身，不加引号。
//   * utf16：UTF-16LE，取到首个 U+0000 或起点位于前 16 个码元内的字符为止（起点在窗
//     口内的代理对即使第二个码元落在窗口外也会补全）；奇数多余的一个字节忽略。
//     孤立代理项输出 U+FFFD 并令 valid=false；输出为 UTF-8。至少 1 个字符才
//     available。
//
// 字节序：ByteOrder 只影响整数、浮点、指针、filetime、time_t。guid（Windows 内存
// 布局）与 utf16（Windows wchar_t 内存布局）固定小端，ascii 无字节序，三者不随
// ByteOrder 变化——这是刻意的，界面不应让字节序开关去改它们。
//
// available 与 valid 是两个正交的标志：
// - available=false：字节不足（或 ascii/utf16 没有任何字符，或 ptr 宽度非法），
//   text 与 copyText 为空，valid 恒为 true（没有内容也就谈不上内容非法）。
// - available=true 且 valid=false：字节足够但内容不合法（时间超范围、孤立代理项）。
// 界面应先判 available，再判 valid；Usable() 把两者合并成"可以放心展示/复制"。
//
// 写入编码（EncodeValue）：解释器里直接改值时，把文本编码成该类型的字节：
// - type 取 "i8" "u8" "i16" "u16" "i32" "u32" "i64" "u64" "f32" "f64" "ptr"
//   （小写、精确匹配；filetime/guid/字符串类不支持编码，返回 UnknownType）。
//   ptr 的宽度取 pointerWidthBytes（4 或 8），其它值返回 UnknownType。
// - 文本两端的空白（空格、制表、回车、换行）被忽略；其余位置出现多余字符一律
//   BadNumber。不接受 '+' 前缀，不接受千分位、下划线或汇编风格后缀。
// - 整数：十进制，或带 0x/0X 前缀的十六进制；有符号类型允许前导 '-'（含 "-0x80"）。
//   取的是"数值"语义：i8 键入 0xFF 是 255，超出 i8 范围而被拒绝（不会按位模式
//   回绕成 -1）；要写位模式请选对应的无符号类型。"-0" 等于 0，对所有整数类型有效。
//   无符号类型与 ptr 键入非零负数是 OutOfRange。
// - 浮点：std::from_chars（十进制与科学计数法），另接受不区分大小写的 nan、inf、
//   infinity（可带前导 '-'），这样解释器显示的 "NaN" "Inf" "-Inf" 能原样键回。
//   NaN 编码为静默 NaN（载荷不保留）。十六进制浮点不接受。
// - 状态码：
//     Ok            成功，bytesOut 为该类型宽度的字节；
//     UnknownType   类型名不认识或 ptr 宽度非法（先于文本检查，坏文本也先报它）；
//     BadNumber     文本不是该类型的合法数字语法（空、多余字符、无数位、整数里的
//                   小数点或指数、浮点里的 '+'）；
//     OutOfRange    语法合法、数值在 64 位内，但超出该类型范围（i8 的 128 与 -129、
//                   u8 的 256 与 -1、f32 的 3.5e38，以及非零数下溢成 0 的浮点）；
//     Overflow      整数数值本身放不进 64 位（大于 18446744073709551615），先于
//                   类型范围检查，所以 u8 键入 99999999999999999999 是 Overflow。
// - 失败时 bytesOut 保持清空（每次调用先清空），绝不留半截结果；绝不回绕、绝不截断。
//
// 测试：KswordARKLightTests/MemoryValueDecodeTests.cpp（套件入口
// RunMemwbValueDecodeTests，套件名 "MEMWB value decode"）。
// ============================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ksword::memwb
{
    // ByteOrder：多字节数值的字节序。Little 即 x86 内存里的自然顺序。
    enum class ByteOrder
    {
        Little = 0,
        Big,
    };

    // kDecodedRowCount：DecodeAll 恒返回的行数。界面可据此预建固定行数的表格。
    inline constexpr std::size_t kDecodedRowCount = 17;

    // DecodedRow：解释器里的一行。
    struct DecodedRow
    {
        // label：英文固定键（i8、u16、f32、ptr、filetime、guid、ascii、utf16 等）。
        std::string label;
        // text：展示文本；不可用行为空；valid=false 的时间行为 "out of range"。
        std::string text;
        // copyText：复制用文本，格式见文件头逐类型说明；不可用行为空。
        std::string copyText;
        // available：字节是否足够解出这一行（ascii/utf16 还要求至少有一个字符）。
        bool available = false;
        // valid：字节足够但内容是否合法；不可用行恒为 true，见文件头说明。
        bool valid = true;

        // Usable：available 且 valid，即可以放心展示并复制。
        bool Usable() const
        {
            return available && valid;
        }
    };

    // IPointerNamer：指针行用的"地址描述"能力，由调用方注入（例如查模块区间）。
    // 这样本模块不依赖任何进程/驱动，测试里用假实现即可统计调用。
    class IPointerNamer
    {
    public:
        // 虚析构：允许经基类指针销毁实现。
        virtual ~IPointerNamer() = default;

        // Describe：描述一个地址。
        // 传入：address 指针值（已按字节序解出，32 位指针零扩展）；out 进来时为空。
        // 传出：返回 true 且 out 非空表示命中，out 形如 "module.dll+0x1A40"；
        //       返回 false 或 out 为空都当作未命中，指针行不追加描述。
        virtual bool Describe(std::uint64_t address, std::string& out) = 0;
    };

    // DecodeAll：把一窗字节解释成固定的 kDecodedRowCount 行。
    // 传入：
    //   bytes             光标处起始的字节；为空指针时视作长度 0；
    //   length            可用字节数（可以是 0，也可以远大于 16）；
    //   order             整数/浮点/指针/时间的字节序；
    //   pointerWidthBytes 指针宽度，只允许 4 或 8（其它值令 ptr 行不可用）；
    //   namer             指针描述器，可为空。
    // 传出：按固定顺序排列的行；字节不足的行 available=false。
    std::vector<DecodedRow> DecodeAll(
        const std::uint8_t* bytes,
        std::size_t length,
        ByteOrder order,
        std::uint32_t pointerWidthBytes,
        IPointerNamer* namer = nullptr);

    // DecodeAll 的 vector 便捷重载，语义完全相同，长度取 bytes.size()。
    inline std::vector<DecodedRow> DecodeAll(
        const std::vector<std::uint8_t>& bytes,
        ByteOrder order,
        std::uint32_t pointerWidthBytes,
        IPointerNamer* namer = nullptr)
    {
        return DecodeAll(bytes.data(), bytes.size(), order, pointerWidthBytes, namer);
    }

    // EncodeStatus：EncodeValue 的结果，每一项的精确含义见文件头"状态码"。
    enum class EncodeStatus
    {
        Ok = 0,
        UnknownType,
        BadNumber,
        OutOfRange,
        Overflow,
    };

    // EncodeValue：把用户键入的文本编码成指定类型的字节（解释器里直接改值用）。
    // 传入：
    //   type              类型名，见文件头；
    //   text              用户键入的文本（UTF-8，两端空白被忽略）；
    //   order             写出的字节序；
    //   pointerWidthBytes type 为 "ptr" 时的宽度（4 或 8），其它类型忽略；
    //   bytesOut          输出字节。
    // 传出：见 EncodeStatus；成功时 bytesOut 恰好是该类型的宽度，失败时为空。
    EncodeStatus EncodeValue(
        std::string_view type,
        std::string_view text,
        ByteOrder order,
        std::uint32_t pointerWidthBytes,
        std::vector<std::uint8_t>& bytesOut);
}
