#pragma once

#include <QIcon>

namespace ks::ui
{
    // MakeThemeAccentIcon：为应用内单色源图建立绘制时读取主题强调色的图标。
    // sourceIcon 必须保留固定默认色轮廓；只用于自制图标，不用于 Shell/进程多色图标。
    // 返回拥有独立 QIconEngine 的图标；空源图原样返回，主题变化无需重建模型项。
    QIcon MakeThemeAccentIcon(const QIcon& sourceIcon);
}
