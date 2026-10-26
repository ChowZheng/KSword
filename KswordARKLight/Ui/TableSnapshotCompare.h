#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace Ksword::Ui {

// TableSnapshotClock is a UTC instant expressed as 100-nanosecond ticks since
// 1601-01-01 (the FILETIME epoch). It never wraps and needs no timezone
// handling, so two captures are always ordered by plain integer comparison.
using TableSnapshotClock = std::uint64_t;

// CurrentSnapshotClock reads the system UTC clock. Input: none. Processing:
// queries GetSystemTimePreciseAsFileTime and falls back to
// GetSystemTimeAsFileTime when the precise call is unavailable. Output: 100ns
// ticks ready to store in TableSnapshot::capturedAt.
TableSnapshotClock CurrentSnapshotClock() noexcept;

// TableSnapshotSortOrder records the sort indicator a table showed at capture
// time. None means "natural order as loaded", which a table without an active
// sort indicator reports.
enum class TableSnapshotSortOrder { None, Ascending, Descending };

// TableSnapshotColumn keeps the source-model column identity as well as the text
// shown in the header. sourceColumn is what makes two snapshots comparable after
// the user drags a header section to a new position.
struct TableSnapshotColumn {
    int sourceColumn = -1;
    std::wstring headerText;
};

// TableSnapshotRow stores display text only. The snapshot is therefore
// independent of the source data lifetime and intentionally represents exactly
// what the user saw on screen.
struct TableSnapshotRow {
    int sourceRow = -1;
    std::vector<std::wstring> values;
};

// TableSnapshotCaptureLimits keeps one capture bounded even when the source table
// exposes millions of rows. progressReportRows replaces the Qt event-loop yield:
// the Win32 UI is single threaded, so the engine calls the caller's progress
// callback every N visited rows instead of pumping a nested event loop.
struct TableSnapshotCaptureLimits {
    int maximumRows = 10000;
    int maximumColumns = 256;
    int maximumCellCharacters = 16 * 1024;
    int maximumHeaderCharacters = 1024;
    std::uint64_t maximumEstimatedBytes = 16ull * 1024ull * 1024ull;
    int progressReportRows = 128;
};

// TableSnapshot is one in-memory capture of a table's visible state. Every
// truncation flag is paired with the counters that explain it, because a
// snapshot that silently drops data is worse than a slow one.
struct TableSnapshot {
    std::wstring label;
    TableSnapshotClock capturedAt = 0;
    std::uint64_t sequence = 0;
    std::vector<TableSnapshotColumn> visibleColumns;
    std::vector<TableSnapshotRow> rows;
    int sortColumn = -1;
    TableSnapshotSortOrder sortOrder = TableSnapshotSortOrder::None;
    std::vector<int> keyColumns;
    int sourceRowCount = 0;
    int visitedSourceRows = 0;
    int sourceColumnCount = 0;
    int visitedSourceColumns = 0;
    std::uint64_t estimatedBytes = 0;
    bool truncatedByRowLimit = false;
    bool truncatedByColumnLimit = false;
    bool truncatedByByteLimit = false;
    bool truncatedByValueLimit = false;
    int truncatedHeaderValueCount = 0;
    int truncatedCellValueCount = 0;
    int unsupportedDisplayValueCount = 0;
    bool sourceInvalidated = false;

    bool isTruncated() const noexcept {
        return truncatedByRowLimit || truncatedByColumnLimit ||
            truncatedByByteLimit || truncatedByValueLimit || sourceInvalidated;
    }
    bool empty() const noexcept { return visibleColumns.empty() && rows.empty(); }
};

// TableSnapshotView is the abstract table a capture reads. It deliberately holds
// no HWND and no ListView handle: a UI adapter forwards whatever control it owns,
// and a headless stress test can feed synthetic rows at full speed.
struct TableSnapshotView {
    // Visible columns in visual header order. Each entry carries both the source
    // identity and the header text, so a header reorder keeps two snapshots
    // comparable. The adapter omits hidden columns; the engine visits at most
    // limits.maximumColumns entries.
    std::function<std::vector<TableSnapshotColumn>()> columnTitles;

    // Number of visual rows currently exposed by the view, counted after any
    // filtering the adapter applied (hidden or filtered-out rows are excluded).
    std::function<int()> rowCount;

    // Display text of one cell addressed by source identity. sourceRow is the
    // value produced by sourceRowAt for the current visual row. An empty string
    // is a valid empty cell; there is no null/unset distinction in Win32 text.
    std::function<std::wstring(int sourceRow, int sourceColumn)> cellText;

    // Optional visual -> source row mapping, needed because a sorted ListView
    // rarely matches the model order. A null callback means the visual index is
    // the source index.
    std::function<int(int visualRow)> sourceRowAt;

    // complete reports whether the three mandatory callbacks are all set.
    bool complete() const noexcept {
        return static_cast<bool>(columnTitles) && static_cast<bool>(rowCount) &&
            static_cast<bool>(cellText);
    }
};

// TableSnapshotCaptureInput bundles one capture request. keyColumns declares the
// stable identity columns read from the view; an empty list means the snapshot
// carries no key and comparison degrades to whole-row matching. capturedAt == 0
// asks the engine to stamp CurrentSnapshotClock(). progress is called every
// limits.progressReportRows visited rows and returning false cancels the capture.
struct TableSnapshotCaptureInput {
    TableSnapshotView view;
    std::wstring label;
    int sortColumn = -1;
    TableSnapshotSortOrder sortOrder = TableSnapshotSortOrder::None;
    std::vector<int> keyColumns;
    TableSnapshotClock capturedAt = 0;
    TableSnapshotCaptureLimits limits;
    std::function<bool(std::size_t visitedRows)> progress;
};

// Per-table storage limits stop repeated captures from retaining unbounded
// memory. The engine keeps at most two live snapshots (base and compare), so
// maximumSnapshots only has an effect when it is set to 0 or 1.
struct TableSnapshotRetentionLimits {
    int maximumSnapshots = 8;
    std::uint64_t maximumEstimatedBytes = 64ull * 1024ull * 1024ull;
};

// TableSnapshotRetentionResult reports which snapshots Retain dropped.
struct TableSnapshotRetentionResult {
    std::vector<std::wstring> evictedLabels;
    std::uint64_t remainingEstimatedBytes = 0;
};

// TableComparisonSource tells a comparison row apart by presence.
enum class TableComparisonSource { EarlierOnly, LaterOnly, Both };

// TableComparisonRow values follow TableComparisonResult::columns.
// changedSourceColumns is populated only when stable key columns make
// field-level comparison safe; with unstable keys it stays empty on purpose,
// because a field-level diff of misaligned rows states false differences.
struct TableComparisonRow {
    TableComparisonSource source = TableComparisonSource::Both;
    int earlierRowIndex = -1;
    int laterRowIndex = -1;
    bool displayLater = false;
    std::unordered_set<int> changedSourceColumns;
};

// TableSnapshotComparisonLimits bounds comparison time and memory. Two snapshots
// that each fit under 16 MiB must still not amplify into an unbounded result, so
// the result budget, the temporary index budget and the work-unit ceiling are
// checked separately.
struct TableSnapshotComparisonLimits {
    std::uint64_t maximumEstimatedBytes = 64ull * 1024ull * 1024ull;
    std::uint64_t maximumTemporaryEstimatedBytes = 32ull * 1024ull * 1024ull;
    std::uint64_t maximumWorkUnits = 16000000ull;
    int cancelCheckRows = 64;
};

// TableComparisonResult owns both compared snapshots plus lazy row indexes. Set
// truncatedBy* or cancelled and isTruncated() reports an incomplete comparison.
struct TableComparisonResult {
    TableSnapshot earlierSnapshot;
    TableSnapshot laterSnapshot;
    std::vector<TableSnapshotColumn> columns;
    std::vector<int> earlierValueIndexes;
    std::vector<int> laterValueIndexes;
    std::vector<TableComparisonRow> rows;
    std::unordered_set<int> ignoredSourceColumns;
    std::uint64_t estimatedBytes = 0;
    std::uint64_t temporaryPeakEstimatedBytes = 0;
    std::uint64_t workUnits = 0;
    bool usesStableKeys = false;
    bool truncatedByResultByteLimit = false;
    bool truncatedByTemporaryByteLimit = false;
    bool truncatedByWorkLimit = false;
    bool cancelled = false;
    bool missingSnapshot = false;
    bool sourceInvalidated = false;

    bool isTruncated() const noexcept {
        return truncatedByResultByteLimit || truncatedByTemporaryByteLimit ||
            truncatedByWorkLimit || cancelled || missingSnapshot || sourceInvalidated;
    }
};

// TableComparisonOptions selects what Compare looks at. An empty keyColumns list
// falls back to the engine's SetKeyColumns list, then to the earlier snapshot's
// own keyColumns. shouldCancel is polled every limits.cancelCheckRows processed
// rows and returning true aborts the comparison.
struct TableComparisonOptions {
    std::unordered_set<int> ignoredSourceColumns;
    std::vector<int> keyColumns;
    TableSnapshotComparisonLimits limits;
    std::function<bool()> shouldCancel;
};

// TableSnapshotEngine owns one table's snapshots and the labels shown beside
// them. Slot A is always the base snapshot and is always the "earlier" side of a
// comparison; later captures overwrite slot B instead of evicting A, so the
// baseline never drifts just because the user refreshed a few times.
class TableSnapshotEngine final {
public:
    TableSnapshotEngine() = default;
    explicit TableSnapshotEngine(const TableSnapshotRetentionLimits& retention)
        : retentionLimits_(retention) {}

    // Capture reads the view once and stores the result. Input: the abstract
    // view, label, sort state and limits. Processing: visits columns then rows
    // under every limit, calls progress periodically, and writes slot A on the
    // first capture or slot B afterwards. Output: false when the view is
    // incomplete or the caller cancelled. A view that cannot be read is retained
    // as an invalidated placeholder so the UI can explain the failure, while a
    // caller-cancelled capture leaves both slots untouched.
    bool Capture(const TableSnapshotCaptureInput& input);

    // Compare diffs the two retained snapshots. Input: ignore set, optional key
    // override and limits. Processing: aligns columns by source identity, builds
    // unique row keys when stable keys exist, otherwise falls back to a
    // whole-row multiset match. Output: a result that owns everything it
    // reports; missingSnapshot or sourceInvalidated flags an unusable pair.
    TableComparisonResult Compare(const TableComparisonOptions& options) const;

    // Retain enforces the count and estimated-byte limits by dropping the oldest
    // snapshot first. Input: none. Processing: recomputes byte totals and evicts
    // until both limits hold. Output: the evicted labels and remaining bytes.
    TableSnapshotRetentionResult Retain();

    // SetKeyColumns declares the stable identity columns used by later captures
    // and comparisons. Input: source-model column numbers. There is no return
    // value; duplicate and negative numbers are dropped.
    void SetKeyColumns(const std::vector<int>& sourceColumns);
    const std::vector<int>& KeyColumns() const noexcept { return keyColumns_; }

    void SetRetentionLimits(const TableSnapshotRetentionLimits& limits) { retentionLimits_ = limits; }
    const TableSnapshotRetentionLimits& RetentionLimits() const noexcept { return retentionLimits_; }

    // Clear drops both slots but keeps the monotonic sequence, so a label never
    // gets reused for a different snapshot. Input: none. No value is returned.
    void Clear();

    bool Empty() const noexcept { return snapshots_.empty(); }
    std::size_t SnapshotCount() const noexcept { return snapshots_.size(); }
    std::uint64_t Sequence() const noexcept { return sequence_; }

    // Earlier is slot A, Later is slot B. Both return a shared empty snapshot
    // when the slot is unset, so callers can read fields without a null check.
    const TableSnapshot& Earlier() const noexcept;
    const TableSnapshot& Later() const noexcept;
    bool HasEarlier() const noexcept { return snapshots_.size() >= 1; }
    bool HasLater() const noexcept { return snapshots_.size() >= 2; }

    const std::vector<std::wstring>& LastEvictedLabels() const noexcept { return lastEvictedLabels_; }

private:
    // StoreSnapshot writes one capture into the base or the compare slot and
    // stamps it with the next sequence number. Input: the finished snapshot.
    // Processing: replaces the compare slot once the base exists. No value is
    // returned.
    void StoreSnapshot(TableSnapshot snapshot);

    std::vector<TableSnapshot> snapshots_;  // Index 0 is the base, index 1 the compare slot.
    std::vector<int> keyColumns_;
    TableSnapshotRetentionLimits retentionLimits_;
    std::uint64_t sequence_ = 0;
    std::vector<std::wstring> lastEvictedLabels_;
};

// SnapshotLabelForOrdinal converts a monotonically increasing capture ordinal to
// A, B, ... Z, AA, AB, ... The owner must keep the ordinal monotonic even after
// deleting a snapshot, or two live snapshots can show the same letter.
std::wstring SnapshotLabelForOrdinal(std::uint64_t ordinal);

// SnapshotTruncationNotices explains every capture limit that took effect.
// Input: one snapshot. Processing: inspects the truncation flags and the value
// counters. Output: ready-to-display Chinese lines, empty when nothing was cut.
std::vector<std::wstring> SnapshotTruncationNotices(const TableSnapshot& snapshot);

// ComparisonTruncationNotices explains every comparison limit that took effect,
// and warns when the fallback whole-row match was used instead of a field-level
// diff. Input: one comparison result. Output: display-ready Chinese lines,
// empty when the comparison was complete and key-aligned.
std::vector<std::wstring> ComparisonTruncationNotices(const TableComparisonResult& result);

// DefaultIgnoredColumnKeywords lists built-in header keywords for columns whose
// values change on every refresh (an index column, a capture timestamp, ...).
// Input: none. Output: the bilingual keyword list.
std::vector<std::wstring> DefaultIgnoredColumnKeywords();

// IgnoredColumnIndexesForKeywords returns source columns whose header contains
// any keyword, case-insensitively. Input: both snapshots plus keywords.
// Processing: scans the union of their visible headers. Output: the source
// column numbers the caller may pre-select in an ignore-columns menu.
std::unordered_set<int> IgnoredColumnIndexesForKeywords(
    const TableSnapshot& earlier, const TableSnapshot& later,
    const std::vector<std::wstring>& keywords);

// TotalEstimatedBytes sums retained snapshot sizes with saturating arithmetic.
std::uint64_t TotalEstimatedBytes(const std::vector<TableSnapshot>& snapshots);

// EstimatedSnapshotBytes reports the accounted size of one snapshot, recomputing
// it when estimatedBytes was never filled in. Used by both retention and callers
// that add snapshots outside the engine.
std::uint64_t EstimatedSnapshotBytes(const TableSnapshot& snapshot);

} // namespace Ksword::Ui
