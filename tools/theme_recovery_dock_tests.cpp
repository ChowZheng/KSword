#include "theme_recovery_test_support.h"

#include "../Ksword5.1/Ksword5.1/UI/DockThemeIcons.h"
#include "../Ksword5.1/Ksword5.1/UI/UI.css/UI_css.h"
#include "../Ksword5.1/Ksword5.1/include/ads/DockAreaTitleBar.h"
#include "../Ksword5.1/Ksword5.1/include/ads/DockAreaWidget.h"
#include "../Ksword5.1/Ksword5.1/include/ads/DockManager.h"
#include "../Ksword5.1/Ksword5.1/include/ads/DockWidget.h"
#include "../Ksword5.1/Ksword5.1/include/ads/DockWidgetTab.h"
#include "../Ksword5.1/Ksword5.1/include/ads/IconProvider.h"
#include "../Ksword5.1/Ksword5.1/theme.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDebug>
#include <QEnterEvent>
#include <QEvent>
#include <QImage>
#include <QLabel>
#include <QPalette>
#include <QPixmap>
#include <QRegion>
#include <QToolButton>
#include <QWidget>
#include <array>

namespace
{
    // IconImages 保存六类图标的正常态像素，比较真实渲染结果而非 QIcon 身份编号。
    using IconImages = std::array<QImage, 6>;

    // iconImage 作用：读取指定状态下的最终像素，统一格式方便比较和检查 alpha。
    // icon 为图标；mode 为正常/选中等状态；返回 16px ARGB 图像。
    QImage iconImage(const QIcon& icon, const QIcon::Mode mode = QIcon::Normal)
    {
        return icon.pixmap(QSize(16, 16), mode, QIcon::Off)
            .toImage().convertToFormat(QImage::Format_ARGB32);
    }

    // hasVisibleTint 作用：确认存在可见轮廓，且所有可见像素保留指定的主题 RGB。
    // image 为渲染图；expected 为对应底色下校准后的前景；返回检查是否通过。
    bool hasVisibleTint(const QImage& image, const QColor& expected)
    {
        bool hasVisiblePixel = false; // 记录是否确实渲染了图标，而非空透明图。
        for (int row = 0; row < image.height(); ++row)
        {
            const QRgb* pixels = reinterpret_cast<const QRgb*>(image.constScanLine(row));
            for (int column = 0; column < image.width(); ++column)
            {
                const QRgb pixel = pixels[column]; // 当前抗锯齿像素，低 alpha 会有量化误差。
                if (qAlpha(pixel) < 192)
                {
                    continue;
                }
                hasVisiblePixel = true;
                if (qAbs(qRed(pixel) - expected.red()) > 2 ||
                    qAbs(qGreen(pixel) - expected.green()) > 2 ||
                    qAbs(qBlue(pixel) - expected.blue()) > 2)
                {
                    return false;
                }
            }
        }
        return hasVisiblePixel;
    }

    // providerImages 作用：检查全部公开 provider 角色的四色态与高 DPI 图标，并捕获正常像素。
    // 无入参；返回后续主题切换比较使用的六幅图像；失败由 Require 汇总。
    IconImages providerImages()
    {
        IconImages images; // 六类 Normal 图像快照，顺序与公开 eIcon 编号一致。
        const std::array<QIcon::Mode, 4> modes{
            QIcon::Normal, QIcon::Active, QIcon::Selected, QIcon::Disabled };
        const std::array<QColor, 4> colors{
            KswordTheme::DockTabGlyphColor(KswordTheme::DockTabState::Inactive),
            KswordTheme::DockTabGlyphColor(KswordTheme::DockTabState::Hover),
            KswordTheme::DockTabGlyphColor(KswordTheme::DockTabState::Active),
            KswordTheme::DockTabGlyphColor(KswordTheme::DockTabState::Inactive, true) };
        const std::array<QColor, 3> backgrounds{
            KswordTheme::DockTabBackgroundColor(KswordTheme::DockTabState::Inactive),
            KswordTheme::DockTabBackgroundColor(KswordTheme::DockTabState::Hover),
            KswordTheme::DockTabBackgroundColor(KswordTheme::DockTabState::Active) };

        for (std::size_t index = 0; index < images.size(); ++index)
        {
            const QIcon icon = ads::CDockManager::iconProvider()
                .customIcon(static_cast<ads::eIcon>(index));
            Require(!icon.isNull(), "ADS provider role has a non-empty themed icon");
            images[index] = iconImage(icon);
            for (std::size_t modeIndex = 0; modeIndex < modes.size(); ++modeIndex)
            {
                Require(hasVisibleTint(iconImage(icon, modes[modeIndex]), colors[modeIndex]),
                    "ADS provider mode preserves contour and calibrated theme RGB");
                // 上游选中标签里的按钮仍可能请求Normal；每种mode都要对所有导航底色可读。
                for (const QColor& background : backgrounds)
                {
                    Require(KswordTheme::ContrastRatio(colors[modeIndex], background) >= 2.999,
                        "Every ADS provider mode contrasts against every navigation state background");
                }
            }
            // hidpi 验证实际输出尺寸，防止标准图标的小位图被错误 DPR 缩成不可见。
            const QPixmap hidpi = icon.pixmap(QSize(32, 32), 2.0);
            Require(!hidpi.isNull() && hidpi.width() >= 32 && hidpi.height() >= 32,
                "ADS provider retains a usable high-DPI representation");
        }
        return images;
    }

    // checkDockButtons 作用：验证已有真实 ADS 按钮也获得 provider 图标，并排除通用二次着色。
    // dock 为真实停靠页；无返回值；不会点击按钮或改变生产布局。
    void checkDockButtons(ads::CDockWidget* dock)
    {
        QAbstractButton* closeButton = dock->tabWidget()->findChild<QAbstractButton*>(
            QStringLiteral("tabCloseButton"), Qt::FindDirectChildrenOnly);
        Require(closeButton != nullptr, "Real ADS tab exposes its upstream close-button object");
        if (closeButton != nullptr)
        {
            Require(closeButton->property("ksword_theme_icon_managed").toBool(),
                "ADS tab close icon keeps its calibrated multi-state owner");
            Require(iconImage(closeButton->icon()) == iconImage(
                ads::CDockManager::iconProvider().customIcon(ads::TabCloseIcon)),
                "Existing ADS tab close icon matches current provider pixels");
        }

        // roles 对应标题栏公开按钮编号；不依赖第三方私有 d 指针或源码修改。
        const std::array<ads::TitleBarButton, 5> roles{
            ads::TitleBarButtonAutoHide, ads::TitleBarButtonTabsMenu,
            ads::TitleBarButtonUndock, ads::TitleBarButtonClose, ads::TitleBarButtonMinimize };
        const std::array<ads::eIcon, 5> icons{
            ads::AutoHideIcon, ads::DockAreaMenuIcon, ads::DockAreaUndockIcon,
            ads::DockAreaCloseIcon, ads::DockAreaMinimizeIcon };
        ads::CDockAreaTitleBar* titleBar = dock->dockAreaWidget()->titleBar();
        for (std::size_t index = 0; index < roles.size(); ++index)
        {
            QAbstractButton* button = titleBar->button(roles[index]);
            Require(button != nullptr, "Real ADS title bar contains the expected role button");
            if (button != nullptr)
            {
                Require(button->property("ksword_theme_icon_managed").toBool(),
                    "ADS title-bar icon keeps its calibrated multi-state owner");
                Require(iconImage(button->icon()) == iconImage(
                    ads::CDockManager::iconProvider().customIcon(icons[index])),
                    "Existing ADS title-bar icon matches current provider pixels");
            }
        }
    }

    // NavigationColors 保存三态底色、文字、图形、禁用图形及选中标记；按最终8位RGB比较。
    using NavigationColors = std::array<QRgb, 11>;
    using ProviderStates = std::array<QImage, 24>; // 六种provider的四态真实像素。
    using NavigationSurfaces = std::array<QImage, 5>; // 普通、hover、选中、选中hover、整排标题栏。

    // drainDockEvents 排空样式和布局队列；无固定等待，不启动生产窗口或业务采样。
    void drainDockEvents()
    {
        for (int round = 0; round < 8; ++round)
        {
            QApplication::processEvents();
        }
    }

    // navigationColors 读取公开生产角色并检查实际底色对比度，不复制偏移或选择算法。
    // 无参数；返回三态角色快照，失败交由Require统一汇总。
    NavigationColors navigationColors()
    {
        NavigationColors colors{}; // 将各角色统一量化为RGBA，分别比较普通底面与强调态。
        const std::array<KswordTheme::DockTabState, 3> states{
            KswordTheme::DockTabState::Inactive,
            KswordTheme::DockTabState::Hover,
            KswordTheme::DockTabState::Active };
        const std::array<QColor, 3> backgrounds{
            KswordTheme::DockTabBackgroundColor(states[0]),
            KswordTheme::DockTabBackgroundColor(states[1]),
            KswordTheme::DockTabBackgroundColor(states[2]) };
        for (std::size_t index = 0; index < states.size(); ++index)
        {
            const QColor background = KswordTheme::DockTabBackgroundColor(states[index]);
            const QColor text = KswordTheme::DockTabTextColor(states[index]);
            const QColor glyph = KswordTheme::DockTabGlyphColor(states[index]);
            colors[index * 3] = background.rgba();
            colors[index * 3 + 1] = text.rgba();
            colors[index * 3 + 2] = glyph.rgba();
            Require(KswordTheme::ContrastRatio(text, background) >= 4.499,
                "ADS navigation text contrasts against its actual state background");
            for (const QColor& possibleBackground : backgrounds)
            {
                Require(KswordTheme::ContrastRatio(glyph, possibleBackground) >= 2.999,
                    "ADS navigation glyph contrasts against every possible button background");
            }
        }
        const QColor disabled = KswordTheme::DockTabGlyphColor(
            KswordTheme::DockTabState::Inactive, true);
        colors[9] = disabled.rgba();
        colors[10] = KswordTheme::DockTabHighlightColor().rgba(); // 选中标记也须跟随主体色且可读。
        Require(KswordTheme::ContrastRatio(QColor::fromRgba(colors[10]), backgrounds[2]) >= 2.999,
            "Selected navigation marker contrasts against the actual background");
        for (const QColor& background : backgrounds)
        {
            Require(KswordTheme::ContrastRatio(disabled, background) >= 2.999,
                "Disabled ADS navigation glyph retains contrast on every state background");
        }
        return colors;
    }

    // providerStates 捕获全六角色四状态；背景种子变化时不能只验证Normal或QIcon缓存键。
    // 返回最终图标像素，无副作用。
    ProviderStates providerStates()
    {
        ProviderStates images; // 角色主序、状态次序，与providerImages相同。
        const std::array<QIcon::Mode, 4> modes{
            QIcon::Normal, QIcon::Active, QIcon::Selected, QIcon::Disabled };
        for (std::size_t index = 0; index < 6; ++index)
        {
            const QIcon icon = ads::CDockManager::iconProvider()
                .customIcon(static_cast<ads::eIcon>(index));
            for (std::size_t modeIndex = 0; modeIndex < modes.size(); ++modeIndex)
            {
                images[index * modes.size() + modeIndex] = iconImage(icon, modes[modeIndex]);
            }
        }
        return images;
    }

    // renderDockSurface 只绘制真实QFrame/QSS底面，排除文字和按钮对背景采样的干扰。
    // widget为真实ADS控件；返回逻辑像素图，不镜像生产样式字符串。
    QImage renderDockSurface(QWidget* widget)
    {
        if (widget == nullptr || widget->size().isEmpty())
        {
            return QImage();
        }
        QImage pixels(widget->size(), QImage::Format_ARGB32); // 固定逻辑坐标，避开显示器DPR差异。
        pixels.fill(Qt::transparent);
        widget->render(&pixels, QPoint(), QRegion(), QWidget::DrawWindowBackground);
        return pixels;
    }

    // surfaceUsesColor 检查真实背景内部像素；空图、透明底或旧背景色均必须失败。
    bool surfaceUsesColor(const QImage& image, const QColor& expected)
    {
        if (image.isNull() || image.width() < 4 || image.height() < 4)
        {
            return false;
        }
        return image.pixelColor(image.width() / 2, image.height() / 2).rgba() == expected.rgba();
    }

    // setDockHover 在离屏窗口设置Qt公开鼠标状态并送真实Enter/Leave，触发QSS伪状态重绘。
    // widget为待绘制标签；hovered决定鼠标状态；不触碰生产主窗口或真实鼠标。
    void setDockHover(QWidget* widget, const bool hovered)
    {
        if (hovered)
        {
            const QPointF position(8, 8); // 标签内坐标，避免进入按钮分支。
            QEnterEvent event(position, position, QPointF(widget->mapToGlobal(QPoint(8, 8))));
            QApplication::sendEvent(widget, &event);
        }
        else
        {
            QEvent event(QEvent::Leave);
            QApplication::sendEvent(widget, &event);
        }
        widget->setAttribute(Qt::WA_UnderMouse, hovered);
        if (auto* tab = qobject_cast<ads::CDockWidgetTab*>(widget))
        {
            // 自有工厂的事件接线另作静态检查；夹具直接使用同一生产前景补偿，不复制其算法。
            ks::ui::ApplyDockTabTextColor(tab, tab->isActiveTab(), hovered);
        }
        widget->update();
        drainDockEvents();
    }

    // checkDockLabel 验证真实标签子QLabel的前景，没有仅检查token而忽略继承/子样式覆盖。
    // tab为真实标签；state为其预期状态；无返回值。
    void checkDockLabel(ads::CDockWidgetTab* tab, const KswordTheme::DockTabState state)
    {
        QLabel* textLabel = nullptr; // 找文本标签而非上游可能存在的图标标签。
        for (QLabel* label : tab->findChildren<QLabel*>())
        {
            if (!label->text().isEmpty())
            {
                textLabel = label;
                break;
            }
        }
        Require(textLabel != nullptr, "Real ADS navigation tab contains a text label");
        if (textLabel != nullptr)
        {
            const QColor actual = textLabel->palette().color(textLabel->foregroundRole());
            const QColor expected = KswordTheme::DockTabTextColor(state);
            if (actual.rgba() != expected.rgba())
            {
                const char* stateName = state == KswordTheme::DockTabState::Inactive ? "Inactive"
                    : state == KswordTheme::DockTabState::Hover ? "Hover" : "Active";
                qWarning().noquote() << "DOCK_LABEL_DIAGNOSTIC"
                    << "mode=" << (KswordTheme::IsDarkModeEnabled() ? "dark" : "light")
                    << "accent=" << KswordTheme::PrimaryAccentColor().name(QColor::HexArgb)
                    << "background=" << KswordTheme::CustomMainBackgroundColor.name(QColor::HexArgb)
                    << "state=" << stateName
                    << "actual=" << actual.name(QColor::HexArgb)
                    << "expected=" << expected.name(QColor::HexArgb)
                    << "class=" << textLabel->metaObject()->className()
                    << "role=" << static_cast<int>(textLabel->foregroundRole())
                    << "labelText=" << textLabel->text()
                    << "activeTab=" << tab->isActiveTab()
                    << "tabUnderMouse=" << tab->underMouse()
                    << "labelUnderMouse=" << textLabel->underMouse()
                    << "tabWindowText=" << tab->palette().color(QPalette::WindowText).name(QColor::HexArgb)
                    << "tabText=" << tab->palette().color(QPalette::Text).name(QColor::HexArgb);
            }
            Require(actual.rgba() == expected.rgba(),
                "Real ADS navigation label receives its state foreground");
        }
    }

    // checkNavigationChildLayers 检查透明子层的真实合成结果，防止旧hover把alternate-base露出来。
    // tab为真实标签；state为其导航底色；探针位于文字基线以下，不遮挡原有图标或文字采样。
    void checkNavigationChildLayers(ads::CDockWidgetTab* tab, const KswordTheme::DockTabState state)
    {
        const std::array<QString, 3> names{
            QStringLiteral("fixtureNavigationWidget"), QStringLiteral("fixtureNavigationButton"),
            QStringLiteral("fixtureNavigationLabel") };
        std::array<QWidget*, 3> probes{}; // 普通QWidget、按钮、QLabel覆盖旧基础样式的三类子层。
        for (std::size_t index = 0; index < names.size(); ++index)
        {
            probes[index] = tab->findChild<QWidget*>(names[index], Qt::FindDirectChildrenOnly);
            Require(probes[index] != nullptr, "Real ADS navigation fixture contains its child-layer probe");
            if (probes[index] != nullptr)
            {
                probes[index]->setGeometry(2 + static_cast<int>(index) * 8, tab->height() - 5, 6, 4);
                probes[index]->setAttribute(Qt::WA_UnderMouse, false);
                probes[index]->raise();
            }
        }
        QImage pixels(tab->size(), QImage::Format_ARGB32); // 带子控件绘制，透明层应透出同一导航底色。
        pixels.fill(Qt::transparent);
        tab->render(&pixels);
        for (QWidget* probe : probes)
        {
            if (probe != nullptr)
            {
                const QPoint point = probe->mapTo(tab, probe->rect().center());
                Require(pixels.rect().contains(point) && pixels.pixelColor(point).rgba()
                    == KswordTheme::DockTabBackgroundColor(state).rgba(),
                    "Real ADS navigation child layer reveals navigation color instead of alternate-base");
            }
        }
    }

    // checkDisabledTitleBarButton 隔离图标后检查禁用按钮是否透出普通标题栏底面。
    // titleBar为真实ADS标题栏；恢复原图标与启用状态，不改变后续图标检查结果。
    void checkDisabledTitleBarButton(ads::CDockAreaTitleBar* titleBar)
    {
        QAbstractButton* button = titleBar->button(ads::TitleBarButtonClose);
        Require(button != nullptr, "Real ADS title bar exposes a close button for disabled-surface verification");
        if (button == nullptr)
        {
            return;
        }
        const bool wasEnabled = button->isEnabled();
        const QIcon icon = button->icon(); // 移除绘制前景，只检查按钮底面；随后立即恢复。
        button->setIcon(QIcon());
        button->setEnabled(false);
        setDockHover(button, false);
        Require(!button->isHidden() && !button->size().isEmpty(),
            "Disabled ADS title-bar button remains visible in the fixture");
        QImage pixels(titleBar->size(), QImage::Format_ARGB32);
        pixels.fill(Qt::transparent);
        titleBar->render(&pixels);
        const QPoint point = button->mapTo(titleBar, button->rect().center());
        Require(pixels.rect().contains(point) && pixels.pixelColor(point).rgba()
            == KswordTheme::DockTabBackgroundColor(KswordTheme::DockTabState::Inactive).rgba(),
            "Disabled ADS title-bar button stays transparent over the navigation background");
        button->setIcon(icon);
        button->setEnabled(wasEnabled);
        drainDockEvents();
    }

    // navigationSurfaces 捕获实际正常/hover/选中/选中hover与整排标题栏底面。
    // inactive/active共享同一真实DockArea；返回渲染快照用于主题和背景切换比较。
    NavigationSurfaces navigationSurfaces(ads::CDockWidget* inactive, ads::CDockWidget* active)
    {
        NavigationSurfaces images; // 状态順序固定，方便比较是否有背景色渗入。
        auto* inactiveTab = inactive->tabWidget();
        auto* activeTab = active->tabWidget();
        setDockHover(activeTab, false);
        setDockHover(inactiveTab, false);
        Require(!inactiveTab->isActiveTab() && activeTab->isActiveTab(),
            "Real ADS navigation fixture preserves inactive and active tabs");
        images[0] = renderDockSurface(inactiveTab);
        checkDockLabel(inactiveTab, KswordTheme::DockTabState::Inactive);
        checkNavigationChildLayers(inactiveTab, KswordTheme::DockTabState::Inactive);
        setDockHover(inactiveTab, true);
        images[1] = renderDockSurface(inactiveTab);
        checkDockLabel(inactiveTab, KswordTheme::DockTabState::Hover);
        checkNavigationChildLayers(inactiveTab, KswordTheme::DockTabState::Hover);
        setDockHover(inactiveTab, false);
        images[2] = renderDockSurface(activeTab);
        checkDockLabel(activeTab, KswordTheme::DockTabState::Active);
        checkNavigationChildLayers(activeTab, KswordTheme::DockTabState::Active);
        setDockHover(activeTab, true);
        images[3] = renderDockSurface(activeTab);
        checkDockLabel(activeTab, KswordTheme::DockTabState::Active);
        checkNavigationChildLayers(activeTab, KswordTheme::DockTabState::Active);
        setDockHover(activeTab, false);
        images[4] = renderDockSurface(active->dockAreaWidget()->titleBar());
        checkDisabledTitleBarButton(active->dockAreaWidget()->titleBar());

        const std::array<KswordTheme::DockTabState, 5> states{
            KswordTheme::DockTabState::Inactive, KswordTheme::DockTabState::Hover,
            KswordTheme::DockTabState::Active, KswordTheme::DockTabState::Active,
            KswordTheme::DockTabState::Inactive };
        for (std::size_t index = 0; index < states.size(); ++index)
        {
            Require(surfaceUsesColor(images[index], KswordTheme::DockTabBackgroundColor(states[index])),
                "Real ADS navigation surface renders the production state color");
        }
        // 活动标签悬停也必须保留主题色标记，避免仅前景角色改变而实际边框仍是旧色。
        for (const std::size_t index : { std::size_t(2), std::size_t(3) })
        {
            const QImage& image = images[index]; // 实际QSS绘制的活动标签底面。
            Require(!image.isNull() && image.height() >= 2
                && image.pixelColor(image.width() / 2, image.height() - 1).rgba()
                    == KswordTheme::DockTabHighlightColor().rgba(),
                "Active navigation renders its theme marker with and without hover");
        }
        return images;
    }

    // applyDockNavigationStyle 接入生产基础块和导航块，局部palette跟随实际主题角色以暴露旧背景规则。
    // manager为离屏真实ADS对象；透明块模拟主窗口末尾兜底；不改QApplication全局palette。
    void applyDockNavigationStyle(ads::CDockManager& manager, const QString& transparentFallback)
    {
        QPalette palette = manager.palette();
        palette.setColor(QPalette::Window, KswordTheme::MainBackgroundColor());
        palette.setColor(QPalette::Base, KswordTheme::SurfaceColor());
        palette.setColor(QPalette::AlternateBase, KswordTheme::SurfaceAltColor());
        palette.setColor(QPalette::Button, KswordTheme::SurfaceAltColor());
        palette.setColor(QPalette::Text, KswordTheme::TextPrimaryColor());
        palette.setColor(QPalette::WindowText, KswordTheme::MainBackgroundTextColor());
        palette.setColor(QPalette::ButtonText, KswordTheme::TextPrimaryColor());
        palette.setColor(QPalette::Highlight, KswordTheme::PrimaryBlueColor);
        palette.setColor(QPalette::HighlightedText, KswordTheme::OnAccentColor());
        manager.setPalette(palette);
        manager.setStyleSheet(QSS_MainWindow_dockStyle + transparentFallback
            + KswordTheme::DockNavigationStyleSheet());
        ks::ui::RefreshDockThemeIcons(&manager);
        drainDockEvents();
    }

    // testDockNavigation 用真实ADS验证三态背景跟随背景色，而强调前景/标记跟随主体色。
    // 无输入或返回；仅创建独立ADS对象，不构造MainWindow、不运行构建或业务后端。
    void testDockNavigation()
    {
        ads::CDockManager manager; // 与生产使用相同上游控件，QSS从公开函数现场生成。
        manager.resize(640, 300);
        auto* first = manager.createDockWidget(QStringLiteral("Inactive navigation"));
        auto* second = manager.createDockWidget(QStringLiteral("Active navigation"));
        first->setWidget(new QWidget());
        second->setWidget(new QWidget());
        for (ads::CDockWidget* dock : { first, second })
        {
            auto* tab = dock->tabWidget();
            tab->setProperty("kswordDockTab", true); // 与生产标签相同属性，启用旧基础hover的高优先级规则。
            QWidget* layer = new QWidget(tab);
            layer->setObjectName(QStringLiteral("fixtureNavigationWidget"));
            QToolButton* button = new QToolButton(tab);
            button->setObjectName(QStringLiteral("fixtureNavigationButton"));
            button->setFocusPolicy(Qt::NoFocus);
            QLabel* label = new QLabel(tab);
            label->setObjectName(QStringLiteral("fixtureNavigationLabel"));
            layer->show();
            button->show();
            label->show();
        }
        auto* area = manager.addDockWidget(ads::LeftDockWidgetArea, first);
        manager.addDockWidgetTabToArea(second, area);
        area->setCurrentDockWidget(second);
        manager.show();
        drainDockEvents();

        // 仅模拟已有透明兜底，不复制任何三态导航QSS；生产块必须在其后生效。
        const QString transparentFallback = QStringLiteral(
            "ads--CDockAreaTitleBar,ads--CDockAreaTabBar{"
            "background:transparent !important;background-color:transparent !important;}");
        for (const bool darkMode : { false, true })
        {
            KswordTheme::SetDarkModeEnabled(darkMode);
            KswordTheme::SetPrimaryAccentColor(QStringLiteral("#216d40"));
            KswordTheme::SetMainBackgroundColor(QString());
            applyDockNavigationStyle(manager, transparentFallback);
            const NavigationColors colors = navigationColors();
            const ProviderStates icons = providerStates();
            const NavigationSurfaces surfaces = navigationSurfaces(first, second);

            // backgrounds覆盖无自定义、极端黑白及相反色相；三态都须保持真实图标对比。
            const std::array<QString, 5> backgrounds{
                QString(), QStringLiteral("#000000"), QStringLiteral("#ffffff"),
                QStringLiteral("#bf2941"), QStringLiteral("#9bd8ef") };
            NavigationColors blackColors{}; // 黑色种子下的三态底面与前景。
            NavigationSurfaces blackSurfaces; // 黑色种子下的真实像素。
            for (const QString& background : backgrounds)
            {
                KswordTheme::SetMainBackgroundColor(background);
                applyDockNavigationStyle(manager, transparentFallback);
                const NavigationColors currentColors = navigationColors(); // 检查当前三态可读性。
                const NavigationSurfaces currentSurfaces = navigationSurfaces(first, second); // 检查实际绘制。
                providerImages(); // 前景可随背景校准，每个图标状态仍须可读。
                if (background == QStringLiteral("#000000"))
                {
                    blackColors = currentColors;
                    blackSurfaces = currentSurfaces;
                }
                else if (background == QStringLiteral("#ffffff"))
                {
                    // 只改变外围窗口不足以证明导航背景已接入，逐态比较黑白种子。
                    for (std::size_t index = 0; index < 3; ++index)
                    {
                        Require(currentColors[index * 3] != blackColors[index * 3],
                            "Background seed changes every navigation state background");
                        Require(currentSurfaces[index] != blackSurfaces[index],
                            "Background seed changes real navigation state surfaces");
                    }
                    Require(currentSurfaces[4] != blackSurfaces[4],
                        "Background seed changes the real title bar surface");
                }
            }

            // 固定背景仅改变主体色，所有底面应保持，强调图标和标记应刷新。
            KswordTheme::SetMainBackgroundColor(QString());
            KswordTheme::SetPrimaryAccentColor(QStringLiteral("#c08040"));
            applyDockNavigationStyle(manager, transparentFallback);
            const NavigationColors changed = navigationColors(); // 换主体色后的公开角色。
            const NavigationSurfaces changedSurfaces = navigationSurfaces(first, second); // 实际背景及标记。
            for (std::size_t index = 0; index < 3; ++index)
            {
                Require(changed[index * 3] == colors[index * 3],
                    "Accent changes preserve every navigation state background");
            }
            Require(changedSurfaces[0] == surfaces[0] && changedSurfaces[1] == surfaces[1]
                && changedSurfaces[4] == surfaces[4],
                "Accent changes preserve ordinary and hovered tab and title bar surfaces");
            Require(changed[10] != colors[10] && changedSurfaces[2] != surfaces[2]
                && changedSurfaces[3] != surfaces[3],
                "Accent changes refresh the selected marker including hovered active tabs");
            Require(providerStates() != icons,
                "Changing primary accent changes real ADS navigation provider pixels");

            // 低饱和灰色与极端黑白容易跨过同一图标可读明度区间，须检查真实四态图标。
            const std::array<QString, 8> accentBoundaries{
                QStringLiteral("#646464"), QStringLiteral("#000000"),
                QStringLiteral("#ffffff"), QStringLiteral("#ff0000"),
                QStringLiteral("#00ff00"), QStringLiteral("#0000ff"),
                QStringLiteral("#ff00ff"), QStringLiteral("#00ffff") };
            for (const QString& accent : accentBoundaries)
            {
                KswordTheme::SetPrimaryAccentColor(accent);
                applyDockNavigationStyle(manager, transparentFallback);
                navigationColors();
                providerImages();
                navigationSurfaces(first, second);
            }
        }
    }
}

void TestDockThemeIcons()
{
    // 保存测试入口状态，避免独立夹具各段相互影响；不读取或写入用户配置。
    const bool previousDarkMode = KswordTheme::IsDarkModeEnabled();
    const QColor previousAccent = KswordTheme::PrimaryAccentColor();
    const QColor previousBackground = KswordTheme::CustomMainBackgroundColor;
    const auto previousConfig = ads::CDockManager::configFlags();
    ads::CDockManager::setConfigFlag(ads::CDockManager::DisableStylesheet, true);
    ads::CDockManager::setConfigFlag(ads::CDockManager::DockAreaHideDisabledButtons, false);
    KswordTheme::SetDarkModeEnabled(false);
    KswordTheme::SetPrimaryAccentColor(QString());

    {
        // manager/dock 是真实上游对象，仅留在离屏 QApplication，不创建生产窗口。
        ads::CDockManager manager;
        ads::CDockWidget* dock = manager.createDockWidget(QStringLiteral("fixture dock"));
        dock->setWidget(new QWidget());
        manager.addDockWidget(ads::LeftDockWidgetArea, dock);
        ks::ui::RefreshDockThemeIcons(&manager);
        const IconImages defaults = providerImages();
        checkDockButtons(dock);

        // first/second 保存两个明确不同的强调色下真实 Normal 像素，避免只测 cacheKey。
        KswordTheme::SetPrimaryAccentColor(QStringLiteral("#216d40"));
        ks::ui::RefreshDockThemeIcons(&manager);
        const IconImages first = providerImages();
        checkDockButtons(dock);
        KswordTheme::SetPrimaryAccentColor(QStringLiteral("#91329f"));
        ks::ui::RefreshDockThemeIcons(&manager);
        const IconImages second = providerImages();
        checkDockButtons(dock);
        for (std::size_t index = 0; index < first.size(); ++index)
        {
            Require(first[index] != second[index], "Normal ADS icon follows custom accent changes");
        }

        // 后创建页面必须继承最新 provider；显式 Refresh 再补齐其通用扫描例外标记。
        ads::CDockWidget* lateDock = manager.createDockWidget(QStringLiteral("late fixture dock"));
        lateDock->setWidget(new QWidget());
        manager.addDockWidget(ads::RightDockWidgetArea, lateDock);
        ks::ui::RefreshDockThemeIcons(&manager);
        checkDockButtons(lateDock);

        KswordTheme::SetPrimaryAccentColor(QString());
        ks::ui::RefreshDockThemeIcons(&manager);
        const IconImages restored = providerImages();
        Require(defaults == restored, "Restoring default accent exactly restores ADS icon pixels");
        checkDockButtons(dock);
        checkDockButtons(lateDock);
    }

    testDockNavigation();

    KswordTheme::SetDarkModeEnabled(previousDarkMode);
    KswordTheme::SetPrimaryAccentColor(previousAccent.name(QColor::HexRgb));
    KswordTheme::SetMainBackgroundColor(previousBackground.isValid()
        ? previousBackground.name(QColor::HexRgb)
        : QString());
    ads::CDockManager::setConfigFlags(previousConfig);
}
