#include "VirtualListView.h"

#include <algorithm>
#include <cwctype>
#include <regex>

namespace Ksword::Ui {
namespace {

bool ContainsCaseInsensitive(const std::wstring& value, const std::wstring& query) {
    if (query.empty()) {
        return true;
    }
    const auto iterator = std::search(value.begin(), value.end(), query.begin(), query.end(), [](const wchar_t left, const wchar_t right) {
        return std::towlower(left) == std::towlower(right);
    });
    return iterator != value.end();
}

} // namespace

bool VirtualListView::create(HWND parent, const int id, const int x, const int y, const int width, const int height, const DWORD extraStyle) {
    hwnd_ = CreateReportListView(parent, id, x, y, width, height, extraStyle | LVS_OWNERDATA);
    return hwnd_ != nullptr;
}

HWND VirtualListView::hwnd() const noexcept {
    return hwnd_;
}

void VirtualListView::detach() noexcept {
    hwnd_ = nullptr;
}

bool VirtualListView::addColumns(const std::vector<ListViewColumn>& columns) {
    return AddListViewColumns(hwnd_, columns);
}

void VirtualListView::setRows(std::vector<VirtualListRow> rows) {
    captureViewportAnchor();
    rows_ = std::make_shared<const std::vector<VirtualListRow>>(std::move(rows));
    rebuildVisibleIndexes();
}

void VirtualListView::setSharedRows(std::shared_ptr<const std::vector<VirtualListRow>> rows) {
    captureViewportAnchor();
    rows_ = std::move(rows);
    rebuildVisibleIndexes();
}

const std::vector<VirtualListRow>& VirtualListView::rows() const noexcept {
    static const std::vector<VirtualListRow> empty;
    return rows_ ? *rows_ : empty;
}

const std::vector<std::size_t>& VirtualListView::visibleIndexes() const noexcept {
    return visibleIndexes_;
}

void VirtualListView::setVisibleIndexes(std::vector<std::size_t> indexes) {
    captureViewportAnchor();
    const std::size_t rowCount = rows_ ? rows_->size() : 0;
    indexes.erase(std::remove_if(indexes.begin(), indexes.end(), [rowCount](const std::size_t index) { return index >= rowCount; }), indexes.end());
    visibleIndexes_ = std::move(indexes);
    updateItemCount();
}

void VirtualListView::resetVisibleIndexes() {
    captureViewportAnchor();
    rebuildVisibleIndexes();
}

void VirtualListView::rebuildVisibleIndexes() {
    visibleIndexes_.resize(rows_ ? rows_->size() : 0);
    for (std::size_t index = 0; index < visibleIndexes_.size(); ++index) {
        visibleIndexes_[index] = index;
    }
    updateItemCount();
}

void VirtualListView::captureViewportAnchor() {
    hasViewportAnchor_ = false;
    viewportAnchorKey_.clear();
    viewportAnchorRow_ = 0;
    if (!hwnd_ || visibleIndexes_.empty()) {
        return;
    }
    const int topVisible = ListView_GetTopIndex(hwnd_);
    const VirtualListRow* row = rowAtVisibleIndex(topVisible);
    if (!row) {
        return;
    }
    viewportAnchorKey_ = row->stableKey;
    viewportAnchorRow_ = visibleIndexes_[static_cast<std::size_t>(topVisible)];
    hasViewportAnchor_ = true;
}

bool VirtualListView::IsValidFilterRegex(const std::wstring& query) {
    if (query.empty()) {
        return false;
    }
    try {
        const std::wregex compiled(query, std::regex_constants::ECMAScript | std::regex_constants::icase);
        static_cast<void>(compiled);
    } catch (const std::regex_error&) {
        return false;
    }
    return true;
}

std::vector<std::size_t> VirtualListView::FilterRowIndexes(
    const std::vector<VirtualListRow>& rows,
    const std::wstring& query,
    const bool useRegex) {
    // Compiling once outside the row loop matters: these snapshots run to tens
    // of thousands of rows and each row carries every detail field as a cell.
    std::wregex compiled;
    bool regexReady = false;
    if (useRegex && !query.empty()) {
        try {
            compiled.assign(query, std::regex_constants::ECMAScript | std::regex_constants::icase);
            regexReady = true;
        } catch (const std::regex_error&) {
            regexReady = false;
        }
    }

    std::vector<std::size_t> indexes;
    indexes.reserve(rows.size());
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const VirtualListRow& row = rows[index];
        bool matched = false;
        if (regexReady) {
            matched = std::regex_search(row.stableKey, compiled);
            for (const std::wstring& cell : row.cells) {
                matched = matched || std::regex_search(cell, compiled);
            }
        } else {
            matched = ContainsCaseInsensitive(row.stableKey, query);
            for (const std::wstring& cell : row.cells) {
                matched = matched || ContainsCaseInsensitive(cell, query);
            }
        }
        if (matched) {
            indexes.push_back(index);
        }
    }
    return indexes;
}

bool VirtualListView::handleNotify(const NMHDR& header, LRESULT& result) {
    if (!hwnd_ || header.hwndFrom != hwnd_) {
        return false;
    }
    if (header.code == LVN_GETDISPINFOW) {
        auto* displayInfo = reinterpret_cast<NMLVDISPINFOW*>(const_cast<NMHDR*>(&header));
        const VirtualListRow* row = rowAtVisibleIndex(displayInfo->item.iItem);
        if (!row) {
            return true;
        }
        if ((displayInfo->item.mask & LVIF_TEXT) != 0) {
            const int column = displayInfo->item.iSubItem;
            const std::wstring empty;
            const std::wstring& text = column >= 0 && static_cast<std::size_t>(column) < row->cells.size() ? row->cells[static_cast<std::size_t>(column)] : empty;
            displayInfo->item.pszText = const_cast<wchar_t*>(text.c_str());
        }
        if ((displayInfo->item.mask & LVIF_PARAM) != 0) {
            displayInfo->item.lParam = row->itemData;
        }
        if ((displayInfo->item.mask & LVIF_IMAGE) != 0 && row->imageIndex >= 0) {
            displayInfo->item.iImage = row->imageIndex;
        }
        result = 0;
        return true;
    }
    if (header.code == NM_CUSTOMDRAW) {
        auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(const_cast<NMHDR*>(&header));
        if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) {
            result = CDRF_NOTIFYITEMDRAW;
            return true;
        }
        if ((draw->nmcd.dwDrawStage & CDDS_ITEMPREPAINT) != 0) {
            const VirtualListRow* row = rowAtVisibleIndex(static_cast<int>(draw->nmcd.dwItemSpec));
            if (row) {
                if (row->textColor != CLR_DEFAULT) {
                    draw->clrText = row->textColor;
                }
                if (row->backgroundColor != CLR_DEFAULT) {
                    draw->clrTextBk = row->backgroundColor;
                }
            }
            result = CDRF_NEWFONT;
            return true;
        }
    }
    return false;
}

std::size_t VirtualListView::rowCount() const noexcept {
    return visibleIndexes_.size();
}

const VirtualListRow* VirtualListView::rowAtVisibleIndex(const int visibleIndex) const noexcept {
    if (visibleIndex < 0 || static_cast<std::size_t>(visibleIndex) >= visibleIndexes_.size()) {
        return nullptr;
    }
    const std::size_t rowIndex = visibleIndexes_[static_cast<std::size_t>(visibleIndex)];
    return rows_ && rowIndex < rows_->size() ? &(*rows_)[rowIndex] : nullptr;
}

void VirtualListView::updateItemCount() {
    if (!hwnd_) {
        return;
    }
    ListView_SetItemCountEx(hwnd_, static_cast<int>(visibleIndexes_.size()), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    // Restore the viewport by identity. Keeping the old top row *number* is what
    // made filtering and sorting jump to an unrelated record: the visible set is
    // reordered, so position 0 no longer means the same row.
    if (hasViewportAnchor_) {
        int target = -1;
        const auto& snapshot = rows();
        if (!viewportAnchorKey_.empty()) {
            for (std::size_t position = 0; position < visibleIndexes_.size(); ++position) {
                const std::size_t rowIndex = visibleIndexes_[position];
                if (rowIndex < snapshot.size() && snapshot[rowIndex].stableKey == viewportAnchorKey_) {
                    target = static_cast<int>(position);
                    break;
                }
            }
        }
        if (target < 0) {
            for (std::size_t position = 0; position < visibleIndexes_.size(); ++position) {
                if (visibleIndexes_[position] == viewportAnchorRow_) {
                    target = static_cast<int>(position);
                    break;
                }
            }
        }
        // The anchor row itself may be filtered out; then the list starts at the
        // beginning, which is the only predictable position left.
        ListView_EnsureVisible(hwnd_, target < 0 ? 0 : target, FALSE);
    }
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

} // namespace Ksword::Ui
