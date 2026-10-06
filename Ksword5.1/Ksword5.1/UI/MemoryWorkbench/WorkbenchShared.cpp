// ============================================================
// WorkbenchShared.cpp
// 作用：
// - 实现 WorkbenchShared.h 声明的进程级单例：三个共享对象（地址簿存储+模型、int3 账本）
//   的惰性创建/配置一次/退出前落盘，以及三个端口工厂、一个审计接收器的持有与转发。
// - 本文件本身不做任何真实的内存读写：那些逻辑全部在装配层（WP-K，MemoryDock 侧）注入
//   的工厂产出的对象内部；本文件只负责"创建一次、记住、按需转发"这一件事。
// - 不包含 Windows.h、不包含 Framework.h，只依赖 Qt Core 与同目录的四个头文件
//   （与 WorkbenchShared.h 本身的依赖面一致）。
//
// 2026-10 wave3 独立审核修复（C1-C6，由主会话裁决后落地）：
//   C1 Configure 不再整份替换已经被惰性访问过的地址簿存储/模型（改为拒绝配置并打印
//      qWarning）；Int3Controller 改成"只构造一次 + 内部转发工厂"，提前访问 Int3()
//      不受这条限制。
//   C2 Shutdown 只在真的有待落盘改动时才 flushNow，不再无条件落盘一遍。
//   C3 Configure 的全部可能失败/抛异常的步骤都在局部变量里完成，最后才用不会抛异常
//      的移动一次性提交，configured_ 放在提交的最后一步。
//   C4 AuditSink() 恒定返回同一个转发器对象，身份不随 Configure 切换。
//   C5 去掉"一次性落盘闩锁"；aboutToQuit 的接线不再只在构造期尝试一次——Configure
//      里也会补一次 HookQuitIfNeeded()，覆盖"Instance() 先于 QCoreApplication 构造"
//      这种边界情况。
//
// 2026-10 wave3 第二轮独立审核修复（D1-D4 + S2，由主会话裁决后落地；复核对象是上面
// C1-C6 的实现本身引入的新缺陷，不是推翻 C1-C6）：
//   D1 Configure() 的 auditSinkFactory 回调（生产里是"打开项目日志"）如果反过来重入
//      访问 AddressBook()/AddressBookTable()，或者重入调用 Configure() 本身，过去
//      会在回调返回后被提交阶段整份替换掉——C1 想杜绝的悬空引用以回调的形式复发。
//      新增 configuring_ 标志：重入的 Configure() 调用在函数开头直接拒绝（零副作用）；
//      重入的地址簿访问没法被拒绝（必须返回有效引用），但会打 qWarning，且
//      Configure() 在审计工厂回调返回后复核一次，发现地址簿已经被重入创建过就整次
//      拒绝、不碰那个对象。新增只读诊断访问器 ConfigureRejectedReason()。
//   D2 load() 挪到全部局部构造（模型、Int3 控制器）完成之后、提交之前的最后一步——
//      避免"模型/控制器构造抛异常时，坏文件已经被 load() 改名走，但这次失败的
//      Configure 什么都没提交，诊断信息跟着局部变量一起被销毁"这种异常安全的死角。
//   D3 HookQuitIfNeeded() 从 Configure() 开头挪到真正要成功提交的那一段——被拒绝的
//      Configure() 调用现在真的是零副作用，不会再顺带把退出钩子接上。
//   D4 记录"接过线没有"的从一个布尔值换成 QPointer<QCoreApplication>，不再在同一
//      进程里重建 QCoreApplication 时静默失效。
//   S2 Configure()/Int3() 两处原来各自内联一份文本相同的 Int3 转发 lambda，抽成私有
//      方法 MakeInt3ForwardingFactory()，只维护一份实现。
// ============================================================

#include "WorkbenchShared.h"

#include <QCoreApplication>
#include <QDebug>

namespace ks::ui
{
    // ------------------------------------------------------------
    // AuditSinkForwarder：AuditSink() 恒定返回的那个对象的真实类型（私有嵌套类，头
    // 文件里只有一句前置声明"class AuditSinkForwarder;"，完整定义必须放在本文件里——
    // std::unique_ptr 的析构代码要在"看得到完整类型"的地方生成，这也是 ~WorkbenchShared()
    // 必须在本文件里定义、不能用头文件内联 "= default" 的原因，见下面析构函数处的
    // 注释）。
    // ------------------------------------------------------------
    class WorkbenchShared::AuditSinkForwarder final : public ksword::memwb::IAuditSink
    {
    public:
        // 构造时绑定 WorkbenchShared::auditSink_ 的常量引用（不是拷贝一份当前值）：
        // auditSink_ 这个成员在 Configure() 执行之前、执行之后都是同一个 unique_ptr
        // 对象，只是它"指向"的内容会从空变成 Configure 注入的真实接收器——引用绑定的
        // 是这个成员本身而不是它此刻的值，所以后续赋值天然可见，不需要任何额外的
        // "重新绑定"步骤。调用方法：构造一次，长期持有，不重新构造。
        explicit AuditSinkForwarder(const std::unique_ptr<ksword::memwb::IAuditSink>& realSink)
            : m_realSink(realSink)
        {
        }

        // Record：转发还是丢弃取决于调用这一刻 m_realSink 是否非空，不是构造那一刻——
        // 这正是"身份恒定、行为随配置切换"的关键。传入：一条审计记录；传出：无，
        // 记录要么被真实接收器吃下，要么原地丢弃（没有第三种结果，也不会崩溃：
        // m_realSink 永远是对一个有效 unique_ptr 成员的引用，哪怕它此刻内容为空）。
        void Record(const ksword::memwb::AuditRecord& record) override
        {
            if (m_realSink)
            {
                m_realSink->Record(record);
            }
        }

    private:
        // m_realSink：常量引用，绑定的是 WorkbenchShared::auditSink_ 这个成员本身，
        // 不是它某一刻的值的拷贝。
        const std::unique_ptr<ksword::memwb::IAuditSink>& m_realSink;
    };

    // ------------------------------------------------------------
    // 构造 / 析构 / Instance / HookQuitIfNeeded
    // ------------------------------------------------------------

    // 构造：详见头文件"单例的创建时机"一节。这一刻 configured_/configuring_ 保持默认的
    // false，hookedApp_ 保持默认的空指针；三个端口工厂、int3Factory_（std::function）
    // 保持默认空；地址簿存储/模型、
    // int3 账本都还没有创建——它们改成惰性创建（见下面 AddressBook()/Int3() 的实现），
    // 因为 AddressBookStore 的持久化路径是"构造时绑定、事后不能改"的（见
    // AddressBookStore.h"filePath()：构造时给定，不可更改"），如果在这里就眼创建一份，
    // Configure() 后面想换成真实的路径时就只能整份重建——与其让"眼创建再重建"分散成
    // 两套逻辑，不如统一成"第一次真正被用到时才创建"。
    // auditForwarder_ 是例外：它不依赖 Configure 的任何输入，提前在这里建好，让
    // AuditSink() 永远可以安全解引用，不需要在访问器里重复判空创建；它绑定的是
    // auditSink_ 这个成员（此刻还是默认构造的空 unique_ptr），按头文件声明顺序
    // auditSink_ 排在 auditForwarder_ 前面，所以构造到这一步 auditSink_ 已经存在，
    // 引用绑定是安全的（C++ 按声明顺序初始化非静态数据成员，与初始化列表里写的顺序
    // 无关）。
    WorkbenchShared::WorkbenchShared()
        : auditForwarder_(std::make_unique<AuditSinkForwarder>(auditSink_))
    {
        // 尝试接上 aboutToQuit；没有 QCoreApplication 时什么都不做，Configure() 真正
        // 要成功提交时会再试一次（见 HookQuitIfNeeded 的说明与头文件"销毁顺序与
        // aboutToQuit 落盘"一节）。
        HookQuitIfNeeded();
    }

    // 析构：必须写在本文件里（不能在头文件里用 "= default"）——AuditSinkForwarder 在
    // 头文件里只是前置声明的不完整类型，std::unique_ptr<AuditSinkForwarder> 析构时要
    // 调用其删除器，删除器需要看到完整的类定义才能生成代码；本文件在这一行之前已经
    // 给出了 AuditSinkForwarder 的完整定义，所以这里可以放心交给编译器生成的默认析构
    // 逻辑。其余 unique_ptr 成员（writePolicy_ 是值成员例外）按头文件声明的逆序自动
    // 销毁，这个顺序的含义见头文件"销毁顺序"一节（"地址簿模型先于地址簿存储析构"
    // 正是由声明顺序决定的）。
    WorkbenchShared::~WorkbenchShared() = default;

    // Instance：惰性创建的进程级单例。C++11 起 static 局部变量的首次初始化本身是语言层面
    // 保证线程安全的（编译器生成的"双检锁"），所以两个线程并发第一次调用 Instance() 不会
    // 导致单例被构造出两份——但这只覆盖"构造那一瞬间"，构造完成之后，本类全部公开方法都
    // 不做任何锁或原子保护，只允许 UI 线程调用（头文件类注释已写明）；这是设计上的使用
    // 限制，不是语言层面的保证，离屏夹具不测这一点（任务书已明确说明这一条只需要注释
    // 说明，不需要验证）。
    WorkbenchShared& WorkbenchShared::Instance()
    {
        static WorkbenchShared instance;
        return instance;
    }

    // HookQuitIfNeeded：见头文件私有成员说明。构造函数尝试一次，Configure() 真正要
    // 成功提交时再尝试一次（2026-10 wave3 第二轮审核修复 D3：后一次调用挪到了
    // Configure() 的提交段落，不再是函数开头第一件事，见下方 Configure() 的实现），
    // 共用这一份逻辑，保证"接线只做一次"这件事只需要维护一处判断。
    void WorkbenchShared::HookQuitIfNeeded()
    {
        QCoreApplication* const app = QCoreApplication::instance();
        if (app == nullptr || hookedApp_ == app)
        {
            // 没有应用对象：什么都不做，调用方此时必须显式调用 Shutdown()（见类注释）。
            // hookedApp_ 已经等于当前这个 app：说明对着它接过线了，直接返回保证幂等，
            // 不会接出第二条连接。两种情况之外（hookedApp_ 为空、app 非空，或者
            // hookedApp_ 指向一个已经销毁的旧 app——QPointer 这时会自动变回空指针，
            // 2026-10 wave3 第二轮审核修复 D4）都会往下走，重新接线。
            return;
        }
        // Qt::DirectConnection：见头文件"销毁顺序与 aboutToQuit 落盘"一节——aboutToQuit
        // 发出的那一刻事件循环即将停止处理排队事件，排队连接的槽函数会被扔进一个从这
        // 一刻起不会再被处理的队列，永远不会真正执行；必须同步直接调用才能确保落盘
        // 真的发生在"退出前"（已用真实 QCoreApplication::exec()+quit() 验证，不是靠
        // 直接 emit 信号模拟出来的结论）。
        QObject::connect(
            app, &QCoreApplication::aboutToQuit,
            this, &WorkbenchShared::Shutdown,
            Qt::DirectConnection);
        hookedApp_ = app;
    }

    // ------------------------------------------------------------
    // Configure / IsConfigured
    // ------------------------------------------------------------

    // IsConfigured：直接返回标志位，不做任何副作用、不触发任何惰性创建。
    bool WorkbenchShared::IsConfigured() const noexcept
    {
        return configured_;
    }

    // ConfigureRejectedReason：直接返回后备存储，不做任何副作用（2026-10 wave3 第二轮
    // 审核修复 D1）。
    WorkbenchShared::ConfigureRejection WorkbenchShared::ConfigureRejectedReason() const noexcept
    {
        return configureRejectedReason_;
    }

    // MakeInt3ForwardingFactory：见头文件私有方法说明（2026-10 wave3 第二轮审核修复
    // S2，取代 Configure()/Int3() 两处过去各自内联的同文本 lambda）。每次被账本调用时
    // 才读取 int3Factory_ 这个成员的当前值，不在构造这一刻拷贝，所以提前创建出来的
    // 控制器在 Configure 把 int3Factory_ 换成真实工厂之后能自动获得真实写入能力。
    Int3Controller::ByteStoreFactory WorkbenchShared::MakeInt3ForwardingFactory()
    {
        return [this](const ksword::memwb::PatchTarget& target, ksword::memwb::Channel channel)
            -> std::unique_ptr<ksword::memwb::IPatchByteStore>
        {
            return int3Factory_ ? int3Factory_(target, channel) : nullptr;
        };
    }

    namespace
    {
        // ConfiguringGuard：RAII 小工具，构造时把目标 bool 置真，析构时无条件复位——
        // 无论作用域是正常 return 离开还是异常传播离开都会执行（2026-10 wave3 第二轮
        // 审核修复 D1）。Configure() 用它保证 configuring_ 这个"审计工厂正在执行"
        // 窗口标志不会因为中途某个 return 分支或者审计工厂抛出的异常而被遗漏复位、
        // 永久卡在 true 上（那样的话这个单例会被永久锁死，之后任何 Configure() 调用
        // 都会被误判成"重入"而拒绝）。只在本文件内部使用，放在匿名命名空间里，不需要
        // 对外暴露。
        class ConfiguringGuard
        {
        public:
            explicit ConfiguringGuard(bool& flag) : m_flag(flag)
            {
                m_flag = true;
            }
            ~ConfiguringGuard()
            {
                m_flag = false;
            }
            // 不需要拷贝/移动：每次调用 Configure() 只会在栈上构造一个，生命周期严格
            // 绑定这一次函数调用。
            ConfiguringGuard(const ConfiguringGuard&) = delete;
            ConfiguringGuard& operator=(const ConfiguringGuard&) = delete;

        private:
            bool& m_flag;
        };
    }

    // Configure：见头文件详细说明，这里只写实现的分支顺序。四个"直接拒绝、不碰任何
    // 状态"的分支必须排在最前面，确保"校验失败"与"校验通过后真正生效"之间没有任何
    // 中间态——调用方据此可以放心地说"返回 false 时什么都没变"，不需要自己再核对一遍。
    bool WorkbenchShared::Configure(WorkbenchBackends backends)
    {
        // 分支零【2026-10 wave3 第二轮审核修复 D1】：重入检测，必须排在最前面、且在
        // HookQuitIfNeeded() 挪到提交段落之后（见下面）不会再有任何副作用。configuring_
        // 只在"审计工厂正在执行"这段窗口内为真——如果这里已经为真，说明是审计工厂
        // 回调（生产里是"打开项目日志"）里又调用了一次 Configure()，必须直接拒绝，
        // 不能让它跑到下面的分支一/二/三甚至提交段落去，否则内层这次调用可能真的
        // 配置成功，外层那次还在栈上的调用紧接着又会把它整份替换掉——重演 C1 想杜绝
        // 的悬空引用，只是换了一个入口。
        if (configuring_)
        {
            qWarning() << "WorkbenchShared::Configure 被拒绝：检测到重入——"
                          "上一层 Configure() 调用（典型触发是 auditSinkFactory 回调里"
                          "又调用了一次 Configure）还没有返回，这一层调用被直接拒绝，"
                          "状态一字节未变。";
            configureRejectedReason_ = ConfigureRejection::ReentrantConfigureCall;
            return false;
        }

        // 分支一：已经成功配置过一次。即使这次传入的 backends 本身完全合法，也必须拒绝
        // ——这正是"防止某个内嵌窗口的构造顺序意外把真实服务重新换成另一份，哪怕内容
        // 一样"的那条要求，不能只检查"新旧内容是否相同"，必须检查"是否已经配置过"。
        if (configured_)
        {
            configureRejectedReason_ = ConfigureRejection::AlreadyConfigured;
            return false;
        }

        // 分支二：三个必填工厂任一为空，配置本身不自洽，同样直接拒绝、不碰任何状态
        // （包含 backends 里其它看起来合法的字段——"全部生效"与"全部不生效"之间不存在
        // 第三种"部分生效"的状态）。kernelPortFactory / auditSinkFactory 允许为空，
        // 不在这条校验里。
        if (!backends.int3Factory || !backends.servicesFactory || !backends.ioPortFactory)
        {
            configureRejectedReason_ = ConfigureRejection::MissingRequiredFactory;
            return false;
        }

        // 分支三【C1】：地址簿存储或模型已经被 Configure 之前的某次惰性访问创建出来
        // 过——这是装配顺序错误（正常顺序下装配层必须在任何人访问这两个对象之前调用
        // 本函数），拒绝配置并用 qWarning 把这件事打出来，而不是像旧版那样整份替换掉
        // 已经被别处持有引用的对象（那样会让对方手里的引用悬空，已实测读到毒化值）。
        // Int3() 不受这条限制——它从始至终只构造一次，见下面"int3 账本"一段的说明。
        // 这条拒绝没有恢复路径：此后任何一次 Configure() 调用都会一直命中这一条分支
        // （见头文件 ConfigureRejectedReason() 的说明），诊断面板应该据此提醒使用者
        // "装配顺序本身错了，不是重试能解决的"。
        if (addressBookStore_ || addressBookModel_)
        {
            qWarning() << "WorkbenchShared::Configure 被拒绝："
                          "AddressBook()/AddressBookTable() 的访问发生在 Configure 之前，"
                          "装配顺序错误（应在创建第一个 MemoryWorkbenchView 之前调用 Configure）。";
            configureRejectedReason_ = ConfigureRejection::AccessedBeforeConfigure;
            return false;
        }

        // ---- 到这里四项校验都已经通过，真正进入"配置中"窗口：下面要调用的审计工厂
        // 如果反过来调用 AddressBook()/AddressBookTable()/Configure()，分支零与下面
        // 的复核会接住（2026-10 wave3 第二轮审核修复 D1）。ConfiguringGuard 构造时把
        // configuring_ 置真，析构时在函数任何出口（正常 return 或异常传播）把它复位，
        // 不会跨调用泄漏——哪怕下面任何一步真的抛出异常也一样；分支零已经保证走到这里
        // 时 configuring_ 必为 false（为真就会在那里被直接拒绝、不会继续往下执行）。
        ConfiguringGuard configuringGuard(configuring_);

        // ---- 下面【C3】把全部可能失败/抛异常的步骤先放在局部变量里完成，最后才用
        // 不会抛异常的移动一次性提交；任何一步真的抛出异常时，单例的任何成员都还没有
        // 被改动过（configured_ 仍为 false，可以用修好的配置重新调用 Configure 重试），
        // 不会留下"部分生效"的半配置状态。

        // 审计接收器：工厂为空就是没有提供（后面退回转发器丢弃记录的默认行为），
        // 工厂本身可能因为"打开项目日志失败"之类的原因抛出异常——这正是本函数要把
        // 它放到最前面局部构建的理由：异常会在任何成员被改动之前就从本函数抛出，
        // Configure 的调用方据此可以用修好的配置立即重试。
        std::unique_ptr<ksword::memwb::IAuditSink> auditLocal =
            backends.auditSinkFactory ? backends.auditSinkFactory() : nullptr;

        // 【D1 复核】审计工厂回调执行期间，如果有代码重入访问过 AddressBook()/
        // AddressBookTable()（分支零已经挡掉了重入 Configure() 本身，能走到这里的
        // 重入只会是这一种），地址簿存储/模型会从空变成非空——这正是"提交前复核此刻
        // 没有任何惰性实例已被取出"的检查点，必须在真正开始构建 storeLocal 之前做，
        // 否则下面提交阶段会把回调里刚创建出来的对象整份替换掉，重演 C1 想杜绝的
        // 悬空引用。一并复核 configured_ 只是防御性的——分支零已经让重入的 Configure()
        // 调用不可能走到提交、不可能把 configured_ 置真，这里留着不会有额外代价。
        if (configured_ || addressBookStore_ || addressBookModel_)
        {
            qWarning() << "WorkbenchShared::Configure 被拒绝：审计工厂回调执行期间，"
                          "AddressBook()/AddressBookTable() 被重入访问，装配顺序错误——"
                          "不碰回调里已经创建出来的对象，这次 Configure 调用整体失败。";
            configureRejectedReason_ = ConfigureRejection::ReentrantAccessDuringAuditFactory;
            return false;
        }

        // 地址簿：AddressBookStore 的持久化路径在构造时就固定（见 AddressBookStore.h
        // "filePath()：构造时给定，不可更改"），这里只会执行一次（上面两次复核已经
        // 保证 addressBookStore_ 此刻必为空），不存在"替换已有对象"的问题。
        std::unique_ptr<AddressBookStore> storeLocal =
            std::make_unique<AddressBookStore>(std::move(backends.addressBookFilePath));
        std::unique_ptr<AddressBookModel> modelLocal =
            std::make_unique<AddressBookModel>(storeLocal.get());

        // int3 账本【C1 的 Int3 部分，S2 抽成了共用的 MakeInt3ForwardingFactory()】：
        // 与地址簿不同，这里不是"只会执行一次"——如果之前有代码提前访问过 Int3()，
        // int3Controller_ 此刻已经非空，不需要（也不应该）重新构造一份新的，否则旧
        // 引用（例如 Int3PatchPanel 持有的裸指针）会悬空。只在 int3Controller_ 为空时
        // 才在局部构造一份。
        std::unique_ptr<Int3Controller> int3Local;
        if (!int3Controller_)
        {
            int3Local = std::make_unique<Int3Controller>(MakeInt3ForwardingFactory());
        }

        // 【D2，2026-10 wave3 第二轮审核修复】load() 放在全部局部构造完成之后、提交
        // 之前的最后一步，不是紧跟着 storeLocal 构造完就做：上面 modelLocal/int3Local
        // 两步理论上仍可能抛出异常（例如 bad_alloc）；旧实现把 load() 放在它们之前，
        // 一旦坏文件已经被 load()->backupCorruptFile() 改名走、而后面这两步才真的抛出
        // 异常，这次失败的 Configure 什么都没提交，storeLocal 随异常传播被销毁，
        // "这份文件坏了"这条诊断信息跟着它一起消失——下一次用同样的路径重试时，坏
        // 文件已经不在原路径上了，新的 load() 调用不会再报告 lastLoadFailed()。现在
        // load() 保证是"万一前面真的抛异常，它根本没有机会执行"的最后一步：真实路径
        // 下 load() 会尝试把已有文件内容加载进来；空路径（纯内存模式）时 load() 本身
        // 就是无副作用的空操作（见 AddressBookStore::load 对空路径的处理）。加载失败
        // （坏文件）时簿维持空白起始状态，lastLoadFailed()/lastLoadErrorText() 由调用方
        // （诊断界面）自行查询——配置是否自洽、与某一份持久化文件内容是否完好无损，
        // 是两件不同粒度的事，Configure 本身不因为后者失败而判定整次配置失败；而如果
        // 这一行之前真的有异常抛出，坏文件会原样留在磁盘上，下一次重试时 load() 第一次
        // 真正运行，诊断信息能如实出现在那次成功提交的 AddressBook() 上。
        storeLocal->load();

        // ---- 提交：以下全部是不会抛异常的移动/赋值，顺序不影响正确性（没有谁依赖谁
        // 先完成）。HookQuitIfNeeded()【D3】放在提交段落的第一步：上面任何一个 return
        // false 都不会执行到这一行，"被拒绝的调用状态一字节未变"现在真的包括这个钩子，
        // 不再是唯一的例外。configured_ 放在最后一行——只要前面任何一步真的抛出了
        // 异常，这一行就永远不会被执行到，IsConfigured() 会如实保持 false（回应可疑点
        // S3：不会在对象还没建好之前就提前读到"已配置"）。
        HookQuitIfNeeded();
        auditSink_ = std::move(auditLocal);
        addressBookStore_ = std::move(storeLocal);
        addressBookModel_ = std::move(modelLocal);
        if (int3Local)
        {
            int3Controller_ = std::move(int3Local);
        }
        int3Factory_ = std::move(backends.int3Factory);
        servicesFactory_ = std::move(backends.servicesFactory);
        ioPortFactory_ = std::move(backends.ioPortFactory);
        kernelPortFactory_ = std::move(backends.kernelPortFactory);
        configured_ = true;
        configureRejectedReason_ = ConfigureRejection::None;

        return true;
    }

    // ------------------------------------------------------------
    // 三个共享对象的访问器（惰性创建）
    // ------------------------------------------------------------

    // AddressBook：惰性创建入口。未 Configure 时传入空路径（纯内存，不落地，见
    // AddressBookStore 对空路径的处理）；已经创建过（无论是被 Configure() 创建的，还是
    // 这里惰性创建的）就直接返回已有的那一份，不会每次调用都重新创建一份新的、丢掉
    // 之前已经写入的任何内容。
    AddressBookStore& WorkbenchShared::AddressBook()
    {
        if (!addressBookStore_)
        {
            // 【D1，2026-10 wave3 第二轮审核修复】configuring_ 为真说明此刻正有一个
            // Configure() 调用卡在它的 auditSinkFactory 回调里，这次访问就是从那个
            // 回调里重入发起的——这个方法的签名不允许返回"失败"，没法在这里真正拒绝，
            // 只能照常把对象惰性创建出来并打一条诊断；真正的拒绝由那个还没返回的
            // Configure() 调用在审计工厂回调结束之后的复核里完成（见 Configure()
            // 实现），它发现地址簿已经非空就会整次失败、绝不会把这里刚创建出来的对象
            // 整份替换掉，所以这次重入访问拿到的引用之后依然有效。
            if (configuring_)
            {
                qWarning() << "WorkbenchShared::AddressBook 在 Configure 的审计工厂回调"
                              "执行期间被重入访问：这次惰性创建出的对象会被保留（不会"
                              "悬空），但触发它的这次 Configure 调用本身会被拒绝。";
            }
            addressBookStore_ = std::make_unique<AddressBookStore>(QString());
            addressBookModel_ = std::make_unique<AddressBookModel>(addressBookStore_.get());
        }
        return *addressBookStore_;
    }

    // AddressBookTable：模型与存储总是成对创建（见上面 AddressBook() 的惰性逻辑），这里
    // 直接借用同一份判断——调用 AddressBook() 只为触发"如果还没创建就创建"这一个副作用，
    // 返回值本身不需要用到。两者无论谁先被外部调用，都能正确地把另一半一起建好，不存在
    // "先调 AddressBookTable() 结果拿到一个空指针解引用"的顺序依赖。
    AddressBookModel& WorkbenchShared::AddressBookTable()
    {
        AddressBook();
        return *addressBookModel_;
    }

    // Int3：惰性创建入口。第一次被调用（无论是 Configure 之前还是之后）就构造出那
    // 唯一一份控制器，绑定的是一个"转发工厂"（MakeInt3ForwardingFactory()，2026-10
    // wave3 第二轮审核修复 S2 抽成的共用私有方法，与 Configure() 里的那一处用的是同一
    // 份实现）：每次账本真正需要字节存储时才去读 int3Factory_ 这个成员的当前值——未
    // Configure 时它是空，效果等价于旧版"拒绝一切"（Install 恒为 WriteFailed，不碰
    // 账本真实状态）；Configure 把它填成真实工厂之后，同一个控制器对象自动切换成
    // "真的会落地"，不需要重新构造、旧引用不会悬空。Int3() 不受 configuring_ 的任何
    // 限制——它从始至终可以在任何时刻被调用，不属于"装配顺序错误"检查的对象。
    Int3Controller& WorkbenchShared::Int3()
    {
        if (!int3Controller_)
        {
            int3Controller_ = std::make_unique<Int3Controller>(MakeInt3ForwardingFactory());
        }
        return *int3Controller_;
    }

    // WritePolicy：值成员，随单例一起构造，这里直接返回引用，不涉及任何惰性创建分支，
    // 也不会因为反复调用而重新构造一份新的（那样会丢掉"本次运行已勾选不再询问"的记忆，
    // 等于让这个功能形同虚设）。
    ksword::memwb::MemoryWritePolicy& WorkbenchShared::WritePolicy()
    {
        return writePolicy_;
    }

    // ------------------------------------------------------------
    // 三个端口工厂的转发，以及审计接收器的访问器
    // ------------------------------------------------------------

    // CreateServices：按 Configure 时注入的工厂创建一份新的 IWorkbenchServices；每次调用
    // 都重新向工厂要一份独立实例（不缓存），这与地址簿/int3 账本"只建一次、长期共用"正好
    // 相反——每个 WorkbenchTarget 需要自己独立的一份服务实现，见头文件 ServicesFactory
    // 的说明。未配置时工厂是空的 std::function，直接判空返回 nullptr。
    std::unique_ptr<IWorkbenchServices> WorkbenchShared::CreateServices() const
    {
        return servicesFactory_ ? servicesFactory_() : nullptr;
    }

    // CreateIoPort：同上的"每次独立一份、不缓存"策略，供 WorkbenchPageProvider 持有自己
    // 专属的一份读写端口。
    std::unique_ptr<ksword::memwb::IMemoryIoPort> WorkbenchShared::CreateIoPort() const
    {
        return ioPortFactory_ ? ioPortFactory_() : nullptr;
    }

    // CreateKernelMutationPort：内核端口工厂本身允许在 Configure 时就是空的（设计上合法，
    // 表示"内核范围 + 标准驱动"这条写入能力在当前环境下不可用）——这里统一按"工厂为空"
    // 处理，不区分"从未 Configure"与"Configure 时就没给这个可选工厂"，两种情况对调用方
    // 的意义完全相同：如实报告这项能力不可用，不自己悄悄换成别的通道。
    std::unique_ptr<ksword::memwb::IKernelMutationPort> WorkbenchShared::CreateKernelMutationPort() const
    {
        return kernelPortFactory_ ? kernelPortFactory_() : nullptr;
    }

    // AuditSink：恒定返回 auditForwarder_（见其类注释）；调用方拿到的始终是同一个对象，
    // 身份不会因为 Configure 把 auditSink_ 从空变成真实接收器而切换。
    ksword::memwb::IAuditSink& WorkbenchShared::AuditSink()
    {
        return *auditForwarder_;
    }

    // UsesNullAudit：auditSink_ 是否为空——为真时 auditForwarder_ 收到的每一条记录都会
    // 被原地丢弃；两者共用同一个判据，不会出现"UsesNullAudit 说没在用空实现，AuditSink()
    // 转发出去的记录却确实被丢弃了"这种自相矛盾的读数。
    bool WorkbenchShared::UsesNullAudit() const noexcept
    {
        return !auditSink_;
    }

    // ------------------------------------------------------------
    // Shutdown
    // ------------------------------------------------------------

    // Shutdown：详见头文件说明。不再有任何一次性闩锁——每次调用都重新判断"此刻是否
    // 真的有待落盘改动"，没有就什么都不做（包括地址簿存储从未被创建过的情况）。这样
    // "Configure 之前先调用过一次 Shutdown()"（例如装配期某处提前调用）不会把之后真正
    // 退出时的落盘变成空操作；"两次 aboutToQuit 之间又有新改动"也总能被下一次调用
    // 重新落盘，不是"第一次之后永远不再生效"。
    void WorkbenchShared::Shutdown()
    {
        if (addressBookStore_ && addressBookStore_->pendingSave())
        {
            addressBookStore_->flushNow();
        }
    }
}
