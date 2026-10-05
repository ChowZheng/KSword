// wpF_tests_common.cpp
// 作用：wpF_tests_common.h 的实现。

#include "wpF_tests_common.h"

#include "../../../Ksword5.1/Ksword5.1/theme.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QMessageBox>
#include <QPalette>
#include <QStyleFactory>
#include <QTest>
#include <QTimer>

#include <iostream>

namespace wpf_test
{
    int g_checks = 0;
    int g_failures = 0;

    // 记录一条断言：失败时打印位置、表达式与附加说明，方便从输出直接定位。
    void Report(const bool ok, const char* expression, const char* file, const int line, const QString& note)
    {
        ++g_checks;
        if (ok)
        {
            return;
        }
        ++g_failures;
        std::cerr << "FAIL: " << expression << "  (" << file << ":" << line << ")";
        if (!note.isEmpty())
        {
            std::cerr << "  " << note.toStdString();
        }
        std::cerr << std::endl;
    }

    // MakeTarget：组装一个 PatchTarget，三个字段按参数顺序赋值。
    ksword::memwb::PatchTarget MakeTarget(
        const std::uint32_t pid,
        const std::uint64_t processCreateTime100ns,
        const std::uint64_t attachGeneration)
    {
        ksword::memwb::PatchTarget target;
        target.pid = pid;
        target.processCreateTime100ns = processCreateTime100ns;
        target.attachGeneration = attachGeneration;
        return target;
    }

    // CountOpenMessageBoxes：遍历全部顶层窗口，数出可见的 QMessageBox。
    int CountOpenMessageBoxes()
    {
        int count = 0;
        const QWidgetList topLevel = QApplication::topLevelWidgets();
        for (QWidget* widget : topLevel)
        {
            if (qobject_cast<QMessageBox*>(widget) != nullptr && widget->isVisible())
            {
                ++count;
            }
        }
        return count;
    }

    // ClickModalButtonByText：在当前模态对话框里按文字找按钮并点击；找不到则什么都不做，
    // 调用方的后续断言会据此发现问题（而不是让测试自己卡死）。
    void ClickModalButtonByText(const QString& text)
    {
        QWidget* modal = QApplication::activeModalWidget();
        auto* box = qobject_cast<QMessageBox*>(modal);
        if (box == nullptr)
        {
            return;
        }
        const QList<QAbstractButton*> buttons = box->buttons();
        for (QAbstractButton* button : buttons)
        {
            if (button->text() == text)
            {
                button->click();
                return;
            }
        }
    }

    // ActiveModalMessageBoxText：按文字读当前模态框（见头文件注释）。
    QString ActiveModalMessageBoxText()
    {
        const auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        return box != nullptr ? box->text() : QString();
    }

    // PressEscapeOnModal：对当前模态对话框发送一次 Escape 按键。
    void PressEscapeOnModal()
    {
        QWidget* modal = QApplication::activeModalWidget();
        if (modal != nullptr)
        {
            QTest::keyClick(modal, Qt::Key_Escape);
        }
    }

    // PressEnterOnModal：对当前模态对话框发送一次 Enter 按键——Qt 的 QDialog 按键处理会把
    // Return/Enter 转发给"默认按钮"（没有默认按钮时才会去找 AutoDefault 的按钮），所以这
    // 一个按键动作就能验证"谁是默认按钮"，不需要直接 click() 某个具体按钮指针。
    void PressEnterOnModal()
    {
        QWidget* modal = QApplication::activeModalWidget();
        if (modal != nullptr)
        {
            QTest::keyClick(modal, Qt::Key_Return);
        }
    }

    // ArmUnexpectedModalWatchdog：见头文件注释。用 Escape 强制关闭是因为它恒等价于
    // "取消"——调用方只需要照常断言返回值与 CountOpenMessageBoxes()，不需要区分
    // "本来就没弹框"和"弹了框但被这个安全网关掉了"这两种物理上不同、但对断言而言
    // 应该得到同一个失败结果的情形（如果真的误弹了框，取消=返回 false，调用方期望
    // 的 true 断言自然会失败并打印位置，这正是我们要的"可读失败"而不是"卡死"）。
    void ArmUnexpectedModalWatchdog(const int delayMs)
    {
        QTimer::singleShot(delayMs, []() {
            QWidget* modal = QApplication::activeModalWidget();
            if (modal != nullptr)
            {
                QTest::keyClick(modal, Qt::Key_Escape);
            }
        });
    }

    // TriggerPopupMenuActionByText：按文字找当前弹出菜单里的动作并触发；找不到就关掉菜单。
    void TriggerPopupMenuActionByText(const QString& text)
    {
        QWidget* popup = QApplication::activePopupWidget();
        auto* menu = qobject_cast<QMenu*>(popup);
        if (menu == nullptr)
        {
            return;
        }
        const QList<QAction*> actions = menu->actions();
        for (QAction* action : actions)
        {
            if (action->text() == text)
            {
                // QAction::trigger() 只发信号，不会像真实点击那样顺带关掉菜单；
                // exec() 的嵌套事件循环要等菜单真正隐藏才会返回，这里必须显式 close()，
                // 否则 exec() 永远不返回（已实测卡死）。
                action->trigger();
                menu->close();
                return;
            }
        }
        menu->close();
    }

    // ApplyThemeForShots：与主夹具 ApplyTheme 同样的完整调色板构造，独立一份避免跨夹具耦合。
    void ApplyThemeForShots(const bool dark)
    {
        KswordTheme::SetDarkModeEnabled(dark);
        qApp->setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

        QPalette palette;
        palette.setColor(QPalette::Window, KswordTheme::WindowColor());
        palette.setColor(QPalette::WindowText, KswordTheme::TextPrimaryColor());
        palette.setColor(QPalette::Base, KswordTheme::SurfaceColor());
        palette.setColor(QPalette::AlternateBase, KswordTheme::SurfaceAltColor());
        palette.setColor(QPalette::Text, KswordTheme::TextPrimaryColor());
        palette.setColor(QPalette::Button, KswordTheme::SurfaceAltColor());
        palette.setColor(QPalette::ButtonText, KswordTheme::TextPrimaryColor());
        palette.setColor(QPalette::PlaceholderText, KswordTheme::TextSecondaryColor());
        palette.setColor(QPalette::Highlight, KswordTheme::PrimaryAccentColor());
        palette.setColor(QPalette::HighlightedText, KswordTheme::OnAccentColor());
        palette.setColor(QPalette::Mid, KswordTheme::BorderColor());
        palette.setColor(QPalette::Midlight, KswordTheme::BorderStrongColor());
        palette.setColor(QPalette::Dark, KswordTheme::PaletteDarkColor());
        palette.setColor(QPalette::ToolTipBase, KswordTheme::SurfaceAltColor());
        palette.setColor(QPalette::ToolTipText, KswordTheme::TextPrimaryColor());
        palette.setColor(QPalette::Disabled, QPalette::Text, KswordTheme::TextDisabledColor());
        palette.setColor(QPalette::Disabled, QPalette::WindowText, KswordTheme::TextDisabledColor());
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, KswordTheme::TextDisabledColor());
        qApp->setPalette(palette);
    }
}
