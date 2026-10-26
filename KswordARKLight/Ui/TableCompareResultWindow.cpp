#include "TableCompareResultWindow.h"

#include "Controls.h"
#include "ExportUtil.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace Ksword::Ui {
namespace {

constexpr wchar_t kCompareResultClass[] = L"KswordARKLight.TableCompareResult";
constexpr int kCompareListId = 11901;
constexpr int kCompareOnlyDiffId = 11902;
constexpr int kCompareSummaryId = 11903;
constexpr int kCompareExportId = 11904;
constexpr int kCompareCopyId = 11905;
constexpr int kCompareCloseId = 11906;

constexpr int kMargin = 10;
constexpr int kRowHeight = 26;
constexpr int kBottomHeight = 66;
constexpr int kStatusColumnWidth = 70;
constexpr int kValueColumnWidth = 150;
constexpr int kMinClientWidth = 480;
constexpr int kMinClientHeight = 240;

void EnsureResultClass() {
    static bool registered = false;
    if (registered) {
        return;
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = &TableCompareResultWindow::WindowProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kCompareResultClass;
    ::RegisterClassW(&wc);
    registered = true;
}

// SuggestedFileName turns the table's captions into something a save dialog can
// use on every Windows install. Chinese table names survive only if the file
// system allows them, so the fallback is a plain ASCII name rather than an empty
// suggestion.
std::wstring SuggestedFileName(const std::wstring& tableName) {
    std::wstring name;
    for (const wchar_t character : tableName) {
        const bool reserved = character == L'\\' || character == L'/' || character == L':'
            || character == L'*' || character == L'?' || character == L'"'
            || character == L'<' || character == L'>' || character == L'|'
            || character == L'\t' || character == L'\r' || character == L'\n';
        if (!reserved) {
            name.push_back(character);
        }
    }
    if (name.empty()) {
        return L"table_compare.tsv";
    }
    return name + L"_compare.tsv";
}

// AttachFont sets the UI font on one child control, if it exists.
void AttachFont(HWND control) {
    if (control != nullptr) {
        ::SendMessageW(control, WM_SETFONT,
            reinterpret_cast<WPARAM>(Ksword::Ui::SystemUIFont()), TRUE);
    }
}

} // namespace

TableCompareResultWindow::TableCompareResultWindow(
    HWND owner, TableComparisonResult result, std::wstring tableName)
    : owner_(owner), result_(std::move(result)), tableName_(std::move(tableName)) {}

void TableCompareResultWindow::showModal() {
    EnsureResultClass();

    RECT ownerRect{};
    if (owner_ == nullptr || !::GetWindowRect(owner_, &ownerRect)) {
        ownerRect = RECT{ 0, 0, 1024, 720 };
    }
    const int ownerWidth = std::max<int>(ownerRect.right - ownerRect.left, 640);
    const int ownerHeight = std::max<int>(ownerRect.bottom - ownerRect.top, 480);
    const int width = std::clamp(ownerWidth * 4 / 5, 640, 1180);
    const int height = std::clamp(ownerHeight * 4 / 5, 380, 820);
    const int left = ownerRect.left + (ownerRect.right - ownerRect.left - width) / 2;
    const int top = ownerRect.top + (ownerRect.bottom - ownerRect.top - height) / 2;

    const std::wstring caption = tableName_.empty()
        ? std::wstring(L"表格比对结果")
        : L"表格比对结果 — " + tableName_;

    hwnd_ = ::CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE,
        kCompareResultClass,
        caption.c_str(),
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
        left,
        top,
        width,
        height,
        owner_,
        nullptr,
        ::GetModuleHandleW(nullptr),
        this);
    if (hwnd_ == nullptr) {
        return;
    }

    if (owner_ != nullptr) {
        ::EnableWindow(owner_, FALSE);
    }
    ::ShowWindow(hwnd_, SW_SHOW);
    ::UpdateWindow(hwnd_);

    // The message loop deliberately skips IsDialogMessage: this window's main
    // control is a report list, and dialog navigation would eat the arrow keys
    // the operator needs to walk the diff. Esc is handled in the window itself.
    MSG message{};
    while (!finished_ && ::GetMessageW(&message, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }
    if (finished_ && hwnd_ != nullptr) {
        // A WM_QUIT for another window may have ended the loop first.
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }

    if (owner_ != nullptr) {
        ::EnableWindow(owner_, TRUE);
        ::SetForegroundWindow(owner_);
    }
}

LRESULT CALLBACK TableCompareResultWindow::WindowProc(
    HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* window = reinterpret_cast<TableCompareResultWindow*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        window = create ? static_cast<TableCompareResultWindow*>(create->lpCreateParams) : nullptr;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
        if (window != nullptr) {
            window->hwnd_ = hwnd;
        }
    }
    return window != nullptr
        ? window->handleMessage(hwnd, message, wParam, lParam)
        : ::DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT TableCompareResultWindow::handleMessage(
    HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        createControls();
        rebuildVisibleRows();
        layout();
        return 0;
    case WM_SIZE:
        layout();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        if (limits != nullptr) {
            RECT rect{ 0, 0, kMinClientWidth, kMinClientHeight };
            ::AdjustWindowRectEx(&rect, WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
                FALSE, WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE);
            limits->ptMinTrackSize.x = rect.right - rect.left;
            limits->ptMinTrackSize.y = rect.bottom - rect.top;
        }
        return 0;
    }
    case WM_NOTIFY: {
        auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (header == nullptr) {
            break;
        }
        if (header->idFrom == kCompareListId && header->code == LVN_GETDISPINFOW) {
            auto* info = reinterpret_cast<NMLVDISPINFOW*>(lParam);
            if (info == nullptr || (info->item.mask & LVIF_TEXT) == 0
                || info->item.pszText == nullptr) {
                return 0;
            }
            const int displayRow = info->item.iItem;
            if (displayRow < 0 || static_cast<std::size_t>(displayRow) >= visibleRows_.size()) {
                info->item.pszText[0] = L'\0';
                return 0;
            }
            const int sourceRow = visibleRows_[static_cast<std::size_t>(displayRow)];
            if (sourceRow < 0 || static_cast<std::size_t>(sourceRow) >= result_.rows.size()) {
                info->item.pszText[0] = L'\0';
                return 0;
            }
            const TableComparisonRow& row = result_.rows[static_cast<std::size_t>(sourceRow)];
            // 0 is the status column; the rest map onto result_.columns.
            const std::wstring text = info->item.iSubItem == 0
                ? ComparisonRowStatusText(row)
                : ComparisonCellText(result_, row, static_cast<std::size_t>(info->item.iSubItem) - 1U);
            // A tab or newline in a cell would render as a column break or a
            // blank line, so the list shows the same flattened text the export
            // writes.
            const std::wstring flattened = SanitizeTsvCell(text);
            ::lstrcpynW(info->item.pszText, flattened.c_str(), info->item.cchTextMax);
            return 0;
        }
        break;
    }
    case WM_COMMAND: {
        const int controlId = LOWORD(wParam);
        if (controlId == kCompareOnlyDiffId && HIWORD(wParam) == BN_CLICKED) {
            rebuildVisibleRows();
            return 0;
        }
        if (controlId == kCompareExportId) {
            exportRows();
            return 0;
        }
        if (controlId == kCompareCopyId) {
            copyRows();
            return 0;
        }
        if (controlId == kCompareCloseId) {
            close();
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        close();
        return 0;
    case WM_DESTROY:
        finished_ = true;
        return 0;
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

void TableCompareResultWindow::createControls() {
    list_ = ::CreateWindowExW(
        WS_EX_CLIENTEDGE,
        WC_LISTVIEWW,
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS,
        0, 0, 0, 0,
        hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCompareListId)),
        ::GetModuleHandleW(nullptr),
        nullptr);
    if (list_ != nullptr) {
        ListView_SetExtendedListViewStyle(list_,
            LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES | LVS_EX_LABELTIP);
        AttachFont(list_);

        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = const_cast<wchar_t*>(L"变化");
        column.cx = kStatusColumnWidth;
        ListView_InsertColumn(list_, 0, &column);
        for (std::size_t index = 0; index < result_.columns.size(); ++index) {
            column.pszText = const_cast<wchar_t*>(result_.columns[index].headerText.c_str());
            column.cx = kValueColumnWidth;
            ListView_InsertColumn(list_, static_cast<int>(index) + 1, &column);
        }
    }

    onlyDifferences_ = Ksword::Ui::CreateButton(
        hwnd_, kCompareOnlyDiffId, L"只显示差异", 0, 0, 0, 0);
    // The default state answers the question the user asked: what changed.
    ::SendMessageW(onlyDifferences_, BM_SETCHECK, BST_CHECKED, 0);

    summary_ = Ksword::Ui::CreateText(hwnd_, kCompareSummaryId, L"", 0, 0, 0, 0);
    exportButton_ = Ksword::Ui::CreateButton(hwnd_, kCompareExportId, L"导出 TSV…", 0, 0, 0, 0);
    copyButton_ = Ksword::Ui::CreateButton(hwnd_, kCompareCopyId, L"复制全部", 0, 0, 0, 0);
    closeButton_ = Ksword::Ui::CreateButton(hwnd_, kCompareCloseId, L"关闭", 0, 0, 0, 0);
}

void TableCompareResultWindow::layout() {
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    const int width = std::max<int>(client.right - client.left, 0);
    const int height = std::max<int>(client.bottom - client.top, 0);

    const int bottomTop = height - kBottomHeight;
    if (list_ != nullptr) {
        ::MoveWindow(list_, kMargin, kMargin,
            std::max(0, width - 2 * kMargin), std::max(0, bottomTop - kMargin), TRUE);
    }

    const int summaryTop = bottomTop + 2;
    Ksword::Ui::PlaceStatusLabel(summary_, kMargin, summaryTop,
        std::max(0, width - 2 * kMargin), kRowHeight);

    const int buttonTop = summaryTop + kRowHeight + 4;
    int right = width - kMargin;
    const auto placeRight = [&](HWND control, const int buttonWidth) {
        if (control == nullptr) {
            return;
        }
        right -= buttonWidth;
        ::MoveWindow(control, right, buttonTop, buttonWidth, kRowHeight, TRUE);
        right -= 8;
    };
    placeRight(closeButton_, 82);
    placeRight(copyButton_, 96);
    placeRight(exportButton_, 108);
    if (onlyDifferences_ != nullptr) {
        ::MoveWindow(onlyDifferences_, kMargin, buttonTop, 120, kRowHeight, TRUE);
    }
}

void TableCompareResultWindow::rebuildVisibleRows() {
    visibleRows_.clear();
    const bool differencesOnly = onlyDifferences_ != nullptr
        && ::SendMessageW(onlyDifferences_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    visibleRows_.reserve(result_.rows.size());
    for (std::size_t index = 0; index < result_.rows.size(); ++index) {
        if (differencesOnly && result_.rows[index].source == TableComparisonSource::Both
            && result_.rows[index].changedSourceColumns.empty()) {
            continue;
        }
        visibleRows_.push_back(static_cast<int>(index));
    }

    if (list_ != nullptr) {
        ListView_SetItemCountEx(list_, static_cast<int>(visibleRows_.size()), LVSICF_NOSCROLL);
        ListView_SetItemState(list_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ::InvalidateRect(list_, nullptr, TRUE);
    }
    updateSummary();
}

void TableCompareResultWindow::updateSummary() {
    if (summary_ == nullptr) {
        return;
    }
    std::wstring text = DescribeComparison(result_);
    text += L"；当前列出 " + std::to_wstring(visibleRows_.size())
        + L" / " + std::to_wstring(result_.rows.size()) + L" 行";
    ::SetWindowTextW(summary_, text.c_str());
}

void TableCompareResultWindow::exportRows() {
    const std::wstring suggested = SuggestedFileName(tableName_);
    std::wstring error;
    const SaveTextFileResult result = SaveUtf8TextFileWithDialog(
        hwnd_,
        suggested.c_str(),
        L"导出比对结果",
        L"制表符分隔文件 (*.tsv)\0*.tsv\0所有文件 (*.*)\0*.*\0",
        L"tsv",
        BuildComparisonTsv(result_),
        &error);
    if (result == SaveTextFileResult::Failed) {
        const std::wstring failure = error.empty() ? std::wstring(L"导出失败。") : error;
        ::MessageBoxW(hwnd_, failure.c_str(), L"导出比对结果", MB_OK | MB_ICONWARNING);
    } else if (result == SaveTextFileResult::Saved && summary_ != nullptr) {
        // Say what was written rather than what is listed: the export always
        // carries the full comparison, not just the filtered view.
        ::SetWindowTextW(summary_,
            (L"已导出完整比对结果（含未显示的相同行），共 "
                + std::to_wstring(result_.rows.size()) + L" 行").c_str());
    }
}

void TableCompareResultWindow::copyRows() {
    const std::wstring text = BuildComparisonTsv(result_);
    if (!CopyTextToClipboard(hwnd_, text, L"表格比对导出")) {
        ::MessageBoxW(hwnd_, L"复制失败：剪贴板被其他程序占用。", L"复制比对结果",
            MB_OK | MB_ICONWARNING);
    }
}

void TableCompareResultWindow::close() {
    finished_ = true;
    if (hwnd_ != nullptr) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void ShowTableCompareResult(
    HWND owner, TableComparisonResult result, const std::wstring& tableName) {
    if (result.missingSnapshot || result.sourceInvalidated) {
        // Nothing to show; the caller already reported why on its status line.
        return;
    }
    TableCompareResultWindow window(owner, std::move(result), tableName);
    window.showModal();
}

} // namespace Ksword::Ui
