// wpE_tests.Model.cpp
// 作用：AddressBookModel 的离屏验证——排序后按 id 不按行号、kind 过滤与计数
// （含"全部"过滤下 Promote 不改变行结构但要改变计数的回归点）、值列喂入与着色、
// 值类型/备注可编辑、"值"列 setData 恒拒绝、表头文案。

#include "wpE_common.h"

#include "../../../Ksword5.1/Ksword5.1/Internationalization/LanguageManager.h"

#include <QSignalSpy>
#include <QSortFilterProxyModel>

namespace wpe_test
{
    namespace
    {
        using ks::ui::AddressBookModel;
        using ks::ui::AddressBookStore;
        using ksword::memwb::EntryKind;
        using ksword::memwb::ValueType;

        // TestSortThenOperateById：QSortFilterProxyModel 对"地址"列升序排序后，按代理行号
        // 读出的 id 必须与排序前按 id 直接查到的条目一致——这是 target.md 1.8 那个旧缺陷
        // （排序后点中别的地址）的结构性回归点：AddressBookModel::idAt 必须换过 id 才能用，
        // 不能假设行号等于插入顺序。
        void TestSortThenOperateById()
        {
            AddressBookStore store(QString{});  // 空路径＝纯内存模式，不落盘。
            AddressBookModel model(&store);
            const std::uint64_t idLow = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "proc-a", 0x9000));
            const std::uint64_t idHigh = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "proc-a", 0x1000));

            QSortFilterProxyModel proxy;
            proxy.setSourceModel(&model);
            proxy.sort(AddressBookModel::ColumnAddress, Qt::AscendingOrder);

            // 排序后第 0 行的地址应是 0x1000（更小），对应 idHigh（变量名按"插入顺序晚"命名，
            // 与地址大小无关，这里故意让插入顺序与地址大小相反来暴露"按行号"的错误实现）。
            const QModelIndex proxyRow0 = proxy.index(0, AddressBookModel::ColumnKindIcon);
            const QModelIndex sourceRow0 = proxy.mapToSource(proxyRow0);
            const std::uint64_t idAtRow0 = model.idAt(sourceRow0);
            WPE_CHECK(idAtRow0 == idHigh);
            WPE_CHECK(idAtRow0 != idLow);

            // 反过来：用 id 查索引（indexForId），再核对它在代理模型里排到了第几行。
            const QModelIndex sourceForLow = model.indexForId(idLow, AddressBookModel::ColumnKindIcon);
            const QModelIndex proxyForLow = proxy.mapFromSource(sourceForLow);
            WPE_CHECK(proxyForLow.row() == 1);
        }

        // TestKindFilterAndCounts：kind 过滤只影响行数，不影响计数（计数永远是全量）。
        void TestKindFilterAndCounts()
        {
            AddressBookStore store(QString{});
            AddressBookModel model(&store);
            store.add(MakeAbsoluteEntry(EntryKind::Search, "p", 1));
            store.add(MakeAbsoluteEntry(EntryKind::Search, "p", 2));
            store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 3));
            store.add(MakeAbsoluteEntry(EntryKind::Watch, "p", 4));

            const AddressBookModel::KindCounts counts = model.kindCounts();
            WPE_CHECK(counts.all == 4);
            WPE_CHECK(counts.search == 2);
            WPE_CHECK(counts.bookmark == 1);
            WPE_CHECK(counts.watch == 1);

            WPE_CHECK(model.rowCount() == 4);
            model.setKindFilter(EntryKind::Search);
            WPE_CHECK(model.rowCount() == 2);
            model.setKindFilter(EntryKind::Watch);
            WPE_CHECK(model.rowCount() == 1);
            model.setKindFilter(std::nullopt);
            WPE_CHECK(model.rowCount() == 4);
        }

        // TestRedundantKindFilterIsNoOp：用同一个过滤值重复调用 setKindFilter 不应该
        // 触发第二次整表重建——重复的 modelReset 会把视图的选中状态、滚动位置无谓地打断。
        // 变异 #4：删掉 setKindFilter 里 "m_kindFilter == kind 就提前返回" 这一判断，
        // 这里的 resetCount 会从 1 变成 2。
        void TestRedundantKindFilterIsNoOp()
        {
            AddressBookStore store(QString{});
            AddressBookModel model(&store);
            store.add(MakeAbsoluteEntry(EntryKind::Search, "p", 1));

            int resetCount = 0;
            QObject::connect(&model, &QAbstractItemModel::modelReset, [&resetCount]() { ++resetCount; });

            model.setKindFilter(EntryKind::Search);  // 第一次：全部 -> 搜索，真正的变化。
            WPE_CHECK(resetCount == 1);
            model.setKindFilter(EntryKind::Search);  // 第二次：还是搜索，应该是空操作。
            WPE_CHECK(resetCount == 1);
            model.setKindFilter(EntryKind::Bookmark);  // 第三次：真正又变了一次。
            WPE_CHECK(resetCount == 2);
        }

        // TestKindFilterSwitchDoesNotEmitCountsChanged（修复可疑点 #9 / C9 根因）：纯粹
        // 切换 kind 过滤不会改变四个计数（全局计数与当前显示哪些行无关），rebuildRows
        // 因此不应该再发 kindCountsChanged——旧写法无条件发信号，Panel 那一侧的分段控件
        // 会被删了重建，审核报告指出这正好发生在分段控件自己的事件处理链路里（C9）。
        // 这是 Model 直接测，不经过 Panel 的分段控件重建判断——那一层还有自己的
        // "文字没变就不重建"兜底，会把 Model 这里的回归悄悄盖住，必须单独测才能钉住它。
        void TestKindFilterSwitchDoesNotEmitCountsChanged()
        {
            AddressBookStore store(QString{});
            AddressBookModel model(&store);
            store.add(MakeAbsoluteEntry(EntryKind::Search, "p", 1));
            store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 2));
            store.add(MakeAbsoluteEntry(EntryKind::Watch, "p", 3));

            int kindCountsSignalCount = 0;
            QObject::connect(&model, &AddressBookModel::kindCountsChanged, [&kindCountsSignalCount]() { ++kindCountsSignalCount; });
            model.setKindFilter(EntryKind::Search);
            model.setKindFilter(EntryKind::Bookmark);
            model.setKindFilter(EntryKind::Watch);
            model.setKindFilter(std::nullopt);
            WPE_CHECK_NOTE(
                kindCountsSignalCount == 0,
                QStringLiteral("纯粹切换过滤不该发 kindCountsChanged，实际发了 %1 次").arg(kindCountsSignalCount));
        }

        // TestPromoteUnderAllFilterUpdatesCounts：这是对 AddressBookModel::onStoreEntryChanged
        // 的回归测试——过滤为"全部"时 Promote 不改变任何行的可见性（旧写法会因此漏掉计数
        // 刷新）。变异 #2：把 onStoreEntryChanged 里 "row>=0 && shouldBeVisible" 的
        // shouldBeVisible 判断去掉，这里会在 counts.search 上立刻炸。
        void TestPromoteUnderAllFilterUpdatesCounts()
        {
            AddressBookStore store(QString{});
            AddressBookModel model(&store);
            const std::uint64_t id = store.add(MakeAbsoluteEntry(EntryKind::Search, "p", 1));
            WPE_CHECK(model.kindCounts().search == 1);
            WPE_CHECK(model.kindCounts().bookmark == 0);

            int kindCountsSignalCount = 0;
            QObject::connect(&model, &AddressBookModel::kindCountsChanged, [&kindCountsSignalCount]() { ++kindCountsSignalCount; });

            WPE_CHECK(store.promote(id, EntryKind::Bookmark));
            WPE_CHECK(model.rowCount() == 1);  // 过滤为"全部"：行数不变。
            WPE_CHECK(model.kindCounts().search == 0);
            WPE_CHECK(model.kindCounts().bookmark == 1);
            WPE_CHECK(kindCountsSignalCount >= 1);
        }

        // TestKindFilterHidesPromotedRow：过滤为具体 kind 时，Promote 把条目的可见性
        // 翻转掉，行必须从 rowCount 里消失（这一支走 rebuildRows，行结构确实变了）。
        void TestKindFilterHidesPromotedRow()
        {
            AddressBookStore store(QString{});
            AddressBookModel model(&store);
            const std::uint64_t id = store.add(MakeAbsoluteEntry(EntryKind::Search, "p", 1));
            model.setKindFilter(EntryKind::Search);
            WPE_CHECK(model.rowCount() == 1);

            store.promote(id, EntryKind::Bookmark);
            WPE_CHECK(model.rowCount() == 0);
            WPE_CHECK(model.idAt(model.index(0, 0)) == 0);
        }

        // TestValueCellTextAndState：setValueText 喂入文本与状态，data() 按状态选择占位文字
        // 与前景色；状态变化要能触发 dataChanged（这里只检查数据本身，着色由截图测试核对）。
        void TestValueCellTextAndState()
        {
            AddressBookStore store(QString{});
            AddressBookModel model(&store);
            const std::uint64_t id = store.add(MakeAbsoluteEntry(EntryKind::Watch, "p", 1));
            const QModelIndex valueIndex = model.indexForId(id, AddressBookModel::ColumnValue);

            WPE_CHECK(model.data(valueIndex, Qt::DisplayRole).toString() == QStringLiteral("—"));
            WPE_CHECK(model.valueState(id) == AddressBookModel::ValueState::NotRead);

            model.setValueText(id, QStringLiteral("00000001"), AddressBookModel::ValueState::Read);
            WPE_CHECK(model.data(valueIndex, Qt::DisplayRole).toString() == QStringLiteral("00000001"));
            WPE_CHECK(model.valueText(id) == QStringLiteral("00000001"));
            WPE_CHECK(model.valueState(id) == AddressBookModel::ValueState::Read);

            model.setValueText(id, QString(), AddressBookModel::ValueState::Unreadable);
            WPE_CHECK(model.data(valueIndex, Qt::DisplayRole).toString() == QStringLiteral("不可读"));
        }

        // TestNoteAndValueTypeEditable：setData 对备注落库成功、可经表格直接编辑；值类型
        // 改用右键菜单（直接调 setData）提交，表格里不再标可编辑（修复 C3：双击空白默认
        // 委托提交会把 toInt() 失败的 0 当新类型，静默把类型重置成 Hex8）；"值"列恒拒绝
        // setData，仅 Read 状态允许进入编辑态（修复 C2）。
        void TestNoteAndValueTypeEditable()
        {
            AddressBookStore store(QString{});
            AddressBookModel model(&store);
            const std::uint64_t id = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 1));

            const QModelIndex noteIndex = model.indexForId(id, AddressBookModel::ColumnNote);
            WPE_CHECK(model.flags(noteIndex).testFlag(Qt::ItemIsEditable));
            WPE_CHECK(model.setData(noteIndex, QStringLiteral("备注文字"), Qt::EditRole));
            WPE_CHECK(model.data(noteIndex, Qt::DisplayRole).toString() == QStringLiteral("备注文字"));

            const QModelIndex typeIndex = model.indexForId(id, AddressBookModel::ColumnValueType);
            WPE_CHECK_NOTE(
                !model.flags(typeIndex).testFlag(Qt::ItemIsEditable),
                QStringLiteral("C3：值类型列不应再标可编辑，只能经右键菜单改"));
            WPE_CHECK(model.setData(typeIndex, static_cast<int>(ValueType::F64), Qt::EditRole));
            WPE_CHECK(model.data(typeIndex, Qt::DisplayRole).toString() == AddressBookModel::ValueTypeDisplayName(ValueType::F64));
            // C3 的类型校验：字符串不是合法的类型值，必须被拒绝（不能把 value.toInt() 的
            // 失败结果 0 当成"用户选了 Hex8"）。
            WPE_CHECK(!model.setData(typeIndex, QStringLiteral("无符号32位"), Qt::EditRole));
            WPE_CHECK(model.data(typeIndex, Qt::DisplayRole).toString() == AddressBookModel::ValueTypeDisplayName(ValueType::F64));

            const QModelIndex valueIndex = model.indexForId(id, AddressBookModel::ColumnValue);
            // C2：从未喂入过值（NotRead 状态）时不允许进入编辑态。
            WPE_CHECK_NOTE(
                !model.flags(valueIndex).testFlag(Qt::ItemIsEditable),
                QStringLiteral("C2：NotRead 状态不应允许编辑"));
            model.setValueText(id, QStringLiteral("100"), AddressBookModel::ValueState::Read);
            WPE_CHECK(model.flags(valueIndex).testFlag(Qt::ItemIsEditable));  // Read 状态允许进入编辑态……
            WPE_CHECK(!model.setData(valueIndex, QStringLiteral("deadbeef"), Qt::EditRole));  // ……但 setData 恒拒绝。
            model.setValueText(id, QStringLiteral("100"), AddressBookModel::ValueState::Stale);
            WPE_CHECK_NOTE(
                !model.flags(valueIndex).testFlag(Qt::ItemIsEditable),
                QStringLiteral("C2：Stale 状态不应允许编辑"));
        }

        // 杀 wpE-M1：setValueText 必须精确通知"值"列那一格的 dataChanged，通知到别的列
        // 的话视图根本不会重绘值列（审核报告的变异把它改成通知 ColumnNote）。
        // 顺带是 C1 的 Model 层回归点：文本与状态都没变时不应该发任何 dataChanged。
        void TestSetValueTextNotifiesValueCell()
        {
            AddressBookStore store{ QString() };
            AddressBookModel model(&store);
            store.add(MakeAbsoluteEntry(EntryKind::Watch, "p", 0x1));
            const std::uint64_t id = store.add(MakeAbsoluteEntry(EntryKind::Watch, "p", 0x2));
            QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
            model.setValueText(id, QStringLiteral("1"), AddressBookModel::ValueState::Read);
            WPE_CHECK(changed.count() == 1);
            if (changed.count() == 1)
            {
                const QModelIndex topLeft = changed.first().at(0).value<QModelIndex>();
                const QModelIndex bottomRight = changed.first().at(1).value<QModelIndex>();
                const int row = model.indexForId(id).row();
                WPE_CHECK(topLeft.row() == row && bottomRight.row() == row);
                WPE_CHECK(topLeft.column() == AddressBookModel::ColumnValue && bottomRight.column() == AddressBookModel::ColumnValue);
            }

            // 修复 C1：同样的文本、同样的状态再喂一次，不应该再发一次 dataChanged。
            model.setValueText(id, QStringLiteral("1"), AddressBookModel::ValueState::Read);
            WPE_CHECK_NOTE(changed.count() == 1, QStringLiteral("C1：值与状态都没变就不该再发 dataChanged"));
        }

        // 杀 wpE-M2：值类型 11 种全部可经 setData 设置（从 F64 倒着设到 Hex8，覆盖两端
        // 边界），越界（负数、F64+1）必须被拒绝——变异把下界 "<0" 改成 "<=0" 会让 Hex8（0）
        // 也被拒绝。
        void TestSetDataAcceptsAllElevenTypes()
        {
            AddressBookStore store{ QString() };
            AddressBookModel model(&store);
            const std::uint64_t id = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 0x1));
            const QModelIndex typeIndex = model.indexForId(id, AddressBookModel::ColumnValueType);
            for (int raw = static_cast<int>(ValueType::F64); raw >= static_cast<int>(ValueType::Hex8); --raw)
            {
                WPE_CHECK_NOTE(model.setData(typeIndex, raw, Qt::EditRole), QString::number(raw));
                WPE_CHECK(static_cast<int>(store.find(id)->valueType) == raw);
            }
            WPE_CHECK(!model.setData(typeIndex, static_cast<int>(ValueType::F64) + 1, Qt::EditRole));
            WPE_CHECK(!model.setData(typeIndex, -1, Qt::EditRole));
        }

        // 杀 wpE-M3：传空展示名等于删除映射，应该回退显示原始 targetKey——之前只测了
        // "设置映射"，没测"清除后回退"。
        void TestTargetDisplayNameCanBeCleared()
        {
            AddressBookStore store{ QString() };
            AddressBookModel model(&store);
            const std::uint64_t id = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "pid-9", 1));
            const QModelIndex targetIndex = model.indexForId(id, AddressBookModel::ColumnTarget);
            model.setTargetDisplayName(QStringLiteral("pid-9"), QStringLiteral("game.exe"));
            WPE_CHECK(model.data(targetIndex, Qt::DisplayRole).toString() == QStringLiteral("game.exe"));
            model.setTargetDisplayName(QStringLiteral("pid-9"), QString());
            WPE_CHECK(model.data(targetIndex, Qt::DisplayRole).toString() == QStringLiteral("pid-9"));
        }

        // TestC14_ValueCellForRemovedIdIsIgnored：add -> remove -> 对已删除的 id 调用
        // setValueText 必须被忽略，不应该悄悄存进 m_valueCells；否则异步读回的结果晚到时
        // 会一直累积从不清理的旧 id，号段被复用后旧值会显示到新条目上。
        void TestC14_ValueCellForRemovedIdIsIgnored()
        {
            AddressBookStore store{ QString() };
            AddressBookModel model(&store);
            const std::uint64_t id = store.add(MakeAbsoluteEntry(EntryKind::Watch, "p", 1));
            store.remove(id);
            model.setValueText(id, QStringLiteral("leak"), AddressBookModel::ValueState::Read);
            WPE_CHECK_NOTE(
                model.valueText(id).isEmpty(),
                QStringLiteral("已删除 id 的值缓存必须被忽略，不应残留"));
            WPE_CHECK(model.valueState(id) == AddressBookModel::ValueState::NotRead);
        }

        // TestC14b_RemovingEntryPurgesExistingValueCell：这一条专门钉住 onStoreEntryRemoved
        // 里"删除时主动清掉这个 id 的值缓存"这一步本身——上一条测试里 setValueText 自己也会
        // 查一次 store（id 不存在就忽略），两道防线都在，删掉"主动清理"那道看不出区别；这里
        // 让值是在 id 还有效的时候就已经写进去的，删除之后直接查 valueText()/valueState()
        // （它们本身只查 map，不会再查 store），不经过 setValueText 的第二道防线。
        void TestC14b_RemovingEntryPurgesExistingValueCell()
        {
            AddressBookStore store{ QString() };
            AddressBookModel model(&store);
            const std::uint64_t id = store.add(MakeAbsoluteEntry(EntryKind::Watch, "p", 1));
            model.setValueText(id, QStringLiteral("stale-before-remove"), AddressBookModel::ValueState::Read);
            WPE_CHECK(model.valueText(id) == QStringLiteral("stale-before-remove"));
            store.remove(id);
            WPE_CHECK_NOTE(
                model.valueText(id).isEmpty(),
                QStringLiteral("删除条目后应该主动清掉它的值缓存，不能指望 setValueText 的新调用来兜底"));
            WPE_CHECK(model.valueState(id) == AddressBookModel::ValueState::NotRead);
        }

        // TestHeaderDataTranslatesInEnglish（修复 C12）：headerData 必须真的经语言包翻译，
        // 不是源码里直接写死的中文永远原样返回——en-US 下七个表头都应该是英文。
        // 本测试会真的切一次当前语言，结束前切回 zh-CN，不影响同进程里其它测试。
        void TestHeaderDataTranslatesInEnglish()
        {
            QString initError;
            const bool initOk = ks::i18n::LanguageManager::instance().initialize(QStringLiteral("zh-CN"), &initError);
            WPE_CHECK_NOTE(initOk, initError);

            AddressBookStore store{ QString() };
            AddressBookModel model(&store);
            WPE_CHECK(model.headerData(AddressBookModel::ColumnAddress, Qt::Horizontal).toString() == QStringLiteral("地址"));

            QString switchError;
            const bool switchOk =
                ks::i18n::LanguageManager::instance().setLanguage(QStringLiteral("en-US"), &switchError);
            WPE_CHECK_NOTE(switchOk, switchError);

            WPE_CHECK(model.headerData(AddressBookModel::ColumnAddress, Qt::Horizontal).toString() == QStringLiteral("Address"));
            WPE_CHECK(model.headerData(AddressBookModel::ColumnValueType, Qt::Horizontal).toString() == QStringLiteral("Value type"));
            // 泛化核对：en-US 下全部七个表头都不应该再含汉字。
            for (int column = 0; column < static_cast<int>(AddressBookModel::ColumnCount); ++column)
            {
                const QString header = model.headerData(column, Qt::Horizontal).toString();
                bool hasHan = false;
                for (const QChar ch : header)
                {
                    if (ch.unicode() >= 0x4E00 && ch.unicode() <= 0x9FFF)
                    {
                        hasHan = true;
                        break;
                    }
                }
                WPE_CHECK_NOTE(!hasHan, QStringLiteral("列 %1 的表头仍含汉字：%2").arg(column).arg(header));
            }

            // 切回去：本进程内其余测试（以及本文件后面的 TestHeaderData）都假设当前语言是
            // zh-CN，不切回去会把后续全部按字面中文比较的断言带崩。
            ks::i18n::LanguageManager::instance().setLanguage(QStringLiteral("zh-CN"));
        }

        // TestHeaderData：七个列的表头文案与设计文档一致。
        void TestHeaderData()
        {
            AddressBookStore store(QString{});
            AddressBookModel model(&store);
            WPE_CHECK(model.headerData(AddressBookModel::ColumnAddress, Qt::Horizontal).toString() == QStringLiteral("地址"));
            WPE_CHECK(model.headerData(AddressBookModel::ColumnValue, Qt::Horizontal).toString() == QStringLiteral("值"));
            WPE_CHECK(model.headerData(AddressBookModel::ColumnValueType, Qt::Horizontal).toString() == QStringLiteral("值类型"));
            WPE_CHECK(model.headerData(AddressBookModel::ColumnNote, Qt::Horizontal).toString() == QStringLiteral("备注"));
            WPE_CHECK(model.headerData(AddressBookModel::ColumnModuleOffset, Qt::Horizontal).toString() == QStringLiteral("模块+RVA"));
            WPE_CHECK(model.headerData(AddressBookModel::ColumnTarget, Qt::Horizontal).toString() == QStringLiteral("目标"));
        }

        // TestTargetDisplayName：映射存在时显示映射名，否则回退原始 targetKey。
        void TestTargetDisplayName()
        {
            AddressBookStore store(QString{});
            AddressBookModel model(&store);
            const std::uint64_t id = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "pid-1234", 1));
            const QModelIndex targetIndex = model.indexForId(id, AddressBookModel::ColumnTarget);
            WPE_CHECK(model.data(targetIndex, Qt::DisplayRole).toString() == QStringLiteral("pid-1234"));

            model.setTargetDisplayName(QStringLiteral("pid-1234"), QStringLiteral("chrome.exe · PID 1234"));
            WPE_CHECK(model.data(targetIndex, Qt::DisplayRole).toString() == QStringLiteral("chrome.exe · PID 1234"));
        }
    }

    void RunModelTests()
    {
        TestSortThenOperateById();
        TestKindFilterAndCounts();
        TestRedundantKindFilterIsNoOp();
        TestKindFilterSwitchDoesNotEmitCountsChanged();
        TestPromoteUnderAllFilterUpdatesCounts();
        TestKindFilterHidesPromotedRow();
        TestValueCellTextAndState();
        TestNoteAndValueTypeEditable();
        TestHeaderData();
        TestTargetDisplayName();
        TestSetValueTextNotifiesValueCell();
        TestSetDataAcceptsAllElevenTypes();
        TestTargetDisplayNameCanBeCleared();
        TestC14_ValueCellForRemovedIdIsIgnored();
        TestC14b_RemovingEntryPurgesExistingValueCell();
        // 放在最后：本测试会真的切换 LanguageManager 的当前语言（结束前切回 zh-CN），
        // 把它放在所有"按字面中文比较"的测试之后，降低顺序耦合带来的意外。
        TestHeaderDataTranslatesInEnglish();
    }
}
