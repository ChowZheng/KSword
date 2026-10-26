#include "TableCompareActions.h"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <utility>

namespace Ksword::Ui {

bool CaptureRowSourceSnapshot(TableSnapshotEngine& engine, const TableCompareRows& rows,
    const std::vector<std::wstring>& columnTitles, std::wstring& messageOut)
{
    if (columnTitles.empty()) {
        messageOut = L"这张表还没有列，无法采集快照。";
        return false;
    }
    if (rows.rowCount == 0 || !rows.cellText) {
        messageOut = L"快照采集失败：表格当前没有可读数据。";
        return false;
    }

    std::unordered_map<int, int> positions;
    if (!rows.sourceColumns.empty()) {
        if (rows.sourceColumns.size() != columnTitles.size()) {
            messageOut = L"快照采集失败：列身份与表头不匹配。";
            return false;
        }
        for (std::size_t index = 0; index < rows.sourceColumns.size(); ++index) {
            const int id = rows.sourceColumns[index];
            if (id < 0 || !positions.emplace(id, static_cast<int>(index)).second) {
                messageOut = L"快照采集失败：列身份无效或重复。";
                return false;
            }
        }
    }

    TableSnapshotCaptureInput input;
    input.label = SnapshotLabelForOrdinal(engine.Sequence() + 1);
    input.view.columnTitles = [columnTitles, ids = rows.sourceColumns]() {
        std::vector<TableSnapshotColumn> columns;
        columns.reserve(columnTitles.size());
        for (std::size_t index = 0; index < columnTitles.size(); ++index) {
            columns.push_back(TableSnapshotColumn{
                ids.empty() ? static_cast<int>(index) : ids[index], columnTitles[index] });
        }
        return columns;
    };
    input.view.rowCount = [&rows]() {
        // Capture walks the count once, so clamping to int here is what keeps a
        // page with more than 2^31 rows from turning the count negative rather
        // than from being truncated by the engine's own row limit.
        const std::size_t maximum = static_cast<std::size_t>(std::numeric_limits<int>::max());
        return static_cast<int>(std::min(rows.rowCount, maximum));
    };
    if (rows.sourceRowAt) {
        input.view.sourceRowAt = rows.sourceRowAt;
    }
    if (positions.empty()) {
        input.view.cellText = rows.cellText;
    } else {
        input.view.cellText = [read = rows.cellText, positions = std::move(positions)](int row, int id) {
            const auto found = positions.find(id);
            return found == positions.end() ? std::wstring{} : read(row, found->second);
        };
    }

    if (!engine.Capture(input)) {
        messageOut = L"快照采集失败：表格当前没有可读数据。";
        return false;
    }
    engine.Retain();
    messageOut = DescribeCapture(engine, engine.HasLater() ? engine.Later() : engine.Earlier());
    return true;
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

} // namespace Ksword::Ui
