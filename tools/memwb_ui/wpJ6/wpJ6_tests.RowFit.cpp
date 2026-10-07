// ============================================================
// wpJ6_tests.RowFit.cpp
// 作用：内存工作台"十六进制画布自适应"（用户反馈"内存编辑器太小，中间的内存编辑区应该自适应大小"）
//       的工作台层回归。画布层的纯函数/锚点/不重读/缩放细节在 tools/memwb_ui/memwb_ui_tests.RowFit.cpp，
//       本文件只验证"装配"：
//   1) 默认值（用户拍板）：构造后、loadSettings 之前画布不是自适应（固定 16 字节，既有夹具行为不变）；
//      loadSettings（设置为空）之后才默认打开自适应；
//   2) 视图宽度 700..2000 依次变化：画布行宽等于按真实视口宽度算出的期望、随视口宽度非递减、
//      放得下时没有横向滚动条（以真实侧栏/解释器占用为准，不硬编码像素）；
//   3) 视图菜单钮（子页签那一行右侧）：只在十六进制子页显示；行宽 自动/8/16/32/48/64 互斥且勾选当前档、
//      分组 1/2/4/8 互斥、字号放大/缩小/恢复（到头置灰）；每项有悬停提示；显式不透明样式且随主题重建；
//      选项真的作用于画布（手选 16 后自适应关闭）；徽标与悬停说明随行宽模式刷新；
//   4) 持久化往返：自动 -> 存 -> 新视图仍自动；手选 32 -> 存 -> 新视图非自动且 32；
//      自动状态保存时不覆盖手选值；分组/字号往返；非权威视图不落盘；字号设置越界退回默认；
//   5) 布局联动：Ctrl+I 隐藏解释器、Ctrl+Shift+B 展开侧栏后画布宽度变了，行宽按新宽度重选；
//   6) 纵向：视图 resize 后画布吃满分割条高度；诊断抽屉展开（持久化过的展开状态）时有最大高度；
//   7) 浅色/深色各一张截图 + 几何断言（右侧空白不超过"下一档与当前档的宽度差"，且小于固定 16 字节时的空白）。
//   8) RunRowFitI18nTests：en-US 下菜单项/徽标/悬停说明都不含汉字（必须排在 RunI18nSmokeTest 之后调用，
//      那时语言已切到 en-US）。
// 入口：RunRowFitTests（排在 RunI18nSmokeTest 之前）、RunRowFitI18nTests（排在 RunI18nSmokeTest 之后），
//       都由 wpJ6_main.cpp 调用。
// QSettings：夹具 main 已把默认格式重定向到 exe 旁的 ini；本文件开头结尾都清掉自己用到的键，
//            并把侧栏/范围/写入模式/子页签恢复成默认，不污染后面的测试。
// ============================================================
#include "wpJ6_common.h"

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/HexCanvas.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/HexCanvasFormat.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/HexViewWidgets.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchDiagnosticsHost.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchHexPane.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchSettings.h"
#include "../../../Ksword5.1/Ksword5.1/theme.h"

#include <QAction>
#include <QActionGroup>
#include <QDir>
#include <QFontMetrics>
#include <QImage>
#include <QKeySequence>
#include <QMenu>
#include <QPixmap>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QStackedWidget>
#include <QString>
#include <QStringList>
#include <QToolButton>

#include <algorithm>
#include <cstdint>
#include <set>
#include <vector>

namespace wpj6_test
{
    namespace
    {
        using ks::ui::HexCanvas;
        using Pane = HexCanvas::ActivePane;
        namespace fmt = ks::ui::hexcanvas_format;

        // kRowWidths：行宽候选集（升序），独立于产品常量，期望值才能抓到产品少一档。
        constexpr int kRowWidths[] = { 8, 16, 32, 48, 64 };

        // ResetPrefs：清掉本文件用到的四个键，并把侧栏/范围/写入模式/子页签恢复成默认
        // （与 wpJ6_tests.Review2Fixes.cpp 的 ResetSettings 同一套默认，避免污染其它测试，
        // 也避免其它测试遗留的值（例如子页签 3）影响本文件）。
        void ResetPrefs()
        {
            using namespace ks::ui::workbench_settings;
            QSettings settings;
            settings.remove(QStringLiteral("memwb/workbench/rowWidthAuto"));
            settings.remove(QStringLiteral("memwb/workbench/bytesPerRow"));
            settings.remove(QStringLiteral("memwb/workbench/groupSize"));
            settings.remove(QStringLiteral("memwb/workbench/hexZoom"));
            settings.sync();
            SaveSidebarVisible(false);
            SaveSidebarWidth(300);
            SaveScope(0U);
            SaveWriteMode(0U);
            SaveSubTab(0);
        }

        // CharWidthOf：画布当前字体下单个等宽字符的像素宽度（与 HexCanvas::rebuildMetrics 同一算法）。
        int CharWidthOf(const HexCanvas& canvas)
        {
            return std::max(1, QFontMetrics(canvas.font()).horizontalAdvance(QLatin1Char('0')));
        }

        // DigitsOf：从画布真实布局反推地址列位数（8 或 16）。
        // 内容宽度 = (RowWidthChars(行宽, 分组, 0) + 地址位数) * 字符宽度，RowWidthChars 对地址位数是线性的。
        int DigitsOf(const HexCanvas& canvas)
        {
            const int contentWidth = canvas.sizeHint().width() - canvas.verticalScrollBar()->sizeHint().width() - 2;
            return contentWidth / CharWidthOf(canvas) - fmt::RowWidthChars(canvas.bytesPerRow(), canvas.groupSize(), 0);
        }

        // ExpectedAuto：自适应行宽的期望值（从小到大扫描，保留最后一个放得下的；一个都放不下是 8）。
        int ExpectedAuto(int viewportWidth, int charWidth, int groupSize, int digits)
        {
            int best = 8;
            for (const int candidate : kRowWidths)
            {
                if (fmt::RowWidthChars(candidate, groupSize, digits) * charWidth <= viewportWidth)
                {
                    best = candidate;
                }
            }
            return best;
        }

        // NextLarger：候选集里比 bytesPerRow 大一档的行宽；已是最大返回 0。
        int NextLarger(int bytesPerRow)
        {
            for (const int candidate : kRowWidths)
            {
                if (candidate > bytesPerRow)
                {
                    return candidate;
                }
            }
            return 0;
        }

        // RightmostInkX：在 top 行以下找与底色不同的最右像素的 x，没有返回 -1。
        int RightmostInkX(const QImage& image, int top, const QColor& background)
        {
            for (int x = image.width() - 1; x >= 0; --x)
            {
                for (int y = top; y < image.height(); ++y)
                {
                    if (image.pixelColor(x, y) != background)
                    {
                        return x;
                    }
                }
            }
            return -1;
        }

        // HasHan：文本里是否含汉字（CJK 统一表意文字区，与 wpJ6_main.cpp 的 HasHanCharacter 同一判据）。
        bool HasHan(const QString& text)
        {
            for (const QChar ch : text)
            {
                if (ch.unicode() >= 0x4E00U && ch.unicode() <= 0x9FFFU)
                {
                    return true;
                }
            }
            return false;
        }

        // FindMenuButton：视图直接拥有的"视图"菜单钮（行宽图标的 HexViewGlyphButton），找不到返回空。
        // HexViewGlyphButton 没有 Q_OBJECT，不能直接 findChildren<HexViewGlyphButton*>（会按基类
        // QToolButton 的元对象把所有工具钮都当成它），所以先按 QToolButton 找再 dynamic_cast。
        ks::ui::HexViewGlyphButton* FindMenuButton(ks::ui::MemoryWorkbenchView& view)
        {
            for (QToolButton* candidate : view.findChildren<QToolButton*>(QString(), Qt::FindDirectChildrenOnly))
            {
                auto* glyphButton = dynamic_cast<ks::ui::HexViewGlyphButton*>(candidate);
                if (glyphButton != nullptr && glyphButton->glyph() == ks::ui::HexViewGlyphButton::Glyph::RowWidth)
                {
                    return glyphButton;
                }
            }
            return nullptr;
        }

        // FindShortcut：按键序列文本找视图里的 QShortcut（与 wpJ6_tests.Review2B.cpp 同一写法）。
        QShortcut* FindShortcut(ks::ui::MemoryWorkbenchView& view, const QString& key)
        {
            for (auto* shortcut : view.findChildren<QShortcut*>())
            {
                if (shortcut->key() == QKeySequence(key))
                {
                    return shortcut;
                }
            }
            return nullptr;
        }

        // ActionByText：按文字在菜单里找动作，找不到返回空指针。
        QAction* ActionByText(QMenu* menu, const QString& text)
        {
            for (QAction* action : menu->actions())
            {
                if (action->text() == text)
                {
                    return action;
                }
            }
            return nullptr;
        }

        // Rebuild：模拟菜单弹出（emit aboutToShow，让十六进制页按当前状态与主题重建内容）。
        void Rebuild(QMenu* menu)
        {
            emit menu->aboutToShow();
        }

        // Trigger：触发菜单里指定文字的项；找不到时记一条失败（而不是空指针崩溃）。传出：是否找到并触发。
        bool Trigger(QMenu* menu, const QString& text)
        {
            QAction* action = ActionByText(menu, text);
            WPJ6_CHECK_NOTE(action != nullptr, QStringLiteral("菜单里找不到要触发的项：%1").arg(text));
            if (action == nullptr)
            {
                return false;
            }
            action->trigger();
            return true;
        }

        // IsChecked / IsEnabled：读菜单项当前的勾选/可用状态；项不存在时返回 false。
        bool IsChecked(QMenu* menu, const QString& text)
        {
            const QAction* action = ActionByText(menu, text);
            return action != nullptr && action->isChecked();
        }
        bool IsEnabled(QMenu* menu, const QString& text)
        {
            const QAction* action = ActionByText(menu, text);
            return action != nullptr && action->isEnabled();
        }

        // WaitForSpace：等画布装上地址空间（selectedRange 有值即说明有数据）。地址列位数取决于地址空间，
        // 必须等它落定之后再从布局反推位数（DigitsOf），否则可能拿到装空间之前的默认值。
        bool WaitForSpace(HexCanvas* canvas)
        {
            return PumpUntil([canvas]() { return canvas->selectedRange().has_value(); }, 2000);
        }

        // SettleWidth：把视图调整到指定宽度（高 700）并泵事件，让自适应与分割条布局落定。
        void SettleWidth(ks::ui::MemoryWorkbenchView& view, int width)
        {
            view.resize(width, 700);
            PumpFor(80);
        }

        // ShotPath：截图输出路径（MEMWB_OUT/shots/<name>.png，与 wpJ6_tests.Visual.cpp 同一目录约定）。
        QString ShotPath(const QString& name)
        {
            const QString fromEnv = qEnvironmentVariable("MEMWB_OUT");
            const QString base = !fromEnv.isEmpty() ? fromEnv : QStringLiteral(".codex-tmp/memwb-wpJ6");
            QDir().mkpath(base + QStringLiteral("/shots"));
            return base + QStringLiteral("/shots/") + name + QStringLiteral(".png");
        }

        // 默认值：未调用 loadSettings 时画布不是自适应（固定 16 字节）；loadSettings（设置为空）后才默认自适应。
        // 杀死：把自适应无条件打开在 WorkbenchHexPane / MemoryWorkbenchView 的构造函数里（会让既有夹具行为全变）；
        //       LoadRowWidthAuto 默认值写成 false；loadHexPreferences 漏调。
        void TestDefaultsAndLoad()
        {
            ResetPrefs();
            Harness harness;
            harness.AttachProcess();
            auto* view = harness.view.get();
            auto* pane = view->hexPaneForTest();
            auto* canvas = pane->canvas();
            WPJ6_CHECK_NOTE(!canvas->isAutoBytesPerRow(), QStringLiteral("构造后、loadSettings 之前画布不得是自适应"));
            WPJ6_CHECK(!pane->rowWidthAutomatic());
            WPJ6_CHECK(canvas->bytesPerRow() == 16);
            WPJ6_CHECK(pane->manualBytesPerRow() == 16);
            WPJ6_CHECK(canvas->zoomLevel() == 0);

            view->loadSettings();
            WPJ6_CHECK_NOTE(canvas->isAutoBytesPerRow(), QStringLiteral("设置为空时 loadSettings 之后应默认自适应"));
            WPJ6_CHECK(pane->rowWidthAutomatic());
            WPJ6_CHECK(canvas->groupSize() == 1);
            WPJ6_CHECK(canvas->zoomLevel() == 0);
            WPJ6_CHECK_NOTE(
                ks::ui::workbench_settings::LoadRowWidthAuto(),
                QStringLiteral("rowWidthAuto 键缺失时默认值应为真"));
            ResetPrefs();
        }

        // 视图宽度依次变化：行宽等于按真实视口宽度算出的期望；随视口宽度非递减；放得下时没有横向滚动条。
        // 注意：视图宽度本身不保证单调（解释器在 <760 时自动收起，会让窄窗口的画布反而更宽），
        // 所以"非递减"按画布视口宽度排序后核对。
        // 杀死：自适应不随视口变化；选宽度时不看视口宽度；不算地址位数/分组；选最小行宽。
        void TestViewWidthsDriveRowWidth()
        {
            ResetPrefs();
            Harness harness;
            harness.AttachProcess();
            auto* view = harness.view.get();
            view->loadSettings();
            view->resize(900, 700);
            view->show();
            PumpFor(100);
            auto* canvas = view->hexPaneForTest()->canvas();
            WPJ6_CHECK(canvas->isAutoBytesPerRow());
            WPJ6_CHECK(WaitForSpace(canvas));
            const int charWidth = CharWidthOf(*canvas);
            const int digits = DigitsOf(*canvas);
            WPJ6_CHECK_NOTE(digits == 8 || digits == 16, QStringLiteral("地址列位数应为 8 或 16，反推得 %1").arg(digits));

            struct Sample
            {
                int viewportWidth;      // 画布视口宽度（真实值）
                int bytesPerRow;        // 当时的行宽
            };
            std::vector<Sample> samples;
            for (const int width : { 700, 900, 1200, 1600, 2000 })
            {
                SettleWidth(*view, width);
                const int viewportWidth = canvas->viewport()->width();
                const int expected = ExpectedAuto(viewportWidth, charWidth, canvas->groupSize(), digits);
                WPJ6_CHECK_NOTE(
                    canvas->bytesPerRow() == expected,
                    QStringLiteral("视图宽 %1（画布视口宽 %2）：行宽应为 %3，实际 %4")
                        .arg(width).arg(viewportWidth).arg(expected).arg(canvas->bytesPerRow()));
                const bool smallestFits = fmt::RowWidthChars(8, canvas->groupSize(), digits) * charWidth <= viewportWidth;
                if (smallestFits)
                {
                    WPJ6_CHECK_NOTE(
                        canvas->horizontalScrollBar()->maximum() == 0,
                        QStringLiteral("视图宽 %1：放得下却有横向滚动（最大值 %2）").arg(width).arg(canvas->horizontalScrollBar()->maximum()));
                }
                samples.push_back({ viewportWidth, canvas->bytesPerRow() });
            }

            std::sort(samples.begin(), samples.end(), [](const Sample& left, const Sample& right) {
                return left.viewportWidth < right.viewportWidth;
            });
            int widest = 0;
            for (std::size_t index = 0; index < samples.size(); ++index)
            {
                widest = std::max(widest, samples[index].bytesPerRow);
                if (index > 0)
                {
                    WPJ6_CHECK_NOTE(
                        samples[index].bytesPerRow >= samples[index - 1].bytesPerRow,
                        QStringLiteral("视口宽 %1 的行宽 %2 小于更窄视口 %3 的行宽 %4（应随宽度非递减）")
                            .arg(samples[index].viewportWidth).arg(samples[index].bytesPerRow)
                            .arg(samples[index - 1].viewportWidth).arg(samples[index - 1].bytesPerRow));
                }
            }
            WPJ6_CHECK_NOTE(widest > 16, QStringLiteral("最宽的视图下行宽仍不超过 16，自适应没有起作用（最大 %1）").arg(widest));
            view->hide();
            ResetPrefs();
        }

        // 视图菜单：显隐、徽标与悬停说明、行宽/分组/字号项的内容与互斥、悬停提示、不透明样式、
        // 选项作用于画布、随主题重建。
        // 杀死：菜单钮在别的子页也显示；行宽项不互斥/没勾选当前档；缺悬停提示；样式不是不透明/不随主题；
        //       选项没有作用于画布；手选后徽标不刷新。
        void TestViewMenu()
        {
            ResetPrefs();
            Harness harness;
            harness.AttachProcess();
            auto* view = harness.view.get();
            view->loadSettings();
            view->resize(1200, 700);
            view->show();
            PumpFor(100);
            auto* pane = view->hexPaneForTest();
            auto* canvas = pane->canvas();
            ks::ui::HexViewGlyphButton* button = FindMenuButton(*view);
            WPJ6_CHECK_NOTE(button != nullptr, QStringLiteral("子页签那一行右侧应有视图菜单钮"));
            if (button == nullptr)
            {
                view->hide();
                return;
            }
            QMenu* menu = button->menu();
            WPJ6_CHECK(menu != nullptr);
            if (menu == nullptr)
            {
                view->hide();
                return;
            }

            // 显隐：只在十六进制子页（第 0 页）显示。
            WPJ6_CHECK(!button->isHidden());
            view->subTabStackForTest()->setCurrentIndex(1);
            PumpFor(30);
            WPJ6_CHECK_NOTE(button->isHidden(), QStringLiteral("切到反汇编子页后菜单钮应隐藏"));
            // 文本页与对比页同样不显示（菜单只对十六进制页有意义）：只切到反汇编页验不出"显示条件写成 != 1"这类缺陷。
            for (const int hiddenTab : {2, 3})
            {
                view->subTabStackForTest()->setCurrentIndex(hiddenTab);
                PumpFor(30);
                WPJ6_CHECK_NOTE(button->isHidden(), QStringLiteral("切到第 %1 个子页后菜单钮应隐藏").arg(hiddenTab));
            }
            view->subTabStackForTest()->setCurrentIndex(0);
            PumpFor(30);
            WPJ6_CHECK_NOTE(!button->isHidden(), QStringLiteral("切回十六进制子页后菜单钮应重新显示"));

            // 徽标与悬停说明：自适应时徽标是"自动"，说明里写着当前行宽。
            WPJ6_CHECK(canvas->isAutoBytesPerRow());
            WPJ6_CHECK_NOTE(button->badgeText() == QStringLiteral("自动"), QStringLiteral("自适应时徽标应是'自动'，实际 '%1'").arg(button->badgeText()));
            WPJ6_CHECK_NOTE(
                button->toolTip().contains(QString::number(canvas->bytesPerRow())) && button->toolTip().contains(QStringLiteral("自动")),
                QStringLiteral("自适应时悬停说明应写明自动与当前行宽：%1").arg(button->toolTip()));

            // 行宽项：自动 + 五档，互斥单选，自适应时只有"自动"被勾选。
            Rebuild(menu);
            const QString autoText = QStringLiteral("自动（按窗口宽度）");
            const QStringList rowTexts = {
                autoText, QStringLiteral("8 字节 / 行"), QStringLiteral("16 字节 / 行"),
                QStringLiteral("32 字节 / 行"), QStringLiteral("48 字节 / 行"), QStringLiteral("64 字节 / 行") };
            QActionGroup* rowGroup = nullptr;
            int checkedRows = 0;
            QString checkedText;
            for (const QString& text : rowTexts)
            {
                QAction* action = ActionByText(menu, text);
                WPJ6_CHECK_NOTE(action != nullptr, QStringLiteral("行宽菜单缺少项：%1").arg(text));
                if (action == nullptr)
                {
                    continue;
                }
                WPJ6_CHECK(action->isCheckable());
                WPJ6_CHECK_NOTE(!action->toolTip().isEmpty(), QStringLiteral("菜单项缺悬停提示：%1").arg(text));
                if (rowGroup == nullptr)
                {
                    rowGroup = action->actionGroup();
                    WPJ6_CHECK(rowGroup != nullptr && rowGroup->isExclusive());
                }
                WPJ6_CHECK_NOTE(action->actionGroup() == rowGroup, QStringLiteral("行宽项应共用同一个互斥组：%1").arg(text));
                if (action->isChecked())
                {
                    ++checkedRows;
                    checkedText = text;
                }
            }
            WPJ6_CHECK_NOTE(checkedRows == 1 && checkedText == autoText, QStringLiteral("自适应时应只勾选'自动'，实际勾选 %1 项（%2）").arg(checkedRows).arg(checkedText));

            // 分组项：四档互斥，默认 1 字节一组被勾选。
            int checkedGroups = 0;
            for (const int group : { 1, 2, 4, 8 })
            {
                const QString text = QStringLiteral("%1 字节一组").arg(group);
                QAction* action = ActionByText(menu, text);
                WPJ6_CHECK_NOTE(action != nullptr, QStringLiteral("分组菜单缺少项：%1").arg(text));
                if (action == nullptr)
                {
                    continue;
                }
                WPJ6_CHECK(action->isCheckable() && !action->toolTip().isEmpty());
                WPJ6_CHECK(action->isChecked() == (group == 1));
                checkedGroups += action->isChecked() ? 1 : 0;
            }
            WPJ6_CHECK(checkedGroups == 1);

            // 字号项：默认级别 0，放大/缩小可用，恢复默认置灰；每项都有悬停提示。
            QAction* zoomIn = ActionByText(menu, QStringLiteral("放大字号"));
            QAction* zoomOut = ActionByText(menu, QStringLiteral("缩小字号"));
            QAction* zoomReset = ActionByText(menu, QStringLiteral("恢复默认字号"));
            WPJ6_CHECK(zoomIn != nullptr && zoomOut != nullptr && zoomReset != nullptr);
            if (zoomIn != nullptr && zoomOut != nullptr && zoomReset != nullptr)
            {
                WPJ6_CHECK(zoomIn->isEnabled() && zoomOut->isEnabled() && !zoomReset->isEnabled());
                WPJ6_CHECK(!zoomIn->toolTip().isEmpty() && !zoomOut->toolTip().isEmpty() && !zoomReset->toolTip().isEmpty());
            }

            // 不透明样式：背景/文字/选中态/禁用态都显式设置，铺满背景，开启悬停提示。
            const QString lightStyle = menu->styleSheet();
            WPJ6_CHECK(!menu->testAttribute(Qt::WA_TranslucentBackground));
            // 不透明用渲染判据而不是 autoFillBackground()：本菜单挂在按钮下面、测试里应用级样式表非空时，
            // 样式引擎在 setStyleSheet 之后会把 autoFillBackground 复位成假（背景改由 QSS 自己画），
            // 属性值不代表是否透明；真正要保证的是画出来的背景不透明。
            {
                menu->resize(menu->sizeHint());
                const QImage rendered = menu->grab().toImage().convertToFormat(QImage::Format_ARGB32);
                WPJ6_CHECK_NOTE(
                    !rendered.isNull() && rendered.width() > 8 && rendered.height() > 8
                        && rendered.pixelColor(rendered.width() / 2, 3).alpha() == 255,
                    QStringLiteral("视图菜单渲染出来的背景必须不透明"));
            }
            WPJ6_CHECK(menu->toolTipsVisible());
            WPJ6_CHECK(lightStyle.contains(KswordTheme::SurfaceColorHex()));
            WPJ6_CHECK(lightStyle.contains(KswordTheme::TextPrimaryColorHex()));
            WPJ6_CHECK(lightStyle.contains(QStringLiteral("QMenu::item:selected")));
            WPJ6_CHECK(lightStyle.contains(QStringLiteral("QMenu::item:disabled")));

            // 选项作用于画布：手选 16 -> 自适应关闭、行宽 16、徽标变数字、"自动"项不再勾选、16 勾选。
            WPJ6_CHECK(Trigger(menu, QStringLiteral("16 字节 / 行")));
            WPJ6_CHECK(!canvas->isAutoBytesPerRow());
            WPJ6_CHECK(canvas->bytesPerRow() == 16);
            WPJ6_CHECK(pane->manualBytesPerRow() == 16);
            WPJ6_CHECK_NOTE(button->badgeText() == QStringLiteral("16"), QStringLiteral("手选后徽标应是 16，实际 '%1'").arg(button->badgeText()));
            WPJ6_CHECK_NOTE(!button->toolTip().contains(QStringLiteral("自动")), QStringLiteral("手选后悬停说明不应再写自动：%1").arg(button->toolTip()));
            Rebuild(menu);
            WPJ6_CHECK(IsChecked(menu, QStringLiteral("16 字节 / 行")));
            WPJ6_CHECK(!IsChecked(menu, autoText));
            WPJ6_CHECK(!IsChecked(menu, QStringLiteral("32 字节 / 行")));

            // 再选"自动"：自适应重新打开，徽标回到"自动"。
            WPJ6_CHECK(Trigger(menu, autoText));
            WPJ6_CHECK(canvas->isAutoBytesPerRow());
            WPJ6_CHECK(button->badgeText() == QStringLiteral("自动"));

            // 分组与字号。
            WPJ6_CHECK(Trigger(menu, QStringLiteral("4 字节一组")));
            WPJ6_CHECK(canvas->groupSize() == 4);
            Rebuild(menu);
            WPJ6_CHECK(IsChecked(menu, QStringLiteral("4 字节一组")));
            WPJ6_CHECK(!IsChecked(menu, QStringLiteral("1 字节一组")));
            WPJ6_CHECK(Trigger(menu, QStringLiteral("放大字号")));
            WPJ6_CHECK(canvas->zoomLevel() == 1);
            // "缩小字号"真的缩小：点一次回到 0，再点一次到 -1（方向写反的变异在这里立刻暴露）。
            Rebuild(menu);
            WPJ6_CHECK(Trigger(menu, QStringLiteral("缩小字号")));
            WPJ6_CHECK_NOTE(canvas->zoomLevel() == 0, QStringLiteral("放大后再缩小应回到 0，实际 %1").arg(canvas->zoomLevel()));
            WPJ6_CHECK(Trigger(menu, QStringLiteral("缩小字号")));
            WPJ6_CHECK_NOTE(canvas->zoomLevel() == -1, QStringLiteral("再缩小一次应是 -1，实际 %1").arg(canvas->zoomLevel()));
            canvas->setZoomLevel(1);
            Rebuild(menu);
            WPJ6_CHECK(IsEnabled(menu, QStringLiteral("恢复默认字号")));
            WPJ6_CHECK(Trigger(menu, QStringLiteral("恢复默认字号")));
            WPJ6_CHECK(canvas->zoomLevel() == 0);
            canvas->setZoomLevel(HexCanvas::kMaxZoomLevel);
            Rebuild(menu);
            WPJ6_CHECK_NOTE(!IsEnabled(menu, QStringLiteral("放大字号")), QStringLiteral("最大级别时'放大字号'应置灰"));
            canvas->setZoomLevel(HexCanvas::kMinZoomLevel);
            Rebuild(menu);
            WPJ6_CHECK_NOTE(!IsEnabled(menu, QStringLiteral("缩小字号")), QStringLiteral("最小级别时'缩小字号'应置灰"));
            canvas->zoomReset();
            WPJ6_CHECK(Trigger(menu, QStringLiteral("1 字节一组")));

            // 主题：菜单每次弹出前重建，样式里的颜色是当前主题的值（深色与浅色不同，切回来又相同）。
            ApplyTheme(true);
            Rebuild(menu);
            const QString darkStyle = menu->styleSheet();
            WPJ6_CHECK_NOTE(darkStyle != lightStyle, QStringLiteral("深色主题下菜单样式应与浅色不同（没有随主题重建）"));
            WPJ6_CHECK(darkStyle.contains(KswordTheme::SurfaceColorHex()));
            ApplyTheme(false);
            Rebuild(menu);
            WPJ6_CHECK_NOTE(menu->styleSheet() == lightStyle, QStringLiteral("切回浅色后菜单样式应回到浅色"));
            view->hide();
            ResetPrefs();
        }

        // 持久化往返：自动 -> 存 -> 新视图仍自动；手选 32 -> 存 -> 新视图非自动且 32；
        // 自动状态保存时不覆盖手选值；分组/字号往返；非权威视图不落盘；字号设置越界退回默认。
        // 杀死：保存时自动状态覆盖 bytesPerRow 键；加载时不应用手选值/自动标志；漏存分组或字号；
        //       LoadHexZoom 不校验范围；非权威视图也落盘。
        void TestPersistence()
        {
            using namespace ks::ui::workbench_settings;
            ResetPrefs();
            const QString bytesKey = QStringLiteral("memwb/workbench/bytesPerRow");

            // A：默认自动，权威视图保存。自动状态不得写 bytesPerRow 键（键不存在）。
            {
                Harness harness;
                harness.view->setSettingsAuthoritative(true);
                harness.view->loadSettings();
                WPJ6_CHECK(harness.view->hexPaneForTest()->rowWidthAutomatic());
                harness.view->saveSettings();
            }
            WPJ6_CHECK(LoadRowWidthAuto());
            WPJ6_CHECK_NOTE(!QSettings().contains(bytesKey), QStringLiteral("自动状态保存时不得写 bytesPerRow 键"));

            // B：新视图加载后手选 32（走菜单），保存。
            {
                Harness harness;
                harness.view->setSettingsAuthoritative(true);
                harness.view->loadSettings();
                auto* button = FindMenuButton(*harness.view);
                WPJ6_CHECK(button != nullptr && button->menu() != nullptr);
                if (button != nullptr && button->menu() != nullptr)
                {
                    Rebuild(button->menu());
                    QAction* width32 = ActionByText(button->menu(), QStringLiteral("32 字节 / 行"));
                    WPJ6_CHECK(width32 != nullptr);
                    if (width32 != nullptr)
                    {
                        width32->trigger();
                    }
                }
                WPJ6_CHECK(!harness.view->hexPaneForTest()->rowWidthAutomatic());
                harness.view->saveSettings();
            }
            WPJ6_CHECK(!LoadRowWidthAuto());
            WPJ6_CHECK(LoadBytesPerRow() == 32);

            // C：新视图加载：非自动且 32；再切自动并保存，bytesPerRow 键仍是 32（自动状态不覆盖手选值）。
            {
                Harness harness;
                harness.view->setSettingsAuthoritative(true);
                harness.view->loadSettings();
                auto* pane = harness.view->hexPaneForTest();
                WPJ6_CHECK_NOTE(!pane->canvas()->isAutoBytesPerRow(), QStringLiteral("已存手选 32：加载后不应是自适应"));
                WPJ6_CHECK_NOTE(pane->canvas()->bytesPerRow() == 32, QStringLiteral("已存手选 32：加载后行宽应是 32，实际 %1").arg(pane->canvas()->bytesPerRow()));
                pane->setRowWidthPreference(true, pane->manualBytesPerRow());
                WPJ6_CHECK(pane->rowWidthAutomatic());
                harness.view->saveSettings();
            }
            WPJ6_CHECK(LoadRowWidthAuto());
            WPJ6_CHECK_NOTE(LoadBytesPerRow() == 32, QStringLiteral("自动状态保存不得覆盖手选值，实际 %1").arg(LoadBytesPerRow()));

            // D：自动加载后手选值被记住（32）；设分组 4、字号 3 并保存。
            {
                Harness harness;
                harness.view->setSettingsAuthoritative(true);
                harness.view->loadSettings();
                auto* pane = harness.view->hexPaneForTest();
                WPJ6_CHECK(pane->rowWidthAutomatic());
                WPJ6_CHECK_NOTE(pane->manualBytesPerRow() == 32, QStringLiteral("自适应加载后应记得上次手选的 32，实际 %1").arg(pane->manualBytesPerRow()));
                pane->canvas()->setGroupSize(4);
                pane->canvas()->setZoomLevel(3);
                harness.view->saveSettings();
            }
            WPJ6_CHECK(LoadGroupSize() == 4);
            WPJ6_CHECK(LoadHexZoom() == 3);

            // E：新视图加载：分组 4、字号 3、自适应。
            {
                Harness harness;
                harness.view->loadSettings();
                auto* canvas = harness.view->hexPaneForTest()->canvas();
                WPJ6_CHECK(canvas->groupSize() == 4);
                WPJ6_CHECK(canvas->zoomLevel() == 3);
                WPJ6_CHECK(canvas->isAutoBytesPerRow());
            }

            // 非权威视图不落盘：改字号后保存，设置里的字号不变。
            {
                Harness harness;
                harness.view->loadSettings();
                WPJ6_CHECK(!harness.view->isSettingsAuthoritative());
                harness.view->hexPaneForTest()->canvas()->setZoomLevel(5);
                harness.view->saveSettings();
            }
            WPJ6_CHECK_NOTE(LoadHexZoom() == 3, QStringLiteral("非权威视图 saveSettings 不应落盘字号，实际 %1").arg(LoadHexZoom()));

            // 设置读写往返：范围内原样，越界退回默认 0；两处范围常量与画布一致。
            WPJ6_CHECK(kHexZoomMin == HexCanvas::kMinZoomLevel && kHexZoomMax == HexCanvas::kMaxZoomLevel);
            SaveHexZoom(-4);
            WPJ6_CHECK(LoadHexZoom() == -4);
            SaveHexZoom(12);
            WPJ6_CHECK(LoadHexZoom() == 12);
            SaveHexZoom(13);
            WPJ6_CHECK(LoadHexZoom() == 0);
            SaveHexZoom(-5);
            WPJ6_CHECK(LoadHexZoom() == 0);
            SaveRowWidthAuto(false);
            WPJ6_CHECK(!LoadRowWidthAuto());
            SaveRowWidthAuto(true);
            WPJ6_CHECK(LoadRowWidthAuto());
            ResetPrefs();
        }

        // 布局联动：Ctrl+I 隐藏解释器、Ctrl+Shift+B 展开侧栏之后，画布视口宽度变了，行宽按新宽度重选。
        // 杀死：画布宽度变化（非窗口 resize）时自适应不重选；自适应只挂在窗口 resize 上。
        void TestLayoutTogglesRefit()
        {
            ResetPrefs();
            Harness harness;
            harness.AttachProcess();
            auto* view = harness.view.get();
            view->loadSettings();
            view->resize(1400, 700);
            view->show();
            PumpFor(120);
            auto* pane = view->hexPaneForTest();
            auto* canvas = pane->canvas();
            WPJ6_CHECK(WaitForSpace(canvas));
            const int charWidth = CharWidthOf(*canvas);
            const int digits = DigitsOf(*canvas);
            WPJ6_CHECK(canvas->isAutoBytesPerRow());
            WPJ6_CHECK_NOTE(!pane->inspector()->isHidden(), QStringLiteral("1400 宽下解释器面板应可见（前置）"));

            const auto expectedNow = [&]() {
                return ExpectedAuto(canvas->viewport()->width(), charWidth, canvas->groupSize(), digits);
            };
            const int width1 = canvas->viewport()->width();
            const int rows1 = canvas->bytesPerRow();
            WPJ6_CHECK(rows1 == expectedNow());

            // Ctrl+I：隐藏解释器，画布变宽。
            QShortcut* toggleInspector = FindShortcut(*view, QStringLiteral("Ctrl+I"));
            WPJ6_CHECK(toggleInspector != nullptr);
            if (toggleInspector == nullptr)
            {
                view->hide();
                return;
            }
            emit toggleInspector->activated();
            PumpFor(80);
            WPJ6_CHECK(pane->inspector()->isHidden());
            const int width2 = canvas->viewport()->width();
            const int rows2 = canvas->bytesPerRow();
            WPJ6_CHECK_NOTE(width2 > width1, QStringLiteral("隐藏解释器后画布应变宽（%1 -> %2）").arg(width1).arg(width2));
            WPJ6_CHECK_NOTE(rows2 == expectedNow(), QStringLiteral("隐藏解释器后行宽应为 %1，实际 %2").arg(expectedNow()).arg(rows2));

            // Ctrl+Shift+B：展开侧栏（默认已存为隐藏），画布变窄。
            QShortcut* toggleSidebar = FindShortcut(*view, QStringLiteral("Ctrl+Shift+B"));
            WPJ6_CHECK(toggleSidebar != nullptr);
            if (toggleSidebar != nullptr)
            {
                emit toggleSidebar->activated();
                PumpFor(80);
                const int width3 = canvas->viewport()->width();
                const int rows3 = canvas->bytesPerRow();
                WPJ6_CHECK_NOTE(width3 < width2, QStringLiteral("展开侧栏后画布应变窄（%1 -> %2）").arg(width2).arg(width3));
                WPJ6_CHECK_NOTE(rows3 == expectedNow(), QStringLiteral("展开侧栏后行宽应为 %1，实际 %2").arg(expectedNow()).arg(rows3));
                const std::set<int> distinct = { rows1, rows2, rows3 };
                WPJ6_CHECK_NOTE(distinct.size() >= 2, QStringLiteral("三种布局下行宽应至少出现两种取值（%1/%2/%3）").arg(rows1).arg(rows2).arg(rows3));
            }
            view->hide();
            ResetPrefs();
        }

        // 纵向：视图 resize 后画布吃满分割条高度；诊断抽屉展开时有最大高度，画布仍占大头。
        // 杀死：抽屉不设最大高度（展开且已持久化时会把画布挤没）；画布高度没跟上视图高度。
        void TestVerticalFill()
        {
            ResetPrefs();
            Harness harness;
            harness.AttachProcess();
            auto* view = harness.view.get();
            view->loadSettings();
            view->resize(900, 700);
            view->show();
            PumpFor(120);
            auto* pane = view->hexPaneForTest();
            auto* canvas = pane->canvas();

            // 吃满：画布高度等于十六进制页的高度（查找条隐藏），且占视图高度的大头。
            WPJ6_CHECK_NOTE(canvas->height() == pane->height(), QStringLiteral("画布高 %1 应等于十六进制页高 %2").arg(canvas->height()).arg(pane->height()));
            WPJ6_CHECK_NOTE(
                canvas->height() >= view->height() * 40 / 100,
                QStringLiteral("画布高 %1 应不小于视图高 %2 的 4/10").arg(canvas->height()).arg(view->height()));
            const int collapsedHeight = canvas->height();
            const std::uint64_t visibleRows = canvas->visibleRowCount();
            WPJ6_CHECK(visibleRows >= 4);

            // 诊断抽屉展开（错误时自动展开；展开状态还会被持久化）：容器有最大高度，画布仍占大头。
            auto* status = view->statusBarForTest();
            QString longText;
            for (int line = 0; line < 80; ++line)
            {
                longText += QStringLiteral("诊断行 %1\n").arg(line);
            }
            status->setDiagnosticsText(longText, true);
            PumpFor(100);
            WPJ6_CHECK(status->isDrawerExpanded());
            auto* host = status->findChild<ks::ui::WorkbenchDiagnosticsHost*>();
            WPJ6_CHECK(host != nullptr);
            if (host != nullptr && host->parentWidget() != nullptr)
            {
                QWidget* drawer = host->parentWidget();
                WPJ6_CHECK_NOTE(
                    drawer->maximumHeight() <= 200,
                    QStringLiteral("诊断抽屉应设最大高度（<=200），实际上限 %1").arg(drawer->maximumHeight()));
                WPJ6_CHECK_NOTE(
                    drawer->height() <= 200,
                    QStringLiteral("展开的抽屉高度 %1 不应超过 200").arg(drawer->height()));
            }
            WPJ6_CHECK_NOTE(
                canvas->height() >= view->height() * 30 / 100,
                QStringLiteral("抽屉展开后画布高 %1 应不小于视图高 %2 的 3/10（展开前 %3）")
                    .arg(canvas->height()).arg(view->height()).arg(collapsedHeight));
            view->hide();
            ResetPrefs();
        }

        // CaptureScreenshot：浅色/深色各一张截图 + 几何断言。深色必须先 ApplyTheme(true) 再构造视图。
        // 几何断言：右侧空白不超过"下一档与当前档的内容宽度差（加两个字符容差）"，并且明显小于固定 16 字节时的空白
        // ——不是只断言 width()>0。
        // 杀死：自适应不起作用（宽窗口仍是 16 字节一行，右边大片空白）。
        void CaptureScreenshot(bool dark)
        {
            ApplyTheme(dark);
            ResetPrefs();
            const QString name = dark ? QStringLiteral("rowfit_dark") : QStringLiteral("rowfit_light");
            {
                Harness harness;
                harness.AttachProcess();
                auto* view = harness.view.get();
                view->loadSettings();
                view->resize(1600, 700);
                view->show();
                PumpFor(120);
                auto* pane = view->hexPaneForTest();
                auto* canvas = pane->canvas();
                WPJ6_CHECK(WaitForStageable(pane, 0x08ULL));

                // 隐藏解释器让画布吃满（侧栏默认已存为隐藏）。
                QShortcut* toggleInspector = FindShortcut(*view, QStringLiteral("Ctrl+I"));
                WPJ6_CHECK(toggleInspector != nullptr);
                if (toggleInspector != nullptr)
                {
                    emit toggleInspector->activated();
                }
                PumpFor(100);
                view->grab().save(ShotPath(name));

                const int charWidth = CharWidthOf(*canvas);
                const int digits = DigitsOf(*canvas);
                const int viewportWidth = canvas->viewport()->width();
                const int bytesPerRow = canvas->bytesPerRow();
                WPJ6_CHECK(canvas->isAutoBytesPerRow());
                WPJ6_CHECK_NOTE(bytesPerRow > 16, QStringLiteral("1600 宽且无解释器/侧栏时行宽应大于 16，实际 %1").arg(bytesPerRow));

                // 抓画布视口：从表头之下找最右的字迹，换算右侧空白。
                const QRect firstCell = canvas->cellRect(canvas->caretAddress(), Pane::Hex);
                const int bodyTop = (firstCell.isNull() ? 40 : firstCell.y()) + 1;
                const QColor surface = KswordTheme::SurfaceColor();
                const QImage fitted = canvas->viewport()->grab().toImage().convertToFormat(QImage::Format_ARGB32);
                const int rightmost = RightmostInkX(fitted, bodyTop, surface);
                WPJ6_CHECK_NOTE(rightmost > viewportWidth / 2, QStringLiteral("内容应铺过视口一半，最右字迹 x=%1，视口宽 %2").arg(rightmost).arg(viewportWidth));
                const int blank = viewportWidth - 1 - rightmost;
                const int next = NextLarger(bytesPerRow);
                if (next != 0)
                {
                    const int allowed = (fmt::RowWidthChars(next, canvas->groupSize(), digits)
                        - fmt::RowWidthChars(bytesPerRow, canvas->groupSize(), digits)) * charWidth + 2 * charWidth;
                    WPJ6_CHECK_NOTE(blank <= allowed, QStringLiteral("右侧空白 %1 超过允许的 %2（行宽 %3）").arg(blank).arg(allowed).arg(bytesPerRow));
                }

                // 与固定 16 字节对照：自适应的空白必须明显更小。
                canvas->setBytesPerRow(16);
                const QImage fixedImage = canvas->viewport()->grab().toImage().convertToFormat(QImage::Format_ARGB32);
                const int blankFixed = viewportWidth - 1 - RightmostInkX(fixedImage, bodyTop, surface);
                WPJ6_CHECK_NOTE(blankFixed > blank, QStringLiteral("固定 16 字节的右侧空白 %1 应大于自适应的 %2").arg(blankFixed).arg(blank));
                canvas->setAutoBytesPerRow(true);
                view->hide();
            }
            ApplyTheme(false);
            ResetPrefs();
        }
    }

    // RunRowFitTests：本文件全部（中文环境下的）测试，由 wpJ6_main.cpp 调用，排在 RunI18nSmokeTest 之前。
    void RunRowFitTests()
    {
        TestDefaultsAndLoad();
        TestViewWidthsDriveRowWidth();
        TestViewMenu();
        TestPersistence();
        TestLayoutTogglesRefit();
        TestVerticalFill();
        CaptureScreenshot(false);
        CaptureScreenshot(true);
    }

    // RunRowFitI18nTests：en-US 下菜单项、徽标、悬停说明都不含汉字。
    // 必须排在 RunI18nSmokeTest 之后调用（initialize("en-US") 之后进程里再也不会切回中文，
    // 本函数不能再假定界面文字是中文，所以只做"无汉字"判据）。
    void RunRowFitI18nTests()
    {
        ResetPrefs();
        Harness harness;
        harness.AttachProcess();
        auto* view = harness.view.get();
        view->loadSettings();
        view->resize(1200, 700);
        view->show();
        PumpFor(150);
        auto* pane = view->hexPaneForTest();
        ks::ui::HexViewGlyphButton* button = FindMenuButton(*view);
        WPJ6_CHECK(button != nullptr && button->menu() != nullptr);
        if (button == nullptr || button->menu() == nullptr)
        {
            view->hide();
            return;
        }

        // checkAll：对菜单所有项与按钮的徽标/悬停说明做"无汉字"检查，并统计检查到的项数（防空转）。
        const auto checkAll = [&](const QString& state) {
            Rebuild(button->menu());
            int checked = 0;
            for (QAction* action : button->menu()->actions())
            {
                if (action->isSeparator())
                {
                    continue;
                }
                ++checked;
                WPJ6_CHECK_NOTE(!HasHan(action->text()), QStringLiteral("%1：菜单项文字仍含汉字：%2").arg(state, action->text()));
                WPJ6_CHECK_NOTE(!action->toolTip().isEmpty(), QStringLiteral("%1：菜单项缺悬停提示：%2").arg(state, action->text()));
                WPJ6_CHECK_NOTE(!HasHan(action->toolTip()), QStringLiteral("%1：菜单项悬停提示仍含汉字：%2").arg(state, action->toolTip()));
            }
            WPJ6_CHECK_NOTE(checked >= 13, QStringLiteral("%1：菜单项数量异常 %2（应有 6 + 4 + 3 项）").arg(state).arg(checked));
            WPJ6_CHECK_NOTE(!button->badgeText().isEmpty(), QStringLiteral("%1：徽标不应为空").arg(state));
            WPJ6_CHECK_NOTE(!HasHan(button->badgeText()), QStringLiteral("%1：徽标仍含汉字：%2").arg(state, button->badgeText()));
            WPJ6_CHECK_NOTE(!button->toolTip().isEmpty(), QStringLiteral("%1：按钮缺悬停说明").arg(state));
            WPJ6_CHECK_NOTE(!HasHan(button->toolTip()), QStringLiteral("%1：按钮悬停说明仍含汉字：%2").arg(state, button->toolTip()));
        };

        checkAll(QStringLiteral("自适应"));
        pane->setRowWidthPreference(false, 32);
        checkAll(QStringLiteral("手选 32"));
        pane->canvas()->setZoomLevel(HexCanvas::kMaxZoomLevel);
        checkAll(QStringLiteral("字号最大"));
        pane->canvas()->zoomReset();
        view->hide();
        ResetPrefs();
    }
}
