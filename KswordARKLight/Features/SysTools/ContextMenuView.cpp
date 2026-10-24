#include "ContextMenuView.h"

#include "ContextMenuScanner.h"
#include "../../Core/EntityRef.h"
#include "../File/PathNavigator.h"
#include "../../Ui/AsyncTask.h"
#include "../../Ui/Controls.h"
#include "../../Ui/EntityNavigation.h"
#include "../../Ui/ExportUtil.h"
#include "../../Ui/FilterBar.h"
#include "../../Ui/ListViewUtil.h"
#include "../../Ui/LoadingOverlay.h"
#include "../../Ui/TableCompareActions.h"
#include "../../Ui/TextFindSupport.h"
#include "../../Ui/Theme.h"
#include "../../Ui/VirtualListView.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace Ksword::Features::SysTools {
namespace {

constexpr wchar_t kContextMenuViewClass[] = L"KswordARKLight.SysTools.ContextMenuView";

constexpr int kRefreshButtonId = 67301;
constexpr int kDisableButtonId = 67302;
constexpr int kEnableButtonId = 67303;
constexpr int kFilterBarId = 67304;
constexpr int kListId = 67305;
constexpr int kDetailEditId = 67306;
constexpr int kLoadingOverlayId = 67307;

constexpr UINT kMenuDisable = 67621;
constexpr UINT kMenuEnable = 67622;
constexpr UINT kMenuCopyRow = 67623;
constexpr UINT kMenuCopyVisible = 67624;
constexpr UINT kMenuRefresh = 67625;
constexpr UINT kMenuOpenRegistry = 67626;
constexpr UINT kMenuOpenModuleDirectory = 67627;
constexpr UINT kMenuExportVisible = 67628;
constexpr UINT kMenuExportAll = 67629;
constexpr UINT kMenuExportDetail = 67630;

constexpr UINT kMsgScanCompleted = WM_APP + 710;
constexpr UINT kMsgFilterCompleted = WM_APP + 711;
constexpr UINT kMsgActionCompleted = WM_APP + 712;

constexpr int kGap = 6;
constexpr int kRowHeight = 24;
constexpr int kHeaderHeight = kGap * 3 + kRowHeight * 2;
constexpr int kDetailHeight = 120;
constexpr int kStatusHeight = 22;
constexpr int kColumnCount = 7;

int Width(const RECT& rc) {
    return rc.right > rc.left ? static_cast<int>(rc.right - rc.left) : 0;
}

int Height(const RECT& rc) {
    return rc.bottom > rc.top ? static_cast<int>(rc.bottom - rc.top) : 0;
}

struct ContextMenuFilterResult final {
    std::uint64_t generation = 0;
    std::wstring query;
    bool useRegex = false;
    std::vector<std::size_t> visibleIndexes;
};

struct ContextMenuViewState final {
    HWND hwnd = nullptr;
    HWND refreshButton = nullptr;
    HWND disableButton = nullptr;
    HWND enableButton = nullptr;
    HWND filterBar = nullptr;
    HWND detailEdit = nullptr;
    HWND loadingOverlay = nullptr;
    Ksword::Ui::VirtualListView list;
    std::vector<ContextMenuEntry> entries;
    std::shared_ptr<const std::vector<Ksword::Ui::VirtualListRow>> filterRows;
    std::wstring statusText = L"正在枚举 shell 扩展注册点…";
    std::wstring filterQuery;
    bool filterUseRegex = false;
    bool actionInProgress = false;
    std::uint64_t displayGeneration = 0;
    // 注册点会在安装/卸载 shell 扩展时增删，同名 CLSID 也可能被新模块接管，
    // 单看一次枚举分不清"本来就有"和"刚刚被换掉"，需要两份快照并排看。
    Ksword::Ui::TableSnapshotEngine snapshotEngine;
    std::unique_ptr<Ksword::Ui::AsyncSnapshotTask<ContextMenuScanResult>> scanTask;
    std::unique_ptr<Ksword::Ui::AsyncSnapshotTask<ContextMenuFilterResult>> filterTask;
    std::unique_ptr<Ksword::Ui::AsyncSnapshotTask<ContextMenuActionResult>> actionTask;
};

// ContextMenuColumnTitles feeds the header and the TSV export from one list, so
// an exported registration table names the columns the user actually saw.
const std::vector<std::wstring>& ContextMenuColumnTitles() {
    static const std::vector<std::wstring> titles{
        L"状态", L"作用域", L"类型", L"名称", L"CLSID", L"模块路径", L"模块"
    };
    return titles;
}

// CopyText forwards to the shared clipboard writer so a copied registration is
// recorded in the evidence session like every other page's copy.
bool CopyText(HWND owner, const std::wstring& text) {
    return Ksword::Ui::CopyTextToClipboard(owner, text, L"右键菜单导出");
}

std::wstring KindText(const ContextMenuKind kind) {
    return kind == ContextMenuKind::ShellVerb ? L"shell 动词" : L"shellex 处理程序";
}

// ModuleStateText separates the three states an operator reacts to differently:
// a working module, a registration pointing at a file that is gone (a classic
// leftover from an uninstalled product), and an entry that has no file to check.
std::wstring ModuleStateText(const ContextMenuEntry& entry) {
    if (entry.moduleFile.empty()) {
        return L"无模块";
    }
    return entry.moduleExists ? L"存在" : L"缺失";
}

std::wstring CellText(const ContextMenuEntry& entry, const int column) {
    switch (column) {
    case 0:
        return entry.enabled ? L"启用" : L"已禁用";
    case 1:
        return entry.scopeText;
    case 2:
        return KindText(entry.kind);
    case 3:
        return entry.name;
    case 4:
        return entry.clsid;
    case 5:
        return entry.modulePath;
    case 6:
        return ModuleStateText(entry);
    default:
        return {};
    }
}

COLORREF RowColor(const ContextMenuEntry& entry) {
    if (!entry.enabled) {
        return RGB(128, 128, 128);
    }
    // A registration whose module is missing is dead weight in every shell
    // menu it appears in, so it is the one state worth coloring.
    if (!entry.moduleFile.empty() && !entry.moduleExists) {
        return RGB(176, 32, 32);
    }
    return CLR_DEFAULT;
}

int SelectedModelIndex(const ContextMenuViewState& state) {
    const HWND list = state.list.hwnd();
    const int selected = list ? ListView_GetNextItem(list, -1, LVNI_SELECTED) : -1;
    const auto& visible = state.list.visibleIndexes();
    if (selected < 0 || static_cast<std::size_t>(selected) >= visible.size()) {
        return -1;
    }
    const std::size_t modelIndex = visible[static_cast<std::size_t>(selected)];
    return modelIndex < state.entries.size() ? static_cast<int>(modelIndex) : -1;
}

const ContextMenuEntry* SelectedEntry(const ContextMenuViewState& state) {
    const int index = SelectedModelIndex(state);
    return index >= 0 ? &state.entries[static_cast<std::size_t>(index)] : nullptr;
}

std::wstring DetailTextForEntry(const ContextMenuEntry& entry) {
    std::wostringstream stream;
    stream << L"注册路径：HKEY_CLASSES_ROOT\\" << entry.registrationPath << L"\r\n"
        << L"作用域：" << entry.scopeText << L"    类型：" << KindText(entry.kind) << L"\r\n"
        << L"名称：" << entry.name << L"    显示名：" << entry.displayText << L"\r\n"
        << L"CLSID：" << (entry.clsid.empty() ? std::wstring(L"—") : entry.clsid) << L"\r\n"
        << L"模块：" << (entry.modulePath.empty() ? std::wstring(L"—") : entry.modulePath)
        << L"（" << ModuleStateText(entry) << L"）\r\n"
        << L"状态：" << (entry.enabled ? L"启用" : L"已禁用");
    if (!entry.backupKeyName.empty()) {
        stream << L"\r\n备份键：" << ContextMenuBackupRootPath() << L"\\" << entry.backupKeyName;
    }
    if (!entry.diagnosticText.empty()) {
        stream << L"\r\n说明：" << entry.diagnosticText;
    }
    return stream.str();
}

void ShowDetail(ContextMenuViewState& state) {
    if (!state.detailEdit) {
        return;
    }
    const ContextMenuEntry* entry = SelectedEntry(state);
    if (!entry) {
        ::SetWindowTextW(state.detailEdit, L"选择一项查看注册详情。禁用会把整个注册子树备份到本工具的备份键后再删除。");
        return;
    }
    const std::wstring text = DetailTextForEntry(*entry);
    ::SetWindowTextW(state.detailEdit, text.c_str());
}

void UpdateActionButtons(ContextMenuViewState& state) {
    const ContextMenuEntry* entry = state.actionInProgress ? nullptr : SelectedEntry(state);
    if (state.disableButton) {
        ::EnableWindow(state.disableButton, entry != nullptr && entry->enabled);
    }
    if (state.enableButton) {
        ::EnableWindow(state.enableButton, entry != nullptr && !entry->enabled);
    }
}

void RefreshSelectionDependentUi(ContextMenuViewState& state) {
    ShowDetail(state);
    UpdateActionButtons(state);
}

void ApplyFilterResult(ContextMenuViewState& state, ContextMenuFilterResult result) {
    if (result.generation != state.displayGeneration || result.query != state.filterQuery ||
        result.useRegex != state.filterUseRegex || !state.list.hwnd()) {
        return;
    }
    state.list.setVisibleIndexes(std::move(result.visibleIndexes));
    RefreshSelectionDependentUi(state);
    ::InvalidateRect(state.hwnd, nullptr, TRUE);
}

void RequestFilter(ContextMenuViewState& state, std::wstring query) {
    state.filterQuery = std::move(query);
    state.filterUseRegex = Ksword::Ui::GetFilterBarRegexEnabled(state.filterBar);
    const auto rows = state.filterRows;
    const std::uint64_t generation = state.displayGeneration;
    const bool useRegex = state.filterUseRegex;
    if (!state.filterTask || !rows) {
        return;
    }
    state.filterTask->request(
        [rows, generation, useRegex, query = state.filterQuery]() mutable {
            ContextMenuFilterResult result{};
            result.generation = generation;
            result.query = std::move(query);
            result.useRegex = useRegex;
            result.visibleIndexes = Ksword::Ui::VirtualListView::FilterRowIndexes(*rows, result.query, useRegex);
            return result;
        },
        [&state](std::uint64_t, std::optional<ContextMenuFilterResult>&& result, std::exception_ptr error) {
            if (error || !result.has_value()) {
                return;
            }
            ApplyFilterResult(state, std::move(*result));
        });
}

void BuildRows(ContextMenuViewState& state) {
    std::vector<Ksword::Ui::VirtualListRow> rows;
    rows.reserve(state.entries.size());
    for (std::size_t index = 0; index < state.entries.size(); ++index) {
        const ContextMenuEntry& entry = state.entries[index];
        Ksword::Ui::VirtualListRow row{};
        // The HKCR-relative registration path is unique across all five roots
        // and survives disable/enable, which is exactly what selection restore
        // after a refresh needs.
        row.stableKey = entry.registrationPath;
        row.itemData = static_cast<LPARAM>(index);
        row.textColor = RowColor(entry);
        row.cells.reserve(kColumnCount + 3);
        for (int column = 0; column < kColumnCount; ++column) {
            row.cells.push_back(CellText(entry, column));
        }
        row.cells.push_back(entry.displayText);
        row.cells.push_back(entry.registrationPath);
        row.cells.push_back(entry.diagnosticText);
        rows.push_back(std::move(row));
    }
    auto filterRows = std::make_shared<std::vector<Ksword::Ui::VirtualListRow>>(std::move(rows));
    state.list.setSharedRows(filterRows);
    state.filterRows = std::move(filterRows);
    ++state.displayGeneration;
}

void BeginScan(ContextMenuViewState& state) {
    if (!state.scanTask) {
        return;
    }
    if (state.refreshButton) {
        ::EnableWindow(state.refreshButton, FALSE);
    }
    state.statusText = L"正在后台枚举 shell 扩展注册点…";
    Ksword::Ui::SetLoadingOverlay(state.loadingOverlay, true, L"正在读取注册表…");
    ::InvalidateRect(state.hwnd, nullptr, TRUE);

    state.scanTask->request(
        [] { return ScanContextMenuEntries(); },
        [&state](std::uint64_t, std::optional<ContextMenuScanResult>&& scan, std::exception_ptr error) {
            if (state.refreshButton) {
                ::EnableWindow(state.refreshButton, TRUE);
            }
            Ksword::Ui::SetLoadingOverlay(state.loadingOverlay, false);
            if (error || !scan.has_value() || !scan->success) {
                state.statusText = L"右键菜单注册项枚举异常结束。";
                ::InvalidateRect(state.hwnd, nullptr, TRUE);
                return;
            }

            std::size_t disabled = 0;
            std::size_t missing = 0;
            for (const ContextMenuEntry& entry : scan->entries) {
                if (!entry.enabled) {
                    ++disabled;
                } else if (!entry.moduleFile.empty() && !entry.moduleExists) {
                    ++missing;
                }
            }
            state.entries = std::move(scan->entries);
            BuildRows(state);

            std::wostringstream summary;
            summary << L"共 " << state.entries.size() << L" 项，其中已禁用 " << disabled
                << L"，模块缺失 " << missing << L"。备份键：" << ContextMenuBackupRootPath();
            if (!scan->diagnosticText.empty()) {
                summary << L"。" << scan->diagnosticText;
            }
            state.statusText = summary.str();
            RequestFilter(state, state.filterBar ? Ksword::Ui::GetFilterBarText(state.filterBar) : std::wstring{});
            RefreshSelectionDependentUi(state);
            ::InvalidateRect(state.hwnd, nullptr, TRUE);
        });
}

// ConfirmDisable is the one prompt on this page. Disabling means deleting a live
// HKCR subtree, and although the backup makes it reversible, the shell will lose
// the entry immediately and the user has to know that before it happens. The
// default button is 否 so a stray Enter cannot remove a handler.
bool ConfirmDisable(HWND owner, const ContextMenuEntry& entry) {
    std::wstring text = L"将备份并删除右键菜单注册项：\n\nHKEY_CLASSES_ROOT\\" + entry.registrationPath + L"\n\n";
    if (!entry.displayText.empty()) {
        text += L"显示名：" + entry.displayText + L"\n";
    }
    if (!entry.modulePath.empty()) {
        text += L"模块：" + entry.modulePath + L"\n";
    }
    text += L"\n整个注册子树会先完整备份到：\n" + ContextMenuBackupRootPath() +
        L"\n\n删除后可用“启用”还原。是否继续？";
    return ::MessageBoxW(owner, text.c_str(), L"禁用右键菜单项", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
}

void RunAction(ContextMenuViewState& state, const bool disable) {
    const ContextMenuEntry* selected = SelectedEntry(state);
    if (!selected) {
        state.statusText = L"未选择注册项。";
        ::InvalidateRect(state.hwnd, nullptr, TRUE);
        return;
    }
    if (state.actionInProgress || !state.actionTask) {
        state.statusText = L"注册表操作正在执行。";
        ::InvalidateRect(state.hwnd, nullptr, TRUE);
        return;
    }
    if (disable && !ConfirmDisable(state.hwnd, *selected)) {
        state.statusText = L"已取消禁用操作。";
        ::InvalidateRect(state.hwnd, nullptr, TRUE);
        return;
    }

    const ContextMenuEntry entry = *selected;
    state.actionInProgress = true;
    UpdateActionButtons(state);
    state.statusText = disable ? L"正在备份并删除注册项…" : L"正在从备份还原注册项…";
    ::InvalidateRect(state.hwnd, nullptr, TRUE);
    state.actionTask->request(
        [entry, disable] { return disable ? DisableContextMenuEntry(entry) : EnableContextMenuEntry(entry); },
        [&state](std::uint64_t, std::optional<ContextMenuActionResult>&& result, std::exception_ptr error) {
            state.actionInProgress = false;
            if (error || !result.has_value()) {
                state.statusText = L"注册表操作异常结束。";
                UpdateActionButtons(state);
                ::InvalidateRect(state.hwnd, nullptr, TRUE);
                return;
            }
            state.statusText = result->message;
            if (result->success) {
                // The table always lies after a successful mutation, so it is
                // re-read rather than patched in place.
                BeginScan(state);
                return;
            }
            UpdateActionButtons(state);
            ::InvalidateRect(state.hwnd, nullptr, TRUE);
        });
}

std::wstring RowsAsText(const ContextMenuViewState& state, const bool allVisible) {
    const auto& rows = state.list.rows();
    const auto& visible = state.list.visibleIndexes();
    const HWND list = state.list.hwnd();
    std::wstring text;
    for (std::size_t item = 0; item < visible.size(); ++item) {
        if (!allVisible &&
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

// ExportRegistrationList writes one row scope of the registration table as TSV.
// "Visible" follows the active filter; "all" keeps the whole snapshot, which is
// what an evidence record needs when the operator has narrowed the table.
void ExportRegistrationList(ContextMenuViewState& state, const Ksword::Ui::ExportRowScope scope) {
    Ksword::Ui::ExportRowCount count{};
    std::wstring error;
    const Ksword::Ui::SaveTextFileResult result = Ksword::Ui::ExportVirtualListTsvWithDialog(
        state.hwnd,
        state.list,
        ContextMenuColumnTitles(),
        scope,
        L"context_menu_handlers.tsv",
        scope == Ksword::Ui::ExportRowScope::Visible
            ? L"导出右键菜单项（可见项）"
            : L"导出右键菜单项（全部项）",
        L"右键菜单导出",
        &count,
        &error);
    if (result == Ksword::Ui::SaveTextFileResult::Failed) {
        state.statusText = error.empty() ? L"右键菜单项导出失败。" : L"右键菜单项导出失败：" + error;
    } else if (result == Ksword::Ui::SaveTextFileResult::Saved) {
        std::wostringstream summary;
        summary << L"已导出 " << count.exportedRows << L" / " << count.totalRows << L" 项右键菜单注册";
        if (count.filteredRows) {
            summary << L"（筛选隐藏的 " << (count.totalRows - count.exportedRows) << L" 项未写入）";
        }
        summary << L"。";
        state.statusText = summary.str();
    }
    ::InvalidateRect(state.hwnd, nullptr, TRUE);
}

// The detail pane is a formatted reading layout, so it is saved as plain text
// instead of being flattened into columns.
void ExportRegistrationDetail(ContextMenuViewState& state) {
    const ContextMenuEntry* entry = SelectedEntry(state);
    if (!entry) {
        state.statusText = L"当前没有可导出的注册项详情。";
        ::InvalidateRect(state.hwnd, nullptr, TRUE);
        return;
    }
    std::wstring error;
    const Ksword::Ui::SaveTextFileResult result = Ksword::Ui::SaveUtf8TextFileWithDialog(
        state.hwnd,
        L"context_menu_detail.txt",
        L"导出右键菜单项详情",
        L"文本文件 (*.txt)\0*.txt\0所有文件 (*.*)\0*.*\0",
        L"txt",
        DetailTextForEntry(*entry),
        &error);
    if (result == Ksword::Ui::SaveTextFileResult::Failed) {
        state.statusText = error.empty() ? L"右键菜单项详情导出失败。" : L"右键菜单项详情导出失败：" + error;
    } else if (result == Ksword::Ui::SaveTextFileResult::Saved) {
        state.statusText = L"已导出所选右键菜单项的注册详情。";
    }
    ::InvalidateRect(state.hwnd, nullptr, TRUE);
}

// SelectRowAtPoint makes a right-click command apply to the row under the
// pointer rather than a stale selection left by an earlier operation.
void SelectRowAtPoint(ContextMenuViewState& state, const POINT screenPoint) {
    const HWND list = state.list.hwnd();
    if (!list) {
        return;
    }
    POINT clientPoint = screenPoint;
    ::ScreenToClient(list, &clientPoint);
    LVHITTESTINFO hit{};
    hit.pt = clientPoint;
    const int clickedItem = ListView_HitTest(list, &hit);
    ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    if (clickedItem < 0 || static_cast<std::size_t>(clickedItem) >= state.list.visibleIndexes().size()) {
        RefreshSelectionDependentUi(state);
        return;
    }
    ListView_SetItemState(list, clickedItem, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    RefreshSelectionDependentUi(state);
}

// OpenSelectedRegistryKey routes a live HKCR-relative registration to the
// registry browser. The destination reopens the key, so this snapshot never
// grants access to a deleted or changed registration.
void OpenSelectedRegistryKey(ContextMenuViewState& state) {
    const ContextMenuEntry* entry = SelectedEntry(state);
    if (!entry || !entry->enabled || entry->registrationPath.empty()) {
        state.statusText = L"当前选择没有可打开的活动注册项。";
        ::InvalidateRect(state.hwnd, nullptr, TRUE);
        return;
    }

    Ksword::Core::NavigationRequest request{};
    request.target = Ksword::Core::NavigationTarget::RegistryBrowser;
    request.entity.kind = Ksword::Core::EntityKind::RegistryKey;
    request.entity.text = L"HKCR\\" + entry->registrationPath;
    state.statusText = Ksword::Ui::RequestEntityNavigation(state.hwnd, request)
        ? L"已转到注册表浏览器；目标路径会重新读取。"
        : L"无法导航到该活动注册表项。";
    ::InvalidateRect(state.hwnd, nullptr, TRUE);
}

std::wstring ModuleDirectoryForEntry(const ContextMenuEntry& entry) {
    if (!entry.moduleExists) {
        return {};
    }
    return Ksword::Features::File::PathNavigator::parentDirectoryForKnownFilePath(entry.moduleFile);
}

// OpenSelectedModuleDirectory accepts only a file the scanner explicitly
// checked, then applies the strict known DOS/UNC path gate before routing.
void OpenSelectedModuleDirectory(ContextMenuViewState& state) {
    const ContextMenuEntry* entry = SelectedEntry(state);
    const std::wstring directory = entry ? ModuleDirectoryForEntry(*entry) : std::wstring{};
    if (directory.empty()) {
        state.statusText = L"当前项没有已验证的可导航模块文件路径。";
        ::InvalidateRect(state.hwnd, nullptr, TRUE);
        return;
    }

    Ksword::Core::NavigationRequest request{};
    request.target = Ksword::Core::NavigationTarget::FileBrowser;
    request.entity.kind = Ksword::Core::EntityKind::File;
    request.entity.text = directory;
    state.statusText = Ksword::Ui::RequestEntityNavigation(state.hwnd, request)
        ? L"已在文件模块打开模块所在目录。"
        : L"文件模块当前无法接收模块所在目录。";
    ::InvalidateRect(state.hwnd, nullptr, TRUE);
}

void ShowContextMenu(ContextMenuViewState& state, POINT screenPoint) {
    SelectRowAtPoint(state, screenPoint);
    const ContextMenuEntry* entry = SelectedEntry(state);
    const std::wstring moduleDirectory = entry ? ModuleDirectoryForEntry(*entry) : std::wstring{};
    HMENU menu = ::CreatePopupMenu();
    if (!menu) {
        return;
    }
    const bool busy = state.actionInProgress;
    ::AppendMenuW(menu, MF_STRING | ((entry && entry->enabled && !busy) ? MF_ENABLED : MF_GRAYED),
        kMenuDisable, L"禁用（备份后删除）");
    ::AppendMenuW(menu, MF_STRING | ((entry && !entry->enabled && !busy) ? MF_ENABLED : MF_GRAYED),
        kMenuEnable, L"启用（从备份还原）");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING | ((entry && entry->enabled && !entry->registrationPath.empty() && !busy) ? MF_ENABLED : MF_GRAYED),
        kMenuOpenRegistry, L"在注册表中打开");
    ::AppendMenuW(menu, MF_STRING | ((!moduleDirectory.empty() && !busy) ? MF_ENABLED : MF_GRAYED),
        kMenuOpenModuleDirectory, L"打开模块所在目录");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING | (entry ? MF_ENABLED : MF_GRAYED), kMenuCopyRow, L"复制选中行");
    ::AppendMenuW(menu, MF_STRING, kMenuCopyVisible, L"复制可见行");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kMenuExportVisible, L"导出可见菜单项 TSV…");
    ::AppendMenuW(menu, MF_STRING, kMenuExportAll, L"导出全部菜单项 TSV…");
    ::AppendMenuW(menu, MF_STRING | (entry ? MF_ENABLED : MF_GRAYED), kMenuExportDetail, L"导出注册详情 TXT…");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kMenuRefresh, L"刷新");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    Ksword::Ui::AppendTableCompareMenuItems(menu, state.snapshotEngine);

    const int command = ::TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, 0, state.hwnd, nullptr);
    ::DestroyMenu(menu);

    switch (static_cast<UINT>(command)) {
    case kMenuDisable:
        RunAction(state, true);
        break;
    case kMenuEnable:
        RunAction(state, false);
        break;
    case kMenuOpenRegistry:
        OpenSelectedRegistryKey(state);
        break;
    case kMenuOpenModuleDirectory:
        OpenSelectedModuleDirectory(state);
        break;
    case kMenuCopyRow:
        state.statusText = CopyText(state.hwnd, RowsAsText(state, false)) ? L"已复制选中行。" : L"复制失败。";
        ::InvalidateRect(state.hwnd, nullptr, TRUE);
        break;
    case kMenuCopyVisible:
        state.statusText = CopyText(state.hwnd, RowsAsText(state, true)) ? L"已复制可见行。" : L"复制失败。";
        ::InvalidateRect(state.hwnd, nullptr, TRUE);
        break;
    case kMenuExportVisible:
        ExportRegistrationList(state, Ksword::Ui::ExportRowScope::Visible);
        break;
    case kMenuExportAll:
        ExportRegistrationList(state, Ksword::Ui::ExportRowScope::All);
        break;
    case kMenuExportDetail:
        ExportRegistrationDetail(state);
        break;
    case kMenuRefresh:
        BeginScan(state);
        break;
    default:
        // 快照命令 id 在所有表之间共用一段，不是本页的一律交给引擎判定。
        if (Ksword::Ui::HandleTableCompareCommand(state.snapshotEngine, static_cast<UINT>(command),
                state.list, ContextMenuColumnTitles(), state.statusText)) {
            ::InvalidateRect(state.hwnd, nullptr, TRUE);
        }
        break;
    }
}

void LayoutView(ContextMenuViewState& state) {
    RECT client{};
    ::GetClientRect(state.hwnd, &client);
    const int width = Width(client);
    const int height = Height(client);

    int cursorX = kGap;
    const int firstRowY = kGap;
    const auto place = [&cursorX, firstRowY](HWND control, int controlWidth) {
        if (control) {
            ::MoveWindow(control, cursorX, firstRowY, controlWidth, kRowHeight, TRUE);
        }
        cursorX += controlWidth + kGap;
    };
    place(state.refreshButton, 64);
    place(state.disableButton, 140);
    place(state.enableButton, 140);

    const int secondRowY = firstRowY + kRowHeight + kGap;
    if (state.filterBar) {
        ::MoveWindow(state.filterBar, kGap, secondRowY, (std::max)(120, width - kGap * 2), kRowHeight, TRUE);
    }

    const int listTop = kHeaderHeight;
    const int detailTop = (std::max)(listTop, height - kStatusHeight - kDetailHeight);
    const int listHeight = (std::max)(0, detailTop - listTop - kGap);
    if (HWND list = state.list.hwnd()) {
        ::MoveWindow(list, kGap, listTop, (std::max)(0, width - kGap * 2), listHeight, TRUE);
    }
    if (state.detailEdit) {
        ::MoveWindow(state.detailEdit, kGap, detailTop, (std::max)(0, width - kGap * 2),
            (std::max)(0, height - kStatusHeight - detailTop - kGap), TRUE);
    }
    if (state.loadingOverlay) {
        ::MoveWindow(state.loadingOverlay, kGap, listTop, (std::max)(0, width - kGap * 2), listHeight, TRUE);
    }
}

bool CreateChildControls(ContextMenuViewState& state) {
    HWND hwnd = state.hwnd;
    state.refreshButton = Ksword::Ui::CreateButton(hwnd, kRefreshButtonId, L"刷新", 0, 0, 0, 0);
    state.disableButton = Ksword::Ui::CreateButton(hwnd, kDisableButtonId, L"禁用（备份后删除）", 0, 0, 0, 0);
    state.enableButton = Ksword::Ui::CreateButton(hwnd, kEnableButtonId, L"启用（从备份还原）", 0, 0, 0, 0);
    state.filterBar = Ksword::Ui::CreateFilterBar(
        hwnd, kFilterBarId, L"筛选作用域、名称、CLSID 与模块路径", 0, 0, 0, 0);
    if (!state.refreshButton || !state.disableButton || !state.enableButton || !state.filterBar) {
        return false;
    }

    if (!state.list.create(hwnd, kListId, 0, 0, 1, 1, LVS_SHOWSELALWAYS | LVS_SINGLESEL)) {
        return false;
    }
    {
        const std::vector<std::wstring>& titles = ContextMenuColumnTitles();
        static const int widths[] = { 70, 80, 130, 180, 250, 340, 70 };
        std::vector<Ksword::Ui::ListViewColumn> columns;
        for (std::size_t index = 0; index < titles.size(); ++index) {
            columns.push_back({ static_cast<int>(index), widths[index], LVCFMT_LEFT, titles[index] });
        }
        state.list.addColumns(columns);
    }
    if (HWND list = state.list.hwnd()) {
        ListView_SetExtendedListViewStyle(list,
            LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES | LVS_EX_LABELTIP);
        ::SendMessageW(list, WM_SETFONT, reinterpret_cast<WPARAM>(Ksword::Ui::SystemUIFont()), TRUE);
    }

    state.detailEdit = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
        0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDetailEditId)),
        ::GetModuleHandleW(nullptr), nullptr);
    if (!state.detailEdit) {
        return false;
    }
    ::SendMessageW(state.detailEdit, WM_SETFONT, reinterpret_cast<WPARAM>(Ksword::Ui::SystemUIFont()), TRUE);
    Ksword::Ui::AttachTextFindSupport(state.detailEdit);

    state.loadingOverlay = Ksword::Ui::CreateLoadingOverlay(hwnd, kLoadingOverlayId, { 0, 0, 1, 1 });
    if (!state.loadingOverlay) {
        return false;
    }

    Ksword::Ui::SetWindowFontRecursive(hwnd);
    return true;
}

LRESULT CALLBACK ContextMenuViewProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<ContextMenuViewState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCCREATE: {
        auto owned = std::make_unique<ContextMenuViewState>();
        owned->hwnd = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(owned.release()));
        return TRUE;
    }
    case WM_CREATE:
        if (state) {
            if (!CreateChildControls(*state)) {
                return -1;
            }
            state->scanTask = std::make_unique<Ksword::Ui::AsyncSnapshotTask<ContextMenuScanResult>>(hwnd, kMsgScanCompleted);
            state->filterTask = std::make_unique<Ksword::Ui::AsyncSnapshotTask<ContextMenuFilterResult>>(hwnd, kMsgFilterCompleted);
            state->actionTask = std::make_unique<Ksword::Ui::AsyncSnapshotTask<ContextMenuActionResult>>(hwnd, kMsgActionCompleted);
            LayoutView(*state);
            RefreshSelectionDependentUi(*state);
            BeginScan(*state);
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
                RequestFilter(*state, Ksword::Ui::GetFilterBarText(state->filterBar));
                return 0;
            }
            if (notification == BN_CLICKED) {
                switch (id) {
                case kRefreshButtonId:
                    BeginScan(*state);
                    return 0;
                case kDisableButtonId:
                    RunAction(*state, true);
                    return 0;
                case kEnableButtonId:
                    RunAction(*state, false);
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
                if (state->list.handleNotify(*header, result)) {
                    return result;
                }
                // The click cycle is unsorted -> ascending -> descending ->
                // unsorted. The sort lives in VirtualListView, so the periodic
                // refresh replays it instead of dropping it with the snapshot.
                if (header->hwndFrom == state->list.hwnd() && header->code == LVN_COLUMNCLICK) {
                    auto* clicked = reinterpret_cast<NMLISTVIEW*>(lParam);
                    const Ksword::Ui::SortSpec next =
                        Ksword::Ui::NextSortSpec(state->list.sortSpec(), clicked->iSubItem);
                    state->list.applySort(next);
                    Ksword::Ui::ShowSortIndicator(state->list.hwnd(), next);
                    return 0;
                }
                if (header->hwndFrom == state->list.hwnd() && header->code == LVN_ITEMCHANGED) {
                    const auto* changed = reinterpret_cast<const NMLISTVIEW*>(lParam);
                    if (changed && (changed->uNewState & LVIS_SELECTED) != 0) {
                        RefreshSelectionDependentUi(*state);
                    }
                    return 0;
                }
                if (header->hwndFrom == state->list.hwnd() && header->code == NM_RCLICK) {
                    POINT point{};
                    ::GetCursorPos(&point);
                    ShowContextMenu(*state, point);
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
    case WM_CTLCOLORSTATIC: {
        // Read-only EDIT controls report through this message too; leaving them
        // transparent smears text while scrolling, so theme STATIC labels only.
        HDC dc = reinterpret_cast<HDC>(wParam);
        const LRESULT labelBrush = Ksword::Ui::PaintLabelBackground(dc,
            reinterpret_cast<HWND>(lParam), Ksword::Ui::AppTheme().windowBrush(),
            Ksword::Ui::AppTheme().textColor);
        if (labelBrush != 0) {
            return labelBrush;
        }
        break;
    }
    default:
        if (state) {
            if (msg == kMsgScanCompleted && state->scanTask) {
                state->scanTask->consume(hwnd, wParam, lParam);
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
            if (state->scanTask) {
                state->scanTask->cancel();
            }
            if (state->filterTask) {
                state->filterTask->cancel();
            }
            if (state->actionTask) {
                state->actionTask->cancel();
            }
            state->list.detach();
            delete state;
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool EnsureContextMenuViewClass() {
    static bool registered = false;
    if (registered) {
        return true;
    }
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = ContextMenuViewProc;
    windowClass.hInstance = ::GetModuleHandleW(nullptr);
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = Ksword::Ui::AppTheme().windowBrush();
    windowClass.lpszClassName = kContextMenuViewClass;
    registered = ::RegisterClassW(&windowClass) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return registered;
}

} // namespace

HWND CreateContextMenuView(HWND parent, const RECT& bounds) {
    if (!parent || !EnsureContextMenuViewClass()) {
        return nullptr;
    }
    return ::CreateWindowExW(
        0, kContextMenuViewClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
        parent, nullptr, ::GetModuleHandleW(nullptr), nullptr);
}

} // namespace Ksword::Features::SysTools
