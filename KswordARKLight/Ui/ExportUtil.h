#pragma once

#include "../Core/Win32Lean.h"
#include "VirtualListView.h"

#include <cstdint>
#include <array>
#include <string>
#include <vector>

namespace Ksword::Ui {

// SanitizeTsvCell replaces the characters a TSV consumer treats as structure —
// tab, CR and LF — with plain spaces. It lives here rather than in each feature
// because every table that builds its own TSV was keeping a private copy of the
// same loop, and a single missed copy turns one cell into two bogus columns.
std::wstring SanitizeTsvCell(std::wstring cell);

// SaveTextFileResult distinguishes an intentional dialog cancellation from a
// failed write so a feature page can keep its local status text accurate.
enum class SaveTextFileResult {
    Saved,
    Cancelled,
    Failed
};

// Destination selection is UI-only. A completed value packet can be handed to
// a worker for lossless serialization/write without moving a dialog or HWND.
struct TextExportDestination {
    std::array<wchar_t, MAX_PATH> path{};
};
SaveTextFileResult ChooseTextExportDestination(HWND owner,
    const wchar_t* suggestedFileName, const wchar_t* dialogTitle,
    const wchar_t* fileFilter, const wchar_t* defaultExtension,
    TextExportDestination& destination, std::wstring* errorOut = nullptr);
SaveTextFileResult WriteUtf8TextToDestination(const TextExportDestination& destination,
    const std::wstring& text, const std::wstring& evidenceSource,
    const std::wstring& evidenceFormat, std::wstring* errorOut = nullptr);

// BuildVisibleVirtualListTsv serializes the currently visible rows of a virtual
// list with the supplied column captions. It deliberately follows the active
// filter rather than exporting hidden rows.
std::wstring BuildVisibleVirtualListTsv(
    const std::vector<std::wstring>& columnTitles,
    const VirtualListView& list);

// ExportRowScope resolves the ambiguity in the word "export" for a filtered
// table. A user who narrowed a filter usually means what is on screen; a user
// building an evidence record means everything the snapshot still holds, and
// that second case used to be impossible outside the kernel page.
enum class ExportRowScope {
    Visible,
    All
};

// ExportRowCount reports how much of the snapshot actually left the process.
// filteredRows is true when a filter hid rows that this export skipped, which
// the caller must surface: silently writing 42 of 5000 rows into a file named
// "export" is the failure this struct exists to prevent.
struct ExportRowCount {
    std::size_t totalRows = 0;
    std::size_t exportedRows = 0;
    bool filteredRows = false;

    bool truncated() const noexcept { return exportedRows < totalRows; }
};

// BuildVirtualListTsv serializes one scope of a virtual list. Inputs are column
// captions, the list and a scope; processing walks the backing snapshot for All
// and the visible index set for Visible; output is TSV text with CRLF endings.
//
// All keeps the visible order while no filter is active, because that order is
// the sort the user just applied. Once a filter is on, only a subset of rows is
// ordered, so All falls back to the snapshot's natural order instead of
// pretending the sort still applies to rows it never touched.
std::wstring BuildVirtualListTsv(
    const std::vector<std::wstring>& columnTitles,
    const VirtualListView& list,
    ExportRowScope scope,
    ExportRowCount* countOut = nullptr);

// BuildAllRowsVirtualListTsv serializes every row in the backing snapshot.
std::wstring BuildAllRowsVirtualListTsv(
    const std::vector<std::wstring>& columnTitles,
    const VirtualListView& list,
    ExportRowCount* countOut = nullptr);

// ExportVirtualListTsvWithDialog asks for a destination, writes one row scope as
// UTF-8 with BOM and records the export in the evidence session. Inputs are the
// owner window, the list, captions, the scope, the suggested name, the dialog
// title and an evidence source label. Processing rejects an export with nothing
// in it instead of writing a BOM-only file. Output is the save result; countOut
// reports how many rows were written so the caller can say so.
SaveTextFileResult ExportVirtualListTsvWithDialog(
    HWND owner,
    const VirtualListView& list,
    const std::vector<std::wstring>& columnTitles,
    ExportRowScope scope,
    const wchar_t* suggestedFileName,
    const wchar_t* dialogTitle,
    const std::wstring& evidenceSource,
    ExportRowCount* countOut,
    std::wstring* errorOut = nullptr);

// WriteUtf8TextFile writes text to an already chosen path as UTF-8 with BOM and
// records the export in the evidence session. It is path-based rather than
// dialog-based on purpose: a worker thread can finish an export the UI thread
// started, without ever touching a dialog or an HWND.
//
// The evidence record keeps the full text only up to an internal size limit and
// degrades to a summary beyond it, so exporting a large table cannot silently
// grow the session by tens of megabytes. An empty evidenceSource suppresses the
// record entirely, which is what a configuration file needs: writing
// callback_rules.kswrules is not an investigation artifact.
SaveTextFileResult WriteUtf8TextFile(
    const std::wstring& path,
    const std::wstring& text,
    const std::wstring& evidenceSource,
    const std::wstring& evidenceFormat,
    std::wstring* errorOut = nullptr);

// CopyTextToClipboard centralizes CF_UNICODETEXT export and records the exact
// text in the current evidence session when the clipboard transfer succeeds.
bool CopyTextToClipboard(
    HWND owner,
    const std::wstring& text,
    const std::wstring& source = L"剪贴板导出");

// SaveUtf8TextFileWithDialog asks for a destination and writes text as UTF-8
// with a BOM. The suggested name, file filter and default extension describe
// the caller's page-specific export. errorOut is filled only for Failed.
SaveTextFileResult SaveUtf8TextFileWithDialog(
    HWND owner,
    const wchar_t* suggestedFileName,
    const wchar_t* dialogTitle,
    const wchar_t* fileFilter,
    const wchar_t* defaultExtension,
    const std::wstring& text,
    std::wstring* errorOut = nullptr);

// SaveBinaryFileWithDialog writes opaque bytes without changing their encoding.
// evidenceText is deliberately separate from the byte payload: the evidence
// session records an auditable description without duplicating arbitrary binary
// contents into a text-only investigation log.
SaveTextFileResult SaveBinaryFileWithDialog(
    HWND owner,
    const wchar_t* suggestedFileName,
    const wchar_t* dialogTitle,
    const wchar_t* fileFilter,
    const wchar_t* defaultExtension,
    const std::vector<std::uint8_t>& bytes,
    const std::wstring& evidenceSource,
    const std::wstring& evidenceText,
    std::wstring* errorOut = nullptr);

} // namespace Ksword::Ui
