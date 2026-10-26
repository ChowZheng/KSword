#include "KernelProjectionBuilder.h"

#include "KernelPageLayout.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace Ksword::Features::Kernel {

void KernelValueProjectionBuilder::RebuildDiagnosticDualTableFromCache(const KernelFeatureId featureId) {
    // RebuildDiagnosticDualTableFromCache reshapes DynData and DriverStatus
    // result packets into the original KernelDock dual-table layout. Inputs are
    // raw ArkDriverClient rows and the local filter text; processing builds the
    // original summary labels, visible matrix rows, and status sentence; output
    // updates the summary table, result table, detail editor, and status label.
    if (!resultList_ || !summaryList_) {
        return;
    }

    const std::vector<std::wstring>& rawColumns = !currentRawColumns_.empty() ? currentRawColumns_ : currentColumns_;
    const std::vector<std::vector<std::wstring>>& rawRows = !currentRawRows_->empty() ? *currentRawRows_ : currentRows_;
    const std::wstring filterText = Text(filterEdit_);
    const auto rawValue = [&](const std::vector<std::wstring>& row, std::initializer_list<const wchar_t*> names) -> std::wstring {
        return ReadField(row, rawColumns, names);
    };
    const auto firstValue = [&](std::initializer_list<const wchar_t*> names) -> std::wstring {
        return FirstRowValue(rawRows, rawColumns, names);
    };
    const auto firstNumber = [&](std::initializer_list<const wchar_t*> names, const std::uint64_t fallback = 0) -> std::uint64_t {
        return FirstRowUInt64(rawRows, rawColumns, names, fallback);
    };
    const auto appendSummary = [&](const std::wstring& name, const std::wstring& value) {
        if (!name.empty() && !value.empty()) {
            AppendSummary(summaryList_, { name, value });
        }
    };
    const auto appendSummaryBool = [&](const std::wstring& name, const bool value) {
        AppendSummary(summaryList_, { name, BoolText(value) });
    };
    const auto addHiddenColumn = [](std::vector<std::wstring>& columns, const wchar_t* name) {
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
        if (columnName == L"Capability") {
            return rawValue(row, { L"Mask", L"CapabilityMask" });
        }
        if (columnName == L"策略") {
            return rawValue(row, { L"Policy", L"RequiredPolicy", L"Flags", L"SecurityPolicy" });
        }
        if (columnName == L"依赖字段") {
            return rawValue(row, { L"Dependency", L"DependencyText" });
        }
        if (columnName == L"原因") {
            return rawValue(row, { L"Reason", L"Detail", L"LastError" });
        }
        return {};
    };

    ClearTables();
    ClearValueList(summaryList_);

    std::vector<std::wstring> displayColumns;
    for (const std::wstring& column : CanonicalColumnNames(featureId)) {
        if (!HasColumn(displayColumns, column)) {
            displayColumns.push_back(column);
        }
    }
    addHiddenColumn(displayColumns, L"Id");
    addHiddenColumn(displayColumns, L"StateId");
    addHiddenColumn(displayColumns, L"StatusFlags");
    addHiddenColumn(displayColumns, L"CapabilityMask");
    addHiddenColumn(displayColumns, L"Version");
    addHiddenColumn(displayColumns, L"Protocol");
    addHiddenColumn(displayColumns, L"ExpectedProtocol");
    addHiddenColumn(displayColumns, L"SecurityPolicy");
    addHiddenColumn(displayColumns, L"DynDataStatus");
    addHiddenColumn(displayColumns, L"DynDataCapability");
    addHiddenColumn(displayColumns, L"FeatureTotal");
    addHiddenColumn(displayColumns, L"FeatureReturned");
    addHiddenColumn(displayColumns, L"LastError");
    addHiddenColumn(displayColumns, L"LastErrorSummary");
        addHiddenColumn(displayColumns, L"LastErrorStatus");
        addHiddenColumn(displayColumns, L"Flags");
        addHiddenColumn(displayColumns, L"StatusQueryOk");
        addHiddenColumn(displayColumns, L"FieldsQueryOk");
        addHiddenColumn(displayColumns, L"PdbProfileScanAttempted");
        addHiddenColumn(displayColumns, L"PdbProfileFound");
        addHiddenColumn(displayColumns, L"PdbProfileApplied");
        addHiddenColumn(displayColumns, L"PdbProfileSource");
        addHiddenColumn(displayColumns, L"PdbProfileName");
        addHiddenColumn(displayColumns, L"PdbProfilePath");
        addHiddenColumn(displayColumns, L"PdbProfileStatus");
        addHiddenColumn(displayColumns, L"PdbProfileAppliedFields");
        addHiddenColumn(displayColumns, L"PdbProfileRejectedFields");
        addHiddenColumn(displayColumns, L"PdbProfileUnknownFields");
        addHiddenColumn(displayColumns, L"PdbProfileIgnoredJsonFields");
        addHiddenColumn(displayColumns, L"PdbProfileMessage");
        addHiddenColumn(displayColumns, L"PdbProfileIo");
        addHiddenColumn(displayColumns, L"MatchedProfileOffset");
        addHiddenColumn(displayColumns, L"MatchedFieldsId");
        addHiddenColumn(displayColumns, L"RequiredPolicy");
        addHiddenColumn(displayColumns, L"DeniedPolicy");
        addHiddenColumn(displayColumns, L"StatusBadges");
        addHiddenColumn(displayColumns, L"DynDataStatusQueryOk");
        addHiddenColumn(displayColumns, L"DynDataFieldsQueryOk");
        addHiddenColumn(displayColumns, L"DynDataStatusIo");
        addHiddenColumn(displayColumns, L"DynDataFieldsIo");
        addHiddenColumn(displayColumns, L"FieldCoverage");
        addHiddenColumn(displayColumns, L"FieldSources");
        addHiddenColumn(displayColumns, L"RequiredMissing");
        addHiddenColumn(displayColumns, L"NtosIdentity");
        addHiddenColumn(displayColumns, L"LxcoreIdentity");
        addHiddenColumn(displayColumns, L"LocalPdbProfileMatched");
        addHiddenColumn(displayColumns, L"LocalPdbProfile");
        addHiddenColumn(displayColumns, L"LocalPdbProfileName");
        addHiddenColumn(displayColumns, L"LocalPdbProfilePath");
        addHiddenColumn(displayColumns, L"LocalPdbMessage");
        addHiddenColumn(displayColumns, L"LocalPdbVersion");
        addHiddenColumn(displayColumns, L"ActiveProcessLinksOffset");
        addHiddenColumn(displayColumns, L"CallbackProfileCoverage");
        addHiddenColumn(displayColumns, L"PdbProfileActive");
        addHiddenColumn(displayColumns, L"CallbackProfileActive");
        addHiddenColumn(displayColumns, L"TrustedPdbOffsetsActive");
        addHiddenColumn(displayColumns, L"TrustedOffset");
        addHiddenColumn(displayColumns, L"UnavailableReason");
        addHiddenColumn(displayColumns, L"Detail");

    std::vector<std::vector<std::wstring>> displayRows;
    std::size_t profileRows = 0;
    std::size_t failedRows = 0;
    std::size_t missingRequiredRows = 0;
    std::size_t unavailableRows = 0;
    std::size_t dynDataDependentRows = 0;
    for (const std::vector<std::wstring>& rawRow : rawRows) {
        const bool isDynField = featureId == KernelFeatureId::DynData &&
            HasValue(rawRow, rawColumns, { L"Field", L"字段", L"Offset", L"Source", L"Feature", L"Mask" }) &&
            !rawValue(rawRow, { L"Field", L"字段" }).empty();
        const bool isDriverFeature = featureId == KernelFeatureId::DriverStatus &&
            HasValue(rawRow, rawColumns, { L"Feature", L"功能", L"State", L"RequiredDyn", L"PresentDyn" }) &&
            !rawValue(rawRow, { L"Feature", L"功能" }).empty();
        const std::wstring merged = MergeRowText(rawRow);
        if (featureId == KernelFeatureId::DynData) {
            const std::wstring source = rawValue(rawRow, { L"Source", L"来源" });
            const std::wstring status = rawValue(rawRow, { L"Status", L"状态", L"DynData Fields IO" });
            if (ContainsCaseInsensitive(source, L"profile") || ContainsCaseInsensitive(source, L"pack") ||
                ContainsCaseInsensitive(source, L"pdb")) {
                ++profileRows;
            }
            if (ContainsCaseInsensitive(status, L"fail") || ContainsCaseInsensitive(status, L"失败") ||
                ContainsCaseInsensitive(status, L"missing") || ContainsCaseInsensitive(status, L"缺失")) {
                ++failedRows;
            }
            if (isDynField && ContainsCaseInsensitive(status, L"缺失(必需)")) {
                ++missingRequiredRows;
            }
        } else if (featureId == KernelFeatureId::DriverStatus) {
            const std::wstring state = rawValue(rawRow, { L"State", L"状态", L"Status" });
            const std::wstring requiredDyn = rawValue(rawRow, { L"RequiredDyn", L"所需DynData" });
            if (ContainsCaseInsensitive(state, L"unavailable") || ContainsCaseInsensitive(state, L"disabled") ||
                ContainsCaseInsensitive(state, L"denied") || ContainsCaseInsensitive(state, L"不可用") ||
                ContainsCaseInsensitive(state, L"禁用")) {
                ++unavailableRows;
            }
            if (!IsZeroMaskText(requiredDyn)) {
                ++dynDataDependentRows;
            }
        }
        if (!isDynField && !isDriverFeature) {
            continue;
        }
        if (!filterText.empty() && !ContainsCaseInsensitive(merged, filterText)) {
            continue;
        }

        std::vector<std::wstring> cells(displayColumns.size());
        for (std::size_t column = 0; column < displayColumns.size(); ++column) {
            const std::wstring& name = displayColumns[column];
            if (name == L"Detail") {
                cells[column] = rawValue(rawRow, { L"Detail" });
            } else {
                cells[column] = displayValue(rawRow, name);
            }
        }
        displayRows.push_back(std::move(cells));
    }

    if (featureId == KernelFeatureId::DynData) {
        const std::uint64_t statusFlags = firstNumber({ L"StatusFlags" });
        const bool initialized = FlagEnabled(statusFlags, KSW_DYN_STATUS_FLAG_INITIALIZED);
        const bool ntosActive = FlagEnabled(statusFlags, KSW_DYN_STATUS_FLAG_NTOS_ACTIVE);
        const bool lxcoreActive = FlagEnabled(statusFlags, KSW_DYN_STATUS_FLAG_LXCORE_ACTIVE);
        const bool extraActive = FlagEnabled(statusFlags, KSW_DYN_STATUS_FLAG_EXTRA_ACTIVE);
        const bool pdbActive = FlagEnabled(statusFlags, KSW_DYN_STATUS_FLAG_PDB_PROFILE_ACTIVE);
        const bool callbackActive = FlagEnabled(statusFlags, KSW_DYN_STATUS_FLAG_CALLBACK_PROFILE_ACTIVE);
        appendSummaryBool(L"DynData 初始化", initialized);
        appendSummaryBool(L"ntoskrnl profile", ntosActive);
        appendSummaryBool(L"lxcore profile", lxcoreActive);
        appendSummaryBool(L"Ksword runtime offset", extraActive);
        appendSummaryBool(L"PDB profile active", pdbActive);
        appendSummaryBool(L"PDB profile 扫描", firstValue({ L"PdbProfileScanAttempted" }) == L"是");
        appendSummaryBool(L"PDB profile 命中", firstValue({ L"PdbProfileFound" }) == L"是");
        appendSummaryBool(L"PDB profile 本次应用", firstValue({ L"PdbProfileApplied" }) == L"是");
        appendSummary(L"PDB profile 来源", firstValue({ L"PdbProfileSource" }));
        appendSummary(L"PDB profile 名称", firstValue({ L"PdbProfileName" }));
        appendSummary(L"PDB profile 路径", firstValue({ L"PdbProfilePath" }));
        appendSummary(L"PDB profile 状态", firstValue({ L"PdbProfileStatus" }));
        appendSummary(L"PDB profile 字段", L"applied=" + firstValue({ L"PdbProfileAppliedFields" }) +
            L" rejected=" + firstValue({ L"PdbProfileRejectedFields" }) +
            L" unknown=" + firstValue({ L"PdbProfileUnknownFields" }) +
            L" ignoredJson=" + firstValue({ L"PdbProfileIgnoredJsonFields" }));
        appendSummary(L"PDB profile 消息", firstValue({ L"PdbProfileMessage" }));
        appendSummary(L"PDB profile IO", firstValue({ L"PdbProfileIo" }));
        appendSummaryBool(L"Callback profile active", callbackActive);
        appendSummary(L"状态位", firstValue({ L"StatusFlags" }));
        appendSummary(L"System Informer 版本", firstValue({ L"SI Version" }));
        appendSummary(L"System Informer 数据长度", firstValue({ L"SI Length" }));
        appendSummary(L"MatchedProfileClass", firstValue({ L"MatchedClass", L"ProfileClass" }));
        appendSummary(L"MatchedProfileOffset", firstValue({ L"MatchedProfileOffset" }));
        appendSummary(L"MatchedFieldsId", firstValue({ L"MatchedFieldsId" }));
        appendSummary(L"CapabilityMask", firstValue({ L"CapabilityMask", L"Mask" }));
        appendSummary(L"字段总数/当前返回", firstValue({ L"FieldCount", L"FieldsTotal" }) + L" / " + std::to_wstring(displayRows.size()));
        appendSummary(L"Fields IO", firstValue({ L"DynData Fields IO" }));
        appendSummary(L"ntoskrnl", firstValue({ L"Ntos", L"NtosIdentity" }));
        appendSummary(L"lxcore", firstValue({ L"Lxcore", L"LxcoreIdentity" }));
        appendSummary(L"Profile/Pack 诊断行", std::to_wstring(profileRows));
        appendSummary(L"失败诊断行", std::to_wstring(failedRows));

        std::wstring statusLine = std::wstring(L"状态：") +
            (ntosActive ? L"ntos profile 已命中" : L"ntos profile 未命中") +
            (pdbActive ? L"，PDB profile 已启用" : L"") +
            L"，字段 " + std::to_wstring(displayRows.size()) +
            L" 项，缺失必需 " + std::to_wstring(missingRequiredRows) + L" 项";
        WriteText(statusText_, statusLine.c_str());
    } else {
        const std::uint64_t statusFlags = firstNumber({ L"StatusFlags" });
        const bool driverLoaded = FlagEnabled(statusFlags, KSWORD_ARK_DRIVER_STATUS_FLAG_DRIVER_LOADED);
        const bool protocolOk = FlagEnabled(statusFlags, KSWORD_ARK_DRIVER_STATUS_FLAG_PROTOCOL_OK);
        const bool dynDataMissing = FlagEnabled(statusFlags, KSWORD_ARK_DRIVER_STATUS_FLAG_DYNDATA_MISSING);
        const bool limited = FlagEnabled(statusFlags, KSWORD_ARK_DRIVER_STATUS_FLAG_LIMITED);
        const std::wstring badges = firstValue({ L"StatusBadges" }).empty()
            ? (std::wstring(L"Driver Loaded=") + BoolText(driverLoaded) +
                L"；Protocol OK=" + BoolText(protocolOk) +
                L"；DynData Missing=" + BoolText(dynDataMissing) +
                L"；Limited=" + BoolText(limited))
            : firstValue({ L"StatusBadges" });
        const std::wstring dynStatus = firstValue({ L"DynDataStatus" });
        const std::wstring dynCapability = firstValue({ L"DynDataCapability", L"CapabilityMask" });
        const std::wstring trustedOffset = !firstValue({ L"TrustedOffset" }).empty()
            ? firstValue({ L"TrustedOffset" })
            : (!dynCapability.empty() && dynCapability != L"0x0"
                ? std::wstring(L"已存在 DynData capability ") + dynCapability
                : std::wstring(L"暂无可用可信偏移。"));
        appendSummary(L"状态栏", badges);
        appendSummaryBool(L"Driver Loaded", driverLoaded);
        appendSummaryBool(L"Protocol OK", protocolOk);
        appendSummaryBool(L"DynData Missing", dynDataMissing);
        appendSummaryBool(L"Limited", limited);
        appendSummary(L"当前内核", firstValue({ L"NtosIdentity", L"Ntos" }));
        appendSummary(L"识别版本", firstValue({ L"LocalPdbVersion", L"KernelVersion" }));
        appendSummary(L"本地 PDB profile", firstValue({ L"LocalPdbProfile", L"LocalPdbMessage" }));
        appendSummary(L"ActiveProcessLinks 偏移", firstValue({ L"ActiveProcessLinksOffset" }));
        appendSummary(L"Callback profile 覆盖", firstValue({ L"CallbackProfileCoverage" }));
        appendSummary(L"可信偏移", trustedOffset);
        appendSummary(L"字段覆盖", firstValue({ L"FieldCoverage" }));
        appendSummary(L"字段来源", firstValue({ L"FieldSources" }));
        appendSummary(L"缺失必需字段", firstValue({ L"RequiredMissing" }));
        appendSummary(L"能力协议版本", firstValue({ L"Version" }));
        appendSummary(L"驱动协议版本", firstValue({ L"Protocol" }));
        appendSummary(L"期望协议版本", firstValue({ L"ExpectedProtocol" }));
        appendSummary(L"状态位", firstValue({ L"StatusFlags" }));
        appendSummary(L"安全策略位", firstValue({ L"SecurityPolicy", L"Policy", L"策略" }));
        appendSummary(L"DynData 状态位", dynStatus);
        appendSummary(L"DynData 能力位", dynCapability);
        appendSummary(L"System Informer 数据", L"version=" + firstValue({ L"SI Version" }) + L" length=" + firstValue({ L"SI Length" }));
        appendSummary(L"匹配内置 Profile", L"class=" + firstValue({ L"MatchedClass" }) +
            L" offset=" + firstValue({ L"MatchedProfileOffset" }) +
            L" fieldsId=" + firstValue({ L"MatchedFieldsId" }));
        appendSummary(L"DynData R3 IO", L"Status=" + firstValue({ L"DynDataStatusQueryOk" }) +
            L" (" + firstValue({ L"DynDataStatusIo" }) + L")；Fields=" +
            firstValue({ L"DynDataFieldsQueryOk" }) + L" (" + firstValue({ L"DynDataFieldsIo" }) + L")");
        appendSummary(L"DynData 不可用原因", firstValue({ L"UnavailableReason" }));
        appendSummary(L"功能数", L"显示 " + std::to_wstring(displayRows.size()) +
            L" / 返回 " + firstValue({ L"FeatureReturned" }) +
            L" / 总计 " + firstValue({ L"FeatureTotal" }));
        appendSummary(L"最近错误", firstValue({ L"LastErrorStatus" }) + L" / " +
            firstValue({ L"LastError" }) + L" / " + firstValue({ L"LastErrorSummary" }));
        appendSummary(L"R3 IO", firstValue({ L"IoMessage", L"Driver Capabilities" }));
        appendSummary(L"不可用/禁用行", std::to_wstring(unavailableRows));
        appendSummary(L"DynData 依赖行", std::to_wstring(dynDataDependentRows));

        const bool driverQueryFailed = rawRows.empty() || (!driverLoaded && !protocolOk && firstValue({ L"Version" }).empty());
        if (driverQueryFailed && dynStatus.empty()) {
            WriteText(statusText_, L"状态：驱动与 DynData 查询均失败");
        } else {
            const std::wstring kernelText = firstValue({ L"NtosIdentity", L"Ntos" }).empty() ? L"内核未识别" : firstValue({ L"NtosIdentity", L"Ntos" });
            const std::wstring statusLine = L"状态：" + badges + L"；" + kernelText + L"；功能 " +
                std::to_wstring(displayRows.size()) + L" 项；可信偏移 " + trustedOffset;
            WriteText(statusText_, statusLine.c_str());
        }
    }

    currentColumns_ = std::move(displayColumns);
    currentRows_ = std::move(displayRows);
    currentRowIndents_.clear();
    for (std::size_t index = 0; index < currentColumns_.size(); ++index) {
        const bool hidden = currentColumns_[index] == L"Id" ||
            currentColumns_[index] == L"StateId" ||
            currentColumns_[index] == L"StatusFlags" ||
            currentColumns_[index] == L"CapabilityMask" ||
            currentColumns_[index] == L"Version" ||
            currentColumns_[index] == L"Protocol" ||
            currentColumns_[index] == L"ExpectedProtocol" ||
            currentColumns_[index] == L"SecurityPolicy" ||
            currentColumns_[index] == L"DynDataStatus" ||
            currentColumns_[index] == L"DynDataCapability" ||
            currentColumns_[index] == L"FeatureTotal" ||
            currentColumns_[index] == L"FeatureReturned" ||
            currentColumns_[index] == L"LastError" ||
            currentColumns_[index] == L"LastErrorSummary" ||
            currentColumns_[index] == L"LastErrorStatus" ||
            currentColumns_[index] == L"Flags" ||
            currentColumns_[index] == L"StatusQueryOk" ||
            currentColumns_[index] == L"FieldsQueryOk" ||
            currentColumns_[index].find(L"PdbProfile") == 0 ||
            currentColumns_[index] == L"MatchedProfileOffset" ||
            currentColumns_[index] == L"MatchedFieldsId" ||
            currentColumns_[index] == L"RequiredPolicy" ||
            currentColumns_[index] == L"DeniedPolicy" ||
            currentColumns_[index] == L"StatusBadges" ||
            currentColumns_[index] == L"DynDataStatusQueryOk" ||
            currentColumns_[index] == L"DynDataFieldsQueryOk" ||
            currentColumns_[index] == L"DynDataStatusIo" ||
            currentColumns_[index] == L"DynDataFieldsIo" ||
            currentColumns_[index] == L"FieldCoverage" ||
            currentColumns_[index] == L"FieldSources" ||
            currentColumns_[index] == L"RequiredMissing" ||
            currentColumns_[index] == L"NtosIdentity" ||
            currentColumns_[index] == L"LxcoreIdentity" ||
            currentColumns_[index] == L"LocalPdbProfileMatched" ||
            currentColumns_[index] == L"LocalPdbProfile" ||
            currentColumns_[index] == L"LocalPdbProfileName" ||
            currentColumns_[index] == L"LocalPdbProfilePath" ||
            currentColumns_[index] == L"LocalPdbMessage" ||
            currentColumns_[index] == L"LocalPdbVersion" ||
            currentColumns_[index] == L"ActiveProcessLinksOffset" ||
            currentColumns_[index] == L"CallbackProfileCoverage" ||
            currentColumns_[index] == L"PdbProfileActive" ||
            currentColumns_[index] == L"CallbackProfileActive" ||
            currentColumns_[index] == L"TrustedPdbOffsetsActive" ||
            currentColumns_[index] == L"TrustedOffset" ||
            currentColumns_[index] == L"UnavailableReason" ||
            currentColumns_[index] == L"Detail";
        SetColumn(static_cast<int>(index), currentColumns_[index], hidden ? 0 : ColumnWidth(currentColumns_[index]));
    }
    CommitRows();
    if (!currentRows_.empty()) {
        SetSelection(resultList_, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        EnsureItem(resultList_, 0, FALSE);
        SelectionDetail();
    } else {
        const std::wstring report = BuildDiagnosticReportForCurrentFeature();
        WriteText(detailEdit_, report.empty() ? EmptyDetailTextForFeature(featureId) : report.c_str());
    }
}

std::wstring KernelValueProjectionBuilder::BuildDiagnosticReportForCurrentFeature() const {
    // BuildDiagnosticReportForCurrentFeature mirrors the original helper
    // buildDynDataReport/buildDriverStatusReport. Inputs are currentRows_ and
    // currentColumns_; processing emits summary key/value lines followed by the
    // visible field/capability matrix; output is empty for non-diagnostic pages.
    const KernelFeatureDescriptor* descriptor = CurrentDescriptor();
    if (descriptor == nullptr) {
        return {};
    }

    const auto rowValue = [&](const std::vector<std::wstring>& row, std::initializer_list<const wchar_t*> names) -> std::wstring {
        return RowFieldByName(row, currentColumns_, names);
    };
    const auto firstValue = [&](std::initializer_list<const wchar_t*> names) -> std::wstring {
        for (const std::vector<std::wstring>& row : currentRows_) {
            const std::wstring value = rowValue(row, names);
            if (!value.empty()) {
                return value;
            }
        }
        return {};
    };
    const auto firstRawValue = [&](std::initializer_list<const wchar_t*> names) -> std::wstring {
        const std::vector<std::wstring>& rawColumns = !currentRawColumns_.empty() ? currentRawColumns_ : currentColumns_;
        const std::vector<std::vector<std::wstring>>& rawRows = !currentRawRows_->empty() ? *currentRawRows_ : currentRows_;
        for (const std::vector<std::wstring>& row : rawRows) {
            const std::wstring value = RowFieldByName(row, rawColumns, names);
            if (!value.empty()) {
                return value;
            }
        }
        return {};
    };
    const auto firstAnyValue = [&](std::initializer_list<const wchar_t*> names) -> std::wstring {
        const std::wstring visible = firstValue(names);
        return visible.empty() ? firstRawValue(names) : visible;
    };
    const auto appendTableHeader = [&](std::wostringstream& report) {
        const std::vector<int> copyColumns = CurrentCopyColumnIndices();
        for (std::size_t position = 0; position < copyColumns.size(); ++position) {
            if (position > 0) {
                report << L'\t';
            }
            const int column = copyColumns[position];
            if (column >= 0 && column < static_cast<int>(currentColumns_.size())) {
                report << currentColumns_[static_cast<std::size_t>(column)];
            }
        }
        report << L"\r\n";
    };
    const auto appendTableRows = [&](std::wostringstream& report) {
        const std::vector<int> copyColumns = CurrentCopyColumnIndices();
        for (const std::vector<std::wstring>& row : currentRows_) {
            for (std::size_t position = 0; position < copyColumns.size(); ++position) {
                if (position > 0) {
                    report << L'\t';
                }
                const int column = copyColumns[position];
                if (column < row.size()) {
                    report << row[static_cast<std::size_t>(column)];
                }
            }
            report << L"\r\n";
        }
    };

    if (descriptor->id == KernelFeatureId::DynData) {
        std::size_t fieldRows = 0;
        std::size_t profileRows = 0;
        std::size_t failedRows = 0;
        for (const std::vector<std::wstring>& row : currentRows_) {
            const std::wstring field = rowValue(row, { L"Field", L"字段", L"Name" });
            const std::wstring source = rowValue(row, { L"Source", L"来源" });
            const std::wstring status = rowValue(row, { L"Status", L"状态", L"DynData Fields IO" });
            if (!field.empty()) {
                ++fieldRows;
            }
            if (ContainsCaseInsensitive(source, L"profile") || ContainsCaseInsensitive(source, L"pack") ||
                ContainsCaseInsensitive(source, L"pdb")) {
                ++profileRows;
            }
            if (ContainsCaseInsensitive(status, L"fail") || ContainsCaseInsensitive(status, L"失败") ||
                ContainsCaseInsensitive(status, L"missing")) {
                ++failedRows;
            }
        }

        std::wostringstream report;
        report << L"Ksword DynData Diagnostic Report\r\n";
        report << L"========================================\r\n";
        AppendReportLine(report, L"StatusQueryOk", firstAnyValue({ L"StatusQueryOk" }));
        AppendReportLine(report, L"FieldsQueryOk", firstAnyValue({ L"FieldsQueryOk" }));
        AppendReportLine(report, L"StatusFlags", firstAnyValue({ L"StatusFlags" }));
        AppendReportLine(report, L"CapabilityMask", firstAnyValue({ L"CapabilityMask", L"Capability" }));
        AppendReportLine(report, L"SystemInformerDataVersion", firstAnyValue({ L"SI Version" }));
        AppendReportLine(report, L"SystemInformerDataLength", firstAnyValue({ L"SI Length" }));
        AppendReportLine(report, L"LastStatus", firstAnyValue({ L"LastStatus" }));
        AppendReportLine(report, L"MatchedProfileClass", firstAnyValue({ L"MatchedClass", L"ProfileClass" }));
        AppendReportLine(report, L"MatchedProfileOffset", firstAnyValue({ L"MatchedProfileOffset" }));
        AppendReportLine(report, L"MatchedFieldsId", firstAnyValue({ L"MatchedFieldsId" }));
        AppendReportLine(report, L"UnavailableReason", firstAnyValue({ L"UnavailableReason" }));
        AppendReportLine(report, L"PdbProfileActive", FlagEnabled(FirstRowUInt64(!currentRawRows_->empty() ? *currentRawRows_ : currentRows_,
            !currentRawColumns_.empty() ? currentRawColumns_ : currentColumns_, { L"StatusFlags" }), KSW_DYN_STATUS_FLAG_PDB_PROFILE_ACTIVE) ? L"是" : L"否");
        AppendReportLine(report, L"PdbProfileScanAttempted", firstAnyValue({ L"PdbProfileScanAttempted" }));
        AppendReportLine(report, L"PdbProfileFound", firstAnyValue({ L"PdbProfileFound" }));
        AppendReportLine(report, L"PdbProfileAppliedThisRefresh", firstAnyValue({ L"PdbProfileApplied" }));
        AppendReportLine(report, L"PdbProfileSource", firstAnyValue({ L"PdbProfileSource" }));
        AppendReportLine(report, L"PdbProfileName", firstAnyValue({ L"PdbProfileName", L"Profile" }));
        AppendReportLine(report, L"PdbProfilePath", firstAnyValue({ L"PdbProfilePath", L"ProfilePath" }));
        AppendReportLine(report, L"PdbProfileStatus", firstAnyValue({ L"PdbProfileStatus" }));
        AppendReportLine(report, L"PdbProfileAppliedFields", firstAnyValue({ L"PdbProfileAppliedFields" }));
        AppendReportLine(report, L"PdbProfileRejectedFields", firstAnyValue({ L"PdbProfileRejectedFields" }));
        AppendReportLine(report, L"PdbProfileUnknownFields", firstAnyValue({ L"PdbProfileUnknownFields" }));
        AppendReportLine(report, L"PdbProfileIgnoredJsonFields", firstAnyValue({ L"PdbProfileIgnoredJsonFields" }));
        AppendReportLine(report, L"PdbProfileMessage", firstAnyValue({ L"PdbProfileMessage" }));
        AppendReportLine(report, L"PdbProfileIo", firstAnyValue({ L"PdbProfileIo" }));
        AppendReportLine(report, L"Ntos", firstAnyValue({ L"Ntos", L"NtosIdentity" }));
        AppendReportLine(report, L"Lxcore", firstAnyValue({ L"Lxcore", L"LxcoreIdentity" }));
        AppendReportLine(report, L"FieldCount", firstAnyValue({ L"FieldCount", L"FieldsTotal" }));
        AppendReportLine(report, L"FieldsReturned", firstAnyValue({ L"FieldsReturned" }));
        report << L"VisibleFieldRows: " << fieldRows << L"\r\n";
        report << L"ProfileDiagnosticRows: " << profileRows << L"\r\n";
        report << L"FailedDiagnosticRows: " << failedRows << L"\r\n\r\n";
        report << L"Capabilities:\r\n";
        std::uint64_t capabilityMask = 0;
        HexToUInt64(firstAnyValue({ L"CapabilityMask", L"Capability" }), capabilityMask);
        report << DynCapabilityNames(capabilityMask) << L"\r\n\r\n";
        report << L"Fields\r\n";
        report << L"----------------------------------------\r\n";
        appendTableHeader(report);
        appendTableRows(report);
        return report.str();
    }

    if (descriptor->id == KernelFeatureId::DriverStatus) {
        std::size_t capabilityRows = 0;
        std::size_t unavailableRows = 0;
        std::size_t dynDataDependentRows = 0;
        for (const std::vector<std::wstring>& row : currentRows_) {
            const std::wstring feature = rowValue(row, { L"Feature", L"功能", L"Name" });
            const std::wstring state = rowValue(row, { L"State", L"状态", L"Status" });
            const std::wstring requiredDyn = rowValue(row, { L"RequiredDyn", L"所需DynData" });
            if (!feature.empty()) {
                ++capabilityRows;
            }
            if (ContainsCaseInsensitive(state, L"unavailable") || ContainsCaseInsensitive(state, L"disabled") ||
                ContainsCaseInsensitive(state, L"不可用") || ContainsCaseInsensitive(state, L"禁用")) {
                ++unavailableRows;
            }
            if (!requiredDyn.empty() && requiredDyn != L"0" && requiredDyn != L"0x0" && requiredDyn != L"0x0000000000000000") {
                ++dynDataDependentRows;
            }
        }

        std::wostringstream report;
        report << L"Ksword Driver Capability Diagnostic Report\r\n";
        report << L"========================================\r\n";
        AppendReportLine(report, L"Status", firstAnyValue({ L"StatusBadges" }));
        AppendReportLine(report, L"QueryOk", firstAnyValue({ L"IO" }));
        AppendReportLine(report, L"IoMessage", firstAnyValue({ L"IoMessage", L"Driver Capabilities" }));
        AppendReportLine(report, L"CapabilityProtocolVersion", firstAnyValue({ L"Version" }));
        AppendReportLine(report, L"DriverProtocolVersion", firstAnyValue({ L"Protocol" }));
        AppendReportLine(report, L"ExpectedDriverProtocolVersion", firstAnyValue({ L"ExpectedProtocol" }));
        AppendReportLine(report, L"StatusFlags", firstAnyValue({ L"StatusFlags" }));
        std::uint64_t policyMask = 0;
        HexToUInt64(firstAnyValue({ L"SecurityPolicy", L"Policy", L"策略" }), policyMask);
        AppendReportLine(report, L"SecurityPolicyFlags", firstAnyValue({ L"SecurityPolicy", L"Policy", L"策略" }) +
            L" (" + SecurityPolicyNames(static_cast<std::uint32_t>(policyMask)) + L")");
        std::uint64_t dynStatusFlags = 0;
        HexToUInt64(firstAnyValue({ L"DynDataStatus" }), dynStatusFlags);
        AppendReportLine(report, L"DynDataStatusFlags", firstAnyValue({ L"DynDataStatus" }) +
            L" (" + DynDataStatusFlagsText(static_cast<std::uint32_t>(dynStatusFlags)) + L")");
        std::uint64_t dynCapability = 0;
        HexToUInt64(firstAnyValue({ L"DynDataCapability", L"CapabilityMask" }), dynCapability);
        AppendReportLine(report, L"DynDataCapabilityMask", firstAnyValue({ L"DynDataCapability", L"CapabilityMask" }) +
            L" (" + DynCapabilityNames(dynCapability) + L")");
        AppendReportLine(report, L"DynDataStatusQueryOk", firstAnyValue({ L"DynDataStatusQueryOk" }));
        AppendReportLine(report, L"DynDataFieldsQueryOk", firstAnyValue({ L"DynDataFieldsQueryOk" }));
        AppendReportLine(report, L"CurrentKernel", firstAnyValue({ L"NtosIdentity", L"Ntos" }));
        AppendReportLine(report, L"RecognizedVersion", firstAnyValue({ L"LocalPdbVersion", L"KernelVersion" }));
        AppendReportLine(report, L"LocalPdbProfileMatched", firstAnyValue({ L"LocalPdbProfileMatched" }));
        AppendReportLine(report, L"LocalPdbProfileName", firstAnyValue({ L"LocalPdbProfileName" }));
        AppendReportLine(report, L"LocalPdbProfilePath", firstAnyValue({ L"LocalPdbProfilePath" }));
        AppendReportLine(report, L"LocalPdbProfileMessage", firstAnyValue({ L"LocalPdbMessage", L"LocalPdbProfile" }));
        AppendReportLine(report, L"ActiveProcessLinksOffset", firstAnyValue({ L"ActiveProcessLinksOffset" }));
        AppendReportLine(report, L"CallbackProfileCoverage", firstAnyValue({ L"CallbackProfileCoverage" }));
        AppendReportLine(report, L"PdbProfileActive", firstAnyValue({ L"PdbProfileActive" }));
        AppendReportLine(report, L"CallbackProfileActive", firstAnyValue({ L"CallbackProfileActive" }));
        AppendReportLine(report, L"TrustedPdbOffsetsActive", firstAnyValue({ L"TrustedPdbOffsetsActive" }));
        AppendReportLine(report, L"TrustedOffsetSummary", firstAnyValue({ L"TrustedOffset" }));
        AppendReportLine(report, L"FieldCoverage", firstAnyValue({ L"FieldCoverage" }));
        AppendReportLine(report, L"FieldSources", firstAnyValue({ L"FieldSources" }));
        AppendReportLine(report, L"SystemInformerData", L"version=" + firstAnyValue({ L"SI Version" }) +
            L" length=" + firstAnyValue({ L"SI Length" }));
        AppendReportLine(report, L"MatchedProfile", L"class=" + firstAnyValue({ L"MatchedClass" }) +
            L" offset=" + firstAnyValue({ L"MatchedProfileOffset" }) +
            L" fieldsId=" + firstAnyValue({ L"MatchedFieldsId" }));
        AppendReportLine(report, L"DynDataUnavailableReason", firstAnyValue({ L"UnavailableReason" }));
        AppendReportLine(report, L"DynDataIo", L"Status=" + firstAnyValue({ L"DynDataStatusQueryOk" }) +
            L" (" + firstAnyValue({ L"DynDataStatusIo" }) + L")；Fields=" +
            firstAnyValue({ L"DynDataFieldsQueryOk" }) + L" (" + firstAnyValue({ L"DynDataFieldsIo" }) + L")");
        AppendReportLine(report, L"LastError", firstAnyValue({ L"LastErrorStatus" }) + L" / " +
            firstAnyValue({ L"LastError" }) + L" / " + firstAnyValue({ L"LastErrorSummary" }));
        AppendReportLine(report, L"FeatureCount", L"returned=" + firstAnyValue({ L"FeatureReturned" }) +
            L" total=" + firstAnyValue({ L"FeatureTotal" }));
        report << L"VisibleCapabilityRows: " << capabilityRows << L"\r\n";
        report << L"UnavailableRows: " << unavailableRows << L"\r\n";
        report << L"DynDataDependentRows: " << dynDataDependentRows << L"\r\n\r\n";
        report << L"Capabilities\r\n";
        report << L"----------------------------------------\r\n";
        appendTableHeader(report);
        appendTableRows(report);
        return report.str();
    }

    return {};
}

std::wstring KernelValueProjectionBuilder::NormalizeCallbackModulePath(const std::wstring& modulePath) const {
    // NormalizeCallbackModulePath mirrors the original CallbackEnum module path
    // normalization. Input is an R0 module path such as \SystemRoot\... or
    // \??\C:\...; processing converts it to a Win32 file path when possible;
    // output is empty when the selected row has no usable module file.
    std::wstring path = modulePath;
    while (!path.empty() && std::iswspace(path.front())) {
        path.erase(path.begin());
    }
    while (!path.empty() && std::iswspace(path.back())) {
        path.pop_back();
    }
    if (path.empty() || path == L"<未解析>" || path == L"<unknown>") {
        return {};
    }
    constexpr wchar_t ntPrefix[] = L"\\??\\";
    if (_wcsnicmp(path.c_str(), ntPrefix, 4) == 0) {
        path.erase(0, 4);
    }
    constexpr wchar_t systemRootPrefix[] = L"\\SystemRoot\\";
    if (_wcsnicmp(path.c_str(), systemRootPrefix, 12) == 0) {
        const auto& windowsDir=input_->windowsDirectory;
        if (!windowsDir.empty()) {
            path = std::wstring(windowsDir) + path.substr(11);
        }
    }
    constexpr wchar_t sysrootPrefix[] = L"SystemRoot\\";
    if (_wcsnicmp(path.c_str(), sysrootPrefix, 11) == 0) {
        const auto& windowsDir=input_->windowsDirectory;
        if (!windowsDir.empty()) {
            path = std::wstring(windowsDir) + L"\\" + path.substr(11);
        }
    }
    return path;
}

KernelValueProjectionBuilder::R0Values KernelValueProjectionBuilder::BuildR0Values(
    const KernelFeatureId featureId,
    std::wstring pageName,
    const std::vector<std::wstring>& rawColumns,
    const KernelRawRows& rawRows,
    std::wstring filterText,
    const bool riskOnly,
    std::wstring selectedRowSignature,
    const int topRow) {
    R0Values snapshot{};
    snapshot.featureId = featureId;
    snapshot.selectedRowSignature = std::move(selectedRowSignature);
    snapshot.topRow = topRow;

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
    const auto isDataRow = [&](const std::vector<std::wstring>& row) {
        switch (featureId) {
        case KernelFeatureId::CpuHardwareSnapshot:
            return HasValue(row, rawColumns, { L"Brand", L"Vendor", L"FeatureMask", L"Leaf1ECX", L"项目" });
        case KernelFeatureId::PhysicalMemoryLayout:
            return HasValue(row, rawColumns, { L"Ranges", L"TotalBytes", L"HighestAddress", L"范围" });
        case KernelFeatureId::MutationAudit:
            return HasValue(row, rawColumns, { L"Seq", L"Tx", L"Operation", L"TargetKind", L"Address" });
        case KernelFeatureId::KeyboardHotkeys:
            return HasValue(row, rawColumns, { L"PID", L"TID", L"热键", L"Object", L"WindowObject" });
        case KernelFeatureId::KeyboardHooks:
            return HasValue(row, rawColumns, { L"PID", L"TID", L"Hook类型", L"Procedure", L"Object" });
        case KernelFeatureId::DynDataCapabilities:
            return HasValue(row, rawColumns, { L"Capability", L"CapabilityMask", L"StatusFlags", L"字段" });
        case KernelFeatureId::MinifilterBypassPids:
            return HasValue(row, rawColumns, { L"PID", L"Process", L"Index" });
        case KernelFeatureId::KernelTimerDpc:
            return HasValue(row, rawColumns, { L"Timer", L"TimerAddress", L"DPC", L"DpcAddress" });
        case KernelFeatureId::IoctlRegistry:
            return HasValue(row, rawColumns, { L"IOCTL", L"IoControlCode", L"Name", L"FunctionNumber" });
        default:
            return false;
        }
    };

    snapshot.columns.push_back(L"#");
    for (const std::wstring& column : CanonicalColumnNames(featureId)) {
        addColumnIfMissing(snapshot.columns, column.c_str());
    }
    for (const std::wstring& column : rawColumns) {
        addColumnIfMissing(snapshot.columns, column.c_str());
    }
    addColumnIfMissing(snapshot.columns, L"Detail");

    std::size_t reportedTotalRows = 0;
    std::size_t reportedReturnedRows = 0;
    std::size_t totalRows = 0;
    std::size_t riskRows = 0;
    std::wstring queryStatus;
    std::wstring unsupportedText;
    std::wstring protocolVersion;
    std::wstring lastStatus;
    for (const std::vector<std::wstring>& rawRow : rawRows) {
        const std::uint64_t total = ReadNonZero(rawRow, rawColumns, { L"Total", L"TotalCount" });
        const std::uint64_t returned = ReadNonZero(rawRow, rawColumns, { L"Returned", L"ReturnedCount" });
        if (total != 0) {
            reportedTotalRows = static_cast<std::size_t>(total);
        }
        if (returned != 0) {
            reportedReturnedRows = static_cast<std::size_t>(returned);
        }
        if (queryStatus.empty()) {
            queryStatus = rawValue(rawRow, { L"Status", L"状态" });
        }
        if (unsupportedText.empty()) {
            unsupportedText = rawValue(rawRow, { L"Unsupported" });
        }
        if (protocolVersion.empty()) {
            protocolVersion = rawValue(rawRow, { L"Version", L"Protocol" });
        }
        if (lastStatus.empty()) {
            lastStatus = rawValue(rawRow, { L"LastStatus" });
        }
    }
    snapshot.rows.reserve(rawRows.size());
    for (const std::vector<std::wstring>& rawRow : rawRows) {
        if (!isDataRow(rawRow)) {
            continue;
        }
        ++totalRows;
        const std::wstring risk = rawValue(rawRow, { L"RiskText", L"Risk", L"FlagsText", L"Flags", L"状态", L"Status" });
        const bool risky = ContainsCaseInsensitive(risk, L"风险") ||
            ContainsCaseInsensitive(risk, L"异常") ||
            ContainsCaseInsensitive(risk, L"failed") ||
            ContainsCaseInsensitive(risk, L"失败") ||
            ContainsCaseInsensitive(risk, L"rejected");
        if (risky) {
            ++riskRows;
        }
        if (riskOnly && !risky) {
            continue;
        }
        if (!filterText.empty() && !ContainsCaseInsensitive(MergeRowText(rawRow), filterText)) {
            continue;
        }
        std::vector<std::wstring> cells(snapshot.columns.size());
        for (std::size_t column = 0; column < snapshot.columns.size(); ++column) {
            const std::wstring& name = snapshot.columns[column];
            if (name == L"#") {
                cells[column] = std::to_wstring(snapshot.rows.size());
            } else if (name == L"Detail") {
                cells[column] = rawValue(rawRow, { L"Detail" });
            } else {
                cells[column] = displayValue(rawRow, name);
            }
        }
        snapshot.rows.push_back(std::move(cells));
    }

    const std::size_t effectiveTotal = reportedTotalRows == 0 ? totalRows : reportedTotalRows;
    const std::size_t effectiveReturned = reportedReturnedRows == 0 ? totalRows : reportedReturnedRows;
    std::wostringstream status;
    status << L"状态：" << pageName << L"已刷新，显示 " << snapshot.rows.size()
           << L"，返回 " << effectiveReturned << L"/" << effectiveTotal;
    if (riskRows != 0) {
        status << L"，风险/异常 " << riskRows;
    }
    if (!queryStatus.empty()) {
        status << L"，Query " << queryStatus;
    }
    if (!unsupportedText.empty()) {
        status << L"，Unsupported " << unsupportedText;
    }
    if (!protocolVersion.empty()) {
        status << L"，Ver " << protocolVersion;
    }
    if (!lastStatus.empty()) {
        status << L"，Last " << lastStatus;
    }
    snapshot.statusText = status.str();
    return snapshot;
}

void KernelValueProjectionBuilder::RebuildR0ValueProjection(const KernelFeatureId featureId) {
    auto snapshot=BuildR0Values(featureId,input_->descriptor.title,currentRawColumns_,*currentRawRows_,input_->filter,input_->riskOnly,{},0);
    currentColumns_=std::move(snapshot.columns);currentRows_=std::move(snapshot.rows);currentRowIndents_.clear();
    ClearTables();
    const std::vector<std::wstring> canonical = CanonicalColumnNames(featureId);
    for (std::size_t index = 0; index < currentColumns_.size(); ++index) {
        const std::wstring& name = currentColumns_[index];
        int width = (name != L"#" && !HasColumn(canonical, name)) ? 0 : ColumnWidth(name);
        if (name == L"对象") width = 180;
        else if (name == L"热键ID" || name == L"进程ID" || name == L"线程ID" || name == L"Flags") width = 80;
        else if (name == L"热键" || name == L"类型" || name == L"范围" || name == L"VK/Mod") width = 130;
        else if (name == L"函数/偏移") width = 170;
        else if (name == L"详情") width = 300;
        else if (name == L"项目" || name == L"Capability") width = 170;
        else if (name == L"值" || name == L"摘要" || name == L"Features" || name == L"字段" || name == L"原因") width = 260;
        else if (name == L"Tx" || name == L"Address" || name == L"回调") width = 165;
        else if (name == L"PID" || name == L"TID" || name == L"Bytes") width = 72;
        else if (name == L"进程" || name == L"Process") width = 180;
        else if (name == L"Detail") width = 320;
        SetColumn(static_cast<int>(index), name, width);
    }

    WriteText(statusText_,snapshot.statusText.c_str());
    if(currentRows_.empty())WriteText(detailEdit_,L"当前过滤条件下没有 R0 证据记录。");
    else {result_->selectFirst=true;SelectionDetail();}
}

} // namespace Ksword::Features::Kernel
