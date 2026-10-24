#include "ExportUtil.h"

#include "EvidenceSession.h"

#include <commdlg.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#pragma comment(lib, "Comdlg32.lib")

namespace Ksword::Ui {

std::wstring SanitizeTsvCell(std::wstring cell) {
    for (wchar_t& character : cell) {
        if (character == L'\t' || character == L'\r' || character == L'\n') {
            character = L' ';
        }
    }
    return cell;
}

void AppendTsvRow(std::wstring& output, const std::vector<std::wstring>& cells,
    const std::size_t columnCount) {
    for (std::size_t column = 0; column < columnCount; ++column) {
        if (column != 0) {
            output.push_back(L'\t');
        }
        if (column < cells.size()) {
            output += SanitizeTsvCell(cells[column]);
        }
    }
    output += L"\r\n";
}

namespace {

bool WriteAll(HANDLE file, const void* data, const std::size_t byteCount, std::wstring* errorOut) {
    if (byteCount == 0U) {
        return true;
    }
    if (!data) {
        if (errorOut) {
            *errorOut = L"导出数据为空。";
        }
        return false;
    }
    const auto* bytes = static_cast<const char*>(data);
    std::size_t offset = 0;
    while (offset < byteCount) {
        const std::size_t remaining = byteCount - offset;
        const DWORD chunk = static_cast<DWORD>((std::min)(
            remaining, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD written = 0;
        if (!::WriteFile(file, bytes + offset, chunk, &written, nullptr) || written != chunk) {
            if (errorOut) {
                *errorOut = L"WriteFile 失败，错误 " + std::to_wstring(::GetLastError()) + L"。";
            }
            return false;
        }
        offset += written;
    }
    return true;
}

SaveTextFileResult ChooseSavePath(HWND owner,
    const wchar_t* suggestedFileName,
    const wchar_t* dialogTitle,
    const wchar_t* fileFilter,
    const wchar_t* defaultExtension,
    std::array<wchar_t, MAX_PATH>& path,
    std::wstring* errorOut) {
    if (suggestedFileName) {
        ::wcsncpy_s(path.data(), path.size(), suggestedFileName, _TRUNCATE);
    }
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.lpstrTitle = dialogTitle;
    dialog.lpstrFilter = fileFilter;
    dialog.lpstrDefExt = defaultExtension;
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (::GetSaveFileNameW(&dialog)) {
        return SaveTextFileResult::Saved;
    }
    const DWORD error = ::CommDlgExtendedError();
    if (error != 0 && errorOut) {
        *errorOut = L"保存对话框失败，错误 " + std::to_wstring(error) + L"。";
    }
    return error == 0 ? SaveTextFileResult::Cancelled : SaveTextFileResult::Failed;
}

SaveTextFileResult WriteExportFile(const wchar_t* path,
    const void* data,
    const std::size_t byteCount,
    std::wstring* errorOut) {
    HANDLE file = ::CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (errorOut) {
            *errorOut = L"无法写入文件，错误 " + std::to_wstring(::GetLastError()) + L"。";
        }
        return SaveTextFileResult::Failed;
    }
    const bool written = WriteAll(file, data, byteCount, errorOut);
    const DWORD closeError = ::CloseHandle(file) ? ERROR_SUCCESS : ::GetLastError();
    if (!written || closeError != ERROR_SUCCESS) {
        if (written && errorOut) {
            *errorOut = L"关闭导出文件失败，错误 " + std::to_wstring(closeError) + L"。";
        }
        return SaveTextFileResult::Failed;
    }
    return SaveTextFileResult::Saved;
}

std::vector<char> ToUtf8WithBom(const std::wstring& text, std::wstring* errorOut) {
    // Empty input must never produce the BOM alone: a 3-byte BOM-only file reads
    // as a successful export even though nothing was written. Callers that need
    // "empty means nothing to export" rely on this returning no bytes at all.
    if (text.empty()) {
        return {};
    }
    const int count = text.empty()
        ? 0
        : ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!text.empty() && count <= 0) {
        if (errorOut) {
            *errorOut = L"WideCharToMultiByte 失败，错误 " + std::to_wstring(::GetLastError()) + L"。";
        }
        return {};
    }
    std::vector<char> bytes;
    bytes.reserve(3U + static_cast<std::size_t>(count));
    bytes.insert(bytes.end(), { static_cast<char>(0xEF), static_cast<char>(0xBB), static_cast<char>(0xBF) });
    if (count > 0) {
        const std::size_t offset = bytes.size();
        bytes.resize(offset + static_cast<std::size_t>(count));
        if (::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), bytes.data() + offset,
                count, nullptr, nullptr) != count) {
            if (errorOut) {
                *errorOut = L"WideCharToMultiByte 失败，错误 " + std::to_wstring(::GetLastError()) + L"。";
            }
            return {};
        }
    }
    return bytes;
}

// kEvidencePayloadLimit is how much exported text the evidence session keeps
// verbatim. Past it the session records a summary instead: a whole-table export
// can reach tens of megabytes, and the session is an investigation log, not a
// second copy of every file the user wrote.
constexpr std::size_t kEvidencePayloadLimit = 64U * 1024U;

void RecordExportEvidence(const std::wstring& source, const std::wstring& format, const std::wstring& text) {
    // An empty source means "this write is not an investigation artifact" --
    // the callback rule file is the one caller that relies on it.
    if (source.empty()) {
        return;
    }
    const std::wstring resolvedFormat = format.empty() ? L"text" : format;
    if (text.size() <= kEvidencePayloadLimit) {
        GlobalEvidenceSession().record(source, resolvedFormat, text);
        return;
    }
    GlobalEvidenceSession().record(source, resolvedFormat,
        L"导出内容较大（" + std::to_wstring(text.size())
            + L" 字符），完整内容已写入导出文件，此处仅记录摘要。");
}

} // namespace

SaveTextFileResult ChooseTextExportDestination(HWND owner,
    const wchar_t* suggestedFileName, const wchar_t* dialogTitle,
    const wchar_t* fileFilter, const wchar_t* defaultExtension,
    TextExportDestination& destination, std::wstring* errorOut) {
    if (errorOut) errorOut->clear();
    return ChooseSavePath(owner, suggestedFileName, dialogTitle, fileFilter,
        defaultExtension, destination.path, errorOut);
}

SaveTextFileResult WriteUtf8TextFile(const std::wstring& path,
    const std::wstring& text,
    const std::wstring& evidenceSource,
    const std::wstring& evidenceFormat,
    std::wstring* errorOut) {
    if (errorOut) errorOut->clear();
    // Distinguish "nothing to export" from a conversion failure so the page can
    // tell the user why the export produced no file.
    if (text.empty()) {
        if (errorOut) *errorOut = L"没有可导出的内容。";
        return SaveTextFileResult::Failed;
    }
    if (path.empty()) {
        if (errorOut) *errorOut = L"导出路径为空。";
        return SaveTextFileResult::Failed;
    }
    std::wstring conversionError;
    const auto bytes = ToUtf8WithBom(text, &conversionError);
    if (bytes.empty()) {
        if (errorOut) *errorOut = conversionError.empty() ? L"无法转换导出文本。" : conversionError;
        return SaveTextFileResult::Failed;
    }
    const auto result = WriteExportFile(path.c_str(), bytes.data(), bytes.size(), errorOut);
    if (result == SaveTextFileResult::Saved) {
        RecordExportEvidence(evidenceSource, evidenceFormat, text);
    }
    return result;
}

SaveTextFileResult WriteUtf8TextToDestination(const TextExportDestination& destination,
    const std::wstring& text, const std::wstring& evidenceSource,
    const std::wstring& evidenceFormat, std::wstring* errorOut) {
    return WriteUtf8TextFile(destination.path.data(), text, evidenceSource, evidenceFormat, errorOut);
}

std::wstring BuildVisibleVirtualListTsv(
    const std::vector<std::wstring>& columnTitles,
    const VirtualListView& list) {
    // Kept as the page-facing name for "export what I can see"; the scope-aware
    // builder owns the traversal so the two scopes cannot drift apart.
    return BuildVirtualListTsv(columnTitles, list, ExportRowScope::Visible, nullptr);
}

std::wstring BuildVirtualListTsv(
    const std::vector<std::wstring>& columnTitles,
    const VirtualListView& list,
    ExportRowScope scope,
    ExportRowCount* countOut) {
    if (countOut) {
        *countOut = ExportRowCount{};
    }
    const auto& rows = list.rows();
    const auto& visible = list.visibleIndexes();
    if (columnTitles.empty() || rows.empty()) {
        return {};
    }

    std::wstring output;
    AppendTsvRow(output, columnTitles, columnTitles.size());

    std::size_t exported = 0;
    const bool walkVisible = scope == ExportRowScope::Visible || visible.size() == rows.size();
    if (walkVisible) {
        // With no filter active the visible index set covers every row, so it is
        // also the sorted order the user is looking at. Reusing it keeps "export
        // all" from silently dropping an applied sort in the common case.
        for (const std::size_t index : visible) {
            if (index < rows.size()) {
                AppendTsvRow(output, rows[index].cells, columnTitles.size());
                ++exported;
            }
        }
    } else {
        // A filter is on, so only a subset carries the sort. Exporting the
        // snapshot's natural order is honest; pretending the sort still covers
        // rows it never ordered would not be.
        for (const auto& row : rows) {
            AppendTsvRow(output, row.cells, columnTitles.size());
            ++exported;
        }
    }

    if (countOut) {
        countOut->totalRows = rows.size();
        countOut->exportedRows = exported;
        countOut->filteredRows = scope == ExportRowScope::Visible && exported < rows.size();
    }
    return output;
}

std::wstring BuildAllRowsVirtualListTsv(
    const std::vector<std::wstring>& columnTitles,
    const VirtualListView& list,
    ExportRowCount* countOut) {
    return BuildVirtualListTsv(columnTitles, list, ExportRowScope::All, countOut);
}

SaveTextFileResult ExportVirtualListTsvWithDialog(
    HWND owner,
    const VirtualListView& list,
    const std::vector<std::wstring>& columnTitles,
    ExportRowScope scope,
    const wchar_t* suggestedFileName,
    const wchar_t* dialogTitle,
    const std::wstring& evidenceSource,
    ExportRowCount* countOut,
    std::wstring* errorOut) {
    if (errorOut) {
        errorOut->clear();
    }
    ExportRowCount count;
    const std::wstring text = BuildVirtualListTsv(columnTitles, list, scope, &count);
    if (countOut) {
        *countOut = count;
    }
    if (text.empty()) {
        if (errorOut) {
            *errorOut = L"没有可导出的行。";
        }
        return SaveTextFileResult::Failed;
    }

    std::array<wchar_t, MAX_PATH> path{};
    const SaveTextFileResult selection = ChooseSavePath(owner, suggestedFileName, dialogTitle,
        L"制表符分隔文件 (*.tsv)\0*.tsv\0所有文件 (*.*)\0*.*\0", L"tsv", path, errorOut);
    if (selection != SaveTextFileResult::Saved) {
        return selection;
    }
    return WriteUtf8TextFile(path.data(), text,
        evidenceSource.empty() ? L"表格导出" : evidenceSource, L"tsv", errorOut);
}

bool CopyTextToClipboard(HWND owner, const std::wstring& text, const std::wstring& source) {
    if (text.empty() || !::OpenClipboard(owner)) {
        return false;
    }
    if (!::EmptyClipboard()) {
        ::CloseClipboard();
        return false;
    }
    const SIZE_T bytes = (text.size() + 1U) * sizeof(wchar_t);
    HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) {
        ::CloseClipboard();
        return false;
    }
    void* target = ::GlobalLock(memory);
    if (!target) {
        ::GlobalFree(memory);
        ::CloseClipboard();
        return false;
    }
    std::memcpy(target, text.c_str(), bytes);
    ::GlobalUnlock(memory);
    if (!::SetClipboardData(CF_UNICODETEXT, memory)) {
        ::GlobalFree(memory);
        ::CloseClipboard();
        return false;
    }
    ::CloseClipboard();
    GlobalEvidenceSession().record(source.empty() ? L"剪贴板导出" : source, L"text", text);
    return true;
}

SaveTextFileResult SaveUtf8TextFileWithDialog(
    HWND owner,
    const wchar_t* suggestedFileName,
    const wchar_t* dialogTitle,
    const wchar_t* fileFilter,
    const wchar_t* defaultExtension,
    const std::wstring& text,
    std::wstring* errorOut) {
    if (errorOut) {
        errorOut->clear();
    }
    std::array<wchar_t, MAX_PATH> path{};
    const SaveTextFileResult selection = ChooseSavePath(owner, suggestedFileName, dialogTitle, fileFilter, defaultExtension, path, errorOut);
    if (selection != SaveTextFileResult::Saved) {
        return selection;
    }

    TextExportDestination destination;
    destination.path = path;
    return WriteUtf8TextToDestination(destination, text,
        dialogTitle ? dialogTitle : L"文件导出",
        defaultExtension ? defaultExtension : L"text", errorOut);
}

SaveTextFileResult SaveBinaryFileWithDialog(
    HWND owner,
    const wchar_t* suggestedFileName,
    const wchar_t* dialogTitle,
    const wchar_t* fileFilter,
    const wchar_t* defaultExtension,
    const std::vector<std::uint8_t>& bytes,
    const std::wstring& evidenceSource,
    const std::wstring& evidenceText,
    std::wstring* errorOut) {
    if (errorOut) {
        errorOut->clear();
    }
    if (bytes.empty()) {
        if (errorOut) {
            *errorOut = L"没有可导出的二进制字节。";
        }
        return SaveTextFileResult::Failed;
    }
    std::array<wchar_t, MAX_PATH> path{};
    const SaveTextFileResult selection = ChooseSavePath(owner, suggestedFileName, dialogTitle, fileFilter, defaultExtension, path, errorOut);
    if (selection != SaveTextFileResult::Saved) {
        return selection;
    }
    const SaveTextFileResult writeResult = WriteExportFile(path.data(), bytes.data(), bytes.size(), errorOut);
    if (writeResult != SaveTextFileResult::Saved) {
        return writeResult;
    }
    GlobalEvidenceSession().record(
        evidenceSource.empty() ? L"二进制文件导出" : evidenceSource,
        defaultExtension ? defaultExtension : L"binary",
        evidenceText.empty() ? L"二进制导出已完成。" : evidenceText);
    return SaveTextFileResult::Saved;
}

} // namespace Ksword::Ui
