#include "KernelProjectionBuilder.h"

#include "KernelPage.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace Ksword::Features::Kernel {

void KernelValueProjectionBuilder::RebuildObjectNamespaceListFromCache(const KernelFeatureId featureId) {
    // RebuildObjectNamespaceListFromCache reshapes generic KernelFacade rows
    // into the original KernelDock object-namespace tree/table schemas. Inputs
    // are currentColumns_/currentRows_ from AddDynamicResultRow; processing adds
    // root rows, tree indentation, original headers and hidden action fields;
    // there is no return value because the ListView owns the visible output.
    const std::vector<std::wstring>& rawColumns = !currentRawColumns_.empty() ? currentRawColumns_ : currentColumns_;
    const std::vector<std::vector<std::wstring>>& rawRows = !currentRawRows_->empty() ? *currentRawRows_ : currentRows_;
    std::vector<std::wstring> displayColumns;
    for (const std::wstring& column : CanonicalColumnNames(featureId)) {
        if (!HasColumn(displayColumns, column)) {
            displayColumns.push_back(column);
        }
    }
    const auto addHiddenColumn = [&](const wchar_t* name) {
        if (!HasColumn(displayColumns, name)) {
            displayColumns.push_back(name);
        }
    };
    addHiddenColumn(L"Parent");
    addHiddenColumn(L"Source");
    addHiddenColumn(L"Directory");
    addHiddenColumn(L"directoryPath");
    addHiddenColumn(L"sourceDirectory");
    addHiddenColumn(L"Depth");
    addHiddenColumn(L"Path");
    addHiddenColumn(L"NtPath");
    addHiddenColumn(L"Target");
    addHiddenColumn(L"Win32Path");
    addHiddenColumn(L"dosCandidate");
    addHiddenColumn(L"fullPath");
    addHiddenColumn(L"targetPath");
    addHiddenColumn(L"symbolicTarget");
    addHiddenColumn(L"objectName");
    addHiddenColumn(L"objectType");
    addHiddenColumn(L"linkName");
    addHiddenColumn(L"Handles");
    addHiddenColumn(L"Pointers");
    addHiddenColumn(L"Detail");
    addHiddenColumn(L"EnumApi");
    addHiddenColumn(L"enumApi");
    addHiddenColumn(L"枚举 API");
    addHiddenColumn(L"EnumerationApi");
    addHiddenColumn(L"NodeKind");

    const auto rawValue = [&](const std::vector<std::wstring>& row, std::initializer_list<const wchar_t*> names) -> std::wstring {
        return ReadField(row, rawColumns, names);
    };
    const auto displayValue = [&](const std::vector<std::wstring>& row, const std::wstring& columnName) -> std::wstring {
        if (columnName == L"Detail") {
            return rawValue(row, { L"Detail" });
        }
        const auto& aliases = Aliases(featureId, columnName);
        for (const std::wstring& alias : aliases) {
            const std::wstring value = ReadField(row, rawColumns, { alias.c_str() });
            if (!value.empty()) {
                return value;
            }
        }
        return {};
    };
    if (featureId == KernelFeatureId::DeviceDriverObjects && deviceDriverDirectoryCombo_ && deviceDriverTypeCombo_) {
        std::set<std::wstring> types;
        for (const std::vector<std::wstring>& rawRow : rawRows) {
            const std::wstring type = rawValue(rawRow, { L"Type", L"objectType", L"对象类型", L"类型" });
            if (!type.empty()) {
                types.insert(type);
            }
        }
        const std::wstring previousDirectory = [this]() -> std::wstring {
            const int selection = static_cast<int>(ChoiceMessage(deviceDriverDirectoryCombo_, CB_GETCURSEL, 0, 0));
            if (selection <= 0) {
                return {};
            }
            std::wstring buffer;
            buffer = ChoiceText(deviceDriverDirectoryCombo_);
            return buffer;
        }();
        const std::wstring previousType = [this]() -> std::wstring {
            const int selection = static_cast<int>(ChoiceMessage(deviceDriverTypeCombo_, CB_GETCURSEL, 0, 0));
            if (selection <= 0) {
                return {};
            }
            std::wstring buffer;
            buffer = ChoiceText(deviceDriverTypeCombo_);
            return buffer;
        }();

        ChoiceMessage(deviceDriverDirectoryCombo_, CB_RESETCONTENT, 0, 0);
        ChoiceMessage(deviceDriverDirectoryCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"全部目录"));
        int restoredDirectory = 0;
        for (const wchar_t* directory : kDeviceDriverFixedDirectories) {
            const int index = static_cast<int>(ChoiceMessage(deviceDriverDirectoryCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(directory)));
            if (!previousDirectory.empty() && _wcsicmp(previousDirectory.c_str(), directory) == 0) {
                restoredDirectory = index;
            }
        }
        ChoiceMessage(deviceDriverDirectoryCombo_, CB_SETCURSEL, restoredDirectory, 0);

        ChoiceMessage(deviceDriverTypeCombo_, CB_RESETCONTENT, 0, 0);
        ChoiceMessage(deviceDriverTypeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"全部类型"));
        int restoredType = 0;
        for (const std::wstring& type : types) {
            const int index = static_cast<int>(ChoiceMessage(deviceDriverTypeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(type.c_str())));
            if (!previousType.empty() && _wcsicmp(previousType.c_str(), type.c_str()) == 0) {
                restoredType = index;
            }
        }
        ChoiceMessage(deviceDriverTypeCombo_, CB_SETCURSEL, restoredType, 0);
    }
    if (featureId == KernelFeatureId::BaseNamedObjects && baseNamedScopeCombo_ && baseNamedTypeCombo_) {
        std::set<std::wstring> scopes;
        std::set<std::wstring> types;
        for (const std::vector<std::wstring>& rawRow : rawRows) {
            const std::wstring scope = rawValue(rawRow, { L"Source", L"Scope", L"scope" });
            const std::wstring type = rawValue(rawRow, { L"Type", L"objectType", L"类型" });
            if (!scope.empty()) {
                scopes.insert(scope);
            }
            if (!type.empty()) {
                types.insert(type);
            }
        }
        const std::wstring previousScope = [this]() -> std::wstring {
            const int selection = static_cast<int>(ChoiceMessage(baseNamedScopeCombo_, CB_GETCURSEL, 0, 0));
            if (selection <= 0) {
                return {};
            }
            std::wstring buffer;
            buffer = ChoiceText(baseNamedScopeCombo_);
            return buffer;
        }();
        const std::wstring previousType = [this]() -> std::wstring {
            const int selection = static_cast<int>(ChoiceMessage(baseNamedTypeCombo_, CB_GETCURSEL, 0, 0));
            if (selection <= 0) {
                return {};
            }
            std::wstring buffer;
            buffer = ChoiceText(baseNamedTypeCombo_);
            return buffer;
        }();
        ChoiceMessage(baseNamedScopeCombo_, CB_RESETCONTENT, 0, 0);
        ChoiceMessage(baseNamedScopeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"全部 Session"));
        int restoredScope = 0;
        for (const std::wstring& scope : scopes) {
            const int index = static_cast<int>(ChoiceMessage(baseNamedScopeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(scope.c_str())));
            if (!previousScope.empty() && _wcsicmp(previousScope.c_str(), scope.c_str()) == 0) {
                restoredScope = index;
            }
        }
        ChoiceMessage(baseNamedScopeCombo_, CB_SETCURSEL, restoredScope, 0);
        ChoiceMessage(baseNamedTypeCombo_, CB_RESETCONTENT, 0, 0);
        ChoiceMessage(baseNamedTypeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"全部类型"));
        int restoredType = 0;
        for (const std::wstring& type : types) {
            const int index = static_cast<int>(ChoiceMessage(baseNamedTypeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(type.c_str())));
            if (!previousType.empty() && _wcsicmp(previousType.c_str(), type.c_str()) == 0) {
                restoredType = index;
            }
        }
        ChoiceMessage(baseNamedTypeCombo_, CB_SETCURSEL, restoredType, 0);
    }
    const auto rowMatchesObjectFilters = [&](const std::vector<std::wstring>& row) -> bool {
        const std::wstring primaryFilter = Text(filterEdit_);
        const std::wstring secondaryFilter = Text(moduleFilterEdit_);
        if (featureId == KernelFeatureId::ObjectNamespaceOverview) {
            const std::wstring merged = rawValue(row, { L"Parent", L"Source" }) + L" | " +
                rawValue(row, { L"Name" }) + L" | " +
                rawValue(row, { L"Type" }) + L" | " +
                rawValue(row, { L"Path" }) + L" | " +
                rawValue(row, { L"Status" }) + L" | " +
                rawValue(row, { L"Target" });
            return primaryFilter.empty() || ContainsCaseInsensitive(merged, primaryFilter);
        }
        if (featureId == KernelFeatureId::ObjectDirectoryRecursive) {
            return true;
        }
        if (featureId == KernelFeatureId::BaseNamedObjects) {
            std::wstring scopeFilter;
            std::wstring typeFilter;
            if (baseNamedScopeCombo_) {
                const int selection = static_cast<int>(ChoiceMessage(baseNamedScopeCombo_, CB_GETCURSEL, 0, 0));
                if (selection > 0) {
                    std::wstring buffer;
                    buffer = ChoiceText(baseNamedScopeCombo_);
                    scopeFilter = buffer;
                }
            }
            if (baseNamedTypeCombo_) {
                const int selection = static_cast<int>(ChoiceMessage(baseNamedTypeCombo_, CB_GETCURSEL, 0, 0));
                if (selection > 0) {
                    std::wstring buffer;
                    buffer = ChoiceText(baseNamedTypeCombo_);
                    typeFilter = buffer;
                }
            }
            const std::wstring merged = rawValue(row, { L"Source", L"Scope" }) + L" | " +
                rawValue(row, { L"Parent", L"Directory" }) + L" | " +
                rawValue(row, { L"Name" }) + L" | " +
                rawValue(row, { L"Type" }) + L" | " +
                rawValue(row, { L"Path" }) + L" | " +
                rawValue(row, { L"Target" }) + L" | " +
                rawValue(row, { L"Status" });
            const bool keywordMatched = primaryFilter.empty() || ContainsCaseInsensitive(merged, primaryFilter);
            const bool scopeMatched = scopeFilter.empty() ||
                _wcsicmp(rawValue(row, { L"Source", L"Scope", L"scope" }).c_str(), scopeFilter.c_str()) == 0;
            const bool typeMatched = typeFilter.empty() ||
                _wcsicmp(rawValue(row, { L"Type", L"objectType" }).c_str(), typeFilter.c_str()) == 0;
            const bool legacyMatched = secondaryFilter.empty() ||
                ContainsCaseInsensitive(rawValue(row, { L"Source", L"Scope" }), secondaryFilter) ||
                ContainsCaseInsensitive(rawValue(row, { L"Type" }), secondaryFilter);
            const bool auxMatched = scopeMatched && typeMatched && legacyMatched;
            return keywordMatched && auxMatched;
        }
        if (featureId == KernelFeatureId::NamedPipe) {
            const std::wstring merged = rawValue(row, { L"Pipe", L"Name" }) + L" | " +
                rawValue(row, { L"NtPath", L"Path" }) + L" | " +
                rawValue(row, { L"Attributes" }) + L" | " +
                rawValue(row, { L"LastWrite" }) + L" | " +
                rawValue(row, { L"Status" }) + L" | " +
                rawValue(row, { L"Directory", L"Source" });
            return primaryFilter.empty() || ContainsCaseInsensitive(merged, primaryFilter);
        }
        if (featureId == KernelFeatureId::ObjectTypeMatrix) {
            const std::wstring merged = rawValue(row, { L"TypeIndex", L"Index" }) + L" | " +
                rawValue(row, { L"Type" }) + L" | " +
                rawValue(row, { L"Objects" }) + L" | " +
                rawValue(row, { L"Handles" }) + L" | " +
                rawValue(row, { L"ValidAccess" }) + L" | " +
                rawValue(row, { L"R0Address" }) + L" | " +
                rawValue(row, { L"R0Validation" }) + L" | " +
                rawValue(row, { L"Detail", L"Status" });
            return primaryFilter.empty() || ContainsCaseInsensitive(merged, primaryFilter);
        }
        if (featureId == KernelFeatureId::CommunicationEndpoint) {
            const std::wstring merged = rawValue(row, { L"Source", L"Parent" }) + L" | " +
                rawValue(row, { L"Name" }) + L" | " +
                rawValue(row, { L"Type" }) + L" | " +
                rawValue(row, { L"Path" }) + L" | " +
                rawValue(row, { L"Status" });
            return primaryFilter.empty() || ContainsCaseInsensitive(merged, primaryFilter);
        }
        if (featureId != KernelFeatureId::SymbolicLink &&
            featureId != KernelFeatureId::DeviceDriverObjects) {
            return true;
        }
        if (featureId == KernelFeatureId::SymbolicLink) {
            const std::wstring merged = rawValue(row, { L"Source", L"Parent" }) + L" | " +
                rawValue(row, { L"Name" }) + L" | " +
                rawValue(row, { L"Path" }) + L" | " +
                rawValue(row, { L"Status" }) + L" | " +
                rawValue(row, { L"Target" }) + L" | " +
                rawValue(row, { L"Win32Path", L"DosCandidates" });
            const bool primaryMatched = primaryFilter.empty() || ContainsCaseInsensitive(merged, primaryFilter);
            const bool secondaryMatched = secondaryFilter.empty() ||
                ContainsCaseInsensitive(rawValue(row, { L"Target" }), secondaryFilter) ||
                ContainsCaseInsensitive(rawValue(row, { L"Win32Path", L"DosCandidates" }), secondaryFilter);
            return primaryMatched && secondaryMatched;
        }
        if (featureId == KernelFeatureId::DeviceDriverObjects) {
            std::wstring directoryFilter;
            std::wstring typeFilter;
            if (deviceDriverDirectoryCombo_) {
                const int selection = static_cast<int>(ChoiceMessage(deviceDriverDirectoryCombo_, CB_GETCURSEL, 0, 0));
                if (selection > 0) {
                    std::wstring buffer;
                    buffer = ChoiceText(deviceDriverDirectoryCombo_);
                    directoryFilter = buffer;
                }
            }
            if (deviceDriverTypeCombo_) {
                const int selection = static_cast<int>(ChoiceMessage(deviceDriverTypeCombo_, CB_GETCURSEL, 0, 0));
                if (selection > 0) {
                    std::wstring buffer;
                    buffer = ChoiceText(deviceDriverTypeCombo_);
                    typeFilter = buffer;
                }
            }
            const std::wstring directory = rawValue(row, { L"Parent", L"Directory", L"Source", L"directoryPath", L"目录路径" });
            const std::wstring type = rawValue(row, { L"Type", L"objectType", L"对象类型", L"类型" });
            const std::wstring keywordMerged = directory + L" | " +
                rawValue(row, { L"Name", L"DriverName", L"objectName", L"对象名称" }) + L" | " +
                type + L" | " +
                rawValue(row, { L"Path", L"NtPath", L"fullPath", L"完整路径" }) + L" | " +
                rawValue(row, { L"Target", L"targetPath", L"symbolicTarget", L"目标路径" }) + L" | " +
                rawValue(row, { L"Status", L"statusText", L"状态" }) + L" | " +
                rawValue(row, { L"Detail", L"Capability", L"capabilityHintText", L"能力提示" });
            const bool keywordMatched = primaryFilter.empty() || ContainsCaseInsensitive(keywordMerged, primaryFilter);
            const bool directoryMatched = directoryFilter.empty() || _wcsicmp(directory.c_str(), directoryFilter.c_str()) == 0;
            const bool typeMatched = typeFilter.empty() || _wcsicmp(type.c_str(), typeFilter.c_str()) == 0;
            return keywordMatched && directoryMatched && typeMatched;
        }
        return true;
    };
    const auto depthOf = [&](const std::vector<std::wstring>& row) -> int {
        const std::wstring text = rawValue(row, { L"Depth", L"深度" });
        if (text.empty()) {
            return 0;
        }
        wchar_t* end = nullptr;
        const long value = std::wcstol(text.c_str(), &end, 10);
        return end != text.c_str() && value > 0 ? static_cast<int>(std::min<long>(value, 16)) : 0;
    };
    std::vector<std::vector<std::wstring>> displayRows;
    std::vector<int> displayIndents;
    std::vector<std::wstring> insertedRoots;
    std::vector<std::wstring> insertedDirectories;
    std::size_t meaningfulRowCount = 0;
    const auto appendDisplayRow = [&](std::vector<std::wstring> cells, const int indent) {
        if (!cells.empty() && !displayColumns.empty() && displayColumns[0] == L"#") {
            cells[0] = std::to_wstring(displayRows.size());
        }
        displayRows.push_back(std::move(cells));
        displayIndents.push_back(std::max(0, indent));
    };
    const auto makeEmptyCells = [&]() {
        return std::vector<std::wstring>(displayColumns.size());
    };
    const auto appendOverviewRoot = [&](const std::wstring& root, const std::wstring& status) {
        if (root.empty()) {
            return;
        }
        const auto duplicate = std::find_if(insertedRoots.begin(), insertedRoots.end(), [&](const std::wstring& existing) {
            return _wcsicmp(existing.c_str(), root.c_str()) == 0;
        });
        if (duplicate != insertedRoots.end()) {
            return;
        }
        insertedRoots.push_back(root);
        std::vector<std::wstring> cells = makeEmptyCells();
        for (std::size_t column = 0; column < displayColumns.size(); ++column) {
            const std::wstring& name = displayColumns[column];
            if (name == L"名称") cells[column] = root;
            else if (name == L"类型") cells[column] = L"根目录";
            else if (name == L"路径/说明") cells[column] = status.empty() ? root : status;
            else if (name == L"状态") cells[column] = L"根节点";
            else if (name == L"Parent" || name == L"Source" || name == L"Path") cells[column] = root;
            else if (name == L"Depth") cells[column] = L"0";
            else if (name == L"NodeKind") cells[column] = L"Root";
            else if (name == L"Detail") cells[column] = L"Object Manager root: " + root;
        }
        appendDisplayRow(std::move(cells), 0);
    };
    const auto appendOverviewDirectory = [&](const std::wstring& root, const std::wstring& directory, const std::wstring& scope) {
        // appendOverviewDirectory reproduces the original middle tree level:
        // each root owns one row per enumerated directory, and object rows sit
        // under that directory. Inputs are root/scope/directory text from the
        // facade row; processing de-duplicates the pair; no value is returned.
        if (directory.empty()) {
            return;
        }
        const std::wstring key = root + L"\n" + directory;
        const auto duplicate = std::find_if(insertedDirectories.begin(), insertedDirectories.end(), [&](const std::wstring& existing) {
            return _wcsicmp(existing.c_str(), key.c_str()) == 0;
        });
        if (duplicate != insertedDirectories.end()) {
            return;
        }
        insertedDirectories.push_back(key);
        std::vector<std::wstring> cells = makeEmptyCells();
        for (std::size_t column = 0; column < displayColumns.size(); ++column) {
            const std::wstring& name = displayColumns[column];
            if (name == L"名称") cells[column] = LeafObjectName(directory);
            else if (name == L"类型") cells[column] = L"目录";
            else if (name == L"路径/说明") cells[column] = directory;
            else if (name == L"状态") cells[column] = L"已展开枚举";
            else if (name == L"Parent" || name == L"Source") cells[column] = root;
            else if (name == L"Directory" || name == L"directoryPath" || name == L"Path" || name == L"fullPath") cells[column] = directory;
            else if (name == L"Depth") cells[column] = L"1";
            else if (name == L"NodeKind") cells[column] = L"Directory";
            else if (name == L"Detail") cells[column] = scope.empty()
                ? (L"Object Manager directory: " + directory)
                : (L"Object Manager directory: " + directory + L"\r\n" + scope);
        }
        appendDisplayRow(std::move(cells), 1);
    };
    if (featureId == KernelFeatureId::ObjectDirectoryRecursive) {
        std::wstring rootPath = Text(filterEdit_);
        if (rootPath.empty()) {
            rootPath = L"\\";
        }
        std::vector<std::wstring> cells = makeEmptyCells();
        for (std::size_t column = 0; column < displayColumns.size(); ++column) {
            const std::wstring& name = displayColumns[column];
            if (name == L"名称") cells[column] = LeafObjectName(rootPath);
            else if (name == L"类型") cells[column] = L"Directory";
            else if (name == L"完整路径" || name == L"Path") cells[column] = rootPath;
            else if (name == L"深度" || name == L"Depth") cells[column] = L"0";
            else if (name == L"状态") cells[column] = L"根目录";
            else if (name == L"Parent" || name == L"Source") cells[column] = ParentObjectPath(rootPath).empty() ? rootPath : ParentObjectPath(rootPath);
            else if (name == L"Detail") cells[column] = L"Object Manager recursive root: " + rootPath;
        }
        appendDisplayRow(std::move(cells), 0);
    }

    for (const std::vector<std::wstring>& rawRow : rawRows) {
        const bool meaningful =
            HasValue(rawRow, rawColumns, { L"Name", L"Path", L"NtPath", L"Pipe", L"TypeIndex", L"Type" }) ||
            HasValue(rawRow, rawColumns, { L"Parent", L"Source", L"Directory", L"Target" });
        if (!meaningful) {
            continue;
        }
        ++meaningfulRowCount;
        const std::wstring parent = rawValue(rawRow, { L"Parent", L"Directory", L"sourceDirectory", L"来源目录", L"目录路径" });
        const std::wstring source = rawValue(rawRow, { L"Source", L"scope" });
        const std::wstring path = rawValue(rawRow, { L"Path", L"NtPath", L"fullPath", L"完整路径" });
        const std::wstring objectName = rawValue(rawRow, { L"Name", L"Pipe", L"objectName", L"linkName", L"名称", L"对象名称", L"Type" });
        const std::wstring objectType = rawValue(rawRow, { L"Type", L"objectType", L"类型", L"对象类型" });
        const bool isQueryHeader = !HasValue(rawRow, rawColumns, { L"Name", L"Pipe", L"objectName", L"linkName", L"TypeIndex" }) &&
            HasValue(rawRow, rawColumns, { L"功能", L"数据源", L"Rows", L"Warnings" });
        if (isQueryHeader) {
            continue;
        }
        if (!rowMatchesObjectFilters(rawRow)) {
            continue;
        }

        if (featureId == KernelFeatureId::ObjectNamespaceOverview) {
            const std::wstring directory = !parent.empty() ? parent : (!rawValue(rawRow, { L"directoryPathText", L"directoryPath", L"Directory" }).empty()
                ? rawValue(rawRow, { L"directoryPathText", L"directoryPath", L"Directory" })
                : ParentObjectPath(path));
            const std::wstring root = !source.empty() ? source : (!rawValue(rawRow, { L"rootPathText", L"Root" }).empty()
                ? rawValue(rawRow, { L"rootPathText", L"Root" })
                : (!directory.empty() ? directory : path));
            const std::wstring scope = rawValue(rawRow, { L"scopeDescriptionText", L"Scope", L"scope", L"Detail" });
            if (Text(filterEdit_).empty() ||
                ContainsCaseInsensitive(root, Text(filterEdit_)) ||
                ContainsCaseInsensitive(directory, Text(filterEdit_)) ||
                ContainsCaseInsensitive(scope, Text(filterEdit_)) ||
                ContainsCaseInsensitive(path, Text(filterEdit_)) ||
                ContainsCaseInsensitive(objectName, Text(filterEdit_)) ||
                ContainsCaseInsensitive(objectType, Text(filterEdit_)) ||
                ContainsCaseInsensitive(rawValue(rawRow, { L"Status" }), Text(filterEdit_)) ||
                ContainsCaseInsensitive(rawValue(rawRow, { L"Target" }), Text(filterEdit_))) {
                appendOverviewRoot(root, scope);
                appendOverviewDirectory(root, directory, scope);
            }
            if (objectName.empty() && objectType.empty() && parent.empty()) {
                continue;
            }
        }

        std::vector<std::wstring> cells = makeEmptyCells();
        int rowIndent = 0;
        if (featureId == KernelFeatureId::ObjectNamespaceOverview) {
            rowIndent = 2;
        } else if (featureId == KernelFeatureId::ObjectDirectoryRecursive) {
            rowIndent = depthOf(rawRow) + 1;
        }
        for (std::size_t column = 0; column < displayColumns.size(); ++column) {
            const std::wstring& name = displayColumns[column];
            if (name == L"#") {
                continue;
            }
            std::wstring value = displayValue(rawRow, name);
            if (featureId == KernelFeatureId::ObjectNamespaceOverview && name == L"名称") {
                value = objectName.empty() ? path : objectName;
            } else if (featureId == KernelFeatureId::ObjectNamespaceOverview && name == L"类型" && value.empty()) {
                value = objectType.empty() ? L"<未知>" : objectType;
            } else if (featureId == KernelFeatureId::ObjectNamespaceOverview && name == L"路径/说明" && value.empty()) {
                value = path;
            } else if (featureId == KernelFeatureId::ObjectNamespaceOverview && name == L"NodeKind") {
                value = L"ObjectEntry";
            } else if (featureId == KernelFeatureId::ObjectDirectoryRecursive && name == L"名称") {
                value = objectName.empty() ? path : objectName;
            } else if (featureId == KernelFeatureId::NamedPipe && name == L"Pipe Name" && value.empty()) {
                const std::wstring ntPath = rawValue(rawRow, { L"NtPath", L"Path" });
                const std::size_t slash = ntPath.find_last_of(L'\\');
                value = slash == std::wstring::npos ? ntPath : ntPath.substr(slash + 1);
            } else if (featureId == KernelFeatureId::DeviceDriverObjects && name == L"目标路径" && value.empty()) {
                value = L"<无>";
            } else if (name == L"Detail" && value.empty()) {
                value = rawValue(rawRow, { L"Status", L"statusText", L"Target", L"symbolicTarget", L"targetPath" });
            }
            cells[column] = std::move(value);
        }
        appendDisplayRow(std::move(cells), rowIndent);
    }

    currentColumns_ = std::move(displayColumns);
    currentRows_ = std::move(displayRows);
    ClearTables();
    for (std::size_t index = 0; index < currentColumns_.size(); ++index) {
        const std::wstring& columnName = currentColumns_[index];
        const bool hidden = columnName == L"Parent" ||
            columnName == L"Source" ||
            columnName == L"Directory" ||
            columnName == L"directoryPath" ||
            columnName == L"sourceDirectory" ||
            columnName == L"Depth" ||
            columnName == L"Path" ||
            columnName == L"NtPath" ||
            columnName == L"Target" ||
            columnName == L"Win32Path" ||
            columnName == L"dosCandidate" ||
            columnName == L"fullPath" ||
            columnName == L"targetPath" ||
            columnName == L"symbolicTarget" ||
            columnName == L"objectName" ||
            columnName == L"objectType" ||
            columnName == L"linkName" ||
            columnName == L"Handles" ||
            columnName == L"Pointers" ||
            columnName == L"EnumApi" ||
            columnName == L"enumApi" ||
            columnName == L"枚举 API" ||
            columnName == L"EnumerationApi" ||
            columnName == L"NodeKind" ||
            columnName == L"Detail";
        int width = hidden ? 0 : ColumnWidth(columnName);
        if (featureId == KernelFeatureId::DeviceDriverObjects) {
            if (columnName == L"目录路径") width = 190;
            else if (columnName == L"对象名称") width = 180;
            else if (columnName == L"对象类型") width = 130;
            else if (columnName == L"完整路径") width = 320;
            else if (columnName == L"目标路径") width = 320;
            else if (columnName == L"状态") width = 260;
            else if (columnName == L"能力提示") width = 360;
        }
        SetColumn(static_cast<int>(index), columnName, width);
    }
    currentRowIndents_ = std::move(displayIndents);
    CommitRows();
    if (featureId == KernelFeatureId::ObjectNamespaceOverview ||
        featureId == KernelFeatureId::ObjectDirectoryRecursive) {
        RequireTreeValueModel();
    }
    std::wostringstream status;
    switch (featureId) {
    case KernelFeatureId::ObjectNamespaceOverview:
    {
        const std::wstring keyword = Text(filterEdit_);
        status << L"状态：对象命名空间已刷新，显示 " << currentRows_.size()
               << L" / " << meaningfulRowCount << L" 项";
        if (!keyword.empty()) {
            status << L"，过滤=" << keyword;
        }
        break;
    }
    case KernelFeatureId::ObjectDirectoryRecursive:
    {
        std::size_t directoryCount = 0;
        std::size_t failedDirectoryCount = 0;
        bool depthLimitReached = false;
        for (const std::vector<std::wstring>& row : currentRows_) {
            const std::wstring type = ReadField(row, currentColumns_, { L"类型", L"Type", L"objectType" });
            const std::wstring rowStatus = ReadField(row, currentColumns_, { L"状态", L"Status", L"statusText" });
            if (_wcsicmp(type.c_str(), L"Directory") == 0) {
                ++directoryCount;
                if (ContainsCaseInsensitive(rowStatus, L"失败") ||
                    ContainsCaseInsensitive(rowStatus, L"denied") ||
                    ContainsCaseInsensitive(rowStatus, L"error") ||
                    ContainsCaseInsensitive(rowStatus, L"0xC")) {
                    ++failedDirectoryCount;
                }
            }
            if (ContainsCaseInsensitive(rowStatus, L"深度") ||
                ContainsCaseInsensitive(rowStatus, L"depth")) {
                depthLimitReached = true;
            }
        }
        status << L"状态：已枚举 " << currentRows_.size()
               << L" 项，目录 " << directoryCount
               << L" 个，失败目录 " << failedDirectoryCount << L" 个";
        if (depthLimitReached) {
            status << L"，触达深度上限";
        }
        break;
    }
    case KernelFeatureId::NamedPipe:
    {
        const std::wstring keyword = Text(filterEdit_);
        status << L"状态：显示 " << currentRows_.size() << L" / " << meaningfulRowCount;
        if (!keyword.empty()) {
            status << L"，过滤=" << keyword;
        }
        break;
    }
    case KernelFeatureId::BaseNamedObjects:
    {
        std::wstring scopeText = L"全部 Session";
        std::wstring typeText = L"全部类型";
        if (baseNamedScopeCombo_) {
            const int selection = static_cast<int>(ChoiceMessage(baseNamedScopeCombo_, CB_GETCURSEL, 0, 0));
            if (selection >= 0) {
                std::wstring buffer;
                buffer = ChoiceText(baseNamedScopeCombo_);
                if (!buffer.empty()) {
                    scopeText = buffer;
                }
            }
        }
        if (baseNamedTypeCombo_) {
            const int selection = static_cast<int>(ChoiceMessage(baseNamedTypeCombo_, CB_GETCURSEL, 0, 0));
            if (selection >= 0) {
                std::wstring buffer;
                buffer = ChoiceText(baseNamedTypeCombo_);
                if (!buffer.empty()) {
                    typeText = buffer;
                }
            }
        }
        const std::wstring keyword = Text(filterEdit_);
        status << L"状态：BaseNamedObjects 已刷新，总数 " << meaningfulRowCount
               << L"，可见 " << currentRows_.size()
               << L"，scope=" << scopeText
               << L"，类型=" << typeText;
        if (!keyword.empty()) {
            status << L"，关键字=" << keyword;
        }
        break;
    }
    case KernelFeatureId::SymbolicLink:
    {
        const std::wstring keyword = Text(filterEdit_);
        const std::wstring targetKeyword = Text(moduleFilterEdit_);
        status << L"状态：显示 " << currentRows_.size() << L" / " << meaningfulRowCount;
        if (!keyword.empty()) {
            status << L"，过滤=" << keyword;
        }
        if (!targetKeyword.empty()) {
            status << L"，目标过滤=" << targetKeyword;
        }
        break;
    }
    case KernelFeatureId::DeviceDriverObjects:
    {
        const std::size_t loadedRows = meaningfulRowCount;
        const std::size_t visibleRows = currentRows_.size();
        if (loadedRows == 0) {
            status << L"状态：暂无结果，点击刷新开始枚举对象目录。";
        } else if (visibleRows == 0) {
            status << L"状态：已加载 " << loadedRows << L" 条，当前过滤后无可见结果。";
        } else {
            status << L"状态：已加载 " << loadedRows << L" 条，当前显示 " << visibleRows << L" 条。";
        }
        break;
    }
    case KernelFeatureId::ObjectTypeMatrix:
    {
        const std::wstring keyword = Text(filterEdit_);
        status << L"状态：已加载 " << meaningfulRowCount << L" 类，显示 " << currentRows_.size() << L" 类";
        if (!keyword.empty()) {
            status << L"，过滤=" << keyword;
        }
        break;
    }
    case KernelFeatureId::CommunicationEndpoint:
    {
        const std::wstring keyword = Text(filterEdit_);
        status << L"状态：已加载 " << meaningfulRowCount << L" 项，显示 " << currentRows_.size() << L" 项";
        if (!keyword.empty()) {
            status << L"，过滤=" << keyword;
        }
        break;
    }
    default:
        status << L"状态：对象页已刷新，显示 " << currentRows_.size() << L" 项";
        break;
    }
    WriteText(statusText_, status.str().c_str());
    if (!currentRows_.empty()) {
        SetSelection(resultList_, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        EnsureItem(resultList_, 0, FALSE);
        SelectionDetail();
    } else {
        WriteText(detailEdit_, EmptyDetailTextForFeature(featureId));
    }
}

void KernelValueProjectionBuilder::RebuildObjectNamespaceTreeFromCurrentRows() {
    // RebuildObjectNamespaceTreeFromCurrentRows renders the original object
    // namespace overview as a three-level Win32 TreeView. Inputs are
    // currentRows_ and currentColumns_ already projected to the original
    // root/directory/object schema; processing creates stable node payloads in
    // objectNamespaceTreeNodeStorage_ and binds them to TVITEM::lParam; no value
    // is returned.
    result_->tree=true;result_->treeNodes.clear();
    std::unordered_map<KernelObjectNamespaceTreeNodeState*,std::shared_ptr<KernelObjectNamespaceTreeNodeState>> owners;
    std::unordered_map<std::wstring, std::size_t> rootItems;
    std::unordered_map<std::wstring, std::size_t> directoryItems;
    std::size_t firstItem = 0;
    std::size_t firstEntryItem = 0;
    const bool expandAll = !Text(filterEdit_).empty();
    const auto rowText = [&](const std::vector<std::wstring>& row, std::initializer_list<const wchar_t*> names) {
        return ReadField(row, currentColumns_, names);
    };
    const auto safeText = [](const std::wstring& value, const wchar_t* fallback = L"<空>") -> std::wstring {
        return value.empty() ? std::wstring(fallback) : value;
    };
    const auto rootFromPath = [](const std::wstring& path) -> std::wstring {
        if (path.empty()) {
            return L"\\";
        }
        if (path == L"\\") {
            return path;
        }
        const std::size_t nextSlash = path.find(L'\\', 1);
        return nextSlash == std::wstring::npos ? path : path.substr(0, nextSlash);
    };
    const auto makeState = [&](const int rowIndex,
        std::wstring kind,
        std::wstring name,
        std::wstring type,
        std::wstring path,
        std::wstring description) -> KernelObjectNamespaceTreeNodeState* {
        auto state = std::make_shared<KernelObjectNamespaceTreeNodeState>();
        state->rowIndex = rowIndex;
        state->nodeKind = std::move(kind);
        state->nodeName = std::move(name);
        state->nodeType = std::move(type);
        state->nodePath = std::move(path);
        state->nodeDescription = std::move(description);
        KernelObjectNamespaceTreeNodeState* raw = state.get();
        owners.emplace(raw,std::move(state));
        return raw;
    };
    const auto insertTreeItem = [&](std::size_t parent,
        const std::wstring& text,
        KernelObjectNamespaceTreeNodeState* state) -> std::size_t {
        if (text.empty()) {
            return 0;
        }
        result_->treeNodes.push_back(KernelProjectedTreeNode{parent,text,owners.at(state),false});
        return result_->treeNodes.size();
    };

    const auto ensureRoot = [&](std::wstring rootPath, const std::wstring& description) -> std::size_t {
        if (rootPath.empty()) {
            rootPath = L"\\";
        }
        const std::wstring key = LowerInvariantKey(rootPath);
        if (const auto found = rootItems.find(key); found != rootItems.end()) {
            return found->second;
        }
        std::wstring text = rootPath + L"    [根目录]    " + safeText(description, L"<无>");
        KernelObjectNamespaceTreeNodeState* state = makeState(
            -1,
            L"Root",
            rootPath,
            L"根目录",
            rootPath,
            description);
        std::size_t item = insertTreeItem(0, text, state);
        if (item != 0) {
            rootItems.emplace(key, item);
            if (firstItem == 0) {
                firstItem = item;
            }
            ExpandTreeValue(item);
        }
        return item;
    };
    const auto ensureDirectory = [&](std::wstring rootPath,
        std::wstring directoryPath,
        const std::wstring& description) -> std::size_t {
        if (directoryPath.empty()) {
            directoryPath = rootPath.empty() ? L"\\" : rootPath;
        }
        if (rootPath.empty() || rootPath[0] != L'\\') {
            rootPath = rootFromPath(directoryPath);
        }
        std::size_t rootItem = ensureRoot(rootPath, description);
        const std::wstring key = LowerInvariantKey(rootPath + L"\n" + directoryPath);
        if (const auto found = directoryItems.find(key); found != directoryItems.end()) {
            return found->second;
        }
        std::wstring text = LeafObjectName(directoryPath) + L"    [目录]    " + directoryPath;
        KernelObjectNamespaceTreeNodeState* state = makeState(
            -1,
            L"Directory",
            LeafObjectName(directoryPath),
            L"目录",
            directoryPath,
            description);
        std::size_t item = insertTreeItem(rootItem, text, state);
        if (item != 0) {
            directoryItems.emplace(key, item);
            if (firstItem == 0) {
                firstItem = item;
            }
            if (rootItem != 0) {
                ExpandTreeValue(rootItem);
            }
            if (expandAll) {
                ExpandTreeValue(item);
            }
        }
        return item;
    };

    for (std::size_t rowIndex = 0; rowIndex < currentRows_.size(); ++rowIndex) {
        const std::vector<std::wstring>& row = currentRows_[rowIndex];
        std::wstring nodeKind = rowText(row, { L"NodeKind" });
        const std::wstring name = rowText(row, { L"名称", L"Name", L"objectName" });
        const std::wstring type = rowText(row, { L"类型", L"Type", L"objectType" });
        const std::wstring path = rowText(row, { L"Path", L"完整路径", L"fullPath", L"路径/说明" });
        const std::wstring directory = rowText(row, { L"Directory", L"directoryPath", L"Parent", L"sourceDirectory", L"来源目录" });
        std::wstring root = rowText(row, { L"rootPathText", L"Root", L"Source" });
        const std::wstring description = rowText(row, { L"scopeDescriptionText", L"Scope", L"scope", L"Detail", L"路径/说明" });
        const std::wstring status = rowText(row, { L"状态", L"Status", L"statusText" });
        const std::wstring target = rowText(row, { L"符号链接目标", L"Target", L"symbolicTarget", L"targetPath" });
        if (name.empty() && path.empty()) {
            continue;
        }

        if (nodeKind.empty()) {
            if (_wcsicmp(type.c_str(), L"根目录") == 0 || _wcsicmp(status.c_str(), L"根节点") == 0) {
                nodeKind = L"Root";
            } else if (_wcsicmp(type.c_str(), L"目录") == 0 || _wcsicmp(status.c_str(), L"已展开枚举") == 0) {
                nodeKind = L"Directory";
            } else {
                nodeKind = L"ObjectEntry";
            }
        }

        if (_wcsicmp(nodeKind.c_str(), L"Root") == 0) {
            ensureRoot(path.empty() ? name : path, description);
            continue;
        }
        if (_wcsicmp(nodeKind.c_str(), L"Directory") == 0) {
            const std::wstring directoryPath = path.empty() ? directory : path;
            if (root.empty() || root[0] != L'\\') {
                root = rootFromPath(directoryPath);
            }
            ensureDirectory(root, directoryPath, description);
            continue;
        }

        std::wstring directoryPath = !directory.empty() ? directory : ParentObjectPath(path);
        if (root.empty() || root[0] != L'\\') {
            root = rootFromPath(!directoryPath.empty() ? directoryPath : path);
        }
        std::size_t directoryItem = ensureDirectory(root, directoryPath, description);
        std::wstring displayText = safeText(name, L"<未命名对象>");
        displayText += L"    [" + safeText(type) + L"]    " + safeText(path);
        if (!target.empty() && target != L"<无>") {
            displayText += L"    -> " + target;
        } else if (!status.empty()) {
            displayText += L"    " + status;
        }
        KernelObjectNamespaceTreeNodeState* state = makeState(
            static_cast<int>(rowIndex),
            L"ObjectEntry",
            name.empty() ? L"<未命名对象>" : name,
            type,
            path,
            description);
        const std::size_t item = insertTreeItem(directoryItem, displayText, state);
        if (item == 0) {
            continue;
        }
        if (firstItem == 0) {
            firstItem = item;
        }
        if (firstEntryItem == 0) {
            firstEntryItem = item;
        }
        if (expandAll && directoryItem != 0) {
            ExpandTreeValue(directoryItem);
        }
    }

    if (firstEntryItem != 0) {
        SelectTreeValue( firstEntryItem);
    } else if (firstItem != 0) {
        SelectTreeValue( firstItem);
    }
}

} // namespace Ksword::Features::Kernel
