// ============================================================
// wpJ6_tests.Narrow.cpp
// 作用：MemoryWorkbenchView 窄宽度"硬下限"回归（接在 wpJ6_tests.Visual.cpp 的 420/360 两档之后）。
//       视图本身的硬下限早已由 root 的 SetNoConstraint 解决（探到 160px 都接受）；残留的是侧栏：
//       用户在窄窗口里手动展开侧栏后，侧栏容器的 minimumSizeHint（约 244px）让 QSplitter 不肯把它
//       压窄，十六进制画布被挤到 1px。（只设最大宽度也不够，QSplitter 沿用已记住的尺寸，必须显式
//       setSizes——测试里"刚从隐藏变可见"与"已可见时缩窄"两条路径都覆盖。）
//       本文件核对：窄宽度下手动展开侧栏，画布仍拿到可用宽度（主体留底）；宽屏不加任何上限；
//       窗口重新变宽后上限解除且侧栏回到偏好宽度；内嵌模式不受影响。
// 入口：RunNarrowTests（由 wpJ6_main.cpp 调用）。
// ============================================================
#include "wpJ6_common.h"

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/HexCanvas.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchHexPane.h"

#include <QFontMetrics>
#include <QLabel>
#include <QSplitter>
#include <QToolButton>

#include <algorithm>

namespace wpj6_test
{
    namespace
    {
        // ClickSidebarExpandButton：点"展开侧栏"钮（窄宽度下侧栏被自动折叠后唯一的展开入口）。
        bool ClickSidebarExpandButton(ks::ui::MemoryWorkbenchView& view)
        {
            for (QToolButton* button : view.findChildren<QToolButton*>())
            {
                if (button->toolTip().contains(QStringLiteral("展开侧栏")) && !button->isHidden())
                {
                    button->click();
                    return true;
                }
            }
            return false;
        }

        // RunSidebarScenario：给定一串窗口宽度，依次缩窄/变宽，核对侧栏手动展开后的各项不变式。
        // 两条路径都要走到：
        // - 路径 A（900→360…）：窄宽度下侧栏先被自动折叠，手动展开时是"刚从隐藏变可见"；
        // - 路径 B（900→700→360…）：先在 700px（侧栏放得下偏好宽度，已落成 300）手动展开，之后再缩窄时
        //   侧栏是"已经可见、已落过偏好宽度"——此时窗口变宽后上限解除，侧栏必须回到偏好宽度，
        //   靠的是上限解除时复位 sidebarWidthApplied_（路径 A 里该标志本来就是假，测不出）。
        void RunSidebarScenario(const QList<int>& widths, const QString& label)
        {
            Harness harness;
            harness.AttachProcess();
            PumpUntil([]() { return true; }, 10);
            auto* view = harness.view.get();
            auto* splitter = view->mainSplitterForTest();
            auto* hexPane = view->hexPaneForTest();
            WPJ6_CHECK(splitter != nullptr && splitter->count() == 2);
            if (splitter == nullptr || splitter->count() != 2)
            {
                return;
            }
            QWidget* sidebar = splitter->widget(1);

            // 宽屏：侧栏默认展开，不加任何宽度上限，宽度是偏好宽度。
            view->resize(900, 700);
            view->show();
            PumpFor(80);
            WPJ6_CHECK(!sidebar->isHidden());
            WPJ6_CHECK_NOTE(sidebar->maximumWidth() == QWIDGETSIZE_MAX, QStringLiteral("%1：宽屏下侧栏不应有宽度上限").arg(label));
            WPJ6_CHECK_NOTE(
                splitter->sizes().at(1) == 300,
                QStringLiteral("%1：宽屏下侧栏应是偏好宽度 300，实际 %2").arg(label).arg(splitter->sizes().at(1)));

            for (const int narrowWidth : widths)
            {
                if (narrowWidth >= 1000)
                {
                    // 窗口重新变宽：上限解除，侧栏回到偏好宽度，画布也回到宽屏的宽度。
                    view->resize(narrowWidth, 700);
                    PumpFor(100);
                    WPJ6_CHECK_NOTE(sidebar->maximumWidth() == QWIDGETSIZE_MAX, QStringLiteral("%1：变宽后侧栏上限应解除").arg(label));
                    WPJ6_CHECK_NOTE(
                        splitter->sizes().at(1) == 300,
                        QStringLiteral("%1：变宽后侧栏应回到偏好宽度 300，实际 %2").arg(label).arg(splitter->sizes().at(1)));
                    WPJ6_CHECK(hexPane->canvas()->width() > 300);
                    continue;
                }
                view->resize(narrowWidth, 700);
                PumpFor(80);
                WPJ6_CHECK_NOTE(view->width() == narrowWidth, QStringLiteral("窗口应接受宽度 %1").arg(narrowWidth));
                if (sidebar->isHidden())
                {
                    WPJ6_CHECK_NOTE(ClickSidebarExpandButton(*view), QStringLiteral("窄宽度下应有可点的展开侧栏钮"));
                    PumpFor(80);
                }
                WPJ6_CHECK_NOTE(!sidebar->isHidden(), QStringLiteral("手动展开后侧栏应可见"));
                WPJ6_CHECK_NOTE(sidebar->maximumWidth() < QWIDGETSIZE_MAX, QStringLiteral("窄宽度下手动展开的侧栏应有宽度上限"));
                const int canvasWidth = hexPane->canvas()->width();
                const int wantedCanvas = (narrowWidth - 12) * 2 / 5;
                WPJ6_CHECK_NOTE(
                    canvasWidth >= wantedCanvas,
                    QStringLiteral("宽度 %1 手动展开侧栏后画布宽 %2 应不小于 %3（主体留底，不能被挤成一条缝）"
                                   " [splitter=%4 sizes=%5/%6 sidebar max=%7 minHint=%8 minW=%9 stackW=%10 handle=%11]")
                        .arg(narrowWidth).arg(canvasWidth).arg(wantedCanvas)
                        .arg(splitter->width()).arg(splitter->sizes().at(0)).arg(splitter->sizes().at(1))
                        .arg(sidebar->maximumWidth()).arg(sidebar->minimumSizeHint().width()).arg(sidebar->minimumWidth())
                        .arg(splitter->widget(0)->width()).arg(splitter->handleWidth()));
                // 侧栏拿到的是剩下的宽度，不是 0：展开了就该看得见一部分。
                WPJ6_CHECK_NOTE(
                    sidebar->width() >= 60,
                    QStringLiteral("宽度 %1 下侧栏宽 %2 不应被压没").arg(narrowWidth).arg(sidebar->width()));
                // 700px 放得下偏好宽度 300 + 主体 300：侧栏应是偏好宽度（上限在这个宽度不起作用）。
                if (narrowWidth == 700)
                {
                    WPJ6_CHECK_NOTE(
                        splitter->sizes().at(1) == 300,
                        QStringLiteral("%1：700px 下侧栏应落成偏好宽度 300，实际 %2").arg(label).arg(splitter->sizes().at(1)));
                }
            }
            view->hide();
        }

        void TestSidebarManualOpenKeepsMainBody()
        {
            // 路径 A：窄宽度下侧栏先被自动折叠，手动展开时是"刚从隐藏变可见"。
            RunSidebarScenario({360, 300, 240, 1000}, QStringLiteral("路径A"));
            // 路径 B：先在 700px 展开（落过偏好宽度），再缩窄、再变宽。
            RunSidebarScenario({700, 360, 300, 240, 1000}, QStringLiteral("路径B"));
        }

        // 内嵌模式：侧栏恒隐藏，不应留下任何宽度上限，窄宽度下画布占满。
        void TestEmbeddedHasNoSidebarCap()
        {
            Harness harness;
            harness.AttachProcess();
            PumpUntil([]() { return true; }, 10);
            auto* view = harness.view.get();
            view->setEmbeddedProcessMode(true);
            view->resize(300, 700);
            view->show();
            PumpFor(80);
            QWidget* sidebar = view->mainSplitterForTest()->widget(1);
            WPJ6_CHECK(sidebar->isHidden());
            WPJ6_CHECK(sidebar->maximumWidth() == QWIDGETSIZE_MAX);
            WPJ6_CHECK(view->hexPaneForTest()->canvas()->width() >= 280);
            view->hide();
        }
    }

    namespace
    {
        // ---- 状态条摘要段：窄宽度下任何一段都不得被压成空白 ----
        // 缺陷：通道·范围/读取结果/窗口范围三个标签原先只有 setMinimumWidth(1)，窗口一窄就被布局压到
        // 1px（"R3 · 进程"在 240px 时整段消失）或被硬裁成半截（360px 时只剩 "R"、"结论是当前不可"），
        // 与主题无关（深浅两套截图一致）。要求：
        //  ① 通道·范围段（短且有界）任何宽度下都完整显示；
        //  ② 读取结果/窗口范围段（可能很长）窄时省略成"…"，宽度不得小于一个省略号，绝不是空白；
        //  ③ 无论怎么省略，标签的 text() 始终是调用方给的原文（运行期整句翻译与 S1 测试按它精确匹配）。
        void TestStatusBarSegmentsNeverBlank()
        {
            const QString channelText = QStringLiteral("R3 · 进程");
            const QString readText = QStringLiteral("⚠ 读取失败（3 处可重试）");
            const QString windowText = QStringLiteral("窗口 0x0–0x1FFF");
            const QString writeText = QStringLiteral("已写入 int3（0x00000000000000B0）");

            Harness harness;
            harness.AttachProcess();
            PumpUntil([]() { return true; }, 10);
            auto* view = harness.view.get();
            view->resize(900, 700);
            view->show();
            PumpFor(100);
            auto* status = view->statusBarForTest();
            status->setChannelScopeText(channelText);
            status->setReadResultText(QStringLiteral("读取失败（3 处可重试）"), true);
            status->setWindowRangeText(windowText);
            status->setWriteResultText(writeText);

            int elidedChecked = 0;   // 做过"绘制成右省略"对照的次数：必须 > 0，否则对照从来没执行过
            for (const int width : {900, 600, 480, 420, 360, 300, 240, 200})
            {
                view->resize(width, 700);
                PumpFor(80);
                int checkedSegments = 0;
                for (QLabel* label : status->findChildren<QLabel*>())
                {
                    if (!label->isVisibleTo(status) || label->text().isEmpty() || label->text() == QStringLiteral("|"))
                    {
                        continue;
                    }
                    const QFontMetrics metrics(label->font());
                    const bool isChannel = label->text() == channelText;
                    const bool isRead = label->text() == readText;
                    const bool isWindow = label->text() == windowText;
                    if (!isChannel && !isRead && !isWindow)
                    {
                        continue;   // 保护徽章/写入结果段/chip 不在本测试范围（写入结果段有自己的省略逻辑与测试）
                    }
                    ++checkedSegments;
                    // ③ text() 必须原样。
                    WPJ6_CHECK_NOTE(
                        label->text() == (isChannel ? channelText : isRead ? readText : windowText),
                        QStringLiteral("宽度 %1：段的 text() 被改动成了 %2").arg(width).arg(label->text()));
                    const int fullWidth = metrics.horizontalAdvance(label->text());
                    const int ellipsisWidth = metrics.horizontalAdvance(QChar(0x2026));
                    if (isChannel)
                    {
                        // ① 通道·范围段完整显示。
                        WPJ6_CHECK_NOTE(
                            label->width() >= fullWidth,
                            QStringLiteral("宽度 %1：通道·范围段宽 %2 小于文字宽 %3（被压缩/裁切）").arg(width).arg(label->width()).arg(fullWidth));
                    }
                    else
                    {
                        // ② 长段至少放得下一个省略号，不得是空白。
                        WPJ6_CHECK_NOTE(
                            label->width() >= std::min(fullWidth, ellipsisWidth),
                            QStringLiteral("宽度 %1：段 '%2' 宽 %3 放不下一个省略号（%4px），渲染出来是空白")
                                .arg(width).arg(label->text().left(8)).arg(label->width()).arg(ellipsisWidth));
                        // ④ 放不下时必须画成"右省略"，而不是被硬裁成半截：拿一个不带选择交互的普通 QLabel，
                        //    装上期望的省略文字、同字体同调色板同尺寸，两张渲染图逐像素相同。
                        //    （普通 QLabel 与 ElidedSegmentLabel 的省略路径都走 QStyle::drawItemText，
                        //    所以不是拿实现跟自己比；选择交互会让 QLabel 改走文本控件绘制，故对照图不设它。）
                        if (fullWidth > label->contentsRect().width())
                        {
                            QLabel reference;
                            reference.setFont(label->font());
                            reference.setPalette(label->palette());
                            reference.setMargin(label->margin());
                            reference.setText(metrics.elidedText(label->text(), Qt::ElideRight, label->contentsRect().width()));
                            reference.resize(label->size());
                            // 统一成同一像素格式再比：QImage::operator== 连格式一起比，已显示/未显示控件的
                            // grab() 格式可能不同（实测像素逐点完全一致却判不等）。
                            const QImage actualImage = label->grab().toImage().convertToFormat(QImage::Format_ARGB32);
                            const QImage expectedImage = reference.grab().toImage().convertToFormat(QImage::Format_ARGB32);
                            if (actualImage != expectedImage)
                            {
                                // 失败时把两张图落盘，方便肉眼比较差在哪。
                                const QString dir = qEnvironmentVariable("MEMWB_OUT");
                                actualImage.save(dir + QStringLiteral("/elide_actual_%1_%2.png").arg(width).arg(isRead ? 'r' : 'w'));
                                expectedImage.save(dir + QStringLiteral("/elide_expected_%1_%2.png").arg(width).arg(isRead ? 'r' : 'w'));
                            }
                            WPJ6_CHECK_NOTE(
                                actualImage == expectedImage,
                                QStringLiteral("宽度 %1：段 '%2' 放不下时没有画成右省略（与期望的省略文字渲染不一致；实际 %3x%4 期望 %5x%6）")
                                    .arg(width).arg(label->text().left(8))
                                    .arg(actualImage.width()).arg(actualImage.height())
                                    .arg(expectedImage.width()).arg(expectedImage.height()));
                            ++elidedChecked;
                        }
                    }
                }
                WPJ6_CHECK_NOTE(checkedSegments == 3, QStringLiteral("宽度 %1：应检查到三个摘要段，实际 %2").arg(width).arg(checkedSegments));
            }
            WPJ6_CHECK_NOTE(elidedChecked >= 4, QStringLiteral("省略绘制对照只执行了 %1 次，窄宽度场景没覆盖到").arg(elidedChecked));
            view->hide();
        }
    }

    void RunNarrowTests()
    {
        TestSidebarManualOpenKeepsMainBody();
        TestEmbeddedHasNoSidebarCap();
        TestStatusBarSegmentsNeverBlank();
    }
}
