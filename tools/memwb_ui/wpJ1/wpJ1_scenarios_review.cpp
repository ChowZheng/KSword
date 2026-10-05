// wpJ1_scenarios_review.cpp
// 作用：WP-J1（WorkbenchShared）离屏验证夹具的"补测场景"集合，与原始 11 个场景
// （wpJ1_scenarios.cpp）分成两个文件，原因有两个：
// 1. 仓库规范"单文件尽量不超过 800 行"——两类场景合在一起会越过这条线；
// 2. 职责分明：本文件只收两类东西——
//    a) GAP_*：wave3 独立审核报告里指出的"原夹具关键判断覆盖不到的缺口"，对应审核者的变异
//       R01-R05、R09-R14、R19；这些场景在修复后的当前代码上应当全部通过。
//    b) DEFECT_*：审核报告列出的确认缺陷的最小复现，修复前失败、修复后应当通过——
//       按任务书要求"DEFECT 类用例修复后转绿并并入默认运行"，这里不再区分"缺陷
//       场景单独一组"，而是和 GAP 一样并入 build-wpJ1-tests.cmd 的默认场景名单。
//    c) 本包自己新补的场景：覆盖 C5 修法里审核者明确没做的那一半（Configure() 补接
//       aboutToQuit，见 InstanceBeforeAppHooksOnConfigure）、C1 的 qWarning 诊断
//       信息（见 GapConfigureRejectionWarnsAboutOrder）、以及"两个共享对象同时被
//       提前访问"的组合场景。
// 分发入口是 RunReviewScenario()，由 wpJ1_scenarios.cpp 的 RunScenario() 在认不出
// 名字时转调；本文件认不出的名字会继续转调 wpJ1_scenarios_r2.cpp 的 RunR2Scenario()
// （wave3 第二轮独立审核并入的补测场景），再转调 wpJ1_scenarios_r2b.cpp 的
// RunR2bScenario()（本包这一轮自己新补的场景）——"未识别场景名"的兜底在调度链条
// 最末端的那个文件生效，详见 wpJ1_common.h 的说明。

#include "wpJ1_common.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QPointer>
#include <QTimer>

#include <stdexcept>

namespace wpj1_test
{
    namespace
    {
        // MakeEntry：与 wpJ1_scenarios.cpp 里的同名函数功能一致（构造一个最简单的
        // "绝对地址 + Bookmark"草稿），各自放在各自文件的匿名命名空间里（内部链接），
        // 不跨文件共享符号——两份实现保持完全一致即可，没有必要为了去重把它提升成
        // 公共头文件里的内联函数。
        ksword::memwb::AddressEntry MakeEntry(const std::string& targetKey, std::uint64_t address, const std::string& note)
        {
            ksword::memwb::AddressEntry draft;
            draft.kind = ksword::memwb::EntryKind::Bookmark;
            draft.targetKey = targetKey;
            draft.absoluteAddress = address;
            draft.note = note;
            draft.valueType = ksword::memwb::ValueType::Hex8;
            return draft;
        }

        // RunExecUntilQuit：真实跑一轮 QCoreApplication::exec()，排一个"立即触发"的
        // quit() 让它马上返回——用来让 Qt 真的发出一次 aboutToQuit 信号（不是用
        // QMetaObject::invokeMethod 模拟），覆盖头文件里"已用真实 exec()+quit() 验证"
        // 这句话。调用方法：在已经构造好 QCoreApplication 的场景函数里直接调用，
        // 调用一次就等于真实经历一次"退出事件循环"。
        void RunExecUntilQuit()
        {
            QTimer::singleShot(0, QCoreApplication::instance(), &QCoreApplication::quit);
            QCoreApplication::instance()->exec();
        }
    }

    // ============================================================
    // GAP_* ：杀死审核者变异表里"原夹具幸存"的那些真实缺口。
    // ============================================================

    // 杀 R01：无效 Configure（三个必填工厂任一为空）不得留下任何残留——工厂没被
    // 换、审计工厂没被调用、CreateServices()/CreateIoPort()/CreateKernelMutationPort()
    // 仍然如实报告"未配置"。
    bool ScenarioGapInvalidConfigureLeavesNoResidue()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();
        int auditCalls = 0;
        ks::ui::WorkbenchShared::WorkbenchBackends bad =
            MakeValidBackends(ScratchFilePath(QStringLiteral("NoResidue"), QStringLiteral("a")));
        bad.int3Factory = nullptr; // 唯一的无效点。
        bad.auditSinkFactory = [&auditCalls]() -> std::unique_ptr<ksword::memwb::IAuditSink>
        {
            ++auditCalls;
            return std::make_unique<FakeAuditSink>(nullptr);
        };
        ok = WPJ1_CHECK(!ws.Configure(bad), QStringLiteral("缺 int3Factory 时 Configure 返回 false")) && ok;
        ok = WPJ1_CHECK(ws.CreateServices() == nullptr, QStringLiteral("无效 Configure 之后 CreateServices() 仍为空")) && ok;
        ok = WPJ1_CHECK(ws.CreateIoPort() == nullptr, QStringLiteral("无效 Configure 之后 CreateIoPort() 仍为空")) && ok;
        ok = WPJ1_CHECK(ws.CreateKernelMutationPort() == nullptr, QStringLiteral("无效 Configure 之后 CreateKernelMutationPort() 仍为空")) && ok;
        ok = WPJ1_CHECK(auditCalls == 0, QStringLiteral("无效 Configure 不得调用审计工厂")) && ok;
        ok = WPJ1_CHECK(ws.UsesNullAudit(), QStringLiteral("无效 Configure 之后 UsesNullAudit() 仍为 true")) && ok;
        return ok;
    }

    // 杀 R02/R03/R04：被拒的重复 Configure 不得替换已持有的工厂/审计接收器；
    // 审计工厂只应在第一次成功 Configure 时调用一次。
    bool ScenarioGapRepeatConfigureKeepsFactoriesAndAudit()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();
        int svcA = 0;
        int ioA = 0;
        int recA = 0;
        int auditCallsA = 0;
        ks::ui::WorkbenchShared::WorkbenchBackends first =
            MakeValidBackends(ScratchFilePath(QStringLiteral("RepeatKeep"), QStringLiteral("A")), &svcA, &ioA, nullptr);
        first.auditSinkFactory = [&]() -> std::unique_ptr<ksword::memwb::IAuditSink>
        {
            ++auditCallsA;
            return std::make_unique<FakeAuditSink>(&recA);
        };
        ok = WPJ1_CHECK(ws.Configure(first), QStringLiteral("第一次 Configure 成功")) && ok;
        ok = WPJ1_CHECK(auditCallsA == 1, QStringLiteral("审计工厂在 Configure 时恰好调用一次")) && ok;
        ksword::memwb::IAuditSink* auditBefore = &ws.AuditSink();

        int svcB = 0;
        int ioB = 0;
        int recB = 0;
        int auditCallsB = 0;
        ks::ui::WorkbenchShared::WorkbenchBackends second =
            MakeValidBackends(ScratchFilePath(QStringLiteral("RepeatKeep"), QStringLiteral("B")), &svcB, &ioB, nullptr);
        second.auditSinkFactory = [&]() -> std::unique_ptr<ksword::memwb::IAuditSink>
        {
            ++auditCallsB;
            return std::make_unique<FakeAuditSink>(&recB);
        };
        ok = WPJ1_CHECK(!ws.Configure(second), QStringLiteral("第二次合法 Configure 被拒")) && ok;
        ok = WPJ1_CHECK(auditCallsB == 0, QStringLiteral("被拒的重复 Configure 不得调用新的审计工厂")) && ok;
        ok = WPJ1_CHECK(&ws.AuditSink() == auditBefore, QStringLiteral("被拒的重复 Configure 之后 AuditSink() 身份不变")) && ok;
        ok = WPJ1_CHECK(!ws.UsesNullAudit(), QStringLiteral("被拒的重复 Configure 之后仍在用第一次的真实审计")) && ok;
        ok = WPJ1_CHECK(ws.CreateServices() != nullptr && svcA == 1 && svcB == 0, QStringLiteral("CreateServices() 仍走第一次的工厂")) && ok;
        ok = WPJ1_CHECK(ws.CreateIoPort() != nullptr && ioA == 1 && ioB == 0, QStringLiteral("CreateIoPort() 仍走第一次的工厂")) && ok;
        return ok;
    }

    // 杀 R05：地址簿文件损坏不得使 Configure 本身失败，失败只通过
    // AddressBookStore::lastLoadFailed() 暴露。
    bool ScenarioGapCorruptBookDoesNotFailConfigure()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();
        const QString path = ScratchFilePath(QStringLiteral("CorruptBook"), QStringLiteral("book"));
        QFile::remove(path);
        {
            QFile bad(path);
            ok = WPJ1_CHECK(bad.open(QIODevice::WriteOnly), QStringLiteral("写入坏文件")) && ok;
            bad.write("this is not an address book\n");
        }
        ok = WPJ1_CHECK(ws.Configure(MakeValidBackends(path)), QStringLiteral("坏地址簿文件不应使 Configure 失败")) && ok;
        ok = WPJ1_CHECK(ws.IsConfigured(), QStringLiteral("坏地址簿文件之后 IsConfigured() 为 true")) && ok;
        ok = WPJ1_CHECK(ws.AddressBook().lastLoadFailed(), QStringLiteral("坏文件通过 lastLoadFailed() 暴露")) && ok;
        ok = WPJ1_CHECK(ws.AddressBook().size() == 0, QStringLiteral("坏文件之后簿为空")) && ok;
        return ok;
    }

    // 杀 R09/R10：真实发出 aboutToQuit 信号（本场景用真实 QCoreApplication::exec()+
    // quit()，不是 QMetaObject::invokeMethod 模拟），Shutdown 必须同步落盘；第二次
    // 触发在没有新改动时为空操作。
    bool ScenarioGapQuitSignalFlushes()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();
        const QString path = ScratchFilePath(QStringLiteral("QuitFlush"), QStringLiteral("book"));
        QFile::remove(path);
        ok = WPJ1_CHECK(ws.Configure(MakeValidBackends(path)), QStringLiteral("Configure 成功")) && ok;
        ks::ui::AddressBookStore& store = ws.AddressBook();
        store.add(MakeEntry("procQ", 0x10ULL, "quit-flush"));
        ok = WPJ1_CHECK(store.writeCount() == 0 && store.pendingSave(), QStringLiteral("真实触发之前仍在防抖窗口内")) && ok;

        RunExecUntilQuit(); // 真实 exec()+quit，Qt 真的发出 aboutToQuit。
        ok = WPJ1_CHECK(store.writeCount() == 1, QStringLiteral("真实 aboutToQuit 同步触发 Shutdown，恰好落盘一次")) && ok;
        ks::ui::AddressBookStore reopened(path);
        ok = WPJ1_CHECK(reopened.load() && reopened.size() == 1, QStringLiteral("落盘文件可被重新加载且含 1 条")) && ok;

        RunExecUntilQuit(); // 第二次真实退出事件循环：没有新改动，应为空操作。
        ok = WPJ1_CHECK(store.writeCount() == 1, QStringLiteral("第二次真实 aboutToQuit 不再落盘（没有新的待落盘改动）")) && ok;
        return ok;
    }

    // 杀 R11：只是惰性访问共享对象不得让 IsConfigured() 变真。
    bool ScenarioGapIsConfiguredFalseAfterLazyAccess()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();
        (void)ws.AddressBook();
        (void)ws.AddressBookTable();
        (void)ws.Int3();
        (void)ws.AuditSink();
        ok = WPJ1_CHECK(!ws.IsConfigured(), QStringLiteral("惰性访问全部共享对象之后 IsConfigured() 仍为 false")) && ok;
        return ok;
    }

    // 杀 R12/R13：WritePolicy 的"本次运行"记忆不应被 Configure()/Shutdown() 清掉。
    bool ScenarioGapWritePolicySurvivesConfigureAndShutdown()
    {
        bool ok = true;
        using ksword::memwb::Channel;
        using ksword::memwb::Scope;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();
        ok = WPJ1_CHECK(ws.WritePolicy().NoteConfirmed(Scope::KernelVirtual, Channel::UserMode, true),
            QStringLiteral("Configure 之前先记入一条")) && ok;
        ok = WPJ1_CHECK(ws.Configure(MakeValidBackends(ScratchFilePath(QStringLiteral("PolicyKeep"), QStringLiteral("book")))),
            QStringLiteral("Configure 成功")) && ok;
        ok = WPJ1_CHECK(ws.WritePolicy().IsRemembered(Scope::KernelVirtual, Channel::UserMode),
            QStringLiteral("Configure 之后记忆仍在")) && ok;
        ws.Shutdown();
        ok = WPJ1_CHECK(ws.WritePolicy().IsRemembered(Scope::KernelVirtual, Channel::UserMode),
            QStringLiteral("Shutdown 之后记忆仍在")) && ok;
        return ok;
    }

    // 杀 R14：进程里第一次访问就是 AddressBookTable()（不先调 AddressBook()）也能
    // 正确惰性建出模型，不存在"先调它就拿到空指针解引用"的顺序依赖。
    bool ScenarioGapAddressBookTableIsFirstAccess()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();
        ok = WPJ1_CHECK(ws.AddressBookTable().rowCount() == 0, QStringLiteral("第一次访问就是 AddressBookTable() 时也能惰性建出模型")) && ok;
        return ok;
    }

    // ============================================================
    // 本包自己新补的场景（不是审核报告列出的缺口，是本次修复新增逻辑需要的新证据）。
    // ============================================================

    // 验证 C1 的 qWarning 诊断信息真的被打印出来，不只是"返回 false"——装上一个
    // 临时消息处理器捕获它，核对非空且提到了 "Configure"。调用方法：场景内部自己
    // 安装/卸载处理器，不影响其它场景（每个场景都是独立进程）。
    bool ScenarioGapConfigureRejectionWarnsAboutOrder()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();
        (void)ws.AddressBook(); // 制造"装配顺序错误"场景：提前惰性访问地址簿。

        static QString s_capturedMessage;
        s_capturedMessage.clear();
        QtMessageHandler previous = qInstallMessageHandler(
            [](QtMsgType type, const QMessageLogContext&, const QString& msg)
            {
                if (type == QtWarningMsg)
                {
                    s_capturedMessage = msg;
                }
            });

        const QString path = ScratchFilePath(QStringLiteral("WarnOrder"), QStringLiteral("book"));
        QFile::remove(path);
        const bool cfg = ws.Configure(MakeValidBackends(path));
        qInstallMessageHandler(previous); // 还原，避免影响本场景之后可能的其它输出。

        ok = WPJ1_CHECK(!cfg, QStringLiteral("装配顺序错误时 Configure 返回 false")) && ok;
        ok = WPJ1_CHECK(!s_capturedMessage.isEmpty(), QStringLiteral("装配顺序错误时打印了一条 qWarning 诊断信息")) && ok;
        ok = WPJ1_CHECK(s_capturedMessage.contains(QStringLiteral("Configure")), QStringLiteral("诊断信息里提到了 Configure")) && ok;
        return ok;
    }

    // 两个共享对象同时被提前访问：地址簿存储+模型（触发 C1 拒绝）与 Int3（本不该
    // 受这条限制）。验证"拒绝的判据只看地址簿、不受 Int3 是否也被提前访问影响"，
    // 且 Int3 的身份在被拒的 Configure 之后仍然保持不变、仍然可用。
    bool ScenarioGapBothEarlyAccessRejectsConfigureButInt3StillUsable()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();
        QPointer<ks::ui::AddressBookStore> preStore(&ws.AddressBook());
        QPointer<ks::ui::Int3Controller> preInt3(&ws.Int3());

        const QString path = ScratchFilePath(QStringLiteral("BothEarly"), QStringLiteral("book"));
        QFile::remove(path);
        const bool cfg = ws.Configure(MakeValidBackends(path));
        ok = WPJ1_CHECK(!cfg, QStringLiteral("地址簿与 Int3 都被提前访问时，Configure 仍因地址簿这一条被拒绝")) && ok;
        ok = WPJ1_CHECK(preStore && &ws.AddressBook() == preStore.data(), QStringLiteral("被拒之后地址簿存储仍是提前访问时那一份，没有被释放")) && ok;
        ok = WPJ1_CHECK(preInt3 && &ws.Int3() == preInt3.data(), QStringLiteral("被拒之后 Int3 控制器仍是提前访问时那一份，没有被释放")) && ok;

        const ksword::memwb::PatchTarget target{};
        const ks::ui::Int3InstallOutcome outcome = ws.Int3().Install(target, 0x5000ULL, 1ULL);
        ok = WPJ1_CHECK(outcome.status == ksword::memwb::InstallStatus::WriteFailed,
            QStringLiteral("Configure 被拒之后 Int3 仍是退化状态（拒绝一切），不是崩溃或别的结果")) && ok;
        return ok;
    }

    // 对应 C5 修法的第二部分（审核报告原文："Configure 里补接 aboutToQuit，副本未
    // 做"）：Instance() 在 QCoreApplication 构造之前就被调用，构造期的那一次接线
    // 必然被跳过；本场景验证 Configure() 开头的 HookQuitIfNeeded() 把这条信号补接上
    // 了——用真实 exec()+quit 触发，不是模拟发信号。顺序安排见 wpJ1_main.cpp（本
    // 场景名是那里唯一特殊处理的名字）。
    bool ScenarioInstanceBeforeAppHooksOnConfigure()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();
        const QString path = ScratchFilePath(QStringLiteral("InstanceBeforeApp"), QStringLiteral("book"));
        QFile::remove(path);

        ok = WPJ1_CHECK(ws.Configure(MakeValidBackends(path)),
            QStringLiteral("Configure 成功（此时 QCoreApplication 已经由 main() 构造好）")) && ok;
        ks::ui::AddressBookStore& store = ws.AddressBook();
        store.add(MakeEntry("procEarly", 0x30ULL, "instance-before-app"));
        ok = WPJ1_CHECK(store.writeCount() == 0 && store.pendingSave(), QStringLiteral("真实触发之前仍在防抖窗口内")) && ok;

        RunExecUntilQuit(); // 真实 exec()+quit。

        ok = WPJ1_CHECK(store.writeCount() == 1,
            QStringLiteral("即使 Instance() 先于 QCoreApplication 构造，Configure 补接的 aboutToQuit 仍能触发一次真实落盘")) && ok;
        return ok;
    }

    // ============================================================
    // DEFECT_* ：审核报告确认缺陷的最小复现，修复后应当全部通过，并入默认运行。
    // ============================================================

    // 对应 C2：零修改的 Shutdown 不得重写一份没变化的地址簿文件。
    bool ScenarioDefectShutdownZeroEditsDoesNotRewriteBook()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();
        const QString path = ScratchFilePath(QStringLiteral("DefectD5a"), QStringLiteral("book"));
        QFile::remove(path);
        {
            ks::ui::AddressBookStore seed(path);
            seed.add(MakeEntry("procSeed", 0x500ULL, "precious"));
            ok = WPJ1_CHECK(seed.flushNow(), QStringLiteral("种子文件落盘")) && ok;
        }
        ok = WPJ1_CHECK(ws.Configure(MakeValidBackends(path)), QStringLiteral("Configure 成功")) && ok;
        ok = WPJ1_CHECK(ws.AddressBook().size() == 1 && !ws.AddressBook().pendingSave(), QStringLiteral("已加载 1 条且没有待落盘改动")) && ok;
        ws.Shutdown();
        ok = WPJ1_CHECK(ws.AddressBook().writeCount() == 0, QStringLiteral("零修改的 Shutdown 不写盘（writeCount 仍为 0）")) && ok;
        return ok;
    }

    // 对应 C2：地址簿加载失败（只剩 .tmp 残留）后，零修改的 Shutdown 不得凭空建出
    // 一份空的正式文件（否则下次启动失败提示静默消失，唯一的好副本 .tmp 被晾在
    // 一边）。
    bool ScenarioDefectShutdownAfterFailedLoadDoesNotCreateMainFile()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();
        const QString path = ScratchFilePath(QStringLiteral("DefectD5b"), QStringLiteral("book"));
        QFile::remove(path);
        QFile::remove(path + QStringLiteral(".tmp"));
        {
            ks::ui::AddressBookStore seed(path + QStringLiteral(".tmp"));
            seed.add(MakeEntry("procSeed", 0x500ULL, "only-good-copy"));
            ok = WPJ1_CHECK(seed.flushNow(), QStringLiteral(".tmp 种子落盘")) && ok;
        }
        QFile::remove(path);
        ok = WPJ1_CHECK(ws.Configure(MakeValidBackends(path)), QStringLiteral("Configure 成功")) && ok;
        ok = WPJ1_CHECK(ws.AddressBook().lastLoadFailed(), QStringLiteral("只有 .tmp 残留时 load 失败")) && ok;
        ws.Shutdown();
        ok = WPJ1_CHECK(!QFile::exists(path), QStringLiteral("零修改的 Shutdown 不得建出空的正式文件（否则下次启动失败提示消失）")) && ok;
        return ok;
    }

    // 对应 C1 的 Int3 部分（A+）：只惰性访问了 Int3()（没碰地址簿），Configure 仍应
    // 成功，且旧的 Int3Controller 引用不悬空、Install 落到注入的工厂。
    bool ScenarioDefectInt3OnlyEarlyTouchSurvivesConfigure()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();
        QPointer<ks::ui::Int3Controller> pre(&ws.Int3());
        int calls = 0;
        ks::ui::WorkbenchShared::WorkbenchBackends b = MakeValidBackends(ScratchFilePath(QStringLiteral("DefectD1b"), QStringLiteral("book")));
        b.int3Factory = [&calls](const ksword::memwb::PatchTarget&, ksword::memwb::Channel) -> std::unique_ptr<ksword::memwb::IPatchByteStore>
        {
            ++calls;
            return nullptr;
        };
        ok = WPJ1_CHECK(ws.Configure(b), QStringLiteral("只有 Int3() 被提前访问时 Configure 仍成功")) && ok;
        ok = WPJ1_CHECK(pre && &ws.Int3() == pre.data(), QStringLiteral("旧的 Int3Controller 引用仍然存活且身份不变")) && ok;
        const ksword::memwb::PatchTarget target{};
        ws.Int3().Install(target, 0x4000ULL, 1ULL);
        ok = WPJ1_CHECK(calls == 1, QStringLiteral("Install 落到 Configure 注入的 int3 工厂")) && ok;
        return ok;
    }

    // 对应 C1 的地址簿部分（A）：配置前已被取走引用的地址簿存储/模型，在本实现里
    // 对应的是"Configure 直接拒绝"，不是"原样保留并重新绑定"——这里按本包实际
    // 采用的选项断言，比审核报告里"两种修法都接受"的宽松版本更贴近当前代码。
    bool ScenarioDefectConfigureAfterLazyAccessDoesNotDangle()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();
        QPointer<ks::ui::AddressBookStore> preStore(&ws.AddressBook());
        QPointer<ks::ui::AddressBookModel> preModel(&ws.AddressBookTable());
        const QString path = ScratchFilePath(QStringLiteral("DefectD1"), QStringLiteral("book"));
        QFile::remove(path);
        const bool cfg = ws.Configure(MakeValidBackends(path));
        ok = WPJ1_CHECK(!cfg, QStringLiteral("本包采用的是 A（拒绝配置），不是整份替换——这次 Configure 必须返回 false")) && ok;
        ok = WPJ1_CHECK(preStore && preModel, QStringLiteral("被拒之后旧的地址簿存储/模型仍然存活，没有被释放（不悬空）")) && ok;
        ok = WPJ1_CHECK(&ws.AddressBook() == preStore.data() && &ws.AddressBookTable() == preModel.data(),
            QStringLiteral("被拒之后访问器返回的仍是提前访问时取得的那两个对象")) && ok;
        return ok;
    }

    // 对应 C4：配置前取得的 AuditSink() 引用，Configure 之后写入必须进真实接收器。
    bool ScenarioDefectPreConfigureAuditRefReachesRealSink()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();
        ksword::memwb::IAuditSink& pre = ws.AuditSink();
        int rec = 0;
        ks::ui::WorkbenchShared::WorkbenchBackends b = MakeValidBackends(ScratchFilePath(QStringLiteral("DefectD2"), QStringLiteral("book")));
        b.auditSinkFactory = [&rec]() -> std::unique_ptr<ksword::memwb::IAuditSink>
        {
            return std::make_unique<FakeAuditSink>(&rec);
        };
        const bool cfg = ws.Configure(b);
        ksword::memwb::AuditRecord dummy;
        pre.Record(dummy);
        ok = WPJ1_CHECK(!cfg || rec == 1,
            QStringLiteral("Configure 之前取得的 AuditSink() 引用，Configure 之后写入仍应进真实审计接收器")) && ok;
        return ok;
    }

    // 对应 C3：审计工厂抛异常时 Configure 不得留下半配置，可以用合法配置立即重试。
    bool ScenarioDefectThrowingAuditFactoryLeavesUnconfigured()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();
        const QString path = ScratchFilePath(QStringLiteral("DefectD3"), QStringLiteral("book"));
        QFile::remove(path);
        ks::ui::WorkbenchShared::WorkbenchBackends b = MakeValidBackends(path);
        b.auditSinkFactory = []() -> std::unique_ptr<ksword::memwb::IAuditSink>
        {
            throw std::runtime_error("audit log cannot be opened");
        };
        bool threw = false;
        try
        {
            (void)ws.Configure(b);
        }
        catch (const std::exception&)
        {
            threw = true;
        }
        ok = WPJ1_CHECK(threw, QStringLiteral("审计工厂的异常从 Configure 传播出来（本实现不在内部吞掉）")) && ok;
        ok = WPJ1_CHECK(!ws.IsConfigured(), QStringLiteral("审计工厂抛异常之后 IsConfigured() 必须仍为 false")) && ok;
        ok = WPJ1_CHECK(ws.Configure(MakeValidBackends(path)), QStringLiteral("异常之后用合法配置重试仍应成功")) && ok;
        ok = WPJ1_CHECK(ws.AddressBook().filePath() == path, QStringLiteral("重试成功之后地址簿路径是真实路径")) && ok;
        return ok;
    }

    // 对应 C5：Shutdown 早于 Configure（例如装配期某处提前调用）不得让之后真正退出
    // 时的落盘变成空操作——用真实 exec()+quit 触发。
    bool ScenarioDefectEarlyShutdownDoesNotDisableQuitFlush()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();
        const QString path = ScratchFilePath(QStringLiteral("DefectD4"), QStringLiteral("book"));
        QFile::remove(path);
        ws.Shutdown(); // 例如装配期某处提前调用。
        ok = WPJ1_CHECK(ws.Configure(MakeValidBackends(path)), QStringLiteral("Configure 成功")) && ok;
        ks::ui::AddressBookStore& store = ws.AddressBook();
        store.add(MakeEntry("procD4", 0x20ULL, "after-early-shutdown"));
        RunExecUntilQuit(); // 真实 exec()+quit。
        ok = WPJ1_CHECK(store.writeCount() == 1, QStringLiteral("早先的 Shutdown 不得让退出时真实的 aboutToQuit 落盘失效")) && ok;
        return ok;
    }

    // ------------------------------------------------------------
    // RunReviewScenario：见 wpJ1_common.h 的声明注释。未识别的名字在这里才真正算
    // 作用法错误（两个文件的调度链条到这里终止）。
    // ------------------------------------------------------------
    bool RunReviewScenario(const QString& name)
    {
        if (name == QStringLiteral("GapInvalidConfigureLeavesNoResidue")) return ScenarioGapInvalidConfigureLeavesNoResidue();
        if (name == QStringLiteral("GapRepeatConfigureKeepsFactoriesAndAudit")) return ScenarioGapRepeatConfigureKeepsFactoriesAndAudit();
        if (name == QStringLiteral("GapCorruptBookDoesNotFailConfigure")) return ScenarioGapCorruptBookDoesNotFailConfigure();
        if (name == QStringLiteral("GapQuitSignalFlushes")) return ScenarioGapQuitSignalFlushes();
        if (name == QStringLiteral("GapIsConfiguredFalseAfterLazyAccess")) return ScenarioGapIsConfiguredFalseAfterLazyAccess();
        if (name == QStringLiteral("GapWritePolicySurvivesConfigureAndShutdown")) return ScenarioGapWritePolicySurvivesConfigureAndShutdown();
        if (name == QStringLiteral("GapAddressBookTableIsFirstAccess")) return ScenarioGapAddressBookTableIsFirstAccess();
        if (name == QStringLiteral("GapConfigureRejectionWarnsAboutOrder")) return ScenarioGapConfigureRejectionWarnsAboutOrder();
        if (name == QStringLiteral("GapBothEarlyAccessRejectsConfigureButInt3StillUsable")) return ScenarioGapBothEarlyAccessRejectsConfigureButInt3StillUsable();
        if (name == QStringLiteral("InstanceBeforeAppHooksOnConfigure")) return ScenarioInstanceBeforeAppHooksOnConfigure();
        if (name == QStringLiteral("DefectShutdownZeroEditsDoesNotRewriteBook")) return ScenarioDefectShutdownZeroEditsDoesNotRewriteBook();
        if (name == QStringLiteral("DefectShutdownAfterFailedLoadDoesNotCreateMainFile")) return ScenarioDefectShutdownAfterFailedLoadDoesNotCreateMainFile();
        if (name == QStringLiteral("DefectInt3OnlyEarlyTouchSurvivesConfigure")) return ScenarioDefectInt3OnlyEarlyTouchSurvivesConfigure();
        if (name == QStringLiteral("DefectConfigureAfterLazyAccessDoesNotDangle")) return ScenarioDefectConfigureAfterLazyAccessDoesNotDangle();
        if (name == QStringLiteral("DefectPreConfigureAuditRefReachesRealSink")) return ScenarioDefectPreConfigureAuditRefReachesRealSink();
        if (name == QStringLiteral("DefectThrowingAuditFactoryLeavesUnconfigured")) return ScenarioDefectThrowingAuditFactoryLeavesUnconfigured();
        if (name == QStringLiteral("DefectEarlyShutdownDoesNotDisableQuitFlush")) return ScenarioDefectEarlyShutdownDoesNotDisableQuitFlush();

        // wave3 第二轮独立审核（review2-wpJ1.md）并入的补测场景、以及本包自己这一轮
        // 新补的场景都不在本文件里，转调下一级（见 wpJ1_common.h 的调度链条说明）。
        return RunR2Scenario(name);
    }
}
