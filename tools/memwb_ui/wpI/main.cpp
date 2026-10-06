// main.cpp
// 作用：WP-I（WorkbenchTarget / 目标与导航接入）离屏验证夹具的入口。
//
// 用法：memwb_wpI_tests.exe
//       memwb_wpI_tests.exe --wpi-exit-now   （内部用途：PID 复用探针拉起的子进程
//       自己识别到这个参数后立即退出，不构造 QCoreApplication，退出越快越能逼近
//       "短命进程"的场景。）
// 退出码：0 全部通过；1 有断言失败。
//
// 文件分工：
//   memwb_wpI_common.*         断言计数、FakeWorkbenchServices、GuardRecorder、PumpUntil、
//                              AttachFakeProcess
//   memwb_wpI_tests.Hooks.cpp  三个 Dock 钩子的顺序与 aboutToDetach 同步语义、checkLiveness
//   memwb_wpI_tests.Guard.cpp  离开守卫否决、身份变更合并一次询问、策略门、T1/T2/T3
//   memwb_wpI_tests.Ddma.cpp   session()/capture() 对 DDMA 代次的拉取规则、T5（锁定 D5）
//   memwb_wpI_tests.Revision.cpp 来源/内容代次、isStale、requestReload，T4（锁定 D2）
//   memwb_wpI_tests.Modules.cpp 异步模块枚举：陈旧票据丢弃、所有者不串、内核懒加载、T6
//   memwb_wpI_tests.Evaluate.cpp evaluate() 的各 Issue 分支、matchProcess 接线
//   memwb_wpI_tests.Anchor.cpp  锚点冒烟（真实本进程）、PID 复用探针、D3/D4 回归
//   memwb_wpI_tests.HandleLifecycle.cpp 句柄生命周期/位数接线/钉住存活，T7（真实子进程）
//   memwb_wpI_tests.Stress.cpp  D1 稳定性循环（200 次创建后立刻销毁）
//   memwb_wpI_tests.Accessors.cpp D6 存在性/创建时间校验、identityAnchored、
//                              lastIdentityFailure 复位、modulesFailed、checkLiveness
//                              范围门、onDockAboutToDetach 的"从未附加"边界
//   memwb_wpI_tests.Extra.cpp  第二轮审核报告 review2-wpI.md 的补测原文（X1-X20）
//   memwb_wpI_tests.Fix2.cpp   本轮修复新增：N1-N5 回归、revisions() 访问器、
//                              按 N4 决策改写的 X18

#include "memwb_wpI_common.h"

#include <QCoreApplication>

#include <cstring>
#include <iostream>

int main(int argc, char** argv)
{
    // PID 复用探针拉起的子进程只传这一个参数，目的是尽快退出，不构造任何 Qt 对象。
    for (int index = 1; index < argc; ++index)
    {
        if (std::strcmp(argv[index], "--wpi-exit-now") == 0)
        {
            return 0;
        }
    }

    QCoreApplication app(argc, argv);

    memwb_wpI_test::RunHooksTests();
    memwb_wpI_test::RunGuardTests();
    memwb_wpI_test::RunDdmaTests();
    memwb_wpI_test::RunRevisionTests();
    memwb_wpI_test::RunModulesTests();
    memwb_wpI_test::RunEvaluateTests();
    memwb_wpI_test::RunAnchorTests();
    memwb_wpI_test::RunHandleLifecycleTests();
    memwb_wpI_test::RunStressTests();
    memwb_wpI_test::RunAccessorTests();
    memwb_wpI_test::RunExtraTests();
    memwb_wpI_test::RunFix2Tests();

    std::cout << "memwb_ui_tests: " << memwb_wpI_test::g_checks << " checks, "
              << memwb_wpI_test::g_failures << " failures" << std::endl;
    return memwb_wpI_test::g_failures == 0 ? 0 : 1;
}
