#include "../Ksword5.1/Ksword5.1/Framework/CustomTitleBar.h"
#include "../Ksword5.1/Ksword5.1/UI/GlobalUiSearch.h"
#include "../Ksword5.1/Ksword5.1/UI/CommandExecutionPopup.h"
#include "../Ksword5.1/Ksword5.1/UI/TableSearchSupport.h"
#include "../Ksword5.1/Ksword5.1/UI/ThemeItemForeground.h"
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
#include <QImage>
#include <QPainter>
#include <QPersistentModelIndex>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QScrollBar>
#include <QStyleOptionViewItem>
#include <QStandardItemModel>
#include <QTabBar>
#include <QTableView>
#include <QTableWidget>
#include <QTreeWidget>
#include <QThread>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
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

    // applyFixtureTheme 模拟主程序提交当前主题后的 palette 通知，不调用 MainWindow 或业务后端。
    void applyFixtureTheme(const bool darkMode, const QString& background, const QString& accent)
    {
        KswordTheme::SetDarkModeEnabled(darkMode);
        KswordTheme::SetMainBackgroundColor(background);
        KswordTheme::SetPrimaryAccentColor(accent);
        QPalette palette = QApplication::palette(); // 仅离屏应用 palette，用真实主题表面与文字角色。
        palette.setColor(QPalette::Window, KswordTheme::WindowColor());
        palette.setColor(QPalette::Base, KswordTheme::SurfaceColor());
        palette.setColor(QPalette::AlternateBase, KswordTheme::SurfaceAltColor());
        palette.setColor(QPalette::Text, KswordTheme::TextPrimaryColor());
        palette.setColor(QPalette::WindowText, KswordTheme::TextPrimaryColor());
        palette.setColor(QPalette::Highlight, KswordTheme::PrimaryAccentColor());
        palette.setColor(QPalette::HighlightedText, KswordTheme::OnAccentColor());
        QApplication::setPalette(palette);
        drainEvents();
    }

    // testItemForegroundRefresh 用真实 Qt 树/表测试生产刷新函数；不构造会枚举或监控的 Dock。
    void testItemForegroundRefresh()
    {
        using Role = ks::ui::ItemForegroundRole;
        applyFixtureTheme(false, QString(), QString());
        QTableWidget table(6, 2); // 五种语义项和一项独立数据原色。
        table.setAlternatingRowColors(true);
        ks::ui::InstallThemeItemForegroundDelegate(&table);
        const Role roles[] = {Role::Accent, Role::Secondary, Role::Info, Role::Warning, Role::Error};
        for (int row = 0; row < 6; ++row)
        {
            table.setItem(row, 0, new QTableWidgetItem(QString::number(row)));
            table.setItem(row, 1, new QTableWidgetItem(QStringLiteral("stable value")));
            if (row < 5)
            {
                ks::ui::ApplyThemeItemForeground(table.item(row, 1), roles[row]);
            }
        }
        table.item(5, 1)->setForeground(QColor(210, 60, 120));
        table.setSortingEnabled(true);
        table.sortItems(0, Qt::AscendingOrder);
        table.setCurrentCell(2, 1);
        table.setRowHidden(4, true);
        const QPersistentModelIndex selected = table.currentIndex(); // 当前选择必须跨刷新保持有效。
        QTableWidgetItem* const stableItem = table.item(2, 1);
        const QColor dataColor = table.item(5, 1)->foreground().color();
        int changedSignals = 0; // ForegroundRole 更新不得通知业务 itemChanged/搜索/排序监听。
        QObject::connect(table.model(), &QAbstractItemModel::dataChanged, &table,
            [&changedSignals]() { ++changedSignals; });
        int itemChangedSignals = 0; // 同时覆盖 QTableWidget 的对外业务信号。
        QObject::connect(&table, &QTableWidget::itemChanged, &table,
            [&itemChangedSignals]() { ++itemChangedSignals; });

        QTreeWidget tree;
        tree.setColumnCount(2);
        tree.setAlternatingRowColors(true);
        ks::ui::InstallThemeItemForegroundDelegate(&tree);
        auto* root = new QTreeWidgetItem(&tree);
        auto* child = new QTreeWidgetItem(root);
        root->setText(0, QStringLiteral("root"));
        child->setText(0, QStringLiteral("child"));
        ks::ui::ApplyThemeItemForeground(root, 1, Role::Accent);
        ks::ui::ApplyThemeItemForeground(child, 1, Role::Warning);
        root->setExpanded(true);
        tree.setCurrentItem(child);
        int treeChangedSignals = 0; // 树节点字段变更监听也不能在重着色时触发。
        QObject::connect(&tree, &QTreeWidget::itemChanged, &tree,
            [&treeChangedSignals]() { ++treeChangedSignals; });

        // 连续 A→B→C 换色和黑/白/灰种子覆盖原项；真实绘制选态逐一对当前底保持可读。
        for (const bool darkMode : {false, true})
        {
            for (const QString& background : {QString(), QStringLiteral("#FFFFFF"),
                QStringLiteral("#000000"), QStringLiteral("#646464")})
            {
                for (const QString& accent : {QStringLiteral("#216D40"),
                    QStringLiteral("#CC4400"), QStringLiteral("#FFFFFF")})
                {
                    applyFixtureTheme(darkMode, background, accent);
                    ks::ui::RefreshThemeItemForegrounds(&table);
                    ks::ui::RefreshThemeItemForegrounds(&tree);
                    check(table.item(2, 1) == stableItem && table.currentIndex() == selected,
                        "semantic refresh preserves table items and selection");
                    check(table.isRowHidden(4) && table.isSortingEnabled(),
                        "semantic refresh preserves hidden rows and sorting");
                    check(table.item(5, 1)->foreground().color() == dataColor,
                        "semantic refresh leaves untagged data colors untouched");
                    check(tree.currentItem() == child && root->isExpanded() && child->parent() == root,
                        "semantic refresh preserves tree selection and expansion");
                    check(changedSignals == 0 && itemChangedSignals == 0 && treeChangedSignals == 0,
                        "semantic refresh emits no model table or tree business changes");
                    for (int row = 0; row < 5; ++row)
                    {
                        for (int state = 0; state < 3; ++state)
                        {
                            QStyleOptionViewItem option; // Alternate 由绘制选项给出，不假定 model row 奇偶。
                            option.initFrom(&table);
                            option.rect = QRect(0, 0, 220, 32);
                            option.font.setPixelSize(16);
                            option.state = QStyle::State_Enabled;
                            if (state == 1)
                            {
                                option.features |= QStyleOptionViewItem::Alternate;
                            }
                            if (state == 2)
                            {
                                option.state |= QStyle::State_Selected;
                            }
                            const QModelIndex index = table.model()->index(row, 1);
                            ks::ui::ApplyThemeItemForegroundToStyleOption(&option, index);
                            const QColor actualBackground = state == 2 ? option.palette.color(QPalette::Highlight)
                                : state == 1 ? option.palette.color(QPalette::AlternateBase)
                                : option.palette.color(QPalette::Base);
                            const QColor actualForeground = option.palette.color(
                                state == 2 ? QPalette::HighlightedText : QPalette::Text);
                            check(KswordTheme::ContrastRatio(actualForeground, actualBackground) >= 4.499,
                                "semantic text readable on actual normal alternate or selected background");
                            QImage image(220, 32, QImage::Format_ARGB32_Premultiplied);
                            image.fill(actualBackground);
                            QPainter painter(&image);
                            table.itemDelegate()->paint(&painter, option, index);
                            painter.end();
                            int foregroundPixels = 0; // 验证实际已安装 delegate 使用了该当前前景。
                            for (int y = 0; y < image.height(); ++y)
                            {
                                for (int x = 0; x < image.width(); ++x)
                                {
                                    if (image.pixelColor(x, y).rgb() == actualForeground.rgb())
                                    {
                                        ++foregroundPixels;
                                    }
                                }
                            }
                            check(foregroundPixels > 2, "actual semantic delegate renders current foreground");
                        }
                    }
                    check(root->foreground(1).color() == table.item(0, 1)->foreground().color(),
                        "tree and table share current accent role");
                }
            }
        }
        applyFixtureTheme(false, QString(), QString());
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
        if (resultList != nullptr && resultList->count() == 1)
        {
            // 标记真实结果对象：如果主题刷新重新扫描/重建列表，这个标记会消失。
            QListWidgetItem* const originalItem = resultList->item(0);
            originalItem->setData(Qt::UserRole + 93, 4821);
            const int scrollValue = resultList->verticalScrollBar()->value();
            for (const bool nextDarkMode : {false, true, false})
            {
                applyFixtureTheme(nextDarkMode, nextDarkMode ? QStringLiteral("#000000")
                    : QStringLiteral("#FFFFFF"), nextDarkMode ? QStringLiteral("#FFFFFF")
                    : QStringLiteral("#216D40"));
                check(resultList->item(0) == originalItem
                    && originalItem->data(Qt::UserRole + 93).toInt() == 4821,
                    "theme refresh keeps original search item instead of rescanning");
                check(resultList->currentRow() == 0
                    && resultList->verticalScrollBar()->value() == scrollValue,
                    "theme refresh preserves selected result and scroll");
                check(input->text() == QStringLiteral("needle") && !table.isRowHidden(0)
                    && table.isRowHidden(1) && table.isRowHidden(2),
                    "theme refresh preserves query and filtered row snapshots");
                const QString html = originalItem->data(Qt::UserRole + 41).toString();
                check(html.contains(QStringLiteral("needle")), "theme refresh uses original query snapshot");
                check(!html.contains(QRegularExpression(QStringLiteral("color:#[0-9A-Fa-f]{6}"))),
                    "search cache retains semantic markup instead of stale RGB");
                for (int state = 0; state < 3; ++state)
                {
                    // 直接调用生产结果 delegate 的绘制，验证 normal/hover/selected 的语义解析。
                    QStyleOptionViewItem option;
                    option.initFrom(resultList);
                    option.rect = QRect(0, 0, 500, 56);
                    option.font.setPixelSize(16);
                    option.state = QStyle::State_Enabled;
                    if (state == 1)
                    {
                        option.state |= QStyle::State_MouseOver;
                    }
                    if (state == 2)
                    {
                        option.state |= QStyle::State_Selected;
                    }
                    const QColor base = option.palette.color(QPalette::Base);
                    const QColor actualBackground = state == 0 ? base
                        : KswordTheme::BlendColors(base, KswordTheme::PrimaryAccentColor(), state == 2 ? 52 : 26);
                    const QColor currentText = KswordTheme::EnsureTextContrast(
                        KswordTheme::PrimaryAccentColor(), actualBackground, 4.5);
                    QImage image(500, 56, QImage::Format_ARGB32_Premultiplied);
                    image.fill(base);
                    QPainter painter(&image);
                    resultList->itemDelegate()->paint(&painter, option, resultList->model()->index(0, 0));
                    painter.end();
                    int accentPixels = 0; // 匹配命中片段的实心像素，排除抗锯齿边缘和行底填充。
                    for (int y = 0; y < image.height(); ++y)
                    {
                        for (int x = 0; x < image.width(); ++x)
                        {
                            if (image.pixelColor(x, y).rgb() == currentText.rgb())
                            {
                                ++accentPixels;
                            }
                        }
                    }
                    check(KswordTheme::ContrastRatio(currentText, actualBackground) >= 4.499
                        && accentPixels > 2, "actual search delegate renders readable current accent per state");
                }
            }
            applyFixtureTheme(darkMode, QString(), QString());
        }
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
        // 第一分片排队时送主题事件；颜色回调不能使已作废的搜索代次重新完成。
        QEvent themeEvent(QEvent::ApplicationPaletteChange);
        QApplication::sendEvent(&host, &themeEvent);
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

        // 正文恰好等于内部颜色标记时必须原样绘制，不能被属性解析替换为短 RGB 串。
        model.setData(model.index(0, 0), QStringLiteral("__KS_SEARCH_PRIMARY_COLOR__"));
        search.activateForTable(&table, QStringLiteral("__KS_SEARCH_PRIMARY_COLOR__"), true);
        processFor(300);
        check(resultList != nullptr && resultList->count() == 1, "literal color token remains searchable");
        if (resultList != nullptr && resultList->count() == 1)
        {
            QStyleOptionViewItem option;
            option.initFrom(resultList);
            option.rect = QRect(0, 0, 900, 56);
            option.font.setPixelSize(16);
            option.state = QStyle::State_Enabled;
            const QColor base = option.palette.color(QPalette::Base);
            const QColor accent = KswordTheme::EnsureTextContrast(
                KswordTheme::PrimaryAccentColor(), base, 4.5);
            QImage image(900, 56, QImage::Format_ARGB32_Premultiplied);
            image.fill(base);
            QPainter painter(&image);
            resultList->itemDelegate()->paint(&painter, option, resultList->model()->index(0, 0));
            painter.end();
            int rightmostAccentPixel = 0; // 只读命中正文第一行；原字面量比 #RRGGBB 明显更长。
            for (int y = 6; y < 27; ++y)
            {
                for (int x = 0; x < image.width(); ++x)
                {
                    if (image.pixelColor(x, y).rgb() == accent.rgb())
                    {
                        rightmostAccentPixel = std::max(rightmostAccentPixel, x);
                    }
                }
            }
            check(rightmostAccentPixel > 180, "actual HTML drawing preserves literal token text");
        }
    }
}

// main 只运行离屏 UI 夹具；返回码和实际断言统计用于构建脚本验收。
int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);
    testItemForegroundRefresh();
    testInputCycle(false);
    testInputCycle(true);
    std::cout << "SEARCH_HISTORY_ASSERTIONS=" << assertionCount << '\n';
    std::cout << "SEARCH_HISTORY_FAILURES=" << failureCount << '\n';
    return failureCount == 0 ? 0 : 1;
}
