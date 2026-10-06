// wpJ1_main.cpp
// 作用：WP-J1（WorkbenchShared 进程级单例）离屏验证夹具的入口。
//
// 单例跨场景污染的取舍（任务书 §自查清单(f)/类注释原文都要求写清楚）：
// WorkbenchShared::Instance() 是一个"首次调用时惰性创建、此后整个进程生命周期内只有
// 这一份"的静态局部变量，而且 Configure() 的契约就是"只允许成功一次"。如果把本包的
// 全部断言都塞进同一个进程/同一次 main() 里跑，第一个调用 Configure() 的场景就会把
// 单例变成"已配置"状态，后面所有"验证未 Configure 时的退化行为""验证第一次 Configure
// 真的生效"这类场景就再也不可能被真实复现——不是用例写错了，是单例本身在语义上
// 不允许"重来一次"。头文件也没有预留任何 ResetForTesting 之类的后门（本包的任务书
// 明确说明头文件冻结，不允许为了测试新增公开接口）。
//
// 本夹具的做法：每个场景各自在一个**独立的操作系统进程**里跑（本可执行文件既是
// "编排者"又是"工作者"，用 --scenario <名字> 区分两种模式）。build-wpJ1-tests.cmd
// 负责按固定的场景名单单独起一次本可执行文件、把各次的 stdout 拼接起来，再用
// findstr 统计 "CHECK OK"/"CHECK FAIL" 的总行数，拼出批内其它包同样格式的汇总行
// "wpJ1_tests: N checks, M failures"。这个职责分工（"谁负责跨进程编排"）特意放在
// .cmd 脚本而不是本文件里用 QProcess 自己拉起子进程，理由：批处理脚本本来就要负责
// 部署 DLL、设置环境变量，由它顺手再多起几次进程、用 findstr 做最终计数，比在 C++
// 里重新发明一遍"捕获子进程 stdout、解析、汇总"更少代码、更容易被人工核对。
// 这个取舍也记在报告 impl-wpJ1.md 的"已知局限"一节。
//
// 用法：
//   wpJ1_tests.exe --scenario <场景名>   仅跑这一个场景，打印若干行 "CHECK OK/FAIL ..."，
//                                        退出码 0 表示该场景内部全部通过，1 表示至少一条失败。
//   不带参数直接运行（不建议）：打印用法说明并返回 2——本可执行文件本身不是一个可以
//                                        一次性跑完整套断言的"全量模式"，必须配合
//                                        build-wpJ1-tests.cmd 的外层循环使用。

#include "wpJ1_common.h"

#include <QCoreApplication>

#include <cstdio>
#include <cstring>

int main(int argc, char** argv)
{
    QString scenario;
    for (int index = 1; index + 1 < argc; ++index)
    {
        if (std::strcmp(argv[index], "--scenario") == 0)
        {
            scenario = QString::fromLocal8Bit(argv[index + 1]);
            break;
        }
    }

    if (scenario.isEmpty())
    {
        std::printf("usage: wpJ1_tests.exe --scenario <name>\n");
        std::printf("由 build-wpJ1-tests.cmd 按固定场景名单逐个调用本可执行文件，\n");
        std::printf("不带参数直接运行不会跑任何断言。\n");
        return 2;
    }

    // InstanceBeforeAppHooksOnConfigure 与 R2DefectRejectedConfigureDoesNotHookQuit
    // 都需要同一种"反常顺序"：必须在构造 QCoreApplication 之前就先调用一次
    // Instance()，构造函数那次 HookQuitIfNeeded() 才会因为没有 app 而被跳过——前者
    // 对应 wave3 审核 C5 修法的第二部分（Configure() 要能补接 aboutToQuit，覆盖
    // "Instance() 先于 QCoreApplication 构造"这种边界情况），后者对应 wave3 第二轮
    // 审核修复 D3（要分清"构造函数接的线"与"被拒绝的 Configure 是否偷偷接了线"，
    // 必须先排除掉构造函数这条来源）。不能像下面的默认分支那样先建 app 再派发。
    if (scenario == QStringLiteral("InstanceBeforeAppHooksOnConfigure")
        || scenario == QStringLiteral("R2DefectRejectedConfigureDoesNotHookQuit"))
    {
        ks::ui::WorkbenchShared::Instance();
        QCoreApplication app(argc, argv);
        const bool ok = wpj1_test::RunScenario(scenario);
        return ok ? 0 : 1;
    }

    // R2DefectAppRecreateReHooks（wave3 第二轮审核修复 D4）需要在同一个进程里先后
    // 构造两个互不重叠的 QCoreApplication 实例，main() 这里完全不替它构造任何 app
    // ——场景函数自己用一份文件级 static 的 argc/argv 管理两次构造/销毁，见
    // wpJ1_scenarios_r2b.cpp 顶部注释。
    if (scenario == QStringLiteral("R2DefectAppRecreateReHooks"))
    {
        const bool ok = wpj1_test::RunScenario(scenario);
        return ok ? 0 : 1;
    }

    // 构造 QCoreApplication（不是 QApplication）：本包不展示任何窗口，AddressBookStore
    // 的防抖定时器（QTimer）与 Int3Controller 的 QObject 机制需要有一个应用对象存在
    // 才能正常构造。部分场景会用真实 QCoreApplication::exec()+quit() 验证 aboutToQuit
    // 落盘（见 wpJ1_scenarios_review.cpp 里带"真实 exec"字样的场景），所以这里的 app
    // 对象必须是真正可以 exec() 的那一个，不是只用来满足 QObject 构造前提的摆设。
    // 不需要部署任何平台插件（qoffscreen.dll 等只有 QGuiApplication/QApplication 才
    // 会加载）。
    QCoreApplication app(argc, argv);

    const bool ok = wpj1_test::RunScenario(scenario);
    return ok ? 0 : 1;
}
