#include "HardwareView.h"

#include "HardwareEnumerator.h"
#include "HardwareModel.h"
#include "../../../Ksword5.1/Ksword5.1/ArkDriverClient/ArkDriverClient.h"
#include "../../Ui/AsyncTask.h"
#include "../../Ui/Controls.h"
#include "../../Ui/FilterBar.h"
#include "../../Ui/LazyTreeView.h"
#include "../../Ui/LoadingOverlay.h"
#include "../../Ui/Theme.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace Ksword::Features::Hardware {
namespace {
constexpr wchar_t kHardwareViewClass[] = L"KswordARKLight.Hardware.DeviceManagerView";
constexpr int kRefreshButtonId = 61001;
constexpr int kTreeId = 61002;
constexpr int kDetailId = 61003;
constexpr int kFilterBarId = 61004;
constexpr int kLoadingOverlayId = 61005;
constexpr int kHeaderHeight = 66;
constexpr int kGap = 6;
constexpr int kTreeWidth = 420;
constexpr UINT kMenuRefresh = 62001;
constexpr UINT kMenuCopyInstanceId = 62002;
constexpr UINT kMenuCopyDetailCell = 62003;
constexpr UINT kMenuCopyDetailRow = 62004;
constexpr UINT kMenuCopyDetailVisible = 62005;
constexpr UINT kMsgRefreshCompleted = WM_APP + 585;
constexpr UINT kMsgFilterCompleted = WM_APP + 586;
constexpr UINT kMsgDetailCompleted = WM_APP + 587;
// One keystroke used to commit an enumeration: every EN_CHANGE fired a filter
// request, so typing "PCI" ran three passes and rebuilt the tree three times.
constexpr UINT_PTR kFilterTimerId = 62010;
constexpr UINT kFilterDebounceMs = 250;
// Upper bound on roots auto-expanded by one filtered rebuild. Roots are cheap to
// leave collapsed, and the control's batch cap only bounds a single parent.
constexpr int kMaxAutoExpandRoots = 20;

struct HardwareDetailSnapshot {
    int deviceIndex = -1;
    std::wstring instanceId;
    HardwareDeviceDetail detail;
    ksword::ark::DeviceAuditResult deviceStack;
    ksword::ark::DeviceAuditResult inputStack;
    ksword::ark::DeviceAuditResult usbTopology;
};

struct HardwareFilterResult {
    std::uint64_t generation = 0;
    std::wstring query;
    bool useRegex = false;
    std::wstring selectedInstanceId;
    std::vector<bool> visibleIndexes;
};

// Width returns a non-negative rectangle width. Input is a RECT; output is the
// usable pixel width used by layout code.
int Width(const RECT& rc) {
    return rc.right > rc.left ? rc.right - rc.left : 0;
}

// Height returns a non-negative rectangle height. Input is a RECT; output is the
// usable pixel height used by layout code.
int Height(const RECT& rc) {
    return rc.bottom > rc.top ? rc.bottom - rc.top : 0;
}

// HardwareViewState owns the child controls and model for one hardware page.
// Inputs arrive through the Win32 window procedure; processing keeps all device
// data local to the Hardware module; no data is shared with other modules.
struct HardwareViewState {
    HWND hwnd = nullptr;
    HWND refreshButton = nullptr;
    HWND filterBar = nullptr;
    Ksword::Ui::LazyTreeView tree;
    HWND detail = nullptr;
    HWND loadingOverlay = nullptr;
    HardwareModel model;
    std::vector<bool> visibleIndexes;
    std::wstring statusText;
    std::wstring filterQuery;
    bool filterUseRegex = false;
    std::uint64_t displayGeneration = 0;
    std::unique_ptr<Ksword::Ui::AsyncSnapshotTask<HardwareEnumerationResult>> refreshTask;
    std::unique_ptr<Ksword::Ui::AsyncSnapshotTask<HardwareFilterResult>> filterTask;
    std::unique_ptr<Ksword::Ui::AsyncSnapshotTask<HardwareDetailSnapshot>> detailTask;
};

// AddListColumn inserts one detail list column. Inputs are list HWND, index,
// title and width; processing sends LVM_INSERTCOLUMNW; no return value.
void AddListColumn(HWND list, int index, const wchar_t* title, int width) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.pszText = const_cast<LPWSTR>(title);
    column.cx = width;
    column.iSubItem = index;
    ListView_InsertColumn(list, index, &column);
}

// SetListText writes one subitem value. Inputs are list HWND, row, column and
// text; processing inserts or updates a list-view item; no return value.
void SetListText(HWND list, int row, int column, const std::wstring& text) {
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

// AddDetailRow appends one property row to the detail list. Inputs are list HWND,
// row index, property name and property value; no value is returned.
void AddDetailRow(HWND list, int row, const std::wstring& name, const std::wstring& value) {
    SetListText(list, row, 0, name);
    SetListText(list, row, 1, value);
}

// WriteClipboardText copies Unicode text to the clipboard. Inputs are an owner
// window and text; processing transfers a movable CF_UNICODETEXT allocation to
// Windows; output reports whether the clipboard accepted it.
bool WriteClipboardText(HWND owner, const std::wstring& text) {
    if (text.empty() || !::OpenClipboard(owner)) {
        return false;
    }

    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) {
        ::CloseClipboard();
        return false;
    }

    void* target = ::GlobalLock(memory);
    if (!target) {
        ::GlobalFree(memory);
        ::CloseClipboard();
        return false;
    }
    std::memcpy(target, text.c_str(), bytes);
    ::GlobalUnlock(memory);

    ::EmptyClipboard();
    if (!::SetClipboardData(CF_UNICODETEXT, memory)) {
        ::GlobalFree(memory);
        ::CloseClipboard();
        return false;
    }
    ::CloseClipboard();
    return true;
}

std::wstring DetailListText(HWND list, int row, int column) {
    std::vector<wchar_t> buffer(4096, L'\0');
    ListView_GetItemText(list, row, column, buffer.data(), static_cast<int>(buffer.size()));
    return std::wstring(buffer.data());
}

std::wstring DetailRowsAsText(HWND list, bool allRows) {
    std::wstring text;
    const int count = list ? ListView_GetItemCount(list) : 0;
    for (int row = 0; row < count; ++row) {
        if (!allRows && (ListView_GetItemState(list, row, LVIS_SELECTED) & LVIS_SELECTED) == 0) {
            continue;
        }
        text += DetailListText(list, row, 0);
        text += L'\t';
        text += DetailListText(list, row, 1);
        text += L"\r\n";
    }
    return text;
}

// Utf8ToWideLossy 将 ArkDriverClient 的窄字符诊断提升为宽字符。
// 输入：通常是 ASCII/UTF-8 风格 message；处理：逐字节提升用于表格展示；
// 返回：宽字符串。
std::wstring Utf8ToWideLossy(const std::string& text) {
    std::wstring wide;
    wide.reserve(text.size());
    for (const char ch : text) {
        wide.push_back(static_cast<unsigned char>(ch));
    }
    return wide;
}

// DeviceAuditSummaryText 生成 R0 设备审计摘要。
// 输入：DeviceAuditResult；处理：组合 IO 状态、行数和设备/驱动计数；
// 返回：一行详情文本。
std::wstring DeviceAuditSummaryText(const ksword::ark::DeviceAuditResult& result) {
    std::wostringstream stream;
    stream << (result.io.ok ? L"OK" : (result.unsupported ? L"Unsupported" : L"Unavailable"))
           << L"; rows=" << result.entries.size()
           << L"; returned=" << result.returnedCount << L"/" << result.totalCount
           << L"; drivers=" << result.driverCount
           << L"; devices=" << result.deviceCount
           << L"; win32=" << result.io.win32Error
           << L"; " << Utf8ToWideLossy(result.io.message);
    return stream.str();
}

// SelectedDeviceIndex returns the model index stored on the selected tree item.
// Input is the hardware page state; output is -1 when no valid tree item is
// selected.
int SelectedDeviceIndex(HardwareViewState* state) {
    if (!state) {
        return -1;
    }
    // 树项不再携带 lParam：模型索引由控件按 HTREEITEM 记录，这里只做一次查表。
    const HTREEITEM selected = state->tree.selected();
    if (!selected) {
        return -1;
    }
    return static_cast<int>(state->tree.userDataOf(selected));
}

std::wstring SelectedInstanceId(const HardwareViewState& state) {
    const int index = SelectedDeviceIndex(const_cast<HardwareViewState*>(&state));
    const HardwareDeviceNode* node = state.model.deviceAt(index);
    return node ? node->instanceId : std::wstring{};
}

bool IsVisibleDevice(const HardwareViewState& state, int index) {
    return index >= 0 && static_cast<std::size_t>(index) < state.visibleIndexes.size() && state.visibleIndexes[static_cast<std::size_t>(index)];
}

// HasVisibleChildren reports whether a node has at least one child represented
// by the current filter snapshot. It keeps empty-filter tree construction lazy.
bool HasVisibleChildren(const HardwareViewState& state, const HardwareDeviceNode& node) {
    return std::any_of(node.childIndices.begin(), node.childIndices.end(), [&state](const int childIndex) {
        return IsVisibleDevice(state, childIndex);
    });
}

// DeviceTreeKey 是树节点的稳定身份，就是模型里的设备索引。同一份快照内唯一，
// 而快照一换整棵树就重建，所以不需要跨快照稳定。
std::wstring DeviceTreeKey(const int index) {
    return std::to_wstring(index);
}

// MakeDeviceRow 把一个可见设备节点翻译成树行。key 是索引，hasChildren 只用来
// 决定展开箭头，因此无子分支不会被插一个多余的空子项。
bool MakeDeviceRow(const HardwareViewState& state, const int index, Ksword::Ui::TreeRow& row) {
    const HardwareDeviceNode* node = state.model.deviceAt(index);
    if (!node || !IsVisibleDevice(state, index)) {
        return false;
    }
    row.text = CompactDeviceName(*node);
    row.key = DeviceTreeKey(index);
    row.userData = static_cast<LPARAM>(index);
    row.hasChildren = HasVisibleChildren(state, *node);
    return true;
}

// ChildRowsOf 是树的同步子节点来源：设备模型早就在内存里，不需要后台往返。
// 控件会在展开的那一刻调用它，并把返回的行分批插进来。
std::vector<Ksword::Ui::TreeRow> ChildRowsOf(const HardwareViewState& state, const int index) {
    std::vector<Ksword::Ui::TreeRow> rows;
    const HardwareDeviceNode* node = state.model.deviceAt(index);
    if (!node) {
        return rows;
    }
    rows.reserve(node->childIndices.size());
    for (const int childIndex : node->childIndices) {
        Ksword::Ui::TreeRow row;
        if (MakeDeviceRow(state, childIndex, row)) {
            rows.push_back(std::move(row));
        }
    }
    return rows;
}

void AddDetailSnapshotToList(HardwareViewState* state, const HardwareDetailSnapshot& snapshot) {
    if (!state || !state->detail) {
        return;
    }
    ListView_DeleteAllItems(state->detail);
    const HardwareDeviceNode* node = state->model.deviceAt(snapshot.deviceIndex);
    if (!node) {
        AddDetailRow(state->detail, 0, L"选择", L"未选择设备");
        return;
    }
    int row = 0;
    AddDetailRow(state->detail, row++, L"设备", snapshot.detail.title);
    AddDetailRow(state->detail, row++, L"审计模式", L"只读证据视图，不提供设备状态修改操作。");
    AddDetailRow(state->detail, row++, L"R0 设备栈协议", DeviceAuditSummaryText(snapshot.deviceStack));
    AddDetailRow(state->detail, row++, L"R0 输入栈协议", DeviceAuditSummaryText(snapshot.inputStack));
    AddDetailRow(state->detail, row++, L"R0 USB 拓扑协议", DeviceAuditSummaryText(snapshot.usbTopology));
    AddDetailRow(state->detail, row++, L"输入隐私", L"键盘、鼠标和 HID 仅展示元数据，不采集按键、移动或报告流。");
    AddDetailRow(state->detail, row++, L"审计分类", HardwareReadOnlyAuditDescription(*node));
    for (const HardwareProperty& property : snapshot.detail.properties) {
        AddDetailRow(state->detail, row++, property.name, property.value);
    }
}

// ShowDetail schedules SetupAPI and R0 evidence queries away from the UI
// thread. The completion verifies the selected instance so a late result from
// an earlier tree selection cannot overwrite the active detail pane.
void ShowDetail(HardwareViewState* state, int index) {
    if (!state || !state->detail) {
        return;
    }
    const HardwareDeviceNode* node = state->model.deviceAt(index);
    if (!node) {
        ListView_DeleteAllItems(state->detail);
        AddDetailRow(state->detail, 0, L"选择", L"未选择设备");
        return;
    }
    if (!state->detailTask) {
        return;
    }
    const std::wstring instanceId = node->instanceId;
    const HardwareDeviceDetail fallback = state->model.detailFromNode(*node);
    ListView_DeleteAllItems(state->detail);
    AddDetailRow(state->detail, 0, L"状态", L"正在后台加载设备详情和 R0 审计…");
    state->detailTask->request(
        [index, instanceId, fallback] {
            HardwareDetailSnapshot snapshot{};
            snapshot.deviceIndex = index;
            snapshot.instanceId = instanceId;
            snapshot.detail = QueryDeviceManagerDetails(instanceId);
            if (!snapshot.detail.found) {
                snapshot.detail = fallback;
            }
            const ksword::ark::DriverClient client;
            snapshot.deviceStack = client.queryDeviceStackAudit();
            snapshot.inputStack = client.queryInputStackAudit();
            snapshot.usbTopology = client.queryUsbTopologyAudit();
            return snapshot;
        },
        [state](std::uint64_t, std::optional<HardwareDetailSnapshot>&& snapshot, std::exception_ptr error) {
            if (error || !snapshot.has_value()) {
                ListView_DeleteAllItems(state->detail);
                AddDetailRow(state->detail, 0, L"状态", L"设备详情后台加载异常结束。");
                return;
            }
            if (SelectedInstanceId(*state) != snapshot->instanceId) {
                return;
            }
            AddDetailSnapshotToList(state, *snapshot);
        });
}

// SelectFirstRoot selects the first available device after refresh. Input is the
// module state; processing updates the tree selection and detail pane; no return.
void SelectFirstRoot(HardwareViewState* state) {
    if (!state || !state->tree.hwnd()) {
        return;
    }
    const HTREEITEM first = TreeView_GetRoot(state->tree.hwnd());
    if (first) {
        state->tree.selectByKey(state->tree.keyOf(first));
    } else {
        ShowDetail(state, -1);
    }
}

// PopulateTree rebuilds the tree control from the immutable filtered snapshot.
// Both query modes materialize only expanded branches. A non-empty search opens
// roots while descendants remain available through the existing lazy expansion.
void PopulateTree(HardwareViewState* state) {
    if (!state || !state->tree.hwnd()) {
        return;
    }
    std::vector<Ksword::Ui::TreeRow> roots;
    roots.reserve(state->model.rootIndices().size());
    for (const int rootIndex : state->model.rootIndices()) {
        Ksword::Ui::TreeRow row;
        if (MakeDeviceRow(*state, rootIndex, row)) {
            roots.push_back(std::move(row));
        }
    }
    // 重绘抑制、占位子节点、批量插入都在控件里，页面只给根节点。
    state->tree.setRoots(std::move(roots));
    if (!state->filterQuery.empty()) {
        // 筛选状态下不该让用户逐层点开：直接把根展开，子节点由同步来源现取，
        // 而且每次展开仍然受控件的分批上限约束。
        //
        // 但分批上限是「单个父节点」的，这条路径是「多个父节点各插一批、全挤在
        // 同一条消息里」，kBatchSize 覆盖不到。所以这里再限一个自动展开的根数：
        // 剩下的根保持折叠，用户点开时才付代价。
        const HWND treeHwnd = state->tree.hwnd();
        int expanded = 0;
        HTREEITEM root = TreeView_GetRoot(treeHwnd);
        while (root && expanded < kMaxAutoExpandRoots) {
            const HTREEITEM next = TreeView_GetNextSibling(treeHwnd, root);
            TreeView_Expand(treeHwnd, root, TVE_EXPAND);
            root = next;
            ++expanded;
        }
    }
    SelectFirstRoot(state);
}

bool ContainsInsensitive(const std::wstring& text, const std::wstring& query) {
    if (query.empty()) {
        return true;
    }
    return std::search(text.begin(), text.end(), query.begin(), query.end(), [](wchar_t left, wchar_t right) {
        return std::towlower(left) == std::towlower(right);
    }) != text.end();
}

// MatchesDevice tests one device node against the current query. The tree is
// matched field by field rather than through VirtualListView::FilterRowIndexes
// because a matching child has to keep its ancestors visible, so the caller
// needs per-node answers rather than a row index list. A non-null pattern means
// the user turned the ".*" toggle on and the expression compiled.
bool MatchesDevice(const HardwareDeviceNode& node, const std::wstring& query, const std::wregex* pattern) {
    const auto matches = [&query, pattern](const std::wstring& text) {
        return pattern != nullptr ? std::regex_search(text, *pattern) : ContainsInsensitive(text, query);
    };
    return matches(node.instanceId) ||
        matches(node.displayName) ||
        matches(node.className) ||
        matches(node.classGuid) ||
        matches(node.manufacturer) ||
        matches(node.serviceName) ||
        matches(node.driverKey) ||
        matches(node.location) ||
        matches(node.locationPaths) ||
        matches(node.hardwareIds) ||
        matches(node.compatibleIds) ||
        matches(node.upperFilters) ||
        matches(node.lowerFilters) ||
        matches(node.classUpperFilters) ||
        matches(node.classLowerFilters);
}

// EnsureTreeItemVisible restores a stable selection after a tree rebuild. It
// materializes only the selected node's ancestor chain for an unfiltered tree,
// preserving the lazy-loading benefit while avoiding selection jumps.
HTREEITEM EnsureTreeItemVisible(HardwareViewState& state, const int deviceIndex) {
    if (!state.tree.hwnd() || !IsVisibleDevice(state, deviceIndex)) {
        return nullptr;
    }
    std::vector<int> lineage;
    int currentIndex = deviceIndex;
    const std::size_t limit = state.model.devices().size();
    while (currentIndex >= 0 && lineage.size() <= limit) {
        const HardwareDeviceNode* node = state.model.deviceAt(currentIndex);
        if (!node) {
            return nullptr;
        }
        lineage.push_back(currentIndex);
        currentIndex = node->parentIndex;
    }
    if (lineage.empty() || lineage.size() > limit) {
        return nullptr;
    }
    std::reverse(lineage.begin(), lineage.end());
    HTREEITEM item = state.tree.findItem(DeviceTreeKey(lineage.front()));
    if (!item) {
        return nullptr;
    }
    for (std::size_t index = 1; index < lineage.size(); ++index) {
        // 展开父节点会把它的子节点现取进来，然后按 key 直接命中；不再是每层一次
        // TVM_GETITEM 的全子节点文本扫描。
        TreeView_Expand(state.tree.hwnd(), item, TVE_EXPAND);
        item = state.tree.findItem(DeviceTreeKey(lineage[index]));
        if (!item) {
            return nullptr;
        }
    }
    return item;
}

void ApplyHardwareFilter(HardwareViewState& state, HardwareFilterResult result) {
    if (result.generation != state.displayGeneration || result.query != state.filterQuery ||
        result.useRegex != state.filterUseRegex) {
        return;
    }
    state.visibleIndexes = std::move(result.visibleIndexes);
    PopulateTree(&state);
    if (!result.query.empty()) {
        const std::size_t count = static_cast<std::size_t>(std::count(state.visibleIndexes.begin(), state.visibleIndexes.end(), true));
        state.statusText = L"设备树筛选结果 " + std::to_wstring(count) + L" 项。";
    }
    if (!result.selectedInstanceId.empty()) {
        const auto& devices = state.model.devices();
        for (const HardwareDeviceNode& node : devices) {
            if (node.instanceId == result.selectedInstanceId) {
                if (EnsureTreeItemVisible(state, node.index)) {
                    state.tree.selectByKey(DeviceTreeKey(node.index));
                }
                break;
            }
        }
    }
    ::InvalidateRect(state.hwnd, nullptr, TRUE);
}

void RequestHardwareFilter(HardwareViewState& state, std::wstring query, std::wstring selectedInstanceId) {
    state.filterQuery = std::move(query);
    state.filterUseRegex = Ksword::Ui::GetFilterBarRegexEnabled(state.filterBar);
    const auto devices = state.model.deviceSnapshot();
    const std::uint64_t generation = state.displayGeneration;
    const bool useRegex = state.filterUseRegex;
    if (!state.filterTask) {
        return;
    }
    state.filterTask->request(
        [devices, generation, useRegex, query = state.filterQuery, selectedInstanceId = std::move(selectedInstanceId)]() mutable {
            HardwareFilterResult result{};
            result.generation = generation;
            result.query = std::move(query);
            result.useRegex = useRegex;
            result.selectedInstanceId = std::move(selectedInstanceId);
            result.visibleIndexes.assign(devices->size(), result.query.empty());
            // Compiled once for the whole pass; a malformed pattern falls back
            // to substring matching, matching the list-view filter's behavior.
            std::wregex compiled;
            bool regexReady = false;
            if (useRegex && !result.query.empty()) {
                try {
                    compiled.assign(result.query, std::regex_constants::ECMAScript | std::regex_constants::icase);
                    regexReady = true;
                } catch (const std::regex_error&) {
                    regexReady = false;
                }
            }
            if (!result.query.empty()) {
                for (const HardwareDeviceNode& node : *devices) {
                    if (!MatchesDevice(node, result.query, regexReady ? &compiled : nullptr)) {
                        continue;
                    }
                    int current = node.index;
                    while (current >= 0 && static_cast<std::size_t>(current) < result.visibleIndexes.size() && !result.visibleIndexes[static_cast<std::size_t>(current)]) {
                        result.visibleIndexes[static_cast<std::size_t>(current)] = true;
                        current = (*devices)[static_cast<std::size_t>(current)].parentIndex;
                    }
                }
            }
            return result;
        },
        [&state](std::uint64_t, std::optional<HardwareFilterResult>&& result, std::exception_ptr error) {
            if (error || !result.has_value()) {
                state.statusText = L"设备树筛选任务异常结束，已保留当前结果。";
                ::InvalidateRect(state.hwnd, nullptr, TRUE);
                return;
            }
            ApplyHardwareFilter(state, std::move(*result));
        });
}

void BeginDeviceRefresh(HardwareViewState& state) {
    if (!state.refreshTask) {
        return;
    }
    const bool firstLoad = state.model.devices().empty();
    state.statusText = state.refreshTask->running() ? L"设备刷新已排队，等待当前快照完成…" : L"正在后台枚举设备、输入和 USB 审计…";
    ::EnableWindow(state.refreshButton, FALSE);
    if (firstLoad) {
        Ksword::Ui::SetLoadingOverlay(state.loadingOverlay, true, L"正在加载设备审计…");
    }
    ::InvalidateRect(state.hwnd, nullptr, TRUE);
    state.refreshTask->request(
        [] { return EnumerateDeviceManagerTree(); },
        [&state](std::uint64_t, std::optional<HardwareEnumerationResult>&& result, std::exception_ptr error) {
            ::EnableWindow(state.refreshButton, TRUE);
            Ksword::Ui::SetLoadingOverlay(state.loadingOverlay, false);
            if (error || !result.has_value()) {
                state.statusText = L"设备后台枚举异常结束。请检查访问权限。";
                ::InvalidateRect(state.hwnd, nullptr, TRUE);
                return;
            }
            if (!result->success) {
                state.statusText = result->diagnosticText;
                ::InvalidateRect(state.hwnd, nullptr, TRUE);
                return;
            }
            const std::wstring selectedInstanceId = SelectedInstanceId(state);
            state.model.setDevices(std::move(result->devices));
            ++state.displayGeneration;
            const HardwareAuditSummary summary = state.model.auditSummary();
            state.statusText = L"设备 " + std::to_wstring(summary.totalDevices) +
                L" | 输入/HID " + std::to_wstring(summary.inputDevices) + L"/" + std::to_wstring(summary.hidDevices) +
                L" | USB " + std::to_wstring(summary.usbDevices) +
                L" | PCI/ACPI " + std::to_wstring(summary.pciDevices) + L"/" + std::to_wstring(summary.acpiDevices) +
                L" | 筛选证据 " + std::to_wstring(summary.filterEvidenceDevices) +
                L" | 注意 " + std::to_wstring(summary.problemDevices);
            RequestHardwareFilter(state,
                state.filterBar ? Ksword::Ui::GetFilterBarText(state.filterBar) : state.filterQuery,
                selectedInstanceId);
            ::InvalidateRect(state.hwnd, nullptr, TRUE);
        });
}

// ShowDeviceContextMenu displays read-only evidence actions for the selected
// device. Inputs are module state and a screen point; processing corrects
// selection and offers only refresh/copy operations; no device configuration is
// changed and no value is returned.
void ShowDeviceContextMenu(HardwareViewState* state, POINT screenPoint) {
    if (!state || !state->tree.hwnd()) {
        return;
    }

    POINT clientPoint = screenPoint;
    ::ScreenToClient(state->tree.hwnd(), &clientPoint);
    TVHITTESTINFO hit{};
    hit.pt = clientPoint;
    HTREEITEM hitItem = TreeView_HitTest(state->tree.hwnd(), &hit);
    if (hitItem != nullptr) {
        state->tree.selectByKey(state->tree.keyOf(hitItem));
    }

    const int index = SelectedDeviceIndex(state);
    const HardwareDeviceNode* node = state->model.deviceAt(index);
    const bool hasDevice = node != nullptr && !node->instanceId.empty();

    HMENU menu = ::CreatePopupMenu();
    if (!menu) {
        return;
    }
    HMENU refreshMenu = ::CreatePopupMenu();
    if (refreshMenu) {
        ::AppendMenuW(refreshMenu, MF_STRING, kMenuRefresh, L"刷新");
        ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(refreshMenu), L"刷新");
    }
    HMENU copyMenu = ::CreatePopupMenu();
    if (copyMenu) {
        ::AppendMenuW(copyMenu, MF_STRING | (hasDevice ? 0U : MF_GRAYED), kMenuCopyInstanceId, L"复制实例 ID");
        ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(copyMenu), L"复制");
    }

    const UINT command = ::TrackPopupMenu(menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x,
        screenPoint.y,
        0,
        state->hwnd,
        nullptr);
    ::DestroyMenu(menu);

    if (command == 0) {
        return;
    }

    switch (command) {
    case kMenuRefresh:
        BeginDeviceRefresh(*state);
        return;
    case kMenuCopyInstanceId:
        state->statusText = WriteClipboardText(state->hwnd, node->instanceId) ? L"已复制设备实例 ID。" : L"复制设备实例 ID 失败。";
        ::InvalidateRect(state->hwnd, nullptr, TRUE);
        return;
    default:
        break;
    }
}

void ShowDetailContextMenu(HardwareViewState& state, POINT screenPoint) {
    HMENU menu = ::CreatePopupMenu();
    if (!menu) {
        return;
    }
    const int selected = ListView_GetNextItem(state.detail, -1, LVNI_SELECTED);
    const bool hasSelection = selected >= 0;
    ::AppendMenuW(menu, MF_STRING | (hasSelection ? 0U : MF_GRAYED), kMenuCopyDetailCell, L"复制单元格");
    ::AppendMenuW(menu, MF_STRING | (hasSelection ? 0U : MF_GRAYED), kMenuCopyDetailRow, L"复制行");
    ::AppendMenuW(menu, MF_STRING | (ListView_GetItemCount(state.detail) > 0 ? 0U : MF_GRAYED), kMenuCopyDetailVisible, L"复制可见结果");
    const UINT command = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, 0, state.hwnd, nullptr);
    ::DestroyMenu(menu);
    if (command == 0) {
        return;
    }
    switch (command) {
    case kMenuCopyDetailCell:
        state.statusText = WriteClipboardText(state.hwnd, DetailListText(state.detail, selected, 1)) ? L"已复制单元格。" : L"复制单元格失败。";
        break;
    case kMenuCopyDetailRow:
        state.statusText = WriteClipboardText(state.hwnd, DetailRowsAsText(state.detail, false)) ? L"已复制详情行。" : L"复制详情行失败。";
        break;
    case kMenuCopyDetailVisible:
        state.statusText = WriteClipboardText(state.hwnd, DetailRowsAsText(state.detail, true)) ? L"已复制可见详情。" : L"复制可见详情失败。";
        break;
    default:
        return;
    }
    ::InvalidateRect(state.hwnd, nullptr, TRUE);
}

// LayoutView positions toolbar, tree, and detail controls. Input is module state;
// processing computes child rectangles from the current client area; no return.
void LayoutView(HardwareViewState* state) {
    if (!state || !state->hwnd) {
        return;
    }
    RECT rc{};
    ::GetClientRect(state->hwnd, &rc);
    const int width = Width(rc);
    const int height = Height(rc);
    ::MoveWindow(state->refreshButton, kGap, kGap, 86, 24, TRUE);
    ::MoveWindow(state->filterBar, kGap, 35, std::max(80, width - kGap * 2), 24, TRUE);
    const int top = kHeaderHeight + kGap;
    const int treeWidth = width > kTreeWidth + 260 ? kTreeWidth : width / 2;
    ::MoveWindow(state->tree.hwnd(), kGap, top, treeWidth - kGap, height - top - kGap, TRUE);
    ::MoveWindow(state->detail, treeWidth + kGap, top, width - treeWidth - (kGap * 2), height - top - kGap, TRUE);
    ::MoveWindow(state->loadingOverlay, kGap, top, std::max(80, width - kGap * 2), std::max(80, height - top - kGap), TRUE);
}

// CreateChildControls creates the refresh button, tree, and detail list. Inputs
// are module state and parent HWND; processing initializes columns/fonts; output
// is true when all required controls exist.
bool CreateChildControls(HardwareViewState* state, HWND hwnd) {
    state->refreshButton = Ksword::Ui::CreateButton(hwnd, kRefreshButtonId, L"刷新", 0, 0, 80, 24);
    state->filterBar = Ksword::Ui::CreateFilterBar(hwnd, kFilterBarId, L"筛选名称、类、实例、驱动、硬件 ID 和审计证据", 0, 0, 200, 24);
    // 设备模型是内存里的投影，展开时直接同步取子节点，不走后台往返。
    state->tree.setChildSource([state](HTREEITEM, const std::wstring& key) {
        return ChildRowsOf(*state, static_cast<int>(std::wcstol(key.c_str(), nullptr, 10)));
    });
    const bool treeReady = state->tree.create(hwnd, kTreeId, 0, 0, 100, 100);
    state->detail = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL,
        0, 0, 100, 100, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDetailId)), ::GetModuleHandleW(nullptr), nullptr);
    state->loadingOverlay = Ksword::Ui::CreateLoadingOverlay(hwnd, kLoadingOverlayId, { 0, 0, 1, 1 });
    if (!state->refreshButton || !state->filterBar || !treeReady || !state->detail || !state->loadingOverlay) {
        return false;
    }
    ::SendMessageW(state->detail, WM_SETFONT, reinterpret_cast<WPARAM>(Ksword::Ui::SystemUIFont()), TRUE);
    ListView_SetExtendedListViewStyle(state->detail, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES | LVS_EX_LABELTIP);
    AddListColumn(state->detail, 0, L"属性", 180);
    AddListColumn(state->detail, 1, L"值", 560);
    return true;
}

// RegisterHardwareViewClass registers the custom hardware page class. There is
// no external input beyond the process module; output is true when the class is
// available for CreateWindowExW.
bool RegisterHardwareViewClass() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
        HardwareViewState* state = reinterpret_cast<HardwareViewState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (msg) {
        case WM_NCCREATE: {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            state = create ? static_cast<HardwareViewState*>(create->lpCreateParams) : nullptr;
            if (state) {
                state->hwnd = hwnd;
                ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            }
            return TRUE;
        }
        case WM_CREATE:
            if (state && CreateChildControls(state, hwnd)) {
                state->refreshTask = std::make_unique<Ksword::Ui::AsyncSnapshotTask<HardwareEnumerationResult>>(hwnd, kMsgRefreshCompleted);
                state->filterTask = std::make_unique<Ksword::Ui::AsyncSnapshotTask<HardwareFilterResult>>(hwnd, kMsgFilterCompleted);
                state->detailTask = std::make_unique<Ksword::Ui::AsyncSnapshotTask<HardwareDetailSnapshot>>(hwnd, kMsgDetailCompleted);
                LayoutView(state);
                BeginDeviceRefresh(*state);
            }
            return 0;
        case WM_SIZE:
            LayoutView(state);
            return 0;
        case WM_TIMER:
            if (state && wParam == kFilterTimerId) {
                ::KillTimer(hwnd, kFilterTimerId);
                RequestHardwareFilter(*state, Ksword::Ui::GetFilterBarText(state->filterBar),
                    SelectedInstanceId(*state));
                return 0;
            }
            break;
        case WM_COMMAND:
            if (state && LOWORD(wParam) == kFilterBarId && HIWORD(wParam) == EN_CHANGE) {
                // Restart the debounce window; the query text is read when it
                // expires, so intermediate keystrokes cost nothing.
                ::KillTimer(hwnd, kFilterTimerId);
                ::SetTimer(hwnd, kFilterTimerId, kFilterDebounceMs, nullptr);
                return 0;
            }
            if (state && LOWORD(wParam) == kRefreshButtonId) {
                BeginDeviceRefresh(*state);
                return 0;
            }
            break;
        case kMsgRefreshCompleted:
            if (state && state->refreshTask && state->refreshTask->consume(hwnd, wParam, lParam)) {
                return 0;
            }
            break;
        case kMsgFilterCompleted:
            if (state && state->filterTask && state->filterTask->consume(hwnd, wParam, lParam)) {
                return 0;
            }
            break;
        case kMsgDetailCompleted:
            if (state && state->detailTask && state->detailTask->consume(hwnd, wParam, lParam)) {
                return 0;
            }
            break;
        case WM_NOTIFY: {
            auto* notify = reinterpret_cast<NMHDR*>(lParam);
            if (state && notify) {
                // 展开、删除、合成行的选中都由控件消费；页面只处理真实选中。
                LRESULT treeResult = 0;
                if (state->tree.handleNotify(*notify, treeResult)) {
                    return treeResult;
                }
            }
            if (state && notify && notify->idFrom == kTreeId && notify->code == TVN_SELCHANGEDW) {
                const auto* change = reinterpret_cast<const NMTREEVIEWW*>(lParam);
                if (change && !state->tree.silentSelection() && !state->tree.isNotice(change->itemNew.hItem)) {
                    ShowDetail(state, static_cast<int>(state->tree.userDataOf(change->itemNew.hItem)));
                }
                return 0;
            }
            if (state && notify && notify->idFrom == kTreeId && notify->code == NM_RCLICK) {
                POINT pt{};
                ::GetCursorPos(&pt);
                ShowDeviceContextMenu(state, pt);
                return 0;
            }
            if (state && notify && notify->idFrom == kDetailId && notify->code == NM_RCLICK) {
                POINT pt{};
                ::GetCursorPos(&pt);
                ShowDetailContextMenu(*state, pt);
                return 0;
            }
            break;
        }
        case WM_CONTEXTMENU:
            if (state && reinterpret_cast<HWND>(wParam) == state->tree.hwnd()) {
                POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                if (pt.x == -1 && pt.y == -1) {
                    RECT rc{};
                    ::GetWindowRect(state->tree.hwnd(), &rc);
                    pt.x = rc.left + 24;
                    pt.y = rc.top + 24;
                }
                ShowDeviceContextMenu(state, pt);
                return 0;
            }
            if (state && reinterpret_cast<HWND>(wParam) == state->detail) {
                POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                if (pt.x == -1 && pt.y == -1) {
                    RECT rc{};
                    ::GetWindowRect(state->detail, &rc);
                    pt = { rc.left + 24, rc.top + 24 };
                }
                ShowDetailContextMenu(*state, pt);
                return 0;
            }
            break;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            ::SetBkMode(dc, TRANSPARENT);
            ::SetTextColor(dc, Ksword::Ui::AppTheme().textColor);
            return reinterpret_cast<LRESULT>(Ksword::Ui::AppTheme().panelBrush());
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = ::BeginPaint(hwnd, &ps);
            RECT rc{};
            ::GetClientRect(hwnd, &rc);
            ::FillRect(dc, &rc, Ksword::Ui::AppTheme().panelBrush());
            // 状态文字和刷新按钮同一行；原来的 y 下界给到 kHeaderHeight，正好被 y=35 的筛选框盖住。
            RECT textRc{ 104, kGap, rc.right - kGap, kGap + 24 };
            const std::wstring title = state ? state->statusText : L"Device manager";
            Ksword::Ui::DrawTextLine(dc, title, textRc, Ksword::Ui::AppTheme().mutedTextColor,
                Ksword::Ui::SystemUIFont(), DT_SINGLELINE | DT_LEFT | DT_VCENTER);
            ::EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_NCDESTROY:
            if (state) {
                if (state->refreshTask) state->refreshTask->cancel();
                if (state->filterTask) state->filterTask->cancel();
                if (state->detailTask) state->detailTask->cancel();
            }
            delete state;
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        default:
            break;
        }
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    };
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = Ksword::Ui::AppTheme().panelBrush();
    wc.lpszClassName = kHardwareViewClass;
    if (::RegisterClassW(&wc)) {
        return true;
    }
    return ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

} // namespace

HWND CreateHardwareDeviceManagerView(HWND parent, const RECT& bounds) {
    if (!parent || !RegisterHardwareViewClass()) {
        return nullptr;
    }
    auto* state = new HardwareViewState();
    HWND hwnd = ::CreateWindowExW(0, kHardwareViewClass, L"Hardware devices",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        bounds.left, bounds.top, Width(bounds), Height(bounds), parent, nullptr, ::GetModuleHandleW(nullptr), state);
    if (!hwnd) {
        delete state;
    }
    return hwnd;
}

} // namespace Ksword::Features::Hardware
