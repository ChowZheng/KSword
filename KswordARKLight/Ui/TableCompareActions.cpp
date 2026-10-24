#include "TableCompareActions.h"

#include "VirtualListView.h"

#include <cstddef>

namespace Ksword::Ui {

namespace {

// StatusLabel names what happened to one comparison row. Input: one row plus the
// later snapshot index range. Processing: prefers the presence flags, then the
// changed-column set. Output: a short Chinese label used both on screen and in
// the exported TSV.
std::wstring StatusLabel(const TableComparisonRow& row)
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

// CellFor reads one compared column from the chosen side. result column index ->
// per-snapshot value index is what keeps the export aligned when a column is
// missing on one side.
std::wstring CellFor(const TableComparisonResult& result, const TableComparisonRow& row,
    std::size_t columnIndex, const std::vector<std::wstring>* values)
{
    if (!values) {
        return {};
    }
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

} // namespace

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

    TableSnapshotCaptureInput input;
    input.label = SnapshotLabelForOrdinal(engine.Sequence() + 1);
    input.view.columnTitles = [columnTitles]() {
        std::vector<TableSnapshotColumn> columns;
        columns.reserve(columnTitles.size());
        for (std::size_t index = 0; index < columnTitles.size(); ++index) {
            columns.push_back(TableSnapshotColumn{ static_cast<int>(index), columnTitles[index] });
        }
        return columns;
    };
    input.view.rowCount = [&list]() {
        return static_cast<int>(list.visibleIndexes().size());
    };
    // The control exposes visible position -> backing row through
    // visibleIndexes(), which is what a sort permutes. Reporting the backing row
    // as the source row keeps the snapshot's sourceRow meaningful and lets the
    // cell reader stay a plain index lookup.
    input.view.sourceRowAt = [&list](int visualRow) -> int {
        const std::vector<std::size_t>& visible = list.visibleIndexes();
        if (visualRow < 0 || static_cast<std::size_t>(visualRow) >= visible.size()) {
            return -1;
        }
        return static_cast<int>(visible[static_cast<std::size_t>(visualRow)]);
    };
    input.view.cellText = [&list](int sourceRow, int sourceColumn) -> std::wstring {
        if (sourceRow < 0 || sourceColumn < 0) {
            return {};
        }
        const std::vector<VirtualListRow>& rows = list.rows();
        const std::size_t backing = static_cast<std::size_t>(sourceRow);
        if (backing >= rows.size()) {
            return {};
        }
        const std::size_t column = static_cast<std::size_t>(sourceColumn);
        return column < rows[backing].cells.size() ? rows[backing].cells[column] : std::wstring();
    };

    if (!engine.Capture(input)) {
        messageOut = L"快照采集失败：表格当前没有可读数据。";
        return false;
    }
    engine.Retain();
    messageOut = DescribeCapture(engine, engine.HasLater() ? engine.Later() : engine.Earlier());
    return true;
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

std::wstring DescribeCapture(const TableSnapshotEngine& engine, const TableSnapshot& snapshot)
{
    std::wstring text = L"已采集快照 " + snapshot.label + L"："
        + std::to_wstring(snapshot.rows.size()) + L" 行 / "
        + std::to_wstring(snapshot.visibleColumns.size()) + L" 列";
    if (snapshot.sequence == engine.Earlier().sequence) {
        text += L"（作为基准，后续刷新不会替换它）";
    }
    for (const std::wstring& notice : SnapshotTruncationNotices(snapshot)) {
        text += L"；" + notice;
    }
    return text;
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
        text += StatusLabel(row);
        text += L"\t";
        const std::vector<std::wstring>* values = RowValues(result, row);
        for (std::size_t column = 0; column < result.columns.size(); ++column) {
            // A tab inside a cell would shift every following column, so the
            // export replaces it with a space instead of promising a TSV the
            // user cannot open.
            std::wstring cell = CellFor(result, row, column, values);
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
    std::wstring& messageOut)
{
    switch (commandId) {
    case kMenuTableCaptureSnapshot:
        return CaptureVisibleListSnapshot(engine, list, columnTitles, messageOut);
    case kMenuTableCompareSnapshots:
        if (!engine.HasLater()) {
            messageOut = L"比对需要两份快照：先采集一次，刷新后再采集一次。";
            return true;
        }
        messageOut = DescribeComparison(CompareListSnapshots(engine));
        return true;
    case kMenuTableClearSnapshots:
        engine.Clear();
        messageOut = L"已清除全部快照。";
        return true;
    default:
        return false;
    }
}

} // namespace Ksword::Ui
