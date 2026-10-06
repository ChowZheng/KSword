// wpJ1_scenarios_r2b.cpp
// 作用：WP-J1（WorkbenchShared）第二轮独立审核修复（wave3 fix2-wpJ1.md）本包自己
// 新补的场景——不是审核报告 review2-wpJ1.md 原文列出的缺口（那些在
// wpJ1_scenarios_r2.cpp 里），是本次修复新增的代码（configuring_ 重入拒绝的
// Configure-调用这一半、HookQuitIfNeeded 挪到提交段落、hookedApp_ 取代
// quitHooked_、新增的 ConfigureRejectedReason() 诊断访问器）各自需要的新证据。
// 单独成一个文件的原因与 wpJ1_scenarios_review.cpp/wpJ1_scenarios_r2.cpp 一样：
// 仓库"单文件尽量不超过 800 行"的规范，以及职责分明——这里只收本包自己这一轮新加的
// 断言。分发入口是 RunR2bScenario()，由 wpJ1_scenarios_r2.cpp 的 RunR2Scenario() 在
// 认不出名字时转调；本文件认不出的名字才真正打印一条 CHECK FAIL（四个文件的调度
// 链条到这里终止：RunScenario → RunReviewScenario → RunR2Scenario → RunR2bScenario）。
//
// 两个场景（R2DefectRejectedConfigureDoesNotHookQuit、R2DefectAppRecreateReHooks）
// 需要反常的 QCoreApplication 构造顺序，在 wpJ1_main.cpp 里按名字特殊处理，与既有的
// InstanceBeforeAppHooksOnConfigure 场景用的是同一套机制。

#include "wpJ1_common.h"

#include <QCoreApplication>
#include <QFile>
#include <QTimer>

namespace wpj1_test
{
    namespace
    {
        // R2bFakeArgc / R2bFakeArgv：供 R2DefectAppRecreateReHooks 场景在同一个进程里
        // 先后构造两个 QCoreApplication 实例时使用——Qt 的 QCoreApplication 构造函数
        // 要求传入的 int&/char** 在应用对象存活期间保持有效，这里用文件级 static
        // 变量保证两次构造（即便分别对应两个先后存在、互不重叠的 app 对象）用的都是
        // 同一份、长期有效的存储，不依赖 main() 自己的 argc/argv（本场景名在
        // wpJ1_main.cpp 里被特殊处理为"完全不让 main() 构造任何 app"，所以这里必须
        // 自备一份）。
        int g_r2bFakeArgc = 1;
        char g_r2bFakeArg0[] = "wpJ1_tests";
        char* g_r2bFakeArgv[] = {g_r2bFakeArg0, nullptr};

        // R2bEntry：与另外两个场景文件里的同名小工具功能一致（内部链接，各自一份，
        // 没有必要为了去重提升成公共头文件）。
        ksword::memwb::AddressEntry R2bEntry(const std::string& key, std::uint64_t address, const std::string& note)
        {
            ksword::memwb::AddressEntry e;
            e.kind = ksword::memwb::EntryKind::Bookmark;
            e.targetKey = key;
            e.absoluteAddress = address;
            e.note = note;
            e.valueType = ksword::memwb::ValueType::Hex8;
            return e;
        }

        // R2bRunExecUntilQuit：真实跑一轮 QCoreApplication::exec()，立即派发 quit()。
        void R2bRunExecUntilQuit()
        {
            QTimer::singleShot(0, QCoreApplication::instance(), &QCoreApplication::quit);
            QCoreApplication::instance()->exec();
        }
    }

    // 验证 D1 的 Configure-重入一半：审计工厂回调里又调用了一次 Configure()，必须在
    // 分支零被立即拒绝——重入调用自己的 servicesFactory 从未被真正调用过（零副作用），
    // 外层这次调用本身不受影响、正常走完并成功，最终生效的工厂是外层传入的那一份，
    // 不是回调里偷偷传进来的那一份；并且 ConfigureRejectedReason() 在工厂回调里（外层
    // 还没来得及把它复位成 None 之前）能读出 ReentrantConfigureCall。
    bool ScenarioR2DefectReentrantConfigureCallRejected()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();
        int outerSvcCalls = 0;
        int innerSvcCalls = 0;
        bool innerCalled = false;
        bool innerConfigureResult = true; // 预期会被改写成 false；先给一个会被断言揪出来的初值。
        ks::ui::WorkbenchShared::ConfigureRejection innerRejectedReasonSeen =
            ks::ui::WorkbenchShared::ConfigureRejection::None;

        ks::ui::WorkbenchShared::WorkbenchBackends outer =
            MakeValidBackends(ScratchFilePath(QStringLiteral("R2bReentrantCfg"), QStringLiteral("outer")), &outerSvcCalls);
        outer.auditSinkFactory = [&]() -> std::unique_ptr<ksword::memwb::IAuditSink>
        {
            innerCalled = true;
            // 内层传一份完全不同的 backends：如果重入没有被拦住、内层真的配置成功，
            // 外层紧接着的提交会把内层刚刚装好的工厂整份替换掉——这正是 D1 想杜绝的
            // 悬空（这次换成了"工厂引用"而不是"地址簿引用"的形状）。
            ks::ui::WorkbenchShared::WorkbenchBackends inner =
                MakeValidBackends(ScratchFilePath(QStringLiteral("R2bReentrantCfg"), QStringLiteral("inner")), &innerSvcCalls);
            innerConfigureResult = ws.Configure(inner);
            // 必须在外层自己的提交把 configureRejectedReason_ 复位成 None 之前读到——
            // 这也是为什么这行必须写在回调内部，不能挪到 Configure(outer) 返回之后。
            innerRejectedReasonSeen = ws.ConfigureRejectedReason();
            return std::make_unique<FakeAuditSink>(nullptr);
        };

        const bool outerResult = ws.Configure(outer);

        ok = WPJ1_CHECK(innerCalled, QStringLiteral("审计工厂确实执行了，重入调用确实发生了")) && ok;
        ok = WPJ1_CHECK(!innerConfigureResult, QStringLiteral("重入的 Configure() 调用立即返回 false（分支零拦住）")) && ok;
        ok = WPJ1_CHECK(innerSvcCalls == 0, QStringLiteral("重入调用自己的 servicesFactory 从未被调用过（零副作用）")) && ok;
        ok = WPJ1_CHECK(innerRejectedReasonSeen == ks::ui::WorkbenchShared::ConfigureRejection::ReentrantConfigureCall,
            QStringLiteral("重入那一刻 ConfigureRejectedReason() 能读出 ReentrantConfigureCall")) && ok;
        ok = WPJ1_CHECK(outerResult, QStringLiteral("外层 Configure 调用本身正常成功，不受内层重入影响")) && ok;
        ok = WPJ1_CHECK(ws.ConfigureRejectedReason() == ks::ui::WorkbenchShared::ConfigureRejection::None,
            QStringLiteral("外层成功之后 ConfigureRejectedReason() 复位为 None")) && ok;

        (void)ws.CreateServices();
        ok = WPJ1_CHECK(outerSvcCalls == 1, QStringLiteral("最终生效的是外层的 servicesFactory，不是回调里偷偷传入的那一份")) && ok;
        return ok;
    }

    // 验证 D3：被拒绝的 Configure 调用不得顺带把 aboutToQuit 接到 Shutdown 上——用
    // QObject::disconnect 的返回值直接探测这条连接是否存在（返回 true 表示确实移除
    // 了至少一条匹配的连接），不依赖 AddressBookStore 的落盘副作用（被拒绝的 Configure
    // 之后 AddressBook() 只能退化成空路径的纯内存实例，add() 永远不会调度落盘，没法
    // 从那条路径上观察到钩子是否存在）。要求 Instance() 在 QCoreApplication 构造之前
    // 就被调用（见 wpJ1_main.cpp 的特殊处理），否则构造函数自己的那次 HookQuitIfNeeded()
    // 会在 app 存在的那一刻就先接上，这个场景就分不清"是谁接的线"了。
    bool ScenarioR2DefectRejectedConfigureDoesNotHookQuit()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();

        ks::ui::WorkbenchShared::WorkbenchBackends bad =
            MakeValidBackends(ScratchFilePath(QStringLiteral("R2bRejectHook"), QStringLiteral("a")));
        bad.int3Factory = nullptr; // 必填工厂缺项，必然在分支二被拒绝。
        ok = WPJ1_CHECK(!ws.Configure(bad), QStringLiteral("缺 int3Factory 时 Configure 返回 false")) && ok;

        const bool hadConnectionAfterRejection = QObject::disconnect(
            QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
            &ws, &ks::ui::WorkbenchShared::Shutdown);
        ok = WPJ1_CHECK(!hadConnectionAfterRejection,
            QStringLiteral("被拒绝的 Configure 不得顺带把 aboutToQuit 接到 Shutdown 上（D3：被拒绝＝零副作用，包括退出钩子）")) && ok;

        ok = WPJ1_CHECK(ws.Configure(MakeValidBackends(ScratchFilePath(QStringLiteral("R2bRejectHook"), QStringLiteral("b")))),
            QStringLiteral("用合法配置重试成功")) && ok;
        const bool hadConnectionAfterSuccess = QObject::disconnect(
            QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
            &ws, &ks::ui::WorkbenchShared::Shutdown);
        ok = WPJ1_CHECK(hadConnectionAfterSuccess,
            QStringLiteral("成功的 Configure 必须把 aboutToQuit 接到 Shutdown 上（证明上面探测用的手法本身没问题）")) && ok;
        return ok;
    }

    // 验证 D4：同一进程里先销毁一个 QCoreApplication、再构造一个新的，Configure 提交
    // 时必须重新对着新的 app 接线，不能被"接过线没有"的旧布尔标志卡住。本场景完全
    // 自己管理 QCoreApplication 的构造/销毁，main() 对这个名字什么都不做（见
    // wpJ1_main.cpp），因为需要在同一个进程里先后存在两个不重叠的 QCoreApplication。
    bool ScenarioR2DefectAppRecreateReHooks()
    {
        bool ok = true;
        {
            QCoreApplication app1(g_r2bFakeArgc, g_r2bFakeArgv);
            ks::ui::WorkbenchShared::Instance(); // 单例首次构造，构造函数对着 app1 接线。
        } // app1 在这里销毁：新实现的 hookedApp_（QPointer）会被自动清空；旧实现的
          // 布尔标志 quitHooked_ 不会——这正是 D4 要堵的那个口子。

        QCoreApplication app2(g_r2bFakeArgc, g_r2bFakeArgv);
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance(); // 早已构造过，取回同一个单例。
        const QString path = ScratchFilePath(QStringLiteral("R2bAppRecreate"), QStringLiteral("book"));
        QFile::remove(path);
        ok = WPJ1_CHECK(ws.Configure(MakeValidBackends(path)), QStringLiteral("对着 app2，Configure 成功")) && ok;
        ks::ui::AddressBookStore& store = ws.AddressBook();
        store.add(R2bEntry("r2b", 0x1, "app-recreate"));
        R2bRunExecUntilQuit(); // 对 app2 真实跑一轮退出事件循环。
        ok = WPJ1_CHECK(store.writeCount() == 1,
            QStringLiteral("app1 销毁、app2 新建之后，Configure 提交时重新对 app2 接线，"
                           "app2 真实退出时仍能触发落盘（D4）")) && ok;
        return ok;
    }

    // 验证新增诊断访问器 ConfigureRejectedReason() 在"成功／缺工厂／重复配置"三种
    // 转换下都能跟踪到正确的值，且成功会把它复位回 None。
    bool ScenarioR2ConfigureRejectedReasonTracksEachCase()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();
        using Reason = ks::ui::WorkbenchShared::ConfigureRejection;
        ok = WPJ1_CHECK(ws.ConfigureRejectedReason() == Reason::None, QStringLiteral("初始状态：None（从未被拒绝过）")) && ok;

        ks::ui::WorkbenchShared::WorkbenchBackends missing =
            MakeValidBackends(ScratchFilePath(QStringLiteral("R2bRejectReason"), QStringLiteral("a")));
        missing.servicesFactory = nullptr;
        ok = WPJ1_CHECK(!ws.Configure(missing), QStringLiteral("缺 servicesFactory 被拒")) && ok;
        ok = WPJ1_CHECK(ws.ConfigureRejectedReason() == Reason::MissingRequiredFactory,
            QStringLiteral("原因：MissingRequiredFactory")) && ok;

        ok = WPJ1_CHECK(ws.Configure(MakeValidBackends(ScratchFilePath(QStringLiteral("R2bRejectReason"), QStringLiteral("b")))),
            QStringLiteral("合法配置成功")) && ok;
        ok = WPJ1_CHECK(ws.ConfigureRejectedReason() == Reason::None, QStringLiteral("成功之后复位为 None")) && ok;

        ok = WPJ1_CHECK(!ws.Configure(MakeValidBackends(ScratchFilePath(QStringLiteral("R2bRejectReason"), QStringLiteral("c")))),
            QStringLiteral("重复 Configure 被拒")) && ok;
        ok = WPJ1_CHECK(ws.ConfigureRejectedReason() == Reason::AlreadyConfigured,
            QStringLiteral("原因：AlreadyConfigured")) && ok;
        return ok;
    }

    // 验证 ConfigureRejectedReason() 在"装配顺序错误"这条没有恢复路径的拒绝分支上
    // 读到的是 AccessedBeforeConfigure——单独一个场景，因为这条分支一旦命中，同一个
    // 进程里之后任何 Configure() 调用都会一直被它拦住，没办法在同一个场景里继续测
    // 别的转换。
    bool ScenarioR2ConfigureRejectedReasonAccessedBeforeConfigure()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();
        (void)ws.AddressBook(); // 制造装配顺序错误：提前惰性访问地址簿。
        ok = WPJ1_CHECK(!ws.Configure(MakeValidBackends(ScratchFilePath(QStringLiteral("R2bRejectReasonOrder"), QStringLiteral("a")))),
            QStringLiteral("装配顺序错误被拒")) && ok;
        ok = WPJ1_CHECK(ws.ConfigureRejectedReason() == ks::ui::WorkbenchShared::ConfigureRejection::AccessedBeforeConfigure,
            QStringLiteral("原因：AccessedBeforeConfigure")) && ok;
        return ok;
    }

    bool RunR2bScenario(const QString& name)
    {
        if (name == QStringLiteral("R2DefectReentrantConfigureCallRejected")) return ScenarioR2DefectReentrantConfigureCallRejected();
        if (name == QStringLiteral("R2DefectRejectedConfigureDoesNotHookQuit")) return ScenarioR2DefectRejectedConfigureDoesNotHookQuit();
        if (name == QStringLiteral("R2DefectAppRecreateReHooks")) return ScenarioR2DefectAppRecreateReHooks();
        if (name == QStringLiteral("R2ConfigureRejectedReasonTracksEachCase")) return ScenarioR2ConfigureRejectedReasonTracksEachCase();
        if (name == QStringLiteral("R2ConfigureRejectedReasonAccessedBeforeConfigure")) return ScenarioR2ConfigureRejectedReasonAccessedBeforeConfigure();

        WPJ1_CHECK(false, QStringLiteral("未识别的场景名：") + name);
        return false;
    }
}
