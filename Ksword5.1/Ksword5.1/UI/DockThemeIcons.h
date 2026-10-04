#pragma once

class QWidget;

namespace ads
{
    class CDockManager;
}

namespace ks::ui
{
    // RefreshDockThemeIcons 作用：让 ADS 六类按钮图标随主题偏移更新。
    // dockManager 为当前停靠管理器；为空时直接返回；不改变按钮行为或停靠布局。
    // 创建管理器后、主题切换后以及新增停靠区域/浮动容器后调用，无返回值。
    void RefreshDockThemeIcons(ads::CDockManager* dockManager);

    // 标签本体与内部文字使用同一导航前景；在选中、hover或主题变化后调用。
    // 状态由ADS事件入口传入，颜色不变时保留已有样式，避免反复polish。
    void ApplyDockTabTextColor(QWidget* tabWidget, bool activeTab, bool hovered);
}
