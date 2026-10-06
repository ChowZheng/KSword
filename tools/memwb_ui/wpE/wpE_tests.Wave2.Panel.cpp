// wpE_tests.Wave2.Panel.cpp
// 作用：第二轮独立审核报告 review2-wpE.md 的 Panel 补测（第一部分，并入默认运行）。覆盖：
// D11（A/B 按钮选中/悬停态必须真的跟随主题切换，不能只断言样式表含 "palette(" 这种恒真
// 判据）、C7 菜单使能与 targetRowId 的歧义（选区在当前格之外时一批动作要置灰）、en-US 下
// 界面文案不得残留汉字的扫描扩面（任务书点名：分段文字/类型图标提示/值列占位/横幅/右键
// 菜单/表头菜单都要扫）、模块地址/值类型列的数值排序键。
// 来源：Wave 2 第二轮独立审核给出的补测。

#include "wpE_common.h"

#include "../../../Ksword5.1/Ksword5.1/Internationalization/LanguageManager.h"
#include "../../../Ksword5.1/Ksword5.1/theme.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/HexViewWidgets.h"

#include <QAction>
#include <QApplication>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QMenu>
#include <QPushButton>
#include <QTableView>
#include <QTest>
#include <QTimer>

#include <cmath>
#include <functional>
#include <optional>

using ks::ui::AddressBookModel;
using ks::ui::AddressBookPanel;
using ks::ui::AddressBookStore;
using ks::ui::HexViewSegmented;
using ksword::memwb::EntryKind;
using ksword::memwb::ValueType;
using namespace wpe_test;

namespace
{
    QTableView* FindView(AddressBookPanel& panel) { return panel.findChild<QTableView*>(); }

    QPushButton* FindButton(AddressBookPanel& panel, const QString& text)
    {
        for (QPushButton* const button : panel.findChildren<QPushButton*>())
        {
            if (button->text() == text)
            {
                return button;
            }
        }
        return nullptr;
    }

    bool ColorsClose(const QColor& a, const QColor& b, const int tolerance = 3)
    {
        return std::abs(a.red() - b.red()) <= tolerance && std::abs(a.green() - b.green()) <= tolerance
            && std::abs(a.blue() - b.blue()) <= tolerance;
    }

    bool HasHan(const QString& text)
    {
        for (const QChar ch : text)
        {
            if (ch.unicode() >= 0x4E00 && ch.unicode() <= 0x9FFF)
            {
                return true;
            }
        }
        return false;
    }

    // 捕获一次会弹出模态 QMenu 的调用里全部动作（含一层子菜单）的文本/使能；轮询间隔要
    // 够长——菜单文案靠 LanguageManager 的 ActionAdded 异步翻译。
    struct MenuItem
    {
        QString text;
        bool enabled = true;
        bool isSub = false;
    };

    std::vector<MenuItem> CaptureMenu(const std::function<void()>& openMenu)
    {
        std::vector<MenuItem> items;
        int ticks = 0;
        QTimer poller;
        poller.setInterval(60);
        QObject::connect(&poller, &QTimer::timeout, &poller, [&]() {
            ++ticks;
            QMenu* const menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (menu != nullptr)
            {
                for (QAction* const action : menu->actions())
                {
                    if (action->isSeparator())
                    {
                        continue;
                    }
                    items.push_back({ action->text(), action->isEnabled(), false });
                    if (action->menu() != nullptr)
                    {
                        for (QAction* const sub : action->menu()->actions())
                        {
                            items.push_back({ sub->text(), sub->isEnabled(), true });
                        }
                    }
                }
                menu->close();
                poller.stop();
                return;
            }
            if (ticks >= 40)
            {
                poller.stop();
            }
        });
        poller.start();
        openMenu();
        return items;
    }

    std::optional<bool> EnabledOf(const std::vector<MenuItem>& items, const QString& prefix)
    {
        for (const MenuItem& item : items)
        {
            if (!item.isSub && item.text.startsWith(prefix))
            {
                return item.enabled;
            }
        }
        return std::nullopt;
    }

    // RepolishLikeMainWindow：模拟主程序 applyAppearanceSettings 切主题时对全局 QSS 的
    // 重设——重设 app 样式表会让全部控件 repolish，样式表里的 palette(...) 占位符才会按
    // 新调色板重新求值。本包自己新加的 changeEvent（监听 ApplicationPaletteChange）已经
    // 不需要这一步也能刷新按钮颜色，这里仍然调用它只是为了同时验证"即便外层也重设了全局
    // 样式表，颜色依然正确"，不依赖触发路径的唯一性。
    void RepolishLikeMainWindow()
    {
        static int generation = 0;
        qApp->setStyleSheet(QStringLiteral("/* wpE wave2 theme switch %1 */").arg(++generation));
        QApplication::processEvents();
    }

    // ========================================================================================
    // [SURVIVOR] 杀 RA01-C10：TestC10 断言"样式表含 palette("，而 OnAccentDynamicHex() 本身
    // 就是 palette(highlighted-text)，样式表里只要还留着它断言恒真。有牙齿的写法是比未选中
    // B 钮的实际像素：切到暗色后必须变成暗色表面色。
    // ========================================================================================
    void TestC10b_UncheckedButtonPixelFollowsTheme()
    {
        ApplyTheme(false);
        AddressBookStore store{ QString() };
        AddressBookModel model(&store);
        AddressBookPanel panel(&model);
        panel.resize(420, 180);
        panel.show();
        QApplication::processEvents();
        QPushButton* const buttonB = FindButton(panel, QStringLiteral("B"));
        WPE_CHECK(buttonB != nullptr);
        if (buttonB == nullptr)
        {
            panel.hide();
            return;
        }
        const QColor light = buttonB->grab().toImage().pixelColor(3, buttonB->height() / 2);
        WPE_CHECK_NOTE(ColorsClose(light, KswordTheme::SurfaceColor()), light.name());
        ApplyTheme(true);
        QApplication::processEvents();
        const QColor dark = buttonB->grab().toImage().pixelColor(3, buttonB->height() / 2);
        WPE_CHECK_NOTE(ColorsClose(dark, KswordTheme::SurfaceColor()),
            QStringLiteral("切到暗色后未选中 B 钮底色应为暗色表面色 %1，实际 %2")
                .arg(KswordTheme::SurfaceColor().name(), dark.name()));
        ApplyTheme(false);
        panel.hide();
    }

    // [DEFECT->已修] D11：选中态底色（原 AccentHex(Blue)）与悬停底（PrimaryBlueSubtleHex）
    // 此前都是调用瞬间固化的字面 #RRGGBB；亮色下建面板、切到暗色后，选中的 A 钮应与"暗色下
    // 新建的面板"颜色一致。
    void TestC10c_CheckedButtonPixelFollowsTheme()
    {
        ApplyTheme(false);
        AddressBookStore store{ QString() };
        AddressBookModel model(&store);
        AddressBookPanel panel(&model);
        panel.resize(420, 180);
        panel.show();
        QApplication::processEvents();
        QPushButton* const buttonA = FindButton(panel, QStringLiteral("A"));  // 默认选中
        WPE_CHECK(buttonA != nullptr);
        if (buttonA == nullptr)
        {
            panel.hide();
            return;
        }
        ApplyTheme(true);
        RepolishLikeMainWindow();
        const QColor switched = buttonA->grab().toImage().pixelColor(3, buttonA->height() / 2);
        AddressBookPanel freshPanel(&model);  // 暗色下新建的面板 = 正确答案
        freshPanel.resize(420, 180);
        freshPanel.show();
        QApplication::processEvents();
        QPushButton* const freshA = FindButton(freshPanel, QStringLiteral("A"));
        WPE_CHECK(freshA != nullptr);
        if (freshA != nullptr)
        {
            const QColor fresh = freshA->grab().toImage().pixelColor(3, 13);
            WPE_CHECK_NOTE(ColorsClose(switched, fresh),
                QStringLiteral("切主题后选中 A 钮底色=%1，暗色下新建的是 %2").arg(switched.name(), fresh.name()));
        }
        freshPanel.hide();
        panel.hide();
        ApplyTheme(false);
        qApp->setStyleSheet(QString());
    }

    // ========================================================================================
    // [SURVIVOR] 杀 RA08b-C7menu：夹具只经菜单点过"跳转"，没看过菜单项的 isEnabled()。
    // 选区={A,C}、当前格在选区外的 B，右键空白处（不落在任何行上，选区与当前格都不受
    // 影响——修复 D10 之后，右键一个"已经被选中"的行会把当前格挪到那一行，不再适合用来
    // 复现"当前格在选区外"这个场景，改用空白处触发菜单）：跳转/反汇编/编辑备注/复制地址/
    // 复制行都没有明确目标（targetRowId()==0），应置灰；"删除"作用于整个选区，仍可用。
    // ========================================================================================
    void TestC7menu_ActionsDisabledWhenCurrentOutsideMultiSelection()
    {
        AddressBookStore store{ QString() };
        AddressBookModel model(&store);
        AddressBookPanel panel(&model);
        store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 0x1000));
        store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 0x2000));
        store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 0x3000));
        panel.resize(520, 400);  // 留足空白区域（3 行远不到 400px 高）供下面右键空白处。
        panel.show();
        QApplication::processEvents();
        QTableView* const view = FindView(panel);
        view->selectRow(0);
        const QPoint posC = view->visualRect(view->model()->index(2, AddressBookModel::ColumnAddress)).center();
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::ControlModifier, posC);
        WPE_CHECK(panel.selectedIds().size() == 2);
        // 用方向键把当前格单独挪到第 1 行（不改变选区）：复现"当前格在选区外"的状态。
        view->selectionModel()->setCurrentIndex(
            view->model()->index(1, AddressBookModel::ColumnAddress), QItemSelectionModel::NoUpdate);
        const QPoint posBlank(10, view->viewport()->height() - 5);
        WPE_CHECK_NOTE(!view->indexAt(posBlank).isValid(), QStringLiteral("这个位置应该落在空白区域，不在任何行上"));
        const std::vector<MenuItem> items = CaptureMenu([view, posBlank]() { emit view->customContextMenuRequested(posBlank); });
        WPE_CHECK(!items.empty());
        WPE_CHECK(EnabledOf(items, QStringLiteral("跳转")).value_or(true) == false);
        WPE_CHECK(EnabledOf(items, QStringLiteral("在反汇编打开")).value_or(true) == false);
        WPE_CHECK(EnabledOf(items, QStringLiteral("编辑备注")).value_or(true) == false);
        WPE_CHECK(EnabledOf(items, QStringLiteral("复制地址")).value_or(true) == false);
        WPE_CHECK(EnabledOf(items, QStringLiteral("复制行")).value_or(true) == false);
        WPE_CHECK(EnabledOf(items, QStringLiteral("删除")).value_or(false) == true);
        panel.hide();
    }

    // ========================================================================================
    // [SURVIVOR] 杀 RA12b/c/d/e/f：夹具里唯一的 i18n 断言是 headerData；分段文字、kind
    // 悬停、值列占位、横幅、值类型名撤回成直接返回中文都不会报错。任务书点名要把这条断言
    // 扩大覆盖面：kind 悬停提示、值列五种状态的显示与悬停、11 种值类型名、分段 4 段文字与
    // 悬停、A/B 提示、横幅、右键菜单（含值类型子菜单）与表头菜单。
    // ========================================================================================
    void TestI18n_EnglishUiHasNoHan()
    {
        QString err;
        ks::i18n::LanguageManager::instance().initialize(QStringLiteral("zh-CN"), &err);
        ks::i18n::LanguageManager::instance().setLanguage(QStringLiteral("en-US"), &err);
        QApplication::processEvents();

        AddressBookStore store{ QString() };
        AddressBookModel model(&store);
        const std::uint64_t w = store.add(MakeModuleEntry(EntryKind::Watch, "pid-1", "client.dll", 0x10));
        const std::uint64_t s = store.add(MakeAbsoluteEntry(EntryKind::Search, "pid-1", 0x20));
        const std::uint64_t b = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "pid-1", 0x30));
        AddressBookPanel panel(&model);
        const auto noHan = [](const QString& text, const char* what) {
            WPE_CHECK_NOTE(!HasHan(text), QStringLiteral("%1 在 en-US 下仍含汉字：%2").arg(QString::fromLatin1(what), text));
        };

        for (const std::uint64_t id : { w, s, b })
        {
            noHan(model.data(model.indexForId(id, AddressBookModel::ColumnKindIcon), Qt::ToolTipRole).toString(), "kind tooltip");
        }
        const QModelIndex valueW = model.indexForId(w, AddressBookModel::ColumnValue);
        const AddressBookModel::ValueState states[] = {
            AddressBookModel::ValueState::NotRead, AddressBookModel::ValueState::Reading,
            AddressBookModel::ValueState::Unreadable, AddressBookModel::ValueState::Stale,
            AddressBookModel::ValueState::Read
        };
        for (const AddressBookModel::ValueState state : states)
        {
            model.setValueText(w,
                (state == AddressBookModel::ValueState::Read || state == AddressBookModel::ValueState::Stale)
                    ? QStringLiteral("42") : QString(),
                state);
            noHan(model.data(valueW, Qt::DisplayRole).toString(), "value display");
            noHan(model.data(valueW, Qt::ToolTipRole).toString(), "value tooltip");
        }
        const QModelIndex typeW = model.indexForId(w, AddressBookModel::ColumnValueType);
        for (int raw = 0; raw <= static_cast<int>(ValueType::F64); ++raw)
        {
            model.setData(typeW, raw, Qt::EditRole);
            noHan(model.data(typeW, Qt::DisplayRole).toString(), "value type name");
        }
        HexViewSegmented* const segment = panel.findChild<HexViewSegmented*>();
        WPE_CHECK(segment != nullptr);
        if (segment != nullptr)
        {
            for (int i = 0; i < 4; ++i)
            {
                noHan(segment->labelAt(i), "segment label");
                noHan(segment->segmentToolTip(i), "segment tooltip");
            }
        }
        QPushButton* const buttonA = FindButton(panel, QStringLiteral("A"));
        QPushButton* const buttonB = FindButton(panel, QStringLiteral("B"));
        WPE_CHECK(buttonA != nullptr && buttonB != nullptr);
        if (buttonA != nullptr) { noHan(buttonA->toolTip(), "A tooltip"); }
        if (buttonB != nullptr) { noHan(buttonB->toolTip(), "B tooltip"); }
        panel.showLoadFailure(QStringLiteral("line 3: bad"), QStringLiteral("C:/x.bad"));
        panel.resize(520, 260);
        panel.show();
        QApplication::processEvents();
        QTableView* const view = FindView(panel);
        const QPoint pos = view->visualRect(view->model()->index(0, AddressBookModel::ColumnAddress)).center();
        for (const MenuItem& item : CaptureMenu([view, pos]() { emit view->customContextMenuRequested(pos); }))
        {
            noHan(item.text, "row menu item");
        }
        QHeaderView* const header = view->horizontalHeader();
        for (const MenuItem& item : CaptureMenu([header]() { emit header->customContextMenuRequested(QPoint(8, 8)); }))
        {
            noHan(item.text, "header menu item");
        }
        panel.hide();
        ks::i18n::LanguageManager::instance().setLanguage(QStringLiteral("zh-CN"), &err);
        QApplication::processEvents();
    }

    // ========================================================================================
    // [SURVIVOR] 杀 NC09/NC22：模块地址按 RVA 数值序排序（不是字符串字典序）；值类型列
    // 按枚举底层整数排序。
    // ========================================================================================
    void TestSortKeysForModuleAddressAndValueType()
    {
        AddressBookStore store{ QString() };
        AddressBookModel model(&store);
        AddressBookPanel panel(&model);
        const std::uint64_t m1 = store.add(MakeModuleEntry(EntryKind::Bookmark, "p", "client.dll", 0x10000));
        const std::uint64_t m2 = store.add(MakeModuleEntry(EntryKind::Bookmark, "p", "client.dll", 0xa00));
        const std::uint64_t m3 = store.add(MakeModuleEntry(EntryKind::Bookmark, "p", "client.dll", 0x2000));
        panel.resize(520, 220);
        panel.show();
        QApplication::processEvents();
        QTableView* const view = FindView(panel);
        view->sortByColumn(AddressBookModel::ColumnAddress, Qt::AscendingOrder);
        QApplication::processEvents();
        QList<quint64> order;
        for (int row = 0; row < view->model()->rowCount(); ++row)
        {
            order << view->model()->index(row, 0).data(AddressBookModel::IdRole).value<quint64>();
        }
        WPE_CHECK_NOTE(order == (QList<quint64>{ m2, m3, m1 }), QStringLiteral("模块地址应按 RVA 数值序"));
        model.setData(model.indexForId(m1, AddressBookModel::ColumnValueType), static_cast<int>(ValueType::U32), Qt::EditRole);
        model.setData(model.indexForId(m2, AddressBookModel::ColumnValueType), static_cast<int>(ValueType::Hex8), Qt::EditRole);
        model.setData(model.indexForId(m3, AddressBookModel::ColumnValueType), static_cast<int>(ValueType::F64), Qt::EditRole);
        view->sortByColumn(AddressBookModel::ColumnValueType, Qt::AscendingOrder);
        QApplication::processEvents();
        order.clear();
        for (int row = 0; row < view->model()->rowCount(); ++row)
        {
            order << view->model()->index(row, 0).data(AddressBookModel::IdRole).value<quint64>();
        }
        WPE_CHECK_NOTE(order == (QList<quint64>{ m2, m1, m3 }), QStringLiteral("值类型列应按枚举序"));
        panel.hide();
    }
}

namespace wpe_test
{
    void RunPanelWave2Tests()
    {
        TestC10b_UncheckedButtonPixelFollowsTheme();
        TestC10c_CheckedButtonPixelFollowsTheme();
        TestC7menu_ActionsDisabledWhenCurrentOutsideMultiSelection();
        TestI18n_EnglishUiHasNoHan();
        TestSortKeysForModuleAddressAndValueType();
    }
}
