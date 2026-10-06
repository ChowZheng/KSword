// ============================================================
// wpJ5_tests.Find.cpp
// 作用：覆盖"查找条 Esc/F3"（照抄 HexView.Panels.cpp 规则的自行实现）——真实
// 按键经真实 QShortcut 触发，不直接调用 openFind/closeFindBar 走捷径，仿
// tools/memwb_ui/memwb_ui_tests.HexView.cpp 的 ActivateWindow + QTest::keyClick
// 写法。
// ============================================================

#include "wpJ5_common.h"

#include <QLineEdit>
#include <QShortcut>
#include <QSignalSpy>
#include <QTest>

namespace wpj5_test
{
    namespace
    {
        // ActivateWindow：显示、激活并等待窗口真正成为活动窗口——
        // WidgetWithChildrenShortcut 上下文的快捷键只在活动窗口的焦点链路内才
        // 会触发，离屏平台下不显式激活窗口，QTest::keyClick 送达的按键不会被
        // 任何快捷键处理。
        bool ActivateWindow(QWidget* window)
        {
            window->show();
            window->activateWindow();
            window->raise();
            return QTest::qWaitForWindowActive(window, 2000);
        }

        // FindEscapeShortcut：按键序找到 Esc 快捷键对象（本类头文件冻结，不能
        // 新增成员指针，生产代码本身也是用 objectName+findChild 找到它，见
        // WorkbenchHexPane.Panels.cpp 的 RefreshEscapeShortcut）；测试直接复用
        // 同一条查找逻辑验证"确实只有一个 Esc 快捷键、且初始是禁用的"。
        QShortcut* FindEscapeShortcut(QWidget* pane)
        {
            for (QShortcut* shortcut : pane->findChildren<QShortcut*>(Qt::FindDirectChildrenOnly))
            {
                if (shortcut->key() == QKeySequence(Qt::Key_Escape))
                {
                    return shortcut;
                }
            }
            return nullptr;
        }

        // F1：openFind/closeFindBar 本身——显隐、焦点转移、Esc 快捷键的启用
        // 状态随之同步。
        void TestOpenCloseFindBarBasic()
        {
            ks::ui::WorkbenchHexPane pane;
            pane.resize(900, 420);
            WPJ5_CHECK(ActivateWindow(&pane));

            QShortcut* escapeShortcut = FindEscapeShortcut(&pane);
            WPJ5_CHECK_NOTE(escapeShortcut != nullptr, "必须恰好存在一个 Esc 快捷键");
            WPJ5_CHECK_NOTE(escapeShortcut != nullptr && !escapeShortcut->isEnabled(), "查找条关闭时 Esc 必须禁用");
            WPJ5_CHECK(!pane.findBar()->isVisible());

            pane.openFind();
            WPJ5_CHECK(pane.findBar()->isVisible());
            WPJ5_CHECK_NOTE(
                escapeShortcut != nullptr && escapeShortcut->isEnabled(), "查找条打开后 Esc 必须启用");

            pane.canvas()->setFocus(Qt::OtherFocusReason);
            QApplication::processEvents();
            pane.closeFindBar();
            WPJ5_CHECK(!pane.findBar()->isVisible());
            WPJ5_CHECK_NOTE(
                escapeShortcut != nullptr && !escapeShortcut->isEnabled(), "查找条关闭后 Esc 必须重新禁用");
        }

        // F2：真实按键——焦点在画布上，Esc 关闭已打开的查找条，焦点回到画布；
        // 查找条关着时按 Esc 不会被吞掉，落到画布自己的处理（折叠选区）。
        void TestEscapeKeyClosesFindBarAndFallsThroughWhenClosed()
        {
            ks::ui::WorkbenchHexPane pane;
            pane.resize(900, 420);
            pane.setAddressSpace(0, 0xFFF, "find-escape");
            WPJ5_CHECK(ActivateWindow(&pane));

            pane.openFind();
            pane.canvas()->setFocus(Qt::OtherFocusReason);
            QApplication::processEvents();
            WPJ5_CHECK(QApplication::focusWidget() == pane.canvas());

            QTest::keyClick(pane.canvas(), Qt::Key_Escape);
            QApplication::processEvents();
            WPJ5_CHECK_NOTE(!pane.findBar()->isVisible(), "Esc 必须真正关闭查找条（经真实快捷键，不是直接调用方法）");
            WPJ5_CHECK(QApplication::focusWidget() == pane.canvas());

            // 查找条已关：选中一段范围，再按 Esc 应该折叠选区（落到画布自己的
            // keyPressEvent），不是什么都不发生、也不是意外地重新打开查找条。
            pane.canvas()->setCaretAddress(0x10, false, false);
            pane.canvas()->setCaretAddress(0x20, true, false);
            WPJ5_CHECK(pane.canvas()->selectedRange().has_value()
                && pane.canvas()->selectedRange()->first != pane.canvas()->selectedRange()->last);
            QTest::keyClick(pane.canvas(), Qt::Key_Escape);
            QApplication::processEvents();
            WPJ5_CHECK_NOTE(!pane.findBar()->isVisible(), "查找条关闭时 Esc 不应该意外打开它");
            WPJ5_CHECK_NOTE(
                pane.canvas()->selectedRange().has_value()
                    && pane.canvas()->selectedRange()->first == pane.canvas()->selectedRange()->last,
                "查找条关闭时 Esc 必须落到画布自己的处理（折叠选区）");
        }

        // F3：F3 没打开查找条时先打开；输入框为空就停在"打开"这一步，不发起
        // 搜索。
        void TestF3OpensWhenClosedAndStaysIfEmpty()
        {
            ks::ui::WorkbenchHexPane pane;
            pane.resize(900, 420);
            pane.setAddressSpace(0, 0xFFF, "find-f3-open");
            WPJ5_CHECK(ActivateWindow(&pane));

            pane.canvas()->setFocus(Qt::OtherFocusReason);
            QApplication::processEvents();
            WPJ5_CHECK(!pane.findBar()->isVisible());

            QSignalSpy matchSpy(pane.findBar(), &ks::ui::HexFindBar::matchFound);
            QTest::keyClick(pane.canvas(), Qt::Key_F3);
            QApplication::processEvents();
            WPJ5_CHECK_NOTE(pane.findBar()->isVisible(), "F3 必须在查找条关闭时先打开它");
            WPJ5_CHECK_NOTE(matchSpy.count() == 0, "输入框为空时不应该发起任何搜索");
        }

        // F4：输入框里有文字、焦点在画布上，F3/Shift+F3 直接触发搜索（不需要
        // 先手动点开查找条），命中后信号 matchFound 真的发出。
        void TestF3TriggersRealSearchWhenPatternPresent()
        {
            ks::ui::WorkbenchHexPane pane;
            pane.resize(900, 420);
            pane.setAddressSpace(0, 0xFFF, "find-f3-search");
            // 纯静态数据，不经过任何管线，只用来验证查找条本身真的被触发并能
            // 搜到东西——查找范围=基线窗口，这里直接用 overlay 载入一段已知
            // 内容的基线，不依赖真实页提供者。
            std::vector<std::uint8_t> bytes(256, 0);
            bytes[0x20] = 0xAB;
            const ksword::memwb::BaselineLoadStatus loadStatus = pane.overlay().LoadBaseline(
                "find-f3-search-id", 0, bytes, std::vector<std::uint8_t>(bytes.size(), 1));
            WPJ5_CHECK(loadStatus == ksword::memwb::BaselineLoadStatus::Ok);
            WPJ5_CHECK(ActivateWindow(&pane));

            pane.findBar()->setPatternText(QStringLiteral("AB"));
            pane.canvas()->setFocus(Qt::OtherFocusReason);
            QApplication::processEvents();

            QSignalSpy matchSpy(pane.findBar(), &ks::ui::HexFindBar::matchFound);
            QTest::keyClick(pane.canvas(), Qt::Key_F3);
            WPJ5_CHECK_NOTE(
                PumpUntil([&]() { return matchSpy.count() >= 1; }, 3000),
                "查找范围=基线窗口：F3 必须真的搜到载入基线里的 0xAB");
        }

        // F5：画布内容变化（contentChanged）必须经 onCanvasContentChanged 转发给
        // findBar_->dataChanged()，让陈旧的查找高亮被清掉——先建立一个真实命中
        // 的高亮，再在别处暂存一个字节触发 contentChanged，核对高亮确实被清空。
        // 这条直接堵住"画布内容变了，查找结果却原地不动"这类陈旧结果残留。
        void TestContentChangeInvalidatesFindHighlights()
        {
            ks::ui::WorkbenchHexPane pane;
            pane.resize(900, 420);
            pane.setAddressSpace(0, 0xFFF, "find-invalidate");
            std::vector<std::uint8_t> bytes(256, 0);
            bytes[0x20] = 0xCD;
            const ksword::memwb::BaselineLoadStatus loadStatus = pane.overlay().LoadBaseline(
                "find-invalidate-id", 0, bytes, std::vector<std::uint8_t>(bytes.size(), 1));
            WPJ5_CHECK(loadStatus == ksword::memwb::BaselineLoadStatus::Ok);
            WPJ5_CHECK(ActivateWindow(&pane));

            pane.findBar()->setPatternText(QStringLiteral("CD"));
            pane.canvas()->setFocus(Qt::OtherFocusReason);
            QApplication::processEvents();

            QSignalSpy matchSpy(pane.findBar(), &ks::ui::HexFindBar::matchFound);
            QTest::keyClick(pane.canvas(), Qt::Key_F3);
            WPJ5_CHECK(PumpUntil([&]() { return matchSpy.count() >= 1; }, 3000));
            WPJ5_CHECK_NOTE(pane.findBar()->highlightActive(), "命中之后查找条必须处于有高亮的状态");

            // 在别处（与命中无关的地址）暂存一个字节，触发画布 contentChanged。
            // 本测试没有接真实页提供者，画布自己的页缓存从未被填过，直接走
            // canvas_->stageBytes 会被"屏幕上看不到值"这条预检拒绝（那条预检
            // 查的是页缓存，不是 overlay 的基线）；改为直接对 overlay 暂存
            // （只要求落在 overlay 自己的基线窗口内，本测试已经整段 LoadBaseline
            // 过），再调用 canvas_->notifyOverlayChanged() 模拟"宿主绕过画布
            // 直接改了叠加层"这一officially 支持的路径（HexCanvas.h"四之二"）。
            const ksword::memwb::StageStatus stageStatus = pane.overlay().Stage(0x50, {0x11});
            WPJ5_CHECK(stageStatus == ksword::memwb::StageStatus::Ok);
            pane.canvas()->notifyOverlayChanged();
            QApplication::processEvents(); // contentChanged 是排队发出的，需要跑一轮事件循环

            WPJ5_CHECK_NOTE(
                !pane.findBar()->highlightActive(),
                "画布内容变化之后，陈旧的查找高亮必须被 dataChanged() 清掉，不能继续显示旧命中");
        }
        // F6（独立审核 wave3 review-wpJ5.md §1.3 夹具缺口补测）：F3（正向）与
        // Shift+F3（反向）必须真的对应 findNext()/findPrevious()，不能被颠倒。
        // 载入两个命中（0x20 与 0x40），把插入点钉在严格位于两者之间的 0x30，
        // 分别按 F3 与 Shift+F3，核对真实命中地址落在期望的那一侧。既有的两个
        // F3 测试都只构造了一个命中，正向/反向结果相同，测不出方向颠倒。
        void TestShiftF3SearchesBackwardF3SearchesForward()
        {
            ks::ui::WorkbenchHexPane pane;
            pane.resize(900, 420);
            pane.setAddressSpace(0, 0xFFF, "find-direction");
            std::vector<std::uint8_t> bytes(256, 0);
            bytes[0x20] = 0xAB;
            bytes[0x40] = 0xAB;
            const ksword::memwb::BaselineLoadStatus loadStatus = pane.overlay().LoadBaseline(
                "find-direction-id", 0, bytes, std::vector<std::uint8_t>(bytes.size(), 1));
            WPJ5_CHECK(loadStatus == ksword::memwb::BaselineLoadStatus::Ok);
            WPJ5_CHECK(ActivateWindow(&pane));

            pane.findBar()->setPatternText(QStringLiteral("AB"));
            pane.canvas()->setCaretAddress(0x30, false, false); // 严格位于两个命中之间
            pane.canvas()->setFocus(Qt::OtherFocusReason);
            QApplication::processEvents();

            QSignalSpy matchSpy(pane.findBar(), &ks::ui::HexFindBar::matchFound);
            QTest::keyClick(pane.canvas(), Qt::Key_F3); // F3：forward=true
            WPJ5_CHECK_NOTE(
                PumpUntil([&]() { return matchSpy.count() >= 1; }, 3000), "F3 必须真的触发一次搜索");
            if (matchSpy.count() >= 1)
            {
                const QList<QVariant> args = matchSpy.takeLast();
                WPJ5_CHECK_NOTE(
                    args.at(0).toULongLong() == 0x40,
                    QString("F3（正向）从 0x30 出发必须命中后面的 0x40，不是前面的 0x20，实际 0x%1")
                        .arg(args.at(0).toULongLong(), 0, 16));
            }

            matchSpy.clear();
            pane.canvas()->setCaretAddress(0x30, false, false); // 复位，隔离下一次触发
            QTest::keyClick(pane.canvas(), Qt::Key_F3, Qt::ShiftModifier); // Shift+F3：forward=false
            WPJ5_CHECK_NOTE(
                PumpUntil([&]() { return matchSpy.count() >= 1; }, 3000), "Shift+F3 必须真的触发一次搜索");
            if (matchSpy.count() >= 1)
            {
                const QList<QVariant> args = matchSpy.takeLast();
                WPJ5_CHECK_NOTE(
                    args.at(0).toULongLong() == 0x20,
                    QString(
                        "Shift+F3（反向）从 0x30 出发必须命中前面的 0x20，不是后面的 0x40，实际 0x%1"
                        "——这条专门堵住 triggerFind 的 forward/backward 对应关系被颠倒（审核报告 N14）")
                        .arg(args.at(0).toULongLong(), 0, 16));
            }
        }

        // F7（独立审核 wave3 review-wpJ5.md §1.4 夹具缺口补测）：closeFindBar()
        // "若焦点原本在查找条内则还给画布"这条分支，必须在焦点**真的**落在查找
        // 条输入框内时生效——既有的两个相关测试都在按 Esc 之前先手动把焦点挪回
        // 画布，从未把焦点真正留在查找条内再按 Esc，测不出"焦点捕获时机被移到
        // deactivate()/hide() 之后"这类真实时序缺陷（那时查找条已经隐藏，
        // QApplication::focusWidget() 已经不是查找条内的控件了）。
        void TestCloseFindBarReturnsFocusWhenFocusWasInsideFindBar()
        {
            ks::ui::WorkbenchHexPane pane;
            pane.resize(900, 420);
            pane.setAddressSpace(0, 0xFFF, "find-focus-return");
            WPJ5_CHECK(ActivateWindow(&pane));

            pane.openFind();
            pane.findBar()->lineEdit()->setFocus(Qt::OtherFocusReason);
            QApplication::processEvents();
            WPJ5_CHECK_NOTE(
                QApplication::focusWidget() == pane.findBar()->lineEdit(),
                "前提：焦点真的落在查找条输入框内（不是像既有测试那样提前挪回画布）");

            pane.closeFindBar();
            QApplication::processEvents();
            WPJ5_CHECK_NOTE(!pane.findBar()->isVisible(), "closeFindBar 必须隐藏查找条");
            WPJ5_CHECK_NOTE(
                QApplication::focusWidget() == pane.canvas(),
                "焦点原本在查找条输入框内时，关闭后必须还给画布（审核报告 N15）——"
                "若焦点捕获时机被移到 deactivate()/hide() 之后才读，此刻查找条已经"
                "隐藏，focusWidget() 已经不是查找条内的控件，这条分支会静默失效");
        }
    }

    void RunFindTests()
    {
        TestOpenCloseFindBarBasic();
        TestEscapeKeyClosesFindBarAndFallsThroughWhenClosed();
        TestF3OpensWhenClosedAndStaysIfEmpty();
        TestF3TriggersRealSearchWhenPatternPresent();
        TestContentChangeInvalidatesFindHighlights();
        TestShiftF3SearchesBackwardF3SearchesForward();
        TestCloseFindBarReturnsFocusWhenFocusWasInsideFindBar();
    }
}
