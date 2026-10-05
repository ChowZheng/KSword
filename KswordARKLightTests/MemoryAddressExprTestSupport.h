#pragma once

// 内存工作台地址表达式（MemoryAddressExpr）测试的共用夹具。
//
// 为什么拆出来：测试文件按职责分成"纯文本层面（数字/语法）"与"需要 resolver
// （模块/解引用）"两份，各自不超过 800 行，共用这里的假 resolver 与断言辅助。
// 假 resolver 记录每一次调用，用来证明"纯数字不碰 resolver""语法错误不碰 resolver"。
// 全部是 inline，只被 MemoryAddressExprTests*.cpp 包含。

#include "TestSupport.h"

#include "../shared/evidence/memory_workbench/MemoryAddressExpr.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace memwb_expr_test {

using ksword::memwb::EvaluateAddressExpr;
using ksword::memwb::ExprError;
using ksword::memwb::ExprResult;
using ksword::memwb::IAddressExprResolver;
using ksword::memwb::ModuleLookup;
using ksword::memwb::ParsePlainAddress;

// 64 位上限，溢出边界用。
inline constexpr std::uint64_t kMax = 0xFFFFFFFFFFFFFFFFULL;
// 测试用模块基址，手算时直接加偏移。
inline constexpr std::uint64_t kClientBase = 0x7FF600000000ULL;
inline constexpr std::uint64_t kNtdllBase = 0x7FFA00000000ULL;

// ------------------------------------------------------------
// 假 resolver：记录每一次调用，行为完全由测试预设。
// ------------------------------------------------------------
class FakeResolver final : public IAddressExprResolver {
public:
    // 模块名 -> 基址。精确匹配、区分大小写：测试要看到的正是解析器传下来的原样文本。
    std::map<std::string, std::uint64_t> modules;
    // 模块名 -> 强制返回的查询结果（重名、需要进程）。
    std::map<std::string, ModuleLookup> specialLookups;
    // 地址 -> 该地址处读出的指针值；不在表里的地址读取失败。
    std::map<std::uint64_t, std::uint64_t> memory;
    // 为真时 LookupModule 返回 rawLookupValue 强转出的枚举，用来模拟越界返回值。
    bool forceRawLookup = false;
    int rawLookupValue = 0;
    // 调用计数与实参记录。
    int lookupCalls = 0;
    int readCalls = 0;
    std::vector<std::string> lookupNames;
    std::vector<std::uint64_t> readAddresses;
    std::vector<std::uint32_t> readWidths;

    ModuleLookup LookupModule(const std::string& name, std::uint64_t& baseOut) override {
        ++lookupCalls;
        lookupNames.push_back(name);
        baseOut = 0;
        if (forceRawLookup) {
            // 给一个非零基址：如果解析器错把越界值当成 Found，结果会带出这个值。
            baseOut = 0x1234ULL;
            return static_cast<ModuleLookup>(rawLookupValue);
        }
        const auto special = specialLookups.find(name);
        if (special != specialLookups.end()) {
            return special->second;
        }
        const auto found = modules.find(name);
        if (found == modules.end()) {
            return ModuleLookup::NotFound;
        }
        baseOut = found->second;
        return ModuleLookup::Found;
    }

    bool ReadPointer(
        std::uint64_t address,
        std::uint32_t widthBytes,
        std::uint64_t& valueOut) override {
        ++readCalls;
        readAddresses.push_back(address);
        readWidths.push_back(widthBytes);
        valueOut = 0;
        const auto found = memory.find(address);
        if (found == memory.end()) {
            return false;
        }
        valueOut = found->second;
        return true;
    }

    // 两类调用是否都没发生过。
    bool Untouched() const {
        return lookupCalls == 0 && readCalls == 0;
    }
};

// MakeResolver：带一组标准夹具的假 resolver。
inline FakeResolver MakeResolver() {
    FakeResolver resolver;
    resolver.modules["client.dll"] = kClientBase;
    resolver.modules["ntdll.dll"] = kNtdllBase;
    // 名字本身是合法十六进制串的模块：只能靠引号走模块通道。
    resolver.modules["abc"] = 0xAAAA0000ULL;
    resolver.modules["1233"] = 0x5000000ULL;
    resolver.modules["api-ms-win-core-file-l1-1-0.dll"] = 0x7FFB00000000ULL;
    resolver.modules["7-zip.dll"] = 0x6000000ULL;
    resolver.modules["C:\\Windows\\System32\\ntdll.dll"] = kNtdllBase;
    resolver.modules["a+b.dll"] = 0x1111000ULL;
    resolver.modules["my module.dll"] = 0x2222000ULL;
    resolver.modules["Client.DLL"] = 0x3333000ULL;
    // UTF-8 的"游戏.dll"，用转义字节写，避免源文件编码影响测试。
    resolver.modules["\xE6\xB8\xB8\xE6\x88\x8F.dll"] = 0x4444000ULL;
    resolver.modules["overflow.dll"] = 0xFFFFFFFFFFFFF000ULL;
    resolver.specialLookups["dupe.dll"] = ModuleLookup::Ambiguous;
    resolver.specialLookups["needproc.dll"] = ModuleLookup::NeedsProcess;

    // 指针链：0x1000 -> 0x2000 -> 0x3000 -> 0x4000 -> 0x5000，用来测嵌套解引用。
    resolver.memory[0x1000ULL] = 0x2000ULL;
    resolver.memory[0x2000ULL] = 0x3000ULL;
    resolver.memory[0x3000ULL] = 0x4000ULL;
    resolver.memory[0x4000ULL] = 0x5000ULL;
    resolver.memory[0x1008ULL] = 0x7777ULL;
    resolver.memory[kClientBase + 0x10ULL] = 0x500ULL;
    // 32 位宽度边界：高位非零的值与恰为 0xFFFFFFFF 的值。
    resolver.memory[0x6000ULL] = 0x100000000ULL;
    resolver.memory[0x6008ULL] = 0xFFFFFFFFULL;
    resolver.memory[0x7000ULL] = kMax;
    return resolver;
}

// ------------------------------------------------------------
// 断言辅助：失败时的标签里带上输入和实际结果，一眼看出哪条、得到了什么。
// ------------------------------------------------------------
inline std::wstring Widen(const std::string& text) {
    std::wstring wide;
    for (const char character : text) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (byte >= 0x20U && byte < 0x7FU) {
            wide.push_back(static_cast<wchar_t>(byte));
        } else {
            wide.push_back(L'?');
        }
    }
    return wide;
}

inline std::wstring Describe(const ExprResult& result) {
    std::wostringstream stream;
    stream << L"{ok=" << (result.ok ? 1 : 0)
           << L" value=0x" << std::hex << result.value << std::dec
           << L" err=" << static_cast<int>(result.error)
           << L" pos=" << result.errorPosition
           << L" detail='" << Widen(result.detail) << L"'"
           << L" mod=" << (result.usedModule ? 1 : 0)
           << L" deref=" << (result.usedDeref ? 1 : 0) << L"}";
    return stream.str();
}

inline std::wstring Label(const wchar_t* what, const std::string& text, const ExprResult& result) {
    std::wostringstream stream;
    stream << L"addr expr: " << what << L" [" << Widen(text) << L"] got " << Describe(result);
    return stream.str();
}

inline ExprResult Eval(const std::string& text, FakeResolver* resolver, std::uint32_t width = 8U) {
    return EvaluateAddressExpr(text, width, resolver);
}

// 成功：ok、值相等，且错误三元组都是初值。
inline bool IsOk(const ExprResult& result, std::uint64_t expected) {
    return result.ok && result.value == expected && result.error == ExprError::None
        && result.detail.empty() && result.errorPosition == 0;
}

// 失败：错误码、偏移、详情全对，且 value 为 0、其余输出字段为初值。
inline bool IsFail(
    const ExprResult& result,
    ExprError error,
    std::size_t position,
    const std::string& detail) {
    return !result.ok && result.error == error && result.errorPosition == position
        && result.detail == detail && result.value == 0 && !result.usedModule
        && !result.usedDeref && result.moduleName.empty();
}

inline void ExpectOk(
    KswordTests::Suite& suite,
    const wchar_t* what,
    const std::string& text,
    const ExprResult& result,
    std::uint64_t expected) {
    suite.expect(IsOk(result, expected), Label(what, text, result).c_str());
}

inline void ExpectFail(
    KswordTests::Suite& suite,
    const wchar_t* what,
    const std::string& text,
    const ExprResult& result,
    ExprError error,
    std::size_t position,
    const std::string& detail) {
    suite.expect(IsFail(result, error, position, detail), Label(what, text, result).c_str());
}

// 表项：纯文本层面的成功与失败用例。
struct OkCase {
    const char* text;
    std::uint64_t value;
};

struct FailCase {
    const char* text;
    ExprError error;
    std::size_t position;
    const char* detail;
};

// RunMemwbAddressExprResolverChecks：需要 resolver 的那一半检查（模块、解引用），
// 定义在 MemoryAddressExprTests.Resolver.cpp，由 RunMemwbAddressExprTests 调用。
void RunMemwbAddressExprResolverChecks(KswordTests::Suite& suite);

} // namespace memwb_expr_test
