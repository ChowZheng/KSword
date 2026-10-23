#pragma once

#include "KernelPageLayout.h"

#include <string>
#include <vector>

namespace Ksword::Features::Kernel {

// 内核结果表的列分成两类：
//   Table   —— 进表格，用户扫一眼就要拿它做决定（对象名、状态、地址、模块、风险）
//   Evidence—— 只对取证有意义（原始键位、掩码、内部编号、二次派生的路径副本），
//              留在详情面板/复制/导出里，不占表格一列
//
// 这张名单以前有四份几乎相同的副本：KernelPage.cpp 的两个隐藏 lambda、
// KernelProjectionValues.inl 的 addHiddenColumn 系列和一处内联判定。副本之间
// 会漂移——同一页面经"首次投影"和"缓存重建"两条路径进来能显示不同列数。
// 现在只保留这一份。
inline const wchar_t* const kKernelEvidenceColumns[] = {
    // 对象命名空间里被树/路径列重复表达的原始键
    L"Parent", L"Source", L"Directory", L"directoryPath", L"sourceDirectory",
    L"Depth", L"Path", L"NtPath", L"Target", L"Win32Path", L"dosCandidate",
    L"fullPath", L"targetPath", L"symbolicTarget", L"objectName", L"objectType",
    L"linkName", L"Handles", L"Pointers", L"NodeKind",
    L"EnumApi", L"enumApi", L"EnumerationApi", L"枚举 API",
    // 诊断/状态页的原始枚举与内部编号
    L"Id", L"StateId", L"SourceIndex", L"StatusFlags", L"CapabilityMask",
    L"Version", L"Protocol", L"ExpectedProtocol", L"SecurityPolicy",
    L"DynDataStatus", L"DynDataCapability", L"FeatureTotal", L"FeatureReturned",
    L"LastError", L"LastErrorSummary", L"LastErrorStatus", L"Class",
    L"Callback", L"Context", L"Registration", L"ModulePath", L"Win32ModulePath",
    L"ModuleBase", L"ModuleSize", L"OperationMask", L"ObjectTypeMask",
    L"FieldFlags", L"Trust", L"Remove", L"RemoveFlags", L"Generation",
    L"IdentityHash", L"RawStorageValue", L"LastStatus",
    // 内存/完整性页的 R0 取证字段
    L"RegionSize", L"Perm", L"Risk", L"OwnerKind", L"OwnerKindText",
    L"OwnerAddress", L"ModuleSizeText", L"Status", L"Kind", L"PageSize",
    L"Confidence", L"BigPoolTag", L"BigPoolFlags", L"SectionRva", L"SectionSize",
    L"SectionSizeText", L"Section", L"HashAlgorithm", L"SampleSize", L"Hash",
    L"HashText", L"Sample", L"ClassText", L"RiskText", L"SourceMask",
    L"SourceText", L"Group", L"CPU", L"Vector", L"CpuVector", L"Object",
    L"ObjectAddress", L"TargetAddress", L"OwnerBase", L"OwnerModuleBase",
    L"OwnerSize", L"OwnerModuleSize", L"OwnerModuleSizeText", L"OwnerModule",
    // 合成的行号/详情列
    L"Detail",
};

// IsKernelEvidenceColumn 判断列名是否属于"只该出现在证据里"的原始键。
// 入参是列名；输出 true 表示该列不进表格。
inline bool IsKernelEvidenceColumn(const std::wstring& name) {
    for (const wchar_t* candidate : kKernelEvidenceColumns) {
        if (::_wcsicmp(candidate, name.c_str()) == 0) {
            return true;
        }
    }
    return false;
}

// IsKernelTableColumn 决定一列要不要建成 ListView 列。入参是功能 id 与列名；
// 输出 true 表示进表格。规则只有一条：有手写 canonical 表的功能，表格就是那张表；
// 没写的功能（纯动态 R0 页）才用证据名单兜底。这样列数不会随 facade 多吐一个键
// 就膨胀一轮。
inline bool IsKernelTableColumn(const KernelFeatureId featureId, const std::wstring& name) {
    if (name.empty()) {
        return false;
    }
    if (name == L"#") {
        return true;
    }
    const std::vector<std::wstring> canonical = CanonicalColumnNames(featureId);
    if (canonical.empty()) {
        return !IsKernelEvidenceColumn(name);
    }
    for (const std::wstring& column : canonical) {
        if (::_wcsicmp(column.c_str(), name.c_str()) == 0) {
            return true;
        }
    }
    return false;
}

} // namespace Ksword::Features::Kernel
