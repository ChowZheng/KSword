// wpJ1_common.cpp
// 作用：wpJ1_common.h 声明的四个假实现与小工具函数的实现。详见头文件顶部注释。

#include "wpJ1_common.h"

#include <QDateTime>
#include <QDir>
#include <QProcessEnvironment>

#include <cstdio>

namespace wpj1_test
{
    // Report：把结果与描述打印到 stdout，格式固定为两种之一，供外层批处理脚本用
    // findstr 统计总数与失败数（见 build-wpJ1-tests.cmd 顶部注释）。使用 std::printf
    // 而不是 QTextStream/std::cout 混用，避免中文描述在控制台代码页下被截断或乱码——
    // printf 按 UTF-8 字节原样写出，终端用 chcp 65001 时能正常显示。
    bool Report(const bool ok, const QString& desc, const char* file, int line)
    {
        const QByteArray descUtf8 = desc.toUtf8();
        if (ok)
        {
            std::printf("CHECK OK %s\n", descUtf8.constData());
        }
        else
        {
            std::printf("CHECK FAIL %s (%s:%d)\n", descUtf8.constData(), file, line);
        }
        std::fflush(stdout);
        return ok;
    }

    // ScratchFilePath：按 MEMWB_OUT 环境变量 + "scratch" + 场景名 + tag 拼出一个
    // 本次进程专用的文件路径。不同场景、同一场景内不同 tag 各自落在不同文件名上，
    // 互不覆盖；多次运行之间会覆盖同名文件，这是有意的（每次重新跑都是全新的内容）。
    QString ScratchFilePath(const QString& scenario, const QString& tag)
    {
        QString outDir = QProcessEnvironment::systemEnvironment().value(
            QStringLiteral("MEMWB_OUT"), QStringLiteral(".codex-tmp/memwb-wpJ1"));
        return outDir + QStringLiteral("/scratch/") + scenario + QStringLiteral(".") + tag + QStringLiteral(".addr");
    }

    // ---- FakeServices ----
    // 四个方法全部返回"失败/空"：本包只验证 WorkbenchShared::CreateServices() 把工厂
    // 产出的对象原样转发，不需要假实现表现出任何真实的枚举/读取行为。
    ks::ui::ModuleEnumResult FakeServices::enumerateProcessModules(std::uint32_t, std::uint64_t)
    {
        ks::ui::ModuleEnumResult result;
        result.ok = false;
        result.failure = "fake: not implemented";
        return result;
    }

    ks::ui::ModuleEnumResult FakeServices::enumerateKernelModules()
    {
        ks::ui::ModuleEnumResult result;
        result.ok = false;
        result.failure = "fake: not implemented";
        return result;
    }

    std::vector<ksword::memwb::ProcessCandidate> FakeServices::processCandidates()
    {
        return {};
    }

    ks::ui::PointerReadResult FakeServices::readPointer(
        const ksword::memwb::MemoryTargetSession&, std::uint64_t, std::uint32_t)
    {
        ks::ui::PointerReadResult result;
        result.ok = false;
        return result;
    }

    std::uint64_t FakeServices::ddmaGeneration()
    {
        return 0;
    }

    // ---- FakeIoPort ----
    ksword::memwb::IoLimits FakeIoPort::Limits(const ksword::memwb::MemoryTargetSession&) const
    {
        return ksword::memwb::IoLimits{};
    }

    ksword::memwb::IoReadResult FakeIoPort::Read(
        const ksword::memwb::MemoryTargetSession&, std::uint64_t, std::uint64_t)
    {
        ksword::memwb::IoReadResult result;
        result.status = ksword::memwb::IoReadStatus::Failed;
        result.failure = "fake: not implemented";
        return result;
    }

    ksword::memwb::IoWriteResult FakeIoPort::Write(
        const ksword::memwb::MemoryTargetSession&, std::uint64_t, const std::vector<std::uint8_t>&, bool)
    {
        ksword::memwb::IoWriteResult result;
        result.ok = false;
        result.failure = "fake: not implemented";
        return result;
    }

    // ---- FakeKernelPort ----
    ksword::memwb::MutationPrepareResult FakeKernelPort::Prepare(
        std::uint64_t, const std::vector<std::uint8_t>&, const std::vector<std::uint8_t>&)
    {
        ksword::memwb::MutationPrepareResult result;
        result.ok = false;
        result.failure = "fake: not implemented";
        return result;
    }

    ksword::memwb::MutationStepResult FakeKernelPort::DryRunCommit(std::uint64_t)
    {
        ksword::memwb::MutationStepResult result;
        result.ok = false;
        result.failure = "fake: not implemented";
        return result;
    }

    ksword::memwb::MutationStepResult FakeKernelPort::ForceCommit(std::uint64_t)
    {
        ksword::memwb::MutationStepResult result;
        result.ok = false;
        result.failure = "fake: not implemented";
        return result;
    }

    ksword::memwb::MutationStepResult FakeKernelPort::Rollback(std::uint64_t)
    {
        ksword::memwb::MutationStepResult result;
        result.ok = false;
        result.failure = "fake: not implemented";
        return result;
    }

    ksword::memwb::IoReadResult FakeKernelPort::ReadBack(std::uint64_t, std::uint64_t)
    {
        ksword::memwb::IoReadResult result;
        result.status = ksword::memwb::IoReadStatus::Failed;
        result.failure = "fake: not implemented";
        return result;
    }

    // ---- FakeAuditSink ----
    void FakeAuditSink::Record(const ksword::memwb::AuditRecord& /*record*/)
    {
        if (m_recordCount != nullptr)
        {
            ++(*m_recordCount);
        }
    }

    // MakeValidBackends：三个必填工厂用 MakeCountingFactory 包一份假实现；可选的
    // kernelPortFactory 在这里也默认给一份（部分场景需要"内核端口非空"的路径），
    // 不需要的场景可以在拿到返回值之后自行把 backends.kernelPortFactory 置空
    // （std::function 本身可赋值，调用方可以按需覆盖，不需要本函数穷举每种组合）。
    // auditSinkFactory 本函数不填（默认空，意味着 UsesNullAudit() 会是 true），
    // 需要真实审计接收器的场景自行追加。
    ks::ui::WorkbenchShared::WorkbenchBackends MakeValidBackends(
        QString addressBookFilePath,
        int* servicesCallCount,
        int* ioPortCallCount,
        int* kernelPortCallCount)
    {
        ks::ui::WorkbenchShared::WorkbenchBackends backends;
        backends.addressBookFilePath = std::move(addressBookFilePath);
        backends.int3Factory = [](const ksword::memwb::PatchTarget&, ksword::memwb::Channel)
            -> std::unique_ptr<ksword::memwb::IPatchByteStore>
        {
            // 默认的 int3 工厂同样返回空指针（不关心调用次数）；需要验证"真实工厂被
            // 使用"的场景会自行构造一个带计数的 int3Factory 覆盖这个字段，见
            // Int3UsesInjectedFactoryAfterConfigure 场景。
            return nullptr;
        };
        backends.servicesFactory = MakeCountingFactory<FakeServices, ks::ui::IWorkbenchServices>(servicesCallCount);
        backends.ioPortFactory = MakeCountingFactory<FakeIoPort, ksword::memwb::IMemoryIoPort>(ioPortCallCount);
        backends.kernelPortFactory = MakeCountingFactory<FakeKernelPort, ksword::memwb::IKernelMutationPort>(kernelPortCallCount);
        return backends;
    }
}
