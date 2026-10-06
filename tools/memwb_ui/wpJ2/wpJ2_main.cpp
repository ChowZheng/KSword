// ============================================================
// wpJ2_main.cpp
// 作用：WP-J2（WorkbenchPageProvider）离屏验证夹具的入口。链接真实
// HexCanvas（QWidget 子类），因此用 QApplication 而不是 QCoreApplication
// （仿 tools/memwb_ui/memwb_ui_tests.cpp 的写法），配合 offscreen 平台插件
// 在无显示环境下运行。
//
// 用法：wpJ2_tests.exe
// 退出码：0 全部通过；1 有断言失败。终端汇总行恰好是
//         "wpJ2_tests: N checks, M failures"（跑法脚本据此解析）。
// ============================================================

#include "wpJ2_common.h"

#include <QApplication>

#include <iostream>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    wpJ2_test::RunGateTests();
    wpJ2_test::RunDeliveryTests();
    wpJ2_test::RunStaleTests();
    wpJ2_test::RunChannelFailedTests();
    wpJ2_test::RunLatchTests();
    wpJ2_test::RunSerializeTests();
    wpJ2_test::RunLifecycleTests();
    wpJ2_test::RunSuppTests();
    wpJ2_test::RunDecision2Tests();
    wpJ2_test::RunSupp2Tests();

    std::cout << "wpJ2_tests: " << wpJ2_test::g_checks << " checks, "
              << wpJ2_test::g_failures << " failures" << std::endl;
    return wpJ2_test::g_failures == 0 ? 0 : 1;
}
