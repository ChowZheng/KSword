#pragma once

#include "KernelModel.h"

#include "../../../shared/driver/KswordArkCapabilityIoctl.h"
#include "../../../shared/driver/KswordArkDynDataIoctl.h"

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace Ksword::Features::Kernel {

struct KernelObjectNamespaceTreeNodeState;

// KernelRawRows is the shared shape of a cached kernel result page. Inputs are
// the display strings emitted by KernelFacade; processing keeps the row order
// aligned with the column schema; no ownership is implied by the alias.
using KernelRawRows = std::vector<std::vector<std::wstring>>;

// EmptyKernelRawRows returns the shared empty row cache. There is no input;
// processing reuses one static instance so retired projections can fall back to
// a non-null cache; output is never null.
std::shared_ptr<const KernelRawRows> EmptyKernelRawRows();

// BoolText converts metadata or result booleans into compact Chinese labels.
// Input is a boolean value; output is "是" or "否".
const wchar_t* BoolText(const bool value);

// HexToUInt64 parses decimal or 0x-prefixed protocol numbers from cached rows.
// Input is display text emitted by KernelFacade; processing uses wcstoull and
// requires full-token consumption; output is false when the cell is missing or
// not numeric.
bool HexToUInt64(const std::wstring& text, std::uint64_t& valueOut);

// RowFieldByName reads a named value from a cached dynamic row. Inputs are a row,
// the current column names and one or more accepted column aliases; processing is
// case-insensitive and preserves the original display value; output is empty when
// no alias is present or the selected cell is blank.
std::wstring RowFieldByName(const std::vector<std::wstring>& row,
    const std::vector<std::wstring>& columns,
    std::initializer_list<const wchar_t*> names);

// FirstRowValue returns the first non-empty field with one of the supplied
// aliases. Inputs are a row cache and column aliases; output is empty when no
// matching cell is present.
std::wstring FirstRowValue(
    const std::vector<std::vector<std::wstring>>& rows,
    const std::vector<std::wstring>& columns,
    std::initializer_list<const wchar_t*> names);

// FirstRowUInt64 returns a numeric value from the first matching cached row.
// Inputs are row data, columns and aliases; processing accepts hexadecimal and
// decimal display forms; output uses fallback when no numeric value is found.
std::uint64_t FirstRowUInt64(
    const std::vector<std::vector<std::wstring>>& rows,
    const std::vector<std::wstring>& columns,
    std::initializer_list<const wchar_t*> names,
    const std::uint64_t fallback = 0);

// FlagEnabled checks whether a numeric bitmask contains a requested flag. Inputs
// are protocol flags and a flag value; output is true when all flag bits are set.
bool FlagEnabled(const std::uint64_t flags, const std::uint64_t flag);

// IsZeroMaskText identifies empty dependency masks in their common display
// forms. Input is a cell string; output is true for blank, decimal zero, or
// hexadecimal zero variants.
bool IsZeroMaskText(const std::wstring& text);

// NamedMask describes one protocol bit with its C macro name and original
// KernelDock Chinese title. Inputs are static constants; processing helpers use
// it to render bitsets; no runtime ownership is stored here.
struct NamedMask {
    std::uint64_t mask;
    const wchar_t* name;
    const wchar_t* title;
};

inline constexpr NamedMask kDynCapabilityMasks[] = {
    { KSW_CAP_DYN_NTOS_ACTIVE, L"KSW_CAP_DYN_NTOS_ACTIVE", L"ntoskrnl profile 已激活" },
    { KSW_CAP_DYN_LXCORE_ACTIVE, L"KSW_CAP_DYN_LXCORE_ACTIVE", L"lxcore profile 已激活" },
    { KSW_CAP_OBJECT_TYPE_FIELDS, L"KSW_CAP_OBJECT_TYPE_FIELDS", L"对象类型字段" },
    { KSW_CAP_HANDLE_TABLE_DECODE, L"KSW_CAP_HANDLE_TABLE_DECODE", L"句柄表解码" },
    { KSW_CAP_PROCESS_OBJECT_TABLE, L"KSW_CAP_PROCESS_OBJECT_TABLE", L"进程 ObjectTable" },
    { KSW_CAP_THREAD_STACK_FIELDS, L"KSW_CAP_THREAD_STACK_FIELDS", L"线程栈字段" },
    { KSW_CAP_THREAD_IO_COUNTERS, L"KSW_CAP_THREAD_IO_COUNTERS", L"线程 I/O 计数" },
    { KSW_CAP_ALPC_FIELDS, L"KSW_CAP_ALPC_FIELDS", L"ALPC 字段" },
    { KSW_CAP_SECTION_CONTROL_AREA, L"KSW_CAP_SECTION_CONTROL_AREA", L"Section/ControlArea" },
    { KSW_CAP_PROCESS_PROTECTION_PATCH, L"KSW_CAP_PROCESS_PROTECTION_PATCH", L"进程保护修改" },
    { KSW_CAP_WSL_LXCORE_FIELDS, L"KSW_CAP_WSL_LXCORE_FIELDS", L"WSL/lxcore 字段" },
    { KSW_CAP_ETW_GUID_FIELDS, L"KSW_CAP_ETW_GUID_FIELDS", L"ETW GUID/Registration 字段" },
    { KSW_CAP_CALLBACK_NOTIFY_GLOBALS, L"KSW_CAP_CALLBACK_NOTIFY_GLOBALS", L"Callback Notify 全局 RVA" },
    { KSW_CAP_CALLBACK_REGISTRY_GLOBALS, L"KSW_CAP_CALLBACK_REGISTRY_GLOBALS", L"Registry Callback 全局 RVA" },
    { KSW_CAP_CALLBACK_OBJECT_FIELDS, L"KSW_CAP_CALLBACK_OBJECT_FIELDS", L"Object Callback 结构偏移" },
    { KSW_CAP_PROCESS_LIST_FIELDS, L"KSW_CAP_PROCESS_LIST_FIELDS", L"进程链表字段" },
    { KSW_CAP_THREAD_LIST_FIELDS, L"KSW_CAP_THREAD_LIST_FIELDS", L"线程链表字段" },
    { KSW_CAP_CID_TABLE_WALK, L"KSW_CAP_CID_TABLE_WALK", L"CID 表遍历" },
    { KSW_CAP_KERNEL_MODULE_LIST_FIELDS, L"KSW_CAP_KERNEL_MODULE_LIST_FIELDS", L"内核模块链表字段" },
    { KSW_CAP_DRIVER_OBJECT_FIELDS, L"KSW_CAP_DRIVER_OBJECT_FIELDS", L"驱动对象字段" },
    { KSW_CAP_KERNEL_GLOBALS, L"KSW_CAP_KERNEL_GLOBALS", L"内核全局 RVA" },
};

inline constexpr NamedMask kSecurityPolicyMasks[] = {
    { KSWORD_ARK_SECURITY_POLICY_FLAG_ACTIVE, L"POLICY_ACTIVE", L"安全策略启用" },
    { KSWORD_ARK_SECURITY_POLICY_ALLOW_MUTATING_ACTIONS, L"ALLOW_MUTATING_ACTIONS", L"允许进程修改动作" },
    { KSWORD_ARK_SECURITY_POLICY_ALLOW_CALLBACK_CONTROL, L"ALLOW_CALLBACK_CONTROL", L"允许回调控制" },
    { KSWORD_ARK_SECURITY_POLICY_ALLOW_PROCESS_PROTECTION, L"ALLOW_PROCESS_PROTECTION", L"允许进程保护修改" },
    { KSWORD_ARK_SECURITY_POLICY_ALLOW_KERNEL_SNAPSHOTS, L"ALLOW_KERNEL_SNAPSHOTS", L"允许内核快照" },
    { KSWORD_ARK_SECURITY_POLICY_REQUIRE_CONFIRMATION, L"REQUIRE_CONFIRMATION", L"要求确认" },
    { KSWORD_ARK_SECURITY_POLICY_DENY_CRITICAL_PROCESS, L"DENY_CRITICAL_PROCESS", L"拒绝关键进程" },
    { KSWORD_ARK_SECURITY_POLICY_ADVANCED_MODE, L"ADVANCED_MODE", L"高级模式" },
};

// NamedMaskText renders a protocol bitset as "MACRO (中文)" entries. Inputs are
// a mask and table; processing preserves original KernelDock names; output is
// "None" when no known bit is enabled.
std::wstring NamedMaskText(const std::uint64_t mask, const NamedMask* items, const std::size_t count);

// DynCapabilityNames renders KSW_CAP_* bits. Input is a 64-bit mask; output is
// the original capability name list used by DynData and DriverStatus pages.
std::wstring DynCapabilityNames(const std::uint64_t mask);

// SecurityPolicyNames renders KSWORD_ARK_SECURITY_POLICY_* bits. Input is a
// 32-bit policy mask; output matches the original DriverStatus detail text.
std::wstring SecurityPolicyNames(const std::uint32_t mask);

// DynDataStatusFlagsText renders KSW_DYN_STATUS_FLAG_* in the same English
// badge style as the full KernelDock. Input is status flags; output is "None" if empty.
std::wstring DynDataStatusFlagsText(const std::uint32_t flags);

// HasColumn reports whether a dynamic result column has already been added.
// Inputs are the ordered column vector and a candidate name; output avoids
// duplicate ListView columns while preserving first-seen order.
bool HasColumn(const std::vector<std::wstring>& columns, const std::wstring& name);

// EmptyDetailTextForFeature returns the original KernelDock empty-detail text.
// Input is the active feature; output is shown when the table has no visible
// rows after a refresh or filter pass.
const wchar_t* EmptyDetailTextForFeature(const KernelFeatureId featureId);

// ContainsCaseInsensitive checks whether text contains a fragment without case
// sensitivity. Inputs are display strings; output is true for empty fragments or
// any case-insensitive substring match.
bool ContainsCaseInsensitive(std::wstring text, std::wstring fragment);

// ColumnWidth returns a practical default width for a kernel result column.
// Input is a column title; output is a compact but readable ListView width.
int ColumnWidth(const std::wstring& name);

// IsKernelHookFeature reports whether a feature uses the original KernelDock
// SSDT/SSSDT/Inline/IAT hook split layout. Input is a feature id; output drives
// the compact Win32 toolbar plus table/detail split.
bool IsKernelHookFeature(const KernelFeatureId featureId);

// KernelHookVisibleColumnIndices returns the ListView column indices that match
// the original KernelDock SSDT/SSSDT/Inline/IAT-EAT table headers. Inputs are
// the feature id and current dynamic column schema; processing skips synthetic
// "#" and hidden diagnostic columns; output is used by Win32 copy/menu code.
std::vector<int> KernelHookVisibleColumnIndices(
    const KernelFeatureId featureId,
    const std::vector<std::wstring>& columns);

// CanonicalVisibleColumnIndices returns the original table columns for pages
// whose Win32 rows also carry hidden protocol fields. Inputs are the feature id
// and current dynamic schema; processing maps CanonicalColumnNames back to
// actual ListView indices; output is empty for pages that should copy every
// column.
std::vector<int> CanonicalVisibleColumnIndices(
    const KernelFeatureId featureId,
    const std::vector<std::wstring>& columns);

// PreferredCopyColumnIndices chooses the column set that should participate in
// row/header copy operations. Inputs are the active feature and current schema;
// output preserves the original KernelDock visible table contract while hidden
// Win32 protocol fields remain available for context menu actions.
std::vector<int> PreferredCopyColumnIndices(
    const KernelFeatureId featureId,
    const std::vector<std::wstring>& columns);

// MergeRowText concatenates all non-empty fields of one cached result row.
// Inputs are row cells; processing joins values with a stable separator; output
// is used by local filter predicates without changing the backing result data.
std::wstring MergeRowText(const std::vector<std::wstring>& row);

// LowerInvariantKey normalizes object-manager paths for TreeView lookup. Inputs
// are display strings from query rows; processing lowercases only for map keys
// and leaves the original UI text untouched; output is empty when no key exists.
std::wstring LowerInvariantKey(std::wstring text);

// ParentObjectPath derives a native parent path from a full object path when a
// row lacks an explicit Parent field. Input is a path like "\Device\Foo";
// output is "\Device", "\" for direct root children, or empty for invalid text.
std::wstring ParentObjectPath(const std::wstring& path);

// LeafObjectName returns the display leaf for synthetic TreeView directory
// nodes. Input is a native object path; output is "\" for the root, the last
// component for normal paths, or the original text when no separator exists.
std::wstring LeafObjectName(const std::wstring& path);

// AppendReportLine appends one diagnostic key/value pair. Inputs are a stream,
// a label and a value; processing skips empty values so copied reports stay
// compact like the original diagnostic reports; there is no return value.
void AppendReportLine(std::wostringstream& report, const wchar_t* label, const std::wstring& value);

// ParseUnsigned64Value converts decimal or 0x-prefixed text into a 64-bit value.
// Inputs are an edit-control string and an output reference; processing trims
// common whitespace and accepts the same address forms used by the original
// MemoryDock pages; output is true only when the whole token parsed.
bool ParseUnsigned64Value(const std::wstring& text, std::uint64_t& valueOut);

// IsKernelMemoryHiddenColumn 与 IsIntegrityHiddenColumn 仍然由
// KernelProjectionBuilder 的宽度计算使用：那里给证据列算 0 宽度。表格是否
// 真的建这一列已由 KernelColumnVisibility.h 统一决定，这两个函数现在只管宽度。
bool IsKernelMemoryHiddenColumn(const std::wstring& name);

bool IsIntegrityHiddenColumn(const std::wstring& name);

// KernelTreeNodeKey builds the stable lower-case TreeView node key. Input is the
// node payload stored behind TVITEM::lParam; output joins kind/type/path so the
// tree can locate an existing node after a rebuild.
std::wstring KernelTreeNodeKey(const KernelObjectNamespaceTreeNodeState& state);

// kDeviceDriverFixedDirectories holds the object-manager directories the driver
// page always lists, even when the live enumeration returns nothing. Shared by
// the projection builder's object-namespace reshaping.
inline constexpr const wchar_t* kDeviceDriverFixedDirectories[] = {
    L"\\Device",
    L"\\Driver",
    L"\\FileSystem",
    L"\\FileSystem\\Filters",
};

} // namespace Ksword::Features::Kernel
