#pragma once

#include "../Core/Win32Lean.h"
#include "TableCompareActions.h"
#include "TableSnapshotCompare.h"

#include <string>
#include <vector>

namespace Ksword::Ui {

// TableCompareResultWindow shows one comparison as a table of its own. The
// engine could already say "新增 3 / 移除 1 / 变更 2" on the page footer and
// write the same rows to TSV, but the operator had no way to look at the diff
// itself without opening the file — which is exactly backwards for a
// live-refreshing table where the interesting rows are the two that changed.
//
// Inputs are the owner window, the finished comparison and the table's display
// name (used in the caption and the suggested file name). Processing keeps the
// result for as long as the window lives; the window is modal so the page's
// table cannot be refreshed out from under the diff being read. Output: none —
// export and copy happen inside, and closing just returns.
class TableCompareResultWindow final {
public:
    TableCompareResultWindow(HWND owner, TableComparisonResult result, std::wstring tableName);

    // showModal runs a local message loop until the window closes. Input: none.
    // Processing: disables the owner, pumps messages until WM_DESTROY, then
    // restores the owner. Output: none.
    void showModal();

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

private:
    LRESULT handleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    void createControls();
    void layout();
    void rebuildVisibleRows();
    void updateSummary();
    void exportRows();
    void copyRows();
    void close();

    HWND owner_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND list_ = nullptr;
    HWND onlyDifferences_ = nullptr;
    HWND summary_ = nullptr;
    HWND exportButton_ = nullptr;
    HWND copyButton_ = nullptr;
    HWND closeButton_ = nullptr;
    TableComparisonResult result_;
    std::wstring tableName_;
    std::vector<int> visibleRows_;
    bool finished_ = false;
};

// ShowTableCompareResult is the whole thing in one call: open the window, let
// the operator read and export, then close. Pages only ever need this.
void ShowTableCompareResult(HWND owner, TableComparisonResult result, const std::wstring& tableName);

} // namespace Ksword::Ui
