#pragma once

// ============================================================
// AddressBookPanel.h
// 作用：
// - 工作台侧栏里的"地址簿"内容控件：kind 分段（全部｜搜索｜书签｜监视，带计数）、
//   A/B 列组按钮、QTableView+QSortFilterProxyModel 展示 AddressBookModel、右键菜单
//   （见同目录 AddressBookPanel.Menu.cpp）与键盘快捷键。
// - 本类**不做任何事**：所有会改变地址簿内容、跳转目标或触发内存写入的动作都只发信号
//   （jumpRequested / openDisassemblyRequested / promoteRequested / valueEditRequested /
//   removeRequested / clearSearchResultsRequested），由上层（MemoryWorkbenchView 等）决定
//   具体怎么处理、要不要确认、要不要写审计。唯二的例外是"备注"与"值类型"两列的编辑：
//   它们走 Qt 标准的表格编辑流程，提交时由 AddressBookModel 直接落到 AddressBookStore
//   （这两项是地址簿自身的记录，不涉及对目标内存的任何读写，不受"不做任何事"约束）。
// - **一切操作按 id**：双击、右键、Del、F2、Ctrl+C 处理的永远是
//   proxy->mapToSource(viewIndex) 换出来的源模型索引上的 id，不信任 QSortFilterProxyModel
//   或 QTableView 给出的行号——这是 target.md 1.8 那个"排序后点错地址"旧缺陷的正面回归点。
// - 窄窗口可用：工具栏用 FlowLayout（UI/FlowLayout.h），放不下时自动换到下一行而不是把
//   分段按钮的文字压没。
// ============================================================

#include "AddressBookModel.h"

#include "../../../../shared/evidence/memory_workbench/MemoryAddressBook.h"

#include <QList>
#include <QMetaType>
#include <QModelIndex>
#include <QPointer>
#include <QString>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QWidget>

#include <cstdint>
#include <vector>

class QEvent;

class QAbstractItemModel;
class QHBoxLayout;
class QKeyEvent;
class QPoint;
class QPushButton;
class QSortFilterProxyModel;
class QStyleOptionViewItem;
class QVBoxLayout;

namespace ks::ui
{
    class FlowLayout;
    class HexViewMessageLabel;
    class HexViewSegmented;
    class AddressBookPanel;

    namespace detail
    {
        // AddressBookTableView：仅供 AddressBookPanel 内部使用的 QTableView 子类。
        // 作用：把 Del / F2 / Enter / Ctrl+C 四个快捷键转发给宿主 AddressBookPanel 的对应
        // 处理函数；不新增信号/槛，因此不含 Q_OBJECT，用持有宿主指针的普通虚函数转发即可，
        // 省一份 moc 产物。
        class AddressBookTableView final : public QTableView
        {
        public:
            explicit AddressBookTableView(AddressBookPanel* owner, QWidget* parent = nullptr);

        protected:
            void keyPressEvent(QKeyEvent* event) override;

        private:
            AddressBookPanel* m_owner;  // 宿主面板，生命周期上严格是本控件的父级，不会悬空。
        };

        // ValueColumnDelegate："值"列专用的编辑委托。
        // 作用：双击/F2/Enter 打开一个预填当前显示文本的单行编辑器；提交（Enter/失焦）时
        // **不调用** model->setData（AddressBookModel 对"值"列的 setData 恒返回 false），
        // 而是直接回调宿主的 commitValueEdit，由宿主发出 valueEditRequested 信号——真正的
        // 写入永远要经过上层的写事务，本委托只负责"问用户要哪一个新文本"。
        // 不新增信号/槛，不含 Q_OBJECT。
        class ValueColumnDelegate final : public QStyledItemDelegate
        {
        public:
            explicit ValueColumnDelegate(AddressBookPanel* owner, QObject* parent = nullptr);

            QWidget* createEditor(
                QWidget* parent,
                const QStyleOptionViewItem& option,
                const QModelIndex& index) const override;
            void setEditorData(QWidget* editor, const QModelIndex& index) const override;
            void setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const override;

        private:
            AddressBookPanel* m_owner;
        };
    }

    // AddressBookPanel：地址簿侧栏内容控件。
    class AddressBookPanel final : public QWidget
    {
        Q_OBJECT

    public:
        // ColumnGroup：A/B 列组预设，与 Custom（用户手动改过列显隐）三态，
        // 命名与行为照抄仓库既有范式（MiscDock/ClipboardGuard/ClipboardGuardPage 的
        // ClipboardGuardColumnLayout），供外层把当前预设持久化到 QSettings。
        enum class ColumnGroup : int
        {
            PresetA = 0,  // 类型图标·地址·值·备注——日常浏览。
            PresetB = 1,  // 地址·值类型·模块+RVA·目标——诊断"这条是怎么来的"。
            Custom = 2    // 用户在表头右键菜单里手动改过列显隐之后。
        };

        // CopyField：previewCopyText 支持的四种复制目标，对应右键菜单的四个"复制…"动作。
        enum class CopyField : int
        {
            Address = 0,
            ModuleOffset,
            Value,
            Row
        };

        // 构造：model 为只读访问的数据来源（通常是进程级共享的单例，内嵌窗口场景下也共享
        // 同一个 model），生命周期必须长于本面板；本面板不拥有它。
        explicit AddressBookPanel(AddressBookModel* model, QWidget* parent = nullptr);

        // model：当前绑定的数据模型（供外层查询计数、喂入值文本等，本面板本身不转发这些调用）。
        AddressBookModel* model() const;

        // ---- 列组预设（供外层持久化 addrBook/columnGroup、addrBook/hiddenColumns）----
        void applyColumnGroup(ColumnGroup group);
        ColumnGroup columnGroup() const;
        // hiddenColumns / setHiddenColumns：当前隐藏的物理列下标集合；setHiddenColumns 会把
        // columnGroup() 置为 Custom（即便结果恰好与某个预设重合——用户显式操作过表头就不再
        // 认为是"预设"，与 ClipboardGuardPage 的既有规则一致）。
        QList<int> hiddenColumns() const;
        void setHiddenColumns(const QList<int>& hiddenColumns);

        // ---- kind 过滤（供外层持久化 addrBook/kind；-1=全部，0=搜索，1=书签，2=监视，
        // 与 ksword::memwb::EntryKind 的底层整数值保持一致，-1 是本面板自己定义的"全部"编码）----
        void setKindFilterIndex(int index);
        int kindFilterIndex() const;

        // previewCopyText：当前行（view->currentIndex()）按 field 应该复制的文本，不写入任何
        // 剪贴板——真正的剪贴板写入只发生在菜单动作与 Ctrl+C 处理函数里，这个方法让测试能够
        // 校验"复制的内容对不对"而不必触碰真实系统剪贴板（仓库规定自动化验证禁止写入真实
        // 剪贴板，见 .claude/memory/ksword-ui-architecture.md 角落通知卡片一节）。
        // 没有当前行时返回空串。
        QString previewCopyText(CopyField field) const;

        // selectedIds：当前选中的全部行对应的 id（已按 id 换过，不含重复）。
        std::vector<std::uint64_t> selectedIds() const;

        // showLoadFailure：显示/隐藏顶部的"载入失败：第 N 行：原因"横幅。
        // 本面板不持有 AddressBookStore、不会自己调用 load()；该横幅由上层在
        // AddressBookStore::load() 返回失败后，把 lastLoadErrorText() 传进来显示——
        // 这样"载入失败不覆盖原文件"的判断仍完全留在 Store 一侧，本面板只管展示文案。
        // 传空串隐藏横幅（例如下一次 load() 成功后）。
        // backupPath（修复 C4）：Store 把坏文件改名备份后的路径；非空时额外追加一句告知
        // 用户去哪里找这份备份，留空则只显示 message 本身（兼容旧调用点）。
        void showLoadFailure(const QString& message, const QString& backupPath = QString());

    protected:
        // changeEvent：修复 D11（A/B 按钮选中/悬停态仍是调用瞬间固化的字面色，主程序里靠
        // 全局 ThemeColorRemap 兜底重写存量样式表，但本面板若被用在没有接这条兜底链路的
        // 宿主里就跟不上深浅主题切换；夹具里没有 RemapStaleThemeColors，必须自己处理）与
        // D4（运行期切换语言不刷新自绘的 kind 分段控件文字——HexViewSegmented 不是
        // QLabel/QAbstractButton，LanguageManager 的运行期遍历够不到它）：分别监听
        // ApplicationPaletteChange/PaletteChange 与 LanguageChange 事件，重新生成 A/B
        // 按钮样式表、强制刷新分段文字。
        void changeEvent(QEvent* event) override;

    signals:
        // jumpRequested：跳转到该条目（右键"跳转"、双击地址/类型图标列、Enter）。
        void jumpRequested(quint64 id);
        // openDisassemblyRequested：在反汇编子页打开该条目。
        void openDisassemblyRequested(quint64 id);
        void pointerChainCreateRequested();
        void pointerChainEditRequested(quint64 id);
        void pointerChainResolveRequested(quint64 id);
        void pointerChainCancelRequested();
        // promoteRequested：把条目升级为书签或监视；newKind 恒为 Bookmark 或 Watch
        // （本面板的菜单从不提供"降回搜索"这个选项，禁止降级是结构性的，不是运行期判断）。
        void promoteRequested(quint64 id, ksword::memwb::EntryKind newKind);
        // valueEditRequested：双击"值"列并按 Enter 提交后发出；真正的内存写入由上层经
        // MemoryWriteTransaction 完成，本面板不知道写成功与否。
        void valueEditRequested(quint64 id, ksword::memwb::ValueType valueType, QString text);
        // removeRequested：删除选中的全部条目（右键"删除"或按 Del）。
        void removeRequested(QList<quint64> ids);
        // clearSearchResultsRequested：清空全部"搜索"kind 的条目（右键"清空搜索结果"）。
        void clearSearchResultsRequested();

    private:
        friend class detail::AddressBookTableView;
        friend class detail::ValueColumnDelegate;

        // ---- 供 detail::AddressBookTableView 转发快捷键调用（friend）----
        void handleDeleteKey();
        void handleF2Key();
        void handleEnterKey();
        void handleCopyShortcut();

        // ---- 供 detail::ValueColumnDelegate 调用（friend）----
        // editorInitialText：index（源模型索引）对应条目当前应显示在编辑器里的初始文本。
        QString editorInitialText(const QModelIndex& sourceIndex) const;
        // commitValueEdit：编辑器提交时调用，发出 valueEditRequested，不touch 任何模型/Store。
        void commitValueEdit(const QModelIndex& sourceIndex, const QString& text);

        // ---- 构建 ----
        void buildUi();
        void buildToolbar(QVBoxLayout* rootLayout);
        void buildTable(QVBoxLayout* rootLayout);
        void connectSignals();

        // ---- 列组 ----
        void applyColumnVisibility(const QList<int>& hiddenColumns);  // 只改视图列显隐，不改 m_columnGroup。
        void updateColumnGroupButtons();                               // A/B 按钮着色随 m_columnGroup 刷新。

        // ---- kind 分段 ----
        void refreshKindSegmentLabelsAndCounts();  // 用 model()->kindCounts() 重写四段文字。
        void onKindSegmentChanged(int index);

        // ---- 行为实现（供菜单与快捷键共用，定义在 AddressBookPanel.cpp）----
        void jumpCurrentRow();
        void openDisassemblyCurrentRow();
        void editPointerChainById(std::uint64_t id);
        void resolvePointerChainById(std::uint64_t id);
        void promoteSelection(ksword::memwb::EntryKind newKind);
        void removeSelection();
        void editNoteCurrentRow();
        void copyCurrentRowField(CopyField field);  // 真正写入系统剪贴板，previewCopyText 之后再调它。
        void setValueTypeForSelection(ksword::memwb::ValueType valueType);

        // targetRowId：修复 C7 的核心——统一"这次操作该落在哪一条"的判据，跳转/反汇编/
        // 编辑备注/单字段复制都改call它，不再各自读 sourceIndexForCurrent() 导致"选区是 A、
        // 当前格停在 B"时操作落到 B 上。规则：选区恰好 1 条时就是它；否则（多选或无选）
        // 退回当前格，但当前格必须落在选区内（无选区时例外，退化为纯"当前格"），不满足
        // 返回 0。
        std::uint64_t targetRowId() const;

        // rowTextForId：id 对应条目当前可见列拼接成的一行文本（Tab 分隔，跳过类型图标列
        // 与隐藏列）；previewCopyText(Row) 与 Ctrl+C 多选复制共用，保证格式一致。
        QString rowTextForId(std::uint64_t id) const;

        // sourceIndexForCurrent / sourceIndexForProxy：把 view 当前/给定索引换成源模型索引；
        // 无当前行或索引失效时返回无效 QModelIndex。
        QModelIndex sourceIndexForCurrent() const;
        QModelIndex sourceIndexForProxy(const QModelIndex& proxyIndex) const;

        // ---- 右键菜单（定义在 AddressBookPanel.Menu.cpp）----
        void showRowContextMenu(const QPoint& viewportPos);
        void showHeaderContextMenu(const QPoint& headerPos);

        // ---- D6：整表 reset 前后按 id 保存/恢复选区与当前格 ----
        // captureSelectionForResetRestore：连在 model 的 modelAboutToBeReset 上，这时行
        // 结构还没真的变，selectedIds()/当前格还能正常读出来，先按 id 记下来。
        void captureSelectionForResetRestore();
        // restoreSelectionAfterReset：连在 model 的 modelReset 上，重建后按 id 换算回新的
        // 行号，能找到就重新选中/置为当前格；真的不在了（确实被删掉）就跳过，不是缺陷。
        void restoreSelectionAfterReset();

        // 修复可疑点 #3：改用 QPointer。本面板的架构前提是"model 生命周期必须长于本面板"
        // （见本文件头注释），但万一这个前提在某次重构中被打破（例如 WorkbenchShared 单例
        // 销毁顺序意外早于某个内嵌窗口里的面板），裸指针会悬挂成一个看着非空、实际早已
        // 失效的地址，而 QPointer 会在目标被销毁时自动变回 nullptr，让各入口的判空真正有效。
        QPointer<AddressBookModel> m_model;                   // 只读引用，不拥有生命周期。
        QSortFilterProxyModel* m_proxy = nullptr;             // 排序代理，拥有（本面板的 child）。
        detail::AddressBookTableView* m_view = nullptr;       // 表格视图，拥有（本面板的 child）。
        detail::ValueColumnDelegate* m_valueDelegate = nullptr; // "值"列编辑委托，拥有（parent=this）。
        FlowLayout* m_toolbarFlow = nullptr;                   // 工具栏布局（放不下自动换行）。
        HexViewSegmented* m_kindSegment = nullptr;            // 全部｜搜索｜书签｜监视，带计数，懒创建。
        QWidget* m_columnButtonsHost = nullptr;               // A/B 按钮的共同宿主（修复 C11，见下）。
        QPushButton* m_columnAButton = nullptr;
        QPushButton* m_columnBButton = nullptr;
        QPushButton* m_pointerChainAddButton = nullptr;
        QPushButton* m_pointerChainCancelButton = nullptr;
        HexViewMessageLabel* m_loadFailureLabel = nullptr;    // "第 N 行：原因"，默认隐藏。
        ColumnGroup m_columnGroup = ColumnGroup::PresetA;
        bool m_applyingColumnGroup = false;                   // 应用预设期间抑制表头菜单把状态打回 Custom。
        // 修复 D6：整表 reset 期间临时记一下"重建之前的选区/当前格"，重建完成后按 id 尝试
        // 恢复；两者只在 captureSelectionForResetRestore -> restoreSelectionAfterReset 这
        // 一小段时间内有意义，恢复完/没恢复成都会清空。
        std::vector<std::uint64_t> m_pendingSelectionRestore;
        std::uint64_t m_pendingCurrentRestore = 0;
    };
}

// Q_DECLARE_METATYPE：promoteRequested/valueEditRequested 把 Core 的这两个普通枚举当
// 信号参数传出去；跨线程排队连接与 QSignalSpy 一类"把参数存进 QVariant"的消费方都要求
// 参数类型是已注册的 Qt 元类型，裸枚举默认不是。写法与 UI/HvmControl.h 对
// ksword::hvm::HvmWatchEntry 的做法一致，放在头文件末尾、命名空间之外。
Q_DECLARE_METATYPE(ksword::memwb::EntryKind)
Q_DECLARE_METATYPE(ksword::memwb::ValueType)
