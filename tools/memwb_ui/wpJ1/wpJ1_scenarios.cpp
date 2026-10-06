// wpJ1_scenarios.cpp
// 作用：WP-J1（WorkbenchShared）离屏验证夹具的全部场景函数。每个场景对应
// "wpJ1_main.cpp 以子进程方式独立跑一次"的一个单元——详见 wpJ1_main.cpp 顶部注释里
// "单例跨场景污染"的取舍说明：单个场景函数内部可以放心假设 WorkbenchShared::Instance()
// 此刻是全新、从未 Configure 过的状态，不需要（也没有办法）重置它。
//
// 每个场景函数签名为 bool ScenarioXxx()，内部用 WPJ1_CHECK 记录每一条断言并打印一行，
// 返回值是"这个场景内部的全部断言是否都通过"（供本进程的退出码使用，真正的判定仍以
// stdout 上的 CHECK 行文本为准）。文件按"场景分组"组织，组之间用分隔注释隔开，
// 对应接口文档 §7 给 WorkbenchShared 列出的关键判断逐条覆盖，以及任务书额外要求的
// 几项（地址簿真实路径、Shutdown 幂等、AuditSink 身份）。

#include "wpJ1_common.h"

#include <QFile>

namespace wpj1_test
{
    namespace
    {
        // MakeEntry：构造一个最简单的"绝对地址 + Bookmark"草稿，供需要真的写一条
        // 地址簿记录的场景复用，不每次都重复敲六行字段赋值。
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
    }

    // ------------------------------------------------------------
    // 场景 1：未 Configure 时的退化行为（接口文档 §7 第二、三条关键判断）
    // ------------------------------------------------------------
    bool ScenarioDegradedBeforeConfigure()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();

        ok = WPJ1_CHECK(!ws.IsConfigured(), QStringLiteral("进程启动后 IsConfigured() 为 false")) && ok;
        ok = WPJ1_CHECK(ws.CreateServices() == nullptr, QStringLiteral("未 Configure 时 CreateServices() 返回空指针")) && ok;
        ok = WPJ1_CHECK(ws.CreateIoPort() == nullptr, QStringLiteral("未 Configure 时 CreateIoPort() 返回空指针")) && ok;
        ok = WPJ1_CHECK(ws.CreateKernelMutationPort() == nullptr, QStringLiteral("未 Configure 时 CreateKernelMutationPort() 返回空指针")) && ok;
        ok = WPJ1_CHECK(ws.AddressBook().filePath().isEmpty(), QStringLiteral("未 Configure 时地址簿是纯内存模式（filePath 为空）")) && ok;
        ok = WPJ1_CHECK(ws.UsesNullAudit(), QStringLiteral("未 Configure 时 UsesNullAudit() 为 true")) && ok;

        // AuditSink 身份在未 Configure 期间保持稳定（同一个内置空接收器，不是每次新建）。
        ksword::memwb::IAuditSink& audit1 = ws.AuditSink();
        ksword::memwb::IAuditSink& audit2 = ws.AuditSink();
        ok = WPJ1_CHECK(&audit1 == &audit2, QStringLiteral("未 Configure 时 AuditSink() 两次调用返回同一个对象")) && ok;

        // 地址簿存储/模型的身份也保持稳定（惰性创建只发生一次）。
        ks::ui::AddressBookStore& store1 = ws.AddressBook();
        ks::ui::AddressBookStore& store2 = ws.AddressBook();
        ok = WPJ1_CHECK(&store1 == &store2, QStringLiteral("未 Configure 时 AddressBook() 两次调用返回同一个对象")) && ok;

        // Int3：路由预检通过（默认范围/通道合法），但工厂拒绝一切，Install 恒为 WriteFailed。
        const ksword::memwb::PatchTarget target{};
        const ks::ui::Int3InstallOutcome outcome = ws.Int3().Install(target, 0x2000ULL, 1ULL);
        ok = WPJ1_CHECK(outcome.routeReject == ks::ui::Int3RouteReject::None, QStringLiteral("未 Configure 时 Int3 默认范围/通道不被路由预检拒绝")) && ok;
        ok = WPJ1_CHECK(outcome.status == ksword::memwb::InstallStatus::WriteFailed, QStringLiteral("未 Configure 时 Int3 工厂拒绝一切，Install 恒为 WriteFailed")) && ok;
        ok = WPJ1_CHECK(outcome.id == 0, QStringLiteral("未 Configure 时 Int3 Install 失败不消耗账本 id")) && ok;

        // AddressBookTable 与 AddressBook 成对创建：先看初始行数为 0，再加一条看是否同步变 1。
        ok = WPJ1_CHECK(ws.AddressBookTable().rowCount() == 0, QStringLiteral("未 Configure 时地址簿表初始为空")) && ok;
        const std::uint64_t newId = ws.AddressBook().add(MakeEntry("t-degraded", 0x3000ULL, "degraded"));
        ok = WPJ1_CHECK(newId != 0, QStringLiteral("未 Configure 时仍可以往仅内存的地址簿里添加条目")) && ok;
        ok = WPJ1_CHECK(ws.AddressBookTable().rowCount() == 1, QStringLiteral("AddressBookTable() 与 AddressBook() 共用同一份存储，新增后同步可见")) && ok;

        return ok;
    }

    // ------------------------------------------------------------
    // 场景 2：Configure 校验顺序——三种无效组合各自被拒绝且不消耗"只生效一次"的机会，
    // 最后一次合法调用才真正生效（接口文档 §7 第一条关键判断的前半段）。
    // ------------------------------------------------------------
    bool ScenarioConfigureRejectsInvalidBackendsThenSucceeds()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();
        const QString realPath = ScratchFilePath(QStringLiteral("ConfigureInvalid"), QStringLiteral("real"));
        QFile::remove(realPath);

        // 缺 ioPortFactory。
        ks::ui::WorkbenchShared::WorkbenchBackends missingIo = MakeValidBackends(realPath);
        missingIo.ioPortFactory = nullptr;
        ok = WPJ1_CHECK(!ws.Configure(missingIo), QStringLiteral("缺 ioPortFactory 时 Configure 返回 false")) && ok;
        ok = WPJ1_CHECK(!ws.IsConfigured(), QStringLiteral("缺 ioPortFactory 的失败调用之后 IsConfigured() 仍为 false")) && ok;

        // 缺 servicesFactory。
        ks::ui::WorkbenchShared::WorkbenchBackends missingServices = MakeValidBackends(realPath);
        missingServices.servicesFactory = nullptr;
        ok = WPJ1_CHECK(!ws.Configure(missingServices), QStringLiteral("缺 servicesFactory 时 Configure 返回 false")) && ok;

        // 缺 int3Factory。
        ks::ui::WorkbenchShared::WorkbenchBackends missingInt3 = MakeValidBackends(realPath);
        missingInt3.int3Factory = nullptr;
        ok = WPJ1_CHECK(!ws.Configure(missingInt3), QStringLiteral("缺 int3Factory 时 Configure 返回 false")) && ok;
        ok = WPJ1_CHECK(!ws.IsConfigured(), QStringLiteral("三次无效调用之后 IsConfigured() 依然为 false（没有被消耗）")) && ok;
        // 【wave3 审核修复 C1 之后的必改点】这里不能再调用 ws.AddressBook()：那会惰性
        // 创建出地址簿存储，而 Configure() 现在把"之前已经惰性访问过地址簿"视为装配
        // 顺序错误，会让紧接着下面那次本该成功的合法 Configure 也被拒绝——改用不会
        // 触发地址簿惰性创建的 CreateServices() 验证"三次无效调用确实什么都没改"。
        ok = WPJ1_CHECK(ws.CreateServices() == nullptr, QStringLiteral("三次无效调用之后 CreateServices() 仍为空")) && ok;

        // 最后一次合法调用才真正生效。
        ok = WPJ1_CHECK(ws.Configure(MakeValidBackends(realPath)), QStringLiteral("全部字段合法时 Configure 返回 true")) && ok;
        ok = WPJ1_CHECK(ws.IsConfigured(), QStringLiteral("合法调用之后 IsConfigured() 变为 true")) && ok;
        ok = WPJ1_CHECK(ws.AddressBook().filePath() == realPath, QStringLiteral("合法调用之后地址簿路径变为真实传入的路径")) && ok;

        return ok;
    }

    // ------------------------------------------------------------
    // 场景 3：Configure 只生效一次——即便第二次传入的是另一份完全合法的配置，也必须
    // 被拒绝，且已经持有的三个共享对象身份与内容都不受影响（接口文档 §7 第一条关键
    // 判断的后半段："重复调用返回 false 且不改已持有对象"）。
    // ------------------------------------------------------------
    bool ScenarioConfigureOnceOnlyKeepsOriginal()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();
        const QString pathA = ScratchFilePath(QStringLiteral("ConfigureOnce"), QStringLiteral("A"));
        const QString pathB = ScratchFilePath(QStringLiteral("ConfigureOnce"), QStringLiteral("B"));
        QFile::remove(pathA);
        QFile::remove(pathB);

        ok = WPJ1_CHECK(ws.Configure(MakeValidBackends(pathA)), QStringLiteral("第一次合法 Configure 成功")) && ok;

        ks::ui::AddressBookStore& storeBefore = ws.AddressBook();
        ks::ui::AddressBookModel& tableBefore = ws.AddressBookTable();
        ks::ui::Int3Controller& int3Before = ws.Int3();

        const bool secondResult = ws.Configure(MakeValidBackends(pathB));
        ok = WPJ1_CHECK(!secondResult, QStringLiteral("第二次合法 Configure（不同路径）仍被拒绝")) && ok;
        ok = WPJ1_CHECK(ws.AddressBook().filePath() == pathA, QStringLiteral("第二次调用之后地址簿路径仍是第一次传入的 pathA，不是 pathB")) && ok;
        ok = WPJ1_CHECK(&ws.AddressBook() == &storeBefore, QStringLiteral("第二次调用之后 AddressBook() 身份未变")) && ok;
        ok = WPJ1_CHECK(&ws.AddressBookTable() == &tableBefore, QStringLiteral("第二次调用之后 AddressBookTable() 身份未变")) && ok;
        ok = WPJ1_CHECK(&ws.Int3() == &int3Before, QStringLiteral("第二次调用之后 Int3() 身份未变")) && ok;

        return ok;
    }

    // ------------------------------------------------------------
    // 场景 4：真实路径下的持久化往返 + Shutdown 幂等（任务书额外要求的两项）。
    // ------------------------------------------------------------
    bool ScenarioRealPathPersistenceAndShutdownIdempotent()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();
        const QString path = ScratchFilePath(QStringLiteral("Persistence"), QStringLiteral("book"));
        QFile::remove(path);

        // 先用一份完全独立的 AddressBookStore 实例预先写一条记录到同一个路径，
        // 核对 Configure() 真的会把"已有文件内容"加载进来，而不是每次都从空簿起步
        // （见 WorkbenchShared::Configure 实现里"真实路径下尝试把已有文件内容加载
        // 进来"那一段注释）。
        {
            ks::ui::AddressBookStore seedStore(path);
            const std::uint64_t seedId = seedStore.add(MakeEntry("procSeed", 0x500ULL, "pre-existing"));
            ok = WPJ1_CHECK(seedId != 0, QStringLiteral("预先写入一条种子记录成功")) && ok;
            ok = WPJ1_CHECK(seedStore.flushNow(), QStringLiteral("种子记录落盘成功")) && ok;
        }

        ok = WPJ1_CHECK(ws.Configure(MakeValidBackends(path)), QStringLiteral("用真实临时路径 Configure 成功")) && ok;

        ks::ui::AddressBookStore& store = ws.AddressBook();
        ok = WPJ1_CHECK(store.size() == 1, QStringLiteral("Configure 时把已有文件内容真的加载进来了（size 为 1）")) && ok;

        const std::uint64_t id1 = store.add(MakeEntry("procA", 0x1000ULL, "note-one"));
        const std::uint64_t id2 = store.add(MakeEntry("procA", 0x2000ULL, "note-two"));
        ok = WPJ1_CHECK(id1 != 0 && id2 != 0 && id1 != id2, QStringLiteral("两条真实条目都添加成功且 id 不同")) && ok;
        ok = WPJ1_CHECK(store.writeCount() == 0, QStringLiteral("add 之后、Shutdown 之前，防抖窗口内还没有真的落盘（writeCount 仍为 0）")) && ok;
        ok = WPJ1_CHECK(ws.AddressBookTable().rowCount() == 3, QStringLiteral("AddressBookTable() 在 Configure 之后仍与 AddressBook() 共用同一份存储（种子 1 条 + 新增 2 条 = 3 行）")) && ok;

        ws.Shutdown();
        ok = WPJ1_CHECK(store.writeCount() == 1, QStringLiteral("第一次 Shutdown 触发恰好一次落盘")) && ok;

        // 用另一份完全独立的 AddressBookStore 实例重新打开同一个文件，核对内容真的落了地。
        ks::ui::AddressBookStore reopened(path);
        const bool loadOk = reopened.load();
        ok = WPJ1_CHECK(loadOk && !reopened.lastLoadFailed(), QStringLiteral("落盘后的文件可以被另一个 AddressBookStore 实例正确加载")) && ok;
        ok = WPJ1_CHECK(reopened.size() == 3, QStringLiteral("重新加载后条目数等于种子 1 条 + 新增 2 条")) && ok;
        ok = WPJ1_CHECK(reopened.find(id1).has_value() && reopened.find(id1)->note == "note-one", QStringLiteral("重新加载后第一条新增内容与添加时一致")) && ok;
        ok = WPJ1_CHECK(reopened.find(id2).has_value() && reopened.find(id2)->note == "note-two", QStringLiteral("重新加载后第二条新增内容与添加时一致")) && ok;

        // 第二次 Shutdown：空操作，writeCount 不应再增加。
        ws.Shutdown();
        ok = WPJ1_CHECK(store.writeCount() == 1, QStringLiteral("第二次 Shutdown 是空操作，writeCount 保持为 1")) && ok;

        return ok;
    }

    // ------------------------------------------------------------
    // 场景 5：从未 Configure、也从未访问过任何共享对象时调用 Shutdown 的安全性。
    // ------------------------------------------------------------
    bool ScenarioShutdownNeverConfiguredIsSafe()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();

        // 刻意不调用 AddressBook()/Configure()，直接两次 Shutdown：既不应该崩溃，
        // 也不应该意外地把 addressBookStore_ 创建出来（创建了也无妨——但如果创建出来，
        // 后续 AddressBook() 应该拿到的还是同一份退化实例，不应该在 Shutdown 内部
        // 产生任何可观察的副作用）。
        ws.Shutdown();
        ws.Shutdown();
        ok = WPJ1_CHECK(!ws.IsConfigured(), QStringLiteral("从未 Configure 时，两次 Shutdown 之后 IsConfigured() 仍为 false")) && ok;
        ok = WPJ1_CHECK(ws.AddressBook().filePath().isEmpty(), QStringLiteral("两次 Shutdown 之后第一次访问 AddressBook() 仍是退化的纯内存实例")) && ok;

        return ok;
    }

    // ------------------------------------------------------------
    // 场景 6：Configure 时未提供 auditSinkFactory——UsesNullAudit() 在配置前后都保持
    // true，且 AuditSink() 的身份在 Configure 前后都不变（因为 auditSink_ 从未被
    // 赋过非空值，一直返回同一个内置 nullAudit_）。
    // ------------------------------------------------------------
    bool ScenarioAuditSinkDefaultsToNullWhenFactoryOmitted()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();

        ksword::memwb::IAuditSink* beforePtr = &ws.AuditSink();
        ok = WPJ1_CHECK(ws.UsesNullAudit(), QStringLiteral("Configure 之前 UsesNullAudit() 为 true")) && ok;

        // 【本包自补的新变异验证】Configure 之前，auditSink_ 仍是空 unique_ptr，
        // 转发器 AuditSinkForwarder::Record() 必须先判空再转发——这里真的调用一次
        // Record()，如果判空逻辑被误删（直接解引用空的 unique_ptr），进程会在这里
        // 访问违规崩溃，而不是走到后面任何一条 CHECK；跑到这一行之后还能继续打印
        // CHECK OK 本身就是"没有崩溃"的证据。
        ksword::memwb::AuditRecord dummyBeforeConfigure;
        ws.AuditSink().Record(dummyBeforeConfigure);
        ok = WPJ1_CHECK(true, QStringLiteral("Configure 之前对 AuditSink() 调用 Record() 不崩溃（转发器判空后原地丢弃）")) && ok;

        const QString path = ScratchFilePath(QStringLiteral("AuditNull"), QStringLiteral("book"));
        QFile::remove(path);
        ks::ui::WorkbenchShared::WorkbenchBackends backends = MakeValidBackends(path);
        // 故意不设置 auditSinkFactory（保持默认空 std::function）。
        ok = WPJ1_CHECK(ws.Configure(backends), QStringLiteral("不提供 auditSinkFactory 时 Configure 仍能成功（它是可选字段）")) && ok;
        ok = WPJ1_CHECK(ws.UsesNullAudit(), QStringLiteral("Configure 之后仍没有提供审计工厂，UsesNullAudit() 保持 true")) && ok;
        ok = WPJ1_CHECK(&ws.AuditSink() == beforePtr, QStringLiteral("没有真实审计接收器时，AuditSink() 在 Configure 前后身份不变")) && ok;

        // 同一个判空逻辑在 Configure 之后（auditSink_ 仍为空，因为没有提供工厂）
        // 再验证一次，覆盖"判空条件只在构造早期生效、Configure 之后反而漏判"这类
        // 更细的回归。
        ksword::memwb::AuditRecord dummyAfterConfigure;
        ws.AuditSink().Record(dummyAfterConfigure);
        ok = WPJ1_CHECK(true, QStringLiteral("Configure 之后（仍是空审计）调用 Record() 依然不崩溃")) && ok;

        return ok;
    }

    // ------------------------------------------------------------
    // 场景 7：Configure 时提供 auditSinkFactory——UsesNullAudit() 变为 false，
    // AuditSink() 身份切换为工厂产出的那一份且保持稳定，Record 调用真的落到了
    // 我注入的假实现上（通过计数器观察，不是内置的空接收器）。
    // ------------------------------------------------------------
    bool ScenarioAuditSinkRealFactoryUsedAndStable()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();
        ksword::memwb::IAuditSink* beforePtr = &ws.AuditSink();

        const QString path = ScratchFilePath(QStringLiteral("AuditReal"), QStringLiteral("book"));
        QFile::remove(path);
        ks::ui::WorkbenchShared::WorkbenchBackends backends = MakeValidBackends(path);
        int recordCount = 0;
        backends.auditSinkFactory = [&recordCount]() -> std::unique_ptr<ksword::memwb::IAuditSink>
        {
            return std::make_unique<FakeAuditSink>(&recordCount);
        };
        ok = WPJ1_CHECK(ws.Configure(backends), QStringLiteral("提供 auditSinkFactory 时 Configure 成功")) && ok;
        ok = WPJ1_CHECK(!ws.UsesNullAudit(), QStringLiteral("提供了真实审计工厂之后 UsesNullAudit() 变为 false")) && ok;

        ksword::memwb::IAuditSink& afterRef = ws.AuditSink();
        // 【wave3 审核修复 C4 之后的必改点】AuditSink() 现在恒定返回同一个转发器
        // 对象（身份从单例构造那一刻起就固定），不会因为 Configure 把真实接收器注入
        // 进去就"切换成另一份"——改为断言身份不变，真正变化的是 Record() 调用最终
        // 被转发到谁手里（下面用计数器验证）。
        ok = WPJ1_CHECK(&afterRef == beforePtr, QStringLiteral("Configure 之后 AuditSink() 身份保持不变（转发器恒定，Record 转发给新注入的真实接收器）")) && ok;

        ksword::memwb::AuditRecord dummy;
        ws.AuditSink().Record(dummy);
        ws.AuditSink().Record(dummy);
        ok = WPJ1_CHECK(recordCount == 2, QStringLiteral("两次 Record 调用都真的落到了注入的假审计接收器上（计数器为 2）")) && ok;
        ok = WPJ1_CHECK(&ws.AuditSink() == &afterRef, QStringLiteral("Configure 之后 AuditSink() 反复调用身份保持不变（单例持有同一份实例，不是每次新建）")) && ok;

        return ok;
    }

    // ------------------------------------------------------------
    // 场景 8：三个端口工厂在 Configure 之后"每次调用产出独立实例、不缓存"。
    // ------------------------------------------------------------
    bool ScenarioPortFactoriesProduceFreshInstancesEachCall()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();
        const QString path = ScratchFilePath(QStringLiteral("PortFactories"), QStringLiteral("book"));
        QFile::remove(path);

        int servicesCalls = 0;
        int ioCalls = 0;
        int kernelCalls = 0;
        ok = WPJ1_CHECK(
            ws.Configure(MakeValidBackends(path, &servicesCalls, &ioCalls, &kernelCalls)),
            QStringLiteral("带计数工厂的 Configure 成功")) && ok;

        auto io1 = ws.CreateIoPort();
        auto io2 = ws.CreateIoPort();
        ok = WPJ1_CHECK(io1 != nullptr && io2 != nullptr, QStringLiteral("两次 CreateIoPort() 都产出非空对象")) && ok;
        ok = WPJ1_CHECK(io1.get() != io2.get(), QStringLiteral("两次 CreateIoPort() 产出两个不同地址的独立实例")) && ok;
        ok = WPJ1_CHECK(ioCalls == 2, QStringLiteral("ioPortFactory 恰好被调用两次")) && ok;

        auto kernel1 = ws.CreateKernelMutationPort();
        auto kernel2 = ws.CreateKernelMutationPort();
        ok = WPJ1_CHECK(kernel1 != nullptr && kernel2 != nullptr, QStringLiteral("两次 CreateKernelMutationPort() 都产出非空对象")) && ok;
        ok = WPJ1_CHECK(kernel1.get() != kernel2.get(), QStringLiteral("两次 CreateKernelMutationPort() 产出两个不同地址的独立实例")) && ok;
        ok = WPJ1_CHECK(kernelCalls == 2, QStringLiteral("kernelPortFactory 恰好被调用两次")) && ok;

        auto services1 = ws.CreateServices();
        auto services2 = ws.CreateServices();
        ok = WPJ1_CHECK(services1 != nullptr && services2 != nullptr, QStringLiteral("两次 CreateServices() 都产出非空对象")) && ok;
        ok = WPJ1_CHECK(services1.get() != services2.get(), QStringLiteral("两次 CreateServices() 产出两个不同地址的独立实例")) && ok;
        ok = WPJ1_CHECK(servicesCalls == 2, QStringLiteral("servicesFactory 恰好被调用两次")) && ok;

        return ok;
    }

    // ------------------------------------------------------------
    // 场景 9：kernelPortFactory 在 Configure 时留空（可选字段）——Configure 仍然成功，
    // CreateKernelMutationPort() 如实返回空指针，且不影响另外两个必填端口正常工作。
    // ------------------------------------------------------------
    bool ScenarioKernelPortOptionalStaysNullWhenOmitted()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();
        const QString path = ScratchFilePath(QStringLiteral("KernelOptional"), QStringLiteral("book"));
        QFile::remove(path);

        ks::ui::WorkbenchShared::WorkbenchBackends backends = MakeValidBackends(path);
        backends.kernelPortFactory = nullptr; // 显式留空：模拟"内核范围 + 标准驱动"能力不可用。
        ok = WPJ1_CHECK(ws.Configure(backends), QStringLiteral("kernelPortFactory 留空时 Configure 仍然成功")) && ok;
        ok = WPJ1_CHECK(ws.CreateKernelMutationPort() == nullptr, QStringLiteral("kernelPortFactory 留空时 CreateKernelMutationPort() 如实返回空指针")) && ok;
        ok = WPJ1_CHECK(ws.CreateIoPort() != nullptr, QStringLiteral("kernelPortFactory 留空不影响 CreateIoPort() 正常产出对象")) && ok;
        ok = WPJ1_CHECK(ws.CreateServices() != nullptr, QStringLiteral("kernelPortFactory 留空不影响 CreateServices() 正常产出对象")) && ok;

        return ok;
    }

    // ------------------------------------------------------------
    // 场景 10：WritePolicy() 返回的是同一份、会积累状态的对象，不是每次调用都重新
    // 默认构造的临时值（否则"本次运行不再询问"的记忆会形同虚设）。
    // ------------------------------------------------------------
    bool ScenarioWritePolicyIsStableAcrossCalls()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();

        using ksword::memwb::Channel;
        using ksword::memwb::Scope;
        ok = WPJ1_CHECK(!ws.WritePolicy().IsRemembered(Scope::KernelVirtual, Channel::UserMode),
            QStringLiteral("刚构造时该组合尚未被记忆")) && ok;

        const bool noted = ws.WritePolicy().NoteConfirmed(Scope::KernelVirtual, Channel::UserMode, true);
        ok = WPJ1_CHECK(noted, QStringLiteral("内核范围+用户态通道属于需要首次确认的一类，NoteConfirmed(dontAskAgain=true) 应记入")) && ok;

        // 关键点：换一次调用（返回的是不是同一个底层对象）再查，必须仍然记得刚才记的那一条。
        ok = WPJ1_CHECK(ws.WritePolicy().IsRemembered(Scope::KernelVirtual, Channel::UserMode),
            QStringLiteral("WritePolicy() 返回同一份对象：换一次调用之后仍然记得刚才 NoteConfirmed 的组合")) && ok;

        // 没有被记忆过的另一组合不受影响，顺带验证记忆是按组合区分的，不是"记一次全亮"。
        ok = WPJ1_CHECK(!ws.WritePolicy().IsRemembered(Scope::Physical, Channel::Ddma),
            QStringLiteral("未被 NoteConfirmed 过的另一组合不受影响")) && ok;

        return ok;
    }

    // ------------------------------------------------------------
    // 场景 11：Configure 之后 Int3() 绑定的是真正注入的工厂（而不是未配置时的"拒绝
    // 一切"兜底工厂），并且 Int3() 的身份在多次调用之间保持稳定。
    // ------------------------------------------------------------
    bool ScenarioInt3UsesInjectedFactoryAfterConfigure()
    {
        bool ok = true;
        ks::ui::WorkbenchShared& ws = ks::ui::WorkbenchShared::Instance();
        const QString path = ScratchFilePath(QStringLiteral("Int3Injected"), QStringLiteral("book"));
        QFile::remove(path);

        int int3FactoryCalls = 0;
        ks::ui::WorkbenchShared::WorkbenchBackends backends = MakeValidBackends(path);
        backends.int3Factory = [&int3FactoryCalls](const ksword::memwb::PatchTarget&, ksword::memwb::Channel)
            -> std::unique_ptr<ksword::memwb::IPatchByteStore>
        {
            // 同样返回空指针（本场景只关心"是不是我这份工厂被调用了"，不关心真实的
            // 补丁字节读写——那是 WP-F 的职责），但每次被调用都会让计数器自增，
            // 内置的"拒绝一切"兜底工厂是另一个不会碰这个计数器的 lambda，
            // 两者在可观察行为上完全可以区分。
            ++int3FactoryCalls;
            return nullptr;
        };
        ok = WPJ1_CHECK(ws.Configure(backends), QStringLiteral("注入计数 int3Factory 的 Configure 成功")) && ok;

        ks::ui::Int3Controller& int3First = ws.Int3();
        const ksword::memwb::PatchTarget target{};
        int3First.Install(target, 0x4000ULL, 1ULL);
        ok = WPJ1_CHECK(int3FactoryCalls == 1, QStringLiteral("Configure 之后 Install 调用真的落到了注入的 int3Factory 上（计数变为 1）")) && ok;

        ok = WPJ1_CHECK(&ws.Int3() == &int3First, QStringLiteral("Configure 之后 Int3() 反复调用身份保持不变")) && ok;

        return ok;
    }

    // ------------------------------------------------------------
    // RunScenario：按名字分发到上面某一个场景函数；未识别的名字视为夹具自身的用法
    // 错误（而不是"这个场景不存在就当通过"），避免 wpJ1_main.cpp 里的列表与本文件
    // 的 if/else 链条不同步却悄悄不报错。
    // ------------------------------------------------------------
    bool RunScenario(const QString& name)
    {
        if (name == QStringLiteral("DegradedBeforeConfigure")) return ScenarioDegradedBeforeConfigure();
        if (name == QStringLiteral("ConfigureRejectsInvalidBackendsThenSucceeds")) return ScenarioConfigureRejectsInvalidBackendsThenSucceeds();
        if (name == QStringLiteral("ConfigureOnceOnlyKeepsOriginal")) return ScenarioConfigureOnceOnlyKeepsOriginal();
        if (name == QStringLiteral("RealPathPersistenceAndShutdownIdempotent")) return ScenarioRealPathPersistenceAndShutdownIdempotent();
        if (name == QStringLiteral("ShutdownNeverConfiguredIsSafe")) return ScenarioShutdownNeverConfiguredIsSafe();
        if (name == QStringLiteral("AuditSinkDefaultsToNullWhenFactoryOmitted")) return ScenarioAuditSinkDefaultsToNullWhenFactoryOmitted();
        if (name == QStringLiteral("AuditSinkRealFactoryUsedAndStable")) return ScenarioAuditSinkRealFactoryUsedAndStable();
        if (name == QStringLiteral("PortFactoriesProduceFreshInstancesEachCall")) return ScenarioPortFactoriesProduceFreshInstancesEachCall();
        if (name == QStringLiteral("KernelPortOptionalStaysNullWhenOmitted")) return ScenarioKernelPortOptionalStaysNullWhenOmitted();
        if (name == QStringLiteral("WritePolicyIsStableAcrossCalls")) return ScenarioWritePolicyIsStableAcrossCalls();
        if (name == QStringLiteral("Int3UsesInjectedFactoryAfterConfigure")) return ScenarioInt3UsesInjectedFactoryAfterConfigure();

        // 本文件只收原始 11 个场景；wave3 独立审核的补测场景（Gap/Defect）与本包自己
        // 补的新变异验证场景另起一个文件 wpJ1_scenarios_review.cpp（见该文件顶部注释
        // 里"为什么拆文件"的说明），这里认不出的名字转交给它，它认不出的名字才真正
        // 算作用法错误。
        return RunReviewScenario(name);
    }
}
