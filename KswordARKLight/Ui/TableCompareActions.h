#pragma once

#include "MenuUtil.h"
#include "TableSnapshotCompare.h"

#include <string>
#include <vector>

namespace Ksword::Ui {

class VirtualListView;

// Shared context-menu command ids for table snapshot comparison. A page adds
// BuildTableCompareMenuItems() to its existing popup and forwards these ids to
// HandleTableCompareCommand(), so no page has to invent its own verbs or
// remember which slot is the base snapshot.
inline constexpr UINT kMenuTableCaptureSnapshot = 0xE110;
inline constexpr UINT kMenuTableCompareSnapshots = 0xE111;
inline constexpr UINT kMenuTableClearSnapshots = 0xE112;

// BuildTableCompareMenuItems describes the three snapshot verbs for one table.
// Input: the table's engine. Processing: labels the compare entry with the pair
// of slot letters the user is about to diff, and disables verbs that cannot run
// yet. Output: menu descriptors ready for BuildPopupMenu.
std::vector<PopupMenuItem> BuildTableCompareMenuItems(const TableSnapshotEngine& engine);

// AppendTableCompareMenuItems is the same three verbs appended straight onto an
// existing HMENU. Half the pages in this program build their popup with raw
// AppendMenuW calls and only own a fragment of it, so handing them a descriptor
// vector they would have to translate back adds a step without adding anything.
// The caller appends its own separator before this call.
void AppendTableCompareMenuItems(HMENU menu, const TableSnapshotEngine& engine);

// CaptureVisibleListSnapshot stores the list's currently visible rows as the
// table's next snapshot. Input: the engine, the list, the exact column captions
// the page shows (same vector it passes to BuildVisibleVirtualListTsv) and an
// out parameter for a status line. Processing: adapts the virtual list to the
// engine's callback view, so hidden columns stay out and the capture is always
// synchronous on the UI thread. Output: true when a snapshot was stored;
// messageOut always carries a display-ready Chinese line.
bool CaptureVisibleListSnapshot(TableSnapshotEngine& engine, const VirtualListView& list,
    const std::vector<std::wstring>& columnTitles, std::wstring& messageOut);

// CompareListSnapshots diffs the engine's two slots with the project's default
// ignore rules applied. Input: the engine. Processing: pre-selects columns whose
// headers look like refresh-varying noise (index, timestamp) so they do not
// produce false differences on every comparison. Output: a self-contained result.
TableComparisonResult CompareListSnapshots(const TableSnapshotEngine& engine);

// DescribeCapture renders one capture as a status line for the page footer.
// Input: the engine plus the snapshot just stored. Processing: reports row and
// column counts and appends every truncation notice that took effect. Output: a
// display-ready Chinese line, never empty.
std::wstring DescribeCapture(const TableSnapshotEngine& engine, const TableSnapshot& snapshot);

// DescribeComparison renders a comparison as a status line. Input: one result.
// Processing: counts added/removed/changed rows and appends truncation notices.
// Output: a display-ready Chinese line, never empty.
std::wstring DescribeComparison(const TableComparisonResult& result);

// BuildComparisonTsv serializes a comparison for export. Input: one result.
// Processing: writes a status column plus every compared column, using the
// later side where it exists so an added row shows its own values. Output: TSV
// text with CRLF line endings, ready for SaveUtf8TextFileWithDialog.
std::wstring BuildComparisonTsv(const TableComparisonResult& result);

// ComparisonRowStatusText names what happened to one comparison row. Input: one
// row. Processing: prefers the presence flags, then the changed-column set.
// Output: 新增 / 移除 / 变更 / 相同 — the same label the TSV and the result
// window show, so a screenshot and an export never disagree.
std::wstring ComparisonRowStatusText(const TableComparisonRow& row);

// ComparisonCellText reads one compared column out of a row. Input: the result,
// the row and the compared column index. Processing: picks the side that is
// actually displayed (a removed row only has earlier values) and maps the
// compared column onto that side's value index. Output: the display text, empty
// when the column does not exist on the chosen side.
std::wstring ComparisonCellText(const TableComparisonResult& result,
    const TableComparisonRow& row, std::size_t columnIndex);

// TableCompareRows exposes a table that is not a VirtualListView as the three
// callbacks a capture needs. The Process and File pages drive a raw
// LVS_OWNERDATA report control and keep their rows in the page, so forcing them
// through VirtualListView just to reach the engine would mean building a second
// container identical to their own. rowCount is the number of rows the user can
// currently see (after the page's own filtering); sourceRowAt is optional and
// may be left empty when the visual index is the source index; cellText
// addresses columns in the same order as the columnTitles vector.
struct TableCompareRows {
    std::size_t rowCount = 0;
    std::function<int(int visualRow)> sourceRowAt;
    std::function<std::wstring(int sourceRow, int column)> cellText;
    // Optional stable field IDs, parallel to columnTitles. cellText still takes
    // a position in the current schema; the adapter translates IDs to positions.
    // Empty keeps positional identities for tables with a fixed schema.
    std::vector<int> sourceColumns;
};

// CaptureRowSourceSnapshot stores the row source's currently visible rows as the
// table's next snapshot. Same contract as CaptureVisibleListSnapshot, only the
// row access differs.
bool CaptureRowSourceSnapshot(TableSnapshotEngine& engine, const TableCompareRows& rows,
    const std::vector<std::wstring>& columnTitles, std::wstring& messageOut);

// HandleTableCompareCommand runs one menu verb against a row source. owner is
// the window the result window is centred on and disabled while it is open.
bool HandleTableCompareCommand(TableSnapshotEngine& engine, UINT commandId,
    const TableCompareRows& rows, HWND owner, const std::vector<std::wstring>& columnTitles,
    std::wstring& messageOut, const std::wstring& tableName = std::wstring());

// HandleTableCompareCommand runs one menu verb and reports the outcome. Input:
// the engine, the command id, then the list and captions used by capture.
// Processing: dispatches capture/compare/clear, opens the result window for a
// successful comparison and leaves the engine consistent when a verb is
// rejected. tableName only captions that window and names the suggested export
// file; an empty name falls back to a generic caption. Output: true when the id
// belonged to this menu; messageOut receives the status line to show.
bool HandleTableCompareCommand(TableSnapshotEngine& engine, UINT commandId,
    const VirtualListView& list, const std::vector<std::wstring>& columnTitles,
    std::wstring& messageOut, const std::wstring& tableName = std::wstring());

} // namespace Ksword::Ui
