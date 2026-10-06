// wpJ1_scenarios_r2.cpp
// 作用：WP-J1（WorkbenchShared）第二轮独立审核的补测场景，原样并入本包夹具目录
// （审核者已验证这份文件叠加后对照组是 196 checks/0 failures，DEFECT 场景在修复前单独失败）。
// 命名与分工规则与 wpJ1_scenarios_review.cpp 一致：本文件场景名全部以 R2 开头，
// 对应报告第 6 节"补测片段与验证"列出的 11 个常规场景 + 1 个 DEFECT 场景；DEFECT
// 场景在本次 D1 修复后应当转绿，按任务书"DEFECT 类用例修复后转绿并并入默认运行"要求
// 不再单独用环境变量区分，直接进入 build-wpJ1-tests.cmd 的默认场景名单。
// 相对审核者原文唯一的实质性改动：DEFECT 场景多补一行断言，核对新增的
// ConfigureRejectedReason() 诊断访问器在这条路径上确实返回
// ReentrantAccessDuringAuditFactory（本包修复 D1 时新增的枚举值，审核者写报告时
// 这个接口还不存在，自然没有断言）。
// 分发链条：wpJ1_scenarios_review.cpp 的 RunReviewScenario() 认不出名字时转调本文件
// 的 RunR2Scenario()；本文件认不出的名字再转调 wpJ1_scenarios_r2b.cpp 的
// RunR2bScenario()（本包这一轮自己新补的场景，见该文件顶部注释）。

#include "wpJ1_common.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QTimer>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <vector>

namespace wpj1_test
{
    namespace
    {
        // R2Entry：构造一个最简单的"绝对地址 + Bookmark"草稿，与其它两个场景文件里
        // 同名函数（MakeEntry）功能一致，各自放在各自文件的匿名命名空间（内部链接），
        // 不跨文件共享符号。
        ksword::memwb::AddressEntry R2Entry(const std::string& key, std::uint64_t address, const std::string& note)
        {
            ksword::memwb::AddressEntry e;
            e.kind = ksword::memwb::EntryKind::Bookmark;
            e.targetKey = key;
            e.absoluteAddress = address;
            e.note = note;
            e.valueType = ksword::memwb::ValueType::Hex8;
            return e;
        }

        // R2RunExecUntilQuit：真实跑一轮 QCoreApplication::exec()，排一个"立即触发"的
        // quit()，让 Qt 真的发出一次 aboutToQuit 信号，不是靠 QMetaObject::invokeMethod
        // 模拟。与 wpJ1_scenarios_review.cpp 里的 RunExecUntilQuit() 功能一致，各自文件
        // 内部链接，不需要去重成公共头文件。
        void R2RunExecUntilQuit()
        {
            QTimer::singleShot(0, QCoreApplication::instance(), &QCoreApplication::quit);
            QCoreApplication::instance()->exec();
        }

        // g_r2Warnings / R2WarnHandler：捕获 qWarning 次数的消息处理器，每个场景各自
        // 在独立进程里跑，文件级 static 计数器不会跨场景互相污染。
        int g_r2Warnings = 0;
        void R2WarnHandler(QtMsgType type, const QMessageLogContext&, const QString&)
        {
            if (type == QtWarningMsg)
            {
                ++g_r2Warnings;
            }
        }

        // CapturingSink：逐字段记录收到的审计记录，供断言"转发器真的原样转发了内容"，
        // 不像 FakeAuditSink 那样只计数——计数分不出"转发了一条默认构造的空记录"与
        // "转发了调用方真正填写的那条记录"。
        class CapturingSink final : public ksword::memwb::IAuditSink
        {
        public:
            explicit CapturingSink(std::vector<ksword::memwb::AuditRecord>* sink) : m_sink(sink) {}
            void Record(const ksword::memwb::AuditRecord& record) override { m_sink->push_back(record); }

        private:
            std::vector<ksword::memwb::AuditRecord>* m_sink;
        };

        // R2MemStore：满足 IPatchByteStore 的最小内存实现，供 Int3 转发参数验证场景
        // 构造出一个"真的能装上"的字节存储（不是处处返回失败的 FakeIoPort 那种假实现），
        // 因为这两个场景要断言 Install/Restore 真的成功，不只是断言参数被转发。
        class R2MemStore final : public ksword::memwb::IPatchByteStore
        {
        public:
            explicit R2MemStore(std::map<std::uint64_t, std::uint8_t>* mem) : m_mem(mem) {}
            bool ReadByte(std::uint64_t address, std::uint8_t& valueOut) override
            {
                auto it = m_mem->find(address);
                valueOut = it == m_mem->end() ? static_cast<std::uint8_t>(0x90) : it->second;
                return true;
            }
            bool WriteByte(std::uint64_t address, std::uint8_t value) override
            {
                (*m_mem)[address] = value;
                return true;
            }

        private:
            std::map<std::uint64_t, std::uint8_t>* m_mem;
        };

        // R2Call：记录一次 int3 工厂被调用时收到的参数，供逐字段核对"转发器原样转发"。
        struct R2Call
        {
            std::uint32_t pid;
            std::uint64_t createTime;
            std::uint64_t generation;
            ksword::memwb::Channel channel;
        };

        // R2Int3ForwardsArguments：两个"Int3 转发参数"场景的共用实现体，int3First 选择
        // 是先调 Int3()（走 Int3() 里新建控制器那条路径）还是先调 Configure()（走
        // Configure() 里新建控制器那条路径）——两处过去是两份文本相同的 lambda（S2），
        // 现在抽成了共用的 MakeInt3ForwardingFactory()，但转发逻辑本身仍然要分别从
        // 两个不同的"首次构造入口"各测一次，防止两条调用路径里任何一条意外绕开了
        // 共用方法、自己重新写了一份不同的实现。
        bool R2Int3ForwardsArguments(bool int3First)
        {
            bool ok = true;
            ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();
            std::vector<R2Call> calls;
            std::map<std::uint64_t, std::uint8_t> mem;
            ks::ui::Int3Controller* early = int3First ? &ws.Int3() : nullptr;
            ks::ui::WorkbenchShared::WorkbenchBackends b =
                MakeValidBackends(ScratchFilePath(QStringLiteral("R2Int3Args"), int3First ? QStringLiteral("a") : QStringLiteral("b")));
            b.int3Factory = [&](const ksword::memwb::PatchTarget& t, ksword::memwb::Channel c) -> std::unique_ptr<ksword::memwb::IPatchByteStore>
            {
                calls.push_back(R2Call{t.pid, t.processCreateTime100ns, t.attachGeneration, c});
                return std::make_unique<R2MemStore>(&mem);
            };
            ok = WPJ1_CHECK(ws.Configure(b), QStringLiteral("Configure 成功")) && ok;
            ks::ui::Int3Controller& c = ws.Int3();
            ok = WPJ1_CHECK(early == nullptr || early == &c, QStringLiteral("Configure 前后 Int3() 是同一个对象")) && ok;
            ksword::memwb::PatchTarget target;
            target.pid = 4321;
            target.processCreateTime100ns = 777;
            target.attachGeneration = 9;
            c.SetCurrentContext(target, ksword::memwb::Scope::ProcessVirtual, ksword::memwb::Channel::StandardDriver);
            const ks::ui::Int3InstallOutcome inst = c.Install(target, 0x1000, 1);
            ok = WPJ1_CHECK(inst.status == ksword::memwb::InstallStatus::Installed && inst.id != 0, QStringLiteral("Install 经转发工厂真的装上了")) && ok;
            ok = WPJ1_CHECK(calls.size() == 1, QStringLiteral("Install 恰好调用一次注入的 int3 工厂")) && ok;
            if (!calls.empty())
            {
                ok = WPJ1_CHECK(calls[0].pid == 4321 && calls[0].createTime == 777 && calls[0].generation == 9,
                    QStringLiteral("转发器把 PatchTarget 原样交给真实工厂（pid/创建时间/代次）")) && ok;
                ok = WPJ1_CHECK(calls[0].channel == ksword::memwb::Channel::StandardDriver,
                    QStringLiteral("转发器把通道原样交给真实工厂（安装通道=StandardDriver）")) && ok;
            }
            // 换到另一个通道之后还原：还原必须仍用安装时的通道，不随当前通道漂移。
            c.SetCurrentContext(target, ksword::memwb::Scope::ProcessVirtual, ksword::memwb::Channel::Hvm);
            const ks::ui::Int3RestoreOutcome rest = c.Restore(inst.id);
            ok = WPJ1_CHECK(rest.status == ksword::memwb::RestoreStatus::Restored, QStringLiteral("Restore 成功")) && ok;
            ok = WPJ1_CHECK(calls.size() == 2, QStringLiteral("Restore 又调用一次工厂")) && ok;
            if (calls.size() == 2)
            {
                ok = WPJ1_CHECK(calls[1].pid == 4321 && calls[1].channel == ksword::memwb::Channel::StandardDriver,
                    QStringLiteral("还原使用安装时记录的通道与目标，不是当前通道 Hvm")) && ok;
            }
            return ok;
        }

        // g_r2ExitPath / g_r2ExitExpect / R2ExitCheck：退出时读盘探针，用
        // std::atexit 在单例首次构造之前注册，所以会在单例的静态析构之后运行（后进
        // 先出）——专门用来观察"只有静态析构能补写"的那一批改动。
        QString g_r2ExitPath;
        std::size_t g_r2ExitExpect = 0;
        void R2ExitCheck()
        {
            ks::ui::AddressBookStore re(g_r2ExitPath);
            const bool loaded = re.load();
            WPJ1_CHECK(loaded && re.size() == g_r2ExitExpect,
                QStringLiteral("进程退出（静态析构之后）磁盘上的条目数等于预期：Shutdown 之后的改动由析构补写"));
        }
    }

    // 杀 C1：合法的重复 Configure 只因"已配置"被拒，不得顺带打印"访问发生在 Configure
    // 之前"的误导性诊断。
    bool ScenarioR2DuplicateConfigureNoSpuriousWarning()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();
        g_r2Warnings = 0;
        qInstallMessageHandler(R2WarnHandler);
        const bool first = ws.Configure(MakeValidBackends(ScratchFilePath(QStringLiteral("R2Dup"), QStringLiteral("A"))));
        const int afterFirst = g_r2Warnings;
        const bool second = ws.Configure(MakeValidBackends(ScratchFilePath(QStringLiteral("R2Dup"), QStringLiteral("B"))));
        const int afterSecond = g_r2Warnings;
        qInstallMessageHandler(nullptr);
        ok = WPJ1_CHECK(first && !second, QStringLiteral("第一次成功、第二次被拒")) && ok;
        ok = WPJ1_CHECK(afterFirst == 0, QStringLiteral("成功的 Configure 不打印警告")) && ok;
        ok = WPJ1_CHECK(afterSecond == 0, QStringLiteral("重复 Configure 被拒时不打印警告（它不是装配顺序错误）")) && ok;
        return ok;
    }

    // 杀 C2：Int3() 先于 Configure 构造控制器时，转发 lambda 必须把目标与通道原样交给
    // 真实工厂。
    bool ScenarioR2Int3ForwardsArgumentsInt3First()
    {
        return R2Int3ForwardsArguments(true);
    }

    // 杀 C3：Configure 内部构造控制器时同上（两份 lambda 文本相同但是两份代码，必须
    // 各测一次——即便本次修复已经把它们合并成共用的 MakeInt3ForwardingFactory()，两个
    // "首次构造入口"各自独立调用它这件事本身仍然值得各测一次）。
    bool ScenarioR2Int3ForwardsArgumentsConfigureFirst()
    {
        return R2Int3ForwardsArguments(false);
    }

    // 杀 C4/C5：转发器必须把审计记录的内容逐字段、恰好一次地交给真实接收器；Configure
    // 之前取得的引用同样适用。
    bool ScenarioR2AuditRecordContentForwarded()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();
        ksword::memwb::IAuditSink& pre = ws.AuditSink();
        std::vector<ksword::memwb::AuditRecord> got;
        ks::ui::WorkbenchShared::WorkbenchBackends b = MakeValidBackends(ScratchFilePath(QStringLiteral("R2Audit"), QStringLiteral("a")));
        b.auditSinkFactory = [&got]() -> std::unique_ptr<ksword::memwb::IAuditSink> { return std::make_unique<CapturingSink>(&got); };
        ok = WPJ1_CHECK(ws.Configure(b), QStringLiteral("Configure 成功")) && ok;
        ksword::memwb::AuditRecord rec;
        rec.event = ksword::memwb::AuditEvent::ApprovalAnswered;
        rec.targetIdentity = "target-identity-123";
        rec.blocksTotal = 7;
        rec.blockIndex = 3;
        rec.address = 0xABCDEF01ULL;
        rec.length = 16;
        rec.text = "hello-audit";
        pre.Record(rec); // 用 Configure 之前取得的引用写。
        ok = WPJ1_CHECK(got.size() == 1, QStringLiteral("一次 Record 到达真实接收器恰好一次")) && ok;
        if (got.size() == 1)
        {
            ok = WPJ1_CHECK(got[0].event == rec.event && got[0].targetIdentity == rec.targetIdentity && got[0].blocksTotal == 7
                    && got[0].blockIndex == 3 && got[0].address == 0xABCDEF01ULL && got[0].length == 16 && got[0].text == "hello-audit",
                QStringLiteral("记录内容逐字段原样到达（不是默认构造的空记录）")) && ok;
        }
        return ok;
    }

    // 杀 C9/C10：装配顺序错误（地址簿先被惰性访问）导致 Configure 被拒时，不得留下
    // 任何残留，IsConfigured() 仍为 false，审计工厂不得被调用，也不得因此让
    // CreateServices() 等变成"已配置"的样子。
    bool ScenarioR2RejectedByOrderLeavesNoResidue()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();
        (void)ws.AddressBook();
        int auditCalls = 0;
        ks::ui::WorkbenchShared::WorkbenchBackends b = MakeValidBackends(ScratchFilePath(QStringLiteral("R2Order"), QStringLiteral("a")));
        b.auditSinkFactory = [&auditCalls]() -> std::unique_ptr<ksword::memwb::IAuditSink>
        {
            ++auditCalls;
            return std::make_unique<FakeAuditSink>(nullptr);
        };
        ok = WPJ1_CHECK(!ws.Configure(b), QStringLiteral("装配顺序错误：Configure 返回 false")) && ok;
        ok = WPJ1_CHECK(!ws.IsConfigured(), QStringLiteral("被拒之后 IsConfigured() 仍为 false")) && ok;
        ok = WPJ1_CHECK(ws.CreateServices() == nullptr, QStringLiteral("被拒之后 CreateServices() 仍为空（无残留）")) && ok;
        ok = WPJ1_CHECK(ws.CreateIoPort() == nullptr, QStringLiteral("被拒之后 CreateIoPort() 仍为空")) && ok;
        ok = WPJ1_CHECK(ws.CreateKernelMutationPort() == nullptr, QStringLiteral("被拒之后 CreateKernelMutationPort() 仍为空")) && ok;
        ok = WPJ1_CHECK(auditCalls == 0, QStringLiteral("被拒的 Configure 不得调用审计工厂")) && ok;
        ok = WPJ1_CHECK(ws.UsesNullAudit(), QStringLiteral("被拒之后 UsesNullAudit() 仍为 true")) && ok;
        ok = WPJ1_CHECK(ws.AddressBook().filePath().isEmpty(), QStringLiteral("被拒之后地址簿仍是纯内存实例")) && ok;
        // 再试一次：状态一字节未变，同样被拒（本修复下无法恢复，这里只钉住"每次都
        // 一致地拒绝"）。
        ok = WPJ1_CHECK(!ws.Configure(MakeValidBackends(ScratchFilePath(QStringLiteral("R2Order"), QStringLiteral("b")))), QStringLiteral("重试仍被拒")) && ok;
        return ok;
    }

    // 杀 C8/A5：审计工厂抛异常之后不得留下任何成员残留（比 DefectThrowingAuditFactory...
    // 多断言一圈 Create*/UsesNullAudit/Int3 工厂）。
    bool ScenarioR2ThrowingAuditFactoryLeavesNoResidue()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();
        int int3Calls = 0;
        ks::ui::WorkbenchShared::WorkbenchBackends b = MakeValidBackends(ScratchFilePath(QStringLiteral("R2Throw"), QStringLiteral("a")));
        b.int3Factory = [&int3Calls](const ksword::memwb::PatchTarget&, ksword::memwb::Channel) -> std::unique_ptr<ksword::memwb::IPatchByteStore>
        {
            ++int3Calls;
            return nullptr;
        };
        b.auditSinkFactory = []() -> std::unique_ptr<ksword::memwb::IAuditSink> { throw std::runtime_error("audit log cannot be opened"); };
        bool threw = false;
        try
        {
            (void)ws.Configure(b);
        }
        catch (const std::exception&)
        {
            threw = true;
        }
        ok = WPJ1_CHECK(threw, QStringLiteral("异常从 Configure 传播出来")) && ok;
        ok = WPJ1_CHECK(!ws.IsConfigured(), QStringLiteral("IsConfigured() 仍为 false")) && ok;
        ok = WPJ1_CHECK(ws.CreateServices() == nullptr && ws.CreateIoPort() == nullptr && ws.CreateKernelMutationPort() == nullptr,
            QStringLiteral("三个 Create* 仍为空：没有任何工厂被提前提交")) && ok;
        ok = WPJ1_CHECK(ws.UsesNullAudit(), QStringLiteral("UsesNullAudit() 仍为 true")) && ok;
        const ksword::memwb::PatchTarget target{};
        ws.Int3().Install(target, 0x2000ULL, 1ULL);
        ok = WPJ1_CHECK(int3Calls == 0, QStringLiteral("异常后 int3 工厂没有被提交（Install 不会落到它上面）")) && ok;
        ok = WPJ1_CHECK(ws.Configure(MakeValidBackends(ScratchFilePath(QStringLiteral("R2Throw"), QStringLiteral("a")))),
            QStringLiteral("异常之后用合法配置重试成功（没有被地址簿残留拦成装配顺序错误）")) && ok;
        return ok;
    }

    // 杀 C7：审计工厂抛异常时，磁盘上的坏地址簿文件不得已被 load() 改名走（Configure
    // 失败应当对磁盘也"一字节未变"），重试之后"加载失败"的指示必须仍然存在。
    bool ScenarioR2ThrowingAuditFactoryDoesNotTouchDisk()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();
        const QString path = ScratchFilePath(QStringLiteral("R2ThrowDisk"), QStringLiteral("book"));
        QFile::remove(path);
        {
            const QFileInfo fi(path);
            QDir dir(fi.absolutePath());
            for (const QString& n : dir.entryList(QStringList() << (fi.fileName() + QStringLiteral(".bad-*")), QDir::Files))
            {
                dir.remove(n);
            }
        }
        {
            QFile bad(path);
            ok = WPJ1_CHECK(bad.open(QIODevice::WriteOnly), QStringLiteral("写入坏文件")) && ok;
            bad.write("this is not an address book\n");
        }
        ks::ui::WorkbenchShared::WorkbenchBackends b = MakeValidBackends(path);
        b.auditSinkFactory = []() -> std::unique_ptr<ksword::memwb::IAuditSink> { throw std::runtime_error("audit log cannot be opened"); };
        bool threw = false;
        try
        {
            (void)ws.Configure(b);
        }
        catch (const std::exception&)
        {
            threw = true;
        }
        ok = WPJ1_CHECK(threw, QStringLiteral("异常传播")) && ok;
        ok = WPJ1_CHECK(QFile::exists(path), QStringLiteral("失败的 Configure 没有把坏文件改名走（磁盘一字节未变）")) && ok;
        ok = WPJ1_CHECK(ws.Configure(MakeValidBackends(path)), QStringLiteral("重试成功")) && ok;
        ok = WPJ1_CHECK(ws.AddressBook().lastLoadFailed(), QStringLiteral("重试后仍能报告“地址簿文件加载失败”")) && ok;
        return ok;
    }

    // 杀 C11/A9/A7：两次退出信号之间的新改动、以及直接 Shutdown 之后的新改动，都必须
    // 被再次落盘。
    bool ScenarioR2ShutdownFlushesEditsAfterEarlierFlush()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();
        const QString path = ScratchFilePath(QStringLiteral("R2Reflush"), QStringLiteral("book"));
        QFile::remove(path);
        ok = WPJ1_CHECK(ws.Configure(MakeValidBackends(path)), QStringLiteral("Configure 成功")) && ok;
        ks::ui::AddressBookStore& st = ws.AddressBook();
        st.add(R2Entry("r2", 0x1, "e1"));
        R2RunExecUntilQuit();
        ok = WPJ1_CHECK(st.writeCount() == 1, QStringLiteral("第一次真实 aboutToQuit 落盘一次")) && ok;
        st.add(R2Entry("r2", 0x2, "e2"));
        ok = WPJ1_CHECK(st.pendingSave(), QStringLiteral("两次退出之间的新改动处于待落盘")) && ok;
        R2RunExecUntilQuit();
        ok = WPJ1_CHECK(st.writeCount() == 2 && !st.pendingSave(), QStringLiteral("第二次真实 aboutToQuit 把新改动落盘（连接不是一次性的）")) && ok;
        st.add(R2Entry("r2", 0x3, "e3"));
        ws.Shutdown();
        ok = WPJ1_CHECK(st.writeCount() == 3 && !st.pendingSave(), QStringLiteral("直接 Shutdown 把第三次改动落盘（不是只落一次）")) && ok;
        ks::ui::AddressBookStore re(path);
        ok = WPJ1_CHECK(re.load() && re.size() == 3, QStringLiteral("磁盘上有三条")) && ok;
        return ok;
    }

    // 杀 C12：Shutdown 只落盘，不得销毁/重置任何共享对象（旧头文件文字曾写"清理共享
    // 对象"）。
    bool ScenarioR2ShutdownKeepsSharedObjectsAlive()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();
        // 【本包修复：补上原夹具缺的 QFile::remove】审核者原文这里没有先清掉上一次
        // 运行留下的同名文件——MEMWB_OUT 目录在重复执行 build-wpJ1-tests.cmd 之间是
        // 持久的（不是每次都清空），不先删除的话第二次往后跑，Configure() 会把上一次
        // 落盘的那一条"keep"记录重新加载进来，store->add() 再加一条，rowCount() 就
        // 不再是断言要求的 1，而是累加——其它所有场景在构造真实路径前都有这一行，
        // 这一个是本次并入时发现的唯一遗漏，补上以后这个场景才是可重复执行的。
        const QString path = ScratchFilePath(QStringLiteral("R2Keep"), QStringLiteral("book"));
        QFile::remove(path);
        ok = WPJ1_CHECK(ws.Configure(MakeValidBackends(path)), QStringLiteral("Configure 成功")) && ok;
        QPointer<ks::ui::AddressBookStore> store(&ws.AddressBook());
        QPointer<ks::ui::AddressBookModel> model(&ws.AddressBookTable());
        QPointer<ks::ui::Int3Controller> int3(&ws.Int3());
        store->add(R2Entry("r2", 0x1, "keep"));
        ws.Shutdown();
        R2RunExecUntilQuit();
        ok = WPJ1_CHECK(store && model && int3, QStringLiteral("Shutdown / aboutToQuit 之后三个共享对象仍然存活")) && ok;
        ok = WPJ1_CHECK(&ws.AddressBookTable() == model.data() && ws.AddressBookTable().rowCount() == 1, QStringLiteral("模型仍可访问且行数正确")) && ok;
        return ok;
    }

    // 杀 C13：Shutdown 之后才发生的改动只能由静态析构补写；用 atexit 在析构之后读磁盘。
    bool ScenarioR2ExitTimeDestructorFlush()
    {
        g_r2ExitPath = ScratchFilePath(QStringLiteral("R2Exit"), QStringLiteral("book"));
        QFile::remove(g_r2ExitPath);
        g_r2ExitExpect = 2;
        std::atexit(R2ExitCheck); // 必须先于单例的首次构造注册，才会在其静态析构之后运行。
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();
        ok = WPJ1_CHECK(ws.Configure(MakeValidBackends(g_r2ExitPath)), QStringLiteral("Configure 成功")) && ok;
        ws.AddressBook().add(R2Entry("r2", 0x1, "before-shutdown"));
        ws.Shutdown();
        ws.AddressBook().add(R2Entry("r2", 0x2, "after-shutdown-only-dtor-can-save"));
        ok = WPJ1_CHECK(ws.AddressBook().pendingSave(), QStringLiteral("Shutdown 之后的改动处于待落盘，只能靠静态析构补写")) && ok;
        return ok;
    }

    // 杀 C14：空路径 = 仅内存，是文档化的合法输入，Configure 必须接受。
    bool ScenarioR2EmptyPathConfigureAccepted()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();
        ok = WPJ1_CHECK(ws.Configure(MakeValidBackends(QString())), QStringLiteral("空路径（仅内存）Configure 成功")) && ok;
        ok = WPJ1_CHECK(ws.IsConfigured() && ws.AddressBook().filePath().isEmpty(), QStringLiteral("已配置且地址簿为纯内存")) && ok;
        ok = WPJ1_CHECK(ws.AddressBook().add(R2Entry("r2", 0x1, "mem")) != 0 && !ws.AddressBook().pendingSave(), QStringLiteral("仍可添加条目且不会调度落盘")) && ok;
        return ok;
    }

    // DEFECT（本轮审核新发现，D1）：审计工厂在 Configure 内部重入 AddressBook()，
    // 随后 Configure 提交时把重入创建出来的存储整份替换掉，重入方手里的引用悬空——
    // 正是 C1 想杜绝的形状，只是入口换成了回调。本包修复后应当转绿：Configure() 在
    // 审计工厂回调返回后复核一次，发现地址簿已经非空就整次拒绝、不碰回调里创建出来
    // 的对象。相对审核者原文多补一行：核对 ConfigureRejectedReason()（本次 D1 修复
    // 新增的诊断访问器，审核者写报告时还不存在）确实能读出具体原因，不只是一个裸的
    // false。
    bool ScenarioR2DefectReentrantAuditFactoryDoesNotDangle()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();
        QPointer<ks::ui::AddressBookStore> grabbed;
        ks::ui::AddressBookStore* raw = nullptr;
        ks::ui::WorkbenchShared::WorkbenchBackends b = MakeValidBackends(ScratchFilePath(QStringLiteral("R2Reenter"), QStringLiteral("book")));
        b.auditSinkFactory = [&]() -> std::unique_ptr<ksword::memwb::IAuditSink>
        {
            raw = &ws.AddressBook();
            grabbed = raw;
            return std::make_unique<FakeAuditSink>(nullptr);
        };
        const bool cfg = ws.Configure(b);
        ok = WPJ1_CHECK(!cfg || (grabbed && &ws.AddressBook() == raw),
            QStringLiteral("审计工厂内取得的地址簿引用：要么 Configure 拒绝，要么该对象在 Configure 之后仍存活且身份不变")) && ok;
        ok = WPJ1_CHECK(!cfg, QStringLiteral("本包修复后：这次 Configure 调用确实被拒绝（不是侥幸地恰好返回 true）")) && ok;
        ok = WPJ1_CHECK(ws.ConfigureRejectedReason() == ks::ui::WorkbenchShared::ConfigureRejection::ReentrantAccessDuringAuditFactory,
            QStringLiteral("ConfigureRejectedReason() 能读出具体原因：审计工厂回调里重入访问了地址簿")) && ok;
        return ok;
    }

    bool RunR2Scenario(const QString& name)
    {
        if (name == QStringLiteral("R2DuplicateConfigureNoSpuriousWarning")) return ScenarioR2DuplicateConfigureNoSpuriousWarning();
        if (name == QStringLiteral("R2Int3ForwardsArgumentsInt3First")) return ScenarioR2Int3ForwardsArgumentsInt3First();
        if (name == QStringLiteral("R2Int3ForwardsArgumentsConfigureFirst")) return ScenarioR2Int3ForwardsArgumentsConfigureFirst();
        if (name == QStringLiteral("R2AuditRecordContentForwarded")) return ScenarioR2AuditRecordContentForwarded();
        if (name == QStringLiteral("R2RejectedByOrderLeavesNoResidue")) return ScenarioR2RejectedByOrderLeavesNoResidue();
        if (name == QStringLiteral("R2ThrowingAuditFactoryLeavesNoResidue")) return ScenarioR2ThrowingAuditFactoryLeavesNoResidue();
        if (name == QStringLiteral("R2ThrowingAuditFactoryDoesNotTouchDisk")) return ScenarioR2ThrowingAuditFactoryDoesNotTouchDisk();
        if (name == QStringLiteral("R2ShutdownFlushesEditsAfterEarlierFlush")) return ScenarioR2ShutdownFlushesEditsAfterEarlierFlush();
        if (name == QStringLiteral("R2ShutdownKeepsSharedObjectsAlive")) return ScenarioR2ShutdownKeepsSharedObjectsAlive();
        if (name == QStringLiteral("R2ExitTimeDestructorFlush")) return ScenarioR2ExitTimeDestructorFlush();
        if (name == QStringLiteral("R2EmptyPathConfigureAccepted")) return ScenarioR2EmptyPathConfigureAccepted();
        if (name == QStringLiteral("R2DefectReentrantAuditFactoryDoesNotDangle")) return ScenarioR2DefectReentrantAuditFactoryDoesNotDangle();

        return RunR2bScenario(name);
    }
}
