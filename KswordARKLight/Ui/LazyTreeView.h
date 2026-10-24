#pragma once

#include "../Core/Win32Lean.h"

#include <commctrl.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Ksword::Ui {

// TreeRow is one logical tree node handed to LazyTreeView. The control never
// interprets text, key or userData.
struct TreeRow {
    std::wstring text;
    // key is the caller's stable identity (a registry path, a PnP instance id,
    // a pid). It is indexed, so findItem() is a hash lookup instead of the
    // recursive TreeView walk pages used to hand-roll.
    std::wstring key;
    LPARAM userData = 0;
    // hasChildren only drives the expander. It is published through
    // TVIF_CHILDREN, so no placeholder child is inserted: the old "insert one
    // empty child per node to get an expander" trick doubles the item count of
    // every listing for no benefit.
    bool hasChildren = false;
};

// LazyTreeView owns a WC_TREEVIEWW plus the per-node records behind it. It
// exists because a plain TreeView has no owner-data mode: every node is a real
// window item, so a page that inserts a whole snapshot in one loop blocks the
// UI thread for the duration of that loop.
//
// Four rules it enforces:
//   1. Insertion is batched. setChildren() inserts at most kBatchSize rows and
//      leaves a "load more" row; activating that row inserts the next batch.
//      No single call ever inserts an unbounded number of nodes.
//   2. One parent is capped at kMaxRowsPerParent inserted rows. Past that the
//      control stops and leaves a truncation row that carries the real total,
//      instead of growing until the process stalls.
//   3. Records live in the control, keyed by HTREEITEM, and keys are indexed.
//      Deleting an item cannot leak a record, and lookups are O(1).
//   4. Every burst runs under WM_SETREDRAW(FALSE) with one invalidate at the
//      end, so insertion cost is item bookkeeping instead of bookkeeping plus
//      a repaint per node.
//
// The caller still owns data acquisition: a page enables lazy expansion by
// returning true from the expansion callback and later calling setChildren().
// The control never queries a backend itself.
class LazyTreeView final {
public:
    // kBatchSize is how many rows one insertion pass may add.
    static constexpr int kBatchSize = 400;
    // kMaxRowsPerParent caps one parent's inserted rows. Reaching it is
    // reported, never silent.
    static constexpr int kMaxRowsPerParent = 5000;

    // ExpansionCallback runs when the user expands a node whose children have
    // not been loaded. Returning true means the request was accepted and
    // setChildren() will follow; the control keeps the node collapsed until
    // that happens so the user never stares at an empty branch. Returning false
    // means the control removes the expander and gives up.
    using ExpansionCallback = std::function<bool(HTREEITEM item, const std::wstring& key)>;

    // ChildSource supplies a node's children synchronously, out of memory that is
    // already loaded. When one is installed the expansion callback is never used:
    // expanding a node inserts its first batch on the spot and the expansion
    // proceeds in the same message, so there is no cancel, no round trip, and no
    // empty branch. Pages whose tree is a projection of an in-memory model want
    // this; pages that must query a backend want setExpansionCallback.
    //
    // Inserting children from inside TVN_ITEMEXPANDINGW is the textbook pattern.
    // What is *not* safe there is deleting the item being expanded, which is why
    // the "load more" row is driven by selection instead.
    using ChildSource = std::function<std::vector<TreeRow>(HTREEITEM item, const std::wstring& key)>;

    LazyTreeView() = default;
    ~LazyTreeView();

    LazyTreeView(const LazyTreeView&) = delete;
    LazyTreeView& operator=(const LazyTreeView&) = delete;

    bool create(HWND parent, int id, int x, int y, int width, int height, DWORD extraStyle = 0);
    HWND hwnd() const noexcept { return hwnd_; }

    // attach adopts an externally created tree. Records stay owned by this
    // object, so the page must route WM_NOTIFY here.
    void attach(HWND treeView) noexcept;

    void setExpansionCallback(ExpansionCallback callback);
    void setChildSource(ChildSource source);

    // reset drops every record and empties the tree.
    //
    // TreeView has no owner-data mode, so TVM_DELETEITEM(TVI_ROOT) erases an
    // arbitrarily large expanded subtree inside one message. reset() therefore
    // drops the record maps first -- the TVN_DELETEITEMW storm that follows then
    // costs an empty lookup per node instead of a hash erase -- and then removes
    // one root subtree per timer tick. A tree small enough to erase outright is
    // still cleared synchronously. setRoots() called while a sharded clear is
    // running defers its roots until the clear finishes.
    void reset();

    // setRoots replaces the root level (registry hives, drive letters). Roots
    // are few and are inserted in one pass.
    void setRoots(std::vector<TreeRow> roots);

    // setChildren installs the children of one parent and expands it. The full
    // list is adopted; the control decides how much to insert now and keeps the
    // rest for the next batch.
    void setChildren(HTREEITEM parent, std::vector<TreeRow> children, bool truncatedByBackend = false);

    // clearChildren removes the children of one parent and marks it unloaded so
    // the next expansion requests them again. Use it to recover from a failed
    // load; use setChildren(parent, {}) to record "this key is empty".
    void clearChildren(HTREEITEM parent);

    // findItem returns the node with the given key, or nullptr.
    HTREEITEM findItem(const std::wstring& key) const;
    // duplicateKeyCount reports how many rows arrived carrying a key that an
    // already live row had claimed. The first claim wins; a duplicate row is
    // still shown but is not addressable through findItem. A page that sees a
    // nonzero count has a key bug of its own: two rows are being given the same
    // identity, and navigation by key will silently land on the first one.
    std::size_t duplicateKeyCount() const noexcept { return duplicateKeyCount_; }
    std::wstring keyOf(HTREEITEM item) const;
    LPARAM userDataOf(HTREEITEM item) const;
    bool isLoaded(HTREEITEM item) const;
    // isNotice reports a synthetic "load more" / "truncated" row: the page must
    // never navigate on one.
    bool isNotice(HTREEITEM item) const;
    HTREEITEM selected() const;

    // selectByKey selects and reveals a node. The selection is silent: the page
    // already knows it navigated, and letting its own selection handler run
    // again would restart the navigation it just performed.
    bool selectByKey(const std::wstring& key);

    // silentSelection is true while the control is changing the selection
    // itself. A page's TVN_SELCHANGEDW handler must return immediately when it
    // is set.
    bool silentSelection() const noexcept { return silent_; }

    // handleNotify consumes TVN_ITEMEXPANDINGW, TVN_DELETEITEMW and the
    // TVN_SELCHANGEDW of a notice row. Returns true when the notification is
    // owned by the control; the page then returns result from WM_NOTIFY without
    // doing anything else.
    bool handleNotify(const NMHDR& header, LRESULT& result);

    // beginUpdate/endUpdate wrap a burst of insertions so the tree does not
    // repaint per node. They nest.
    void beginUpdate();
    void endUpdate();

private:
    struct Node {
        std::wstring key;
        LPARAM userData = 0;
        HTREEITEM parent = nullptr;
        bool hasChildren = false;
        bool childrenLoaded = false;
        bool childrenLoading = false;
        bool notice = false;
        bool truncated = false;
        // pending holds the adopted child list minus what has been inserted:
        // cursor is how many entries are consumed and their text is cleared, so
        // the retained copy only costs what has not been shown yet.
        std::vector<TreeRow> pending;
        std::size_t cursor = 0;
        std::size_t totalKnown = 0;
        int shown = 0;
        // noticeItem is the synthetic last row under this parent, if any.
        HTREEITEM noticeItem = nullptr;
    };

    Node* nodeOf(HTREEITEM item) const;
    LRESULT onExpanding(const NMTREEVIEWW& info);
    HTREEITEM insertOne(HTREEITEM parent, const TreeRow& row);
    HTREEITEM insertNotice(HTREEITEM parent, Node& parentNode);
    // insertBatch appends up to kBatchSize rows under parent and returns the
    // first item it inserted (nullptr when nothing was added).
    HTREEITEM insertBatch(HTREEITEM parent, Node& parentNode);
    // installChildren adopts a child list and inserts the first batch. expand tells
    // it whether to open the parent afterwards; a synchronous ChildSource must pass
    // false because the control's own expansion is already in progress.
    void installChildren(HTREEITEM parent, Node& node, std::vector<TreeRow> children, bool truncated, bool expand);
    void dropNotice(Node& parentNode);
    void removeChildren(HTREEITEM parent);
    void eraseRecord(HTREEITEM item);
    void setChildrenFlag(HTREEITEM item, bool hasChildren);

    // Sharded clear. kInlineClearItems is the point below which one
    // TVM_DELETEITEM(TVI_ROOT) is cheaper than scheduling ticks at all.
    static constexpr UINT kInlineClearItems = 400;
    static constexpr UINT_PTR kClearTimerId = 0x4B53;
    static constexpr UINT kClearIntervalMs = 1;

    void installClearSubclass();
    void discardAllRecords();
    void clearImmediately();
    void stepShardedClear();
    void flushShardedClear();
    void applyDeferredRoots();
    static LRESULT CALLBACK clearSubclassProc(
        HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR refData);

private:
    HWND hwnd_ = nullptr;
    std::unordered_map<HTREEITEM, std::unique_ptr<Node>> nodes_;
    // itemByKey_ is the index that kills the recursive FindTreeItemByPath scan
    // every page used to run on each expand.
    std::unordered_map<std::wstring, HTREEITEM> itemByKey_;
    std::size_t duplicateKeyCount_ = 0;
    ExpansionCallback onExpand_;
    ChildSource childSource_;
    int updateDepth_ = 0;
    bool silent_ = false;
    // clearing_ is true between the first sharded delete and the last root
    // disappearing; deferredRoots_ holds a setRoots() that arrived meanwhile.
    bool clearing_ = false;
    bool subclassed_ = false;
    std::vector<TreeRow> deferredRoots_;
    bool hasDeferredRoots_ = false;
};

} // namespace Ksword::Ui
