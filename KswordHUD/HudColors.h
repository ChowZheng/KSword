#pragma once

#include <QColor>
#include <QtGlobal>
#include <cmath>

// HUD 独立配色工具：只计算颜色，不读取主程序主题或改变用户背景配置。
namespace KswordHudColors
{
    // Composite 输入覆盖色和不透明底色，按实际 alpha 混合后返回不透明可见色。
    inline QColor Composite(const QColor& overlay, const QColor& background)
    {
        const double weight = overlay.alphaF();
        return QColor(
            qRound(overlay.red() * weight + background.red() * (1.0 - weight)),
            qRound(overlay.green() * weight + background.green() * (1.0 - weight)),
            qRound(overlay.blue() * weight + background.blue() * (1.0 - weight)));
    }

    // Luminance 将 sRGB 通道线性化，返回用于前景/背景对比计算的相对亮度。
    inline double Luminance(const QColor& color)
    {
        const auto linear = [](const int channel) {
            const double value = channel / 255.0;
            return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * linear(color.red())
            + 0.7152 * linear(color.green())
            + 0.0722 * linear(color.blue());
    }

    // Contrast 输入实际可见颜色，返回对比比值；alpha 必须先通过 Composite 合成。
    inline double Contrast(const QColor& foreground, const QColor& background)
    {
        const double first = Luminance(foreground);
        const double second = Luminance(background);
        return (qMax(first, second) + 0.05) / (qMin(first, second) + 0.05);
    }

    // Readable 保留合格原色，否则只调整 HSL 亮度；文字默认 4.5，图线可传 3.0。
    inline QColor Readable(const QColor& preferred, const QColor& background, const double minimum = 4.5)
    {
        QColor candidate = preferred;
        candidate.setAlpha(255);
        if (Contrast(candidate, background) >= minimum)
        {
            return candidate;
        }
        const QColor black(Qt::black);
        const QColor white(Qt::white);
        const bool lighten = Contrast(white, background) >= Contrast(black, background);
        int hue = -1;
        int saturation = 0;
        int lightness = 0;
        candidate.getHsl(&hue, &saturation, &lightness);
        for (int offset = 1; offset <= 255; ++offset)
        {
            QColor adjusted;
            adjusted.setHsl(hue, saturation, qBound(0, lightness + (lighten ? offset : -offset), 255));
            if (Contrast(adjusted, background) >= minimum)
            {
                return adjusted;
            }
        }
        return lighten ? white : black;
    }

    // UsageHighlight 输入采样比例，返回与进程资源轨道实际绘制共用的半透明覆盖色。
    inline QColor UsageHighlight(const double ratio)
    {
        return QColor(46, 139, 255, static_cast<int>(24.0 + qBound(0.0, ratio, 1.0) * 146.0));
    }

    // LegibleOverlay 在单一文字色无法同时覆盖轨道两种底色时降低覆盖 alpha，保留色相。
    inline QColor LegibleOverlay(const QColor& overlay, const QColor& background, const QColor& text)
    {
        QColor adjusted = overlay;
        while (adjusted.alpha() > 0 && Contrast(text, Composite(adjusted, background)) < 4.5)
        {
            adjusted.setAlpha(adjusted.alpha() - 1);
        }
        return adjusted;
    }

    // SelectedText 兼顾蓝色选中底、白色轨道和资源填充，不改普通行的用户字体设置。
    inline QColor SelectedText(const QColor& preferred, const QColor& background, const double usage = -1.0)
    {
        const QColor selected = Composite(QColor(89, 139, 214, 88), background);
        const QColor track = Composite(QColor(255, 255, 255, 18), selected);
        const QColor filled = Composite(UsageHighlight(usage), track);
        const auto readableOnAll = [&](const QColor& candidate) {
            return Contrast(candidate, selected) >= 4.5
                && (usage < 0.0 || (Contrast(candidate, track) >= 4.5 && Contrast(candidate, filled) >= 4.5));
        };
        QColor candidate = preferred;
        candidate.setAlpha(255);
        if (readableOnAll(candidate))
        {
            return candidate;
        }
        const bool lighten = Luminance(selected) < 0.179;
        int hue = -1;
        int saturation = 0;
        int lightness = 0;
        candidate.getHsl(&hue, &saturation, &lightness);
        for (int offset = 1; offset <= 255; ++offset)
        {
            QColor adjusted;
            adjusted.setHsl(hue, saturation, qBound(0, lightness + (lighten ? offset : -offset), 255));
            if (readableOnAll(adjusted))
            {
                return adjusted;
            }
        }
        // 极端图片区域仍需实机核验；不可达的多底约束退回最可读的单底文字色。
        return Readable(candidate, selected);
    }
}
