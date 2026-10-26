#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <string>
#include "StartupSplash.h"

namespace ks::splash
{
    inline constexpr int CanvasWidth = 832; // 原生窗口与设置预览使用相同逻辑画布。
    inline constexpr float TrackHeight = 4.0f; // 进度轨道的逻辑像素高度。
    inline constexpr unsigned int DefaultAccent = 0x4C8DEBU; // 默认启动进度色。

    // 返回内置底图文件名；原生窗口与预览只从exe同级Style目录加载。
    const wchar_t* backgroundFileName(SplashBackgroundStyle background);
    // 在调用方已经配置的逻辑坐标系中完整绘制底图，不裁切或叠加遮罩。
    void drawBackground(Gdiplus::Graphics& graphics, float width, float height, Gdiplus::Image& image);
    // 两个宿主共用底轨、确定进度及颜色规则；原生不确定动画仍由原窗口管理。
    void drawTrack(Gdiplus::Graphics& graphics, float width, float height);
    void drawDeterminateProgress(Gdiplus::Graphics& graphics, float width, float height,
        int percent, unsigned int accentRgb = DefaultAccent);

    // 三种排版共享同一份原始展示数据，选择样式只改变投影，不另造发行元数据。
    struct SplashText final
    {
        std::wstring title; // 中央粗体产品名称。
        std::wstring version; // 名称下方的完整发行版本与原始阶段大小写。
        std::wstring copyright; // 右下角精简版权。
        SplashLayoutStyle layout = SplashLayoutStyle::VerticalCompact; // 缺省保持当前简化版。
        std::wstring buildTime; // 完整版使用的构建于日期与时间。
        std::wstring authors; // 完整版使用的作者名单。
        std::wstring license; // 左右布局的许可正文。
        std::wstring thirdParty; // 左右布局的第三方组件说明。
    };

    // 在完整逻辑画布中绘制中央品牌及版权，返回避让版权后的动态状态恢复区域。
    Gdiplus::RectF drawStatic(Gdiplus::Graphics& graphics, float width, float height,
        Gdiplus::Image* logo, const SplashText& text);

    // 状态位于返回区域左侧；缩放、单行省略和可见中心线与静态布局保持一致。
    void drawStatus(Gdiplus::Graphics& graphics, const Gdiplus::RectF& bounds,
        const std::wstring& status);
}
