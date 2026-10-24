#include "PrivilegeView.h"

#include "PrivilegeActions.h"
#include "PrivilegeEnumerator.h"
#include "PrivilegeModel.h"
#include "../../Ui/AsyncTask.h"
#include "../../Ui/Controls.h"
#include "../../Ui/ExportUtil.h"
#include "../../Ui/FilterBar.h"
#include "../../Ui/ListViewUtil.h"
#include "../../Ui/LoadingOverlay.h"
#include "../../Ui/TableCompareActions.h"
#include "../../Ui/Theme.h"
#include "../../Ui/VirtualListView.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace Ksword::Features::Privilege {
namespace {

constexpr wchar_t kPrivilegeViewClass[] = L"KswordARKLight.Privilege.FeatureView";

constexpr int kRefreshButtonId = 65001;
constexpr int kEnableButtonId = 65002;
constexpr int kDisableButtonId = 65003;
constexpr int kFilterBarId = 65004;
constexpr int kPrivilegeListId = 65005;
constexpr int kDetailListId = 65006;
constexpr int kLoadingOverlayId = 65007;

constexpr UINT kMenuEnable = 65601;
constexpr UINT kMenuDisable = 65602;
constexpr UINT kMenuCopyRow = 65603;
constexpr UINT kMenuCopyVisible = 65604;
constexpr UINT kMenuCopyDetail = 65605;
constexpr UINT kMenuRefresh = 65606;
constexpr UINT kMenuExportVisible = 65607;
constexpr UINT kMenuExportAll = 65608;
constexpr UINT kMenuExportDetail = 65609;

constexpr UINT kMsgRefreshCompleted = WM_APP + 650;
constexpr UINT kMsgFilterCompleted = WM_APP + 651;
constexpr UINT kMsgActionCompleted = WM_APP + 652;

constexpr int kGap = 6;
constexpr int kRowHeight = 24;
constexpr int kHeaderHeight = kGap * 2 + kRowHeight;
constexpr int kDetailHeight = 220;
constexpr int kStatusHeight = 22;
constexpr int kColumnCount = 4;

int Width(const RECT& rc) {
    return rc.right > rc.left ? static_cast<int>(rc.right - rc.left) : 0;
}

int Height(const RECT& rc) {
    return rc.bottom > rc.top ? static_cast<int>(rc.bottom - rc.top) : 0;
}

struct PrivilegeFilterResult final {
    std::uint64_t generation = 0;
    std::wstring query;
    bool useRegex = false;
    std::wstring selectedStableKey;
    std::vector<std::size_t> visibleIndexes;
};

struct PrivilegeActionTaskResult final {
    PrivilegeActionResult action;
    bool refreshRequired = false;
};

// PrivilegeColumnTitles is read by the header and by the TSV export, so a saved
// privilege inventory always carries the captions it was read from.
const std::vector<std::wstring>& PrivilegeColumnTitles() {
    static const std::vector<std::wstring> titles{
        L"权限名", L"显示名", L"状态", L"风险"
    };
    return titles;
}

struct PrivilegeViewState final {
    HWND hwnd = nullptr;
    HWND refreshButton = nullptr;
    HWND enableButton = nullptr;
    HWND disableButton = nullptr;
    HWND filterBar = nullptr;
    HWND detailList = nullptr;
    HWND loadingOverlay = nullptr;
    Ksword::Ui::VirtualListView privilegeList;
    PrivilegeModel model;
    std::shared_ptr<const std::vector<Ksword::Ui::VirtualListRow>> filterRows;
    std::wstring statusText = L"正在读取当前进程令牌…";
    // 令牌权限会被本页动作、组策略或调试器在两次刷新之间改变；列表只说明"现在是什么"，
    // 比对才能指出这次多给了哪一项（提权痕迹往往就藏在这个差集里）。
    Ksword::Ui::TableSnapshotEngine snapshotEngine;
    std::wstring filterQuery;
    bool filterUseRegex = false;
    std::uint64_t displayGeneration = 0;
    bool actionInProgress = false;
    std::unique_ptr<Ksword::Ui::AsyncSnapshotTask<PrivilegeSnapshot>> refreshTask;
    std::unique_ptr<Ksword::Ui::AsyncSnapshotTask<PrivilegeFilterResult>> filterTask;
    std::unique_ptr<Ksword::Ui::AsyncSnapshotTask<PrivilegeActionTaskResult>> actionTask;
};

void AddColumn(HWND list, int index, const wchar_t* title, int width) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.pszText = const_cast<LPWSTR>(title);
    column.cx = width;
    column.iSubItem = index;
    ListView_InsertColumn(list, index, &column);
}

void SetDetailText(HWND list, int row, int column, const std::wstring& text) {
    if (column == 0) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = row;
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(text.c_str());
        ListView_InsertItem(list, &item);
        return;
    }
    ListView_SetItemText(list, row, column, const_cast<LPWSTR>(text.c_str()));
}

// CopyText forwards to the shared clipboard writer so a copied privilege row is
// recorded in the evidence session like every other page's copy.
bool CopyText(HWND owner, const std::wstring& text) {
    return Ksword::Ui::CopyTextToClipboard(owner, text, L"令牌权限导出");
}

int SelectedModelIndex(const PrivilegeViewState& state) {
    const HWND list = state.privilegeList.hwnd();
    const int selected = list ? ListView_GetNextItem(list, -1, LVNI_SELECTED) : -1;
    const auto& visible = state.privilegeList.visibleIndexes();
    if (selected < 0 || static_cast<std::size_t>(selected) >= visible.size()) {
        return -1;
    }
    const std::size_t modelIndex = visible[static_cast<std::size_t>(selected)];
    return modelIndex <= static_cast<std::size_t>(INT_MAX) ? static_cast<int>(modelIndex) : -1;
}

const PrivilegeEntry* SelectedEntry(const PrivilegeViewState& state) {
    return state.model.entryAt(SelectedModelIndex(state));
}

std::wstring StableKeyFromListItem(const PrivilegeViewState& state, int item) {
    const auto& visible = state.privilegeList.visibleIndexes();
    const auto& rows = state.privilegeList.rows();
    if (item < 0 || static_cast<std::size_t>(item) >= visible.size()) {
        return {};
    }
    const std::size_t sourceIndex = visible[static_cast<std::size_t>(item)];
    return sourceIndex < rows.size() ? rows[sourceIndex].stableKey : std::wstring{};
}

// ShowDetail falls back to the token summary when no row is selected, so the
// pane always carries the context the privilege list is read against instead of
// sitting empty.
void ShowDetail(PrivilegeViewState& state, int modelIndex) {
    if (!state.detailList) {
        return;
    }
    ListView_DeleteAllItems(state.detailList);
    const PrivilegeEntry* entry = state.model.entryAt(modelIndex);
    const std::vector<PrivilegeProperty> properties =
        entry != nullptr ? state.model.propertiesForEntry(*entry) : state.model.tokenProperties();
    for (int row = 0; row < static_cast<int>(properties.size()); ++row) {
        SetDetailText(state.detailList, row, 0, properties[static_cast<std::size_t>(row)].name);
        SetDetailText(state.detailList, row, 1, properties[static_cast<std::size_t>(row)].value);
    }
}

void UpdateActionButtons(PrivilegeViewState& state) {
    const PrivilegeEntry* entry = state.actionInProgress ? nullptr : SelectedEntry(state);
    // A removed privilege is gone for this token's lifetime; neither direction
    // can bring it back, and the SCM-style "it worked" lie is exactly what this
    // page is meant to avoid.
    const bool adjustable = entry != nullptr && !entry->removed;
    if (state.enableButton) {
        ::EnableWindow(state.enableButton, adjustable && !entry->enabled);
    }
    if (state.disableButton) {
        ::EnableWindow(state.disableButton, adjustable && entry->enabled);
    }
}

void ApplyPrivilegeFilter(PrivilegeViewState& state, PrivilegeFilterResult result) {
    if (result.generation != state.displayGeneration || result.query != state.filterQuery ||
        result.useRegex != state.filterUseRegex || !state.privilegeList.hwnd()) {
        return;
    }
    state.privilegeList.setVisibleIndexes(std::move(result.visibleIndexes));
    const auto& visible = state.privilegeList.visibleIndexes();
    const auto& rows = state.privilegeList.rows();
    int selectedItem = -1;
    for (std::size_t item = 0; item < visible.size(); ++item) {
        const std::size_t sourceIndex = visible[item];
        if (sourceIndex < rows.size() && rows[sourceIndex].stableKey == result.selectedStableKey) {
            selectedItem = static_cast<int>(item);
            break;
        }
    }
    HWND list = state.privilegeList.hwnd();
    ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    if (selectedItem >= 0) {
        ListView_SetItemState(list, selectedItem, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list, selectedItem, FALSE);
    }
    ShowDetail(state, SelectedModelIndex(state));
    UpdateActionButtons(state);
    if (!result.query.empty()) {
        state.statusText = L"筛选结果 " + std::to_wstring(visible.size()) + L" / " +
            std::to_wstring(rows.size()) + L" 项。";
    }
    ::InvalidateRect(state.hwnd, nullptr, TRUE);
}

void RequestPrivilegeFilter(PrivilegeViewState& state, std::wstring query, std::wstring selectedStableKey) {
    state.filterQuery = std::move(query);
    state.filterUseRegex = Ksword::Ui::GetFilterBarRegexEnabled(state.filterBar);
    const auto rows = state.filterRows;
    const std::uint64_t generation = state.displayGeneration;
    const bool useRegex = state.filterUseRegex;
    if (!state.filterTask || !rows) {
        return;
    }
    state.filterTask->request(
        [rows, generation, useRegex, query = state.filterQuery, selectedStableKey = std::move(selectedStableKey)]() mutable {
            PrivilegeFilterResult result{};
            result.generation = generation;
            result.query = std::move(query);
            result.useRegex = useRegex;
            result.selectedStableKey = std::move(selectedStableKey);
            result.visibleIndexes = Ksword::Ui::VirtualListView::FilterRowIndexes(*rows, result.query, useRegex);
            return result;
        },
        [&state](std::uint64_t, std::optional<PrivilegeFilterResult>&& result, std::exception_ptr error) {
            if (error || !result.has_value()) {
                state.statusText = L"权限筛选任务异常结束，已保留当前结果。";
                ::InvalidateRect(state.hwnd, nullptr, TRUE);
                return;
            }
            ApplyPrivilegeFilter(state, std::move(*result));
        });
}

void BuildRows(PrivilegeViewState& state) {
    std::vector<Ksword::Ui::VirtualListRow> rows;
    const auto& privileges = state.model.privileges();
    rows.reserve(privileges.size());
    for (std::size_t index = 0; index < privileges.size(); ++index) {
        const PrivilegeEntry& entry = privileges[index];
        Ksword::Ui::VirtualListRow row{};
        row.stableKey = entry.name;
        row.itemData = static_cast<LPARAM>(index);
        row.cells.reserve(kColumnCount + 1);
        for (int column = 0; column < kColumnCount; ++column) {
            row.cells.push_back(state.model.textForColumn(entry, column));
        }
        // The description joins the filter text so searching for what a
        // privilege does works, not only for its constant name.
        row.cells.push_back(entry.description);
        rows.push_back(std::move(row));
    }
    auto filterRows = std::make_shared<std::vector<Ksword::Ui::VirtualListRow>>(std::move(rows));
    state.privilegeList.setSharedRows(filterRows);
    state.filterRows = std::move(filterRows);
    ++state.displayGeneration;
}

void BeginPrivilegeRefresh(PrivilegeViewState& state) {
    if (!state.refreshTask) {
        return;
    }
    const bool firstLoad = state.privilegeList.rows().empty();
    state.statusText = L"正在后台读取令牌权限…";
    if (state.refreshButton) {
        ::EnableWindow(state.refreshButton, FALSE);
    }
    if (firstLoad) {
        Ksword::Ui::SetLoadingOverlay(state.loadingOverlay, true, L"正在读取令牌…");
    }
    ::InvalidateRect(state.hwnd, nullptr, TRUE);
    state.refreshTask->request(
        [] { return EnumerateProcessPrivileges(); },
        [&state](std::uint64_t, std::optional<PrivilegeSnapshot>&& snapshot, std::exception_ptr error) {
            if (state.refreshButton) {
                ::EnableWindow(state.refreshButton, TRUE);
            }
            Ksword::Ui::SetLoadingOverlay(state.loadingOverlay, false);
            if (error || !snapshot.has_value()) {
                state.statusText = L"令牌权限读取异常结束。";
                ::InvalidateRect(state.hwnd, nullptr, TRUE);
                return;
            }
            if (!snapshot->success) {
                state.statusText = snapshot->diagnosticText.empty() ? L"令牌权限读取失败。" : snapshot->diagnosticText;
                ::InvalidateRect(state.hwnd, nullptr, TRUE);
                return;
            }
            const std::wstring selectedStableKey =
                StableKeyFromListItem(state, ListView_GetNextItem(state.privilegeList.hwnd(), -1, LVNI_SELECTED));
            const std::size_t total = snapshot->privileges.size();
            std::size_t enabled = 0;
            std::size_t risky = 0;
            for (const PrivilegeEntry& entry : snapshot->privileges) {
                if (entry.enabled) {
                    ++enabled;
                }
                if (entry.enabled && !entry.riskText.empty()) {
                    ++risky;
                }
            }
            const bool elevated = snapshot->token.elevated;
            const std::wstring integrity = snapshot->token.integrityLevel;
            state.model.setSnapshot(std::move(*snapshot));
            BuildRows(state);
            state.statusText = L"令牌权限 " + std::to_wstring(total) + L" 项，已启用 " + std::to_wstring(enabled) +
                L"，其中越权类 " + std::to_wstring(risky) + L"；完整性 " + (integrity.empty() ? L"未知" : integrity) +
                L"，" + (elevated ? L"已提升。" : L"未提升。");
            RequestPrivilegeFilter(state,
                state.filterBar ? Ksword::Ui::GetFilterBarText(state.filterBar) : state.filterQuery,
                selectedStableKey);
            ::InvalidateRect(state.hwnd, nullptr, TRUE);
        });
}

void RunPrivilegeAction(PrivilegeViewState& state, bool enable) {
    const PrivilegeEntry* selected = SelectedEntry(state);
    if (!selected) {
        state.statusText = L"未选择权限。";
        ::InvalidateRect(state.hwnd, nullptr, TRUE);
        return;
    }
    if (state.actionInProgress || !state.actionTask) {
        state.statusText = L"权限操作正在执行。";
        ::InvalidateRect(state.hwnd, nullptr, TRUE);
        return;
    }

    const std::wstring privilegeName = selected->name;
    state.actionInProgress = true;
    UpdateActionButtons(state);
    state.statusText = (enable ? L"正在启用 " : L"正在禁用 ") + privilegeName + L"…";
    ::InvalidateRect(state.hwnd, nullptr, TRUE);
    state.actionTask->request(
        [privilegeName, enable] {
            PrivilegeActionTaskResult result{};
            result.action = SetPrivilegeEnabled(privilegeName, enable);
            result.refreshRequired = result.action.success;
            return result;
        },
        [&state](std::uint64_t, std::optional<PrivilegeActionTaskResult>&& result, std::exception_ptr error) {
            state.actionInProgress = false;
            if (error || !result.has_value()) {
                state.statusText = L"权限操作异常结束。";
                UpdateActionButtons(state);
                ::InvalidateRect(state.hwnd, nullptr, TRUE);
                return;
            }
            state.statusText = result->action.message;
            if (result->refreshRequired) {
                BeginPrivilegeRefresh(state);
                return;
            }
            UpdateActionButtons(state);
            ::InvalidateRect(state.hwnd, nullptr, TRUE);
        });
}

std::wstring RowsAsText(const PrivilegeViewState& state, bool visibleRows) {
    const auto& rows = state.privilegeList.rows();
    const auto& visible = state.privilegeList.visibleIndexes();
    const HWND list = state.privilegeList.hwnd();
    std::wstring text;
    for (std::size_t item = 0; item < visible.size(); ++item) {
        if (!visibleRows &&
            (!list || (ListView_GetItemState(list, static_cast<int>(item), LVIS_SELECTED) & LVIS_SELECTED) == 0)) {
            continue;
        }
        const std::size_t rowIndex = visible[item];
        if (rowIndex >= rows.size()) {
            continue;
        }
        const auto& cells = rows[rowIndex].cells;
        for (std::size_t column = 0; column < (std::min)(static_cast<std::size_t>(kColumnCount), cells.size()); ++column) {
            if (column != 0) {
                text += L'\t';
            }
            text += cells[column];
        }
        text += L"\r\n";
    }
    return text;
}

std::wstring DetailAsText(const PrivilegeViewState& state) {
    const PrivilegeEntry* entry = SelectedEntry(state);
    const std::vector<PrivilegeProperty> properties =
        entry != nullptr ? state.model.propertiesForEntry(*entry) : state.model.tokenProperties();
    std::wstring text;
    for (const PrivilegeProperty& property : properties) {
        text += property.name + L"\t" + property.value + L"\r\n";
    }
    return text;
}

// ExportPrivilegeList writes one row scope of the inventory as TSV. "Visible"
// follows the active filter; "all" keeps the whole snapshot, which is what an
// evidence record needs when the operator has narrowed the table.
void ExportPrivilegeList(PrivilegeViewState& state, const Ksword::Ui::ExportRowScope scope) {
    Ksword::Ui::ExportRowCount count{};
    std::wstring error;
    const Ksword::Ui::SaveTextFileResult result = Ksword::Ui::ExportVirtualListTsvWithDialog(
        state.hwnd,
        state.privilegeList,
        PrivilegeColumnTitles(),
        scope,
        L"privileges.tsv",
        scope == Ksword::Ui::ExportRowScope::Visible
            ? L"导出令牌权限（可见项）"
            : L"导出令牌权限（全部项）",
        L"令牌权限导出",
        &count,
        &error);
    if (result == Ksword::Ui::SaveTextFileResult::Failed) {
        state.statusText = error.empty() ? L"令牌权限导出失败。" : L"令牌权限导出失败：" + error;
    } else if (result == Ksword::Ui::SaveTextFileResult::Saved) {
        std::wostringstream summary;
        summary << L"已导出 " << count.exportedRows << L" / " << count.totalRows << L" 项令牌权限";
        if (count.filteredRows) {
            summary << L"（筛选隐藏的 " << (count.totalRows - count.exportedRows) << L" 项未写入）";
        }
        summary << L"。";
        state.statusText = summary.str();
    }
    ::InvalidateRect(state.hwnd, nullptr, TRUE);
}

// The detail pane is a reading layout, so it is saved as "属性：值" text rather
// than flattened into two TSV columns that only make it harder to read.
void ExportPrivilegeDetail(PrivilegeViewState& state) {
    const PrivilegeEntry* entry = SelectedEntry(state);
    const std::vector<PrivilegeProperty> properties =
        entry != nullptr ? state.model.propertiesForEntry(*entry) : state.model.tokenProperties();
    std::wstring text;
    for (const PrivilegeProperty& property : properties) {
        text += property.name + L"：" + property.value + L"\r\n";
    }
    if (text.empty()) {
        state.statusText = L"当前没有可导出的令牌权限详情。";
        ::InvalidateRect(state.hwnd, nullptr, TRUE);
        return;
    }
    std::wstring error;
    const Ksword::Ui::SaveTextFileResult result = Ksword::Ui::SaveUtf8TextFileWithDialog(
        state.hwnd,
        L"privilege_detail.txt",
        L"导出令牌权限详情",
        L"文本文件 (*.txt)\0*.txt\0所有文件 (*.*)\0*.*\0",
        L"txt",
        text,
        &error);
    if (result == Ksword::Ui::SaveTextFileResult::Failed) {
        state.statusText = error.empty() ? L"令牌权限详情导出失败。" : L"令牌权限详情导出失败：" + error;
    } else if (result == Ksword::Ui::SaveTextFileResult::Saved) {
        state.statusText = L"已导出令牌权限详情。";
    }
    ::InvalidateRect(state.hwnd, nullptr, TRUE);
}

void ShowPrivilegeContextMenu(PrivilegeViewState& state, POINT screenPoint) {
    const PrivilegeEntry* entry = SelectedEntry(state);
    HMENU menu = ::CreatePopupMenu();
    if (!menu) {
        return;
    }
    const bool adjustable = entry != nullptr && !entry->removed && !state.actionInProgress;
    ::AppendMenuW(menu, MF_STRING | ((adjustable && !entry->enabled) ? MF_ENABLED : MF_GRAYED), kMenuEnable, L"启用");
    ::AppendMenuW(menu, MF_STRING | ((adjustable && entry->enabled) ? MF_ENABLED : MF_GRAYED), kMenuDisable, L"禁用");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING | (entry ? MF_ENABLED : MF_GRAYED), kMenuCopyRow, L"复制选中行");
    ::AppendMenuW(menu, MF_STRING, kMenuCopyVisible, L"复制可见行");
    ::AppendMenuW(menu, MF_STRING, kMenuCopyDetail, L"复制详情");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kMenuExportVisible, L"导出可见令牌权限 TSV…");
    ::AppendMenuW(menu, MF_STRING, kMenuExportAll, L"导出全部令牌权限 TSV…");
    ::AppendMenuW(menu, MF_STRING, kMenuExportDetail, L"导出令牌权限详情 TXT…");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kMenuRefresh, L"刷新");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    Ksword::Ui::AppendTableCompareMenuItems(menu, state.snapshotEngine);

    const int command = ::TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, 0, state.hwnd, nullptr);
    ::DestroyMenu(menu);

    switch (static_cast<UINT>(command)) {
    case kMenuEnable:
        RunPrivilegeAction(state, true);
        break;
    case kMenuDisable:
        RunPrivilegeAction(state, false);
        break;
    case kMenuCopyRow:
        state.statusText = CopyText(state.hwnd, RowsAsText(state, false)) ? L"已复制选中行。" : L"复制失败。";
        ::InvalidateRect(state.hwnd, nullptr, TRUE);
        break;
    case kMenuCopyVisible:
        state.statusText = CopyText(state.hwnd, RowsAsText(state, true)) ? L"已复制可见行。" : L"复制失败。";
        ::InvalidateRect(state.hwnd, nullptr, TRUE);
        break;
    case kMenuCopyDetail:
        state.statusText = CopyText(state.hwnd, DetailAsText(state)) ? L"已复制详情。" : L"复制失败。";
        ::InvalidateRect(state.hwnd, nullptr, TRUE);
        break;
    case kMenuExportVisible:
        ExportPrivilegeList(state, Ksword::Ui::ExportRowScope::Visible);
        break;
    case kMenuExportAll:
        ExportPrivilegeList(state, Ksword::Ui::ExportRowScope::All);
        break;
    case kMenuExportDetail:
        ExportPrivilegeDetail(state);
        break;
    case kMenuRefresh:
        BeginPrivilegeRefresh(state);
        break;
    default:
        if (Ksword::Ui::HandleTableCompareCommand(state.snapshotEngine, static_cast<UINT>(command),
                state.privilegeList, PrivilegeColumnTitles(), state.statusText, L"令牌权限")) {
            ::InvalidateRect(state.hwnd, nullptr, TRUE);
        }
        break;
    }
}

void LayoutView(PrivilegeViewState& state) {
    RECT client{};
    ::GetClientRect(state.hwnd, &client);
    const int width = Width(client);
    const int height = Height(client);

    int cursorX = kGap;
    const auto place = [&cursorX](HWND control, int controlWidth) {
        if (control) {
            ::MoveWindow(control, cursorX, kGap, controlWidth, kRowHeight, TRUE);
        }
        cursorX += controlWidth + kGap;
    };
    place(state.refreshButton, 64);
    place(state.enableButton, 64);
    place(state.disableButton, 64);
    if (state.filterBar) {
        ::MoveWindow(state.filterBar, cursorX, kGap, (std::max)(120, width - cursorX - kGap), kRowHeight, TRUE);
    }

    const int listTop = kHeaderHeight;
    const int detailTop = (std::max)(listTop, height - kStatusHeight - kDetailHeight);
    const int listHeight = (std::max)(0, detailTop - listTop - kGap);
    if (HWND list = state.privilegeList.hwnd()) {
        ::MoveWindow(list, kGap, listTop, (std::max)(0, width - kGap * 2), listHeight, TRUE);
    }
    if (state.detailList) {
        ::MoveWindow(state.detailList, kGap, detailTop, (std::max)(0, width - kGap * 2),
            (std::max)(0, height - kStatusHeight - detailTop - kGap), TRUE);
    }
    if (state.loadingOverlay) {
        ::MoveWindow(state.loadingOverlay, kGap, listTop, (std::max)(0, width - kGap * 2), listHeight, TRUE);
    }
}

bool CreateChildControls(PrivilegeViewState& state) {
    HWND hwnd = state.hwnd;
    state.refreshButton = Ksword::Ui::CreateButton(hwnd, kRefreshButtonId, L"刷新", 0, 0, 0, 0);
    state.enableButton = Ksword::Ui::CreateButton(hwnd, kEnableButtonId, L"启用", 0, 0, 0, 0);
    state.disableButton = Ksword::Ui::CreateButton(hwnd, kDisableButtonId, L"禁用", 0, 0, 0, 0);
    state.filterBar = Ksword::Ui::CreateFilterBar(hwnd, kFilterBarId, L"筛选权限名、显示名、状态与说明", 0, 0, 0, 0);

    if (!state.privilegeList.create(hwnd, kPrivilegeListId, 0, 0, 1, 1, LVS_SHOWSELALWAYS | LVS_SINGLESEL)) {
        return false;
    }
    {
        const std::vector<std::wstring>& titles = PrivilegeColumnTitles();
        static const int widths[] = { 280, 260, 120, 180 };
        std::vector<Ksword::Ui::ListViewColumn> columns;
        for (std::size_t index = 0; index < titles.size(); ++index) {
            columns.push_back({ static_cast<int>(index), widths[index], LVCFMT_LEFT, titles[index] });
        }
        state.privilegeList.addColumns(columns);
    }
    if (HWND list = state.privilegeList.hwnd()) {
        ListView_SetExtendedListViewStyle(list,
            LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES | LVS_EX_LABELTIP);
        ::SendMessageW(list, WM_SETFONT, reinterpret_cast<WPARAM>(Ksword::Ui::SystemUIFont()), TRUE);
    }

    state.detailList = Ksword::Ui::CreateReportListView(hwnd, kDetailListId, 0, 0, 1, 1, LVS_SINGLESEL);
    if (state.detailList) {
        AddColumn(state.detailList, 0, L"属性", 160);
        AddColumn(state.detailList, 1, L"值", 760);
        ListView_SetExtendedListViewStyle(state.detailList,
            LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES | LVS_EX_LABELTIP);
    }

    state.loadingOverlay = Ksword::Ui::CreateLoadingOverlay(hwnd, kLoadingOverlayId, { 0, 0, 1, 1 });
    if (!state.refreshButton || !state.enableButton || !state.disableButton || !state.filterBar ||
        !state.detailList || !state.loadingOverlay) {
        return false;
    }
    Ksword::Ui::SetWindowFontRecursive(hwnd);
    return true;
}

LRESULT CALLBACK PrivilegeViewProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<PrivilegeViewState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCCREATE: {
        auto owned = std::make_unique<PrivilegeViewState>();
        owned->hwnd = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(owned.release()));
        return TRUE;
    }
    case WM_CREATE:
        if (state) {
            if (!CreateChildControls(*state)) {
                return -1;
            }
            state->refreshTask = std::make_unique<Ksword::Ui::AsyncSnapshotTask<PrivilegeSnapshot>>(hwnd, kMsgRefreshCompleted);
            state->filterTask = std::make_unique<Ksword::Ui::AsyncSnapshotTask<PrivilegeFilterResult>>(hwnd, kMsgFilterCompleted);
            state->actionTask = std::make_unique<Ksword::Ui::AsyncSnapshotTask<PrivilegeActionTaskResult>>(hwnd, kMsgActionCompleted);
            LayoutView(*state);
            ShowDetail(*state, -1);
            UpdateActionButtons(*state);
            BeginPrivilegeRefresh(*state);
        }
        return 0;
    case WM_SIZE:
        if (state) {
            LayoutView(*state);
        }
        return 0;
    case WM_COMMAND:
        if (!state) {
            break;
        }
        {
            const int id = LOWORD(wParam);
            const int notification = HIWORD(wParam);
            if (id == kFilterBarId && notification == EN_CHANGE) {
                RequestPrivilegeFilter(*state, Ksword::Ui::GetFilterBarText(state->filterBar),
                    StableKeyFromListItem(*state, ListView_GetNextItem(state->privilegeList.hwnd(), -1, LVNI_SELECTED)));
                return 0;
            }
            if (notification == BN_CLICKED) {
                switch (id) {
                case kRefreshButtonId:
                    BeginPrivilegeRefresh(*state);
                    return 0;
                case kEnableButtonId:
                    RunPrivilegeAction(*state, true);
                    return 0;
                case kDisableButtonId:
                    RunPrivilegeAction(*state, false);
                    return 0;
                default:
                    break;
                }
            }
        }
        break;
    case WM_NOTIFY:
        if (state) {
            const auto* header = reinterpret_cast<const NMHDR*>(lParam);
            if (header) {
                LRESULT result = 0;
                if (state->privilegeList.handleNotify(*header, result)) {
                    return result;
                }
                if (header->hwndFrom == state->privilegeList.hwnd() && header->code == LVN_COLUMNCLICK) {
                    const auto* clicked = reinterpret_cast<const NMLISTVIEW*>(lParam);
                    if (clicked) {
                        const Ksword::Ui::SortSpec next =
                            Ksword::Ui::NextSortSpec(state->privilegeList.sortSpec(), clicked->iSubItem);
                        state->privilegeList.applySort(next);
                        Ksword::Ui::ShowSortIndicator(state->privilegeList.hwnd(), next);
                    }
                    return 0;
                }
                if (header->hwndFrom == state->privilegeList.hwnd() && header->code == LVN_ITEMCHANGED) {
                    const auto* changed = reinterpret_cast<const NMLISTVIEW*>(lParam);
                    if (changed && (changed->uNewState & LVIS_SELECTED) != 0) {
                        ShowDetail(*state, SelectedModelIndex(*state));
                        UpdateActionButtons(*state);
                    }
                    return 0;
                }
                if (header->hwndFrom == state->privilegeList.hwnd() && header->code == NM_RCLICK) {
                    POINT point{};
                    ::GetCursorPos(&point);
                    ShowPrivilegeContextMenu(*state, point);
                    return 0;
                }
            }
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        if (state) {
            PAINTSTRUCT paint{};
            HDC dc = ::BeginPaint(hwnd, &paint);
            RECT client{};
            ::GetClientRect(hwnd, &client);
            ::FillRect(dc, &client, Ksword::Ui::AppTheme().windowBrush());
            RECT statusRect{ kGap, client.bottom - kStatusHeight, client.right - kGap, client.bottom };
            Ksword::Ui::DrawTextLine(dc, state->statusText, statusRect,
                Ksword::Ui::AppTheme().mutedTextColor, Ksword::Ui::SystemUIFont(),
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            ::EndPaint(hwnd, &paint);
            return 0;
        }
        break;
    default:
        if (state) {
            if (msg == kMsgRefreshCompleted && state->refreshTask) {
                state->refreshTask->consume(hwnd, wParam, lParam);
                return 0;
            }
            if (msg == kMsgFilterCompleted && state->filterTask) {
                state->filterTask->consume(hwnd, wParam, lParam);
                return 0;
            }
            if (msg == kMsgActionCompleted && state->actionTask) {
                state->actionTask->consume(hwnd, wParam, lParam);
                return 0;
            }
        }
        if (msg == WM_NCDESTROY && state) {
            if (state->refreshTask) {
                state->refreshTask->cancel();
            }
            if (state->filterTask) {
                state->filterTask->cancel();
            }
            if (state->actionTask) {
                state->actionTask->cancel();
            }
            state->privilegeList.detach();
            delete state;
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool EnsurePrivilegeViewClass() {
    static bool registered = false;
    if (registered) {
        return true;
    }
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = PrivilegeViewProc;
    windowClass.hInstance = ::GetModuleHandleW(nullptr);
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = Ksword::Ui::AppTheme().windowBrush();
    windowClass.lpszClassName = kPrivilegeViewClass;
    registered = ::RegisterClassW(&windowClass) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return registered;
}

} // namespace

HWND CreatePrivilegeView(HWND parent, const RECT& bounds) {
    if (!parent || !EnsurePrivilegeViewClass()) {
        return nullptr;
    }
    return ::CreateWindowExW(
        0, kPrivilegeViewClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
        parent, nullptr, ::GetModuleHandleW(nullptr), nullptr);
}

} // namespace Ksword::Features::Privilege
