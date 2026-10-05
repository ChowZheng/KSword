#include "../Framework.h"

#include "MemoryDock.WorkbenchServices.h"
#include "MemoryDock.WorkbenchServices.Internal.h"
#include "MemoryAccessBackend.h"
#include "WorkbenchIoPorts.h"
#include "WorkbenchServicesMapping.h"
#include "WorkbenchServicesProbe.h"
#include "../KernelDock/KernelThreadAuditTab.h"

#include <QRunnable>
#include <QString>
#include <QThreadPool>

#include <chrono>
#include <exception>
#include <mutex>
#include <utility>
#include <vector>

// ============================================================
// MemoryDock.WorkbenchServices.Impl.cpp
// 作用：
// - 内存工作台生产 IWorkbenchServices（DockWorkbenchServices）：模块枚举、内核模块枚举、
//   候选进程、指针读取、DDMA 代次五个方法，全部复用既有能力，不另造读写后端：
//     enumerateProcessModules  -> ks::process::EnumerateProcessModulesAndThreads(IfIdentityMatches)
//     enumerateKernelModules   -> KernelThreadAuditTab::queryKernelModules（R3 NtQuerySystemInformation）
//     processCandidates        -> ks::process::EnumerateProcesses（带进程级缓存）
//     readPointer              -> ksword::memwb_ports::WorkbenchIoPort::Read（真实端口，不回退）
//     ddmaGeneration           -> ksword::memory_backend::ddmaSessionGeneration
// - 候选进程的进程级缓存（ProcessCandidateCache）与目标 chip 的展示字段查询
//   （QueryAttachedProcessInfo）。
// - 判据（模块记录映射、快照成败判定、指针读取结果映射、刷新时机）全部来自纯函数文件
//   WorkbenchServicesMapping.* / WorkbenchServicesProbe.*，这里只负责"问系统一次"。
//
// 线程与生命周期（IWorkbenchServices.h 的线程规则）：
// - enumerateProcessModules / enumerateKernelModules 在 QThreadPool 工作线程调用，本文件对应
//   的实现只用局部变量与线程安全的系统调用，不碰任何 QWidget/QObject。
// - processCandidates / readPointer / ddmaGeneration 恒在 UI 线程调用。
// - DockWorkbenchServices 不持有任何 QObject/Dock 裸指针，唯一的成员是无状态的端口对象，
//   因此在"最后一个 shared_ptr 持有者恰好是工作线程"时析构也是安全的。
// ============================================================

namespace svc = ksword::memwb_services_detail;

namespace
{
    // KernelThreadAuditTab::ModuleQueryStatus 的四个数值必须与纯函数
    // KernelModuleQueryStatusName 的映射一致；枚举被改动会在编译期立刻暴露，而不是静默错名。
    static_assert(
        static_cast<std::uint32_t>(KernelThreadAuditTab::ModuleQueryStatus::Ok) == 0U,
        "ModuleQueryStatus::Ok must stay 0");
    static_assert(
        static_cast<std::uint32_t>(KernelThreadAuditTab::ModuleQueryStatus::ApiUnavailable) == 1U,
        "ModuleQueryStatus::ApiUnavailable must stay 1");
    static_assert(
        static_cast<std::uint32_t>(KernelThreadAuditTab::ModuleQueryStatus::LengthQueryFailed) == 2U,
        "ModuleQueryStatus::LengthQueryFailed must stay 2");
    static_assert(
        static_cast<std::uint32_t>(KernelThreadAuditTab::ModuleQueryStatus::SnapshotFailed) == 3U,
        "ModuleQueryStatus::SnapshotFailed must stay 3");

    // kCandidateStaleMs：候选进程缓存的陈旧阈值（毫秒）。目标进程输入框每次输入都会拉
    // 候选列表，3 秒内重复拉取直接用缓存；超过阈值再后台刷新一次。
    constexpr std::uint64_t kCandidateStaleMs = 3000U;

    // NowMs：单调时钟的毫秒读数，供缓存年龄判断使用。
    // 传出：steady_clock 自起点以来的毫秒数（只用于差值，不对应墙钟时间）。
    std::uint64_t NowMs()
    {
        const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    }

    // ProcessCandidateCache：候选进程列表的进程级缓存。
    // 为什么需要：ks::process::EnumerateProcesses 每次都附带一次系统句柄快照，耗时可达上百
    //   毫秒；processCandidates() 却在 UI 线程被反复调用。缓存策略由纯函数
    //   DecideCandidateRefresh 决定：从未取到过才允许同步阻塞一次，之后要么直接用缓存，
    //   要么返回旧缓存并在线程池里后台刷新。
    // 生命周期：全进程唯一，故意泄漏（new 出来不释放），避免静态析构阶段后台刷新任务
    //   还在运行时访问已销毁的互斥锁。只存纯数据，不存任何指针。
    class ProcessCandidateCache
    {
    public:
        // Instance：取全进程唯一实例（首次调用时创建）。
        static ProcessCandidateCache& Instance()
        {
            static ProcessCandidateCache* const instance = new ProcessCandidateCache();
            return *instance;
        }

        // Snapshot：取一份候选进程列表（拷贝）。
        // 行为：按 DecideCandidateRefresh 的结论——直接返回缓存、同步枚举后返回、或返回旧
        //       缓存并启动后台刷新。UI 线程调用。
        std::vector<ksword::memwb::ProcessCandidate> Snapshot()
        {
            svc::CandidateRefreshDecision decision = svc::CandidateRefreshDecision::ReturnCache;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                decision = svc::DecideCandidateRefresh(
                    everFilled_,
                    refreshing_,
                    filledAtMs_,
                    NowMs(),
                    kCandidateStaleMs);
                if (decision == svc::CandidateRefreshDecision::ReturnCache)
                {
                    return candidates_;
                }
                // 抢占刷新权：之后的查询看到 refreshing_ 为真就不会重复启动。
                refreshing_ = true;
            }

            if (decision == svc::CandidateRefreshDecision::RefreshAsync)
            {
                StartAsyncRefresh();
            }
            else
            {
                // 从未填充过：同步枚举一次（只会发生一次，且通常已被 Prefetch 抢先）。
                RunRefresh();
            }

            std::lock_guard<std::mutex> guard(mutex_);
            return candidates_;
        }

        // Prefetch：服务对象创建时调用，提前在后台填充缓存，让用户第一次输入时缓存已就绪。
        // 行为：缓存从未填充且没有刷新在途才启动；其余情况什么都不做。
        void Prefetch()
        {
            {
                std::lock_guard<std::mutex> guard(mutex_);
                if (everFilled_ || refreshing_)
                {
                    return;
                }
                refreshing_ = true;
            }
            StartAsyncRefresh();
        }

        // FindName：只在已有缓存里按 pid 查名，不触发任何枚举。
        // 传出：命中的进程名；没有返回空串。
        std::string FindName(const std::uint32_t pid)
        {
            std::lock_guard<std::mutex> guard(mutex_);
            return svc::FindProcessName(candidates_, pid);
        }

    private:
        // 构造私有：只能经 Instance() 取得。
        ProcessCandidateCache() = default;

        // StartAsyncRefresh：把一次刷新交给全局线程池。
        // 前置条件：调用方已把 refreshing_ 置真（RunRefresh 结束时会复位）。
        void StartAsyncRefresh()
        {
            QThreadPool::globalInstance()->start(QRunnable::create([]() {
                ProcessCandidateCache::Instance().RunRefresh();
            }));
        }

        // RunRefresh：枚举进程并发布结果，结束时一定复位 refreshing_。
        // 失败（枚举异常或结果为空）时保留旧列表，但仍刷新"填充时刻"，避免枚举持续失败
        // 时每次查询都重新同步阻塞；下一次重试等到陈旧阈值之后在后台进行。
        void RunRefresh()
        {
            std::vector<ksword::memwb::ProcessCandidate> fresh;
            bool enumerated = false;
            try
            {
                const std::vector<ks::process::ProcessRecord> records =
                    ks::process::EnumerateProcesses(ks::process::ProcessEnumStrategy::Auto);
                fresh.reserve(records.size());
                for (const ks::process::ProcessRecord& record : records)
                {
                    // pid 为 0（系统空闲进程）不能作为目标，不进候选列表。
                    if (record.pid == 0U)
                    {
                        continue;
                    }
                    ksword::memwb::ProcessCandidate candidate;
                    candidate.pid = record.pid;
                    candidate.name = record.processName;
                    fresh.push_back(std::move(candidate));
                }
                enumerated = !fresh.empty();
            }
            catch (...)
            {
                enumerated = false;
            }

            std::lock_guard<std::mutex> guard(mutex_);
            if (enumerated)
            {
                candidates_ = std::move(fresh);
            }
            everFilled_ = true;
            filledAtMs_ = NowMs();
            refreshing_ = false;
        }

        // mutex_：保护下面全部可变状态（UI 线程与后台刷新线程都会访问）。
        std::mutex mutex_;
        // candidates_：最近一次成功枚举得到的候选列表。
        std::vector<ksword::memwb::ProcessCandidate> candidates_;
        // everFilled_：是否至少完成过一次填充尝试（成功或失败都算）。
        bool everFilled_ = false;
        // refreshing_：是否有刷新在途。
        bool refreshing_ = false;
        // filledAtMs_：上一次填充尝试完成的时刻（NowMs 口径）。
        std::uint64_t filledAtMs_ = 0U;
    };

    // KernelRecordsToRaw：把内核模块枚举结果转成纯函数使用的原始字段列表。
    // 传入：records 既有枚举的返回值。传出：等长的 RawModuleInfo 列表（UTF-8）。
    std::vector<svc::RawModuleInfo> KernelRecordsToRaw(
        const std::vector<KernelThreadAuditTab::ModuleRecord>& records)
    {
        std::vector<svc::RawModuleInfo> rawList;
        rawList.reserve(records.size());
        for (const KernelThreadAuditTab::ModuleRecord& record : records)
        {
            svc::RawModuleInfo raw;
            raw.name = record.name.toStdString();
            raw.fullPath = record.path.toStdString();
            raw.base = record.baseAddress;
            raw.size = record.imageSize;
            rawList.push_back(std::move(raw));
        }
        return rawList;
    }

    // DockWorkbenchServices：生产 IWorkbenchServices，详见文件头。
    class DockWorkbenchServices final : public ks::ui::IWorkbenchServices
    {
    public:
        // 构造：提前在后台预热候选进程缓存。UI 线程调用。
        DockWorkbenchServices()
        {
            ProcessCandidateCache::Instance().Prefetch();
        }

        // enumerateProcessModules：枚举某个进程实例的模块列表。工作线程调用。
        // 传入：pid 目标进程号；expectCreateTime 期望的进程创建时间（100ns），非 0 时先
        //       核对进程身份——既有的 EnumerateProcessModulesAndThreadsIfIdentityMatches
        //       在整个枚举期间持有同一个进程句柄，核对 PID 与创建时间，不符时返回空快照
        //       并在诊断文本里写明原因，因此不会枚举到复用同一 PID 的另一个进程实例；
        //       为 0 表示调用方不要求校验。
        // 传出：ok=false 时 failure 是英文技术说明；任何异常都转成 ok=false，绝不抛出。
        ks::ui::ModuleEnumResult enumerateProcessModules(
            const std::uint32_t pid,
            const std::uint64_t expectCreateTime) override
        {
            ks::ui::ModuleEnumResult result;
            if (pid == 0U)
            {
                result.failure = "process module enumeration requires a non-zero pid";
                return result;
            }

            try
            {
                // 不做签名校验（includeSignatureCheck=false）：目录只需要名字与基址，
                // 签名校验很慢且与解析模块名无关。
                const ks::process::ProcessModuleSnapshot snapshot = (expectCreateTime != 0U)
                    ? ks::process::EnumerateProcessModulesAndThreadsIfIdentityMatches(
                        pid, expectCreateTime, false)
                    : ks::process::EnumerateProcessModulesAndThreads(pid, false);

                // 空模块列表不等于失败：成败只看诊断文本里的成功标记。
                const svc::ModuleSnapshotVerdict verdict =
                    svc::JudgeProcessModuleSnapshot(snapshot.diagnosticText, snapshot.modules.size());
                if (!verdict.ok)
                {
                    result.failure = verdict.failure;
                    return result;
                }

                std::vector<svc::RawModuleInfo> rawList;
                rawList.reserve(snapshot.modules.size());
                for (const ks::process::ProcessModuleRecord& module : snapshot.modules)
                {
                    svc::RawModuleInfo raw;
                    raw.name = module.moduleName;
                    raw.fullPath = module.modulePath;
                    raw.base = module.moduleBaseAddress;
                    raw.size = module.moduleSizeBytes;
                    rawList.push_back(std::move(raw));
                }
                result.records = svc::MapRawModules(rawList);
                result.ok = true;
                return result;
            }
            catch (const std::exception& exception)
            {
                result.ok = false;
                result.records.clear();
                result.failure = std::string("process module enumeration threw: ") + exception.what();
                return result;
            }
            catch (...)
            {
                result.ok = false;
                result.records.clear();
                result.failure = "process module enumeration threw an unknown exception";
                return result;
            }
        }

        // enumerateKernelModules：枚举内核模块列表。工作线程调用，无参数。
        // 复用既有 KernelThreadAuditTab::queryKernelModules：它走 R3 的
        // NtQuerySystemInformation(SystemModuleInformation)，不依赖驱动、不碰
        // DeviceIoControl，注释明确"可在任意线程调用"；旧 Tab5 的内核模块下拉框用的也是它。
        // 传出：ok=false 时 failure 带状态名、NTSTATUS 与所需缓冲区字节数。
        ks::ui::ModuleEnumResult enumerateKernelModules() override
        {
            ks::ui::ModuleEnumResult result;
            try
            {
                KernelThreadAuditTab::ModuleQueryStatus status =
                    KernelThreadAuditTab::ModuleQueryStatus::Ok;
                long nativeStatus = 0L;
                unsigned long requiredBytes = 0UL;
                const std::vector<KernelThreadAuditTab::ModuleRecord> records =
                    KernelThreadAuditTab::queryKernelModules(&status, &nativeStatus, &requiredBytes);

                if (status != KernelThreadAuditTab::ModuleQueryStatus::Ok)
                {
                    result.failure = svc::FormatKernelModuleFailure(
                        svc::KernelModuleQueryStatusName(static_cast<std::uint32_t>(status)),
                        static_cast<std::uint32_t>(nativeStatus),
                        static_cast<std::uint32_t>(requiredBytes));
                    return result;
                }

                result.records = svc::MapRawModules(KernelRecordsToRaw(records));
                result.ok = true;
                return result;
            }
            catch (const std::exception& exception)
            {
                result.ok = false;
                result.records.clear();
                result.failure = std::string("kernel module enumeration threw: ") + exception.what();
                return result;
            }
            catch (...)
            {
                result.ok = false;
                result.records.clear();
                result.failure = "kernel module enumeration threw an unknown exception";
                return result;
            }
        }

        // processCandidates：当前可见的候选进程列表。UI 线程调用。
        // 走进程级缓存（见 ProcessCandidateCache），不会每次都做完整进程枚举。
        std::vector<ksword::memwb::ProcessCandidate> processCandidates() override
        {
            return ProcessCandidateCache::Instance().Snapshot();
        }

        // readPointer：按会话描述的目标读一个指针宽度的值。UI 线程调用。
        // 只经真实端口 WorkbenchIoPort::Read 读取，**不回退到别的通道**：读不到就如实
        // 报失败。解析器的解引用门控已经排除了物理范围与 DDMA，这里再核对一遍作为第二道
        // 保险（门控被绕过时也不会去改写磁盘暂存扇区）。
        // 传入：session 当前会话；address 读取地址；width 宽度（4 或 8）。
        ks::ui::PointerReadResult readPointer(
            const ksword::memwb::MemoryTargetSession& session,
            const std::uint64_t address,
            const std::uint32_t width) override
        {
            ks::ui::PointerReadResult result;
            if (width != 4U && width != 8U)
            {
                result.failure = "pointer width must be 4 or 8 bytes";
                return result;
            }
            if (ksword::memwb::Validate(session) != ksword::memwb::SessionError::None)
            {
                result.failure = "target session is not self-consistent";
                return result;
            }
            if (!svc::IsPointerReadAllowed(session.scope, session.channel))
            {
                result.failure =
                    "pointer dereference is not allowed on the physical scope or the disk-transfer channel";
                return result;
            }
            // 读取范围 [address, address+width) 不得在 64 位地址空间尾部回绕。
            if (address > UINT64_MAX - static_cast<std::uint64_t>(width - 1U))
            {
                result.failure = "pointer address range overflows the 64-bit address space";
                return result;
            }

            const ksword::memwb::IoReadResult read = ioPort_.Read(session, address, width);
            return svc::MapPointerRead(read, width);
        }

        // ddmaGeneration：DDMA 暂存扇区当前代次，包装既有 ddmaSessionGeneration()。UI 线程调用。
        std::uint64_t ddmaGeneration() override
        {
            return ksword::memory_backend::ddmaSessionGeneration();
        }

    private:
        // ioPort_：真实读写端口，无成员状态（每次调用各自开句柄），可安全随对象销毁。
        ksword::memwb_ports::WorkbenchIoPort ioPort_;
    };
}

namespace ks::ui::workbench_dock::detail
{
    std::unique_ptr<IWorkbenchServices> CreateProductionServices()
    {
        return std::make_unique<DockWorkbenchServices>();
    }

    std::string LookupProcessName(const std::uint32_t pid)
    {
        if (pid == 0U)
        {
            return std::string();
        }
        // 先取映像路径（一次 OpenProcess + QueryFullProcessImageName，便宜且是当前事实），
        // 再截文件名；不用 GetProcessNameByPID：它在路径取不到时会回退到完整进程枚举。
        const std::string path = ks::process::QueryProcessPathByPid(pid);
        if (!path.empty())
        {
            const std::string fileName = svc::FileNameFromPath(path);
            if (!fileName.empty())
            {
                return fileName;
            }
        }
        // 路径取不到（例如受保护进程）：退回候选进程缓存，不触发枚举。
        return ProcessCandidateCache::Instance().FindName(pid);
    }
}

namespace ks::ui::workbench_dock
{
    std::optional<ks::ui::AttachedProcessDisplayInfo> QueryAttachedProcessInfo(const std::uint32_t pid)
    {
        if (pid == 0U)
        {
            return std::nullopt;
        }

        const std::string processName = detail::LookupProcessName(pid);

        // 可读写判据与旧 Dock 附加时完全一致（MemoryDock.ProcessRegion.cpp attachToProcess 的
        // 首选权限组合）：能以读+写+操作+查询权限打开才算可读写。只用来探测，立即关闭句柄。
        bool canReadWrite = false;
        unsigned long openError = 0UL;
        const HANDLE processHandle = ::OpenProcess(
            PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION,
            FALSE,
            static_cast<DWORD>(pid));
        if (processHandle != nullptr)
        {
            canReadWrite = true;
            ::CloseHandle(processHandle);
        }
        else
        {
            openError = ::GetLastError();
        }

        // 进程已不存在（pid 无效）且取不到名字：没有任何可展示的信息，让 chip 只显示 pid。
        if (!canReadWrite && processName.empty() && openError == ERROR_INVALID_PARAMETER)
        {
            return std::nullopt;
        }

        // 变量名不用 info：会遮蔽 Framework.h 的全局日志流 info（C4459）。
        ks::ui::AttachedProcessDisplayInfo displayInfo;
        displayInfo.processName = QString::fromStdString(processName);
        displayInfo.canReadWrite = canReadWrite;
        return displayInfo;
    }
}
