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

// HandleTableCompareCommand runs one menu verb and reports the outcome. Input:
// the engine, the command id, then the list and captions used by capture.
// Processing: dispatches capture/compare/clear and leaves the engine consistent
// when a verb is rejected. Output: true when the id belonged to this menu;
// messageOut receives the status line to show.
bool HandleTableCompareCommand(TableSnapshotEngine& engine, UINT commandId,
    const VirtualListView& list, const std::vector<std::wstring>& columnTitles,
    std::wstring& messageOut);

} // namespace Ksword::Ui
