// ============================================================
// wpK1_main.cpp
// 作用：WP-K1 纯函数夹具入口，依次运行各分组并输出汇总行。
// 用法：wpK1_tests.exe
// 退出码：0 全部通过；1 有断言失败。终端汇总行恰好是 "wpK1_tests: N checks, M failures"。
// ============================================================

#include "wpK1_common.h"

#include <iostream>

int main()
{
    wpK1_test::RunMappingModuleTests();
    wpK1_test::RunMappingPointerTests();
    wpK1_test::RunMappingMiscTests();
    wpK1_test::RunAuditClassifyTests();
    wpK1_test::RunAuditFormatTests();
    wpK1_test::RunAuditRegistryTests();
    wpK1_test::RunAuditSinkTests();
    wpK1_test::RunProbeTrackerTests();
    wpK1_test::RunProbeDecisionTests();

    std::cout << "wpK1_tests: " << wpK1_test::g_checks << " checks, "
              << wpK1_test::g_failures << " failures" << std::endl;
    return wpK1_test::g_failures == 0 ? 0 : 1;
}
