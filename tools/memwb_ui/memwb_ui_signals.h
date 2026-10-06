#pragma once

// ============================================================
// memwb_ui_signals.h
// 作用：第二轮接口补全（contentChanged / editableChanged / stageBytes / 视口坐标命中 / 不可读占位符 / 填充图标）
//       离屏夹具的公共设施——处理一轮事件、按文字找菜单项、手动供页的异步画布、字形形状比较，
//       以及三组测试的入口。只被 memwb_ui_tests.Signals*.cpp 使用，不属于主程序。
// 文件分工：
//   memwb_ui_tests.Signals.cpp           公共设施定义 + contentChanged / editableChanged 测试 + 总入口 RunSignalTests
//   memwb_ui_tests.Signals.Stage.cpp     公开 stageBytes 的成功/拒绝路径、入口共用检查、超大选区填充
//   memwb_ui_tests.Signals.View.cpp      视口坐标命中、不可读占位符（含像素实测）、填充图标、截图
//   memwb_ui_tests.Signals.Inspector.cpp 解释器面板侧的信号接线（无定时器、订阅信号、无重复信号）
// ============================================================

#include "memwb_ui_inspector.h"

#include <QAction>
#include <QImage>
#include <QMenu>
#include <QRect>

#include <cstdint>
#include <memory>
#include <vector>

namespace memwb_test
{
    // Flush：处理一轮事件，让排队的 contentChanged 发出。
    void Flush();

    // FindAction：按文字在菜单里找动作，找不到返回空指针。
    QAction* FindAction(QMenu* menu, const QString& text);

    // Effective：叠加后的字节视图（从叠加层 Materialize 取）。
    // 传入：叠加层、起始地址、字节数；传出：叠加后的字节。
    std::vector<std::uint8_t> Effective(
        const ksword::memwb::MemoryDiffOverlay& overlay,
        std::uint64_t address,
        std::uint64_t length);

    // AsyncCanvas：页由测试手动投递的画布，叠加层基线与 MakePattern 一致。
    // 成员声明顺序决定析构顺序：画布先于叠加层与提供者销毁。
    struct AsyncCanvas
    {
        ksword::memwb::MemoryDiffOverlay overlay;           // 暂存叠加层
        RecordingProvider provider;                         // 记录型页提供者（只记录，不回填）
        std::unique_ptr<ks::ui::HexCanvas> canvas;          // 被测画布
    };

    // MakeAsyncCanvas：构造地址空间 [first, last] 的异步画布（宽 900、高 420，已显示）。
    // 传入：首末地址、是否可编辑、是否把 MakePattern 数据载入叠加层基线（基线覆盖整个空间，掩码全 1）。
    std::unique_ptr<AsyncCanvas> MakeAsyncCanvas(std::uint64_t first, std::uint64_t last, bool editable, bool loadBaseline);

    // DeliverPage：向画布投递一页有效数据（MakePattern 内容，掩码全 1），返回接收结果。
    ks::ui::HexCanvas::PageResult DeliverPage(ks::ui::HexCanvas& canvas, std::uint64_t pageStart);

    // InkPixels：区域内与给定底色不同的像素数（"有字"的正向对照）。
    int InkPixels(const QImage& image, const QRect& rect, const QColor& background);

    // InkDifference：比较两个单元格里"字形的形状"，与文字颜色无关。
    // 逐像素判断"是不是字迹"（与底色不同），统计一个有字迹、另一个没有的位置数。
    // 为什么不直接比像素：不可读与真实字符的文字颜色本来就不同（禁用色 vs 正文色），
    // 直接比像素即使两格画的是同一个字形也会"不同"，抓不到把占位符改回 '?' 的退化。
    int InkDifference(
        const QImage& leftImage,
        const QRect& leftRect,
        const QImage& rightImage,
        const QRect& rightRect,
        const QColor& background);

    // 三组测试的入口（定义在各自的 .cpp，由 RunSignalTests 依次调用）。
    void RunSignalContentTests();
    void RunSignalStageTests();
    void RunSignalViewTests(const QString& shotsDir);
}
