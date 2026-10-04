#include "../Ksword5.1/Ksword5.1/Framework/CustomTitleBar.h"
#include "../Ksword5.1/Ksword5.1/UI/GlobalUiSearch.h"
#include "../Ksword5.1/Ksword5.1/UI/CommandExecutionPopup.h"
#include "../Ksword5.1/Ksword5.1/UI/TableSearchSupport.h"
#include "../Ksword5.1/Ksword5.1/theme.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QStandardItemModel>
#include <QTabBar>
#include <QTableView>
#include <QThread>
#include <QToolButton>
#include <QVBoxLayout>
#include <iostream>

namespace
{
    int assertionCount = 0; // 记录独立 UI 回归的实际断言数量。
    int failureCount = 0;   // 任一断言失败时使测试进程返回非零。

    // check 验证可观察的控件行为；message 用于定位失败，不执行生产命令。
    void check(const bool condition, const char* message)
    {
        ++assertionCount;
        if (!condition)
        {
            ++failureCount;
            std::cerr << "FAIL: " << message << '\n';
        }
    }

    // drainEvents 排空排队的弹层刷新；不使用固定等待或开启真实采样。
    void drainEvents()
    {
        for (int round = 0; round < 8; ++round)
        {
            QApplication::processEvents();
        }
    }

    // processFor 覆盖真实防抖定时器窗口，参数为毫秒，结束后继续排空分片事件。
    void processFor(const int milliseconds)
    {
        QElapsedTimer elapsed;
        elapsed.start();
        while (elapsed.elapsed() < milliseconds)
        {
            QApplication::processEvents();
            QThread::msleep(1);
        }
        drainEvents();
    }

    // sendKey 将真实按键送到目标控件，覆盖事件过滤与 QWidget 焦点处理。
    void sendKey(QWidget* target, const int key,
        const Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        QKeyEvent keyEvent(QEvent::KeyPress, key, modifiers);
        QApplication::sendEvent(target, &keyEvent);
        drainEvents();
    }

    // testInputCycle 创建实际标题栏、搜索控制器和 CMD 弹层，参数决定深浅主题。
    void testInputCycle(const bool darkMode)
    {
        KswordTheme::SetDarkModeEnabled(darkMode);
        QWidget host; // 只承载测试控件，不构造会启动业务后端的 MainWindow。
        host.resize(1100, 700);
        QVBoxLayout layout(&host);
        ks::ui::CustomTitleBar titleBar(&host);
        layout.addWidget(&titleBar);
        QTableView table(&host);
        table.setObjectName(QStringLiteral("historySearchFixtureTable"));
        QStandardItemModel model(3, 1);
        model.setData(model.index(0, 0), QStringLiteral("needle"));
        model.setData(model.index(1, 0), QStringLiteral("other"));
        model.setData(model.index(2, 0), QStringLiteral("already hidden"));
        table.setModel(&model);
        table.setRowHidden(2, true);
        layout.addWidget(&table);

        // 按生产创建顺序安装两组事件过滤器，防止单独测试遗漏后安装者的截获。
        QLineEdit* input = titleBar.titleInputLineEdit();
        ks::ui::GlobalUiSearchController search(
            &host, input, titleBar.titleInputAnchorWidget());
        ks::ui::CommandExecutionPopup commandPopup(
            &host, titleBar.titleInputAnchorWidget(), input);
        QObject::connect(&titleBar, &ks::ui::CustomTitleBar::inputModeChanged,
            &commandPopup, [&commandPopup](const bool searchMode) {
                commandPopup.setCommandModeActive(!searchMode);
            });
        QObject::connect(&search,
            &ks::ui::GlobalUiSearchController::requestSearchInputActivation,
            &titleBar, &ks::ui::CustomTitleBar::activateSearchInput);
        QObject::connect(&search,
            &ks::ui::GlobalUiSearchController::requestCommandInputActivation,
            &titleBar, &ks::ui::CustomTitleBar::activateCommandInput);
        QObject::connect(&search,
            &ks::ui::GlobalUiSearchController::searchScopeDisplayTextChanged,
            &titleBar, &ks::ui::CustomTitleBar::setSearchScopeDisplayText);
        QObject::connect(&titleBar, &ks::ui::CustomTitleBar::searchTextEdited,
            &search, &ks::ui::GlobalUiSearchController::handleQueryEdited);
        QObject::connect(&titleBar, &ks::ui::CustomTitleBar::inputModeChanged,
            &search, &ks::ui::GlobalUiSearchController::setSearchInputActive);

        // executeRequested 只记入测试变量，绝不连接 CreateProcess 或系统操作。
        int executionCount = 0;
        QString executedText;
        QObject::connect(&commandPopup, &ks::ui::CommandExecutionPopup::executeRequested,
            &host, [&executionCount, &executedText](const QString& command,
                const ks::ui::CommandExecutionOptions&) {
                ++executionCount;
                executedText = command;
            });
        host.show();
        input->setFocus();
        drainEvents();
        check(titleBar.isSearchInputModeActive(), "initial search mode");
        check(search.searchScopeDisplayText() == QStringLiteral("全局"), "initial global scope");

        // 每个方向跑两整圈，观察范围、模式和焦点，不读取私有状态或复制算法。
        for (int round = 0; round < 2; ++round)
        {
            sendKey(input, Qt::Key_Tab);
            check(search.searchScopeDisplayText() == QStringLiteral("当前页面"), "forward page");
            sendKey(input, Qt::Key_Tab);
            check(search.searchScopeDisplayText().startsWith(QStringLiteral("当前表格")), "forward table");
            sendKey(input, Qt::Key_Tab);
            check(!titleBar.isSearchInputModeActive(), "forward table to CMD");
            check(commandPopup.isPopupVisible(), "CMD popup follows mode");
            check(input->hasFocus(), "CMD keeps input focus");
            check(executionCount == 0, "Tab never submits command");
            sendKey(input, Qt::Key_Tab);
            check(titleBar.isSearchInputModeActive(), "forward CMD to search");
            check(search.searchScopeDisplayText() == QStringLiteral("全局"), "forward CMD to global");
            check(!commandPopup.isPopupVisible(), "search hides CMD popup");
        }
        for (int round = 0; round < 2; ++round)
        {
            sendKey(input, Qt::Key_Backtab, Qt::ShiftModifier);
            check(!titleBar.isSearchInputModeActive(), "reverse global to CMD");
            sendKey(input, Qt::Key_Backtab, Qt::ShiftModifier);
            check(titleBar.isSearchInputModeActive(), "reverse CMD to search");
            check(search.searchScopeDisplayText().startsWith(QStringLiteral("当前表格")), "reverse CMD to table");
            sendKey(input, Qt::Key_Backtab, Qt::ShiftModifier);
            check(search.searchScopeDisplayText() == QStringLiteral("当前页面"), "reverse page");
            sendKey(input, Qt::Key_Backtab, Qt::ShiftModifier);
            check(search.searchScopeDisplayText() == QStringLiteral("全局"), "reverse global");
        }

        // Shift+Tab 还可能以 Key_Tab 到达，不能只处理 Key_Backtab 的平台表示。
        sendKey(input, Qt::Key_Tab, Qt::ShiftModifier);
        check(!titleBar.isSearchInputModeActive(), "Shift+Key_Tab reaches CMD");
        sendKey(input, Qt::Key_Tab, Qt::ShiftModifier);
        check(search.searchScopeDisplayText().startsWith(QStringLiteral("当前表格")), "Shift+Key_Tab returns table");

        // 带 Ctrl/Alt/Meta 的 Tab 不应改变当前搜索范围或抢成模式循环。
        // 正常焦点移动可能更新当前表格名称；比较范围标签索引，不冻结上下文名称。
        auto* modifierScopeTabs = host.findChild<QTabBar*>();
        check(modifierScopeTabs != nullptr, "scope tabs available before modifier test");
        const int unchangedScopeIndex = modifierScopeTabs != nullptr
            ? modifierScopeTabs->currentIndex()
            : -1;
        for (const auto modifiers : {Qt::ControlModifier, Qt::AltModifier, Qt::MetaModifier})
        {
            input->setFocus();
            sendKey(input, Qt::Key_Tab, modifiers);
            check(titleBar.isSearchInputModeActive(), "modified Tab preserves search mode");
            check(modifierScopeTabs != nullptr
                && modifierScopeTabs->currentIndex() == unchangedScopeIndex, "modified Tab preserves scope");
        }

        // 完成真实防抖查询并安装结果专显，随后验证跨模式撤销行过滤和旧结果。
        ks::ui::InstallTableSearchSupport(&table);
        check(ks::ui::IsGenericTableSearchEligible(&table), "fixture table supports real row filtering");
        search.setSearchResultsOnly(true);
        search.activateForTable(&table, QStringLiteral("needle"), true);
        processFor(300);
        auto* resultList = host.findChild<QListWidget*>(QStringLiteral("ksGlobalUiSearchResultList"));
        check(!table.isRowHidden(0) && table.isRowHidden(1), "completed query filters unmatched row");
        check(table.isRowHidden(2), "query preserves independently hidden row");
        check(resultList != nullptr && resultList->count() == 1, "completed query presents one hit");
        sendKey(input, Qt::Key_Tab);
        check(!table.isRowHidden(1), "entering CMD removes query row filter");
        check(table.isRowHidden(2), "entering CMD preserves original row visibility");
        input->clear();
        sendKey(input, Qt::Key_Tab);
        processFor(300);
        check(resultList != nullptr && resultList->count() == 0, "empty return cancels old timer and clears hits");
        check(!table.isRowHidden(1) && table.isRowHidden(2), "empty return leaves original table visibility");
        // 在第一搜索分片仍排队时立即跨模式，旧代数不能重新写回结果或过滤。
        search.activateForTable(&table, QStringLiteral("other"), true);
        search.dismissPopup();
        QKeyEvent beginSearch(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(input, &beginSearch);
        QKeyEvent enterCommand(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
        QApplication::sendEvent(input, &enterCommand);
        input->clear();
        QKeyEvent returnSearch(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
        QApplication::sendEvent(input, &returnSearch);
        processFor(300);
        check(titleBar.isSearchInputModeActive(), "queued search race returns to search");
        check(resultList != nullptr && resultList->count() == 0, "old queued chunk cannot publish hits");
        check(!table.isRowHidden(0) && !table.isRowHidden(1) && table.isRowHidden(2),
            "old queued chunk cannot restore query filter");
        search.setSearchResultsOnly(false);

        // 从表格入口回到搜索后，原查询和 popup 三范围标签仍能正确工作。
        search.activateForTable(&table, QStringLiteral("needle"), true);
        drainEvents();
        check(input->text() == QStringLiteral("needle"), "table activation copies query");
        sendKey(input, Qt::Key_Tab);
        check(!titleBar.isSearchInputModeActive(), "table activation reaches CMD");
        check(input->text() == QStringLiteral("needle"), "mode change preserves input text");
        input->clear();
        sendKey(input, Qt::Key_Tab);
        check(titleBar.isSearchInputModeActive() && input->text().isEmpty(), "CMD empty input returns empty search");
        auto* scopeTabs = host.findChild<QTabBar*>();
        check(scopeTabs != nullptr && scopeTabs->count() == 3, "popup retains three mouse scope tabs");
        if (scopeTabs != nullptr)
        {
            scopeTabs->setCurrentIndex(2);
            drainEvents();
            check(search.searchScopeDisplayText().startsWith(QStringLiteral("当前表格")), "mouse scope selection retained");
        }

        // 用既有菜单切模式，确保新增键盘路径没有取代菜单触发链。
        QAction* commandAction = nullptr;
        QAction* searchAction = nullptr;
        for (QAction* action : titleBar.findChildren<QAction*>())
        {
            if (action->isCheckable() && action->text().contains(QStringLiteral("CMD")))
            {
                commandAction = action;
            }
            else if (action->isCheckable() && action->text().contains(QStringLiteral("搜索")))
            {
                searchAction = action;
            }
        }
        check(commandAction != nullptr && searchAction != nullptr, "both mode menu actions exist");
        if (commandAction != nullptr && searchAction != nullptr)
        {
            commandAction->trigger();
            drainEvents();
            check(!titleBar.isSearchInputModeActive(), "CMD menu action retained");
            searchAction->trigger();
            drainEvents();
            check(titleBar.isSearchInputModeActive(), "search menu action retained");
        }

        // CMD 弹层内部字段的 Tab 继续使用 Qt 焦点顺序，不能跨到全局搜索。
        titleBar.activateCommandInput(true);
        auto* workingDirectory = commandPopup.findChild<QLineEdit*>();
        check(workingDirectory != nullptr, "CMD options have editable field");
        if (workingDirectory != nullptr)
        {
            workingDirectory->setFocus();
            sendKey(workingDirectory, Qt::Key_Tab);
            check(!titleBar.isSearchInputModeActive(), "CMD inner field Tab keeps mode");
        }

        // Escape 收起配置但保持 CMD；Enter 仍走校验后的既有执行信号。
        input->setFocus();
        sendKey(input, Qt::Key_Escape);
        check(!titleBar.isSearchInputModeActive() && !commandPopup.isPopupVisible(), "CMD Escape keeps mode and closes popup");
        input->setText(QStringLiteral("echo history-fixture"));
        sendKey(input, Qt::Key_Return);
        check(executionCount == 1 && executedText == QStringLiteral("echo history-fixture"), "CMD Enter retains execute signal");
        sendKey(input, Qt::Key_Tab);
        sendKey(input, Qt::Key_Return);
        check(executionCount == 1, "search Enter never submits command");
    }
}

// main 只运行离屏 UI 夹具；返回码和实际断言统计用于构建脚本验收。
int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);
    testInputCycle(false);
    testInputCycle(true);
    std::cout << "SEARCH_HISTORY_ASSERTIONS=" << assertionCount << '\n';
    std::cout << "SEARCH_HISTORY_FAILURES=" << failureCount << '\n';
    return failureCount == 0 ? 0 : 1;
}
