// ============================================================
// WorkbenchHexPane.ViewMenu.cpp
// 作用：十六进制子页的"视图"菜单（子页签那一行右侧的菜单钮弹出）——
// - 行宽：自动（按窗口宽度）/ 8 / 16 / 32 / 48 / 64 字节每行，互斥单选；
// - 分组：1 / 2 / 4 / 8 字节一组，互斥单选；
// - 字号：放大 / 缩小 / 恢复默认（HexCanvas 的缩放级别，Ctrl+滚轮与 Ctrl+= / Ctrl+- / Ctrl+0 同效）。
// 另提供菜单钮的徽标文字与悬停说明（viewButtonBadgeText / viewButtonToolTip）。
//
// 约定（项目对菜单的硬性要求）：
// - 菜单每次弹出前（装配层连接 aboutToShow）调用 rebuildViewMenu 重建，因此勾选状态与主题色
//   都是此刻的真实值；样式显式设置不透明的背景、文字、选中态、禁用态、分隔线（不依赖默认样式——
//   默认样式在某些页面会继承透明背景，浅色模式下出现黑底黑字），颜色全部现取主题令牌，不缓存。
// - 每个菜单项都有悬停提示（setToolTipsVisible(true)）。
// - 菜单钮是自绘控件（HexViewGlyphButton），运行期整树翻译扫描不到它的徽标，所以本文件所有对外文字
//   都在源头经 ks::i18n::sourceText 翻译；带 %1 的整串先翻译再 arg。
// - 选中某项直接作用于画布（行宽/分组/字号），不涉及目标内存读写，不弹确认。
//
// 链接依赖：本文件用到 LanguageManager（sourceText），所以只被"链接了 LanguageManager.cpp"的构建收录
// （主程序、wpJ6）；wpJ5 这类只测 WorkbenchHexPane 接线的夹具不引用本文件里的函数，不需要编进去。
// ============================================================

#include "WorkbenchHexPane.h"

#include "../../Internationalization/LanguageManager.h"
#include "../../theme.h"

#include <QAction>
#include <QActionGroup>
#include <QMenu>

namespace ks::ui
{
    namespace
    {
        // kRowWidthChoices：行宽菜单的手选档位，与画布（HexViewport）支持的集合一致。
        constexpr int kRowWidthChoices[] = { 8, 16, 32, 48, 64 };

        // kGroupChoices：分组菜单的档位，与画布支持的集合一致。
        constexpr int kGroupChoices[] = { 1, 2, 4, 8 };

        // ViewMenuStyleSheet：菜单样式表——背景、文字、边框、选中态、禁用态、分隔线全部用主题色。
        // 照抄 HexView.Toolbar.cpp 的 menuStyleSheet（同一套外观）；每次弹出前重建菜单时才调用，
        // 所以此刻取到的就是当前主题（深浅切换后下一次弹出即是新主题）。
        QString ViewMenuStyleSheet()
        {
            return QStringLiteral(
                "QMenu{background-color:%1;color:%2;border:1px solid %3;padding:3px;}"
                "QMenu::item{color:%2;background-color:transparent;padding:5px 20px 5px 24px;}"
                "QMenu::item:selected{background-color:%4;color:%5;}"
                "QMenu::item:disabled{color:%6;background-color:transparent;}"
                "QMenu::separator{height:1px;background-color:%3;margin:3px 6px;}")
                .arg(KswordTheme::SurfaceColorHex())
                .arg(KswordTheme::TextPrimaryColorHex())
                .arg(KswordTheme::BorderColorHex())
                .arg(KswordTheme::ThemeColorName(KswordTheme::PrimaryAccentColor()))
                .arg(KswordTheme::OnAccentHex())
                .arg(KswordTheme::TextDisabledColorHex());
        }
    }

    // 重建"视图"菜单。传入：要填充的菜单（先清空）。
    // 勾选状态取画布此刻的真实状态：自适应时只有"自动"项被勾选，手动时勾选与当前行宽相同的那一档。
    void WorkbenchHexPane::rebuildViewMenu(QMenu* menu)
    {
        if (menu == nullptr || canvas_ == nullptr)
        {
            return;
        }
        menu->clear();

        // 不透明样式：显式关闭透明背景、铺满背景，并开启悬停提示（QAction 的 toolTip 默认不显示）。
        menu->setAttribute(Qt::WA_TranslucentBackground, false);
        menu->setAutoFillBackground(true);
        menu->setStyleSheet(ViewMenuStyleSheet());
        menu->setToolTipsVisible(true);

        // 上一次弹出时建的互斥组随菜单项一起作废，先删掉，避免每次弹出都多留一个孤儿对象。
        qDeleteAll(menu->findChildren<QActionGroup*>(Qt::FindDirectChildrenOnly));

        // ---- 行宽：自动 + 五档，互斥单选 ----
        auto* rowGroup = new QActionGroup(menu);
        rowGroup->setExclusive(true);

        const bool automatic = canvas_->isAutoBytesPerRow();
        QAction* autoAction = menu->addAction(
            ks::i18n::sourceText(QStringLiteral("自动（按窗口宽度）")));
        autoAction->setCheckable(true);
        autoAction->setChecked(automatic);
        autoAction->setToolTip(ks::i18n::sourceText(
            QStringLiteral("每行字节数随窗口宽度自动调整：放得下就尽量多排，窗口变窄时自动减少")));
        rowGroup->addAction(autoAction);
        connect(autoAction, &QAction::triggered, this, [this]() {
            canvas_->setAutoBytesPerRow(true);
        });

        for (const int choice : kRowWidthChoices)
        {
            QAction* action = menu->addAction(
                ks::i18n::sourceText(QStringLiteral("%1 字节 / 行")).arg(choice));
            action->setCheckable(true);
            action->setChecked(!automatic && choice == canvas_->bytesPerRow());
            action->setToolTip(ks::i18n::sourceText(QStringLiteral("每行显示 %1 个字节")).arg(choice));
            rowGroup->addAction(action);
            connect(action, &QAction::triggered, this, [this, choice]() {
                canvas_->setBytesPerRow(choice);
            });
        }

        menu->addSeparator();

        // ---- 分组：四档，互斥单选 ----
        auto* groupGroup = new QActionGroup(menu);
        groupGroup->setExclusive(true);
        for (const int choice : kGroupChoices)
        {
            QAction* action = menu->addAction(
                ks::i18n::sourceText(QStringLiteral("%1 字节一组")).arg(choice));
            action->setCheckable(true);
            action->setChecked(choice == canvas_->groupSize());
            action->setToolTip(ks::i18n::sourceText(
                QStringLiteral("十六进制列每 %1 个字节为一组，组间留出间隙")).arg(choice));
            groupGroup->addAction(action);
            connect(action, &QAction::triggered, this, [this, choice]() {
                canvas_->setGroupSize(choice);
            });
        }

        menu->addSeparator();

        // ---- 字号：放大 / 缩小 / 恢复默认（到头或已是默认时置灰，悬停提示仍在） ----
        const int zoomLevel = canvas_->zoomLevel();
        QAction* zoomIn = menu->addAction(ks::i18n::sourceText(QStringLiteral("放大字号")));
        zoomIn->setToolTip(ks::i18n::sourceText(
            QStringLiteral("放大十六进制视图的字号（Ctrl+滚轮向上，或 Ctrl+=）")));
        zoomIn->setEnabled(zoomLevel < HexCanvas::kMaxZoomLevel);
        connect(zoomIn, &QAction::triggered, this, [this]() {
            canvas_->zoomBy(1);
        });

        QAction* zoomOut = menu->addAction(ks::i18n::sourceText(QStringLiteral("缩小字号")));
        zoomOut->setToolTip(ks::i18n::sourceText(
            QStringLiteral("缩小十六进制视图的字号（Ctrl+滚轮向下，或 Ctrl+-）")));
        zoomOut->setEnabled(zoomLevel > HexCanvas::kMinZoomLevel);
        connect(zoomOut, &QAction::triggered, this, [this]() {
            canvas_->zoomBy(-1);
        });

        QAction* zoomReset = menu->addAction(ks::i18n::sourceText(QStringLiteral("恢复默认字号")));
        zoomReset->setToolTip(ks::i18n::sourceText(
            QStringLiteral("把十六进制视图的字号恢复为默认大小（Ctrl+0）")));
        zoomReset->setEnabled(zoomLevel != 0);
        connect(zoomReset, &QAction::triggered, this, [this]() {
            canvas_->zoomReset();
        });
    }

    // 菜单钮徽标：自适应时显示"自动"，否则显示当前每行字节数。
    // 徽标是自绘文字，必须在源头翻译（"自动" 经 sourceText；数字不需要）。
    QString WorkbenchHexPane::viewButtonBadgeText() const
    {
        if (canvas_ == nullptr)
        {
            return QString();
        }
        if (canvas_->isAutoBytesPerRow())
        {
            return ks::i18n::sourceText(QStringLiteral("自动"));
        }
        return QString::number(canvas_->bytesPerRow());
    }

    // 菜单钮悬停说明：写明含义与当前行宽（自适应时同时写出当前选出的那一档）。
    QString WorkbenchHexPane::viewButtonToolTip() const
    {
        if (canvas_ == nullptr)
        {
            return QString();
        }
        if (canvas_->isAutoBytesPerRow())
        {
            return ks::i18n::sourceText(
                QStringLiteral("每行字节数：自动（当前 %1）。点击设置行宽、分组与字号"))
                .arg(canvas_->bytesPerRow());
        }
        return ks::i18n::sourceText(
            QStringLiteral("每行字节数：%1。点击设置行宽、分组与字号"))
            .arg(canvas_->bytesPerRow());
    }
}
