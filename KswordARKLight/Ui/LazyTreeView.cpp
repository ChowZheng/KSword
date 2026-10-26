#include "LazyTreeView.h"

#include "TreeViewUtil.h"

namespace Ksword::Ui {

namespace {

// kMoreRowKeyPrefix marks a synthetic "load more" row. Real keys never contain
// U+0001, so a registry path, instance id or pid can never collide with it.
constexpr wchar_t kMoreRowKeyPrefix[] = L"\x01more:";

std::wstring MoreRowKey(const std::wstring& parentKey)
{
    std::wstring key;
    key.reserve(parentKey.size() + 8);
    key.assign(kMoreRowKeyPrefix);
    key += parentKey;
    return key;
}

} // namespace

LazyTreeView::~LazyTreeView()
{
    // The HWND belongs to the page's window tree and is destroyed by it. Only
    // the records and the timer we armed are ours to release.
    if (hwnd_) {
        ::KillTimer(hwnd_, kClearTimerId);
        if (subclassed_) {
            ::RemoveWindowSubclass(hwnd_, &LazyTreeView::clearSubclassProc, kClearTimerId);
            subclassed_ = false;
        }
    }
    nodes_.clear();
    itemByKey_.clear();
    duplicateKeyCount_ = 0;
}

bool LazyTreeView::create(HWND parent, int id, int x, int y, int width, int height, DWORD extraStyle)
{
    HWND tree = CreateTreeView(parent, id, x, y, width, height, extraStyle);
    if (!tree) {
        return false;
    }
    attach(tree);
    return true;
}

void LazyTreeView::attach(HWND treeView) noexcept
{
    hwnd_ = treeView;
    installClearSubclass();
}

void LazyTreeView::installClearSubclass()
{
    if (!hwnd_ || subclassed_) {
        return;
    }
    // The subclass exists only so the sharded clear can own a WM_TIMER. A plain
    // tree control never forwards WM_TIMER to its parent, and asking every page
    // to route one more message would spread this concern across both callers.
    subclassed_ = ::SetWindowSubclass(hwnd_, &LazyTreeView::clearSubclassProc,
        kClearTimerId, reinterpret_cast<DWORD_PTR>(this)) != FALSE;
}

LRESULT CALLBACK LazyTreeView::clearSubclassProc(
    HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR refData)
{
    auto* view = reinterpret_cast<LazyTreeView*>(refData);
    if (view && message == WM_TIMER && wParam == kClearTimerId) {
        view->stepShardedClear();
        return 0;
    }
    if (view && message == WM_NCDESTROY) {
        // The HWND is going away: stop referencing it so a later call cannot
        // send to a recycled handle.
        view->subclassed_ = false;
        view->clearing_ = false;
        view->hwnd_ = nullptr;
        ::RemoveWindowSubclass(hwnd, &LazyTreeView::clearSubclassProc, id);
    }
    return ::DefSubclassProc(hwnd, message, wParam, lParam);
}

void LazyTreeView::setExpansionCallback(ExpansionCallback callback)
{
    onExpand_ = std::move(callback);
}

void LazyTreeView::setChildSource(ChildSource source)
{
    childSource_ = std::move(source);
}

void LazyTreeView::beginUpdate()
{
    if (!hwnd_) {
        return;
    }
    if (updateDepth_++ == 0) {
        ::SendMessageW(hwnd_, WM_SETREDRAW, FALSE, 0);
    }
}

void LazyTreeView::endUpdate()
{
    if (!hwnd_ || updateDepth_ == 0) {
        return;
    }
    if (--updateDepth_ == 0) {
        ::SendMessageW(hwnd_, WM_SETREDRAW, TRUE, 0);
        ::InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

LazyTreeView::Node* LazyTreeView::nodeOf(HTREEITEM item) const
{
    if (!item) {
        return nullptr;
    }
    const auto it = nodes_.find(item);
    return it == nodes_.end() ? nullptr : it->second.get();
}

void LazyTreeView::setChildrenFlag(HTREEITEM item, bool hasChildren)
{
    if (!hwnd_ || !item) {
        return;
    }
    TVITEMW update{};
    update.mask = TVIF_CHILDREN;
    update.hItem = item;
    update.cChildren = hasChildren ? 1 : 0;
    TreeView_SetItem(hwnd_, &update);
}

void LazyTreeView::eraseRecord(HTREEITEM item)
{
    const auto it = nodes_.find(item);
    if (it == nodes_.end()) {
        return;
    }
    if (!it->second->key.empty()) {
        // Only release the key if this record still owns it. A duplicate row
        // never claimed it, and deleting the older row must not make the newer
        // row unfindable.
        const auto keyIt = itemByKey_.find(it->second->key);
        if (keyIt != itemByKey_.end() && keyIt->second == item) {
            itemByKey_.erase(keyIt);
        }
    }
    nodes_.erase(it);
}

HTREEITEM LazyTreeView::insertOne(HTREEITEM parent, const TreeRow& row)
{
    if (!hwnd_) {
        return nullptr;
    }

    TVINSERTSTRUCTW insert{};
    insert.hParent = parent ? parent : TVI_ROOT;
    insert.hInsertAfter = TVI_LAST;
    insert.item.mask = TVIF_TEXT | TVIF_PARAM;
    if (row.hasChildren) {
        // TVIF_CHILDREN is what makes the expander appear without an actual
        // child item existing. Placeholder children used to double the item
        // count of every listing.
        insert.item.mask |= TVIF_CHILDREN;
        insert.item.cChildren = 1;
    }
    insert.item.pszText = const_cast<LPWSTR>(row.text.c_str());
    // The HTREEITEM carries no ownership: the record map is the single owner,
    // so a delete cannot leak and a lookup cannot walk a dangling pointer.
    insert.item.lParam = 0;

    const HTREEITEM item = reinterpret_cast<HTREEITEM>(
        ::SendMessageW(hwnd_, TVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&insert)));
    if (!item) {
        return nullptr;
    }

    auto node = std::make_unique<Node>();
    node->key = row.key;
    node->userData = row.userData;
    node->parent = parent;
    node->hasChildren = row.hasChildren;
    if (!row.key.empty()) {
        // Keep the first claim instead of overwriting. Overwriting used to make
        // findItem() jump to the newest duplicate while the older row stayed on
        // screen, and deleting the older row then erased the key that the newer
        // row was supposed to be reachable by.
        if (itemByKey_.find(node->key) == itemByKey_.end()) {
            itemByKey_.emplace(node->key, item);
        } else {
            ++duplicateKeyCount_;
        }
    }
    nodes_.emplace(item, std::move(node));
    return item;
}

void LazyTreeView::dropNotice(Node& parentNode)
{
    const HTREEITEM item = parentNode.noticeItem;
    if (!item) {
        return;
    }
    parentNode.noticeItem = nullptr;
    // Erase the record first: the TVN_DELETEITEMW that follows then finds
    // nothing and stays a no-op instead of racing this function.
    eraseRecord(item);
    if (hwnd_) {
        TreeView_DeleteItem(hwnd_, item);
    }
}

HTREEITEM LazyTreeView::insertNotice(HTREEITEM parent, Node& parentNode)
{
    const std::size_t total = parentNode.totalKnown != 0 ? parentNode.totalKnown : parentNode.pending.size();
    const std::size_t shown = static_cast<std::size_t>(parentNode.shown);
    const bool capped = shown >= static_cast<std::size_t>(kMaxRowsPerParent);

    std::wstring text;
    if (capped) {
        text = L"\x2026 已截断：只显示前 " + std::to_wstring(shown);
        if (total > shown) {
            text += L" / 共 " + std::to_wstring(total);
        }
        text += L" 项，请用筛选框缩小范围";
    } else {
        text = L"\x2026 载入更多（已显示 " + std::to_wstring(shown) + L" / 共 " + std::to_wstring(total) + L"）";
    }

    TreeRow row;
    row.text = text;
    row.key = MoreRowKey(parentNode.key);
    // No expander: the row is activated by selecting it, so the control never
    // mutates the tree from inside TVN_ITEMEXPANDINGW (where deleting items is
    // undefined behaviour).
    row.hasChildren = false;

    const HTREEITEM item = insertOne(parent, row);
    if (!item) {
        return nullptr;
    }
    if (Node* node = nodeOf(item)) {
        node->notice = true;
    }
    parentNode.noticeItem = item;
    return item;
}

HTREEITEM LazyTreeView::insertBatch(HTREEITEM parent, Node& parentNode)
{
    if (!hwnd_) {
        return nullptr;
    }

    beginUpdate();
    dropNotice(parentNode);

    HTREEITEM firstInserted = nullptr;
    int added = 0;
    while (parentNode.cursor < parentNode.pending.size()
           && parentNode.shown < kMaxRowsPerParent
           && added < kBatchSize) {
        TreeRow& row = parentNode.pending[parentNode.cursor];
        const HTREEITEM item = insertOne(parent, row);
        if (item && !firstInserted) {
            firstInserted = item;
        }
        if (item) {
            ++parentNode.shown;
        }
        // The row now lives in the control. Releasing the text and key keeps
        // the retained copy proportional to what has not been shown, instead
        // of to the whole snapshot.
        row.text.clear();
        row.key.clear();
        row.text.shrink_to_fit();
        row.key.shrink_to_fit();
        ++parentNode.cursor;
        ++added;
    }

    const bool more = parentNode.cursor < parentNode.pending.size();
    if (parentNode.shown >= kMaxRowsPerParent && more) {
        parentNode.truncated = true;
    }
    if (more) {
        insertNotice(parent, parentNode);
    } else {
        parentNode.pending.clear();
        parentNode.pending.shrink_to_fit();
        parentNode.cursor = 0;
    }

    endUpdate();
    return firstInserted;
}

void LazyTreeView::removeChildren(HTREEITEM parent)
{
    if (!hwnd_ || !parent) {
        return;
    }
    HTREEITEM child = TreeView_GetChild(hwnd_, parent);
    if (!child) {
        return;
    }
    beginUpdate();
    while (child) {
        // The sibling handle survives deleting its predecessor, and every
        // record in the removed subtree is released by its own
        // TVN_DELETEITEMW notification.
        const HTREEITEM next = TreeView_GetNextSibling(hwnd_, child);
        TreeView_DeleteItem(hwnd_, child);
        child = next;
    }
    endUpdate();
}

void LazyTreeView::discardAllRecords()
{
    // Dropping the maps up front is what makes the delete notifications cheap:
    // every TVN_DELETEITEMW that arrives afterwards finds no record and returns.
    nodes_.clear();
    itemByKey_.clear();
    duplicateKeyCount_ = 0;
}

void LazyTreeView::clearImmediately()
{
    clearing_ = false;
    ::KillTimer(hwnd_, kClearTimerId);
    beginUpdate();
    ClearTreeView(hwnd_);
    endUpdate();
    applyDeferredRoots();
}

void LazyTreeView::stepShardedClear()
{
    if (!hwnd_) {
        clearing_ = false;
        return;
    }
    const HTREEITEM root = TreeView_GetRoot(hwnd_);
    if (!root) {
        clearing_ = false;
        ::KillTimer(hwnd_, kClearTimerId);
        endUpdate();
        applyDeferredRoots();
        return;
    }
    // One root subtree per tick. Recursion still happens inside TVM_DELETEITEM,
    // but it is now bounded by a single root instead of the whole tree, and the
    // redraw lock stays held across ticks so nothing repaints in between.
    TreeView_DeleteItem(hwnd_, root);
    if (!TreeView_GetRoot(hwnd_)) {
        clearing_ = false;
        ::KillTimer(hwnd_, kClearTimerId);
        endUpdate();
        applyDeferredRoots();
        return;
    }
    ::SetTimer(hwnd_, kClearTimerId, kClearIntervalMs, nullptr);
}

void LazyTreeView::flushShardedClear()
{
    if (!clearing_) {
        return;
    }
    clearing_ = false;
    ::KillTimer(hwnd_, kClearTimerId);
    ClearTreeView(hwnd_);
    endUpdate();
}

void LazyTreeView::applyDeferredRoots()
{
    if (!hasDeferredRoots_) {
        return;
    }
    hasDeferredRoots_ = false;
    std::vector<TreeRow> roots = std::move(deferredRoots_);
    deferredRoots_.clear();
    if (!hwnd_ || roots.empty()) {
        return;
    }
    beginUpdate();
    for (const TreeRow& row : roots) {
        insertOne(nullptr, row);
    }
    endUpdate();
}

void LazyTreeView::reset()
{
    discardAllRecords();
    if (!hwnd_) {
        clearing_ = false;
        return;
    }
    installClearSubclass();
    if (clearing_) {
        // A drain is already armed. Taking the redraw lock a second time here
        // would leave updateDepth_ permanently above zero, so the ticks that are
        // already scheduled are left to finish the job.
        return;
    }
    if (TreeView_GetCount(hwnd_) <= kInlineClearItems) {
        clearImmediately();
        return;
    }
    if (!subclassed_) {
        // Without the subclass there is no WM_TIMER owner, so a sharded clear
        // would never drain and every later setRoots would stay deferred. Fall
        // back to the single-shot delete instead of stalling the tree forever.
        clearImmediately();
        return;
    }
    clearing_ = true;
    // Hold the redraw lock for the whole drain: re-arming it per tick would
    // repaint a half-empty tree once per millisecond.
    beginUpdate();
    stepShardedClear();
}

void LazyTreeView::setRoots(std::vector<TreeRow> roots)
{
    reset();
    if (!hwnd_ || roots.empty()) {
        return;
    }
    if (clearing_) {
        // The previous tree is still draining. Deferring keeps the new roots
        // from being erased by the ticks that are already scheduled.
        deferredRoots_ = std::move(roots);
        hasDeferredRoots_ = true;
        return;
    }
    beginUpdate();
    for (const TreeRow& row : roots) {
        insertOne(nullptr, row);
    }
    endUpdate();
}

void LazyTreeView::installChildren(HTREEITEM parent, Node& node, std::vector<TreeRow> children, bool truncated, bool expand)
{
    removeChildren(parent);
    node.childrenLoaded = true;
    node.childrenLoading = false;
    node.pending = std::move(children);
    node.cursor = 0;
    node.shown = 0;
    node.noticeItem = nullptr;
    node.truncated = truncated;
    node.totalKnown = node.pending.size();

    if (node.pending.empty()) {
        // A key with no children still expanded. Drop the expander so the user
        // is not offered a "+" that can never produce anything.
        node.hasChildren = false;
        setChildrenFlag(parent, false);
        return;
    }

    node.hasChildren = true;
    insertBatch(parent, node);
    if (expand) {
        // The node is already marked loaded, so the ITEMEXPANDING handler lets
        // this through and the click that started the load visibly lands.
        TreeView_Expand(hwnd_, parent, TVE_EXPAND);
    }
}

void LazyTreeView::setChildren(HTREEITEM parent, std::vector<TreeRow> children, bool truncatedByBackend)
{
    if (clearing_) {
        // The parent's record was discarded when the drain started, so this load
        // targets a node from the previous tree. Dropping it is the correct
        // outcome; the page installs new roots once the clear finishes.
        return;
    }
    Node* node = nodeOf(parent);
    if (!node) {
        return;
    }
    installChildren(parent, *node, std::move(children), truncatedByBackend, true);
}

void LazyTreeView::clearChildren(HTREEITEM parent)
{
    Node* node = nodeOf(parent);
    if (!node) {
        return;
    }
    removeChildren(parent);
    node->childrenLoaded = false;
    node->childrenLoading = false;
    node->pending.clear();
    node->cursor = 0;
    node->shown = 0;
    node->noticeItem = nullptr;
    node->truncated = false;
    node->totalKnown = 0;
    node->hasChildren = true;
    setChildrenFlag(parent, true);
}

HTREEITEM LazyTreeView::findItem(const std::wstring& key) const
{
    if (key.empty()) {
        return nullptr;
    }
    const auto it = itemByKey_.find(key);
    return it == itemByKey_.end() ? nullptr : it->second;
}

std::wstring LazyTreeView::keyOf(HTREEITEM item) const
{
    const Node* node = nodeOf(item);
    return node ? node->key : std::wstring();
}

LPARAM LazyTreeView::userDataOf(HTREEITEM item) const
{
    const Node* node = nodeOf(item);
    return node ? node->userData : 0;
}

bool LazyTreeView::isLoaded(HTREEITEM item) const
{
    const Node* node = nodeOf(item);
    return node && node->childrenLoaded;
}

bool LazyTreeView::isNotice(HTREEITEM item) const
{
    const Node* node = nodeOf(item);
    return node && node->notice;
}

HTREEITEM LazyTreeView::selected() const
{
    return hwnd_ ? TreeView_GetSelection(hwnd_) : nullptr;
}

bool LazyTreeView::selectByKey(const std::wstring& key)
{
    const HTREEITEM item = findItem(key);
    if (!hwnd_ || !item) {
        return false;
    }
    const bool previous = silent_;
    silent_ = true;
    TreeView_SelectItem(hwnd_, item);
    // EnsureVisible can expand ancestors, which is exactly what a navigation
    // into a deep path wants; those expansions run the page's own lazy loader.
    TreeView_EnsureVisible(hwnd_, item);
    silent_ = previous;
    return true;
}

LRESULT LazyTreeView::onExpanding(const NMTREEVIEWW& info)
{
    const bool expanding = info.action == TVE_EXPAND
        || (info.action == TVE_TOGGLE && (info.itemNew.state & TVIS_EXPANDED) != 0);
    if (!expanding) {
        return FALSE;
    }

    const HTREEITEM item = info.itemNew.hItem;
    Node* node = nodeOf(item);
    if (!node || node->notice) {
        return FALSE;
    }
    if (node->childrenLoaded) {
        return FALSE;
    }
    if (node->childrenLoading) {
        // A request is already in flight; stay collapsed instead of opening an
        // empty branch that would fill in the wrong order.
        return TRUE;
    }
    if (!node->hasChildren) {
        return FALSE;
    }

    if (childSource_) {
        // Synchronous source: children are already in memory, so load them here
        // and let the control's own expansion continue in this same message. No
        // cancel, no round trip, no empty branch on screen.
        installChildren(item, *node, childSource_(item, node->key), false, false);
        return FALSE;
    }

    node->childrenLoading = true;
    if (onExpand_ && onExpand_(item, node->key)) {
        // Cancelled on purpose: setChildren() will expand it when the data
        // lands, so the user never sees an empty shell.
        return TRUE;
    }

    node->childrenLoading = false;
    node->hasChildren = false;
    setChildrenFlag(item, false);
    return TRUE;
}

bool LazyTreeView::handleNotify(const NMHDR& header, LRESULT& result)
{
    if (!hwnd_ || !header.hwndFrom || header.hwndFrom != hwnd_) {
        return false;
    }

    switch (header.code) {
    case TVN_ITEMEXPANDINGW: {
        const auto* info = reinterpret_cast<const NMTREEVIEWW*>(&header);
        result = onExpanding(*info);
        return true;
    }
    case TVN_DELETEITEMW: {
        const auto* info = reinterpret_cast<const NMTREEVIEWW*>(&header);
        eraseRecord(info->itemOld.hItem);
        result = 0;
        return true;
    }
    case TVN_SELCHANGEDW: {
        const auto* info = reinterpret_cast<const NMTREEVIEWW*>(&header);
        const HTREEITEM item = info->itemNew.hItem;
        Node* node = nodeOf(item);
        if (!node || !node->notice) {
            // A real row: the page owns this selection.
            return false;
        }
        if (silent_) {
            // The control moved the selection itself while inserting.
            result = 0;
            return true;
        }

        HTREEITEM parent = node->parent;
        Node* parentNode = nodeOf(parent);
        if (!parentNode) {
            result = 0;
            return true;
        }

        // Selecting the notice row is the "load more" gesture. Everything that
        // happens below is the control talking to itself, so the page's
        // selection handler must stay quiet throughout.
        const bool previous = silent_;
        silent_ = true;
        // Deleting the item whose TVN_SELCHANGEDW we are inside is the same
        // class of hazard as deleting inside TVN_ITEMEXPANDINGW. Move the
        // selection onto a real neighbour first, then the notice row can go.
        if (HTREEITEM sibling = TreeView_GetPrevSibling(hwnd_, item)) {
            TreeView_SelectItem(hwnd_, sibling);
        } else {
            TreeView_SelectItem(hwnd_, parent);
        }
        const HTREEITEM firstNew = insertBatch(parent, *parentNode);
        if (firstNew) {
            TreeView_SelectItem(hwnd_, firstNew);
            TreeView_EnsureVisible(hwnd_, firstNew);
        }
        silent_ = previous;
        result = 0;
        return true;
    }
    default:
        return false;
    }
}

} // namespace Ksword::Ui
