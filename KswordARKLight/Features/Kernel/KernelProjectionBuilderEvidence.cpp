#include "KernelProjectionBuilder.h"

#include "../../../shared/driver/KswordArkCallbackIoctl.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace Ksword::Features::Kernel {

void KernelValueProjectionBuilder::RebuildIntegrityEvidenceListFromCache(const KernelFeatureId featureId) {
    // RebuildIntegrityEvidenceListFromCache mirrors the original DriverDock and
    // HardwareDock integrity evidence tables. Inputs are raw R0 rows cached by
    // RenderResult and the local filter edit; processing projects the original
    // visible columns while retaining raw protocol fields for detail/actions;
    // output is the updated ListView, status text, and detail pane selection.
    if (!resultList_) {
        return;
    }

    const std::vector<std::wstring>& rawColumns = !currentRawColumns_.empty() ? currentRawColumns_ : currentColumns_;
    const std::vector<std::vector<std::wstring>>& rawRows = !currentRawRows_->empty() ? *currentRawRows_ : currentRows_;
    const std::wstring filterText = Text(filterEdit_);
    const bool cpuPage = featureId == KernelFeatureId::KernelCpuIntegrity;
    const auto rawValue = [&](const std::vector<std::wstring>& row, std::initializer_list<const wchar_t*> names) -> std::wstring {
        return ReadField(row, rawColumns, names);
    };
    const auto addColumnIfMissing = [](std::vector<std::wstring>& columns, const wchar_t* name) {
        if (!HasColumn(columns, name)) {
            columns.push_back(name);
        }
    };
    const auto displayValue = [&](const std::vector<std::wstring>& row, const std::wstring& columnName) -> std::wstring {
        const auto& aliases = Aliases(featureId, columnName);
        for (const std::wstring& alias : aliases) {
            const std::wstring value = rawValue(row, { alias.c_str() });
            if (!value.empty()) {
                return value;
            }
        }
        return {};
    };

    std::vector<std::wstring> displayColumns;
    displayColumns.push_back(L"#");
    for (const std::wstring& column : CanonicalColumnNames(featureId)) {
        addColumnIfMissing(displayColumns, column.c_str());
    }
    const wchar_t* hiddenColumns[] = {
        L"Class", L"ClassText", L"Risk", L"RiskText", L"Source", L"SourceMask", L"SourceText",
        L"Confidence", L"Group", L"CPU", L"Vector", L"CpuVector", L"Object", L"ObjectAddress",
        L"Target", L"TargetAddress", L"OwnerBase", L"OwnerModuleBase", L"OwnerSize",
        L"OwnerModuleSize", L"OwnerModuleSizeText", L"OwnerModule", L"LastStatus"
    };
    for (const wchar_t* column : hiddenColumns) {
        addColumnIfMissing(displayColumns, column);
    }

    std::vector<std::vector<std::wstring>> displayRows;
    std::size_t reportedTotalRows = 0;
    std::size_t reportedReturnedRows = 0;
    std::size_t reportedCpuRows = 0;
    std::size_t reportedModuleRows = 0;
    std::size_t totalRows = 0;
    std::size_t riskRows = 0;
    std::wstring r0State;
    std::wstring r0Protocol;
    std::wstring dynDataState;
    std::wstring queryStatus;
    std::wstring sourceText;
    for (const std::vector<std::wstring>& rawRow : rawRows) {
        const std::uint64_t total = ReadNonZero(rawRow, rawColumns, { L"Total" });
        const std::uint64_t returned = ReadNonZero(rawRow, rawColumns, { L"Returned" });
        const std::uint64_t cpuCount = ReadNonZero(rawRow, rawColumns, { L"CpuCount" });
        const std::uint64_t moduleCount = ReadNonZero(rawRow, rawColumns, { L"ModuleCount" });
        if (total != 0) {
            reportedTotalRows = static_cast<std::size_t>(total);
        }
        if (returned != 0) {
            reportedReturnedRows = static_cast<std::size_t>(returned);
        }
        if (cpuCount != 0) {
            reportedCpuRows = static_cast<std::size_t>(cpuCount);
        }
        if (moduleCount != 0) {
            reportedModuleRows = static_cast<std::size_t>(moduleCount);
        }
        if (r0State.empty()) {
            r0State = ReadField(rawRow, rawColumns, { L"R0" });
        }
        if (r0Protocol.empty()) {
            r0Protocol = ReadField(rawRow, rawColumns, { L"Protocol" });
        }
        if (dynDataState.empty()) {
            dynDataState = ReadField(rawRow, rawColumns, { L"DynDataStatus" });
        }
        if (queryStatus.empty()) {
            queryStatus = ReadField(rawRow, rawColumns, { L"Status" });
        }
        if (sourceText.empty()) {
            sourceText = ReadField(rawRow, rawColumns, { L"SourceText", L"SourceMask" });
        }
    }
    for (const std::vector<std::wstring>& rawRow : rawRows) {
        const bool evidenceRow = HasValue(rawRow, rawColumns, {
            L"类别", L"Class", L"ClassText", L"对象", L"ObjectAddress", L"目标", L"TargetAddress",
            L"Owner", L"OwnerModule", L"CPU/Vector", L"CpuVector", L"风险", L"RiskText"
        });
        if (!evidenceRow) {
            continue;
        }
        ++totalRows;
        const std::wstring riskText = rawValue(rawRow, { L"风险", L"RiskText", L"Risk" });
        const std::wstring riskMask = rawValue(rawRow, { L"Risk" });
        const bool risky = !riskText.empty() &&
            !ContainsCaseInsensitive(riskText, L"正常") &&
            !ContainsCaseInsensitive(riskMask, L"0x0") &&
            riskMask != L"0";
        if (risky) {
            ++riskRows;
        }
        const bool supportsRiskOnly = featureId == KernelFeatureId::MutationAudit;
        if (supportsRiskOnly && riskOnlyCheck_ && Checked(riskOnlyCheck_) == BST_CHECKED && !risky) {
            continue;
        }
        const std::wstring merged = MergeRowText(rawRow);
        if (!filterText.empty() && !ContainsCaseInsensitive(merged, filterText)) {
            continue;
        }

        std::vector<std::wstring> cells(displayColumns.size());
        for (std::size_t column = 0; column < displayColumns.size(); ++column) {
            const std::wstring& name = displayColumns[column];
            if (name == L"#") {
                cells[column] = std::to_wstring(displayRows.size());
            } else if (name == L"Detail") {
                cells[column] = rawValue(rawRow, { L"Detail" });
            } else {
                cells[column] = displayValue(rawRow, name);
            }
        }
        displayRows.push_back(std::move(cells));
    }

    currentColumns_ = std::move(displayColumns);
    currentRows_ = std::move(displayRows);
    ClearTables();
    for (std::size_t index = 0; index < currentColumns_.size(); ++index) {
        const std::wstring& name = currentColumns_[index];
        const bool canonicalColumn = HasColumn(CanonicalColumnNames(featureId), name);
        int width = (!canonicalColumn && IsIntegrityHiddenColumn(name)) ? 0 : ColumnWidth(name);
        if (name == L"类别") {
            width = 150;
        } else if (name == L"对象" || name == L"目标" || name == L"对象/寄存器" || name == L"目标/入口") {
            width = 170;
        } else if (name == L"Owner" || name == L"Owner模块") {
            width = 240;
        } else if (name == L"CPU/Vector") {
            width = 110;
        } else if (name == L"风险") {
            width = 220;
        } else if (name == L"置信度") {
            width = 72;
        } else if (name == L"Detail") {
            width = 360;
        }
        SetColumn(static_cast<int>(index), name, width);
    }
    CommitRows();

    std::wostringstream status;
    const std::size_t effectiveTotal = reportedTotalRows == 0 ? totalRows : reportedTotalRows;
    const std::size_t effectiveReturned = reportedReturnedRows == 0 ? totalRows : reportedReturnedRows;
    status << L"状态：" << (cpuPage ? L"CPU/IDT 完整性" : L"驱动完整性")
           << L"已刷新，显示 " << currentRows_.size()
           << L"，返回 " << effectiveReturned << L"/" << effectiveTotal
           << L"，风险 " << riskRows;
    if (!queryStatus.empty()) {
        status << L"，Query " << queryStatus;
    }
    if (!sourceText.empty()) {
        status << L"，Source " << sourceText;
    }
    if (reportedCpuRows != 0 || reportedModuleRows != 0) {
        status << L"，CPU " << reportedCpuRows << L"，模块 " << reportedModuleRows;
    }
    if (cpuPage && (!r0State.empty() || !r0Protocol.empty() || !dynDataState.empty())) {
        status << L"，R0 " << (r0State.empty() ? L"<unknown>" : r0State)
               << L"，Protocol " << (r0Protocol.empty() ? L"-" : r0Protocol)
               << L"，DynData " << (dynDataState.empty() ? L"-" : dynDataState);
    }
    WriteText(statusText_, status.str().c_str());
    if (!currentRows_.empty()) {
        SetSelection(resultList_, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        EnsureItem(resultList_, 0, FALSE);
        SelectionDetail();
    } else {
        WriteText(detailEdit_, cpuPage
            ? L"当前过滤条件下没有 CPU/IDT 完整性证据。"
            : L"当前过滤条件下没有驱动完整性证据。");
    }
}

void KernelValueProjectionBuilder::RebuildCallbackEnumerationListFromCache() {
    // RebuildCallbackEnumerationListFromCache mirrors the original
    // KernelDock.CallbackEnum table. Inputs are raw rows from
    // ArkDriverClient::enumerateCallbacks; processing keeps the exact visible
    // registration subtypes and policy glyphs, preserves hidden protocol fields for remove actions, and
    // applies the local filter edit; output updates resultList_ and detailEdit_.
    if (!resultList_) {
        return;
    }
    const std::vector<std::wstring>& rawColumns = !currentRawColumns_.empty() ? currentRawColumns_ : currentColumns_;
    const std::vector<std::vector<std::wstring>>& rawRows = !currentRawRows_->empty() ? *currentRawRows_ : currentRows_;
    const std::wstring filterText = Text(filterEdit_);
    const auto rawValue = [&](const std::vector<std::wstring>& row, std::initializer_list<const wchar_t*> names) -> std::wstring {
        return ReadField(row, rawColumns, names);
    };
    const auto addColumnIfMissing = [](std::vector<std::wstring>& columns, const wchar_t* name) {
        if (!HasColumn(columns, name)) {
            columns.push_back(name);
        }
    };
    const auto displayValue = [&](const std::vector<std::wstring>& row, const std::wstring& columnName) -> std::wstring {
        const auto& aliases = Aliases(KernelFeatureId::CallbackEnumeration, columnName);
        for (const std::wstring& alias : aliases) {
            const std::wstring value = rawValue(row, { alias.c_str() });
            if (!value.empty()) {
                return value;
            }
        }
        return {};
    };

    std::vector<std::wstring> displayColumns;
    for (const std::wstring& column : CanonicalColumnNames(KernelFeatureId::CallbackEnumeration)) {
        addColumnIfMissing(displayColumns, column.c_str());
    }
    addColumnIfMissing(displayColumns, L"SourceIndex");
    addColumnIfMissing(displayColumns, L"Class");
    addColumnIfMissing(displayColumns, L"StatusCode");
    addColumnIfMissing(displayColumns, L"RegistrationType");
    addColumnIfMissing(displayColumns, L"RemovePolicy");
    addColumnIfMissing(displayColumns, L"Source");
    addColumnIfMissing(displayColumns, L"Callback");
    addColumnIfMissing(displayColumns, L"Context");
    addColumnIfMissing(displayColumns, L"Registration");
    addColumnIfMissing(displayColumns, L"ModulePath");
    addColumnIfMissing(displayColumns, L"Win32ModulePath");
    addColumnIfMissing(displayColumns, L"ModuleBase");
    addColumnIfMissing(displayColumns, L"ModuleSize");
    addColumnIfMissing(displayColumns, L"OperationMask");
    addColumnIfMissing(displayColumns, L"ObjectTypeMask");
    addColumnIfMissing(displayColumns, L"FieldFlags");
    addColumnIfMissing(displayColumns, L"Trust");
    addColumnIfMissing(displayColumns, L"Remove");
    addColumnIfMissing(displayColumns, L"RemoveFlags");
    addColumnIfMissing(displayColumns, L"Generation");
    addColumnIfMissing(displayColumns, L"IdentityHash");
    addColumnIfMissing(displayColumns, L"RawStorageValue");
    addColumnIfMissing(displayColumns, L"LastStatus");
    addColumnIfMissing(displayColumns, L"Detail");

    std::vector<std::vector<std::wstring>> displayRows;
    std::size_t unsupportedCount = 0;
    std::size_t totalCallbackRows = 0;
    bool responseTruncated = false;
    for (const std::vector<std::wstring>& rawRow : rawRows) {
        std::uint64_t flagsValue = 0;
        if (ParseUnsigned64Value(rawValue(rawRow, { L"CallbackEnumResponseFlags", L"ResponseFlags", L"Flags" }), flagsValue) &&
            FlagEnabled(flagsValue, KSWORD_ARK_ENUM_CALLBACK_RESPONSE_FLAG_TRUNCATED)) {
            responseTruncated = true;
        }
        if (rawValue(rawRow, { L"CallbackEnumTruncated" }) == L"1") {
            responseTruncated = true;
        }
    }
    for (std::size_t sourceIndex = 0; sourceIndex < rawRows.size(); ++sourceIndex) {
        const std::vector<std::wstring>& rawRow = rawRows[sourceIndex];
        const bool meaningful = HasValue(rawRow, rawColumns, {
            L"ClassText", L"Class", L"SourceText", L"Source", L"Callback", L"Object", L"Registration", L"RawStorageValue", L"Name", L"ModulePath"
        });
        if (!meaningful) {
            continue;
        }
        ++totalCallbackRows;
        const std::wstring status = rawValue(rawRow, { L"Status", L"状态" });
        if (ContainsCaseInsensitive(status, L"unsupported") || ContainsCaseInsensitive(status, L"未支持")) {
            ++unsupportedCount;
        }
        const std::wstring merged = MergeRowText(rawRow);
        if (!filterText.empty() && !ContainsCaseInsensitive(merged, filterText)) {
            continue;
        }

        std::vector<std::wstring> cells(displayColumns.size());
        for (std::size_t column = 0; column < displayColumns.size(); ++column) {
            const std::wstring& name = displayColumns[column];
            if (name == L"SourceIndex") {
                cells[column] = std::to_wstring(sourceIndex);
            } else if (name == L"回调/对象地址") {
                std::wstring value = rawValue(rawRow, { L"Callback" });
                if (value.empty() || value == L"0" || value == L"0x0" || value == L"0x0000000000000000") {
                    value = rawValue(rawRow, { L"Object", L"Registration", L"RawStorageValue", L"Context" });
                }
                cells[column] = std::move(value);
            } else if (name == L"模块") {
                std::wstring module = rawValue(rawRow, { L"ModulePath", L"Module" });
                if (module.empty()) {
                    module = L"<未解析>";
                }
                cells[column] = std::move(module);
            } else if (name == L"名称") {
                std::wstring value = rawValue(rawRow, { L"Name", L"名称" });
                if (value.empty()) {
                    value = L"<无名称>";
                }
                cells[column] = std::move(value);
            } else if (name == L"Win32ModulePath") {
                cells[column] = NormalizeCallbackModulePath(rawValue(rawRow, { L"ModulePath", L"Module" }));
            } else if (name == L"Detail") {
                cells[column] = rawValue(rawRow, { L"Detail", L"FieldText", L"TrustText", L"RemoveText" });
            } else {
                cells[column] = displayValue(rawRow, name);
            }
        }
        displayRows.push_back(std::move(cells));
    }

    currentColumns_ = std::move(displayColumns);
    currentRows_ = std::move(displayRows);
    ClearTables();
    for (std::size_t index = 0; index < currentColumns_.size(); ++index) {
        const bool hidden = currentColumns_[index] == L"类别" ||
            currentColumns_[index] == L"StatusCode" ||
            currentColumns_[index] == L"RegistrationType" ||
            currentColumns_[index] == L"RemovePolicy" ||
            currentColumns_[index] == L"SourceIndex" ||
            currentColumns_[index] == L"Class" ||
            currentColumns_[index] == L"Source" ||
            currentColumns_[index] == L"Callback" ||
            currentColumns_[index] == L"Context" ||
            currentColumns_[index] == L"Registration" ||
            currentColumns_[index] == L"ModulePath" ||
            currentColumns_[index] == L"Win32ModulePath" ||
            currentColumns_[index] == L"ModuleBase" ||
            currentColumns_[index] == L"ModuleSize" ||
            currentColumns_[index] == L"OperationMask" ||
            currentColumns_[index] == L"ObjectTypeMask" ||
            currentColumns_[index] == L"FieldFlags" ||
            currentColumns_[index] == L"Trust" ||
            currentColumns_[index] == L"Remove" ||
            currentColumns_[index] == L"RemoveFlags" ||
            currentColumns_[index] == L"Generation" ||
            currentColumns_[index] == L"IdentityHash" ||
            currentColumns_[index] == L"RawStorageValue" ||
            currentColumns_[index] == L"LastStatus" ||
            currentColumns_[index] == L"Detail";
        int width = hidden ? 0 : ColumnWidth(currentColumns_[index]);
        if (currentColumns_[index] == L"可信状态") {
            width = 170;
        } else if (currentColumns_[index] == L"可移除") {
            width = 64;
        } else if (currentColumns_[index] == L"回调/对象地址") {
            width = 180;
        } else if (currentColumns_[index] == L"模块") {
            width = 220;
        } else if (currentColumns_[index] == L"名称") {
            width = 260;
        }
        SetColumn(static_cast<int>(index), currentColumns_[index], width);
    }
    CommitRows();
    std::wostringstream status;
    status << L"状态：已刷新 " << totalCallbackRows
           << L" 项，私有未支持 " << unsupportedCount << L" 项";
    if (responseTruncated) {
        status << L"，响应截断";
    }
    if (!filterText.empty()) {
        status << L"，显示 " << currentRows_.size() << L" 项";
    }
    WriteText(statusText_, status.str().c_str());
    if (!currentRows_.empty()) {
        SetSelection(resultList_, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        EnsureItem(resultList_, 0, FALSE);
        SelectionDetail();
    } else {
        WriteText(detailEdit_, L"当前环境未返回可见回调记录。");
    }
}

} // namespace Ksword::Features::Kernel
