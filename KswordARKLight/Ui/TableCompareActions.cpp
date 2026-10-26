#include "TableCompareActions.h"

#include "TableCompareResultWindow.h"
#include "VirtualListView.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <utility>

namespace Ksword::Ui {

namespace {

// RowValues picks the value vector that belongs to the side being displayed.
const std::vector<std::wstring>* RowValues(const TableComparisonResult& result, const TableComparisonRow& row)
{
    if (row.displayLater && row.laterRowIndex >= 0
        && static_cast<std::size_t>(row.laterRowIndex) < result.laterSnapshot.rows.size()) {
        return &result.laterSnapshot.rows[static_cast<std::size_t>(row.laterRowIndex)].values;
    }
    if (row.earlierRowIndex >= 0
        && static_cast<std::size_t>(row.earlierRowIndex) < result.earlierSnapshot.rows.size()) {
        return &result.earlierSnapshot.rows[static_cast<std::size_t>(row.earlierRowIndex)].values;
    }
    return nullptr;
}

} // namespace

std::wstring ComparisonRowStatusText(const TableComparisonRow& row)
{
    if (row.source == TableComparisonSource::LaterOnly) {
        return L"新增";
    }
    if (row.source == TableComparisonSource::EarlierOnly) {
        return L"移除";
    }
    // Field-level differences only exist when stable keys aligned the rows.
    // Without them the engine deliberately leaves the set empty, so "both" with
    // an empty set means "matched and identical as far as we can prove".
    return row.changedSourceColumns.empty() ? L"相同" : L"变更";
}

std::wstring ComparisonCellText(const TableComparisonResult& result, const TableComparisonRow& row,
    const std::size_t columnIndex)
{
    const std::vector<std::wstring>* values = RowValues(result, row);
    if (!values) {
        return {};
    }
    // result column index -> per-snapshot value index is what keeps the export
    // aligned when a column is missing on one side.
    const bool later = row.displayLater && row.laterRowIndex >= 0;
    const std::vector<int>& indexes = later ? result.laterValueIndexes : result.earlierValueIndexes;
    if (columnIndex >= indexes.size()) {
        return {};
    }
    const int valueIndex = indexes[columnIndex];
    if (valueIndex < 0 || static_cast<std::size_t>(valueIndex) >= values->size()) {
        return {};
    }
    return (*values)[static_cast<std::size_t>(valueIndex)];
}

std::vector<PopupMenuItem> BuildTableCompareMenuItems(const TableSnapshotEngine& engine)
{
    std::vector<PopupMenuItem> items;
    items.push_back({ kMenuTableCaptureSnapshot, L"采集快照", false, true, false });

    PopupMenuItem compare{ kMenuTableCompareSnapshots, L"与快照比对", false, engine.HasLater(), false };
    if (engine.HasLater()) {
        compare.text = L"与快照比对（" + engine.Earlier().label + L" \x2192 " + engine.Later().label + L"）";
    } else if (engine.HasEarlier()) {
        // Explain the disabled state instead of leaving a dead entry.
        compare.text = L"与快照比对（再采集一次后可用）";
    }
    items.push_back(compare);

    items.push_back({ kMenuTableClearSnapshots, L"清除快照", false, !engine.Empty(), false });
    return items;
}

void AppendTableCompareMenuItems(HMENU menu, const TableSnapshotEngine& engine)
{
    if (!menu) {
        return;
    }
    for (const PopupMenuItem& item : BuildTableCompareMenuItems(engine)) {
        if (item.separator) {
            ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            continue;
        }
        ::AppendMenuW(menu, MF_STRING | (item.enabled ? 0U : MF_GRAYED), item.id, item.text.c_str());
    }
}

bool CaptureVisibleListSnapshot(TableSnapshotEngine& engine, const VirtualListView& list,
    const std::vector<std::wstring>& columnTitles, std::wstring& messageOut)
{
    if (columnTitles.empty()) {
        messageOut = L"这张表还没有列，无法采集快照。";
        return false;
    }

    // The control exposes visible position -> backing row through
    // visibleIndexes(), which is what a sort permutes. Reporting the backing row
    // as the source row keeps the snapshot's sourceRow meaningful and lets the
    // cell reader stay a plain index lookup.
    TableCompareRows rows;
    rows.rowCount = list.visibleIndexes().size();
    rows.sourceRowAt = [&list](int visualRow) -> int {
        const std::vector<std::size_t>& visible = list.visibleIndexes();
        if (visualRow < 0 || static_cast<std::size_t>(visualRow) >= visible.size()) {
            return -1;
        }
        return static_cast<int>(visible[static_cast<std::size_t>(visualRow)]);
    };
    rows.cellText = [&list](int sourceRow, int sourceColumn) -> std::wstring {
        if (sourceRow < 0 || sourceColumn < 0) {
            return {};
        }
        const std::vector<VirtualListRow>& backingRows = list.rows();
        const std::size_t backing = static_cast<std::size_t>(sourceRow);
        if (backing >= backingRows.size()) {
            return {};
        }
        const std::size_t column = static_cast<std::size_t>(sourceColumn);
        return column < backingRows[backing].cells.size()
            ? backingRows[backing].cells[column]
            : std::wstring();
    };
    return CaptureRowSourceSnapshot(engine, rows, columnTitles, messageOut);
}

TableComparisonResult CompareListSnapshots(const TableSnapshotEngine& engine)
{
    TableComparisonOptions options;
    // Refresh-varying columns would otherwise report a difference on every
    // single comparison, which trains the user to ignore the feature.
    options.ignoredSourceColumns = IgnoredColumnIndexesForKeywords(
        engine.Earlier(), engine.Later(), DefaultIgnoredColumnKeywords());
    return engine.Compare(options);
}

std::wstring DescribeComparison(const TableComparisonResult& result)
{
    if (result.missingSnapshot) {
        return L"比对需要两份快照：再采集一次后重试。";
    }
    if (result.sourceInvalidated) {
        return L"其中一份快照在采集时源表格已失效，无法比对。";
    }

    std::size_t added = 0;
    std::size_t removed = 0;
    std::size_t changed = 0;
    for (const TableComparisonRow& row : result.rows) {
        if (row.source == TableComparisonSource::LaterOnly) {
            ++added;
        } else if (row.source == TableComparisonSource::EarlierOnly) {
            ++removed;
        } else if (!row.changedSourceColumns.empty()) {
            ++changed;
        }
    }

    std::wstring text = result.earlierSnapshot.label + L" \x2192 " + result.laterSnapshot.label + L"：";
    text += L"新增 " + std::to_wstring(added);
    text += L" / 移除 " + std::to_wstring(removed);
    text += L" / 变更 " + std::to_wstring(changed);
    text += L"（共比对 " + std::to_wstring(result.rows.size()) + L" 行）";
    if (!result.usesStableKeys) {
        text += L"；无可用的稳定键列，已降级为整行比对";
    }
    for (const std::wstring& notice : ComparisonTruncationNotices(result)) {
        text += L"；" + notice;
    }
    return text;
}

std::wstring BuildComparisonTsv(const TableComparisonResult& result)
{
    std::wstring text;
    text += L"变化\t";
    for (std::size_t index = 0; index < result.columns.size(); ++index) {
        text += result.columns[index].headerText;
        text += L"\t";
    }
    text += L"\r\n";

    for (const TableComparisonRow& row : result.rows) {
        text += ComparisonRowStatusText(row);
        text += L"\t";
        for (std::size_t column = 0; column < result.columns.size(); ++column) {
            // A tab inside a cell would shift every following column, so the
            // export replaces it with a space instead of promising a TSV the
            // user cannot open.
            std::wstring cell = ComparisonCellText(result, row, column);
            for (wchar_t& character : cell) {
                if (character == L'\t' || character == L'\r' || character == L'\n') {
                    character = L' ';
                }
            }
            text += cell;
            text += L"\t";
        }
        text += L"\r\n";
    }
    return text;
}

bool HandleTableCompareCommand(TableSnapshotEngine& engine, UINT commandId,
    const VirtualListView& list, const std::vector<std::wstring>& columnTitles,
    std::wstring& messageOut, const std::wstring& tableName)
{
    switch (commandId) {
    case kMenuTableCompareSnapshots: {
        if (!engine.HasLater()) {
            messageOut = L"比对需要两份快照：先采集一次，刷新后再采集一次。";
            return true;
        }
        // The list overload still owns the capture path (it can read the control
        // directly), but the comparison half is shared with the row-source
        // overload so both end up in the same window.
        TableComparisonResult result = CompareListSnapshots(engine);
        messageOut = DescribeComparison(result);
        HWND listWindow = list.hwnd();
        HWND owner = listWindow != nullptr ? ::GetParent(listWindow) : nullptr;
        ShowTableCompareResult(owner != nullptr ? owner : listWindow, std::move(result), tableName);
        return true;
    }
    case kMenuTableCaptureSnapshot:
        return CaptureVisibleListSnapshot(engine, list, columnTitles, messageOut);
    case kMenuTableClearSnapshots:
        engine.Clear();
        messageOut = L"已清除全部快照。";
        return true;
    default:
        return false;
    }
}

bool HandleTableCompareCommand(TableSnapshotEngine& engine, UINT commandId,
    const TableCompareRows& rows, HWND owner, const std::vector<std::wstring>& columnTitles,
    std::wstring& messageOut, const std::wstring& tableName)
{
    switch (commandId) {
    case kMenuTableCaptureSnapshot:
        return CaptureRowSourceSnapshot(engine, rows, columnTitles, messageOut);
    case kMenuTableCompareSnapshots: {
        if (!engine.HasLater()) {
            messageOut = L"比对需要两份快照：先采集一次，刷新后再采集一次。";
            return true;
        }
        TableComparisonResult result = CompareListSnapshots(engine);
        messageOut = DescribeComparison(result);
        ShowTableCompareResult(owner, std::move(result), tableName);
        return true;
    }
    case kMenuTableClearSnapshots:
        engine.Clear();
        messageOut = L"已清除全部快照。";
        return true;
    default:
        return false;
    }
}

} // namespace Ksword::Ui
