#pragma once

// ============================================================
// WorkbenchShared.h
// 作用：
// - 内存工作台的"进程级单例"：全部 MemoryWorkbenchView 实例（主 Dock 的那一个，以及
//   每个 ProcessDetailWindow 内嵌的那一个）共享同一份地址簿存储/模型、同一份 int3
//   账本，以及同一份"本次运行不再询问"的确认策略状态。取消了调研阶段"内嵌窗口只读"
//   的设想（见《内存工作台Phase3集成设计.md》§0 第 5 条）：内嵌实例与主实例对这些共享
//   对象拥有同等的读写权限，谁先改谁的改动先生效，不做特殊的只读包装。
// - 本类不碰目标进程/内核内存，不做任何读写内存的 I/O；它只管理"进程内要共享一份"
//   的这几个对象的创建、持有与退出前落盘，真正的内存读写发生在 WorkbenchPageProvider/
//   WorkbenchWriteController/Int3Controller 内部。
// - 单例的创建时机：首次调用 Instance() 时惰性创建（而不是在某个全局对象的构造函数里，
//   避免 Qt 应用对象尚未就位时就去建 QObject）。真正把它变得"有用"要调用一次
//   Configure()——这一步只允许发生一次，由装配层（WP-K，MemoryDock 构造第一个
//   MemoryWorkbenchView 之前）负责，传入地址簿文件路径与若干工厂回调（见下方
//   WorkbenchBackends）。
//
// ------------------------------------------------------------
// 装配顺序错误必须可见（2026-10 wave3 审核修复，C1；2026-10 wave3 第二轮审核修复 D1
// 补了"回调重入"这一个变种）
// ------------------------------------------------------------
// - 正常顺序下装配层必须在创建第一个 MemoryWorkbenchView 之前调用 Configure()，此时
//   谁都还没有访问过 AddressBook()/AddressBookTable()。如果有人违反这个顺序——在
//   Configure() 之前就先调用了 AddressBook() 或 AddressBookTable()（例如 MainWindow
//   惰性创建 MemoryDock 的时机早于装配代码接线）——这里曾经的做法是"整份替换掉"那个
//   提前创建出来的退化实例，后果是对方手里那个引用变成悬空引用（实测毒化堆下读到
//   0xDD，沿旧引用调用 add()/Install() 是访问违规）。现在改为：Configure() 检测到
//   addressBookStore_/addressBookModel_ 已经被惰性创建过，直接返回 false 并通过
//   qWarning 打印"访问发生在 Configure 之前"的诊断信息，不碰任何已有对象——顺序错误
//   从潜伏的内存破坏变成一个调用方立刻能看到的 false 返回值。
// - 这条规则有一个不太直观的触发入口：Configure() 调用方传入的 auditSinkFactory
//   （生产里是"打开项目日志"）在执行期间，如果它内部的代码反过来调用了
//   AddressBook()/AddressBookTable()，或者又调用了一次 Configure()，效果与"提前
//   访问"完全一样——只是"提前"换成了"从里面插队"。Configure() 用 configuring_
//   这个标志挡住这种重入：重入的 Configure() 调用在函数开头就被直接拒绝（零副作用，
//   见下面"configuring_"成员的说明）；重入的 AddressBook()/AddressBookTable()
//   访问没办法在调用那一刻被拒绝（这两个方法必须返回一个有效引用），但会打一条
//   qWarning，而且 Configure() 在审计工厂回调返回后会复核一次"此刻有没有惰性实例
//   被取出过"，一旦发现就把整次 Configure 判定为失败——回调里取得的那个对象因此
//   不会被提交阶段整份替换掉，旧引用依然有效，只是触发它的这次 Configure 调用本身
//   拿不到"配置成功"的结果。
// - 顺序错误（装配层确实先调用了 AddressBook()/AddressBookTable()，不是上面的重入
//   变种）目前没有恢复路径：这一个实例的后续任何 Configure() 调用都会一直被这条
//   分支拒绝，直到进程退出——这是主会话裁决选项 A 的直接后果（只拒绝、不替换），
//   需要"允许事后重新绑定持久化路径"的能力才能让它变得可恢复，那属于
//   AddressBookStore（WP-E）的接口扩展，不在本类范围内。本类为这种"确定性但没有
//   恢复路径"的失败状态提供了一个只读诊断访问器 ConfigureRejectedReason()
//   （IsConfigured() 之外的那一个，见下方公开方法），保证这件事至少是可诊断的——
//   不需要靠一条可能已经滚出日志窗口的 qWarning 去猜"刚才为什么被拒绝"。
// - Int3() 不受这条限制：见下面 Int3Controller 相关成员的说明，它从"遇到就整份替换"
//   改成了"只构造一次、内部转发"，提前访问 Int3() 不会让后续 Configure() 失败。
// ------------------------------------------------------------
// 销毁顺序与 aboutToQuit 落盘
// ------------------------------------------------------------
// - AddressBookStore 自己的析构已经会在仍有未落盘改动时补一次 flushNow()（见
//   AddressBookStore.h 的"C6"），所以本类不需要在析构里额外调用它；但 QTimer 驱动的
//   500ms 防抖要求 QCoreApplication 的事件循环仍然活着，静态析构阶段（main 返回之后）
//   这个前提不一定成立。因此本类会把 Shutdown() 接到 QCoreApplication::aboutToQuit()
//   信号上，确保落盘发生在事件循环仍能处理这最后一批排队事件的"退出前"，而不是赌静态
//   析构顺序。
// - 接线时机（2026-10 wave3 审核修复，C5）：不是只在构造期尝试接一次。如果
//   WorkbenchShared::Instance() 第一次被调用时 QCoreApplication 还不存在（某些离屏
//   夹具，或理论上某处代码异常早地碰了 Instance()），构造期的那一次接线会被跳过；
//   之前的实现到此为止，之后再也不会补接，等于这个实例永远不会被"退出前落盘"覆盖到。
//   现在 Configure() 也会尝试补一次 HookQuitIfNeeded()：只要此刻 QCoreApplication
//   已经存在、而且之前确实还没对着"这同一个" app 接上，就补接一次。没有
//   QCoreApplication 的离屏夹具（两处都探测不到）仍然必须显式调用 Shutdown()。
//   2026-10 wave3 第二轮审核修复两处改动：
//   D3 这一次补接挪到了 Configure() 真正要返回成功之前的"提交"段落里，不再是
//      函数开头第一件事——Configure() 因为任何原因被拒绝（已配置过、必填工厂缺项、
//      装配顺序错误、审计工厂回调重入）时，不会再顺带把钩子接上；"被拒绝的调用
//      状态一字节未变"现在真的包括这个钩子，不再是唯一的例外。
//   D4 记住"接过没有"的不再是一个布尔值（quitHooked_），而是一个指向"已经接上的
//      那个 QCoreApplication"的弱引用（hookedApp_，QPointer<QCoreApplication>）。
//      旧的布尔标志只回答"接过线没有"，如果同一个进程里把旧的 QCoreApplication
//      销毁、重新构造一个新的（本类自己不会这样用，但理论上某个以后的测试夹具或者
//      某处异常的重入场景可能这样做），旧连接随旧 app 一起失效，布尔标志却仍然是
//      true，新 app 退出时再也不会触发落盘——这是一个静默的回归，没有任何报错。
//      QPointer 在它指向的对象被销毁时会自动变回空指针，所以换了新 app 之后
//      hookedApp_ 与当前 app 自然不相等，HookQuitIfNeeded() 会据此重新接线；对着
//      同一个 app 反复调用仍然是幂等的（相等就直接返回，不会接出第二条连接）。
// - 连接方式固定为 Qt::DirectConnection：aboutToQuit 是在事件循环即将停止处理排队事件
//   的那一刻发出的（不是"循环仍在正常运转"），如果用排队连接，Shutdown() 这个槽函数
//   会被扔进一个从这一刻起不会再被处理的队列，永远不会真正执行——这一点已经用真实的
//   QCoreApplication::exec()+quit() 验证过（见 tools/memwb_ui/wpJ1/ 夹具里带
//   "真实 exec" 字样的场景），不是靠直接 emit 信号模拟出来的结论。
// - 内部用 std::unique_ptr 按"地址簿模型先于地址簿存储析构"的顺序声明成员（模型只用
//   QPointer 弱引用存储，顺序本身并非安全必需，但这样声明能让"模型仍在、存储已经
//   被释放"这类中间状态更短，便于复查）；int3 控制器与前两者之间没有依赖，顺序不影响
//   正确性。
// - 本文件不包含 Windows.h、不包含 Framework.h；只依赖 Qt Core/Widgets 与同目录的
//   AddressBookStore/AddressBookModel/Int3Controller/WorkbenchServices 四个头文件。
// ============================================================

#include "AddressBookModel.h"
#include "AddressBookStore.h"
#include "Int3Controller.h"
#include "WorkbenchServices.h"

#include "../../../../shared/evidence/memory_workbench/MemoryIoPort.h"
#include "../../../../shared/evidence/memory_workbench/MemoryWritePolicy.h"
#include "../../../../shared/evidence/memory_workbench/MemoryWriteTransaction.h"

#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>
#include <memory>

// 前置声明：本类只持有 QCoreApplication 的一个弱引用（QPointer，见 hookedApp_ 的
// 说明），不需要在这里包含完整的 <QCoreApplication>——真正解引用、调用
// QCoreApplication::instance()/connect() 的代码都在 .cpp 里，那里已经包含了完整头。
class QCoreApplication;

namespace ks::ui
{
    // WorkbenchShared：进程级单例，详见文件头。全部公开成员只允许在 UI 线程调用。
    class WorkbenchShared final : public QObject
    {
        Q_OBJECT

    public:
        // ServicesFactory：创建一份 IWorkbenchServices 的工厂。每个 MemoryDock 实例的
        // WorkbenchTarget 构造时各自调用一次 CreateServices()，得到自己独立的一份——
        // 服务实现内部如果持有 Dock 的裸指针/句柄，必须一个实例配一个服务对象，不能
        // 跨 Dock 共享（target.md 2.1 的 IWorkbenchServices 本就是"按 Dock 实例注入"）。
        using ServicesFactory = std::function<std::unique_ptr<IWorkbenchServices>()>;
        // IoPortFactory / KernelPortFactory / AuditSinkFactory：读写端口、内核分步事务端口、
        // 审计接收器的工厂。端口工厂每次调用产出一个独立实例（PageProvider 与
        // WriteController 各自持有自己的一份，不共享可变状态）；审计接收器在 Configure 时
        // 只调用一次，之后全进程共用同一份（审计日志是单写者）。
        using IoPortFactory = std::function<std::unique_ptr<ksword::memwb::IMemoryIoPort>()>;
        using KernelPortFactory = std::function<std::unique_ptr<ksword::memwb::IKernelMutationPort>()>;
        using AuditSinkFactory = std::function<std::unique_ptr<ksword::memwb::IAuditSink>()>;

        // WorkbenchBackends：装配层（WP-K，MemoryDock 侧）一次性注入的全部"生产实现"。
        // 本类与装配层六个类都不直接依赖任何驱动/Win32/日志框架头文件，一切重依赖都从
        // 这里的回调进来。字段含义：
        //   addressBookFilePath  地址簿持久化文件路径（空串=仅内存，不持久化，供夹具使用）；
        //   int3Factory          见 Int3Controller::ByteStoreFactory，必须非空；
        //   servicesFactory      每个 Dock 实例一份 IWorkbenchServices，必须非空；
        //   ioPortFactory        必须非空（读写内存的唯一出口，生产实现是 WorkbenchIoPort）；
        //   kernelPortFactory    可空：为空时"内核范围 + 标准驱动"的写入会如实失败并给出
        //                        原因（见 MemoryIoByteStore），不会静默降级；
        //   auditSinkFactory     **生产必须提供**（写入项目日志，不变式"跳过 UI 确认不跳过
        //                        审计"）；为空时退回内置空接收器，仅供夹具使用，并且
        //                        UsesNullAudit() 返回 true，便于装配层/测试断言生产路径
        //                        没有漏接。
        struct WorkbenchBackends
        {
            QString addressBookFilePath;
            Int3Controller::ByteStoreFactory int3Factory;
            ServicesFactory servicesFactory;
            IoPortFactory ioPortFactory;
            KernelPortFactory kernelPortFactory;
            AuditSinkFactory auditSinkFactory;
        };

        // Instance：取得进程内唯一实例；首次调用时创建。未 Configure 之前，AddressBook()/
        // Int3()/CreateServices() 等访问器仍然可以调用，但分别退化为"仅内存、不持久化"的
        // AddressBookStore 与"工厂永远返回空指针"的 Int3Controller（不会崩溃，只是什么都
        // 不落地）；IsConfigured() 为 false 可据此提醒装配层尚未接线。
        static WorkbenchShared& Instance();

        // Configure：装配层（WP-K）在创建第一个 MemoryWorkbenchView 之前调用一次。
        // 传入：backends 见 WorkbenchBackends；int3Factory/servicesFactory/ioPortFactory
        //       任一为空视为配置无效（返回 false 且不改变任何状态）。
        // 传出：true 表示本次调用真正生效；false 表示下列任一情况——
        //       (a) 配置本身不自洽（必填工厂缺项）；
        //       (b) 已经 Configure 过一次，本次调用被忽略；
        //       (c) 【见"装配顺序错误必须可见"一节】Configure 之前已经有代码惰性访问过
        //           AddressBook()/AddressBookTable()，装配顺序本身就是错的；
        //       (d) 【2026-10 wave3 第二轮审核修复 D1 新增】重入——本次调用是从
        //           auditSinkFactory 回调内部又发起的一次 Configure()，或者
        //           auditSinkFactory 回调内部重入访问了 AddressBook()/
        //           AddressBookTable()（回调返回后才能发现，见同一节说明）。
        //       四种情况下所有字段均不生效，已有对象的身份与内容都不受影响，钩子状态
        //       （见下方 Shutdown 的说明）也不受影响——"被拒绝"在本类里统一等于"零
        //       副作用"；(c)/(d) 还会打印一条 qWarning 诊断信息。Int3()/AuditSink() 的
        //       提前访问不属于以上任何一种拒绝理由，不会让 Configure 失败。被拒绝的
        //       具体原因可以用 ConfigureRejectedReason() 读到（IsConfigured() 只回答
        //       "成功过没有"，不区分这四种失败）。
        bool Configure(WorkbenchBackends backends);

        // IsConfigured：是否已经成功 Configure 过一次。
        bool IsConfigured() const noexcept;

        // ConfigureRejection：ConfigureRejectedReason() 的返回类型，列出 Configure()
        // 的全部拒绝理由（2026-10 wave3 第二轮审核修复 D1 新增，详见"装配顺序错误
        // 必须可见"一节；本枚举与诊断访问器是任务书点名允许本包新增的头文件改动）。
        enum class ConfigureRejection
        {
            None,                              // 还没被拒绝过，或者最近一次 Configure 成功。
            AlreadyConfigured,                 // 对应上面 Configure() 传出说明的 (b)。
            MissingRequiredFactory,            // 对应 (a)：三个必填工厂任一为空。
            AccessedBeforeConfigure,           // 对应 (c)：装配顺序错误，没有恢复路径。
            ReentrantConfigureCall,            // 对应 (d) 的前一半：审计工厂回调里重入调用了 Configure()。
            ReentrantAccessDuringAuditFactory, // 对应 (d) 的后一半：审计工厂回调里重入访问了地址簿。
        };

        // ConfigureRejectedReason：IsConfigured() 之外的只读诊断访问器，回答"最近
        // 一次 Configure() 调用具体是哪一种处置结果"。成功过的话是 None；被拒绝的话
        // 是上面枚举里对应的那一项，直到某次 Configure() 成功才会被复位回 None（本类
        // 的 Configure() 一生只能成功一次，所以实际上"被复位"只会发生在那一次成功
        // 调用上；(c) 顺序错误这条分支没有恢复路径，意味着一旦命中它，这个访问器会
        // 一直稳定地返回 AccessedBeforeConfigure，直到进程退出——这正是它存在的意义：
        // 哪怕状态没办法恢复，至少"为什么一直失败"这件事本身要能被诊断出来，不用靠
        // 翻一条可能已经滚出日志窗口的 qWarning 去猜）。不触发任何副作用，只读取一个
        // 枚举成员，可以随时调用。
        ConfigureRejection ConfigureRejectedReason() const noexcept;

        // AddressBook / AddressBookTable / Int3：三个共享对象的引用访问器。生命周期
        // 与本单例一致；调用方不得缓存裸指针跨越"主程序退出"这件事之外的任何场景——
        // 单例本身不会在运行期中途被销毁或重建（Configure() 一旦检测到有人在它之前就
        // 惰性创建过地址簿存储/模型，会直接拒绝配置而不是悄悄替换，见上方说明；Int3()
        // 则是从始至终只构造一次，Configure 只是换一个内部转发目标）。
        // 2026-10 wave3 第二轮审核修复 D1：如果 AddressBook()/AddressBookTable() 是在
        // 某次 Configure() 调用的 auditSinkFactory 回调里被重入调用的（即此刻还有一个
        // Configure() 调用正挂在调用栈上没有返回），这里会打一条 qWarning 诊断信息，
        // 但仍然会按原来的惰性创建逻辑返回一个有效对象（本方法的签名不允许返回"失败"）
        // ——真正的拒绝由那个还没返回的 Configure() 调用在审计工厂回调结束之后的复核
        // 里完成，见"装配顺序错误必须可见"一节。
        AddressBookStore& AddressBook();
        AddressBookModel& AddressBookTable();
        Int3Controller& Int3();

        // WritePolicy：确认策略的"本次运行"状态（MemoryWritePolicy，见
        // shared/evidence/memory_workbench/MemoryWritePolicy.h）。它不是持久化设置，
        // 只在本次进程运行期间记忆"用户在哪些范围+通道组合上勾选过不再询问"；进程
        // 重启后自动清空（对象随单例一起重新构造）。装配层把它传给
        // WorkbenchConfirmations 的构造参数，所有视图共用同一份记忆。
        ksword::memwb::MemoryWritePolicy& WritePolicy();

        // CreateServices：按 Configure 时注入的工厂创建一份新的 IWorkbenchServices。
        // 传出：工厂产出的对象；未 Configure 时返回空指针，调用方（WorkbenchTarget 的
        //       创建者）应自行判断并使用假实现兜底，不能假设永远非空。
        std::unique_ptr<IWorkbenchServices> CreateServices() const;

        // CreateIoPort / CreateKernelMutationPort：按 Configure 注入的工厂各创建一个新的
        // 端口实例，供 WorkbenchPageProvider 与 WorkbenchWriteController 各持一份。
        // 传出：未 Configure（或内核端口工厂为空）时返回空指针；调用方必须判空并把这种
        //       情况当作"该能力不可用"如实报告，不得静默降级到别的通道。
        std::unique_ptr<ksword::memwb::IMemoryIoPort> CreateIoPort() const;
        std::unique_ptr<ksword::memwb::IKernelMutationPort> CreateKernelMutationPort() const;

        // AuditSink：全进程共用的审计接收器引用，生命周期与单例一致，永不为空，
        // **身份在整个单例生命周期内恒定**（内部永远是同一个转发器对象，见
        // AuditSinkForwarder 的说明）：无论生产审计接收器是在取得这个引用之前还是之后
        // 才注入，这个引用上调用 Record() 的最终去向都会跟着切换，不会出现"早取的引用
        // 永久指向内置空实现"的静默丢审计。WriteController 把它交给
        // MemoryWriteTransaction 与撤销协调器；两者都只在 UI 线程调用。
        // UsesNullAudit：当前是否仍没有注入生产审计接收器（生产装配完成后必须为 false）。
        ksword::memwb::IAuditSink& AuditSink();
        bool UsesNullAudit() const noexcept;

        // Shutdown：若地址簿存储存在且确实还有未落盘的改动（pendingSave() 为真），
        // 立即补一次 flushNow()；没有待落盘改动（包括从未创建过地址簿存储）时什么都
        // 不做。可以反复调用：每次都按"此刻是否真的有待落盘改动"重新判断，不是"第一次
        // 之后就永远空操作"的一次性闩锁——闩锁会让"Configure 之前先调用过一次
        // Shutdown()"这种边界情况意外关闭掉之后真正退出时的落盘（2026-10 wave3 审核
        // 修复，C2/C5）。QCoreApplication::aboutToQuit 存在时会被自动接到这里，接线
        // 时机见上方"销毁顺序与 aboutToQuit 落盘"一节。
        void Shutdown();

    private:
        // 构造私有：只能通过 Instance() 取得；见 HookQuitIfNeeded() 的说明。
        WorkbenchShared();
        ~WorkbenchShared() override;

        // 单例禁止拷贝/移动。
        WorkbenchShared(const WorkbenchShared&) = delete;
        WorkbenchShared& operator=(const WorkbenchShared&) = delete;

        // HookQuitIfNeeded：把 Shutdown() 接到 QCoreApplication::aboutToQuit 上，前提是
        // 此刻确实存在 QCoreApplication 且之前还没有对着"这同一个" app 接过
        // （hookedApp_ 与当前 app 不相等）。调用方法：构造函数里尝试一次，Configure()
        // 真正要成功提交时再尝试一次（2026-10 wave3 第二轮审核修复 D3：这次调用挪到了
        // 提交段落，Configure() 被拒绝的任何一条分支都不会再调用它，见 .cpp 里
        // Configure() 实现的注释）；两处都是"尽力而为"——没有 QCoreApplication 时
        // 什么都不做，之后也不会自动重试（调用方此时必须像类注释写的那样显式调用
        // Shutdown()）。幂等：对着同一个 app 接过一次之后反复调用不会产生第二条连接；
        // 换了一个新的 QCoreApplication 之后会重新接线（2026-10 wave3 第二轮审核修复
        // D4，见 hookedApp_ 的说明）。
        void HookQuitIfNeeded();

        // MakeInt3ForwardingFactory：构造绑定 this 的"转发工厂" lambda，供 Configure()
        // 与 Int3() 两处惰性创建 Int3Controller 时共用（2026-10 wave3 第二轮审核修复
        // S2：这两处过去各自内联一份文本相同的 lambda，审核报告指出"两份代码能各自
        // 独立变坏而原夹具都看不出"，现在抽成这一个私有方法，只维护一份实现）。
        // 传出：每次被账本调用时都读取 int3Factory_ 这个成员的当前值——为空就等价于
        // "拒绝一切"的退化工厂，非空就转发给 Configure 注入的真实工厂；不在构造这一刻
        // 把 int3Factory_ 的内容拷贝进闭包，所以提前创建出来的控制器在 Configure 把
        // int3Factory_ 换成真实工厂之后会自动获得真实写入能力，不需要重新绑定。
        Int3Controller::ByteStoreFactory MakeInt3ForwardingFactory();

        // configured_：Configure() 是否已经成功调用过一次。
        bool configured_ = false;
        // configuring_：此刻是否正处于某次 Configure() 调用的"审计工厂正在执行"这段
        // 窗口内（2026-10 wave3 第二轮审核修复 D1 新增）。只在 Configure() 内部短暂
        // 置真，函数的全部出口（正常 return 或异常传播）都通过一个定义在 .cpp 里的
        // 函数局部 RAII 守卫复位，不会跨调用泄漏。用途：Configure() 开头检测到
        // configuring_ 已经为真，说明是审计工厂回调里又调用了一次 Configure()，直接
        // 拒绝（零副作用）；AddressBook()/AddressBookTable() 在 configuring_ 为真时
        // 被调用，说明是审计工厂回调里重入访问了地址簿，打一条 qWarning 诊断。
        bool configuring_ = false;
        // configureRejectedReason_：ConfigureRejectedReason() 的后备存储，默认 None
        // （构造时还没有调用过 Configure()，语义上等同于"最近一次成功"，不提示任何
        // 拒绝原因）。
        ConfigureRejection configureRejectedReason_ = ConfigureRejection::None;
        // hookedApp_：已经成功接上 aboutToQuit 的那个 QCoreApplication，弱引用、不持有
        // 所有权（2026-10 wave3 第二轮审核修复 D4，取代旧版的一次性布尔标志
        // quitHooked_——旧标志只回答"接过线没有"，同一进程里销毁旧的 QCoreApplication
        // 再构造一个新的时，旧连接随旧 app 一起失效，但旧标志仍是 true，新 app 退出时
        // 再也不会触发落盘，这是一个没有任何报错的静默回归）。QPointer 在它指向的对象
        // 被销毁时自动变回空指针，所以换了新 app 之后 hookedApp_ 与当前 app 自然不
        // 相等，HookQuitIfNeeded() 会据此重新接线；对着同一个 app 反复调用仍然幂等。
        QPointer<QCoreApplication> hookedApp_;
        // servicesFactory_ / ioPortFactory_ / kernelPortFactory_：Configure() 注入的工厂；
        // 未配置时为空 std::function。
        ServicesFactory servicesFactory_;
        IoPortFactory ioPortFactory_;
        KernelPortFactory kernelPortFactory_;
        // int3Factory_：Configure() 注入的真实 int3 字节存储工厂。Int3Controller 本身只在
        // 第一次被用到时构造一次（见 Int3()），构造时绑定的是一个"转发工厂"：每次被账本
        // 调用时都读取这个成员的当前值，为空就等价于旧版"拒绝一切"的退化工厂，非空就
        // 转发给 Configure 注入的真实工厂——这样提前访问 Int3() 不需要事后整份替换
        // 控制器对象，旧引用永远有效。
        Int3Controller::ByteStoreFactory int3Factory_;
        // auditSink_：Configure 时由 auditSinkFactory 产出的审计接收器（单例持有）；
        // 为空时转发器 auditForwarder_ 内部的 Record() 调用被原地丢弃。
        std::unique_ptr<ksword::memwb::IAuditSink> auditSink_;
        // AuditSinkForwarder：AuditSink() 恒定返回的那个对象的真实类型，私有嵌套类，
        // 完整定义见 .cpp（原因见 .cpp 里析构函数处的注释）。它本身不是"空实现"，而是
        // 一个"转发器"：持有 auditSink_ 的常量引用，Record() 被调用时才去看 auditSink_
        // 这一刻是否非空，非空就转发、为空就丢弃——这保证了 AuditSink() 的返回对象
        // 身份从单例构造那一刻起就固定不变，不会因为 Configure 把 auditSink_ 从空指针
        // 换成真实对象而发生"身份切换"。
        class AuditSinkForwarder;
        std::unique_ptr<AuditSinkForwarder> auditForwarder_;
        // addressBookStore_ / addressBookModel_：共享地址簿的存储与表格模型。声明顺序
        // 见文件头"销毁顺序"一节。两者只会在进程生命周期里被创建一次（无论是被
        // Configure() 创建，还是在它之前被某次惰性访问创建）：一旦非空，Configure()
        // 只会原样保留或者直接拒绝，绝不会重建替换（见"装配顺序错误必须可见"一节）。
        std::unique_ptr<AddressBookStore> addressBookStore_;
        std::unique_ptr<AddressBookModel> addressBookModel_;
        // int3Controller_：共享 int3 补丁账本的 Qt 装配层，全进程只构造一次（见
        // int3Factory_ 的说明），Configure() 遇到它已经存在时不会重新创建。
        std::unique_ptr<Int3Controller> int3Controller_;
        // writePolicy_：确认策略的"本次运行"状态，值成员（随单例一起构造/销毁）。
        ksword::memwb::MemoryWritePolicy writePolicy_;
    };
}
