// main.cpp
// 作用：WP-J3（WorkbenchBaselineFeeder + HexCanvas::settledPageStartsInRange 前置
// 小改）离屏验证夹具的入口。
//
// 用法：wpJ3_tests.exe
// 退出码：0 全部通过；1 有断言失败。
//
// 文件分工：
//   wpJ3_common.*               断言计数、NoOpPageProvider、画布填页小工具、
//                                PumpUntil 事件泵、BaselineRecorder 信号录像机
//   wpJ3_tests.SettledPages.cpp HexCanvas::settledPageStartsInRange：四种页状态、
//                                跨页边界、LRU 不被刷新、first>last 与极端区间
//   wpJ3_tests.Debounce.cpp     noteDirty 的 50ms 重置式防抖、hasInFlightRequests
//                                门控、flushNow 跳过防抖并取消待定定时器、
//                                canvas/overlay 任一为空时的空操作
//   wpJ3_tests.Decision.cpp     Keep/RefreshSameSpan/Recompute、
//                                baselineUnavailable 两种原因、identityKey 变化
//                                不续喂旧窗口、setPolicy/setAddressSpaceBounds
//                                使记录失效、窗口跟随跨页/跨地址空间边界、
//                                窗口页数上限、重读后 previous 保留使
//                                ExternalChange 成立
//   wpJ3_tests.Lifecycle.cpp    空白构造、QPointer 悬空保护、overlay 置空再恢复、
//                                析构时有未到期防抖定时器、lastWindow() 访问器
//   wpJ3_tests.Regression.cpp  独立审核报告确认的 D1（在途取消不了已起算的定时
//                                器）/D2（record_ 不与叠加层真实基线对账）/D3
//                                （没有挂起入口）三个缺陷的回归测试，以及审核者
//                                变异测试揪出的原夹具真实缺口补测
//   wpJ3_tests.Review2.cpp     第二轮独立审核报告补测：核心组堵第二轮新变异
//                                C 系列与 V04 节流重放；缺陷组是 N1/N2/N3 的回归
//                                （恢复无视在途位、flushNow 不受挂起门控、恢复后
//                                凭旧缓存重算）

#include "wpJ3_common.h"

#include <QApplication>

#include <iostream>

int main(int argc, char** argv)
{
    // HexCanvas 是 QWidget，需要真正的 QApplication（不是 QCoreApplication）才能
    // 构造；离屏运行靠 QT_QPA_PLATFORM=offscreen（由 build-wpJ3-tests.cmd 设置），
    // 这里不需要也不应该调用 show()。
    QApplication app(argc, argv);

    memwb_wpJ3_test::RunSettledPagesTests();
    memwb_wpJ3_test::RunDebounceTests();
    memwb_wpJ3_test::RunDecisionTests();
    memwb_wpJ3_test::RunLifecycleTests();
    memwb_wpJ3_test::RunRegressionTests();
    memwb_wpJ3_test::RunReview2Tests();
    memwb_wpJ3_test::RunReview2DefectTests();

    std::cout << "wpJ3_tests: " << memwb_wpJ3_test::g_checks << " checks, "
              << memwb_wpJ3_test::g_failures << " failures" << std::endl;
    return memwb_wpJ3_test::g_failures == 0 ? 0 : 1;
}
