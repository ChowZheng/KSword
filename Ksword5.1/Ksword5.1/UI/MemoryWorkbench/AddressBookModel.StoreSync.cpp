// AddressBookModel.StoreSync.cpp
// 作用：AddressBookModel 对 AddressBookStore 四个变更信号（entryAdded/entryRemoved/
// entryChanged/reset，以及本轮新增的 bookReloaded/Store 销毁）的响应——行结构增删改、
// 计数增量/全量重算、值缓存清理。从 AddressBookModel.cpp 拆出来（本轮修复后原文件超过
// AGENTS.md 的单文件行数上限），按"列数据格式化/绘制"与"响应 Store 变更同步内部状态"两类
// 职责拆开，声明仍在 AddressBookModel.h。

#include "AddressBookModel.h"

#include "AddressBookStore.h"

#include <algorithm>
#include <set>

namespace ks::ui
{
    void AddressBookModel::onStoreEntryAdded(const quint64 id)
    {
        // 修复 C8 / C16：单条新增不再整表 beginResetModel/endResetModel——那会让
        // QSortFilterProxyModel 整个重新排序、QTableView 整个重新布局、还会让 Panel 把
        // kind 分段控件删了重建（即便计数没变，详见下面的 before==after 判断），地址簿
        // 设计上限约一万条、连续多次单条 add 在该规模下会被这整套重排代价拖成 O(n²)
        // 的 UI 冻结；全 reset 也会把视图的选区与当前格清空（C16）。改成增量 insert：
        // 计数照常整表重算（这部分仍是 O(n)，但便宜——不触发任何 Qt 模型/视图机制），
        // 新行是否可见则单独判断，只在确实可见时才 beginInsertRows/endInsertRows。
        const std::optional<ksword::memwb::AddressEntry> entry = m_store != nullptr ? m_store->find(id) : std::nullopt;
        if (!entry.has_value())
        {
            return; // 理论上不会发生：这个信号只在 Store::add 成功之后才会发出。
        }
        // 修复 D12（性能，部分）：新增这一条的 kind 已经知道，不需要为了重算四个计数再向
        // store 要一次全量列表——recomputeCountsOnly 本身是 O(n) 的条目拷贝（含若干
        // std::string），逐条 add() 的场景下这是性能数据里唯一还没补齐的一环（addMany/
        // remove/changed 三条路径分别有各自的理由保留 O(n)，见对应函数的注释）。这里直接
        // 对命中的那个计数加一，省掉这次全量扫描。
        const KindCounts before = m_counts;
        ++m_counts.all;
        switch (entry->kind)
        {
        case ksword::memwb::EntryKind::Search: ++m_counts.search; break;
        case ksword::memwb::EntryKind::Bookmark: ++m_counts.bookmark; break;
        case ksword::memwb::EntryKind::Watch: ++m_counts.watch; break;
        }
        const bool visible = !m_kindFilter.has_value() || entry->kind == *m_kindFilter;
        if (visible)
        {
            // Store 的插入顺序决定新条目必然排在 list() 的最后；它若通过了当前过滤，
            // 也必然排在 m_rowIds 的最后——这与整表重建（computeCountsAndRowIds 顺序遍历）
            // 会得到的位置完全一致。
            const int newRow = static_cast<int>(m_rowIds.size());
            beginInsertRows(QModelIndex(), newRow, newRow);
            m_rowIds.push_back(id);
            endInsertRows();
        }
        if (!(before == m_counts))
        {
            emit kindCountsChanged();
        }
    }

    void AddressBookModel::onStoreEntryRemoved(const quint64 id)
    {
        // 同上，改成增量 remove（修复 C8 / C16）。此时 Store 里已经没有这条了，
        // recomputeCountsOnly 据当前内容重算天然正确。
        // D12（性能，保留 O(n)，不展开成增量）：与 onStoreEntryAdded 不同，这里拿不到
        // 被删条目的 kind 了——Store::remove 先改动了内部存储才发这个信号，id 已经查不到。
        // 要做成增量，需要在 Model 侧额外维护一份"id -> kind"的影子缓存，改动面会扩大到
        // 和 D6/D7/D9 的改动互相纠缠（例如 bookReloaded/onStoreDestroyed 也要同步清理这份
        // 新缓存），风险超过这一项低优先级性能问题本身的收益，本轮保留 O(n)。
        const KindCounts before = m_counts;
        const int row = rowForId(id);
        recomputeCountsOnly();
        if (row >= 0)
        {
            beginRemoveRows(QModelIndex(), row, row);
            m_rowIds.erase(m_rowIds.begin() + row);
            endRemoveRows();
        }
        // 修复 C14：精确清掉这一个 id 的值缓存，不等下一次整表重建的"存活集合"清理再收尾
        // ——删除之后立刻清，才能保证紧接着的 setValueText(刚删的 id, ...) 一定被忽略。
        m_valueCells.erase(id);
        if (!(before == m_counts))
        {
            emit kindCountsChanged();
        }
    }

    void AddressBookModel::onStoreEntryChanged(const quint64 id)
    {
        // 不知道这次变更具体改了哪个字段：备注/值类型编辑不影响 kind 与可见性，
        // 但 Promote 会改 kind——先按"该 id 现在是否应该可见"判断要不要整表重建，
        // 计数则无论哪种情况都要重新算一遍（Promote 在过滤为"全部"时不改变任何行的可见性，
        // 但确实改变了搜索/书签/监视三个计数）。
        const std::optional<ksword::memwb::AddressEntry> entry =
            m_store != nullptr ? m_store->find(id) : std::nullopt;
        const int row = rowForId(id);
        const bool shouldBeVisible = entry.has_value()
            && (!m_kindFilter.has_value() || entry->kind == *m_kindFilter);

        if (row >= 0 && shouldBeVisible)
        {
            // 行位置与可见性都没变：只有字段内容变了（备注/值类型，或者 Promote 恰好没
            // 改变可见性）。精确刷新这一行，不动行结构与视图的当前选中状态。
            // 修复可疑点 #9：计数没有真的变化时（绝大多数备注/值类型编辑都是这样）不再
            // 发 kindCountsChanged——Panel 那一侧收到这个信号会把 kind 分段控件删了重建，
            // 编辑一下备注就把分段控件拆了是不必要的开销，也是 C9 的部分成因。
            const KindCounts before = m_counts;
            recomputeCountsOnly();
            if (!(before == m_counts))
            {
                emit kindCountsChanged();
            }
            emit dataChanged(index(row, 0), index(row, static_cast<int>(ColumnCount) - 1));
            return;
        }
        if (row < 0 && !shouldBeVisible)
        {
            // 修复 D6（第一部分）：这条本来就不在当前过滤范围内（row<0），这次变更也没有
            // 让它变得可见（shouldBeVisible 仍是 false）——没有任何一行的可见性真的翻转，
            // 不需要整表 beginResetModel/endResetModel（那会把用户在当前可见分段里的选区
            // /当前格一起清空：对一条被"书签"过滤隐藏的监视条目 setNote，原来的实现会走到
            // 下面的 rebuildRows()，把用户选中的别的书签行的选区白白清没了）。只需要重算
            // 计数——备注/值类型编辑不改计数，但 Promote 可能改变"搜索/书签/监视"三个计数
            // 的分布，即便目标 kind 仍然不在当前过滤范围内（例如把一条被过滤掉的"监视"
            // 提升为"书签"：监视计数要减、书签计数要加，但两种 kind 若都不是当前过滤的
            // 那一种，可见行集合确实没变）。
            const KindCounts before = m_counts;
            recomputeCountsOnly();
            if (!(before == m_counts))
            {
                emit kindCountsChanged();
            }
            return;
        }
        // 其余情况：该 id 的可见性真的翻转了（kind 变化使它从过滤范围外进入可见，或从
        // 可见变为过滤范围外），或条目已经不存在——行数会变，必须整表重建（里面会一并
        // 重算计数）。
        rebuildRows();
    }

    void AddressBookModel::onStoreReset()
    {
        rebuildRows();
    }

    void AddressBookModel::onStoreBookReloaded()
    {
        // 修复 D7 后半：load() 把整本簿替换掉之后，旧的值缓存不能只靠"id 是否仍存在"来
        // 判断要不要清——文件里完全可能出现一个与内存里旧条目相同的 id（号段被复用，见
        // MemoryAddressBook.h 关于下一个 id 的注释），这种情况下 id 仍"存在"，但对应的已
        // 经是另一条完全不同的真实条目，继续显示旧值就是把一条记录的历史值显示到了另一条
        // 记录上。这里整个清空，紧随其后到来的 reset() 信号会负责重建行结构（见
        // onStoreReset），两者各管一件事，顺序由 Store::load() 的发信号顺序保证。
        m_valueCells.clear();
    }

    void AddressBookModel::onStoreDestroyed()
    {
        // 修复 D9：此刻 m_store 已经是 nullptr（见构造函数里这个连接旁的注释），整表重建
        // 一次，让 rowCount()/kindCounts() 都归零，不再留着指向已销毁 Store 的"幽灵行"——
        // 否则 Enter/右键菜单会一直对着这些行发信号，直到下一次毫不相关的改动才意外清空。
        rebuildRows();
    }

    void AddressBookModel::rebuildRows()
    {
        // 修复可疑点 #9（并顺带是 C9 的根因之一）：行结构（beginResetModel/endResetModel）
        // 在 kind 过滤切换时确实要重建——换一个分段看到的行集合本来就不同；但"计数变了没
        // 有"是另一件事：切换过滤不会改变四个计数（全局计数与当前显示哪些行无关），若仍然
        // 无条件发 kindCountsChanged，Panel 会把 kind 分段控件删了重建——点一下分段按钮，
        // 自己把自己删了重建，这正是审核报告里"在自身 mousePressEvent 里 delete 自己"的
        // 触发链路。只在计数真的变化时才发。
        const KindCounts before = m_counts;
        beginResetModel();
        computeCountsAndRowIds();
        endResetModel();
        if (!(before == m_counts))
        {
            emit kindCountsChanged();
        }
    }

    void AddressBookModel::computeCountsAndRowIds()
    {
        m_rowIds.clear();
        m_counts = KindCounts{};
        if (m_store == nullptr)
        {
            m_valueCells.clear();  // 没有数据源：值缓存同样失去意义，清空避免悬挂增长。
            return;
        }
        // existingIds：本轮全量存在的 id 集合，用于下面清理已经失效的值缓存（修复 C14）。
        std::set<std::uint64_t> existingIds;
        // 只拉一次全量列表：既用来算四个计数，也用来按当前过滤条件建行号映射，
        // 避免对 store 的 list() 调用两次（一次为了过滤、一次为了计数）。
        for (const ksword::memwb::AddressEntry& entry : m_store->list())
        {
            ++m_counts.all;
            switch (entry.kind)
            {
            case ksword::memwb::EntryKind::Search: ++m_counts.search; break;
            case ksword::memwb::EntryKind::Bookmark: ++m_counts.bookmark; break;
            case ksword::memwb::EntryKind::Watch: ++m_counts.watch; break;
            }
            existingIds.insert(entry.id);
            if (!m_kindFilter.has_value() || entry.kind == *m_kindFilter)
            {
                m_rowIds.push_back(entry.id);
            }
        }
        // 清理指向已经不存在的条目的值缓存：这一步覆盖"整表重建"路径（load() 成功、
        // removeMany、clearSearchResults、addMany）——单条删除已经在 onStoreEntryRemoved
        // 里精确处理过，这里是批量路径的收尾，避免号段被复用后旧值显示到新条目上。
        std::erase_if(m_valueCells, [&existingIds](const auto& keyValue) {
            return existingIds.find(keyValue.first) == existingIds.end();
        });
    }

    void AddressBookModel::recomputeCountsOnly()
    {
        m_counts = KindCounts{};
        if (m_store == nullptr)
        {
            return;
        }
        for (const ksword::memwb::AddressEntry& entry : m_store->list())
        {
            ++m_counts.all;
            switch (entry.kind)
            {
            case ksword::memwb::EntryKind::Search: ++m_counts.search; break;
            case ksword::memwb::EntryKind::Bookmark: ++m_counts.bookmark; break;
            case ksword::memwb::EntryKind::Watch: ++m_counts.watch; break;
            }
        }
    }

    std::optional<ksword::memwb::AddressEntry> AddressBookModel::entryForRow(const int row) const
    {
        if (row < 0 || static_cast<std::size_t>(row) >= m_rowIds.size() || m_store == nullptr)
        {
            return std::nullopt;
        }
        return m_store->find(m_rowIds[static_cast<std::size_t>(row)]);
    }

    int AddressBookModel::rowForId(const std::uint64_t id) const
    {
        const auto it = std::find(m_rowIds.begin(), m_rowIds.end(), id);
        if (it == m_rowIds.end())
        {
            return -1;
        }
        return static_cast<int>(std::distance(m_rowIds.begin(), it));
    }
}
