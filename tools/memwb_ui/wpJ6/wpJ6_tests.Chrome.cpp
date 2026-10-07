// ============================================================
// wpJ6_tests.Chrome.cpp
// 作用：真窗口反馈里"控件意义不明/样式被污染"那一类问题的回归：
//   1) 写入模式胶囊（WriteModeSwitch）：两半都带文字（即时/暂存），悬停说明逐半边不同；
//      主程序全局 QToolButton 样式（带边框、悬停整块填强调色、`!important`）不得改变胶囊的外观——
//      真窗口里 ⚡ 那一半曾被画成一块溢出胶囊的蓝色方块。做法是差分：同一个胶囊在"无全局按钮样式"
//      与"全局按钮样式 + 悬停"两种环境下渲染，像素必须完全相同。
//   2) 地址栏右侧三个钮（重读/实时刷新/展开侧栏）：宽窗口带文字标签，窄窗口退成纯图标且恢复后文字回来。
// 入口：RunChromeTests（由 wpJ6_main.cpp 调用）。
// ============================================================
#include "wpJ6_common.h"

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WriteModeSwitch.h"
#include "../../../Ksword5.1/Ksword5.1/theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QFontMetrics>
#include <QImage>
#include <QToolButton>

#include <algorithm>

namespace wpj6_test
{
    namespace
    {
        // HostileToolButtonStyle：与 MainWindow.cpp 的 buttonInteractionStyle / theme.h 的 ThemedButtonStyle
        // 同形状的全局规则——带边框的背景、悬停整块填强调色，全部 `!important`，字重 600、内边距 4px 10px。
        // 是真窗口里污染胶囊的那类规则；这里原样复刻（含 !important）来做对照。
        QString HostileToolButtonStyle()
        {
            return QStringLiteral(
                "QPushButton,QToolButton{background-color:#2b6cb0 !important;color:#ffffff !important;"
                "border:2px solid #ff00ff !important;border-radius:3px;padding:4px 10px;font-weight:600;}"
                "QPushButton:hover,QToolButton:hover{background-color:#00ff00 !important;color:#000000 !important;"
                "border-color:#00ff00 !important;}"
                "QPushButton:pressed,QToolButton:pressed{background-color:#ff0000 !important;border-color:#ff0000 !important;}")
                + KswordTheme::ThemedButtonStyle();
        }

        // RenderCapsule：把一个胶囊渲染成图。hover 为真时给每个内部按钮置 WA_UnderMouse 模拟悬停。
        // 传入：mode 要显示的模式；hover 是否模拟悬停。传出：渲染图（统一成 ARGB32，避免格式差异造成假不等）。
        QImage RenderCapsule(const ksword::memwb::WriteMode mode, const bool hover)
        {
            ks::ui::WriteModeSwitch capsule;
            capsule.setMode(mode);
            capsule.resize(capsule.sizeHint());
            capsule.show();
            PumpFor(40);
            if (hover)
            {
                for (QToolButton* button : capsule.findChildren<QToolButton*>())
                {
                    button->setAttribute(Qt::WA_UnderMouse, true);
                    button->update();
                }
                PumpFor(40);
            }
            QImage image = capsule.grab().toImage().convertToFormat(QImage::Format_ARGB32);
            capsule.hide();
            return image;
        }

        // TestWriteModeCapsule：胶囊的文字、逐半边悬停说明、不受全局按钮样式污染。
        void TestWriteModeCapsule()
        {
            {
                ks::ui::WriteModeSwitch capsule;
                const QList<QToolButton*> halves = capsule.findChildren<QToolButton*>();
                WPJ6_CHECK_NOTE(halves.size() == 2, QStringLiteral("胶囊应恰有两个半边按钮，实际 %1").arg(halves.size()));
                if (halves.size() == 2)
                {
                    // 文字：图标旁边必须有文字，且两半不同（用户反馈"两个图标意义不明"）。
                    WPJ6_CHECK_NOTE(!halves[0]->text().trimmed().isEmpty() && !halves[1]->text().trimmed().isEmpty(),
                        QStringLiteral("两个半边都必须带文字标签"));
                    WPJ6_CHECK(halves[0]->text() != halves[1]->text());
                    WPJ6_CHECK(halves[0]->toolButtonStyle() == Qt::ToolButtonTextBesideIcon);
                    // 悬停说明：逐半边，互不相同，且都不是空串。
                    WPJ6_CHECK_NOTE(!halves[0]->toolTip().isEmpty() && !halves[1]->toolTip().isEmpty(),
                        QStringLiteral("两个半边都必须有悬停说明"));
                    WPJ6_CHECK_NOTE(halves[0]->toolTip() != halves[1]->toolTip(),
                        QStringLiteral("两个半边的悬停说明必须各讲各的，不能共用一句"));
                }
                // 建议宽度必须放得下两半的图标 + 文字：两半都比"只有图标"时宽。
                WPJ6_CHECK_NOTE(capsule.sizeHint().width() > 2 * 28,
                    QStringLiteral("带文字后胶囊的建议宽度应大于旧的纯图标宽度 56，实际 %1").arg(capsule.sizeHint().width()));
                // 点击不得自己翻转高亮：切换请不请得动由调用方说了算，只发请求信号。
                int requests = 0;
                QObject::connect(&capsule, &ks::ui::WriteModeSwitch::modeToggleRequested,
                    [&requests](ksword::memwb::WriteMode) { ++requests; });
                capsule.setMode(ksword::memwb::WriteMode::Immediate);
                if (halves.size() == 2)
                {
                    // 第二个孩子是"暂存"半边（构造顺序，wpG 的既有测试同样依赖它）。
                    halves[1]->click();
                    WPJ6_CHECK_NOTE(requests == 1, QStringLiteral("点非当前半边应恰好发一次请求，实际 %1").arg(requests));
                    WPJ6_CHECK_NOTE(capsule.mode() == ksword::memwb::WriteMode::Immediate,
                        QStringLiteral("请求被拒（调用方没回写）时高亮必须保持原样"));
                    WPJ6_CHECK_NOTE(halves[0]->isChecked() && !halves[1]->isChecked(),
                        QStringLiteral("点击不得自己翻转半边按钮的选中态"));

                    // 回写：setMode 之后两半的选中态都要跟着走（只测即时半边会漏掉"暂存半边永远不选中"）。
                    capsule.setMode(ksword::memwb::WriteMode::StagedThenApply);
                    WPJ6_CHECK_NOTE(!halves[0]->isChecked() && halves[1]->isChecked(),
                        QStringLiteral("setMode(暂存) 后应是暂存半边选中、即时半边不选中"));
                    capsule.setMode(ksword::memwb::WriteMode::Immediate);
                    WPJ6_CHECK_NOTE(halves[0]->isChecked() && !halves[1]->isChecked(),
                        QStringLiteral("setMode(即时) 后应回到即时半边选中"));

                    // 建议宽度下界：每一半至少放得下图标（16）加较宽的那个文字，两半相加。
                    // 只断言"大于旧的纯图标宽度"抓不到"只给一半宽度"的缺陷（减半后仍大于 56）。
                    const QFontMetrics metrics(capsule.font());
                    const int widerText = std::max(
                        metrics.horizontalAdvance(halves[0]->text()), metrics.horizontalAdvance(halves[1]->text()));
                    WPJ6_CHECK_NOTE(capsule.sizeHint().width() >= 2 * (16 + widerText),
                        QStringLiteral("建议宽度 %1 应不小于两半各 (图标16+文字%2) 之和").arg(capsule.sizeHint().width()).arg(widerText));
                }
            }

            // 差分：无全局按钮样式 vs 全局按钮样式（含悬停），渲染必须逐像素相同。
            for (const auto mode : {ksword::memwb::WriteMode::Immediate, ksword::memwb::WriteMode::StagedThenApply})
            {
                const QString modeName = mode == ksword::memwb::WriteMode::Immediate ? QStringLiteral("即时") : QStringLiteral("暂存");
                qApp->setStyleSheet(QString());
                const QImage plain = RenderCapsule(mode, false);
                qApp->setStyleSheet(HostileToolButtonStyle());
                const QImage styled = RenderCapsule(mode, false);
                const QImage styledHover = RenderCapsule(mode, true);
                qApp->setStyleSheet(QString());
                WPJ6_CHECK_NOTE(!plain.isNull() && plain.size() == styled.size(),
                    QStringLiteral("%1：两种环境下胶囊尺寸应相同（%2x%3 vs %4x%5）")
                        .arg(modeName).arg(plain.width()).arg(plain.height()).arg(styled.width()).arg(styled.height()));
                WPJ6_CHECK_NOTE(plain == styled,
                    QStringLiteral("%1：全局按钮样式改变了胶囊的渲染（半边按钮必须不吃 QSS）").arg(modeName));
                // 悬停：只允许文字提亮（颜色变化），不允许出现样式引擎画的底板/边框；
                // 用"有颜色的像素分布"粗判：悬停图与静止图在 #00ff00 / #ff00ff / #ff0000 三种陷阱色上都没有像素。
                int trapPixels = 0;
                for (int y = 0; y < styledHover.height(); ++y)
                {
                    for (int x = 0; x < styledHover.width(); ++x)
                    {
                        const QRgb rgb = styledHover.pixel(x, y);
                        const int r = qRed(rgb);
                        const int g = qGreen(rgb);
                        const int b = qBlue(rgb);
                        const bool green = g > 200 && r < 60 && b < 60;
                        const bool magenta = r > 200 && b > 200 && g < 60;
                        const bool red = r > 200 && g < 60 && b < 60;
                        if (green || magenta || red)
                        {
                            ++trapPixels;
                        }
                    }
                }
                WPJ6_CHECK_NOTE(trapPixels == 0,
                    QStringLiteral("%1：悬停下出现 %2 个全局按钮样式陷阱色像素（样式引擎仍在给半边按钮画底板/边框）")
                        .arg(modeName).arg(trapPixels));
            }
        }

        // FindButtonByTip：在视图里按悬停说明里的关键词找 QToolButton（含隐藏的）。
        QToolButton* FindButtonByTip(ks::ui::MemoryWorkbenchView& view, const QString& keyword)
        {
            for (QToolButton* button : view.findChildren<QToolButton*>())
            {
                if (button->toolTip().contains(keyword))
                {
                    return button;
                }
            }
            return nullptr;
        }

        // TestAddressRowLabels：重读/实时刷新/展开侧栏三个控件的文字标签随宽度显隐。
        void TestAddressRowLabels()
        {
            Harness harness;
            harness.AttachProcess();
            PumpUntil([]() { return true; }, 10);
            auto* view = harness.view.get();
            QToolButton* reread = FindButtonByTip(*view, QStringLiteral("重读窗口"));
            QToolButton* sidebar = FindButtonByTip(*view, QStringLiteral("展开侧栏"));
            QCheckBox* live = view->findChild<QCheckBox*>();
            WPJ6_CHECK_NOTE(reread != nullptr && sidebar != nullptr && live != nullptr,
                QStringLiteral("应找到重读钮/展开侧栏钮/实时刷新复选框"));
            if (reread == nullptr || sidebar == nullptr || live == nullptr)
            {
                return;
            }

            // 宽窗口：三个控件都带文字，且重读钮宽度大于纯图标方块（文字真的占了宽度）。
            view->resize(900, 700);
            view->show();
            PumpFor(80);
            WPJ6_CHECK_NOTE(reread->toolButtonStyle() == Qt::ToolButtonTextBesideIcon && !reread->text().isEmpty(),
                QStringLiteral("宽窗口下重读钮应带文字"));
            WPJ6_CHECK_NOTE(sidebar->toolButtonStyle() == Qt::ToolButtonTextBesideIcon && !sidebar->text().isEmpty(),
                QStringLiteral("宽窗口下展开侧栏钮应带文字"));
            WPJ6_CHECK_NOTE(!live->text().isEmpty(), QStringLiteral("宽窗口下实时刷新复选框应带文字"));
            WPJ6_CHECK_NOTE(reread->width() > 28, QStringLiteral("带文字的重读钮应比 28px 方块宽，实际 %1").arg(reread->width()));
            const QString liveText = live->text();

            // 阈值两侧：600px（> 560）仍带文字，540px（< 560）退成纯图标。只探 900 与 400 抓不到
            // "阈值写成别的常量（例如侧栏折叠阈值 760）"这类缺陷。
            view->resize(600, 700);
            PumpFor(80);
            WPJ6_CHECK_NOTE(reread->toolButtonStyle() == Qt::ToolButtonTextBesideIcon && !live->text().isEmpty(),
                QStringLiteral("600px 仍高于文字标签阈值，应带文字"));
            view->resize(540, 700);
            PumpFor(80);
            WPJ6_CHECK_NOTE(reread->toolButtonStyle() == Qt::ToolButtonIconOnly && live->text().isEmpty(),
                QStringLiteral("540px 低于文字标签阈值，应退成纯图标"));
            view->resize(900, 700);
            PumpFor(80);

            // 窄窗口：退成纯图标（方块宽度），复选框去掉文字，悬停说明仍在。
            view->resize(400, 700);
            PumpFor(80);
            WPJ6_CHECK_NOTE(reread->toolButtonStyle() == Qt::ToolButtonIconOnly,
                QStringLiteral("窄窗口下重读钮应退成纯图标"));
            WPJ6_CHECK_NOTE(sidebar->toolButtonStyle() == Qt::ToolButtonIconOnly,
                QStringLiteral("窄窗口下展开侧栏钮应退成纯图标"));
            WPJ6_CHECK_NOTE(live->text().isEmpty(), QStringLiteral("窄窗口下实时刷新复选框应去掉文字"));
            WPJ6_CHECK_NOTE(reread->width() <= 28 + 2, QStringLiteral("纯图标的重读钮应回到方块宽度，实际 %1").arg(reread->width()));
            WPJ6_CHECK(!reread->toolTip().isEmpty() && !live->toolTip().isEmpty());

            // 变宽恢复：文字回来。
            view->resize(900, 700);
            PumpFor(80);
            WPJ6_CHECK_NOTE(reread->toolButtonStyle() == Qt::ToolButtonTextBesideIcon,
                QStringLiteral("恢复宽度后重读钮的文字应回来"));
            WPJ6_CHECK_NOTE(live->text() == liveText,
                QStringLiteral("恢复宽度后复选框文字应回到 '%1'，实际 '%2'").arg(liveText, live->text()));
            view->hide();
        }
    }

    void RunChromeTests()
    {
        TestWriteModeCapsule();
        TestAddressRowLabels();
    }
}
