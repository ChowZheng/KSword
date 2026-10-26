#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <string>

namespace ks::splash
{
    // FooterText 只保存当前启动页需要显示的文字；调用方在进入UI线程前完成本地化。
    struct FooterText final
    {
        std::wstring title; // 产品名称由调用方提供，不从版本字符串中解析。
        std::wstring version; // 原始发行版本，数字与PRE等阶段在页脚分别绘制。
        std::wstring buildTime; // 已本地化的构建标签、日期与时间。
        std::wstring authors; // 两位作者沿发行元数据的分隔符传入。
        std::wstring copyright; // 右侧短版权说明。
        std::wstring website; // 右侧站点地址，绘制时只显示域名或路径。
        unsigned int accentRgb = 0x4C8DEBU; // 阶段徽标与主进度共用的RGB强调色。
    };

    constexpr float FooterHeight = 270.0f; // 所有页脚坐标均相对这块逻辑区域。

    // drawFooter 绘制静态文字与图标；Graphics 应已设置DPI和页脚贴底变换。
    void drawFooter(Gdiplus::Graphics& graphics, float width, Gdiplus::Image* logo,
        const FooterText& text);

    // statusBounds 返回页脚局部坐标中的固定动态区域，供调用方恢复静态像素。
    Gdiplus::RectF statusBounds(float width);

    // drawStatus 只绘制本次状态与真实百分比；不可确定阶段由调用方传入false。
    void drawStatus(Gdiplus::Graphics& graphics, float width, const std::wstring& status,
        bool showPercent, int percent);
}
