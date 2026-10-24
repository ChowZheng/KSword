#include "../Core/Win32Lean.h"

#include "TableSnapshotCompare.h"

#include <algorithm>
#include <cwctype>
#include <limits>
#include <unordered_map>
#include <utility>

namespace {

using Ksword::Ui::TableComparisonRow;
using Ksword::Ui::TableComparisonResult;
using Ksword::Ui::TableComparisonSource;
using Ksword::Ui::TableSnapshot;
using Ksword::Ui::TableSnapshotColumn;
using Ksword::Ui::TableSnapshotRow;

// SaturatingAdd adds two byte counters without wrapping. Input: two counts.
// Processing: clamps at the type maximum. Output: the saturating sum.
std::uint64_t SaturatingAdd(const std::uint64_t left, const std::uint64_t right) noexcept
{
    return right > (std::numeric_limits<std::uint64_t>::max)() - left
        ? (std::numeric_limits<std::uint64_t>::max)()
        : left + right;
}

// CanRetainBytes reports whether one more allocation still fits the budget.
// Input: current bytes, additional bytes and the limit. Processing: compares
// without subtracting past zero. Output: true when both fit.
bool CanRetainBytes(const std::uint64_t currentBytes, const std::uint64_t additionalBytes,
    const std::uint64_t byteLimit) noexcept
{
    return currentBytes <= byteLimit && additionalBytes <= byteLimit - currentBytes;
}

// TextStorageBytes is the retained size of one wide string. Input: the text.
// Processing: multiplies the character count by the width of wchar_t, which is
// the same accounting the Qt engine did for QChar. Output: saturating byte count.
std::uint64_t TextStorageBytes(const std::wstring& text) noexcept
{
    const std::uint64_t characters = static_cast<std::uint64_t>(text.size());
    return characters > (std::numeric_limits<std::uint64_t>::max)() / sizeof(wchar_t)
        ? (std::numeric_limits<std::uint64_t>::max)()
        : characters * sizeof(wchar_t);
}

// ColumnStorageBytes is the accounted size of one snapshot column.
std::uint64_t ColumnStorageBytes(const TableSnapshotColumn& column)
{
    return SaturatingAdd(sizeof(TableSnapshotColumn), TextStorageBytes(column.headerText));
}

// RowStorageBytes is the accounted size of one snapshot row.
std::uint64_t RowStorageBytes(const TableSnapshotRow& row)
{
    std::uint64_t bytes = SaturatingAdd(sizeof(TableSnapshotRow),
        static_cast<std::uint64_t>(row.values.size()) * sizeof(std::wstring));
    for (const std::wstring& value : row.values) {
        bytes = SaturatingAdd(bytes, TextStorageBytes(value));
    }
    return bytes;
}

// BoundedText is one display value after the character limit was applied.
struct BoundedText {
    std::wstring text;
    bool truncated = false;
};

// BoundDisplayText applies the character limit to one cell or header value.
// Input: source text and the maximum retained characters. Processing: keeps the
// prefix and marks the value truncated. Output: the bounded text.
BoundedText BoundDisplayText(const std::wstring& value, const int maximumCharacters)
{
    BoundedText result;
    const std::size_t limit = static_cast<std::size_t>(std::max(0, maximumCharacters));
    if (value.size() <= limit) {
        result.text = value;
        return result;
    }
    result.text.assign(value, 0, limit);
    result.truncated = true;
    return result;
}

// RecordBoundedValue updates the truncation counters for one read value. Input:
// the snapshot under construction, the bounded value and whether it is a header.
// Processing: counts the header and cell truncations separately. No value is
// returned. Win32 cells always arrive as text, so unsupportedDisplayValueCount is
// only ever advanced by a caller that could not produce text at all.
void RecordBoundedValue(TableSnapshot& snapshot, const BoundedText& value, const bool headerValue)
{
    if (!value.truncated) {
        return;
    }
    snapshot.truncatedByValueLimit = true;
    if (headerValue) {
        ++snapshot.truncatedHeaderValueCount;
    } else {
        ++snapshot.truncatedCellValueCount;
    }
}

// SnapshotStorageBytes accounts one snapshot end to end. Input: the snapshot.
// Processing: sums the label, columns, rows and key column list. Output:
// saturating byte count.
std::uint64_t SnapshotStorageBytes(const TableSnapshot& snapshot)
{
    std::uint64_t bytes = SaturatingAdd(sizeof(TableSnapshot), TextStorageBytes(snapshot.label));
    for (const TableSnapshotColumn& column : snapshot.visibleColumns) {
        bytes = SaturatingAdd(bytes, ColumnStorageBytes(column));
    }
    for (const TableSnapshotRow& row : snapshot.rows) {
        bytes = SaturatingAdd(bytes, RowStorageBytes(row));
    }
    return SaturatingAdd(bytes, static_cast<std::uint64_t>(snapshot.keyColumns.size()) * sizeof(int));
}

// DiscardInvalidatedSnapshot keeps the label and timestamp but drops the data.
// Input: the snapshot to empty out. Processing: sets sourceInvalidated and
// re-accounts the size. No value is returned; the caller still stores the
// snapshot so the UI can explain why the capture failed.
void DiscardInvalidatedSnapshot(TableSnapshot& snapshot)
{
    snapshot.sourceInvalidated = true;
    snapshot.visibleColumns.clear();
    snapshot.rows.clear();
    snapshot.keyColumns.clear();
    snapshot.estimatedBytes = SnapshotStorageBytes(snapshot);
}

// SnapshotColumnLookup maps source column identity to a value index in a row.
// Input: one snapshot. Processing: first occurrence wins, matching the Qt
// engine, so a duplicated source column cannot shift later indexes.
class SnapshotColumnLookup final {
public:
    explicit SnapshotColumnLookup(const TableSnapshot& snapshot)
    {
        valueIndexBySourceColumn_.reserve(snapshot.visibleColumns.size());
        for (std::size_t index = 0; index < snapshot.visibleColumns.size(); ++index) {
            const int sourceColumn = snapshot.visibleColumns[index].sourceColumn;
            if (sourceColumn >= 0) {
                valueIndexBySourceColumn_.emplace(sourceColumn, static_cast<int>(index));
            }
        }
    }

    int ValueIndex(const int sourceColumn) const
    {
        const auto iterator = valueIndexBySourceColumn_.find(sourceColumn);
        return iterator == valueIndexBySourceColumn_.end() ? -1 : iterator->second;
    }

    std::size_t Size() const noexcept { return valueIndexBySourceColumn_.size(); }

private:
    std::unordered_map<int, int> valueIndexBySourceColumn_;
};

// ValueForSourceColumn returns the display text of one column, or a shared empty
// string when the column is absent from that snapshot.
const std::wstring& ValueForSourceColumn(const TableSnapshotRow& row,
    const SnapshotColumnLookup& lookup, const int sourceColumn)
{
    static const std::wstring emptyValue;
    const int valueIndex = lookup.ValueIndex(sourceColumn);
    if (valueIndex < 0 || static_cast<std::size_t>(valueIndex) >= row.values.size()) {
        return emptyValue;
    }
    return row.values[static_cast<std::size_t>(valueIndex)];
}

// ValueIndexesForColumns resolves the combined column list against one snapshot.
std::vector<int> ValueIndexesForColumns(const SnapshotColumnLookup& lookup,
    const std::vector<TableSnapshotColumn>& columns)
{
    std::vector<int> indexes;
    indexes.reserve(columns.size());
    for (const TableSnapshotColumn& column : columns) {
        indexes.push_back(lookup.ValueIndex(column.sourceColumn));
    }
    return indexes;
}

// CombinedColumns unions both snapshots' columns in visual order, keeping the
// earlier position first so the comparison table matches what the user saw
// before refreshing.
std::vector<TableSnapshotColumn> CombinedColumns(const TableSnapshot& earlier,
    const TableSnapshot& later)
{
    std::vector<TableSnapshotColumn> columns;
    std::unordered_set<int> seenColumns;
    for (const TableSnapshot* snapshot : { &earlier, &later }) {
        for (const TableSnapshotColumn& column : snapshot->visibleColumns) {
            if (column.sourceColumn >= 0 && seenColumns.insert(column.sourceColumn).second) {
                columns.push_back(column);
            }
        }
    }
    return columns;
}

// ComparisonWork tracks the comparison budgets. Input: limits, cancel callback
// and the result being filled. Processing: every unit of real work and every
// temporary byte is charged here before it happens. Output: false from any
// Consume* call means the caller must stop immediately.
class ComparisonWork final {
public:
    ComparisonWork(const Ksword::Ui::TableSnapshotComparisonLimits& limits,
        const std::function<bool()>& shouldCancel, TableComparisonResult& result)
        : limits_(limits), shouldCancel_(shouldCancel), result_(result)
    {
    }

    bool ConsumeWork(const std::uint64_t units = 1)
    {
        const std::uint64_t updated = SaturatingAdd(result_.workUnits, units);
        if (updated > limits_.maximumWorkUnits) {
            result_.truncatedByWorkLimit = true;
            return false;
        }
        result_.workUnits = updated;
        return true;
    }

    bool ReserveTemporary(const std::uint64_t bytes)
    {
        const std::uint64_t updated = SaturatingAdd(temporaryEstimatedBytes_, bytes);
        if (updated > limits_.maximumTemporaryEstimatedBytes) {
            result_.truncatedByTemporaryByteLimit = true;
            return false;
        }
        temporaryEstimatedBytes_ = updated;
        result_.temporaryPeakEstimatedBytes = std::max(result_.temporaryPeakEstimatedBytes,
            temporaryEstimatedBytes_);
        return true;
    }

    bool CheckpointAfterRow()
    {
        ++processedRows_;
        const int checkRows = std::max(1, limits_.cancelCheckRows);
        if (processedRows_ % static_cast<std::uint64_t>(checkRows) != 0) {
            return true;
        }
        if (shouldCancel_ && shouldCancel_()) {
            result_.cancelled = true;
            return false;
        }
        return true;
    }

    bool CanContinue() const noexcept { return !result_.isTruncated(); }

private:
    const Ksword::Ui::TableSnapshotComparisonLimits& limits_;
    const std::function<bool()>& shouldCancel_;
    TableComparisonResult& result_;
    std::uint64_t temporaryEstimatedBytes_ = 0;
    std::uint64_t processedRows_ = 0;
};

// AppendSignatureInteger mixes one integer into a row signature. The signature is
// a wide string used purely as an opaque byte carrier, so embedded nulls are
// fine and every component is length delimited by construction.
void AppendSignatureInteger(std::wstring& signature, const std::int64_t value)
{
    signature.append(reinterpret_cast<const wchar_t*>(&value),
        sizeof(value) / sizeof(wchar_t));
}

// AppendSignatureText mixes one cell value into a row signature.
void AppendSignatureText(std::wstring& signature, const std::wstring& value)
{
    AppendSignatureInteger(signature, static_cast<std::int64_t>(value.size()));
    signature.append(value);
}

// IsBlankKeyValue reports whether a key cell holds only whitespace, which makes
// the row unusable as a key because several rows would collide on it.
bool IsBlankKeyValue(const std::wstring& value) noexcept
{
    for (const wchar_t character : value) {
        if (!std::iswspace(static_cast<wint_t>(character))) {
            return false;
        }
    }
    return true;
}

// RowSignature hashes every compared column of one row. Input: the row, its
// snapshot lookup, the combined column list, the ignore set and the work budget.
// Processing: appends source column, length and text for each compared column.
// Output: a non-empty signature, or an empty string when the budget ran out.
std::wstring RowSignature(const TableSnapshotRow& row, const SnapshotColumnLookup& lookup,
    const std::vector<TableSnapshotColumn>& columns, const std::unordered_set<int>& ignoredColumns,
    ComparisonWork& work)
{
    std::wstring signature;
    signature.reserve(64);
    signature.push_back(L'S');
    signature.push_back(L'R');
    for (const TableSnapshotColumn& column : columns) {
        if (ignoredColumns.count(column.sourceColumn) != 0) {
            continue;
        }
        if (!work.ConsumeWork()) {
            return {};
        }
        AppendSignatureInteger(signature, column.sourceColumn);
        AppendSignatureText(signature, ValueForSourceColumn(row, lookup, column.sourceColumn));
    }
    return signature;
}

// RowKeySignature hashes only the declared stable key columns. Output: an empty
// string when any key cell is blank, which makes the whole key set unusable.
std::wstring RowKeySignature(const TableSnapshotRow& row, const SnapshotColumnLookup& lookup,
    const std::vector<int>& keyColumns, ComparisonWork& work)
{
    std::wstring signature;
    signature.reserve(32);
    signature.push_back(L'K');
    signature.push_back(L'R');
    for (const int sourceColumn : keyColumns) {
        if (!work.ConsumeWork()) {
            return {};
        }
        const std::wstring& value = ValueForSourceColumn(row, lookup, sourceColumn);
        if (IsBlankKeyValue(value)) {
            return {};
        }
        AppendSignatureInteger(signature, sourceColumn);
        AppendSignatureText(signature, value);
    }
    return signature;
}

// UsableStableKeyColumns filters the requested key columns down to the ones both
// snapshots really contain. Input: both snapshots, both lookups, the requested
// keys (an empty list means "use the snapshot's own keys") and the ignore set.
// Processing: drops ignored, missing and, for the declared-key path, keys the
// later snapshot did not record. Output: the keys a field-level diff may use.
std::vector<int> UsableStableKeyColumns(const TableSnapshot& earlier, const TableSnapshot& later,
    const SnapshotColumnLookup& earlierLookup, const SnapshotColumnLookup& laterLookup,
    const std::vector<int>& requestedKeyColumns, const std::unordered_set<int>& ignoredColumns)
{
    std::vector<int> keyColumns;
    const bool requested = !requestedKeyColumns.empty();
    const std::vector<int>& candidates = requested ? requestedKeyColumns : earlier.keyColumns;
    for (const int sourceColumn : candidates) {
        if (ignoredColumns.count(sourceColumn) != 0) {
            continue;
        }
        if (!requested && std::find(later.keyColumns.begin(), later.keyColumns.end(), sourceColumn)
            == later.keyColumns.end()) {
            continue;
        }
        if (earlierLookup.ValueIndex(sourceColumn) < 0 || laterLookup.ValueIndex(sourceColumn) < 0) {
            continue;
        }
        keyColumns.push_back(sourceColumn);
    }
    return keyColumns;
}

// BuildUniqueRowKeys indexes one snapshot by key signature. Input: the snapshot,
// its lookup, the key columns and the work budget. Processing: refuses the index
// as soon as a key is blank or duplicated, because a duplicated key would make
// the field-level diff pair the wrong rows. Output: true when every row keyed
// uniquely.
bool BuildUniqueRowKeys(const TableSnapshot& snapshot, const SnapshotColumnLookup& lookup,
    const std::vector<int>& keyColumns, std::unordered_map<std::wstring, int>& rowByKey,
    ComparisonWork& work)
{
    rowByKey.clear();
    if (!work.ReserveTemporary(static_cast<std::uint64_t>(snapshot.rows.size()) *
            (sizeof(int) + 96ull))) {
        return false;
    }
    rowByKey.reserve(snapshot.rows.size());
    for (std::size_t rowIndex = 0; rowIndex < snapshot.rows.size(); ++rowIndex) {
        const std::wstring key = RowKeySignature(snapshot.rows[rowIndex], lookup, keyColumns, work);
        if (!work.CanContinue()) {
            return false;
        }
        if (key.empty() || !rowByKey.emplace(key, static_cast<int>(rowIndex)).second) {
            rowByKey.clear();
            return false;
        }
        if (!work.CheckpointAfterRow()) {
            return false;
        }
    }
    return true;
}

// LogicalSnapshotBytes is the size charged against the comparison budget; it
// never under-reports a snapshot whose cached size was stale.
std::uint64_t LogicalSnapshotBytes(const TableSnapshot& snapshot)
{
    return std::max(snapshot.estimatedBytes, SnapshotStorageBytes(snapshot));
}

// AppendComparisonRow appends one result row under the result byte budget.
// Input: the result, the limits, the row fields and its changed columns.
// Processing: charges the row before storing it. Output: false when the result
// budget is exhausted, which also sets truncatedByResultByteLimit.
bool AppendComparisonRow(TableComparisonResult& result,
    const Ksword::Ui::TableSnapshotComparisonLimits& limits, const TableComparisonSource source,
    const int earlierRowIndex, const int laterRowIndex, const bool displayLater,
    std::unordered_set<int> changedSourceColumns = std::unordered_set<int>())
{
    std::uint64_t rowBytes = sizeof(TableComparisonRow);
    rowBytes = SaturatingAdd(rowBytes,
        static_cast<std::uint64_t>(changedSourceColumns.size()) * 64ull);
    if (!CanRetainBytes(result.estimatedBytes, rowBytes, limits.maximumEstimatedBytes)) {
        result.truncatedByResultByteLimit = true;
        return false;
    }

    TableComparisonRow row;
    row.source = source;
    row.earlierRowIndex = earlierRowIndex;
    row.laterRowIndex = laterRowIndex;
    row.displayLater = displayLater;
    row.changedSourceColumns = std::move(changedSourceColumns);
    result.rows.push_back(std::move(row));
    result.estimatedBytes += rowBytes;
    return true;
}

// LowercaseWide folds a wide string to lower case for case-insensitive keyword
// matching without pulling in locale-sensitive collation.
std::wstring LowercaseWide(const std::wstring& text)
{
    std::wstring folded;
    folded.reserve(text.size());
    for (const wchar_t character : text) {
        folded.push_back(static_cast<wchar_t>(std::towlower(static_cast<wint_t>(character))));
    }
    return folded;
}

// HeaderMatchesKeyword reports whether a header contains a keyword, folded and
// trimmed, mirroring the bilingual default keyword list.
bool HeaderMatchesKeyword(const std::wstring& header, const std::wstring& keyword)
{
    const std::wstring foldedKeyword = LowercaseWide(keyword);
    std::size_t begin = 0;
    std::size_t end = foldedKeyword.size();
    while (begin < end && std::iswspace(static_cast<wint_t>(foldedKeyword[begin]))) {
        ++begin;
    }
    while (end > begin && std::iswspace(static_cast<wint_t>(foldedKeyword[end - 1]))) {
        --end;
    }
    if (begin == end) {
        return false;
    }
    return LowercaseWide(header).find(foldedKeyword.substr(begin, end - begin)) !=
        std::wstring::npos;
}

// AppendMatchingColumns adds every column whose header matches a keyword.
void AppendMatchingColumns(std::unordered_set<int>& ignoredColumns,
    const std::vector<TableSnapshotColumn>& columns, const std::vector<std::wstring>& keywords)
{
    for (const TableSnapshotColumn& column : columns) {
        for (const std::wstring& keyword : keywords) {
            if (HeaderMatchesKeyword(column.headerText, keyword)) {
                ignoredColumns.insert(column.sourceColumn);
                break;
            }
        }
    }
}

} // namespace

namespace Ksword::Ui {

TableSnapshotClock CurrentSnapshotClock() noexcept
{
    // The SDK only declares the precise variant for a matching _WIN32_WINNT, so it
    // is bound at run time here and the plain clock covers older targets.
    using PreciseClockApi = void(WINAPI*)(LPFILETIME);
    static const PreciseClockApi preciseClock = reinterpret_cast<PreciseClockApi>(
        reinterpret_cast<void*>(GetProcAddress(
            GetModuleHandleW(L"kernel32.dll"), "GetSystemTimePreciseAsFileTime")));
    FILETIME fileTime{};
    if (preciseClock != nullptr) {
        preciseClock(&fileTime);
    } else {
        GetSystemTimeAsFileTime(&fileTime);
    }
    ULARGE_INTEGER ticks{};
    ticks.LowPart = fileTime.dwLowDateTime;
    ticks.HighPart = fileTime.dwHighDateTime;
    return static_cast<TableSnapshotClock>(ticks.QuadPart);
}

std::uint64_t EstimatedSnapshotBytes(const TableSnapshot& snapshot)
{
    return snapshot.estimatedBytes != 0 ? snapshot.estimatedBytes : SnapshotStorageBytes(snapshot);
}

std::uint64_t TotalEstimatedBytes(const std::vector<TableSnapshot>& snapshots)
{
    std::uint64_t totalBytes = 0;
    for (const TableSnapshot& snapshot : snapshots) {
        totalBytes = SaturatingAdd(totalBytes, EstimatedSnapshotBytes(snapshot));
    }
    return totalBytes;
}

std::wstring SnapshotLabelForOrdinal(const std::uint64_t ordinal)
{
    std::wstring label;
    std::uint64_t current = ordinal + 1;
    while (current > 0) {
        const std::uint64_t remainder = (current - 1) % 26;
        label.insert(label.begin(), static_cast<wchar_t>(L'A' + remainder));
        current = (current - 1) / 26;
    }
    return label;
}

bool TableSnapshotEngine::Capture(const TableSnapshotCaptureInput& input)
{
    TableSnapshot snapshot;
    snapshot.label = input.label;
    snapshot.capturedAt = input.capturedAt != 0 ? input.capturedAt : CurrentSnapshotClock();
    snapshot.sortColumn = input.sortColumn;
    snapshot.sortOrder = input.sortOrder;
    snapshot.keyColumns = input.keyColumns.empty() ? keyColumns_ : input.keyColumns;
    snapshot.estimatedBytes = SnapshotStorageBytes(snapshot);

    if (!input.view.complete()) {
        DiscardInvalidatedSnapshot(snapshot);
        lastEvictedLabels_.clear();
        StoreSnapshot(std::move(snapshot));
        return false;
    }

    const TableSnapshotCaptureLimits& limits = input.limits;
    const int maximumRows = std::max(0, limits.maximumRows);
    const int maximumColumns = std::max(0, limits.maximumColumns);
    const int maximumCellCharacters = std::max(0, limits.maximumCellCharacters);
    const int maximumHeaderCharacters = std::max(0, limits.maximumHeaderCharacters);
    const std::uint64_t maximumEstimatedBytes =
        std::max(limits.maximumEstimatedBytes, snapshot.estimatedBytes);
    const int progressReportRows = std::max(1, limits.progressReportRows);

    const std::vector<TableSnapshotColumn> sourceColumns = input.view.columnTitles();
    const int columnCount = static_cast<int>(sourceColumns.size());
    const int rowCount = std::max(0, input.view.rowCount());
    snapshot.sourceColumnCount = columnCount;
    snapshot.sourceRowCount = rowCount;
    const int columnsToVisit = std::min(columnCount, maximumColumns);
    const int rowsToVisit = std::min(rowCount, maximumRows);

    // 表头按视觉顺序抓取，但每列都带着 sourceColumn 身份，因此用户拖动表头后
    // 两份快照仍然按同一列比对。
    snapshot.visibleColumns.reserve(static_cast<std::size_t>(columnsToVisit));
    for (int visualColumn = 0; visualColumn < columnsToVisit; ++visualColumn) {
        ++snapshot.visitedSourceColumns;
        const TableSnapshotColumn& sourceColumn = sourceColumns[static_cast<std::size_t>(visualColumn)];
        if (sourceColumn.sourceColumn < 0) {
            continue;
        }
        const BoundedText headerText = BoundDisplayText(sourceColumn.headerText, maximumHeaderCharacters);
        RecordBoundedValue(snapshot, headerText, true);
        TableSnapshotColumn column{ sourceColumn.sourceColumn, headerText.text };
        const std::uint64_t columnBytes = ColumnStorageBytes(column);
        if (!CanRetainBytes(snapshot.estimatedBytes, columnBytes, maximumEstimatedBytes)) {
            snapshot.truncatedByByteLimit = true;
            break;
        }
        snapshot.visibleColumns.push_back(std::move(column));
        snapshot.estimatedBytes += columnBytes;
    }
    if (!snapshot.truncatedByByteLimit && columnsToVisit < columnCount) {
        snapshot.truncatedByColumnLimit = true;
    }

    const std::size_t valueCount = snapshot.visibleColumns.size();
    snapshot.rows.reserve(static_cast<std::size_t>(rowsToVisit));
    for (int visualRow = 0; visualRow < rowsToVisit && !snapshot.truncatedByByteLimit; ++visualRow) {
        if (visualRow > 0 && visualRow % progressReportRows == 0 && input.progress) {
            // Win32 UI 是单线程的，没有事件循环可让；进度回调就是唯一的让出点。
            // 返回 false 表示用户取消，本次抓取整体作废且不进入槽位。
            if (!input.progress(static_cast<std::size_t>(snapshot.visitedSourceRows))) {
                return false;
            }
        }
        ++snapshot.visitedSourceRows;
        const int sourceRow = input.view.sourceRowAt ? input.view.sourceRowAt(visualRow) : visualRow;
        if (sourceRow < 0) {
            continue;
        }

        TableSnapshotRow row;
        row.sourceRow = sourceRow;
        row.values.reserve(valueCount);
        std::uint64_t rowBytes = SaturatingAdd(sizeof(TableSnapshotRow),
            static_cast<std::uint64_t>(valueCount) * sizeof(std::wstring));
        if (!CanRetainBytes(snapshot.estimatedBytes, rowBytes, maximumEstimatedBytes)) {
            snapshot.truncatedByByteLimit = true;
            break;
        }
        for (const TableSnapshotColumn& column : snapshot.visibleColumns) {
            const BoundedText value = BoundDisplayText(
                input.view.cellText(sourceRow, column.sourceColumn), maximumCellCharacters);
            RecordBoundedValue(snapshot, value, false);
            rowBytes = SaturatingAdd(rowBytes, TextStorageBytes(value.text));
            if (!CanRetainBytes(snapshot.estimatedBytes, rowBytes, maximumEstimatedBytes)) {
                snapshot.truncatedByByteLimit = true;
                break;
            }
            row.values.push_back(value.text);
        }
        if (snapshot.truncatedByByteLimit) {
            break;
        }
        snapshot.rows.push_back(std::move(row));
        snapshot.estimatedBytes += rowBytes;
    }

    if (!snapshot.truncatedByByteLimit && rowsToVisit < rowCount) {
        snapshot.truncatedByRowLimit = true;
    }
    snapshot.visibleColumns.shrink_to_fit();
    snapshot.rows.shrink_to_fit();
    snapshot.keyColumns.shrink_to_fit();
    snapshot.estimatedBytes = SnapshotStorageBytes(snapshot);

    lastEvictedLabels_.clear();
    StoreSnapshot(std::move(snapshot));
    return true;
}

void TableSnapshotEngine::StoreSnapshot(TableSnapshot snapshot)
{
    // 槽位 A 一旦建立就不再被后续抓取覆盖：重复抓取只重写槽位 B，否则用户每次
    // 刷新都会悄悄挪动比对基线。
    if (snapshots_.size() >= 2) {
        snapshots_.back() = std::move(snapshot);
    } else {
        snapshots_.push_back(std::move(snapshot));
    }
    ++sequence_;
    snapshots_.back().sequence = sequence_;
}

TableComparisonResult TableSnapshotEngine::Compare(const TableComparisonOptions& options) const
{
    TableComparisonResult result;
    const TableSnapshot& earlier = Earlier();
    const TableSnapshot& later = Later();
    result.earlierSnapshot = earlier;
    result.laterSnapshot = later;
    if (snapshots_.size() < 2) {
        result.missingSnapshot = true;
        return result;
    }
    if (earlier.sourceInvalidated || later.sourceInvalidated) {
        result.sourceInvalidated = true;
        return result;
    }

    const TableSnapshotComparisonLimits& limits = options.limits;
    result.columns = CombinedColumns(earlier, later);
    result.ignoredSourceColumns = options.ignoredSourceColumns;

    const SnapshotColumnLookup earlierLookup(result.earlierSnapshot);
    const SnapshotColumnLookup laterLookup(result.laterSnapshot);
    result.earlierValueIndexes = ValueIndexesForColumns(earlierLookup, result.columns);
    result.laterValueIndexes = ValueIndexesForColumns(laterLookup, result.columns);
    result.estimatedBytes = sizeof(TableComparisonResult);
    result.estimatedBytes = SaturatingAdd(result.estimatedBytes, LogicalSnapshotBytes(result.earlierSnapshot));
    result.estimatedBytes = SaturatingAdd(result.estimatedBytes, LogicalSnapshotBytes(result.laterSnapshot));
    result.estimatedBytes = SaturatingAdd(result.estimatedBytes,
        static_cast<std::uint64_t>(result.columns.size()) *
            (sizeof(TableSnapshotColumn) + sizeof(int) * 2ull));
    if (result.estimatedBytes > limits.maximumEstimatedBytes) {
        result.truncatedByResultByteLimit = true;
        return result;
    }
    if (options.shouldCancel && options.shouldCancel()) {
        result.cancelled = true;
        return result;
    }

    ComparisonWork work(limits, options.shouldCancel, result);
    if (!work.ReserveTemporary(static_cast<std::uint64_t>(
            earlierLookup.Size() + laterLookup.Size()) * 64ull)) {
        return result;
    }

    const std::vector<int> requestedKeys = !options.keyColumns.empty()
        ? options.keyColumns
        : keyColumns_;
    const std::vector<int> keyColumns = UsableStableKeyColumns(earlier, later, earlierLookup,
        laterLookup, requestedKeys, result.ignoredSourceColumns);
    std::unordered_map<std::wstring, int> earlierRowsByKey;
    std::unordered_map<std::wstring, int> laterRowsByKey;
    // 键不稳定时绝不能做字段级比对：错位的两行会算出一堆假差异。这里一旦建索引
    // 失败就整体退回整行多重集比对。
    const bool canUseStableKeys = !keyColumns.empty() &&
        BuildUniqueRowKeys(earlier, earlierLookup, keyColumns, earlierRowsByKey, work) &&
        BuildUniqueRowKeys(later, laterLookup, keyColumns, laterRowsByKey, work);
    if (!work.CanContinue()) {
        return result;
    }

    if (canUseStableKeys) {
        result.usesStableKeys = true;
        if (!work.ReserveTemporary(static_cast<std::uint64_t>(later.rows.size()) * sizeof(bool))) {
            return result;
        }
        std::vector<bool> matchedLaterRows(later.rows.size(), false);
        for (std::size_t earlierIndex = 0; earlierIndex < earlier.rows.size(); ++earlierIndex) {
            const TableSnapshotRow& earlierRow = earlier.rows[earlierIndex];
            const std::wstring key = RowKeySignature(earlierRow, earlierLookup, keyColumns, work);
            if (!work.CanContinue()) {
                return result;
            }
            const auto laterIterator = laterRowsByKey.find(key);
            if (laterIterator == laterRowsByKey.end()) {
                if (!AppendComparisonRow(result, limits, TableComparisonSource::EarlierOnly,
                        static_cast<int>(earlierIndex), -1, false)) {
                    return result;
                }
                if (!work.CheckpointAfterRow()) {
                    return result;
                }
                continue;
            }

            const std::size_t laterIndex = static_cast<std::size_t>(laterIterator->second);
            matchedLaterRows[laterIndex] = true;
            const TableSnapshotRow& laterRow = later.rows[laterIndex];
            std::unordered_set<int> changedColumns;
            for (const TableSnapshotColumn& column : result.columns) {
                if (!work.ConsumeWork()) {
                    return result;
                }
                if (result.ignoredSourceColumns.count(column.sourceColumn) == 0 &&
                    ValueForSourceColumn(earlierRow, earlierLookup, column.sourceColumn) !=
                        ValueForSourceColumn(laterRow, laterLookup, column.sourceColumn)) {
                    changedColumns.insert(column.sourceColumn);
                }
            }
            if (!AppendComparisonRow(result, limits, TableComparisonSource::Both,
                    static_cast<int>(earlierIndex), static_cast<int>(laterIndex), true,
                    std::move(changedColumns))) {
                return result;
            }
            if (!work.CheckpointAfterRow()) {
                return result;
            }
        }
        for (std::size_t laterIndex = 0; laterIndex < later.rows.size(); ++laterIndex) {
            if (!matchedLaterRows[laterIndex] &&
                !AppendComparisonRow(result, limits, TableComparisonSource::LaterOnly, -1,
                    static_cast<int>(laterIndex), true)) {
                return result;
            }
            if (!work.CheckpointAfterRow()) {
                return result;
            }
        }
        return result;
    }

    if (!work.ReserveTemporary(static_cast<std::uint64_t>(later.rows.size()) * 112ull)) {
        return result;
    }
    std::unordered_map<std::wstring, std::vector<int>> laterRowsBySignature;
    laterRowsBySignature.reserve(later.rows.size());
    for (std::size_t laterIndex = 0; laterIndex < later.rows.size(); ++laterIndex) {
        const std::wstring signature = RowSignature(later.rows[laterIndex], laterLookup,
            result.columns, result.ignoredSourceColumns, work);
        if (!work.CanContinue()) {
            return result;
        }
        laterRowsBySignature[signature].push_back(static_cast<int>(laterIndex));
        if (!work.CheckpointAfterRow()) {
            return result;
        }
    }

    if (!work.ReserveTemporary(static_cast<std::uint64_t>(later.rows.size()) *
            (sizeof(bool) + 96ull))) {
        return result;
    }
    std::unordered_map<std::wstring, int> matchedPerSignature;
    std::vector<bool> matchedLaterRows(later.rows.size(), false);
    for (std::size_t earlierIndex = 0; earlierIndex < earlier.rows.size(); ++earlierIndex) {
        const TableSnapshotRow& earlierRow = earlier.rows[earlierIndex];
        const std::wstring signature = RowSignature(earlierRow, earlierLookup, result.columns,
            result.ignoredSourceColumns, work);
        if (!work.CanContinue()) {
            return result;
        }
        const auto matchesIterator = laterRowsBySignature.find(signature);
        const int matchOffset = matchedPerSignature[signature];
        if (matchesIterator != laterRowsBySignature.end() &&
            matchOffset < static_cast<int>(matchesIterator->second.size())) {
            const int laterIndex = matchesIterator->second[static_cast<std::size_t>(matchOffset)];
            matchedPerSignature[signature] = matchOffset + 1;
            matchedLaterRows[static_cast<std::size_t>(laterIndex)] = true;
            if (!AppendComparisonRow(result, limits, TableComparisonSource::Both,
                    static_cast<int>(earlierIndex), laterIndex, false)) {
                return result;
            }
        } else if (!AppendComparisonRow(result, limits, TableComparisonSource::EarlierOnly,
                static_cast<int>(earlierIndex), -1, false)) {
            return result;
        }
        if (!work.CheckpointAfterRow()) {
            return result;
        }
    }
    for (std::size_t laterIndex = 0; laterIndex < later.rows.size(); ++laterIndex) {
        if (!matchedLaterRows[laterIndex] &&
            !AppendComparisonRow(result, limits, TableComparisonSource::LaterOnly, -1,
                static_cast<int>(laterIndex), true)) {
            return result;
        }
        if (!work.CheckpointAfterRow()) {
            return result;
        }
    }
    return result;
}

TableSnapshotRetentionResult TableSnapshotEngine::Retain()
{
    TableSnapshotRetentionResult result;
    for (TableSnapshot& snapshot : snapshots_) {
        if (snapshot.estimatedBytes == 0) {
            snapshot.estimatedBytes = SnapshotStorageBytes(snapshot);
        }
    }
    result.remainingEstimatedBytes = TotalEstimatedBytes(snapshots_);

    const int maximumSnapshots = std::max(0, retentionLimits_.maximumSnapshots);
    while (!snapshots_.empty() &&
        (static_cast<int>(snapshots_.size()) > maximumSnapshots ||
            result.remainingEstimatedBytes > retentionLimits_.maximumEstimatedBytes)) {
        const TableSnapshot& oldest = snapshots_.front();
        result.evictedLabels.push_back(oldest.label);
        result.remainingEstimatedBytes = oldest.estimatedBytes >= result.remainingEstimatedBytes
            ? 0
            : result.remainingEstimatedBytes - oldest.estimatedBytes;
        // 淘汰最旧的一份；如果被淘汰的是基础快照，剩下的对比快照升为新的基础。
        snapshots_.erase(snapshots_.begin());
    }
    lastEvictedLabels_ = result.evictedLabels;
    return result;
}

void TableSnapshotEngine::SetKeyColumns(const std::vector<int>& sourceColumns)
{
    keyColumns_.clear();
    keyColumns_.reserve(sourceColumns.size());
    for (const int sourceColumn : sourceColumns) {
        if (sourceColumn < 0 ||
            std::find(keyColumns_.begin(), keyColumns_.end(), sourceColumn) != keyColumns_.end()) {
            continue;
        }
        keyColumns_.push_back(sourceColumn);
    }
}

void TableSnapshotEngine::Clear()
{
    snapshots_.clear();
    lastEvictedLabels_.clear();
}

const TableSnapshot& TableSnapshotEngine::Earlier() const noexcept
{
    static const TableSnapshot emptySnapshot;
    return snapshots_.empty() ? emptySnapshot : snapshots_.front();
}

const TableSnapshot& TableSnapshotEngine::Later() const noexcept
{
    static const TableSnapshot emptySnapshot;
    return snapshots_.size() < 2 ? emptySnapshot : snapshots_.back();
}

// AppendNotice formats one Chinese line with its counts.
std::vector<std::wstring> SnapshotTruncationNotices(const TableSnapshot& snapshot)
{
    std::vector<std::wstring> notices;
    if (snapshot.sourceInvalidated) {
        notices.push_back(L"抓取过程中源表格发生变化或已被取消，本次快照已作废，请重新抓取。");
    }
    if (snapshot.truncatedByRowLimit) {
        notices.push_back(L"行数超过上限，快照只保留了前 " +
            std::to_wstring(snapshot.rows.size()) + L" 行（源表共 " +
            std::to_wstring(snapshot.sourceRowCount) + L" 行）。");
    }
    if (snapshot.truncatedByColumnLimit) {
        notices.push_back(L"列数超过上限，快照只保留了前 " +
            std::to_wstring(snapshot.visibleColumns.size()) + L" 列（源表共 " +
            std::to_wstring(snapshot.sourceColumnCount) + L" 列）。");
    }
    if (snapshot.truncatedByByteLimit) {
        notices.push_back(L"快照已达到估算内存上限（" +
            std::to_wstring(snapshot.estimatedBytes) + L" 字节），抓取提前停止。");
    }
    if (snapshot.truncatedHeaderValueCount > 0 || snapshot.truncatedCellValueCount > 0) {
        notices.push_back(L"文本长度超过上限：" +
            std::to_wstring(snapshot.truncatedHeaderValueCount) + L" 个表头、" +
            std::to_wstring(snapshot.truncatedCellValueCount) + L" 个单元格已截短保存。");
    }
    if (snapshot.unsupportedDisplayValueCount > 0) {
        notices.push_back(L"有 " + std::to_wstring(snapshot.unsupportedDisplayValueCount) +
            L" 个单元格无法转换为文本，已按空值处理。");
    }
    return notices;
}

std::vector<std::wstring> ComparisonTruncationNotices(const TableComparisonResult& result)
{
    std::vector<std::wstring> notices;
    if (result.missingSnapshot) {
        notices.push_back(L"至少需要两份快照才能比对，请先抓取基础快照与对比快照。");
    }
    if (result.sourceInvalidated) {
        notices.push_back(L"参与比对的一份快照已作废，结果不可信，请重新抓取。");
    }
    if (!result.usesStableKeys && !result.missingSnapshot && !result.sourceInvalidated) {
        notices.push_back(L"未提供唯一键列，本次只做整行比对，不标注字段级差异；"
            L"如已指定键列，请确认键值非空且不重复。");
    }
    if (result.truncatedByResultByteLimit) {
        notices.push_back(L"比对结果超过估算内存上限，只输出了前 " +
            std::to_wstring(result.rows.size()) + L" 行。");
    }
    if (result.truncatedByTemporaryByteLimit) {
        notices.push_back(L"比对临时索引超过估算内存上限，结果不完整。");
    }
    if (result.truncatedByWorkLimit) {
        notices.push_back(L"比对工作量超过上限，结果不完整。");
    }
    if (result.cancelled) {
        notices.push_back(L"比对已取消，结果不完整。");
    }
    return notices;
}

std::vector<std::wstring> DefaultIgnoredColumnKeywords()
{
    return {
        L"序号", L"行号", L"时间", L"日期",
        L"Index", L"No.", L"Sequence", L"Time", L"Timestamp", L"Date"
    };
}

std::unordered_set<int> IgnoredColumnIndexesForKeywords(const TableSnapshot& earlier,
    const TableSnapshot& later, const std::vector<std::wstring>& keywords)
{
    std::unordered_set<int> ignoredColumns;
    AppendMatchingColumns(ignoredColumns, earlier.visibleColumns, keywords);
    AppendMatchingColumns(ignoredColumns, later.visibleColumns, keywords);
    return ignoredColumns;
}

} // namespace Ksword::Ui
