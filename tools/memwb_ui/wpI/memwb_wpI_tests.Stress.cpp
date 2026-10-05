// memwb_wpI_tests.Stress.cpp
// 作用：锁定 D1（ModuleEnumTask 在工作线程把裸指针交给 invokeMethod 造成的
// 释放后使用）的专门稳定性测试——循环创建 WorkbenchTarget、发起一次异步模块
// 枚举、立刻销毁对象（不等待任何事件循环），重复 200 次。
//
// 审核报告 D1 的压力探针显示：未修复代码在类似形态下约 2~3/100 次会以
// 0xC0000005（访问冲突）整个进程崩溃退出；用下文"修法"打补丁后压力探针与整套
// 夹具均 0 次崩溃。本测试把这个压力形态固定下来，专门反复跑、不混在功能测试里。
//
// 判据：本测试"通过"等于进程能跑完整个循环并继续执行到 main.cpp 打印汇总行。
// 如果 D1 复发，进程会在某次迭代里直接崩溃退出，连汇总行都不会打印；
// mutrun.ps1 一类的调用方会把"没有汇总行"的异常退出判成 CRASH_OR_NOSUMMARY
// （真正被抓到），而不是被误判成"0 failures"的假通过——这正是这类缺陷必须靠
// "进程还活着"而不是"某个断言为真"来判定的原因。

#include "memwb_wpI_common.h"

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchTarget.h"

#include <memory>

namespace memwb_wpI_test
{
    void RunStressTests()
    {
        constexpr int kIterations = 200;
        for (int i = 0; i < kIterations; ++i)
        {
            auto owned = std::make_unique<FakeWorkbenchServices>();
            // 内核模块枚举不配置任何延迟：工作线程几乎立刻就能跑到"结果要投递回
            // UI 线程"那一步，紧接着对象就被销毁——这正是 D1 复现所需要的、
            // "从 QPointer 取出判空结果到回调真正排队执行之间"的那段竞争窗口。
            owned->SetKernelModulesResult(ks::ui::ModuleEnumResult{true, {}, {}});
            auto target = std::make_unique<ks::ui::WorkbenchTarget>(std::move(owned));

            // 切到内核范围：没有注册离开守卫（无视图=放行），会同步触发一次内核
            // 模块的懒加载，在工作线程上异步跑；调用立即返回，不等待任何结果。
            target->requestScope(ksword::memwb::Scope::KernelVirtual);

            // 立刻销毁，不调用 QCoreApplication::processEvents，不 PumpUntil——
            // 故意不给在途的工作线程任务任何"先知道对象已经不在了"的机会。
            target.reset();
        }

        // 跑到这里说明 200 次循环全部没有崩溃。真正验证"陈旧/迟到结果被安全
        // 丢弃"这条逻辑性质已经在 memwb_wpI_tests.Modules.cpp 的陈旧票据测试里
        // 覆盖过，本文件只关心这一件事：不崩。
        WPI_CHECK_NOTE(
            true,
            QStringLiteral("D1 稳定性循环：%1 次\"创建后立刻销毁\"均未触发访问冲突").arg(kIterations));
    }
}
