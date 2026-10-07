#pragma once

// ============================================================
// AddressBookModel.h
// 作用：
// - 把 AddressBookStore（见同目录 AddressBookStore.h）的内容展示成一张表：
//   QAbstractTableModel，列固定为 类型图标｜地址｜值｜值类型｜备注｜模块+RVA｜目标。
// - 一切操作按 id，不按行号：行号会随 kind 过滤与外部排序代理（QSortFilterProxyModel）
//   变化，只有 id 是稳定的。每个单元格都带 IdRole，调用方（AddressBookPanel）必须先用它
//   换回 id，再找条目，绝不能直接用 QModelIndex::row() 当"第几条"——这正是旧搜索结果表
//   "排序后点中别的地址"那个缺陷的根因（见 target.md 1.8），本模型从结构上堵死它。
// - "值"列的文本不是本类读出来的：本类完全不碰目标进程/内核内存，连间接调用都没有。
//   它由外部在真正完成一次内存读取之后，调用 setValueText(id, text, state) 回填；本类
//   只负责存一份"外部喂给我的文本 + 状态"并显示。常驻值（Watch）要不要去读、多久读一次，
//   是上层轮询器的职责，不属于本模型。
// - "值类型"与"备注"两列是可编辑的（Qt::ItemIsEditable），提交时直接调用
//   AddressBookStore 的 setValueType/setNote——这两项都是地址簿自身的"记录"（存成什么类型、
//   写什么备注），不涉及对目标内存的任何读写，所以不违反上一条。"值"列本身不可通过
//   setData 提交：双击只能触发 AddressBookPanel 里一个专门的编辑器，Enter 提交时发
//   AddressBookPanel::valueEditRequested 信号，真正的内存写入留给上层经写事务完成。
// ============================================================

#include "../../../../shared/evidence/memory_workbench/MemoryAddressBook.h"

#include <QAbstractTableModel>
#include <QPointer>
#include <QString>

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace ks::ui
{
    class AddressBookStore;

    // AddressBookModel：地址簿的表格模型。
    class AddressBookModel final : public QAbstractTableModel
    {
        Q_OBJECT

    public:
        // Column：物理列顺序。A/B 两组列预设（见 AddressBookPanel）各显示其中 4 列，
        // 列本身的下标与含义不随预设切换而改变。
        enum Column : int
        {
            ColumnKindIcon = 0,  // 类型图标：搜索结果/书签/监视，仅图标 + 悬停提示，不显示文字。
            ColumnAddress,       // 地址：模块名+RVA（"client.dll+0x1a40"）或绝对地址（"0x7ff6...") 的统一展示文本。
            ColumnValue,         // 值：外部通过 setValueText 喂入的文本；本列不可经 setData 编辑。
            ColumnValueType,     // 值类型：ksword::memwb::ValueType 的中文名；可编辑。
            ColumnNote,          // 备注：用户自由文本；可编辑。
            ColumnModuleOffset,  // 模块+RVA：仅模块条目有值，绝对地址条目显示"—"；用于 B 组辨认"是否绑定模块"。
            ColumnTarget,        // 目标：entry.targetKey 经 setTargetDisplayName 映射后的展示名。
            ColumnCount          // 哨兵：列总数，不对应真实列。
        };

        // IdRole：该行条目的 id（quint64）。AddressBookPanel 的一切操作都必须先读它。
        static constexpr int IdRole = Qt::UserRole + 1;
        // KindRole：该行条目的原始 EntryKind（int），供排序代理与菜单判断用，不经过文本解析。
        static constexpr int KindRole = Qt::UserRole + 2;
        // ValueTypeRole：该行条目的原始 ValueType（int）。
        static constexpr int ValueTypeRole = Qt::UserRole + 3;
        // ValueStateRole：ColumnValue 单元格当前的 ValueState（int，见下方枚举）。
        static constexpr int ValueStateRole = Qt::UserRole + 4;
        // SortRole：供 QSortFilterProxyModel::setSortRole 使用的可比较排序键（修复 C15）。
        // "地址"列按十六进制文本排序会把 0x10000 排在 0x2000 前面（字符串序），这里按列
        // 分别给出数值/枚举序：地址列是零填充十六进制字符串（绝对地址与模块+RVA 分两组，
        // 组内再数值序）；类型图标列、值类型列按枚举底层整数；其余列退回与 DisplayRole
        // 相同的文本，不改变现有行为。
        static constexpr int SortRole = Qt::UserRole + 5;

        // ValueState：外部喂给"值"列文本时附带的状态，决定文字颜色与悬停提示。
        enum class ValueState : int
        {
            NotRead = 0,  // 从未读取（新加入的监视/书签条目初始状态）。
            Reading,      // 本轮正在读取中。
            Read,         // 已成功读取，text 是最新值。
            Unreadable,   // 读取失败（地址不可读/模块未加载等），text 通常为空或旧值。
            Stale         // 曾经读到过，但目标已变化（例如换了目标进程），text 是过期值。
        };

        // 构造：store 为只读访问的数据来源，生命周期必须长于本模型（通常是进程级单例）；
        // 本模型只连接它的信号读数据，不拥有它、也不负责创建或销毁它。
        explicit AddressBookModel(AddressBookStore* store, QObject* parent = nullptr);

        // ---- QAbstractTableModel 覆写 ----
        int rowCount(const QModelIndex& parent = QModelIndex()) const override;
        int columnCount(const QModelIndex& parent = QModelIndex()) const override;
        QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
        bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;
        Qt::ItemFlags flags(const QModelIndex& index) const override;
        QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

        // ---- kind 过滤 ----

        // setKindFilter：切换显示的 kind；std::nullopt 表示"全部"。切换会整表重建行号映射。
        void setKindFilter(std::optional<ksword::memwb::EntryKind> kind);
        std::optional<ksword::memwb::EntryKind> kindFilter() const;

        // KindCounts：四个计数，供分段按钮显示"全部(12)｜搜索(3)｜书签(5)｜监视(4)"。
        struct KindCounts
        {
            int all = 0;
            int search = 0;
            int bookmark = 0;
            int watch = 0;

            // 修复可疑点 #9：rebuildRows / onStoreEntryChanged 等多处都需要"这一轮计数是不是
            // 真的变了"，用默认生成的逐成员比较即可，不用每处都手写四个 && ——避免过滤 kind
            // 分段（只换行可见性，四个计数本就不变）或备注/值类型编辑（不碰计数）也触发
            // kindCountsChanged，进而触发 Panel 侧重建分段控件（这正是 C9 的根因）。
            friend bool operator==(const KindCounts&, const KindCounts&) = default;
        };
        KindCounts kindCounts() const;

        // ---- 值列喂入（外部完成一次真实读取之后回填）----

        // setValueText：id 不在当前簿里（已被删除）时忽略（修复 C14：避免悬挂值在号段复用
        // 后显示到新条目上）。文本与状态都与上一次完全相同时直接跳过，不写入也不发
        // dataChanged（修复 C1）：常驻监视通常按 1Hz 喂入，哪怕值没变也会重复调用本函数，
        // 若每次都发 dataChanged，正在编辑这一格的用户会被反复打断（Qt 对"正在编辑的索引"
        // 收到 dataChanged 会再调一次委托的 setEditorData）。为仍在可见范围的行发 dataChanged。
        void setValueText(std::uint64_t id, const QString& text, ValueState state);

        // valueState / valueText：供调用方查询某条目此刻展示的值与状态（例如轮询器判断要不要重读）。
        ValueState valueState(std::uint64_t id) const;
        QString valueText(std::uint64_t id) const;
        bool isPointerChain(std::uint64_t id) const;

        // ---- 行 <-> id ----

        // idAt：该索引对应的条目 id；索引非法或越界返回 0（0 是地址簿里永不出现的无效 id）。
        std::uint64_t idAt(const QModelIndex& index) const;
        // indexForId：该 id 当前在模型里的索引（受 kind 过滤影响，不在当前过滤范围内返回无效索引）。
        QModelIndex indexForId(std::uint64_t id, int column = ColumnKindIcon) const;

        // ---- 目标展示名 ----

        // ValueTypeDisplayName：ValueType 的中文展示名；本列（ColumnValueType）的显示文本与
        // AddressBookPanel 右键菜单"值类型▸"子菜单共用这一份文案，不在两处各写一遍。
        static QString ValueTypeDisplayName(ksword::memwb::ValueType valueType);

        // setTargetDisplayName：把 targetKey（AddressEntry::targetKey，通常是调用方内部用的
        // 不透明标识）映射成人看的名字（例如 "chrome.exe · PID 1234"）；传空 displayName
        // 等于删除该映射，ColumnTarget 回退显示原始 targetKey。
        void setTargetDisplayName(const QString& targetKey, const QString& displayName);

    signals:
        // kindCountsChanged：四个计数（kindCounts()）发生变化时发出——新增/删除一定会触发
        // （它们都经过整表重建）；Promote 在当前 kind 过滤恰好接纳新旧两种 kind 时（例如
        // 过滤为"全部"）不改变任何行的可见性，所以不会触发 QAbstractItemModel 自带的
        // modelReset，必须靠这个信号单独通知 Panel 去重读 kindCounts() 刷新分段按钮文字。
        void kindCountsChanged();

    private slots:
        void onStoreEntryAdded(quint64 id);
        void onStoreEntryRemoved(quint64 id);
        void onStoreEntryChanged(quint64 id);
        void onStoreReset();
        // onStoreBookReloaded：修复 D7 后半——Store::load() 整本替换成功时单独发出的
        // bookReloaded() 信号的槛；只清空值缓存（旧 id 可能已经对应完全不同的真实条目），
        // 行结构的重建仍交给随后到来的 reset()/onStoreReset()，这里不重复调用 rebuildRows。
        void onStoreBookReloaded();
        // onStoreDestroyed：修复 D9——Store 先于本模型销毁时（m_store 这个 QPointer 会
        // 自动变回 nullptr，但不会有任何模型信号通知视图），主动整表重建一次，让
        // rowCount/计数归零，不留着一堆指向已经不存在的 Store 的"幽灵行"。
        void onStoreDestroyed();

    private:
        // ValueCell：ColumnValue 单元格的外部喂入内容。
        struct ValueCell
        {
            QString text;
            ValueState state = ValueState::NotRead;
        };

        // rebuildRows：在 beginResetModel/endResetModel 之间调用 computeCountsAndRowIds，
        // 用于"行结构真的变了"的场景（新增/删除/kind 变化导致可见性翻转）。
        void rebuildRows();

        // computeCountsAndRowIds：从 store 拉取全部条目一次，重算 m_counts 并按当前过滤
        // 条件重建 m_rowIds（行号 -> id，保持插入顺序）。不发任何信号，调用方负责。
        void computeCountsAndRowIds();

        // recomputeCountsOnly：只重算四个计数，不touch m_rowIds——用于"行结构确定没变，
        // 但计数可能变了"的场景（例如 Promote 在 kind 过滤为'全部'时不改变任何行的可见性，
        // 但确实改变了搜索/书签/监视三个计数）。调用方应自行决定是否需要再发信号刷新分段
        // 按钮文字。
        void recomputeCountsOnly();

        // entryForRow：行号 -> 条目副本；行号越界返回 std::nullopt。
        std::optional<ksword::memwb::AddressEntry> entryForRow(int row) const;

        // rowForId：id -> 当前过滤结果里的行号；不在里面返回 -1。
        int rowForId(std::uint64_t id) const;

        // 修复可疑点 #3：改用 QPointer，Store 若先于本模型销毁（例如 WorkbenchShared 单例的
        // 析构顺序意外早于某个内嵌窗口里的面板），m_store 会自动变回 nullptr——现有各处
        // "m_store == nullptr" 的判空分支因此才真正有效；裸指针在对象销毁后仍是那个旧地址，
        // 同样的判空代码其实什么都挡不住（这正是裸指针悬挂最隐蔽的地方）。
        QPointer<AddressBookStore> m_store;                        // 只读引用，不持有生命周期。
        std::optional<ksword::memwb::EntryKind> m_kindFilter;      // 当前 kind 过滤，nullopt=全部。
        std::vector<std::uint64_t> m_rowIds;                       // 行号 -> id，按插入顺序。
        KindCounts m_counts;                                       // 最近一次 rebuildRows 算出的计数。
        std::map<std::uint64_t, ValueCell> m_valueCells;           // id -> 外部喂入的值文本/状态。
        std::map<QString, QString> m_targetNames;                  // targetKey -> 展示名。
    };
}
