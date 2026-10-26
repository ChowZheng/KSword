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
    // 静态展示数据只包含新设计实际显示的三项，其余发行资料仍由原公共接口保留。
    struct SplashText final
    {
        std::wstring title; // 中央粗体产品名称。
        std::wstring version; // 名称下方的完整发行版本与原始阶段大小写。
        std::wstring copyright; // 右下角精简版权。
    };

    // 在完整逻辑画布中绘制中央品牌及版权，返回避让版权后的动态状态恢复区域。
    Gdiplus::RectF drawStatic(Gdiplus::Graphics& graphics, float width, float height,
        Gdiplus::Image* logo, const SplashText& text);

    // 状态位于返回区域左侧；缩放、单行省略和可见中心线与静态布局保持一致。
    void drawStatus(Gdiplus::Graphics& graphics, const Gdiplus::RectF& bounds,
        const std::wstring& status);
}
