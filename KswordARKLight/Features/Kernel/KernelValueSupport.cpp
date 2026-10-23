#include "KernelValueSupport.h"

#include "KernelPage.h"

#include "KernelColumnVisibility.h"
#include "KernelPageLayout.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace Ksword::Features::Kernel {

std::shared_ptr<const KernelRawRows> EmptyKernelRawRows() {
    static const auto empty = std::make_shared<const KernelRawRows>();
    return empty;
}

// BoolText converts metadata or result booleans into compact Chinese labels.
// Input is a boolean value; output is "是" or "否".
const wchar_t* BoolText(const bool value) {
    return value ? L"是" : L"否";
}

// HexToUInt64 parses decimal or 0x-prefixed protocol numbers from cached rows.
// Input is display text emitted by KernelFacade; processing uses wcstoull and
// requires full-token consumption; output is false when the cell is missing or
// not numeric.
bool HexToUInt64(const std::wstring& text, std::uint64_t& valueOut) {
    if (text.empty()) {
        return false;
    }
    wchar_t* end = nullptr;
    const int base = text.size() > 2 && text[0] == L'0' && (text[1] == L'x' || text[1] == L'X') ? 16 : 10;
    const unsigned long long value = std::wcstoull(text.c_str(), &end, base);
    if (end == text.c_str() || *end != L'\0') {
        return false;
    }
    valueOut = static_cast<std::uint64_t>(value);
    return true;
}

// RowFieldByName reads a named value from a cached dynamic row. Inputs are a row,
// the current column names and one or more accepted column aliases; processing is
// case-insensitive and preserves the original display value; output is empty when
// no alias is present or the selected cell is blank.
std::wstring RowFieldByName(const std::vector<std::wstring>& row,
    const std::vector<std::wstring>& columns,
    std::initializer_list<const wchar_t*> names) {
    for (const wchar_t* name : names) {
        for (std::size_t column = 0; column < columns.size() && column < row.size(); ++column) {
            if (_wcsicmp(columns[column].c_str(), name) == 0 && !row[column].empty()) {
                return row[column];
            }
        }
    }
    return {};
}

// FirstRowValue returns the first non-empty field with one of the supplied
// aliases. Inputs are a row cache and column aliases; output is empty when no
// matching cell is present.
std::wstring FirstRowValue(
    const std::vector<std::vector<std::wstring>>& rows,
    const std::vector<std::wstring>& columns,
    std::initializer_list<const wchar_t*> names) {
    for (const std::vector<std::wstring>& row : rows) {
        const std::wstring value = RowFieldByName(row, columns, names);
        if (!value.empty()) {
            return value;
        }
    }
    return {};
}

// FirstRowUInt64 returns a numeric value from the first matching cached row.
// Inputs are row data, columns and aliases; processing accepts hexadecimal and
// decimal display forms; output uses fallback when no numeric value is found.
std::uint64_t FirstRowUInt64(
    const std::vector<std::vector<std::wstring>>& rows,
    const std::vector<std::wstring>& columns,
    std::initializer_list<const wchar_t*> names,
    const std::uint64_t fallback) {
    for (const std::vector<std::wstring>& row : rows) {
        std::uint64_t value = 0;
        if (HexToUInt64(RowFieldByName(row, columns, names), value)) {
            return value;
        }
    }
    return fallback;
}

// FlagEnabled checks whether a numeric bitmask contains a requested flag. Inputs
// are protocol flags and a flag value; output is true when all flag bits are set.
bool FlagEnabled(const std::uint64_t flags, const std::uint64_t flag) {
    return (flags & flag) == flag;
}

// IsZeroMaskText identifies empty dependency masks in their common display
// forms. Input is a cell string; output is true for blank, decimal zero, or
// hexadecimal zero variants.
bool IsZeroMaskText(const std::wstring& text) {
    std::uint64_t value = 0;
    return text.empty() || (HexToUInt64(text, value) && value == 0);
}

// NamedMaskText renders a protocol bitset as "MACRO (中文)" entries. Inputs are
// a mask and table; processing preserves original KernelDock names; output is
// "None" when no known bit is enabled.
std::wstring NamedMaskText(const std::uint64_t mask, const NamedMask* items, const std::size_t count) {
    std::wstring text;
    for (std::size_t index = 0; index < count; ++index) {
        const NamedMask& item = items[index];
        if ((mask & item.mask) != item.mask) {
            continue;
        }
        if (!text.empty()) {
            text += L", ";
        }
        text += item.name;
        text += L" (";
        text += item.title;
        text += L")";
    }
    return text.empty() ? L"None" : text;
}

// DynCapabilityNames renders KSW_CAP_* bits. Input is a 64-bit mask; output is
// the original capability name list used by DynData and DriverStatus pages.
std::wstring DynCapabilityNames(const std::uint64_t mask) {
    return NamedMaskText(mask, kDynCapabilityMasks, std::size(kDynCapabilityMasks));
}

// SecurityPolicyNames renders KSWORD_ARK_SECURITY_POLICY_* bits. Input is a
// 32-bit policy mask; output matches the original DriverStatus detail text.
std::wstring SecurityPolicyNames(const std::uint32_t mask) {
    return NamedMaskText(mask, kSecurityPolicyMasks, std::size(kSecurityPolicyMasks));
}

// DynDataStatusFlagsText renders KSW_DYN_STATUS_FLAG_* in the same English
// badge style as the full KernelDock. Input is status flags; output is "None" if empty.
std::wstring DynDataStatusFlagsText(const std::uint32_t flags) {
    std::vector<std::wstring> parts;
    if (FlagEnabled(flags, KSW_DYN_STATUS_FLAG_INITIALIZED)) { parts.push_back(L"Initialized"); }
    if (FlagEnabled(flags, KSW_DYN_STATUS_FLAG_NTOS_ACTIVE)) { parts.push_back(L"NtosActive"); }
    if (FlagEnabled(flags, KSW_DYN_STATUS_FLAG_LXCORE_ACTIVE)) { parts.push_back(L"LxcoreActive"); }
    if (FlagEnabled(flags, KSW_DYN_STATUS_FLAG_EXTRA_ACTIVE)) { parts.push_back(L"ExtraActive"); }
    if (FlagEnabled(flags, KSW_DYN_STATUS_FLAG_PDB_PROFILE_ACTIVE)) { parts.push_back(L"PdbProfileActive"); }
    if (FlagEnabled(flags, KSW_DYN_STATUS_FLAG_CALLBACK_PROFILE_ACTIVE)) { parts.push_back(L"CallbackProfileActive"); }
    std::wstring text;
    for (const std::wstring& part : parts) {
        if (!text.empty()) {
            text += L", ";
        }
        text += part;
    }
    return text.empty() ? L"None" : text;
}

// HasColumn reports whether a dynamic result column has already been added.
// Inputs are the ordered column vector and a candidate name; output avoids
// duplicate ListView columns while preserving first-seen order.
bool HasColumn(const std::vector<std::wstring>& columns, const std::wstring& name) {
    return std::find(columns.begin(), columns.end(), name) != columns.end();
}

// EmptyDetailTextForFeature returns the original KernelDock empty-detail text.
// Input is the active feature; output is shown when the table has no visible
// rows after a refresh or filter pass.
const wchar_t* EmptyDetailTextForFeature(const KernelFeatureId featureId) {
    switch (featureId) {
    case KernelFeatureId::AtomTable:
        return L"请选择一条原子记录查看详情。";
    case KernelFeatureId::NtQueryLegacy:
        return L"请选择一条 NtQuery 结果查看详情。";
    case KernelFeatureId::Ssdt:
        return L"当前环境未返回可见 SSDT 条目。";
    case KernelFeatureId::ShadowSsdt:
        return L"当前环境未返回 SSSDT stub 解析结果。";
    case KernelFeatureId::InlineHook:
        return L"当前过滤条件下未返回 Inline Hook 记录。";
    case KernelFeatureId::IatEatHook:
        return L"当前过滤条件下未返回 IAT/EAT 记录。";
    case KernelFeatureId::CallbackEnumeration:
        return L"当前环境未返回可见回调记录。";
    case KernelFeatureId::NamedPipe:
        return L"说明：命名管道属于 NPFS 文件系统目录枚举，本页使用 NtOpenFile + NtQueryDirectoryFile 读取 \\Device\\NamedPipe。\r\n这不是 NtQueryDirectoryObject 下钻，也不是系统句柄表枚举。";
    case KernelFeatureId::ObjectDirectoryRecursive:
        return L"输入根路径后点击刷新。";
    case KernelFeatureId::DynData:
        return L"请选择一条动态偏移字段查看详情。";
    case KernelFeatureId::DriverStatus:
        return L"当前筛选条件下没有驱动能力记录。";
    default:
        return L"";
    }
}

// ContainsCaseInsensitive checks whether text contains a fragment without case
// sensitivity. Inputs are display strings; output is true for empty fragments or
// any case-insensitive substring match.
bool ContainsCaseInsensitive(std::wstring text, std::wstring fragment) {
    if (fragment.empty()) {
        return true;
    }
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    std::transform(fragment.begin(), fragment.end(), fragment.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return text.find(fragment) != std::wstring::npos;
}

// ColumnWidth returns a practical default width for a kernel result column.
// Input is a column title; output is a compact but readable ListView width.
int ColumnWidth(const std::wstring& name) {
    if (name == L"#") {
        return 52;
    }
    if (name == L"字段" || name == L"功能") {
        return 260;
    }
    if (name == L"Path" || name == L"NtPath" || name == L"Target" || name == L"Detail" || name == L"Module" || name == L"TargetModule" ||
        name == L"路径/说明" || name == L"符号链接目标" || name == L"完整路径" || name == L"目标路径" ||
        name == L"targetPath" || name == L"fullPath" || name == L"dosCandidate" || name == L"Win32Path" ||
        name == L"模块" || name == L"目标模块" || name == L"原因" || name == L"导入模块") {
        if (name == L"完整路径" || name == L"目标路径") {
            return 320;
        }
        return 260;
    }
    if (name == L"服务名" || name == L"函数" || name == L"函数/序号") {
        return 240;
    }
    if (name == L"Function" || name == L"Name" || name == L"objectName" || name == L"linkName" ||
        name == L"Pipe" || name == L"Pipe Name" || name == L"Type" || name == L"objectType" || name == L"Status" ||
        name == L"函数" || name == L"名称" || name == L"对象名称" || name == L"类型" || name == L"对象类型" ||
        name == L"状态" || name == L"服务名") {
        return 180;
    }
    if (name == L"Parent" || name == L"Source" || name == L"Directory" || name == L"directoryPath" ||
        name == L"sourceDirectory" || name == L"目录路径" || name == L"来源目录") {
        return name == L"目录路径" ? 190 : 220;
    }
    if (name == L"Address" || name == L"Target" || name == L"Service" || name == L"Zw" || name == L"Callback" ||
        name == L"函数地址" || name == L"目标地址" || name == L"回调/对象地址" || name == L"表项地址" ||
        name == L"Stub地址" || name == L"Zw导出地址" || name == L"服务例程" || name == L"当前目标" ||
        name == L"期望目标" || name == L"Thunk/EAT项") {
        return 180;
    }
    if (name == L"索引") {
        return 90;
    }
    if (name == L"偏移" || name == L"类型" || name == L"类别" || name == L"状态" || name == L"策略" ||
        name == L"所需DynData" || name == L"已满足DynData") {
        return name == L"状态" ? 260 : 140;
    }
    if (name == L"能力提示") {
        return 320;
    }
    return 130;
}

// IsKernelHookFeature reports whether a feature uses the original KernelDock
// SSDT/SSSDT/Inline/IAT hook split layout. Input is a feature id; output drives
// the compact Win32 toolbar plus table/detail split.
bool IsKernelHookFeature(const KernelFeatureId featureId) {
    return featureId == KernelFeatureId::Ssdt ||
        featureId == KernelFeatureId::ShadowSsdt ||
        featureId == KernelFeatureId::InlineHook ||
        featureId == KernelFeatureId::IatEatHook;
}

// KernelHookVisibleColumnIndices returns the ListView column indices that match
// the original KernelDock SSDT/SSSDT/Inline/IAT-EAT table headers. Inputs are
// the feature id and current dynamic column schema; processing skips synthetic
// "#" and hidden diagnostic columns; output is used by Win32 copy/menu code.
std::vector<int> KernelHookVisibleColumnIndices(
    const KernelFeatureId featureId,
    const std::vector<std::wstring>& columns) {
    std::vector<int> indices;
    if (!IsKernelHookFeature(featureId)) {
        return indices;
    }
    const std::vector<std::wstring> canonical = CanonicalColumnNames(featureId);
    indices.reserve(canonical.size());
    for (const std::wstring& name : canonical) {
        for (int index = 0; index < static_cast<int>(columns.size()); ++index) {
            if (_wcsicmp(columns[static_cast<std::size_t>(index)].c_str(), name.c_str()) == 0) {
                indices.push_back(index);
                break;
            }
        }
    }
    return indices;
}

// CanonicalVisibleColumnIndices returns the original table columns for pages
// whose Win32 rows also carry hidden protocol fields. Inputs are the feature id
// and current dynamic schema; processing maps CanonicalColumnNames back to
// actual ListView indices; output is empty for pages that should copy every
// column.
std::vector<int> CanonicalVisibleColumnIndices(
    const KernelFeatureId featureId,
    const std::vector<std::wstring>& columns) {
    if (featureId != KernelFeatureId::CallbackEnumeration &&
        featureId != KernelFeatureId::DynData &&
        featureId != KernelFeatureId::DriverStatus) {
        return {};
    }

    std::vector<int> indices;
    const std::vector<std::wstring> canonical = CanonicalColumnNames(featureId);
    indices.reserve(canonical.size());
    for (const std::wstring& name : canonical) {
        for (int index = 0; index < static_cast<int>(columns.size()); ++index) {
            if (_wcsicmp(columns[static_cast<std::size_t>(index)].c_str(), name.c_str()) == 0) {
                indices.push_back(index);
                break;
            }
        }
    }
    return indices;
}

// PreferredCopyColumnIndices chooses the column set that should participate in
// row/header copy operations. Inputs are the active feature and current schema;
// output preserves the original KernelDock visible table contract while hidden
// Win32 protocol fields remain available for context menu actions.
std::vector<int> PreferredCopyColumnIndices(
    const KernelFeatureId featureId,
    const std::vector<std::wstring>& columns) {
    std::vector<int> indices = KernelHookVisibleColumnIndices(featureId, columns);
    if (!indices.empty()) {
        return indices;
    }
    return CanonicalVisibleColumnIndices(featureId, columns);
}

// MergeRowText concatenates all non-empty fields of one cached result row.
// Inputs are row cells; processing joins values with a stable separator; output
// is used by local filter predicates without changing the backing result data.
std::wstring MergeRowText(const std::vector<std::wstring>& row) {
    std::wstring merged;
    for (const std::wstring& value : row) {
        if (value.empty()) {
            continue;
        }
        if (!merged.empty()) {
            merged += L" | ";
        }
        merged += value;
    }
    return merged;
}

// LowerInvariantKey normalizes object-manager paths for TreeView lookup. Inputs
// are display strings from query rows; processing lowercases only for map keys
// and leaves the original UI text untouched; output is empty when no key exists.
std::wstring LowerInvariantKey(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return text;
}

// ParentObjectPath derives a native parent path from a full object path when a
// row lacks an explicit Parent field. Input is a path like "\Device\Foo";
// output is "\Device", "\" for direct root children, or empty for invalid text.
std::wstring ParentObjectPath(const std::wstring& path) {
    if (path.empty() || path == L"\\") {
        return {};
    }
    const std::size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos) {
        return {};
    }
    if (slash == 0) {
        return L"\\";
    }
    return path.substr(0, slash);
}

// LeafObjectName returns the display leaf for synthetic TreeView directory
// nodes. Input is a native object path; output is "\" for the root, the last
// component for normal paths, or the original text when no separator exists.
std::wstring LeafObjectName(const std::wstring& path) {
    if (path.empty() || path == L"\\") {
        return L"\\";
    }
    const std::size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos || slash + 1 >= path.size()) {
        return path;
    }
    return path.substr(slash + 1);
}

// AppendReportLine appends one diagnostic key/value pair. Inputs are a stream,
// a label and a value; processing skips empty values so copied reports stay
// compact like the original diagnostic reports; there is no return value.
void AppendReportLine(std::wostringstream& report, const wchar_t* label, const std::wstring& value) {
    if (!value.empty()) {
        report << label << L": " << value << L"\r\n";
    }
}

// ParseUnsigned64Value converts decimal or 0x-prefixed text into a 64-bit value.
// Inputs are an edit-control string and an output reference; processing trims
// common whitespace and accepts the same address forms used by the original
// MemoryDock pages; output is true only when the whole token parsed.
bool ParseUnsigned64Value(const std::wstring& text, std::uint64_t& valueOut) {
    std::wstring trimmed = text;
    trimmed.erase(trimmed.begin(), std::find_if(trimmed.begin(), trimmed.end(), [](wchar_t ch) {
        return !std::iswspace(ch);
    }));
    trimmed.erase(std::find_if(trimmed.rbegin(), trimmed.rend(), [](wchar_t ch) {
        return !std::iswspace(ch);
    }).base(), trimmed.end());
    if (trimmed.empty()) {
        return false;
    }
    wchar_t* end = nullptr;
    const int base = trimmed.size() > 2 && trimmed[0] == L'0' && (trimmed[1] == L'x' || trimmed[1] == L'X') ? 16 : 10;
    const unsigned long long value = std::wcstoull(trimmed.c_str(), &end, base);
    if (end == trimmed.c_str() || *end != L'\0') {
        return false;
    }
    valueOut = static_cast<std::uint64_t>(value);
    return true;
}

// IsKernelMemoryHiddenColumn 与 IsIntegrityHiddenColumn 仍然由
// KernelProjectionValues.inl 的宽度计算使用：那里给证据列算 0 宽度。表格是否
// 真的建这一列已由 KernelColumnVisibility.h 统一决定，这两个函数现在只管宽度。
bool IsKernelMemoryHiddenColumn(const std::wstring& name) {
    return IsKernelEvidenceColumn(name);
}

bool IsIntegrityHiddenColumn(const std::wstring& name) {
    return IsKernelEvidenceColumn(name);
}

std::wstring KernelTreeNodeKey(const KernelObjectNamespaceTreeNodeState& state) {
    return LowerInvariantKey(state.nodeKind+L"\n"+state.nodeType+L"\n"+state.nodePath);
}

} // namespace Ksword::Features::Kernel
