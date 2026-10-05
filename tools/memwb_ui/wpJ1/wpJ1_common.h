#pragma once

// ============================================================
// wpJ1_common.h
// 作用：
// - WP-J1（WorkbenchShared 进程级单例）离屏验证夹具的公共设施：轻量断言计数宏、
//   四个最小假实现（FakeServices/FakeIoPort/FakeKernelPort/FakeAuditSink，分别满足
//   IWorkbenchServices/IMemoryIoPort/IKernelMutationPort/ksword::memwb::IAuditSink
//   四个纯虚接口），以及拼装一份"全部字段合法"的 WorkbenchBackends 的小工具函数。
// - 本包的测试架构见 wpJ1_main.cpp 顶部注释："单例跨场景污染"的取舍（每个场景一个
//   独立的操作系统进程，不在同一个进程里反复 Configure）。
// - 不含任何真实 I/O：四个假实现要么直接返回"失败/空"，要么只负责被调用次数计数，
//   供测试断言"注入的工厂真的被用上了，不是退化成内置的兜底实现"。
// ============================================================

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchShared.h"

#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace wpj1_test
{
    // ---- 断言计数与打印：每条 CHECK 都原样打印到 stdout 一行，由外层批处理脚本
    // （build-wpJ1-tests.cmd）在所有场景各自跑完一次独立进程之后统一数行数——本包
    // 的进程内不维护跨场景的全局计数器（本就是刻意让每个场景互相隔离的那一个设计点）。

    // Report：打印一行 "CHECK OK <desc>" 或 "CHECK FAIL <desc> (<file>:<line>)"，
    // 并返回这一条是否通过，供调用方顺手统计"这个场景内部"的通过情况（决定本进程的
    // 退出码，退出码只是旁证，真正的判定仍以 CHECK 行文本为准，与其它工作包的
    // mutrun-j.ps1 判据保持一致）。
    bool Report(bool ok, const QString& desc, const char* file, int line);

#define WPJ1_CHECK(expression, desc) \
    ::wpj1_test::Report(static_cast<bool>(expression), (desc), __FILE__, __LINE__)

    // ScratchDirPath：本次进程独占的临时目录路径（位于 MEMWB_OUT 环境变量指向的目录下，
    // 未设置时退回 ".codex-tmp/memwb-wpJ1"），按场景名+随机序号区分，调用方用于构造
    // AddressBookStore 的真实持久化路径。目录本身由 Qt 的 QSaveFile/QDir 按需创建，
    // 这里只负责拼路径字符串，不提前创建目录（AddressBookStore::writeAtomic 自己会
    // mkpath）。
    QString ScratchFilePath(const QString& scenario, const QString& tag);

    // ---- 四个最小假实现 ----

    // FakeServices：满足 IWorkbenchServices 的四个纯虚方法，全部返回"失败/空"，
    // 不做任何真实枚举/读取——WorkbenchShared 的契约只要求 CreateServices() 把工厂
    // 产出的对象原样转发出去，不关心对象内部行为，所以这里不需要更丰富的假行为。
    class FakeServices final : public ks::ui::IWorkbenchServices
    {
    public:
        ks::ui::ModuleEnumResult enumerateProcessModules(std::uint32_t pid, std::uint64_t expectCreateTime) override;
        ks::ui::ModuleEnumResult enumerateKernelModules() override;
        std::vector<ksword::memwb::ProcessCandidate> processCandidates() override;
        ks::ui::PointerReadResult readPointer(
            const ksword::memwb::MemoryTargetSession& session,
            std::uint64_t address,
            std::uint32_t width) override;
        std::uint64_t ddmaGeneration() override;
    };

    // FakeIoPort：满足 IMemoryIoPort。Read/Write 都报告"通道失败"，本包只关心
    // CreateIoPort() 是否真的把工厂产出的对象转发出来、每次调用是否产出独立实例，
    // 不关心端口本身的读写语义（那是 WorkbenchPageProvider/WorkbenchWriteController
    // 两个包的职责）。
    class FakeIoPort final : public ksword::memwb::IMemoryIoPort
    {
    public:
        ksword::memwb::IoLimits Limits(const ksword::memwb::MemoryTargetSession& session) const override;
        ksword::memwb::IoReadResult Read(
            const ksword::memwb::MemoryTargetSession& session,
            std::uint64_t address,
            std::uint64_t length) override;
        ksword::memwb::IoWriteResult Write(
            const ksword::memwb::MemoryTargetSession& session,
            std::uint64_t address,
            const std::vector<std::uint8_t>& bytes,
            bool approved) override;
    };

    // FakeKernelPort：满足 IKernelMutationPort，五个方法全部报告失败，理由同上。
    class FakeKernelPort final : public ksword::memwb::IKernelMutationPort
    {
    public:
        ksword::memwb::MutationPrepareResult Prepare(
            std::uint64_t address,
            const std::vector<std::uint8_t>& after,
            const std::vector<std::uint8_t>& expectedBefore) override;
        ksword::memwb::MutationStepResult DryRunCommit(std::uint64_t transactionId) override;
        ksword::memwb::MutationStepResult ForceCommit(std::uint64_t transactionId) override;
        ksword::memwb::MutationStepResult Rollback(std::uint64_t transactionId) override;
        ksword::memwb::IoReadResult ReadBack(std::uint64_t address, std::uint64_t length) override;
    };

    // FakeAuditSink：满足 ksword::memwb::IAuditSink。构造时接一个外部计数器指针
    // （生命周期由调用方——场景函数的局部变量——保证，必须比本对象活得久；本对象只在
    // 场景函数返回前被使用，进程随后立即退出，不存在"计数器已经销毁但本对象仍被调用"
    // 的窗口），Record 每次调用就把计数器加一，供场景断言"这确实是我注入的那一份，
    // 不是内置的 NullAuditSink"。
    class FakeAuditSink final : public ksword::memwb::IAuditSink
    {
    public:
        explicit FakeAuditSink(int* recordCount) : m_recordCount(recordCount) {}
        void Record(const ksword::memwb::AuditRecord& record) override;

    private:
        int* m_recordCount = nullptr;
    };

    // MakeCountingFactory：把"每次调用产出一个新假实现、同时让 *callCount 自增"这件
    // 重复出现的逻辑包成一个模板小工具，避免四处手写几乎一样的 lambda。
    // 调用方法：MakeCountingFactory<FakeIoPort, ksword::memwb::IMemoryIoPort>(&counter)
    // 返回一个 std::function<std::unique_ptr<Base>()>，每调用一次 *counter 加一、
    // 产出一个全新的 Derived 实例（地址互不相同，供调用方断言"真的是独立实例"）。
    template <typename Derived, typename Base>
    std::function<std::unique_ptr<Base>()> MakeCountingFactory(int* callCount)
    {
        return [callCount]() -> std::unique_ptr<Base>
        {
            if (callCount != nullptr)
            {
                ++(*callCount);
            }
            return std::make_unique<Derived>();
        };
    }

    // MakeValidBackends：拼装一份"三个必填工厂都非空"的 WorkbenchBackends，供需要
    // "先验证能成功 Configure 一次"的场景复用，避免每个场景重复敲四行 lambda。
    // 传入：addressBookFilePath 持久化路径（空串表示本次仍要求非空字符串以外的真实
    //       场景，调用方按需传）；三个可选计数器指针（nullptr 表示该路工厂不关心调用
    //       次数，仍然会产出假实现，只是不计数）。
    ks::ui::WorkbenchShared::WorkbenchBackends MakeValidBackends(
        QString addressBookFilePath,
        int* servicesCallCount = nullptr,
        int* ioPortCallCount = nullptr,
        int* kernelPortCallCount = nullptr);

    // RunScenario：按名字分发到 wpJ1_scenarios.cpp 里的某一个场景函数并运行它，
    // 认不出的名字会转交给 RunReviewScenario（见下）。
    bool RunScenario(const QString& name);

    // RunReviewScenario：按名字分发到 wpJ1_scenarios_review.cpp 里的场景函数——那个
    // 文件专门收 wave3 独立审核的补测场景（Gap_*/Defect_*）与本包自己补的新变异验证
    // 场景，单独成一个文件是为了不让 wpJ1_scenarios.cpp 超过"单文件 800 行"的仓库
    // 规范（见该文件顶部注释）。调用方法：RunScenario() 认不出名字时转调本函数；
    // 本函数认不出的名字转调下面的 RunR2Scenario()。
    bool RunReviewScenario(const QString& name);

    // RunR2Scenario：按名字分发到 wpJ1_scenarios_r2.cpp——wave3 第二轮独立审核
    // （review2-wpJ1.md）并入本包夹具的补测场景（R2* 开头），调用方法同上；本函数
    // 认不出的名字转调下面的 RunR2bScenario()。
    bool RunR2Scenario(const QString& name);

    // RunR2bScenario：按名字分发到 wpJ1_scenarios_r2b.cpp——本包这一轮（D1 的
    // Configure-重入一半、D3、D4、新增 ConfigureRejectedReason() 诊断访问器）自己
    // 新补的场景，不是审核报告原文列出的缺口。四个场景文件的调度链条
    // （RunScenario → RunReviewScenario → RunR2Scenario → RunR2bScenario）到这里
    // 终止：本函数认不出的名字才真正打印一条 CHECK FAIL 并返回 false。
    bool RunR2bScenario(const QString& name);
}
