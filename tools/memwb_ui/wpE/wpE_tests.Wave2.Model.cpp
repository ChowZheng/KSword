// wpE_tests.Wave2.Model.cpp
// 作用：第二轮独立审核报告 review2-wpE.md 的 Model 相关补测（并入默认运行）。覆盖 C8 修复
// 新写的增量模型代码——此前零覆盖：增量删除（第 0 行边界、擦错行）、过滤下的增量插入、
// Promote 使条目"进入"当前过滤（夹具原来只测了"离开"方向）、备注/值类型编辑不改变计数、
// 批量路径清理值缓存、整表重建（切过滤）不得清掉仍然存活条目的值缓存、setData 对数字
// 字符串/浮点数的类型守卫。
// 来源：Wave 2 第二轮独立审核给出的补测（已实测对未变异代码全部 PASS），按
// Store/Model/Panel 拆成三个文件并入本夹具。

#include "wpE_common.h"

#include <QSignalSpy>

using ks::ui::AddressBookModel;
using ks::ui::AddressBookStore;
using ksword::memwb::EntryKind;
using ksword::memwb::ValueType;
using namespace wpe_test;

namespace
{
    // ========================================================================================
    // [SURVIVOR] 杀 RA04-C3typeId：现有断言只用中文类型名字符串，被 toInt(&ok) 的失败兜住
    // 了；数字字符串（"5"/"0"）与浮点数（能被 toInt 悄悄转换）才真正需要 typeId 守卫。
    // ========================================================================================
    void TestSetDataRejectsNumericStringAndDouble()
    {
        AddressBookStore store{ QString() };
        AddressBookModel model(&store);
        const std::uint64_t id = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 0x1));
        const QModelIndex typeIndex = model.indexForId(id, AddressBookModel::ColumnValueType);
        WPE_CHECK(model.setData(typeIndex, static_cast<int>(ValueType::U32), Qt::EditRole));
        WPE_CHECK(!model.setData(typeIndex, QStringLiteral("5"), Qt::EditRole));
        WPE_CHECK(!model.setData(typeIndex, QStringLiteral("0"), Qt::EditRole));
        WPE_CHECK(!model.setData(typeIndex, 7.0, Qt::EditRole));
        WPE_CHECK(!model.setData(typeIndex, QVariant(), Qt::EditRole));
        WPE_CHECK(store.find(id)->valueType == ValueType::U32);
    }

    // ========================================================================================
    // [SURVIVOR] 杀 NC04/NC05：增量删除——第 0 行（边界）与中间行都要擦对行，不能擦掉
    // 末行或漏掉边界。
    // ========================================================================================
    void TestIncrementalRemoveKeepsRowsInSync()
    {
        AddressBookStore store{ QString() };
        AddressBookModel model(&store);
        const std::uint64_t a = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 1));
        const std::uint64_t b = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 2));
        const std::uint64_t c = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 3));
        const std::uint64_t d = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 4));
        store.remove(a);  // 第 0 行
        WPE_CHECK(model.rowCount() == 3);
        WPE_CHECK(model.idAt(model.index(0, 0)) == b && model.idAt(model.index(1, 0)) == c
            && model.idAt(model.index(2, 0)) == d);
        store.remove(c);  // 中间行
        WPE_CHECK(model.rowCount() == 2);
        WPE_CHECK(model.idAt(model.index(0, 0)) == b && model.idAt(model.index(1, 0)) == d);
        WPE_CHECK(model.data(model.index(1, AddressBookModel::ColumnAddress), Qt::DisplayRole).toString()
            == QStringLiteral("0x4"));
    }

    // ========================================================================================
    // [SURVIVOR] 杀 NC06：过滤开启下的增量插入——命中过滤的条目出现在表里，未命中的不出现，
    // 全局计数仍按全量统计。
    // ========================================================================================
    void TestIncrementalAddUnderFilter()
    {
        AddressBookStore store{ QString() };
        AddressBookModel model(&store);
        model.setKindFilter(EntryKind::Watch);
        QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
        const std::uint64_t w = store.add(MakeAbsoluteEntry(EntryKind::Watch, "p", 1));
        store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 2));
        store.add(MakeAbsoluteEntry(EntryKind::Search, "p", 3));
        WPE_CHECK_NOTE(model.rowCount() == 1, QString::number(model.rowCount()));
        WPE_CHECK(inserted.count() == 1);
        WPE_CHECK(model.idAt(model.index(0, 0)) == w);
        WPE_CHECK(model.kindCounts().all == 3 && model.kindCounts().watch == 1);
    }

    // ========================================================================================
    // [SURVIVOR] 杀 NC07：Promote 使条目"进入"当前过滤必须让它出现在表里——夹具原来的
    // TestKindFilterHidesPromotedRow 只测了"离开"方向。
    // ========================================================================================
    void TestPromoteIntoFilterShowsRow()
    {
        AddressBookStore store{ QString() };
        AddressBookModel model(&store);
        const std::uint64_t s = store.add(MakeAbsoluteEntry(EntryKind::Search, "p", 1));
        model.setKindFilter(EntryKind::Bookmark);
        WPE_CHECK(model.rowCount() == 0);
        WPE_CHECK(store.promote(s, EntryKind::Bookmark));
        WPE_CHECK(model.rowCount() == 1);
        WPE_CHECK(model.idAt(model.index(0, 0)) == s);
        WPE_CHECK(model.kindCounts().search == 0 && model.kindCounts().bookmark == 1);
    }

    // ========================================================================================
    // [SURVIVOR] 杀 NC08：备注/值类型/值文本编辑不改变四个计数，不得发 kindCountsChanged
    // （否则 Panel 侧会把分段控件反复删了重建，回归到 C9 的问题）；真的改变计数的只有
    // Promote，且恰好一次。
    // ========================================================================================
    void TestEditsDoNotEmitCountsChanged()
    {
        AddressBookStore store{ QString() };
        AddressBookModel model(&store);
        const std::uint64_t id = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 1));
        QSignalSpy counts(&model, &AddressBookModel::kindCountsChanged);
        store.setNote(id, QStringLiteral("n"));
        store.setValueType(id, ValueType::U32);
        model.setValueText(id, QStringLiteral("1"), AddressBookModel::ValueState::Read);
        WPE_CHECK_NOTE(counts.count() == 0, QStringLiteral("实际发了 %1 次").arg(counts.count()));
        store.promote(id, EntryKind::Watch);
        WPE_CHECK(counts.count() == 1);
    }

    // ========================================================================================
    // [SURVIVOR] 杀 NC20：整表重建（切过滤 + 批量删除其它条目）不得清掉仍然存活条目的值
    // 缓存——computeCountsAndRowIds 的"存活集合"清理只应该针对真的消失的 id。
    // ========================================================================================
    void TestValueCellsSurviveFilterSwitch()
    {
        AddressBookStore store{ QString() };
        AddressBookModel model(&store);
        const std::uint64_t id = store.add(MakeAbsoluteEntry(EntryKind::Watch, "p", 1));
        const std::uint64_t other = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 2));
        model.setValueText(id, QStringLiteral("keep"), AddressBookModel::ValueState::Read);
        model.setKindFilter(EntryKind::Bookmark);
        model.setKindFilter(std::nullopt);
        store.removeMany({ other });   // 批量路径也要走一遍整表重建
        WPE_CHECK(model.valueText(id) == QStringLiteral("keep"));
        WPE_CHECK(model.valueState(id) == AddressBookModel::ValueState::Read);
    }

    // ========================================================================================
    // [SURVIVOR] 杀 RA14b：批量路径（removeMany/clearSearchResults/addMany/load）必须清掉
    // 失效 id 的值缓存——夹具原来只测了"单条 remove 后清缓存"。
    // ========================================================================================
    void TestBatchRemovePurgesValueCells()
    {
        AddressBookStore store{ QString() };
        AddressBookModel model(&store);
        const std::uint64_t s = store.add(MakeAbsoluteEntry(EntryKind::Search, "p", 1));
        model.setValueText(s, QStringLiteral("gone"), AddressBookModel::ValueState::Read);
        store.clearSearchResults();
        WPE_CHECK_NOTE(model.valueText(s).isEmpty(), QStringLiteral("批量删除后值缓存应被清掉"));
    }
}

namespace wpe_test
{
    void RunModelWave2Tests()
    {
        TestSetDataRejectsNumericStringAndDouble();
        TestIncrementalRemoveKeepsRowsInSync();
        TestIncrementalAddUnderFilter();
        TestPromoteIntoFilterShowsRow();
        TestEditsDoNotEmitCountsChanged();
        TestValueCellsSurviveFilterSwitch();
        TestBatchRemovePurgesValueCells();
    }
}
