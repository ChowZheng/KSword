// T 模块（表格快照比对引擎）的抗压测试。
//
// 被测对象是 KswordARKLight/Ui/TableSnapshotCompare.{h,cpp}：抓取侧用回调式
// 抽象视图（columnTitles / rowCount / cellText），完全不需要 HWND，因此这里全部
// 用合成数据喂满四类限额，不做任何 UI 依赖。
//
// 断言原则：
//   * 期望值一律独立手写（行数、列号、计数、字节预算），不从被测代码反算；
//   * 每条断言都必须能单独证伪：只断言"没崩"不算覆盖，必须同时给出精确计数；
//   * 提示文本只断言"非空 + 含中文 + 含关键数字"，不写死中文字面量 —— 本工程
//     没有 /utf-8 开关，源文件按系统代码页解释，跨翻译单元的宽字面量比对会
//     随编译环境漂移；
//   * 取消语义两侧方向相反（抓取 progress 返回 false = 取消，比对 shouldCancel
//     返回 true = 取消），这里对两者分别断言，写反了必须被抓到。

#include "TestSupport.h"

#include "../KswordARKLight/Ui/TableSnapshotCompare.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

using namespace Ksword::Ui;

// ---------------------------------------------------------------------------
// 合成表：视觉列顺序与源列身份分离，用来模拟用户拖动表头之后的视图。
// ---------------------------------------------------------------------------
struct SyntheticTable {
    std::vector<int> visualOrder;              // 视觉顺序 -> 源列号
    std::vector<std::wstring> headersBySource; // 源列号 -> 表头文本
    int rowCount = 0;
    std::function<std::wstring(int, int)> cellAt;  // (源行, 源列) -> 单元格文本
    std::function<int(int)> sourceRowAt;           // 可选：视觉行 -> 源行
};

SyntheticTable MakeGeneratedTable(const int rowCount, const int columnCount,
                                  std::function<std::wstring(int, int)> cellAt) {
    SyntheticTable table;
    table.rowCount = rowCount;
    for (int column = 0; column < columnCount; ++column) {
        table.visualOrder.push_back(column);
        table.headersBySource.push_back(L"col" + std::to_wstring(column));
    }
    table.cellAt = std::move(cellAt);
    return table;
}

// MakeGridTable 用二维文本构造小表；visualOrder 为空表示按源列号自然顺序。
SyntheticTable MakeGridTable(const std::vector<std::wstring>& headers,
                             const std::vector<std::vector<std::wstring>>& cells,
                             const std::vector<int>& visualOrder = {}) {
    SyntheticTable table;
    table.headersBySource = headers;
    table.visualOrder = visualOrder;
    if (table.visualOrder.empty()) {
        for (std::size_t index = 0; index < headers.size(); ++index) {
            table.visualOrder.push_back(static_cast<int>(index));
        }
    }
    table.rowCount = static_cast<int>(cells.size());
    table.cellAt = [cells](const int sourceRow, const int sourceColumn) -> std::wstring {
        if (sourceRow < 0 || sourceColumn < 0) {
            return std::wstring();
        }
        const std::size_t row = static_cast<std::size_t>(sourceRow);
        const std::size_t column = static_cast<std::size_t>(sourceColumn);
        if (row >= cells.size() || column >= cells[row].size()) {
            return std::wstring();
        }
        return cells[row][column];
    };
    return table;
}

TableSnapshotView MakeView(const SyntheticTable& table) {
    TableSnapshotView view;
    view.columnTitles = [&table]() {
        std::vector<TableSnapshotColumn> columns;
        columns.reserve(table.visualOrder.size());
        for (const int sourceColumn : table.visualOrder) {
            TableSnapshotColumn column;
            column.sourceColumn = sourceColumn;
            if (sourceColumn >= 0 &&
                static_cast<std::size_t>(sourceColumn) < table.headersBySource.size()) {
                column.headerText = table.headersBySource[static_cast<std::size_t>(sourceColumn)];
            }
            columns.push_back(std::move(column));
        }
        return columns;
    };
    view.rowCount = [&table]() { return table.rowCount; };
    view.cellText = [&table](const int sourceRow, const int sourceColumn) -> std::wstring {
        return table.cellAt ? table.cellAt(sourceRow, sourceColumn) : std::wstring();
    };
    if (table.sourceRowAt) {
        view.sourceRowAt = table.sourceRowAt;
    }
    return view;
}

TableSnapshotCaptureInput MakeInput(const SyntheticTable& table, const std::wstring& label,
                                    const std::vector<int>& keyColumns = {}) {
    TableSnapshotCaptureInput input;
    input.view = MakeView(table);
    input.label = label;
    input.keyColumns = keyColumns;
    return input;
}

// ---------------------------------------------------------------------------
// 断言辅助
// ---------------------------------------------------------------------------
int CountSource(const TableComparisonResult& result, const TableComparisonSource source) {
    return static_cast<int>(std::count_if(result.rows.begin(), result.rows.end(),
                                          [source](const TableComparisonRow& row) {
                                              return row.source == source;
                                          }));
}

int CountFieldChangedRows(const TableComparisonResult& result) {
    return static_cast<int>(std::count_if(result.rows.begin(), result.rows.end(),
                                          [](const TableComparisonRow& row) {
                                              return !row.changedSourceColumns.empty();
                                          }));
}

// HasWideNonAscii 断言提示里确实有中文（非 ASCII）——不写死字面量，见文件头说明。
bool HasWideNonAscii(const std::wstring& text) {
    for (const wchar_t character : text) {
        if (static_cast<unsigned int>(character) > 0x7FU) {
            return true;
        }
    }
    return false;
}

const std::wstring* FindNotice(const std::vector<std::wstring>& notices, const wchar_t* needle) {
    for (const std::wstring& notice : notices) {
        if (notice.find(needle) != std::wstring::npos) {
            return &notice;
        }
    }
    return nullptr;
}

bool AllNoticesAreChinese(const std::vector<std::wstring>& notices) {
    if (notices.empty()) {
        return false;
    }
    for (const std::wstring& notice : notices) {
        if (notice.empty() || !HasWideNonAscii(notice)) {
            return false;
        }
    }
    return true;
}

double ElapsedMs(const std::chrono::steady_clock::time_point& begin) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin);
    return static_cast<double>(elapsed.count());
}

// ---------------------------------------------------------------------------
// T-01：行上限截断（10 万行，默认限额）
// ---------------------------------------------------------------------------
void TestRowLimitTruncation(KswordTests::Suite& s) {
    const SyntheticTable table = MakeGeneratedTable(
        100000, 3, [](const int row, const int column) {
            return L"r" + std::to_wstring(row) + L"c" + std::to_wstring(column);
        });

    TableSnapshotEngine engine;
    const auto begin = std::chrono::steady_clock::now();
    const bool captured = engine.Capture(MakeInput(table, L"A"));
    const double captureMs = ElapsedMs(begin);
    std::wcout << L"  [T-01] 100000 rows x 3 cols capture: " << captureMs << L" ms\n";
    KswordTests::Suite::clearIfBroken(std::wcout);

    s.expect(captured, L"T-01 a 100k row table is captured successfully");
    const TableSnapshot& snapshot = engine.Earlier();
    s.expect(snapshot.rows.size() == 10000U,
             L"T-01 the default row limit keeps exactly 10000 rows out of 100000");
    s.expect(snapshot.truncatedByRowLimit,
             L"T-01 the snapshot flags the row limit instead of silently dropping rows");
    s.expect(!snapshot.truncatedByByteLimit,
             L"T-01 short cells hit the row limit first, not the byte limit");
    s.expect(snapshot.sourceRowCount == 100000,
             L"T-01 the snapshot reports the real source row count, not the kept count");
    s.expect(snapshot.visitedSourceRows == 10000,
             L"T-01 only the visited rows are charged against the capture");
    s.expect(snapshot.visibleColumns.size() == 3U,
             L"T-01 all three columns survive a row-only truncation");
    s.expect(snapshot.isTruncated(), L"T-01 the truncated snapshot reports itself as truncated");
    s.expect(engine.Sequence() == 1U, L"T-01 the first capture stamps sequence 1");

    const std::vector<std::wstring> notices = SnapshotTruncationNotices(snapshot);
    s.expect(AllNoticesAreChinese(notices),
             L"T-01 the truncation notices are non-empty Chinese lines");
    const std::wstring* rowNotice = FindNotice(notices, L"10000");
    s.expect(rowNotice != nullptr,
             L"T-01 the row notice states how many rows were kept");
    s.expect(rowNotice != nullptr && rowNotice->find(L"100000") != std::wstring::npos,
             L"T-01 the row notice also states the real source row count");
}

// ---------------------------------------------------------------------------
// T-02：抓取取消语义（progress 返回 false = 取消，两个槽位都不许动）
// ---------------------------------------------------------------------------
void TestCaptureCancellation(KswordTests::Suite& s) {
    const SyntheticTable table = MakeGeneratedTable(
        1000, 2, [](const int row, const int column) {
            return L"v" + std::to_wstring(row) + L"-" + std::to_wstring(column);
        });

    TableSnapshotEngine engine;
    s.expect(engine.Capture(MakeInput(table, L"A")),
             L"T-02 the base capture succeeds before the cancel test");

    const std::uint64_t sequenceBefore = engine.Sequence();
    const std::size_t baseRowCount = engine.Earlier().rows.size();
    const std::wstring baseFirstValue = engine.Earlier().rows.front().values.front();
    s.expect(baseRowCount == 1000U, L"T-02 the base snapshot really holds all 1000 rows");

    TableSnapshotCaptureInput cancelled = MakeInput(table, L"B");
    cancelled.limits.progressReportRows = 100;
    std::size_t progressCalls = 0;
    std::size_t lastVisitedRows = 0;
    cancelled.progress = [&progressCalls, &lastVisitedRows](const std::size_t visitedRows) {
        ++progressCalls;
        lastVisitedRows = visitedRows;
        return visitedRows < 500U;
    };

    s.expect(!engine.Capture(cancelled), L"T-02 a cancelled capture reports failure");
    s.expect(progressCalls > 0U, L"T-02 the progress callback really ran during the capture");
    s.expect(lastVisitedRows == 500U,
             L"T-02 the capture stopped exactly where the callback said to stop");
    s.expect(engine.Sequence() == sequenceBefore,
             L"T-02 a cancelled capture does not advance the sequence");
    s.expect(engine.SnapshotCount() == 1U && !engine.HasLater(),
             L"T-02 a cancelled capture leaves the compare slot empty");
    s.expect(engine.Earlier().rows.size() == baseRowCount &&
                 engine.Earlier().rows.front().values.front() == baseFirstValue,
             L"T-02 the base snapshot is untouched by the cancelled capture");
}

// ---------------------------------------------------------------------------
// T-03：键列字段级 diff（改 1 格 / 少 1 行 / 多 1 行）
// ---------------------------------------------------------------------------
void TestKeyColumnFieldDiff(KswordTests::Suite& s) {
    const std::vector<std::wstring> headers = { L"id", L"value", L"extra" };
    std::vector<std::vector<std::wstring>> earlierRows;
    for (int row = 0; row < 5; ++row) {
        earlierRows.push_back({ L"id-" + std::to_wstring(row),
                                L"value-" + std::to_wstring(row),
                                L"extra-" + std::to_wstring(row) });
    }
    std::vector<std::vector<std::wstring>> laterRows = earlierRows;
    laterRows[2][1] = L"value-2-changed";   // 改 1 个单元格
    laterRows.erase(laterRows.begin() + 3); // 少 1 行（id-3）
    laterRows.push_back({ L"id-5", L"value-5", L"extra-5" });  // 多 1 行

    const SyntheticTable earlierTable = MakeGridTable(headers, earlierRows);
    const SyntheticTable laterTable = MakeGridTable(headers, laterRows);

    TableSnapshotEngine engine;
    engine.SetKeyColumns({ 0 });
    s.expect(engine.KeyColumns() == std::vector<int>{ 0 },
             L"T-03 the engine keeps the asked-for key column");
    s.expect(engine.Capture(MakeInput(earlierTable, L"A")), L"T-03 the earlier capture succeeds");
    s.expect(engine.Capture(MakeInput(laterTable, L"B")), L"T-03 the later capture succeeds");

    const TableComparisonResult result = engine.Compare(TableComparisonOptions{});
    s.expect(!result.missingSnapshot && !result.isTruncated(),
             L"T-03 a complete key-aligned comparison reports no truncation");
    s.expect(result.usesStableKeys, L"T-03 a unique key column enables the field-level diff");
    s.expect(ComparisonTruncationNotices(result).empty(),
             L"T-03 a key-aligned comparison emits no fallback notice");

    s.expect(CountSource(result, TableComparisonSource::Both) == 4,
             L"T-03 four rows pair up by key");
    s.expect(CountSource(result, TableComparisonSource::EarlierOnly) == 1,
             L"T-03 exactly one row exists only in the earlier snapshot");
    s.expect(CountSource(result, TableComparisonSource::LaterOnly) == 1,
             L"T-03 exactly one row exists only in the later snapshot");
    s.expect(CountFieldChangedRows(result) == 1,
             L"T-03 exactly one paired row carries a field-level change");

    bool sawExactChangedColumn = false;
    for (const TableComparisonRow& row : result.rows) {
        if (row.source != TableComparisonSource::Both || row.changedSourceColumns.empty()) {
            continue;
        }
        // 改的是 id-2 那一行的第 1 列（source column 1），必须精确到列号与行号。
        const bool rightRow = row.earlierRowIndex == 2 && row.laterRowIndex == 2 &&
            result.earlierSnapshot.rows[2].values[0] == L"id-2";
        sawExactChangedColumn =
            rightRow && row.changedSourceColumns == std::unordered_set<int>{ 1 };
    }
    s.expect(sawExactChangedColumn,
             L"T-03 the changed cell is reported on source column 1 of the edited row");
    s.expect(result.earlierSnapshot.rows.size() == 5U && result.laterSnapshot.rows.size() == 5U,
             L"T-03 the result owns both captured snapshots unchanged");
}

// ---------------------------------------------------------------------------
// T-04：表头顺序反转后零假差异（本引擎的核心卖点，单独成组）
// ---------------------------------------------------------------------------
void TestHeaderReorderKeepsZeroFalseDiff(KswordTests::Suite& s) {
    const std::vector<std::wstring> headers = { L"id", L"name", L"state" };
    std::vector<std::vector<std::wstring>> rows;
    for (int row = 0; row < 4; ++row) {
        rows.push_back({ L"id-" + std::to_wstring(row),
                         L"name-" + std::to_wstring(row),
                         L"state-" + std::to_wstring(row) });
    }

    // 同一份数据：第二次抓取时视图把表头顺序整个反过来，但 sourceColumn 保持正确。
    const SyntheticTable forward = MakeGridTable(headers, rows, { 0, 1, 2 });
    const SyntheticTable reversed = MakeGridTable(headers, rows, { 2, 1, 0 });

    TableSnapshotEngine engine;
    engine.SetKeyColumns({ 0 });
    s.expect(engine.Capture(MakeInput(forward, L"A")), L"T-04 the forward-order capture succeeds");
    s.expect(engine.Capture(MakeInput(reversed, L"B")), L"T-04 the reversed-order capture succeeds");
    s.expect(engine.Earlier().visibleColumns.size() == 3U &&
                 engine.Earlier().visibleColumns.front().sourceColumn == 0,
             L"T-04 the first snapshot keeps the visual order it was captured in");
    s.expect(engine.Later().visibleColumns.front().sourceColumn == 2,
             L"T-04 the second snapshot really is captured in reversed order");

    const TableComparisonResult result = engine.Compare(TableComparisonOptions{});
    s.expect(!result.isTruncated(), L"T-04 the reordered comparison completes");
    s.expect(result.usesStableKeys, L"T-04 the reordered comparison still diffes by key");
    s.expect(result.rows.size() == 4U, L"T-04 every row pairs up despite the header reorder");
    s.expect(CountSource(result, TableComparisonSource::Both) == 4,
             L"T-04 no row is reported as added or removed because of the reorder");
    s.expect(CountSource(result, TableComparisonSource::EarlierOnly) == 0 &&
                 CountSource(result, TableComparisonSource::LaterOnly) == 0,
             L"T-04 a pure header reorder produces neither additions nor removals");
    s.expect(CountFieldChangedRows(result) == 0,
             L"T-04 a pure header reorder produces zero field-level differences");

    s.expect(result.columns.size() == 3U &&
                 result.columns[0].sourceColumn == 0 && result.columns[1].sourceColumn == 1 &&
                 result.columns[2].sourceColumn == 2,
             L"T-04 the combined column list keeps the earlier visual order");
    s.expect(result.earlierValueIndexes == std::vector<int>({ 0, 1, 2 }) &&
                 result.laterValueIndexes == std::vector<int>({ 2, 1, 0 }),
             L"T-04 each side resolves the same source column through its own visual order");
}

// ---------------------------------------------------------------------------
// T-05：无键列降级为整行比对
// ---------------------------------------------------------------------------
void TestWholeRowFallback(KswordTests::Suite& s) {
    const std::vector<std::wstring> headers = { L"id", L"name", L"state" };
    std::vector<std::vector<std::wstring>> earlierRows;
    for (int row = 0; row < 4; ++row) {
        earlierRows.push_back({ L"id-" + std::to_wstring(row),
                                L"name-" + std::to_wstring(row),
                                L"state-" + std::to_wstring(row) });
    }
    std::vector<std::vector<std::wstring>> laterRows = earlierRows;
    laterRows[2][2] = L"state-2-changed";

    const SyntheticTable earlierTable = MakeGridTable(headers, earlierRows);
    const SyntheticTable laterTable = MakeGridTable(headers, laterRows);

    TableSnapshotEngine engine;
    s.expect(engine.KeyColumns().empty(), L"T-05 no key column is declared for this engine");
    s.expect(engine.Capture(MakeInput(earlierTable, L"A")), L"T-05 the earlier capture succeeds");
    s.expect(engine.Capture(MakeInput(laterTable, L"B")), L"T-05 the later capture succeeds");

    const TableComparisonResult result = engine.Compare(TableComparisonOptions{});
    s.expect(!result.isTruncated(), L"T-05 the fallback comparison completes");
    s.expect(!result.usesStableKeys, L"T-05 a snapshot pair without keys degrades to whole-row matching");
    s.expect(CountFieldChangedRows(result) == 0,
             L"T-05 the fallback never claims a field-level difference");
    for (const TableComparisonRow& row : result.rows) {
        s.expect(row.changedSourceColumns.empty(),
                 L"T-05 every fallback row ships an empty changed-column set");
    }
    s.expect(CountSource(result, TableComparisonSource::Both) == 3,
             L"T-05 the three untouched rows still pair up as identical whole rows");
    s.expect(CountSource(result, TableComparisonSource::EarlierOnly) == 1 &&
                 CountSource(result, TableComparisonSource::LaterOnly) == 1,
             L"T-05 the edited row appears once per side instead of as a field diff");

    const std::vector<std::wstring> notices = ComparisonTruncationNotices(result);
    s.expect(notices.size() == 1U && AllNoticesAreChinese(notices),
             L"T-05 the fallback states in one Chinese line that it only compared whole rows");
}

// ---------------------------------------------------------------------------
// T-06：字节上限提前停止
// ---------------------------------------------------------------------------
void TestByteLimitStopsCapture(KswordTests::Suite& s) {
    const std::wstring payload(40, L'x');
    const SyntheticTable table = MakeGeneratedTable(
        5000, 2, [payload](const int row, const int column) {
            return payload + std::to_wstring(row) + L"-" + std::to_wstring(column);
        });

    TableSnapshotEngine engine;
    TableSnapshotCaptureInput input = MakeInput(table, L"A");
    input.limits.maximumEstimatedBytes = 4096;
    s.expect(engine.Capture(input), L"T-06 a byte-limited capture still reports success");

    const TableSnapshot& snapshot = engine.Earlier();
    s.expect(snapshot.truncatedByByteLimit,
             L"T-06 the snapshot flags the estimated byte limit");
    s.expect(!snapshot.truncatedByRowLimit,
             L"T-06 the byte limit fired before the row limit, so no row-limit flag is set");
    s.expect(snapshot.rows.size() > 0U && snapshot.rows.size() < 5000U,
             L"T-06 the capture stopped early but kept a usable prefix");
    s.expect(snapshot.visibleColumns.size() == 2U,
             L"T-06 both columns fit before the byte budget ran out");
    bool noRaggedRow = true;
    for (const TableSnapshotRow& row : snapshot.rows) {
        if (row.values.size() != snapshot.visibleColumns.size()) {
            noRaggedRow = false;
        }
    }
    s.expect(noRaggedRow,
             L"T-06 a partially filled row is never stored, so no row is ragged");
    s.expect(snapshot.estimatedBytes > 0U,
             L"T-06 the truncated snapshot still accounts its own size");

    const std::vector<std::wstring> notices = SnapshotTruncationNotices(snapshot);
    s.expect(AllNoticesAreChinese(notices),
             L"T-06 the byte-limit notice is a non-empty Chinese line");
    // 字节上限提示里带的是实际估算字节数：必须真的是本次抓取达到的那个数字。
    const std::wstring bytesText = std::to_wstring(snapshot.estimatedBytes);
    s.expect(FindNotice(notices, bytesText.c_str()) != nullptr,
             L"T-06 the notice quotes the estimated byte count that the capture reached");
}

// ---------------------------------------------------------------------------
// T-07：单元格 / 表头文本长度截断
// ---------------------------------------------------------------------------
void TestTextLengthTruncation(KswordTests::Suite& s) {
    const std::wstring header(30, L'H');
    const std::wstring cell(30, L'C');
    const std::vector<std::wstring> headers = { header, header };
    const std::vector<std::vector<std::wstring>> cells = { { cell, cell }, { cell, cell } };
    const SyntheticTable table = MakeGridTable(headers, cells);

    TableSnapshotEngine engine;
    TableSnapshotCaptureInput input = MakeInput(table, L"A");
    input.limits.maximumCellCharacters = 8;
    input.limits.maximumHeaderCharacters = 4;
    s.expect(engine.Capture(input), L"T-07 the text-limited capture succeeds");

    const TableSnapshot& snapshot = engine.Earlier();
    s.expect(snapshot.truncatedByValueLimit, L"T-07 the snapshot flags the value-length limit");
    s.expect(snapshot.truncatedHeaderValueCount == 2,
             L"T-07 exactly two header values are counted as truncated");
    s.expect(snapshot.truncatedCellValueCount == 4,
             L"T-07 exactly four cells are counted as truncated");
    s.expect(snapshot.visibleColumns.size() == 2U && snapshot.rows.size() == 2U,
             L"T-07 the truncation changes text length, not the shape of the snapshot");

    bool headerBound = true;
    for (const TableSnapshotColumn& column : snapshot.visibleColumns) {
        if (column.headerText.size() != 4U) {
            headerBound = false;
        }
    }
    s.expect(headerBound, L"T-07 every kept header is cut to exactly the header limit");
    bool cellBound = true;
    bool prefixKept = true;
    for (const TableSnapshotRow& row : snapshot.rows) {
        for (const std::wstring& value : row.values) {
            if (value.size() != 8U) {
                cellBound = false;
            }
            if (value != cell.substr(0, 8)) {
                prefixKept = false;
            }
        }
    }
    s.expect(cellBound, L"T-07 every kept cell is cut to exactly the cell limit");
    s.expect(prefixKept, L"T-07 the truncation keeps the prefix, not a random suffix");
    s.expect(snapshot.rows[0].values[0] == cell.substr(0, 8),
             L"T-07 the first kept cell carries the leading eight characters");

    const std::vector<std::wstring> notices = SnapshotTruncationNotices(snapshot);
    s.expect(AllNoticesAreChinese(notices),
             L"T-07 the value-length notice is a non-empty Chinese line");
    s.expect(FindNotice(notices, L"2") != nullptr && FindNotice(notices, L"4") != nullptr,
             L"T-07 the notice names both truncation counts");
}

// ---------------------------------------------------------------------------
// T-08：重复抓取不漂移基础快照（含 Retain 淘汰）
// ---------------------------------------------------------------------------
void TestBaseSnapshotNeverDrifts(KswordTests::Suite& s) {
    TableSnapshotEngine engine;
    TableSnapshotRetentionLimits retention;
    retention.maximumSnapshots = 1;
    engine.SetRetentionLimits(retention);

    bool allCaptured = true;
    for (int round = 0; round < 5; ++round) {
        const std::vector<std::wstring> headers = { L"id", L"value" };
        const std::vector<std::vector<std::wstring>> cells = {
            { L"id-0", L"round-" + std::to_wstring(round) }
        };
        const SyntheticTable table = MakeGridTable(headers, cells);
        allCaptured = engine.Capture(MakeInput(table, SnapshotLabelForOrdinal(
            static_cast<std::uint64_t>(round)))) && allCaptured;
        s.expect(engine.SnapshotCount() <= 2U,
                 L"T-08 the engine never retains more than two slots");
    }
    s.expect(allCaptured, L"T-08 all five refreshes are captured");

    s.expect(engine.Sequence() == 5U, L"T-08 five captures advance the sequence five times");
    s.expect(engine.SnapshotCount() == 2U, L"T-08 two slots are live after repeated refreshes");
    s.expect(engine.Earlier().rows[0].values[1] == L"round-0",
             L"T-08 slot A still holds the very first capture after four refreshes");
    s.expect(engine.Earlier().sequence == 1U,
             L"T-08 slot A keeps the sequence number of the first capture");
    s.expect(engine.Later().rows[0].values[1] == L"round-4",
             L"T-08 slot B holds the newest refresh");
    s.expect(engine.Later().sequence == 5U, L"T-08 slot B carries the newest sequence number");
    s.expect(EstimatedSnapshotBytes(engine.Earlier()) == EstimatedSnapshotBytes(engine.Later()),
             L"T-08 both slots account the same size for the same shape of data");

    const TableComparisonResult result = engine.Compare(TableComparisonOptions{});
    // No key column means the engine cannot align the two rows, so the honest
    // answer is "one row gone, one row added" rather than "one field changed".
    // Field-level change sets stay empty in this mode by design: a per-field
    // diff of misaligned rows would state differences that never happened.
    s.expect(result.rows.size() == 2U
                 && CountSource(result, TableComparisonSource::EarlierOnly) == 1
                 && CountSource(result, TableComparisonSource::LaterOnly) == 1,
             L"T-08 without a key the drift shows as one row gone and one row added");
    s.expect(CountFieldChangedRows(result) == 0,
             L"T-08 field-level change sets stay empty when no key column was declared");
    s.expect(result.usesStableKeys == false,
             L"T-08 no key column was declared, so this comparison is whole-row");

    const TableSnapshotRetentionResult retentionResult = engine.Retain();
    s.expect(retentionResult.evictedLabels.size() == 1U &&
                 retentionResult.evictedLabels.front() == L"A",
             L"T-08 retention with a one-snapshot budget evicts the oldest label first");
    s.expect(engine.SnapshotCount() == 1U && engine.HasEarlier() && !engine.HasLater(),
             L"T-08 the surviving compare slot is promoted to base after the eviction");
    s.expect(engine.LastEvictedLabels() == retentionResult.evictedLabels,
             L"T-08 the engine remembers which labels it just evicted");
    s.expect(retentionResult.remainingEstimatedBytes == EstimatedSnapshotBytes(engine.Earlier()),
             L"T-08 the retention result reports the bytes that really remain");
}

// ---------------------------------------------------------------------------
// T-09：退化输入（0 行 / 0 列 / 单列 / 全空字符串）
// ---------------------------------------------------------------------------
void TestDegenerateInputs(KswordTests::Suite& s) {
    // (a) 0 行、有列。
    {
        const std::vector<std::wstring> headers = { L"id", L"value" };
        const SyntheticTable table = MakeGridTable(headers, {});
        TableSnapshotEngine engine;
        s.expect(engine.Capture(MakeInput(table, L"A")),
                 L"T-09 an empty table is captured without failing");
        const TableSnapshot& snapshot = engine.Earlier();
        s.expect(snapshot.rows.empty(), L"T-09 zero rows yield zero snapshot rows");
        s.expect(snapshot.visibleColumns.size() == 2U,
                 L"T-09 the columns are still captured for an empty table");
        s.expect(!snapshot.isTruncated(), L"T-09 an empty table is not a truncated table");
        s.expect(snapshot.empty() == false,
                 L"T-09 a table with columns but no rows is not reported as an empty snapshot");
        s.expect(SnapshotTruncationNotices(snapshot).empty(),
                 L"T-09 an empty table produces no truncation notice");
    }

    // (b) 0 列、有行。
    {
        const SyntheticTable table = MakeGeneratedTable(5, 0, [](const int, const int) {
            return std::wstring();
        });
        TableSnapshotEngine engine;
        s.expect(engine.Capture(MakeInput(table, L"A")),
                 L"T-09 a zero-column table is captured without failing");
        const TableSnapshot& snapshot = engine.Earlier();
        s.expect(snapshot.visibleColumns.empty(), L"T-09 zero columns yield zero snapshot columns");
        s.expect(snapshot.rows.size() == 5U,
                 L"T-09 the rows still exist even though every row is empty");
        bool allEmptyRows = true;
        for (const TableSnapshotRow& row : snapshot.rows) {
            if (!row.values.empty()) {
                allEmptyRows = false;
            }
        }
        s.expect(allEmptyRows, L"T-09 each zero-column row stores no values at all");
        s.expect(!snapshot.isTruncated(), L"T-09 a zero-column table is not truncated");

        s.expect(engine.Capture(MakeInput(table, L"B")),
                 L"T-09 a second zero-column capture succeeds");
        const TableComparisonResult result = engine.Compare(TableComparisonOptions{});
        s.expect(!result.missingSnapshot && !result.isTruncated(),
                 L"T-09 comparing two zero-column snapshots completes");
        s.expect(CountSource(result, TableComparisonSource::Both) == 5,
                 L"T-09 five identical empty rows pair up one to one");
    }

    // (c) 单列、全空字符串、键列全是空值 —— 键不可用，必须降级而不是崩。
    {
        const std::vector<std::wstring> headers = { L"only" };
        const std::vector<std::vector<std::wstring>> cells = { { L"" }, { L"" }, { L"" } };
        const SyntheticTable table = MakeGridTable(headers, cells);
        TableSnapshotEngine engine;
        engine.SetKeyColumns({ 0 });
        s.expect(engine.Capture(MakeInput(table, L"A")), L"T-09 the blank-key capture succeeds");
        s.expect(engine.Capture(MakeInput(table, L"B")), L"T-09 the second blank-key capture succeeds");
        s.expect(engine.Earlier().rows.size() == 3U, L"T-09 all three blank rows are kept");

        const TableComparisonResult result = engine.Compare(TableComparisonOptions{});
        s.expect(!result.isTruncated(), L"T-09 a blank key never truncates the comparison");
        s.expect(!result.usesStableKeys,
                 L"T-09 an all-blank key column is rejected instead of pairing rows blindly");
        s.expect(CountSource(result, TableComparisonSource::Both) == 3,
                 L"T-09 identical blank rows still pair up through the whole-row path");
        s.expect(CountFieldChangedRows(result) == 0,
                 L"T-09 the blank-key fallback reports no field-level difference");
    }

    // (d) 两份 0 行快照互比。
    {
        const std::vector<std::wstring> headers = { L"id" };
        const SyntheticTable table = MakeGridTable(headers, {});
        TableSnapshotEngine engine;
        s.expect(engine.Capture(MakeInput(table, L"A")), L"T-09 the first empty capture succeeds");
        s.expect(engine.Capture(MakeInput(table, L"B")), L"T-09 the second empty capture succeeds");
        const TableComparisonResult result = engine.Compare(TableComparisonOptions{});
        s.expect(result.rows.empty(), L"T-09 two empty snapshots compare to zero rows");
        s.expect(!result.missingSnapshot && !result.isTruncated(),
                 L"T-09 an empty comparison is complete, not missing and not truncated");
    }
}

// ---------------------------------------------------------------------------
// T-10：200 次「抓取 -> 比对 -> Clear」循环
// ---------------------------------------------------------------------------
void TestCaptureCompareLoop(KswordTests::Suite& s) {
    const std::vector<std::wstring> headers = { L"id", L"value" };
    TableSnapshotEngine engine;
    engine.SetKeyColumns({ 0 });

    std::uint64_t previousSequence = 0;
    std::uint64_t firstSnapshotBytes = 0;
    bool sequenceMonotonic = true;
    bool bytesStable = true;
    bool everyRoundComplete = true;
    bool everyRoundCleared = true;

    for (int round = 0; round < 200; ++round) {
        std::vector<std::vector<std::wstring>> cells = {
            { L"id-0", L"v0" }, { L"id-1", L"v1" }, { L"id-2", L"v2" }
        };
        const SyntheticTable base = MakeGridTable(headers, cells);
        cells[1][1] = L"v1-round-" + std::to_wstring(round);
        const SyntheticTable refreshed = MakeGridTable(headers, cells);

        if (!engine.Capture(MakeInput(base, L"A")) ||
            !engine.Capture(MakeInput(refreshed, L"B"))) {
            everyRoundComplete = false;
            break;
        }
        if (engine.Sequence() <= previousSequence) {
            sequenceMonotonic = false;
        }
        previousSequence = engine.Sequence();

        if (round == 0) {
            firstSnapshotBytes = EstimatedSnapshotBytes(engine.Earlier());
        } else if (EstimatedSnapshotBytes(engine.Earlier()) != firstSnapshotBytes) {
            bytesStable = false;
        }

        const TableComparisonResult result = engine.Compare(TableComparisonOptions{});
        if (result.rows.size() != 3U ||
            CountSource(result, TableComparisonSource::Both) != 3 ||
            CountFieldChangedRows(result) != 1) {
            everyRoundComplete = false;
        }

        engine.Clear();
        if (!engine.Empty() || engine.SnapshotCount() != 0U) {
            everyRoundCleared = false;
        }
        if (engine.Sequence() != previousSequence) {
            sequenceMonotonic = false;
        }
    }

    s.expect(everyRoundComplete,
             L"T-10 all 200 capture/compare rounds complete with the expected counts");
    s.expect(sequenceMonotonic,
             L"T-10 the sequence strictly grows and Clear never rewinds it");
    s.expect(everyRoundCleared, L"T-10 Clear empties both slots on every round");
    s.expect(bytesStable,
             L"T-10 the accounted snapshot size does not creep across 200 rounds");
    s.expect(engine.Sequence() == 400U,
             L"T-10 200 rounds of two captures leave the sequence at 400");
    s.expect(engine.Empty(), L"T-10 the engine is empty when the loop finishes");
}

// ---------------------------------------------------------------------------
// T-11：10000 行 x 20 列大表比对（只防病态回归，不做脆弱的性能断言）
// ---------------------------------------------------------------------------
void TestLargeTableComparison(KswordTests::Suite& s) {
    const int columnCount = 20;
    const int rowCount = 10000;
    const SyntheticTable table = MakeGeneratedTable(
        rowCount, columnCount, [](const int row, const int column) {
            return L"r" + std::to_wstring(row) + L"k" + std::to_wstring(column);
        });

    TableSnapshotEngine engine;
    engine.SetKeyColumns({ 0 });

    TableSnapshotCaptureInput first = MakeInput(table, L"A");
    first.limits.maximumEstimatedBytes = 64ull * 1024ull * 1024ull;
    TableSnapshotCaptureInput second = MakeInput(table, L"B");
    second.limits.maximumEstimatedBytes = 64ull * 1024ull * 1024ull;

    const auto begin = std::chrono::steady_clock::now();
    const bool capturedFirst = engine.Capture(first);
    const bool capturedSecond = engine.Capture(second);
    const double captureMs = ElapsedMs(begin);

    const auto compareBegin = std::chrono::steady_clock::now();
    const TableComparisonResult result = engine.Compare(TableComparisonOptions{});
    const double compareMs = ElapsedMs(compareBegin);
    std::wcout << L"  [T-11] 10000 rows x 20 cols: capture " << captureMs
               << L" ms (both slots), compare " << compareMs << L" ms\n";
    KswordTests::Suite::clearIfBroken(std::wcout);

    s.expect(capturedFirst && capturedSecond, L"T-11 both large captures succeed");
    s.expect(engine.Earlier().rows.size() == static_cast<std::size_t>(rowCount),
             L"T-11 the large table is captured without hitting the row limit");
    s.expect(!engine.Earlier().isTruncated(),
             L"T-11 a 10000x20 table fits under the explicit byte budget");
    s.expect(!result.isTruncated(), L"T-11 the large comparison is complete");
    s.expect(result.usesStableKeys, L"T-11 the large comparison uses stable keys");
    s.expect(result.rows.size() == static_cast<std::size_t>(rowCount),
             L"T-11 every row of the earlier side shows up in the result");
    s.expect(CountSource(result, TableComparisonSource::Both) == rowCount &&
                 CountSource(result, TableComparisonSource::EarlierOnly) == 0 &&
                 CountSource(result, TableComparisonSource::LaterOnly) == 0,
             L"T-11 two identical large snapshots produce only paired rows");
    s.expect(CountFieldChangedRows(result) == 0,
             L"T-11 identical large snapshots produce zero false differences");
    // 16,000,000 是引擎默认的工作量上限（TableSnapshotComparisonLimits），独立写死。
    s.expect(result.workUnits > 0U && result.workUnits < 16000000ull,
             L"T-11 the comparison stays well inside the work-unit ceiling");
    s.expect(compareMs < 3000.0,
             L"T-11 the 10000x20 comparison finishes far below the pathological bound");
}

} // namespace

int RunTableSnapshotStressTests() {
    KswordTests::Suite suite(L"T table snapshot stress");
    TestRowLimitTruncation(suite);
    TestCaptureCancellation(suite);
    TestKeyColumnFieldDiff(suite);
    TestHeaderReorderKeepsZeroFalseDiff(suite);
    TestWholeRowFallback(suite);
    TestByteLimitStopsCapture(suite);
    TestTextLengthTruncation(suite);
    TestBaseSnapshotNeverDrifts(suite);
    TestDegenerateInputs(suite);
    TestCaptureCompareLoop(suite);
    TestLargeTableComparison(suite);
    suite.report();
    return suite.failures();
}
