#include "RegistryView.h"

#include "RegistryActions.h"
#include "RegistryModel.h"
#include "../../Ui/AsyncTask.h"
#include "../../Ui/Controls.h"
#include "../../Ui/ExportUtil.h"
#include "../../Ui/FilterBar.h"
#include "../../Ui/LazyTreeView.h"
#include "../../Ui/ListViewUtil.h"
#include "../../Ui/LoadingOverlay.h"
#include "../../Ui/TextFindSupport.h"
#include "../../Ui/Theme.h"
#include "../../Ui/VirtualListView.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cwchar>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace Ksword::Features::Registry {
namespace {

constexpr wchar_t kRegistryViewClass[] = L"KswordARKLight.RegistryView";
constexpr int kRefreshButtonId = 68001;
constexpr int kPathEditId = 68002;
constexpr int kGoButtonId = 68003;
constexpr int kModeComboId = 68004;
constexpr int kListId = 68005;
constexpr int kNameEditId = 68006;
constexpr int kTypeComboId = 68007;
constexpr int kDataEditId = 68008;
constexpr int kReadButtonId = 68009;
constexpr int kWriteButtonId = 68010;
constexpr int kCreateKeyButtonId = 68011;
constexpr int kDeleteButtonId = 68012;
constexpr int kRenameButtonId = 68013;
constexpr int kStatusId = 68014;
constexpr int kTreeId = 68015;
constexpr int kUpButtonId = 68016;

// 树筛选是"重建整棵可见树"，所以必须攒到一次输入停顿再跑：原先每个
// EN_CHANGE 都重建，打十个字就是十次全量拆树 + 十次 HKLM 重新枚举。
constexpr UINT_PTR kTreeFilterTimerId = 0x6810;
constexpr UINT kTreeFilterDebounceMs = 250;
constexpr int kValueFilterBarId = 68017;
constexpr int kTreeFilterBarId = 68018;

constexpr UINT kMenuRefresh = 68101;
constexpr UINT kMenuCopyName = 68102;
constexpr UINT kMenuCopyData = 68103;
constexpr UINT kMenuRead = 68104;
constexpr UINT kMenuWrite = 68105;
constexpr UINT kMenuDelete = 68106;
constexpr UINT kMenuCreateSubKey = 68107;
constexpr UINT kMenuRename = 68108;
constexpr UINT kMenuCopyRow = 68110;
constexpr UINT kMenuCopyVisible = 68111;
constexpr UINT kMenuCopyCell = 68112;
constexpr UINT kMenuExportVisible = 68113;
constexpr UINT kMsgSnapshotCompleted = WM_APP + 560;
constexpr UINT kMsgTreeChildrenCompleted = WM_APP + 561;
constexpr UINT kMsgFilterCompleted = WM_APP + 562;
constexpr UINT kMsgOperationCompleted = WM_APP + 563;
constexpr UINT kMsgExternalNavigate = WM_APP + 564;
constexpr int kLoadingOverlayId = 68109;

struct RegistryRefreshSnapshot {
    std::wstring path;
    RegistryViewMode mode = RegistryViewMode::WinApi;
    std::uint64_t navigationGeneration = 0;
    RegistrySnapshot snapshot;
};

struct RegistryTreeChildrenSnapshot {
    std::wstring path;
    RegistryViewMode mode = RegistryViewMode::WinApi;
    std::wstring query;
    std::wstring statusText;
    std::vector<std::wstring> subKeys;
};

struct RegistryFilterResult {
    std::uint64_t generation = 0;
    std::wstring query;
    bool useRegex = false;
    std::wstring selectedStableKey;
    std::wstring topStableKey;
    std::vector<std::size_t> visibleIndexes;
};

enum class RegistryOperationKind {
    Read,
    Write,
    CreateKey,
    DeleteValue,
    DeleteKey,
    RenameValue,
    RenameKey
};

struct RegistryOperationRequest {
    RegistryOperationKind kind = RegistryOperationKind::Read;
    std::wstring path;
    std::wstring name;
    std::wstring alternateName;
    RegistryViewMode mode = RegistryViewMode::WinApi;
    std::uint32_t valueType = REG_SZ;
    std::vector<std::uint8_t> data;
};

struct RegistryOperationSnapshot {
    RegistryOperationKind kind = RegistryOperationKind::Read;
    RegistryOperationResult result;
    bool refreshRequired = false;
};

struct RegistryViewState {
    HWND hwnd = nullptr;
    HWND refreshButton = nullptr;
    HWND upButton = nullptr;
    HWND pathEdit = nullptr;
    HWND goButton = nullptr;
    HWND modeCombo = nullptr;
    Ksword::Ui::LazyTreeView tree;
    HWND valueFilterBar = nullptr;
    HWND treeFilterBar = nullptr;
    Ksword::Ui::VirtualListView list;
    HWND nameEdit = nullptr;
    HWND typeCombo = nullptr;
    HWND dataEdit = nullptr;
    HWND readButton = nullptr;
    HWND writeButton = nullptr;
    HWND createKeyButton = nullptr;
    HWND deleteButton = nullptr;
    HWND renameButton = nullptr;
    HWND statusText = nullptr;
    HWND loadingOverlay = nullptr;
    RegistryViewMode mode = RegistryViewMode::WinApi;
    RegistrySnapshot snapshot;
    std::shared_ptr<const std::vector<Ksword::Ui::VirtualListRow>> filterRows;
    std::wstring valueFilterQuery;
    bool valueFilterUseRegex = false;
    std::wstring treeFilterQuery;
    bool treeFilterUseRegex = false;
    std::wstring treeLoadingPath;
    std::uint64_t displayGeneration = 0;
    std::uint64_t navigationGeneration = 0;
    bool currentSnapshotReady = false;
    bool operationInProgress = false;
    int contextColumn = 0;
    // suppressPathEditChange 挡住 NavigateTo 自己 SetWindowTextW 引发的那次
    // EN_CHANGE，否则一次导航要把刚清好的快照作废两遍。
    bool suppressPathEditChange = false;
    std::unique_ptr<Ksword::Ui::AsyncSnapshotTask<RegistryRefreshSnapshot>> refreshTask;
    std::unique_ptr<Ksword::Ui::AsyncSnapshotTask<RegistryTreeChildrenSnapshot>> treeChildrenTask;
    std::unique_ptr<Ksword::Ui::AsyncSnapshotTask<RegistryFilterResult>> filterTask;
    std::unique_ptr<Ksword::Ui::AsyncSnapshotTask<RegistryOperationSnapshot>> operationTask;
};


struct RegistryTypeOption {
    std::uint32_t type;
    const wchar_t* text;
};

struct RegistryRootNode {
    const wchar_t* displayText;
    const wchar_t* pathText;
};

constexpr RegistryTypeOption kTypeOptions[] = {
    { REG_SZ, L"REG_SZ" },
    { REG_EXPAND_SZ, L"REG_EXPAND_SZ" },
    { REG_MULTI_SZ, L"REG_MULTI_SZ" },
    { REG_DWORD, L"REG_DWORD" },
    { REG_BINARY, L"REG_BINARY" },
};

constexpr RegistryRootNode kRootNodes[] = {
    { L"HKEY_CLASSES_ROOT", L"HKCR" },
    { L"HKEY_CURRENT_USER", L"HKCU" },
    { L"HKEY_LOCAL_MACHINE", L"HKLM" },
    { L"HKEY_USERS", L"HKU" },
    { L"HKEY_CURRENT_CONFIG", L"HKCC" },
};

void PopulateList(RegistryViewState& state);
void RefreshSnapshot(RegistryViewState& state);
void SyncEditorFromSelection(RegistryViewState& state);
void LayoutChildren(RegistryViewState& state);
void SelectPathInTree(RegistryViewState& state, const std::wstring& path);
void NavigateTo(RegistryViewState& state, const std::wstring& path);
void InvalidateCurrentSnapshot(RegistryViewState& state);
void SetOperationControlsEnabled(RegistryViewState& state, bool enabled);
void RequestValueFilter(RegistryViewState& state, std::wstring query, std::wstring selectedStableKey, std::wstring topStableKey);

RegistryViewState* StateFromWindow(HWND hwnd) {
    return reinterpret_cast<RegistryViewState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

int Width(const RECT& rc) {
    return rc.right > rc.left ? rc.right - rc.left : 0;
}

int Height(const RECT& rc) {
    return rc.bottom > rc.top ? rc.bottom - rc.top : 0;
}

std::wstring WindowTextOf(HWND hwnd) {
    if (!hwnd) {
        return {};
    }
    const int length = ::GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<std::size_t>(std::max(length, 0)) + 1U, L'\0');
    ::GetWindowTextW(hwnd, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(std::max(length, 0)));
    return text;
}

std::wstring DisplayPathFromRootAndChild(const std::wstring& rootText, const std::wstring& childName) {
    if (rootText.empty()) {
        return childName;
    }
    return childName.empty() ? rootText : (rootText + L"\\" + childName);
}

void SetStatus(RegistryViewState& state, const std::wstring& text) {
    if (state.statusText) {
        ::SetWindowTextW(state.statusText, text.c_str());
    }
}

std::wstring CurrentPath(const RegistryViewState& state) {
    return WindowTextOf(state.pathEdit);
}

std::uint32_t SelectedRegistryType(const RegistryViewState& state) {
    if (!state.typeCombo) {
        return REG_SZ;
    }
    const LRESULT selection = ::SendMessageW(state.typeCombo, CB_GETCURSEL, 0, 0);
    if (selection < 0 || selection >= static_cast<LRESULT>(std::size(kTypeOptions))) {
        return REG_SZ;
    }
    return kTypeOptions[static_cast<std::size_t>(selection)].type;
}

void SetSelectedRegistryType(RegistryViewState& state, const std::uint32_t type) {
    for (std::size_t index = 0; index < std::size(kTypeOptions); ++index) {
        if (kTypeOptions[index].type == type) {
            ::SendMessageW(state.typeCombo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
            return;
        }
    }
    ::SendMessageW(state.typeCombo, CB_SETCURSEL, 0, 0);
}

std::wstring FormatDataForEditor(const RegistryEntry& entry) {
    if (entry.valueType == REG_DWORD && entry.data.size() >= sizeof(std::uint32_t)) {
        const std::uint32_t value = static_cast<std::uint32_t>(entry.data[0]) |
            (static_cast<std::uint32_t>(entry.data[1]) << 8) |
            (static_cast<std::uint32_t>(entry.data[2]) << 16) |
            (static_cast<std::uint32_t>(entry.data[3]) << 24);
        wchar_t buffer[32]{};
        ::swprintf_s(buffer, L"0x%08X", value);
        return buffer;
    }
    return FormatRegistryData(entry.valueType, entry.data);
}

std::wstring ChildPath(const std::wstring& basePath, const std::wstring& name) {
    if (name.empty()) {
        return basePath;
    }
    if (basePath.empty()) {
        return name;
    }
    return basePath.back() == L'\\' ? (basePath + name) : (basePath + L"\\" + name);
}

std::wstring ParentPath(const std::wstring& path) {
    return ParentRegistryPath(path);
}

std::vector<Ksword::Ui::ListViewColumn> RegistryColumns() {
    return {
        { 0, 220, LVCFMT_LEFT, L"名称" },
        { 1, 120, LVCFMT_LEFT, L"类型" },
        { 2, 420, LVCFMT_LEFT, L"数据" },
        { 3, 260, LVCFMT_LEFT, L"详情" },
    };
}

bool SelectedEntry(RegistryViewState& state, int* rowIndex, RegistryEntry** entryOut) {
    const HWND list = state.list.hwnd();
    if (!list) {
        return false;
    }
    const int selected = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    const auto& visibleIndexes = state.list.visibleIndexes();
    const auto& rows = state.list.rows();
    if (selected < 0 || static_cast<std::size_t>(selected) >= visibleIndexes.size()) {
        return false;
    }
    const std::size_t sourceIndex = visibleIndexes[static_cast<std::size_t>(selected)];
    if (sourceIndex >= rows.size()) {
        return false;
    }
    const std::size_t snapshotIndex = static_cast<std::size_t>(rows[sourceIndex].itemData);
    if (snapshotIndex >= state.snapshot.rows.size()) {
        return false;
    }
    if (rowIndex) {
        *rowIndex = selected;
    }
    if (entryOut) {
        *entryOut = &state.snapshot.rows[snapshotIndex];
    }
    return true;
}

// ---- 树层 ---------------------------------------------------------------
//
// 节点记录、批量插入、单父节点上限和 key 索引都归 Ksword::Ui::LazyTreeView。
// 本文件不再维护 RegistryTreeNodeData，也不再手写递归全树扫描：一个节点的
// key 就是它的规范注册表路径，findItem 是一次哈希查找。

void FillTreeRoots(RegistryViewState& state) {
    std::vector<Ksword::Ui::TreeRow> roots;
    roots.reserve(std::size(kRootNodes));
    for (const RegistryRootNode& rootNode : kRootNodes) {
        Ksword::Ui::TreeRow row;
        row.text = rootNode.displayText;
        row.key = rootNode.pathText;
        // 不再插占位子节点，展开箭头由 TVIF_CHILDREN 提供。
        row.hasChildren = true;
        roots.push_back(std::move(row));
    }
    state.tree.setRoots(std::move(roots));
}

void ReplaceTreeNodeChildren(RegistryViewState& state, HTREEITEM item, const RegistryTreeChildrenSnapshot& snapshot) {
    if (!state.tree.hwnd() || !item || state.tree.keyOf(item) != snapshot.path) {
        return;
    }
    std::vector<Ksword::Ui::TreeRow> children;
    children.reserve(snapshot.subKeys.size());
    for (const std::wstring& subKey : snapshot.subKeys) {
        Ksword::Ui::TreeRow row;
        row.text = subKey;
        row.key = DisplayPathFromRootAndChild(snapshot.path, subKey);
        row.hasChildren = true;
        children.push_back(std::move(row));
    }
    // 控件自己决定先插多少：一次最多 kBatchSize 行，剩下的挂在末尾的"载入更多"
    // 上，触到 kMaxRowsPerParent 就换成截断行。UI 线程不会再被"一次插完整个
    // 子键列表"钉住。
    state.tree.setChildren(item, std::move(children));
    SetStatus(state, snapshot.statusText.empty() ? L"已加载注册表子键。" : snapshot.statusText);
}

bool ReleaseTreeChildrenRequest(RegistryViewState& state, const std::wstring& path) {
    // Input is the path of a tree-children request whose result will be dropped.
    // Processing releases the loading flag that request owns, because the control
    // refuses to re-request a node whose children are still marked loading; a
    // leaked flag makes the node permanently un-expandable. Output is true only
    // when this request still owns treeLoadingPath, i.e. it is the expansion the
    // user is currently waiting for: a request superseded by faster node
    // switching has already handed the path to the newer node and stays silent so
    // newer status text survives.
    if (path.empty()) {
        return false;
    }
    if (const HTREEITEM item = state.tree.findItem(path)) {
        // 正在加载的节点一定还没有子节点，所以这里等价于"只把加载标志放掉"，
        // 让它下次还能被点开。
        state.tree.clearChildren(item);
    }
    if (state.treeLoadingPath != path) {
        return false;
    }
    state.treeLoadingPath.clear();
    return true;
}

bool RequestTreeChildren(RegistryViewState& state, HTREEITEM item, const std::wstring& path) {
    // Inputs are the node the user just tried to open and its canonical path.
    // Processing starts the direct-child query on its own snapshot task and keeps
    // the node collapsed until the answer lands, so tree expansion never blocks a
    // dock switch, drag, or close. Output is true when the request was accepted.
    if (!item || path.empty() || !state.treeChildrenTask) {
        return false;
    }
    // 同一时刻只让一个子键枚举在飞。被顶掉的那个节点的加载标志必须显式放掉，
    // 否则控件会替它守着一个永不到来的结果，那个节点就再也点不开。
    if (!state.treeLoadingPath.empty() && state.treeLoadingPath != path) {
        if (const HTREEITEM previous = state.tree.findItem(state.treeLoadingPath)) {
            state.tree.clearChildren(previous);
        }
    }
    const RegistryViewMode mode = state.mode;
    const std::wstring query = state.treeFilterBar ? Ksword::Ui::GetFilterBarText(state.treeFilterBar) : state.treeFilterQuery;
    state.treeFilterUseRegex = Ksword::Ui::GetFilterBarRegexEnabled(state.treeFilterBar);
    const bool useRegex = state.treeFilterUseRegex;
    state.treeLoadingPath = path;
    SetStatus(state, L"正在后台加载注册表树节点…");
    state.treeChildrenTask->request(
        [path, mode, query, useRegex] {
            RegistryTreeChildrenSnapshot snapshot{};
            snapshot.path = path;
            snapshot.mode = mode;
            snapshot.query = query;
            snapshot.subKeys = EnumerateRegistrySubKeyNames(path, mode, &snapshot.statusText);
            if (!snapshot.query.empty()) {
                std::vector<Ksword::Ui::VirtualListRow> rows;
                rows.reserve(snapshot.subKeys.size());
                for (const std::wstring& subKey : snapshot.subKeys) {
                    rows.push_back({ subKey, { subKey }, 0 });
                }
                const std::vector<std::size_t> visible = Ksword::Ui::VirtualListView::FilterRowIndexes(rows, snapshot.query, useRegex);
                std::vector<std::wstring> filtered;
                filtered.reserve(visible.size());
                for (const std::size_t index : visible) {
                    filtered.push_back(std::move(snapshot.subKeys[index]));
                }
                snapshot.subKeys = std::move(filtered);
            }
            return snapshot;
        },
        [&state, path](std::uint64_t, std::optional<RegistryTreeChildrenSnapshot>&& snapshot, std::exception_ptr error) {
            if (error || !snapshot.has_value()) {
                ReleaseTreeChildrenRequest(state, path);
                SetStatus(state, L"注册表树节点加载异常结束。已保留现有节点。");
                return;
            }
            if (snapshot->mode != state.mode) {
                if (ReleaseTreeChildrenRequest(state, path)) {
                    SetStatus(state, L"注册表树节点加载结果已过期，请重新展开该节点。");
                }
                return;
            }
            const HTREEITEM item = state.tree.findItem(snapshot->path);
            if (!item) {
                if (ReleaseTreeChildrenRequest(state, path)) {
                    SetStatus(state, L"注册表树节点加载结果已过期，请重新展开该节点。");
                }
                return;
            }
            state.treeLoadingPath.clear();
            const std::wstring loadedPath = snapshot->path;
            ReplaceTreeNodeChildren(state, item, *snapshot);
            // 当前路径正好在这个节点下面：这一层刚落地的数据正好解锁下一层，接着
            // 往下走一步。没有这一步，从外部跳到深层键只会停在第一个还没枚举的
            // 层级上。
            const std::wstring current = CurrentPath(state);
            if (current.size() > loadedPath.size() && current.compare(0, loadedPath.size(), loadedPath) == 0 &&
                current[loadedPath.size()] == L'\\') {
                SelectPathInTree(state, current);
            }
        });
    return true;
}

void SelectPathInTree(RegistryViewState& state, const std::wstring& path) {
    // Inputs are a registry path from the edit box or navigation command.
    // Processing walks the ancestor chain one segment at a time, expanding only
    // what is needed to reveal the path. Every step is a hash lookup on the
    // canonical path, not a scan of the siblings' text. There is no return value;
    // the tree selection is best-effort.
    if (!state.tree.hwnd()) {
        return;
    }
    const RegistryPathInfo parsed = ParseRegistryPath(path);
    if (!parsed.valid) {
        return;
    }

    std::wstring current = parsed.rootText;
    HTREEITEM item = state.tree.findItem(current);
    if (!item) {
        return;
    }

    std::wstring remaining = parsed.subKey;
    while (!remaining.empty()) {
        const std::size_t slash = remaining.find(L'\\');
        const std::wstring segment = slash == std::wstring::npos ? remaining : remaining.substr(0, slash);
        const std::wstring childPath = DisplayPathFromRootAndChild(current, segment);
        const HTREEITEM child = state.tree.findItem(childPath);
        if (!child) {
            // 这一层还没枚举出来：把当前层展开，剩下的等数据落地后由
            // RequestTreeChildren 的回调接着往下走。
            RequestTreeChildren(state, item, current);
            break;
        }
        current = childPath;
        item = child;
        if (slash == std::wstring::npos) {
            break;
        }
        remaining.erase(0, slash + 1);
    }

    // 静默选中：页面自己知道刚导航到哪，让它的选中回调再跑一遍只会把这次导航
    // 从头再做一次。
    state.tree.selectByKey(current);
}

void SyncEditorFromSelection(RegistryViewState& state) {
    RegistryEntry* entry = nullptr;
    if (!SelectedEntry(state, nullptr, &entry) || entry == nullptr) {
        return;
    }
    ::SetWindowTextW(state.nameEdit, entry->name.c_str());
    if (entry->kind == RegistryRowKind::Value) {
        SetSelectedRegistryType(state, entry->valueType);
        ::SetWindowTextW(state.dataEdit, FormatDataForEditor(*entry).c_str());
    } else {
        ::SetWindowTextW(state.dataEdit, L"");
    }
}

std::wstring StableKeyForRegistryEntry(const RegistryEntry& entry) {
    return entry.name + L"|" + std::to_wstring(entry.valueType) + L"|" + entry.typeText;
}

std::wstring StableKeyFromListItem(const RegistryViewState& state, int item) {
    const auto& visible = state.list.visibleIndexes();
    const auto& rows = state.list.rows();
    if (item < 0 || static_cast<std::size_t>(item) >= visible.size()) {
        return {};
    }
    const std::size_t sourceIndex = visible[static_cast<std::size_t>(item)];
    return sourceIndex < rows.size() ? rows[sourceIndex].stableKey : std::wstring{};
}

void ApplyValueFilter(RegistryViewState& state, RegistryFilterResult result) {
    if (!state.list.hwnd() || result.generation != state.displayGeneration || result.query != state.valueFilterQuery ||
        result.useRegex != state.valueFilterUseRegex) {
        return;
    }
    state.list.setVisibleIndexes(std::move(result.visibleIndexes));
    const auto& visible = state.list.visibleIndexes();
    const auto& rows = state.list.rows();
    HWND list = state.list.hwnd();
    int selectedItem = -1;
    int topItem = -1;
    for (std::size_t item = 0; item < visible.size(); ++item) {
        const std::size_t source = visible[item];
        if (source >= rows.size()) {
            continue;
        }
        if (selectedItem < 0 && rows[source].stableKey == result.selectedStableKey) {
            selectedItem = static_cast<int>(item);
        }
        if (topItem < 0 && rows[source].stableKey == result.topStableKey) {
            topItem = static_cast<int>(item);
        }
    }
    ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    if (selectedItem < 0 && !visible.empty()) {
        selectedItem = 0;
    }
    if (selectedItem >= 0) {
        ListView_SetItemState(list, selectedItem, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        SyncEditorFromSelection(state);
    } else {
        ::SetWindowTextW(state.nameEdit, L"");
        ::SetWindowTextW(state.dataEdit, L"");
    }
    if (topItem >= 0) {
        ListView_EnsureVisible(list, topItem, FALSE);
    }
    if (!result.query.empty()) {
        SetStatus(state, L"注册表筛选结果 " + std::to_wstring(visible.size()) + L" / " +
            std::to_wstring(rows.size()) + L" 项。");
    }
}

void RequestValueFilter(RegistryViewState& state,
    std::wstring query,
    std::wstring selectedStableKey,
    std::wstring topStableKey) {
    state.valueFilterQuery = std::move(query);
    state.valueFilterUseRegex = Ksword::Ui::GetFilterBarRegexEnabled(state.valueFilterBar);
    const auto rows = state.filterRows;
    const std::uint64_t generation = state.displayGeneration;
    const bool useRegex = state.valueFilterUseRegex;
    if (!state.filterTask || !rows) {
        return;
    }
    state.filterTask->request(
        [rows, generation, useRegex, query = state.valueFilterQuery, selectedStableKey = std::move(selectedStableKey), topStableKey = std::move(topStableKey)]() mutable {
            RegistryFilterResult result{};
            result.generation = generation;
            result.query = std::move(query);
            result.useRegex = useRegex;
            result.selectedStableKey = std::move(selectedStableKey);
            result.topStableKey = std::move(topStableKey);
            result.visibleIndexes = Ksword::Ui::VirtualListView::FilterRowIndexes(*rows, result.query, useRegex);
            return result;
        },
        [&state](std::uint64_t, std::optional<RegistryFilterResult>&& result, std::exception_ptr error) {
            if (error || !result.has_value()) {
                SetStatus(state, L"注册表筛选任务异常结束，已保留当前可见结果。");
                return;
            }
            ApplyValueFilter(state, std::move(*result));
        });
}

void PopulateList(RegistryViewState& state) {
    const std::wstring selectedStableKey = StableKeyFromListItem(state, ListView_GetNextItem(state.list.hwnd(), -1, LVNI_SELECTED));
    const std::wstring topStableKey = StableKeyFromListItem(state, ListView_GetTopIndex(state.list.hwnd()));
    auto rows = std::make_shared<std::vector<Ksword::Ui::VirtualListRow>>();
    rows->reserve(state.snapshot.rows.size());
    for (std::size_t index = 0; index < state.snapshot.rows.size(); ++index) {
        const RegistryEntry& row = state.snapshot.rows[index];
        if (row.kind != RegistryRowKind::Value) {
            continue;
        }
        Ksword::Ui::VirtualListRow displayRow{};
        displayRow.stableKey = StableKeyForRegistryEntry(row);
        displayRow.itemData = static_cast<LPARAM>(index);
        displayRow.cells = {
            row.name.empty() ? std::wstring(L"(Default)") : row.name,
            row.typeText,
            row.dataText,
            row.detailText
        };
        rows->push_back(std::move(displayRow));
    }
    // Holding the redraw lock keeps the control from painting the freshly loaded
    // rows with the previous filter's visible set before the filter request lands.
    {
        Ksword::Ui::ScopedListViewRedrawLock listRedraw(state.list.hwnd());
        // 直接交出共享快照，不再把整份行表按值复制一遍（每行 4 个 wstring）。
        state.list.setSharedRows(rows);
        state.list.setVisibleIndexes({});
    }
    state.filterRows = std::move(rows);
    ++state.displayGeneration;
    RequestValueFilter(state,
        state.valueFilterBar ? Ksword::Ui::GetFilterBarText(state.valueFilterBar) : state.valueFilterQuery,
        selectedStableKey,
        topStableKey);
}

void RefreshSnapshot(RegistryViewState& state) {
    if (!state.refreshTask) {
        return;
    }
    const std::wstring current = CurrentPath(state);
    const RegistryViewMode mode = state.mode;
    const std::uint64_t navigationGeneration = state.navigationGeneration;
    const bool firstLoad = state.list.rows().empty();
    SetStatus(state, state.refreshTask->running()
        ? L"注册表刷新已排队，等待当前快照完成…"
        : L"正在后台枚举注册表键和值…");
    if (state.refreshButton) {
        ::EnableWindow(state.refreshButton, FALSE);
    }
    if (firstLoad) {
        Ksword::Ui::SetLoadingOverlay(state.loadingOverlay, true, L"正在后台加载注册表键和值…");
    }
    state.refreshTask->request(
        [current, mode, navigationGeneration]() {
            RegistryRefreshSnapshot snapshot{};
            snapshot.path = current;
            snapshot.mode = mode;
            snapshot.navigationGeneration = navigationGeneration;
            snapshot.snapshot = EnumerateRegistryKey(current, mode);
            return snapshot;
        },
        [&state, navigationGeneration](std::uint64_t, std::optional<RegistryRefreshSnapshot>&& snapshot, std::exception_ptr error) {
            if (navigationGeneration != state.navigationGeneration) {
                return;
            }
            if (state.refreshButton) {
                ::EnableWindow(state.refreshButton, TRUE);
            }
            Ksword::Ui::SetLoadingOverlay(state.loadingOverlay, false);
            if (error || !snapshot.has_value()) {
                SetStatus(state, L"注册表后台枚举异常结束。请检查权限、路径和驱动状态。");
                return;
            }
            if (snapshot->navigationGeneration != state.navigationGeneration ||
                snapshot->path != CurrentPath(state) || snapshot->mode != state.mode) {
                return;
            }
            state.snapshot = std::move(snapshot->snapshot);
            state.currentSnapshotReady = state.snapshot.success;
            PopulateList(state);
            SetOperationControlsEnabled(state, state.currentSnapshotReady && !state.operationInProgress);
            SetStatus(state, state.snapshot.statusText);
        });
}

void RebuildRegistryTree(RegistryViewState& state) {
    // Input is the current view state. Processing recreates only the five root
    // nodes with placeholder children; real subkeys are still loaded lazily.
    // No value is returned.
    if (!state.tree.hwnd()) {
        return;
    }
    FillTreeRoots(state);
    SelectPathInTree(state, CurrentPath(state));
}

// PrepareModeForExternalNavigation preserves the selected R0 view when it can
// address the requested key, but moves roots such as HKCR/HKCU/HKCC to the
// already-supported WinAPI view before the external route reports success.
void PrepareModeForExternalNavigation(RegistryViewState& state, const std::wstring& path) {
    if (state.mode != RegistryViewMode::R0) {
        return;
    }
    const RegistryPathInfo parsed = ParseRegistryPath(path);
    if (!parsed.valid || !parsed.kernelPath.empty()) {
        return;
    }
    state.mode = RegistryViewMode::WinApi;
    if (state.modeCombo) {
        ::SendMessageW(state.modeCombo, CB_SETCURSEL, 0, 0);
    }
    RebuildRegistryTree(state);
}

void NavigateTo(RegistryViewState& state, const std::wstring& path) {
    InvalidateCurrentSnapshot(state);
    // SetWindowTextW 会同步触发 pathEdit 的 EN_CHANGE，那里会把刚作废过的快照
    // 再作废一遍。守卫让一次导航只作废一次。
    state.suppressPathEditChange = true;
    ::SetWindowTextW(state.pathEdit, path.c_str());
    state.suppressPathEditChange = false;
    SelectPathInTree(state, path);
    RefreshSnapshot(state);
}

HWND CreateEdit(HWND parent, int id, DWORD style) {
    HWND hwnd = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), ::GetModuleHandleW(nullptr), nullptr);
    if (hwnd) {
        ::SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(Ksword::Ui::SystemUIFont()), TRUE);
    }
    return hwnd;
}

HWND CreateCombo(HWND parent, int id) {
    HWND hwnd = ::CreateWindowExW(0, WC_COMBOBOXW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL,
        0, 0, 0, 200, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), ::GetModuleHandleW(nullptr), nullptr);
    if (hwnd) {
        ::SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(Ksword::Ui::SystemUIFont()), TRUE);
    }
    return hwnd;
}

void LayoutChildren(RegistryViewState& state) {
    RECT rc{};
    ::GetClientRect(state.hwnd, &rc);
    const int width = Width(rc);
    const int height = Height(rc);
    const int gap = 6;
    const int margin = 6;
    const int toolbarHeight = 24;
    const int actionRowHeight = 24;
    const int treeWidth = std::clamp(width / 4, 220, 360);
    const int editorWidth = std::max(260, width / 3);
    const int listWidth = std::max(200, width - treeWidth - editorWidth - gap * 4 - margin * 2);

    int x = margin;
    int y = margin;
    ::MoveWindow(state.refreshButton, x, y, 58, toolbarHeight, TRUE); x += 58 + gap;
    ::MoveWindow(state.upButton, x, y, 58, toolbarHeight, TRUE); x += 58 + gap;
    ::MoveWindow(state.goButton, x, y, 58, toolbarHeight, TRUE); x += 58 + gap;
    ::MoveWindow(state.modeCombo, x, y, 120, 300, TRUE); x += 120 + gap;
    ::MoveWindow(state.pathEdit, x, y, std::max(120, width - x - margin), toolbarHeight, TRUE);

    const int filterTop = y + toolbarHeight + gap;
    const int treeLeft = margin;
    const int listLeft = treeLeft + treeWidth + gap;
    const int editorLeft = listLeft + listWidth + gap;
    ::MoveWindow(state.treeFilterBar, treeLeft, filterTop, treeWidth, toolbarHeight, TRUE);
    ::MoveWindow(state.valueFilterBar, listLeft, filterTop, listWidth, toolbarHeight, TRUE);
    const int contentTop = filterTop + toolbarHeight + gap;
    // 状态文字画在 height-22..height-2，内容区不能越过它。
    // 原来 listHeight 用 max(120, height-contentTop-92) 硬夹，客户区 ≤ 207 时
    // 列表和目录树就压到状态行上。
    const int statusTop = height - 22;
    const int contentLimit = std::max(0, statusTop - gap - contentTop);
    const int listHeight = std::max(0, std::min(std::max(120, height - contentTop - 92), contentLimit));
    ::MoveWindow(state.tree.hwnd(), treeLeft, contentTop, treeWidth, listHeight, TRUE);
    ::MoveWindow(state.list.hwnd(), listLeft, contentTop, listWidth, listHeight, TRUE);
    if (state.loadingOverlay) {
        ::MoveWindow(state.loadingOverlay, listLeft, contentTop, listWidth, listHeight, TRUE);
    }

    int editY = contentTop;
    ::MoveWindow(state.nameEdit, editorLeft, editY, editorWidth, 24, TRUE);
    editY += 24 + gap;
    ::MoveWindow(state.typeCombo, editorLeft, editY, editorWidth, 220, TRUE);
    editY += 24 + gap;
    // 编辑列固定行（名称 24 + 类型 24 + 两排按钮 2*24 + 三处间距）合计 114px，
    // 数据框吃掉剩下的高度。原来的 max(90, ...) 下限会在矮窗口下把两排按钮
    // 顶到状态行上（客户区 ≤ 297 开始重叠），改成按剩余空间收缩。
    const int dataEditTop = editY;
    const int dataEditHeight = std::max(0, std::min(
        std::max(90, listHeight - 24 - gap - actionRowHeight * 2),
        contentLimit - 24 - gap - 24 - gap - gap - actionRowHeight * 2));
    ::MoveWindow(state.dataEdit, editorLeft, dataEditTop, editorWidth, dataEditHeight, TRUE);

    // 两排操作按钮贴着状态行上沿摆，绝不越过它。空间够时用自然位置（与原布局
    // 逐像素一致），不够时整体上移；此时数据框已被压到很小，不会互相遮挡。
    const int actionBlockHeight = actionRowHeight * 2 + gap;
    const int actionTop = std::min(dataEditTop + dataEditHeight + gap,
        std::max(contentTop, statusTop - gap - actionBlockHeight));
    ::MoveWindow(state.readButton, editorLeft, actionTop, 58, actionRowHeight, TRUE);
    ::MoveWindow(state.writeButton, editorLeft + 58 + gap, actionTop, 58, actionRowHeight, TRUE);
    ::MoveWindow(state.createKeyButton, editorLeft + (58 + gap) * 2, actionTop, 70, actionRowHeight, TRUE);
    ::MoveWindow(state.deleteButton, editorLeft, actionTop + actionRowHeight + gap, 58, actionRowHeight, TRUE);
    ::MoveWindow(state.renameButton, editorLeft + 58 + gap, actionTop + actionRowHeight + gap, 70, actionRowHeight, TRUE);

    ::MoveWindow(state.statusText, margin, statusTop, width - margin * 2, 20, TRUE);
}

RegistryOperationSnapshot ExecuteRegistryOperation(const RegistryOperationRequest& request) {
    RegistryOperationSnapshot snapshot{};
    snapshot.kind = request.kind;
    switch (request.kind) {
    case RegistryOperationKind::Read:
        snapshot.result = ReadRegistryValue(request.path, request.name, request.mode);
        break;
    case RegistryOperationKind::Write:
        snapshot.result = WriteRegistryValue(request.path, request.name, request.valueType, request.data, request.mode);
        snapshot.refreshRequired = snapshot.result.success;
        break;
    case RegistryOperationKind::CreateKey:
        snapshot.result = CreateRegistryKey(request.path, request.mode);
        snapshot.refreshRequired = snapshot.result.success;
        break;
    case RegistryOperationKind::DeleteValue:
        snapshot.result = DeleteRegistryValue(request.path, request.name, request.mode);
        snapshot.refreshRequired = snapshot.result.success;
        break;
    case RegistryOperationKind::DeleteKey:
        snapshot.result = DeleteRegistryKey(request.path, request.mode);
        snapshot.refreshRequired = snapshot.result.success;
        break;
    case RegistryOperationKind::RenameValue:
        snapshot.result = RenameRegistryValue(request.path, request.name, request.alternateName, request.mode);
        snapshot.refreshRequired = snapshot.result.success;
        break;
    case RegistryOperationKind::RenameKey:
        snapshot.result = RenameRegistryKey(request.path, request.alternateName, request.mode);
        snapshot.refreshRequired = snapshot.result.success;
        break;
    }
    return snapshot;
}

void SetOperationControlsEnabled(RegistryViewState& state, bool enabled) {
    for (HWND control : { state.readButton, state.writeButton, state.createKeyButton, state.deleteButton, state.renameButton }) {
        if (control) {
            ::EnableWindow(control, enabled);
        }
    }
}

// InvalidateCurrentSnapshot prevents a row captured for one registry key or
// transport mode from being used after navigation begins. The next matching
// background snapshot is the only point that re-enables registry operations.
void InvalidateCurrentSnapshot(RegistryViewState& state) {
    ++state.navigationGeneration;
    state.currentSnapshotReady = false;
    state.snapshot = {};
    state.filterRows.reset();
    state.list.setRows({});
    ++state.displayGeneration;
    if (state.nameEdit) {
        ::SetWindowTextW(state.nameEdit, L"");
    }
    if (state.dataEdit) {
        ::SetWindowTextW(state.dataEdit, L"");
    }
    SetOperationControlsEnabled(state, false);
}

void BeginRegistryOperation(RegistryViewState& state, RegistryOperationRequest request) {
    if (!state.currentSnapshotReady) {
        SetStatus(state, L"当前注册表快照尚未就绪，不能执行读写操作。");
        return;
    }
    if (!state.operationTask || state.operationInProgress) {
        SetStatus(state, L"注册表操作正在执行。");
        return;
    }
    const std::uint64_t operationGeneration = state.navigationGeneration;
    state.operationInProgress = true;
    SetOperationControlsEnabled(state, false);
    SetStatus(state, L"正在后台执行注册表操作…");
    state.operationTask->request(
        [request = std::move(request)] { return ExecuteRegistryOperation(request); },
        [&state, operationGeneration](std::uint64_t, std::optional<RegistryOperationSnapshot>&& snapshot, std::exception_ptr error) {
            state.operationInProgress = false;
            if (operationGeneration != state.navigationGeneration) {
                SetOperationControlsEnabled(state, state.currentSnapshotReady);
                return;
            }
            if (error || !snapshot.has_value()) {
                SetOperationControlsEnabled(state, state.currentSnapshotReady);
                SetStatus(state, L"注册表操作异常结束。请检查权限、路径和驱动状态。");
                return;
            }
            SetStatus(state, snapshot->result.statusText);
            if (snapshot->kind == RegistryOperationKind::Read && snapshot->result.success) {
                SetSelectedRegistryType(state, snapshot->result.valueType);
                RegistryEntry entry;
                entry.valueType = snapshot->result.valueType;
                entry.data = snapshot->result.data;
                ::SetWindowTextW(state.dataEdit, FormatDataForEditor(entry).c_str());
            }
            if (snapshot->refreshRequired) {
                InvalidateCurrentSnapshot(state);
                RefreshSnapshot(state);
                return;
            }
            SetOperationControlsEnabled(state, state.currentSnapshotReady);
        });
}

bool ConfirmMutation(HWND owner, const wchar_t* action, const std::wstring& targetPath) {
    const std::wstring prompt = std::wstring(L"该操作将修改注册表：") + action +
        L"。\n\n目标键：" + targetPath + L"\n是否继续？";
    return ::MessageBoxW(owner, prompt.c_str(), L"确认注册表操作", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) == IDYES;
}

void CreateSubKeyFromEditor(RegistryViewState& state) {
    const std::wstring childName = WindowTextOf(state.nameEdit);
    if (childName.empty()) {
        SetStatus(state, L"Create key needs a key name in the name edit.");
        return;
    }
    RegistryOperationRequest request{};
    request.kind = RegistryOperationKind::CreateKey;
    request.path = ChildPath(CurrentPath(state), childName);
    request.mode = state.mode;
    if (!ConfirmMutation(state.hwnd, L"创建子键", request.path)) {
        return;
    }
    BeginRegistryOperation(state, std::move(request));
}

void DeleteSelection(RegistryViewState& state) {
    RegistryEntry* entry = nullptr;
    if (!SelectedEntry(state, nullptr, &entry) || entry == nullptr) {
        SetStatus(state, L"No registry row is selected.");
        return;
    }
    RegistryOperationRequest request{};
    request.path = CurrentPath(state);
    request.mode = state.mode;
    if (entry->kind == RegistryRowKind::SubKey) {
        request.kind = RegistryOperationKind::DeleteKey;
        request.path = ChildPath(request.path, entry->name);
    } else {
        request.kind = RegistryOperationKind::DeleteValue;
        request.name = entry->name;
    }
    if (!ConfirmMutation(state.hwnd, L"删除选中项", request.path)) {
        return;
    }
    BeginRegistryOperation(state, std::move(request));
}

void RenameSelection(RegistryViewState& state) {
    RegistryEntry* entry = nullptr;
    if (!SelectedEntry(state, nullptr, &entry) || entry == nullptr) {
        SetStatus(state, L"No registry row is selected.");
        return;
    }
    const std::wstring newName = WindowTextOf(state.nameEdit);
    if (newName.empty()) {
        SetStatus(state, L"Rename needs a non-empty name.");
        return;
    }
    RegistryOperationRequest request{};
    request.path = CurrentPath(state);
    request.mode = state.mode;
    request.alternateName = newName;
    if (entry->kind == RegistryRowKind::SubKey) {
        request.kind = RegistryOperationKind::RenameKey;
        request.path = ChildPath(request.path, entry->name);
    } else {
        request.kind = RegistryOperationKind::RenameValue;
        request.name = entry->name;
    }
    if (!ConfirmMutation(state.hwnd, L"重命名选中项", request.path)) {
        return;
    }
    BeginRegistryOperation(state, std::move(request));
}

void ReadCurrentValue(RegistryViewState& state) {
    RegistryOperationRequest request{};
    request.kind = RegistryOperationKind::Read;
    request.path = CurrentPath(state);
    request.name = WindowTextOf(state.nameEdit);
    request.mode = state.mode;
    BeginRegistryOperation(state, std::move(request));
}

void WriteCurrentValue(RegistryViewState& state) {
    const std::wstring valueName = WindowTextOf(state.nameEdit);
    std::vector<std::uint8_t> bytes;
    std::wstring errorText;
    const std::uint32_t type = SelectedRegistryType(state);
    if (!ParseRegistryDataText(type, WindowTextOf(state.dataEdit), bytes, errorText)) {
        SetStatus(state, errorText);
        return;
    }
    RegistryOperationRequest request{};
    request.kind = RegistryOperationKind::Write;
    request.path = CurrentPath(state);
    request.name = valueName;
    request.mode = state.mode;
    request.valueType = type;
    request.data = std::move(bytes);
    if (!ConfirmMutation(state.hwnd, L"写入值", request.path)) {
        return;
    }
    BeginRegistryOperation(state, std::move(request));
}

std::wstring RegistryRowsAsText(const RegistryViewState& state, bool allVisible) {
    const HWND list = state.list.hwnd();
    const auto& visible = state.list.visibleIndexes();
    const auto& rows = state.list.rows();
    std::wstring text;
    for (std::size_t item = 0; item < visible.size(); ++item) {
        if (!allVisible && (!list || (ListView_GetItemState(list, static_cast<int>(item), LVIS_SELECTED) & LVIS_SELECTED) == 0)) {
            continue;
        }
        const std::size_t sourceIndex = visible[item];
        if (sourceIndex >= rows.size()) {
            continue;
        }
        const auto& cells = rows[sourceIndex].cells;
        for (std::size_t column = 0; column < cells.size(); ++column) {
            if (column != 0) {
                text += L'\t';
            }
            text += cells[column];
        }
        text += L"\r\n";
    }
    return text;
}

std::wstring RegistrySelectedCellText(const RegistryViewState& state) {
    const HWND list = state.list.hwnd();
    const int selected = list ? ListView_GetNextItem(list, -1, LVNI_SELECTED) : -1;
    const auto& visible = state.list.visibleIndexes();
    const auto& rows = state.list.rows();
    if (selected < 0 || static_cast<std::size_t>(selected) >= visible.size()) {
        return {};
    }
    const std::size_t source = visible[static_cast<std::size_t>(selected)];
    if (source >= rows.size() || state.contextColumn < 0 || static_cast<std::size_t>(state.contextColumn) >= rows[source].cells.size()) {
        return {};
    }
    return rows[source].cells[static_cast<std::size_t>(state.contextColumn)];
}

// ExportVisibleRegistrySnapshot writes only the values already materialized in
// the current ListView. It never re-reads the registry, recursively exports a
// key, or turns a partial R0 snapshot into a claimed .reg backup.
void ExportVisibleRegistrySnapshot(RegistryViewState& state) {
    if (!state.currentSnapshotReady) {
        SetStatus(state, L"当前注册表快照尚未就绪，无法导出。");
        return;
    }

    const std::wstring text = Ksword::Ui::BuildVisibleVirtualListTsv(
        { L"名称", L"类型", L"数据", L"详情" }, state.list);
    if (text.empty()) {
        SetStatus(state, L"当前没有可导出的注册表可见值。");
        return;
    }

    std::wstring error;
    switch (Ksword::Ui::SaveUtf8TextFileWithDialog(
        state.hwnd,
        L"ksword-arklight-registry-visible.tsv",
        L"导出当前可见注册表快照",
        L"TSV (*.tsv)\0*.tsv\0All Files (*.*)\0*.*\0",
        L"tsv",
        text,
        &error)) {
    case Ksword::Ui::SaveTextFileResult::Saved:
        SetStatus(state, L"已导出当前可见注册表快照 TSV。");
        break;
    case Ksword::Ui::SaveTextFileResult::Cancelled:
        SetStatus(state, L"已取消导出当前可见注册表快照。");
        break;
    case Ksword::Ui::SaveTextFileResult::Failed:
    default:
        SetStatus(state, L"导出当前可见注册表快照失败：" + error);
        break;
    }
}

void ShowContextMenu(RegistryViewState& state, POINT screenPoint) {
    HMENU menu = ::CreatePopupMenu();
    if (!menu) {
        return;
    }
    POINT client = screenPoint;
    ::ScreenToClient(state.list.hwnd(), &client);
    LVHITTESTINFO hit{};
    hit.pt = client;
    const int clickedItem = ListView_SubItemHitTest(state.list.hwnd(), &hit);
    ListView_SetItemState(state.list.hwnd(), -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    if (clickedItem >= 0 && static_cast<std::size_t>(clickedItem) < state.list.visibleIndexes().size()) {
        ListView_SetItemState(state.list.hwnd(), clickedItem, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        state.contextColumn = hit.iSubItem;
        SyncEditorFromSelection(state);
    }
    const bool hasSelection = SelectedEntry(state, nullptr, nullptr);
    const bool canOperate = state.currentSnapshotReady && !state.operationInProgress;
    const bool canExportVisible = state.currentSnapshotReady && !state.list.visibleIndexes().empty();
    ::AppendMenuW(menu, MF_STRING, kMenuRefresh, L"刷新");
    ::AppendMenuW(menu, MF_STRING | (hasSelection ? 0U : MF_GRAYED), kMenuCopyName, L"复制名称");
    ::AppendMenuW(menu, MF_STRING | (hasSelection ? 0U : MF_GRAYED), kMenuCopyData, L"复制数据");
    ::AppendMenuW(menu, MF_STRING | (hasSelection ? 0U : MF_GRAYED), kMenuCopyCell, L"复制单元格");
    ::AppendMenuW(menu, MF_STRING | (hasSelection ? 0U : MF_GRAYED), kMenuCopyRow, L"复制行");
    ::AppendMenuW(menu, MF_STRING | (!state.list.visibleIndexes().empty() ? 0U : MF_GRAYED), kMenuCopyVisible, L"复制可见结果");
    ::AppendMenuW(menu, MF_STRING | (canExportVisible ? 0U : MF_GRAYED), kMenuExportVisible, L"导出当前可见值 TSV");
    ::AppendMenuW(menu, MF_STRING | (canOperate ? 0U : MF_GRAYED), kMenuRead, L"读取值");
    ::AppendMenuW(menu, MF_STRING | (canOperate ? 0U : MF_GRAYED), kMenuWrite, L"写入值");
    ::AppendMenuW(menu, MF_STRING | (canOperate ? 0U : MF_GRAYED), kMenuCreateSubKey, L"创建子键");
    ::AppendMenuW(menu, MF_STRING | (canOperate && hasSelection ? 0U : MF_GRAYED), kMenuDelete, L"删除");
    ::AppendMenuW(menu, MF_STRING | (canOperate && hasSelection ? 0U : MF_GRAYED), kMenuRename, L"重命名");

    const UINT command = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, 0, state.hwnd, nullptr);
    ::DestroyMenu(menu);
    if (command == 0) {
        return;
    }

    RegistryEntry* entry = nullptr;
    switch (command) {
    case kMenuRefresh:
        RefreshSnapshot(state);
        break;
    case kMenuCopyName:
        if (SelectedEntry(state, nullptr, &entry) && entry) {
            SetStatus(state, CopyRegistryTextToClipboard(state.hwnd, entry->name) ? L"Name copied." : L"Copy name failed.");
        }
        break;
    case kMenuCopyData:
        if (SelectedEntry(state, nullptr, &entry) && entry) {
            SetStatus(state, CopyRegistryTextToClipboard(state.hwnd, entry->dataText) ? L"Data copied." : L"Copy data failed.");
        }
        break;
    case kMenuCopyCell:
        SetStatus(state, CopyRegistryTextToClipboard(state.hwnd, RegistrySelectedCellText(state)) ? L"已复制单元格。" : L"复制单元格失败。");
        break;
    case kMenuCopyRow:
        SetStatus(state, CopyRegistryTextToClipboard(state.hwnd, RegistryRowsAsText(state, false)) ? L"已复制注册表行。" : L"复制注册表行失败。");
        break;
    case kMenuCopyVisible:
        SetStatus(state, CopyRegistryTextToClipboard(state.hwnd, RegistryRowsAsText(state, true)) ? L"已复制可见注册表结果。" : L"复制可见注册表结果失败。");
        break;
    case kMenuExportVisible:
        ExportVisibleRegistrySnapshot(state);
        break;
    case kMenuRead:
        ReadCurrentValue(state);
        break;
    case kMenuWrite:
        WriteCurrentValue(state);
        break;
    case kMenuCreateSubKey:
        CreateSubKeyFromEditor(state);
        break;
    case kMenuDelete:
        DeleteSelection(state);
        break;
    case kMenuRename:
        RenameSelection(state);
        break;
    default:
        break;
    }
}

bool CreateChildControls(RegistryViewState& state) {
    state.refreshButton = Ksword::Ui::CreateButton(state.hwnd, kRefreshButtonId, L"刷新", 0, 0, 0, 0);
    state.upButton = Ksword::Ui::CreateButton(state.hwnd, kUpButtonId, L"上一级", 0, 0, 0, 0);
    state.pathEdit = CreateEdit(state.hwnd, kPathEditId, ES_AUTOHSCROLL);
    state.goButton = Ksword::Ui::CreateButton(state.hwnd, kGoButtonId, L"转到", 0, 0, 0, 0);
    state.modeCombo = CreateCombo(state.hwnd, kModeComboId);
    // 树只在这里创建一次。LazyTreeView 自己持有节点记录与 key 索引，页面只
    // 需要提供"这个节点该取哪些子节点"的回调。
    state.tree.setExpansionCallback([&state](HTREEITEM item, const std::wstring& key) {
        return RequestTreeChildren(state, item, key);
    });
    const bool treeCreated = state.tree.create(state.hwnd, kTreeId, 0, 0, 0, 0);
    state.treeFilterBar = Ksword::Ui::CreateFilterBar(state.hwnd, kTreeFilterBarId, L"筛选已加载的树节点", 0, 0, 0, 0);
    state.valueFilterBar = Ksword::Ui::CreateFilterBar(state.hwnd, kValueFilterBarId, L"筛选名称、类型、数据和详情", 0, 0, 0, 0);
    state.nameEdit = CreateEdit(state.hwnd, kNameEditId, ES_AUTOHSCROLL);
    state.typeCombo = CreateCombo(state.hwnd, kTypeComboId);
    state.dataEdit = CreateEdit(state.hwnd, kDataEditId, ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL);
    // Writable, so this one also gets replace: REG_MULTI_SZ and long REG_SZ
    // payloads are routinely edited by hand here.
    Ksword::Ui::AttachTextFindSupport(state.dataEdit);
    state.readButton = Ksword::Ui::CreateButton(state.hwnd, kReadButtonId, L"读取", 0, 0, 0, 0);
    state.writeButton = Ksword::Ui::CreateButton(state.hwnd, kWriteButtonId, L"写入", 0, 0, 0, 0);
    state.createKeyButton = Ksword::Ui::CreateButton(state.hwnd, kCreateKeyButtonId, L"建子键", 0, 0, 0, 0);
    state.deleteButton = Ksword::Ui::CreateButton(state.hwnd, kDeleteButtonId, L"删除", 0, 0, 0, 0);
    state.renameButton = Ksword::Ui::CreateButton(state.hwnd, kRenameButtonId, L"重命名", 0, 0, 0, 0);
    state.statusText = Ksword::Ui::CreateText(state.hwnd, kStatusId, L"", 0, 0, 0, 0);
    state.loadingOverlay = Ksword::Ui::CreateLoadingOverlay(state.hwnd, kLoadingOverlayId, { 0, 0, 1, 1 });
    if (!state.refreshButton || !state.upButton || !state.pathEdit || !state.goButton || !state.modeCombo || !treeCreated || !state.treeFilterBar || !state.valueFilterBar ||
        !state.nameEdit || !state.typeCombo || !state.dataEdit || !state.readButton || !state.writeButton ||
        !state.createKeyButton || !state.deleteButton || !state.renameButton || !state.statusText || !state.loadingOverlay ||
        !state.list.create(state.hwnd, kListId, 0, 0, 0, 0, LVS_SHOWSELALWAYS)) {
        return false;
    }

    ::SendMessageW(state.modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"WinAPI"));
    ::SendMessageW(state.modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"R0"));
    ::SendMessageW(state.modeCombo, CB_SETCURSEL, 0, 0);
    for (const RegistryTypeOption& option : kTypeOptions) {
        ::SendMessageW(state.typeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(option.text));
    }
    ::SendMessageW(state.typeCombo, CB_SETCURSEL, 0, 0);
    Ksword::Ui::AddListViewColumns(state.list.hwnd(), RegistryColumns());
    ListView_SetExtendedListViewStyle(state.list.hwnd(), LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES | LVS_EX_LABELTIP);
    ::SetWindowTextW(state.pathEdit, L"HKLM\\SOFTWARE");
    SetOperationControlsEnabled(state, false);
    SetStatus(state, L"Registry dock ready.");
    return true;
}

LRESULT CALLBACK RegistryViewProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    RegistryViewState* state = StateFromWindow(hwnd);
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = create ? static_cast<RegistryViewState*>(create->lpCreateParams) : nullptr;
        if (state) {
            state->hwnd = hwnd;
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        }
    }

    switch (msg) {
    case WM_CREATE:
        if (state) {
            if (!CreateChildControls(*state)) {
                delete state;
                ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                return -1;
            }
            state->refreshTask = std::make_unique<Ksword::Ui::AsyncSnapshotTask<RegistryRefreshSnapshot>>(hwnd, kMsgSnapshotCompleted);
            state->treeChildrenTask = std::make_unique<Ksword::Ui::AsyncSnapshotTask<RegistryTreeChildrenSnapshot>>(hwnd, kMsgTreeChildrenCompleted);
            state->filterTask = std::make_unique<Ksword::Ui::AsyncSnapshotTask<RegistryFilterResult>>(hwnd, kMsgFilterCompleted);
            state->operationTask = std::make_unique<Ksword::Ui::AsyncSnapshotTask<RegistryOperationSnapshot>>(hwnd, kMsgOperationCompleted);
            LayoutChildren(*state);
            RebuildRegistryTree(*state);
            RefreshSnapshot(*state);
        }
        return 0;
    case WM_SIZE:
        if (state) {
            LayoutChildren(*state);
        }
        return 0;
    case kMsgSnapshotCompleted:
        if (state && state->refreshTask && state->refreshTask->consume(hwnd, wParam, lParam)) {
            return 0;
        }
        break;
    case kMsgTreeChildrenCompleted:
        if (state && state->treeChildrenTask && state->treeChildrenTask->consume(hwnd, wParam, lParam)) {
            return 0;
        }
        break;
    case kMsgFilterCompleted:
        if (state && state->filterTask && state->filterTask->consume(hwnd, wParam, lParam)) {
            return 0;
        }
        break;
    case kMsgOperationCompleted:
        if (state && state->operationTask && state->operationTask->consume(hwnd, wParam, lParam)) {
            return 0;
        }
        break;
    case kMsgExternalNavigate:
        if (state && lParam != 0) {
            const auto* path = reinterpret_cast<const std::wstring*>(lParam);
            if (!path->empty()) {
                PrepareModeForExternalNavigation(*state, *path);
                NavigateTo(*state, *path);
                return TRUE;
            }
        }
        return FALSE;
    case WM_NOTIFY:
        if (state) {
            const auto* header = reinterpret_cast<const NMHDR*>(lParam);
            // 树控件的展开/删除/合成行选中由 LazyTreeView 自己消费；它返回真时
            // 页面不需要再做任何事。
            if (header) {
                LRESULT treeResult = 0;
                if (state->tree.handleNotify(*header, treeResult)) {
                    return treeResult;
                }
            }
            if (header && header->hwndFrom == state->list.hwnd()) {
                LRESULT result = 0;
                if (state->list.handleNotify(*header, result)) {
                    return result;
                }
            }
            if (header && header->hwndFrom == state->list.hwnd() && header->code == LVN_COLUMNCLICK) {
                const auto* clicked = reinterpret_cast<const NMLISTVIEW*>(lParam);
                if (clicked) {
                    const Ksword::Ui::SortSpec next =
                        Ksword::Ui::NextSortSpec(state->list.sortSpec(), clicked->iSubItem);
                    state->list.applySort(next);
                    Ksword::Ui::ShowSortIndicator(state->list.hwnd(), next);
                }
                return 0;
            }
            if (header && header->hwndFrom == state->list.hwnd() && header->code == LVN_ITEMCHANGED) {
                const auto* changed = reinterpret_cast<const NMLISTVIEW*>(lParam);
                if ((changed->uNewState & LVIS_SELECTED) != 0) {
                    SyncEditorFromSelection(*state);
                }
                return 0;
            }
            if (header && header->hwndFrom == state->list.hwnd() && header->code == NM_DBLCLK) {
                RegistryEntry* entry = nullptr;
                if (SelectedEntry(*state, nullptr, &entry) && entry && entry->kind == RegistryRowKind::SubKey) {
                    NavigateTo(*state, ChildPath(CurrentPath(*state), entry->name));
                }
                return 0;
            }
            if (header && header->hwndFrom == state->list.hwnd() && header->code == NM_RCLICK) {
                POINT pt{};
                ::GetCursorPos(&pt);
                ShowContextMenu(*state, pt);
                return 0;
            }
            // TVN_ITEMEXPANDINGW 不再由页面处理：控件负责"没加载就取消展开、发
            // 请求、结果落地后再展开"这一整套，页面只提供取数回调。
            if (header && header->hwndFrom == state->tree.hwnd() && header->code == TVN_SELCHANGEDW) {
                const auto* changed = reinterpret_cast<const NMTREEVIEWW*>(lParam);
                if (changed && !state->tree.silentSelection() && !state->tree.isNotice(changed->itemNew.hItem)) {
                    const std::wstring path = state->tree.keyOf(changed->itemNew.hItem);
                    if (!path.empty()) {
                        NavigateTo(*state, path);
                    }
                }
                return 0;
            }
        }
        break;
    case WM_TIMER:
        if (state && wParam == kTreeFilterTimerId) {
            ::KillTimer(hwnd, kTreeFilterTimerId);
            RebuildRegistryTree(*state);
            return 0;
        }
        break;
    case WM_COMMAND:
        if (state) {
            if (LOWORD(wParam) == kPathEditId && HIWORD(wParam) == EN_CHANGE) {
                if (!state->suppressPathEditChange && state->currentSnapshotReady) {
                    InvalidateCurrentSnapshot(*state);
                    SetStatus(*state, L"路径已更改；请先转到新路径并等待快照加载完成。");
                }
                return 0;
            }
            if (LOWORD(wParam) == kValueFilterBarId && HIWORD(wParam) == EN_CHANGE) {
                RequestValueFilter(*state,
                    Ksword::Ui::GetFilterBarText(state->valueFilterBar),
                    StableKeyFromListItem(*state, ListView_GetNextItem(state->list.hwnd(), -1, LVNI_SELECTED)),
                    StableKeyFromListItem(*state, ListView_GetTopIndex(state->list.hwnd())));
                return 0;
            }
            if (LOWORD(wParam) == kTreeFilterBarId && HIWORD(wParam) == EN_CHANGE) {
                state->treeFilterQuery = Ksword::Ui::GetFilterBarText(state->treeFilterBar);
                // 只记下查询，等用户停手再重建。原先每个 EN_CHANGE 都
                // RebuildRegistryTree：拆整棵树 + 重插根 + 再发起一次 HKLM 枚举。
                ::SetTimer(state->hwnd, kTreeFilterTimerId, kTreeFilterDebounceMs, nullptr);
                return 0;
            }
            switch (LOWORD(wParam)) {
            case kRefreshButtonId:
                RefreshSnapshot(*state);
                return 0;
            case kUpButtonId:
                NavigateTo(*state, ParentPath(CurrentPath(*state)));
                return 0;
            case kGoButtonId:
                NavigateTo(*state, CurrentPath(*state));
                return 0;
            case kReadButtonId:
                ReadCurrentValue(*state);
                return 0;
            case kWriteButtonId:
                WriteCurrentValue(*state);
                return 0;
            case kCreateKeyButtonId:
                CreateSubKeyFromEditor(*state);
                return 0;
            case kDeleteButtonId:
                DeleteSelection(*state);
                return 0;
            case kRenameButtonId:
                RenameSelection(*state);
                return 0;
            case kModeComboId:
                if (HIWORD(wParam) == CBN_SELCHANGE) {
                    state->mode = (::SendMessageW(state->modeCombo, CB_GETCURSEL, 0, 0) == 1)
                        ? RegistryViewMode::R0
                        : RegistryViewMode::WinApi;
                    InvalidateCurrentSnapshot(*state);
                    RebuildRegistryTree(*state);
                    RefreshSnapshot(*state);
                    return 0;
                }
                break;
            default:
                break;
            }
        }
        break;
    case WM_CONTEXTMENU:
        if (state) {
            if (reinterpret_cast<HWND>(wParam) != state->list.hwnd()) {
                return 0;
            }
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (pt.x == -1 && pt.y == -1) {
                RECT rc{};
                ::GetWindowRect(state->list.hwnd(), &rc);
                pt = { rc.left + 16, rc.top + 16 };
            }
            ShowContextMenu(*state, pt);
            return 0;
        }
        break;
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        ::SetBkMode(dc, TRANSPARENT);
        ::SetTextColor(dc, Ksword::Ui::AppTheme().textColor);
        return reinterpret_cast<LRESULT>(Ksword::Ui::AppTheme().windowBrush());
    }
    case WM_NCDESTROY:
        if (state) {
            if (state->refreshTask) {
                state->refreshTask->cancel();
            }
            if (state->treeChildrenTask) {
                state->treeChildrenTask->cancel();
            }
            if (state->filterTask) {
                state->filterTask->cancel();
            }
            if (state->operationTask) {
                state->operationTask->cancel();
            }
            // 控件自己释放节点记录；HWND 随父窗口一起销毁。
            state->tree.reset();
        }
        delete state;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return 0;
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool RegisterRegistryViewClass() {
    static bool registered = false;
    if (registered) {
        return true;
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = RegistryViewProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = Ksword::Ui::AppTheme().windowBrush();
    wc.lpszClassName = kRegistryViewClass;
    if (::RegisterClassW(&wc) || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        registered = true;
    }
    return registered;
}

} // namespace

HWND CreateRegistryView(HWND parent, const RECT& bounds) {
    if (!parent || !RegisterRegistryViewClass()) {
        return nullptr;
    }
    auto* state = new RegistryViewState();
    HWND hwnd = ::CreateWindowExW(
        0,
        kRegistryViewClass,
        L"Registry",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        bounds.left,
        bounds.top,
        bounds.right - bounds.left,
        bounds.bottom - bounds.top,
        parent,
        nullptr,
        ::GetModuleHandleW(nullptr),
        state);
    if (!hwnd) {
        delete state;
    }
    return hwnd;
}

bool RequestRegistryViewNavigate(HWND page, const std::wstring& path) {
    return page && !path.empty() &&
        ::SendMessageW(page, kMsgExternalNavigate, 0, reinterpret_cast<LPARAM>(&path)) != 0;
}

} // namespace Ksword::Features::Registry
