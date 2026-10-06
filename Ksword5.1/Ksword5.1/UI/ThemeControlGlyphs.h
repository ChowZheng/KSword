#pragma once

#include <QColor>
#include <QString>

namespace ks::ui
{
    // ThemedControlGlyphPath 作用：将单色控件 SVG 以指定主题前景色缓存为文件，供 QSS url 使用。
    // resourcePath 为资源 SVG 路径；foregroundColor 为已按实际底色校准的前景色。
    // cacheDirectory 留空时使用应用缓存目录，离屏测试可指定仓库内目录；失败回退原资源。
    // 返回 QFile 可读取的原始路径，写入 QSS 时调用方必须为 url 的路径加双引号。
    QString ThemedControlGlyphPath(
        const QString& resourcePath,
        const QColor& foregroundColor,
        const QString& cacheDirectory = QString());
}
