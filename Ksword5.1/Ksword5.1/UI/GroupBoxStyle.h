#pragma once

#include <QGroupBox>
#include <QLayout>
#include <QMargins>
#include <QString>
#include <QtGlobal>

namespace ks::ui
{
    // 生成统一的组框标题与正文几何：独立卡片拥有16px内距，嵌入参数区借用外卡内距。
    // 标题字高参与顶部留白，字号放大时不裁切；无标题折叠区只保留4px顶部空隙。
    inline QString GroupBoxStyle(const QString& selector, const QString& background,
        const QString& titleColor, const QString& disabledTitleColor, int fontHeight,
        bool embedded = false, bool hasTitle = true)
    {
        const int textHeight = qMax(0, fontHeight);
        const int top = hasTitle ? (embedded ? textHeight + 8
            : qMax(32, 10 + textHeight + 8)) : 4;
        const int horizontal = embedded ? 0 : 16;
        const int bottom = embedded ? 4 : 16;
        const int titleLeft = embedded ? 0 : 16;
        const int titleTop = embedded ? 0 : 10;
        return QStringLiteral(
            "%1{background:%2;border:0;border-radius:%3px;margin:0;"
            "padding:%4px %5px %6px %5px;}"
            "%1::title{subcontrol-origin:padding;subcontrol-position:top left;"
            "left:%7px;top:%8px;padding:0;background:transparent;"
            "color:%9;font-weight:600;}"
            "%1::title:disabled{color:%10;}")
            .arg(selector).arg(embedded ? QStringLiteral("transparent") : background)
            .arg(embedded ? 0 : 8).arg(top).arg(horizontal).arg(bottom)
            .arg(titleLeft).arg(titleTop).arg(titleColor).arg(disabledTitleColor);
    }

    // 组框自身布局只管理区内行距；标题留白和卡片内距统一交给上方QSS拥有。
    inline void ConfigureGroupBoxLayout(QGroupBox* group)
    {
        if (!group || !group->layout()) return;
        QLayout* const layout = group->layout();
        const QMargins margins(0, 0, 0, 0);
        if (layout->contentsMargins() != margins) layout->setContentsMargins(margins);
        if (layout->spacing() != 8) layout->setSpacing(8);
    }
}
