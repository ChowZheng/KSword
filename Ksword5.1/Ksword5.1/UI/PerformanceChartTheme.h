#pragma once

#include "../theme.h"
#include <QPalette>

class QChart;

namespace ks::ui
{
    // RefreshPerformanceChartTheme 作用：按已知曲线角色重算性能图颜色，保留数据与透明度。
    // chart 为已有图表；primaryRole/secondaryRole 是按创建顺序登记的曲线角色。
    // 单曲线图两项传同一角色；surfacePalette 可指定独立浮窗的底色与文字角色。
    // 函数不采样、不改坐标范围，也不依赖旧 RGB 的反向匹配或修改全局 palette。
    void RefreshPerformanceChartTheme(
        QChart* chart,
        KswordTheme::PerformanceRole primaryRole,
        KswordTheme::PerformanceRole secondaryRole,
        const QPalette* surfacePalette = nullptr);
}
