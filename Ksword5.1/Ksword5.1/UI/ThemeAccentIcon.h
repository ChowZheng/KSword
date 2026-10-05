#pragma once

#include <QIcon>
#include <QColor>

namespace ks::ui
{
    // MakeThemeAccentIcon：为应用内单色源图建立绘制时读取主题强调色的图标。
    // sourceIcon 必须保留固定默认色轮廓；只用于自制图标，不用于 Shell/进程多色图标。
    // 返回拥有独立 QIconEngine 的图标；空源图原样返回，主题变化无需重建模型项。
    QIcon MakeThemeAccentIcon(const QIcon& sourceIcon);

    // fixedAccent 为通用图标管理器收到的主体色快照；底色仍在绘制时读取当前主题。
    // Normal覆盖中性表面，Active覆盖按钮强调底，Selected/On覆盖选中底；自管图标不走此入口。
    QIcon MakeThemeAccentIcon(const QIcon& sourceIcon, const QColor& fixedAccent);
}
