#pragma once

// ============================================================
// wpJ3_common.h
// 作用：WP-J3（WorkbenchBaselineFeeder + HexCanvas::settledPageStartsInRange 前置
//       小改）离屏验证夹具的公共设施——轻量断言计数、不响应的页提供者（驱动画布
//       自动请求可见页时让它们停在"在途"状态，供构造 Pending 场景）、按页填充画布
//       缓存的小工具函数、等到条件成立的事件泵、以及记录 Feeder 两个信号的录像机。
// 说明：
// - 本目录（tools/memwb_ui/wpJ3/）是 WP-J3 专属夹具目录，自己的产物目录
//   （MEMWB_OUT，默认 .codex-tmp/memwb-wpJ3），不与 tools/memwb_ui/ 下既有的
//   HexCanvas 夹具或其它 wp* 包共用任何产物目录。
// - 命名空间 memwb_wpJ3_test，与既有夹具的 memwb_test / memwb_wpI_test 等区分，
//   避免任何名字混淆。
// - 本文件只 #include 真实的 HexCanvas.h / WorkbenchBaselineFeeder.h 与 Core 的
//   MemoryBaselineWindow.h / MemoryDiffOverlay.h，不使用任何假 Feeder/假
//   Overlay——这两个类本身都足够轻量（Overlay 是 Qt-free 纯算法类，HexCanvas 是
//   自绘画布但不需要真实显示), 直接用真实对象驱动最接近生产行为。
// ============================================================

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/HexCanvas.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchBaselineFeeder.h"

#include "../../../shared/evidence/memory_workbench/MemoryDiffOverlay.h"

#include <QByteArray>
#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <set>
#include <vector>

namespace memwb_wpJ3_test
{
    // HexCanvas/WorkbenchBaselineFeeder 都定义在 ks::ui 命名空间里；本夹具文件集中
    // 用这几个 using 声明把它们引入本命名空间，避免后面每处都写 ks::ui:: 前缀
    // （仍然保留 ksword::memwb:: 前缀，那些类型本来就分散在多个头文件里，前缀能
    // 提醒读者"这是 Core 层的类型"）。
    using ks::ui::HexCanvas;
    using ks::ui::IHexPageProvider;
    using ks::ui::HexFetchRange;
    using ks::ui::WorkbenchBaselineFeeder;

    // ---- 断言计数：与既有夹具同样的轻量框架，本包独立计数器。----
    extern int g_checks;
    extern int g_failures;

    // Report：记录一条断言结果，失败时向 stderr 打印位置、表达式与附加说明。
    // 传入：ok 断言是否成立；expression 断言源文本；file/line 发生位置；note 附加说明
    // （可为空串）。传出：无，累加 g_checks/g_failures。
    void Report(bool ok, const char* expression, const char* file, int line, const QString& note);

#define WPJ3_CHECK(expression) \
    ::memwb_wpJ3_test::Report(static_cast<bool>(expression), #expression, __FILE__, __LINE__, QString())
#define WPJ3_CHECK_NOTE(expression, note) \
    ::memwb_wpJ3_test::Report(static_cast<bool>(expression), #expression, __FILE__, __LINE__, (note))

    // kPageBytes：与 HexViewport::kPageBytes 保持一致，供测试代码按页对齐算地址用。
    inline constexpr std::uint64_t kPageBytes = 4096ULL;

    // NoOpPageProvider：什么都不做的页提供者。
    // 用途：挂到 HexCanvas 上之后，画布自己的"规划可见页并请求"机制
    // （HexCanvas::requestVisiblePages，发生在 setAddressSpace / 滚动时）仍然会把
    // 可见范围附近的页登记为"在途"（Pending），但因为 RequestPages 什么都不回填，
    // 这些页会一直停在 Pending，不会被测试代码意外"顺手"变成 Valid——这正是用来
    // 构造"Pending/NotLoaded"两种状态而不需要手写假数据的办法：画布视口附近（本夹具
    // 统一让地址空间从 0 开始，初始可见范围落在 0 附近）的页自动变成 Pending；离视口
    // 很远、从没被显式 deliverPage/deliverUnreadable 过的页天然保持 NotLoaded。
    class NoOpPageProvider final : public ks::ui::IHexPageProvider
    {
    public:
        // RequestPages：故意什么都不做（不调用 deliverPage/deliverUnreadable/
        // cancelPages），让传入的范围保持"在途"状态。
        void RequestPages(const std::vector<ks::ui::HexFetchRange>& ranges, std::uint64_t sourceRevision) override
        {
            (void)ranges;
            (void)sourceRevision;
        }
    };

    // SetupCanvas：按统一顺序装好一个画布——先挂 NoOpPageProvider 再设地址空间
    // （setPageProvider 内部会调用 refresh()，此时还没有地址空间，是空操作；随后
    // setAddressSpace 触发的首次 requestVisiblePages 才会真正用到这个提供者）。
    // 传入：canvas 目标画布；provider 配套的不响应提供者（调用方持有其生命周期，
    //       必须比 canvas 活得久或至少同生命周期，因为 canvas 只存非拥有指针）；
    //       firstAddress/lastAddress 地址空间闭区间。
    void SetupCanvas(
        HexCanvas& canvas,
        NoOpPageProvider& provider,
        std::uint64_t firstAddress,
        std::uint64_t lastAddress);

    // DeliverValidPage：把一整页喂成"完全读到"（Valid），每个字节都等于 fillValue。
    // 传入：canvas 目标画布；pageStart 页起始地址（必须页对齐）；fillValue 填充值。
    // 传出：HexCanvas::PageResult（调用方一般断言为 Accepted）。
    HexCanvas::PageResult DeliverValidPage(HexCanvas& canvas, std::uint64_t pageStart, std::uint8_t fillValue);

    // DeliverPartialPage：把一整页喂成"部分字节读到"（页本身已落定，但某些字节的
    // 有效掩码为 0，查询时状态是 Unreadable）。
    // 传入：canvas 目标画布；pageStart 页起始地址；fillValue 有效字节的填充值；
    //       validPrefixBytes 页内前多少字节标为有效（其余标为无效，取值范围
    //       [0, 4096]，0 表示整页都无效但仍然是"已缓存"的部分有效页）。
    HexCanvas::PageResult DeliverPartialPage(
        HexCanvas& canvas,
        std::uint64_t pageStart,
        std::uint8_t fillValue,
        std::size_t validPrefixBytes);

    // DeliverUnreadablePage：把一整页标成"整页不可读"（Unreadable，与"部分有效"是
    // 两种不同的落定方式：前者从未成功读到任何字节，后者至少成功读到了一部分）。
    // 传入：canvas 目标画布；pageStart 页起始地址。
    HexCanvas::PageResult DeliverUnreadablePage(HexCanvas& canvas, std::uint64_t pageStart);

    // DeliverCustomPage：把一整页喂成"完全读到"，但字节内容由调用方逐字节指定
    // （不是统一填充值），供需要"大部分字节不变、少数字节变化"场景的测试使用
    // （例如验证 RefreshBaseline 之后 ExternalChange/Unchanged 的逐字节区分）。
    // 传入：canvas 目标画布；pageStart 页起始地址；bytes 正好 4096 字节的内容。
    // 传出：HexCanvas::PageResult；bytes 长度不是 4096 时直接返回
    //       HexCanvas::PageResult::RejectedBadSize，不调用 deliverPage。
    HexCanvas::PageResult DeliverCustomPage(HexCanvas& canvas, std::uint64_t pageStart, const QByteArray& bytes);

    // PumpUntil：反复处理一次 Qt 事件循环直到 predicate 为真或超时。
    // 用途：WorkbenchBaselineFeeder 的 50 ms 防抖定时器需要真实的事件循环才会触发，
    // 测试要等待 onDebounceTimeout 真正执行完。
    // 传入：predicate 判定是否已达成；timeoutMs 最长等待毫秒数。
    // 传出：达成返回 true；超时仍未达成返回 false（调用方应据此判失败，不能把超时
    //       静默当成功）。
    bool PumpUntil(const std::function<bool()>& predicate, int timeoutMs);

    // BaselineRecorder：记录 WorkbenchBaselineFeeder 两个信号（baselineRefreshed /
    // baselineUnavailable）各自被触发的次数与最近一次的参数，供测试断言"喂了几次、
    // 喂的是哪一段"或"没喂、原因是什么"。
    class BaselineRecorder
    {
    public:
        // Attach：订阅 feeder 的两个信号，连接到 context（必须比 recorder 活得久，
        // 一般直接传 feeder 自己或测试函数里的一个局部 QObject）。
        void Attach(ks::ui::WorkbenchBaselineFeeder& feeder, QObject& context);

        // Reset：清空全部计数与记录的参数，不解除信号连接。
        void Reset();

        // refreshedCount / unavailableCount：各自被触发的累计次数。
        int refreshedCount = 0;
        int unavailableCount = 0;
        // lastBase / lastLength：最近一次 baselineRefreshed 的参数。
        std::uint64_t lastBase = 0;
        std::uint64_t lastLength = 0;
        // lastStatus：最近一次 baselineUnavailable 的参数。
        ksword::memwb::BaselineSpanStatus lastStatus = ksword::memwb::BaselineSpanStatus::Ok;
    };

    // 各组测试入口（定义在对应的 wpJ3_tests.*.cpp，由 main.cpp 依次调用）。
    void RunSettledPagesTests();
    void RunDebounceTests();
    void RunDecisionTests();
    void RunLifecycleTests();
    // RunRegressionTests：独立审核报告确认的 D1/D2/D3 缺陷回归测试，以及审核者
    // 变异测试揪出的原夹具真实缺口补测（见 wpJ3_tests.Regression.cpp 文件头）。
    void RunRegressionTests();
    // RunReview2Tests：第二轮独立审核报告补测核心组——堵第二轮新变异 C 系列
    // （C1/C2/C2b/C7b/C8/C9/C13/C14/C42）与第一轮 V04 节流重放，另加本包自补的
    // 跨周期回归（见 wpJ3_tests.Review2.cpp 文件头）。
    void RunReview2Tests();
    // RunReview2DefectTests：第二轮确认的新缺陷 N1/N2（无挂起期脏事件的变体）/N3
    // 回归测试，修复后应全部通过，并入默认运行。
    void RunReview2DefectTests();
}
