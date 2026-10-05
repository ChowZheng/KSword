#include "../Framework.h"

#include "MemoryDock.WorkbenchServices.h"
#include "MemoryDock.WorkbenchServices.Internal.h"
#include "WorkbenchIoPorts.h"
#include "WorkbenchServicesMapping.h"
#include "../SettingsDock/AppearanceSettings.h"
#include "../UI/MemoryWorkbench/WorkbenchShared.h"
#include "../../../shared/evidence/memory_workbench/MemoryPatchByteStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QString>

#include <memory>
#include <string>
#include <utility>
#include <vector>

// ============================================================
// MemoryDock.WorkbenchServices.cpp
// 作用：
// - ConfigureShared：构造 WorkbenchShared::WorkbenchBackends 并调用一次
//   WorkbenchShared::Instance().Configure(...)，把"生产实现"全部注入装配层：
//     addressBookFilePath -> 用户配置目录下的 memory_workbench_address_book.txt（目录不存在则创建）
//     int3Factory         -> MemoryPatchByteStore + 真实端口 WorkbenchIoPort（OwnedPatchByteStore）
//     servicesFactory     -> 生产 IWorkbenchServices（MemoryDock.WorkbenchServices.Impl.cpp）
//     ioPortFactory       -> WorkbenchIoPort（真实读写端口）
//     kernelPortFactory   -> WorkbenchKernelMutationPort（内核分步字节事务端口）
//     auditSinkFactory    -> 写项目日志的审计接收器（MemoryDock.WorkbenchServices.Audit.cpp）
// - GlobalSkipDangerousConfirm：读取既有的"跳过危险操作重复确认"设置项。
//
// 本文件只做装配，不含读写策略；所有读写策略（分块/重试/回滚/确认）在 shared/evidence/
// memory_workbench/ 的 Qt-free 逻辑层里，真实端口只"问一次系统"。R3 访问 KswordARK 驱动
// 统一经 ArkDriverClient（WorkbenchIoPorts 内部），本文件不含 DeviceIoControl。
// ============================================================

namespace svc = ksword::memwb_services_detail;

namespace
{
    // kAddressBookFileName：地址簿持久化文件名（固定，与设计文档一致）。
    constexpr const char* kAddressBookFileName = "memory_workbench_address_book.txt";

    // kLocalDataSubDirectory：回退目录（AppLocalDataLocation）下存放工作台数据的子目录名。
    constexpr const char* kLocalDataSubDirectory = "memory_workbench";

    // EnsureWritableDirectory：确保目录存在且可写。
    // 为什么要写探测：Qt 在 Windows 上的 QFileInfo::isWritable 默认只看只读属性，不查 ACL，
    //   对 Program Files 这类目录会误报"可写"，所以真的创建并删除一个探测文件。
    // 传入：directory 目录路径（不存在则创建，含上级目录）。
    // 传出：true 表示目录已存在（或已创建）且能创建文件；false 表示不可用。
    bool EnsureWritableDirectory(const QString& directory)
    {
        if (directory.isEmpty())
        {
            return false;
        }
        QDir dir;
        if (!dir.mkpath(directory))
        {
            return false;
        }

        // 探测文件名带进程号，避免多个实例同时启动时互相删对方的探测文件。
        const QString probePath = QDir(directory).filePath(
            QStringLiteral(".memwb_write_probe_%1").arg(QCoreApplication::applicationPid()));
        QFile probeFile(probePath);
        if (!probeFile.open(QIODevice::WriteOnly))
        {
            return false;
        }
        probeFile.close();
        probeFile.remove();
        return true;
    }

    // ResolveAddressBookFilePath：解析地址簿持久化文件的完整路径。
    // 目录约定（按优先级）：
    //   1) 与外观设置文件 appearance_settings.json 同目录（客户配置固定落在 exe 同级的
    //      style 目录，见 ks::settings::resolveSettingsJsonPathForWrite）——复用既有的
    //      "用户配置目录"约定，不另立一处；
    //   2) QStandardPaths::AppLocalDataLocation/memory_workbench（exe 目录不可写，例如装在
    //      Program Files 时的回退，MonitorDock 等模块也用这个位置）。
    // 目录不存在时创建。
    // 传出：文件完整路径；两个目录都不可用时返回空串（地址簿退化为仅内存，ConfigureShared
    //       会写一条警告日志，不静默）。
    QString ResolveAddressBookFilePath()
    {
        std::vector<std::string> candidates;

        const QString settingsJsonPath = ks::settings::resolveSettingsJsonPathForWrite();
        if (!settingsJsonPath.isEmpty())
        {
            candidates.push_back(QFileInfo(settingsJsonPath).absolutePath().toStdString());
        }

        const QString localDataPath = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        if (!localDataPath.isEmpty())
        {
            candidates.push_back(
                QDir(localDataPath).filePath(QString::fromLatin1(kLocalDataSubDirectory)).toStdString());
        }

        const std::string chosenDirectory = svc::ChooseWritableDirectory(
            candidates,
            [](const std::string& directory) {
                return EnsureWritableDirectory(QString::fromStdString(directory));
            });
        if (chosenDirectory.empty())
        {
            return QString();
        }
        return QString::fromStdString(svc::JoinPath(chosenDirectory, kAddressBookFileName));
    }

    // OwnedPatchByteStore：把"端口 + 会话 + MemoryPatchByteStore"打包成一个自包含对象。
    // 为什么需要：MemoryPatchByteStore 只持有端口与会话的**引用**，引用必须比它活得久；
    //   Int3Controller 的工厂返回的是 unique_ptr<IPatchByteStore>，对象的生命周期完全交给账本，
    //   所以端口与会话必须作为成员跟着它一起创建、一起销毁。
    class OwnedPatchByteStore final : public ksword::memwb::IPatchByteStore
    {
    public:
        // 构造：session 为这个补丁目标构造好的会话（BuildPatchSession 的产物）。
        explicit OwnedPatchByteStore(const ksword::memwb::MemoryTargetSession& session)
            : session_(session)
            , store_(port_, session_)
        {
        }

        // ReadByte / WriteByte：全部转发给 MemoryPatchByteStore，规则见它的头文件
        //（范围/通道检查、approved 固定为 false、needsApproval 当作失败）。
        bool ReadByte(const std::uint64_t address, std::uint8_t& valueOut) override
        {
            return store_.ReadByte(address, valueOut);
        }

        bool WriteByte(const std::uint64_t address, const std::uint8_t value) override
        {
            return store_.WriteByte(address, value);
        }

    private:
        // 成员声明顺序即初始化顺序：先端口，再会话，最后引用前两者的补丁存储。
        // port_：真实读写端口，无成员状态。
        ksword::memwb_ports::WorkbenchIoPort port_;
        // session_：补丁目标的会话（进程范围）。
        ksword::memwb::MemoryTargetSession session_;
        // store_：补丁字节存储，引用 port_ 与 session_。
        ksword::memwb::MemoryPatchByteStore store_;
    };

    // CreateInt3ByteStore：Int3Controller::ByteStoreFactory 的实现体。
    // 传入：target 补丁目标身份；channel 必须使用的通道（安装取当前通道，还原取安装通道）。
    // 传出：绑定好的字节存储；通道不被允许（磁盘传输）或目标无效（pid 为 0）时返回空指针——
    //       账本不会被调用，调用方按失败处理，**不会自动换成别的通道**。
    // R0 要求显式同意（needsApproval）时：MemoryPatchByteStore 把它当作普通失败，这里
    //       不自动强制，失败如实返回给账本。
    std::unique_ptr<ksword::memwb::IPatchByteStore> CreateInt3ByteStore(
        const ksword::memwb::PatchTarget& target,
        const ksword::memwb::Channel channel)
    {
        if (!svc::IsPatchChannelAllowed(channel))
        {
            kLogEvent rejectEvent;
            warn << rejectEvent
                << "[MemoryWorkbench] int3 byte store rejected: channel="
                << static_cast<std::uint32_t>(channel)
                << " is not allowed for int3 patches"
                << eol;
            return nullptr;
        }
        if (target.pid == 0U)
        {
            kLogEvent rejectEvent;
            warn << rejectEvent
                << "[MemoryWorkbench] int3 byte store rejected: target pid is 0"
                << eol;
            return nullptr;
        }
        return std::make_unique<OwnedPatchByteStore>(svc::BuildPatchSession(target, channel));
    }

    // RejectionName：WorkbenchShared::ConfigureRejection -> 日志用的稳定英文名。
    const char* RejectionName(const ks::ui::WorkbenchShared::ConfigureRejection reason)
    {
        using Rejection = ks::ui::WorkbenchShared::ConfigureRejection;
        switch (reason)
        {
        case Rejection::None:
            return "None";
        case Rejection::AlreadyConfigured:
            return "AlreadyConfigured";
        case Rejection::MissingRequiredFactory:
            return "MissingRequiredFactory";
        case Rejection::AccessedBeforeConfigure:
            return "AccessedBeforeConfigure";
        case Rejection::ReentrantConfigureCall:
            return "ReentrantConfigureCall";
        case Rejection::ReentrantAccessDuringAuditFactory:
            return "ReentrantAccessDuringAuditFactory";
        }
        return "Unknown";
    }
}

namespace ks::ui::workbench_dock
{
    void ConfigureShared()
    {
        // 只尝试一次：UI 线程调用，没有并发。无论第一次成败，之后的调用都立即返回——
        // 顺序错误（AccessedBeforeConfigure）没有恢复路径，重复调用只会重复刷同一条日志。
        static bool attempted = false;
        if (attempted)
        {
            return;
        }
        attempted = true;

        // 本次装配过程的全部日志共用同一个事件，便于按调用链追踪。
        kLogEvent configureEvent;

        // 已经被别处配置过（例如夹具）：什么都不做。
        if (WorkbenchShared::Instance().IsConfigured())
        {
            info << configureEvent
                << "[MemoryWorkbench] ConfigureShared: WorkbenchShared is already configured, skipped"
                << eol;
            return;
        }

        WorkbenchShared::WorkbenchBackends backends;
        backends.addressBookFilePath = ResolveAddressBookFilePath();
        backends.int3Factory = [](const ksword::memwb::PatchTarget& target, const ksword::memwb::Channel channel) {
            return CreateInt3ByteStore(target, channel);
        };
        backends.servicesFactory = []() -> std::unique_ptr<IWorkbenchServices> {
            return detail::CreateProductionServices();
        };
        backends.ioPortFactory = []() -> std::unique_ptr<ksword::memwb::IMemoryIoPort> {
            return std::make_unique<ksword::memwb_ports::WorkbenchIoPort>();
        };
        backends.kernelPortFactory = []() -> std::unique_ptr<ksword::memwb::IKernelMutationPort> {
            return std::make_unique<ksword::memwb_ports::WorkbenchKernelMutationPort>();
        };
        backends.auditSinkFactory = []() -> std::unique_ptr<ksword::memwb::IAuditSink> {
            return detail::CreateAuditLogSink();
        };

        // 地址簿路径为空 = 两个候选目录都不可写：地址簿退化为仅内存，必须留痕。
        const bool persistent = !backends.addressBookFilePath.isEmpty();
        const QString addressBookPath = backends.addressBookFilePath;

        const bool configured = WorkbenchShared::Instance().Configure(std::move(backends));
        if (!configured)
        {
            err << configureEvent
                << "[MemoryWorkbench] ConfigureShared: Configure was rejected, reason="
                << RejectionName(WorkbenchShared::Instance().ConfigureRejectedReason())
                << eol;
            return;
        }

        info << configureEvent
            << "[MemoryWorkbench] ConfigureShared: configured, addressBook="
            << (persistent ? addressBookPath : QStringLiteral("(memory only)"))
            << eol;
        if (!persistent)
        {
            warn << configureEvent
                << "[MemoryWorkbench] ConfigureShared: no writable directory for the address book, "
                << "address book entries will not be saved"
                << eol;
        }
    }

    bool GlobalSkipDangerousConfirm()
    {
        // 复用既有设置项（外观设置里的 suppress_dangerous_action_confirmations），不新增键名；
        // 不做缓存：用户刚在设置里关掉开关后，下一次写入必须立刻重新确认。
        return ks::settings::dangerousActionConfirmationsSuppressed();
    }
}
