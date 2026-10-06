#include "../Framework.h"

#include "MemoryDock.WorkbenchServices.h"
#include "MemoryAccessBackend.h"
#include "WorkbenchServicesMapping.h"
#include "WorkbenchServicesProbe.h"
#include "../ArkDriverClient/ArkDriverClient.h"
#include "../UI/ThemeStatusRole.h"

#include <QRunnable>
#include <QString>
#include <QThreadPool>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <vector>

// ============================================================
// MemoryDock.WorkbenchServices.Gate.cpp
// 作用：
// - QueryGateInputs：通道可用性（EvaluateChannel）的运行期输入——驱动是否已加载、HVM 通道
//   探测缓存、DDMA 会话是否就绪。
// - QueryProtection：状态条"保护"段的数据——按 pid + 地址 VirtualQueryEx 查页保护与区域类型，
//   进程句柄按 pid + 进程创建时间做小缓存。
// - 判据全部来自纯函数文件（WorkbenchServicesProbe.* 的 HvmProbeTracker / DecideHandleCache /
//   CreateTimesMatch，WorkbenchServicesMapping.* 的 FormatProtectionBadge），这里只负责
//   "问系统一次"。
//
// 线程：两个函数都只在 UI 线程调用；HVM 探测的真正驱动调用在 QThreadPool 工作线程里完成，
// 结果经 HvmProbeTracker（内部有互斥锁）回到状态里，UI 线程从不为它阻塞。
// ============================================================

namespace svc = ksword::memwb_services_detail;

namespace
{
    // Win32 页状态/保护/类型常量的本地副本（WorkbenchServicesMapping.h 里为了不包含
    // Windows.h 而抄了一份数值）必须与 winnt.h 的宏逐个一致；宏被改动会编译失败。
    static_assert(svc::kMemCommit == static_cast<std::uint32_t>(MEM_COMMIT), "MEM_COMMIT mismatch");
    static_assert(svc::kMemReserve == static_cast<std::uint32_t>(MEM_RESERVE), "MEM_RESERVE mismatch");
    static_assert(svc::kMemFree == static_cast<std::uint32_t>(MEM_FREE), "MEM_FREE mismatch");
    static_assert(svc::kMemPrivate == static_cast<std::uint32_t>(MEM_PRIVATE), "MEM_PRIVATE mismatch");
    static_assert(svc::kMemMapped == static_cast<std::uint32_t>(MEM_MAPPED), "MEM_MAPPED mismatch");
    static_assert(svc::kMemImage == static_cast<std::uint32_t>(MEM_IMAGE), "MEM_IMAGE mismatch");
    static_assert(svc::kPageNoAccess == static_cast<std::uint32_t>(PAGE_NOACCESS), "PAGE_NOACCESS mismatch");
    static_assert(svc::kPageReadOnly == static_cast<std::uint32_t>(PAGE_READONLY), "PAGE_READONLY mismatch");
    static_assert(svc::kPageReadWrite == static_cast<std::uint32_t>(PAGE_READWRITE), "PAGE_READWRITE mismatch");
    static_assert(svc::kPageWriteCopy == static_cast<std::uint32_t>(PAGE_WRITECOPY), "PAGE_WRITECOPY mismatch");
    static_assert(svc::kPageExecute == static_cast<std::uint32_t>(PAGE_EXECUTE), "PAGE_EXECUTE mismatch");
    static_assert(
        svc::kPageExecuteRead == static_cast<std::uint32_t>(PAGE_EXECUTE_READ),
        "PAGE_EXECUTE_READ mismatch");
    static_assert(
        svc::kPageExecuteReadWrite == static_cast<std::uint32_t>(PAGE_EXECUTE_READWRITE),
        "PAGE_EXECUTE_READWRITE mismatch");
    static_assert(
        svc::kPageExecuteWriteCopy == static_cast<std::uint32_t>(PAGE_EXECUTE_WRITECOPY),
        "PAGE_EXECUTE_WRITECOPY mismatch");
    static_assert(svc::kPageGuard == static_cast<std::uint32_t>(PAGE_GUARD), "PAGE_GUARD mismatch");

    // kDriverCacheMs：驱动是否已加载这个结论的缓存时长（毫秒）。页读取每次请求都会取
    // GateInputs，不能每次都去 CreateFile 设备；1 秒内复用上一次结论。
    constexpr std::uint64_t kDriverCacheMs = 1000U;

    // kHvmReprobeMs：HVM 通道得出结论之后重新探测的间隔（毫秒）。窗口标定状态可能在运行期
    // 变化，但探测是一次驱动 IOCTL，不需要频繁重发。
    constexpr std::uint64_t kHvmReprobeMs = 10000U;

    // kHandleRevalidateMs：保护查询句柄缓存复核进程身份的间隔（毫秒）。
    constexpr std::uint64_t kHandleRevalidateMs = 2000U;

    // kMaxProtectionHandles：保护查询句柄缓存的条目上限（"小缓存"）。主 Dock 与内嵌进程
    // 详情窗口各查各的目标，几个条目足够，超过时淘汰最久没核验过的一个。
    constexpr std::size_t kMaxProtectionHandles = 4U;

    // NowMs：单调时钟的毫秒读数（只用于差值）。
    std::uint64_t NowMs()
    {
        const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    }

    // GateState：QueryGateInputs 的进程级状态。
    // 生命周期：全进程唯一，故意泄漏，避免静态析构阶段后台探测任务仍在运行时访问已销毁对象。
    struct GateState
    {
        // hvmTracker：HVM 探测的异步缓存状态机（内部有互斥锁，工作线程会调用 Complete）。
        svc::HvmProbeTracker hvmTracker{kHvmReprobeMs};
        // driverChecked：是否已经检查过驱动是否加载。
        bool driverChecked = false;
        // driverLoaded：最近一次检查的结论。
        bool driverLoaded = false;
        // driverCheckedAtMs：最近一次检查的时刻。
        std::uint64_t driverCheckedAtMs = 0U;
    };

    // Gate：取全进程唯一的 GateState。
    GateState& Gate()
    {
        static GateState* const state = new GateState();
        return *state;
    }

    // QueryDriverLoaded：驱动设备是否可打开（带 1 秒缓存）。UI 线程调用。
    // 判据：与 ProcessDock 的被动轮询路径同口径——静默打开 KswordARK 控制设备（不触发全局
    //       "请启用 R0"通知），能打开就算已加载。
    // 传入：state 全局状态；nowMs 当前时刻。传出：驱动是否已加载。
    bool QueryDriverLoaded(GateState& state, const std::uint64_t nowMs)
    {
        // 缓存命中：检查过、没过期、且时钟没有倒退。
        if (state.driverChecked &&
            nowMs >= state.driverCheckedAtMs &&
            (nowMs - state.driverCheckedAtMs) < kDriverCacheMs)
        {
            return state.driverLoaded;
        }

        const ksword::ark::DriverClient driverClient;
        ksword::ark::DriverHandle driverHandle = driverClient.openSilently();
        state.driverLoaded = driverHandle.isValid();
        state.driverChecked = true;
        state.driverCheckedAtMs = nowMs;
        return state.driverLoaded;
    }

    // StartHvmProbe：在线程池里做一次 HVM 通道可用性探测，完成后把结论交回 HvmProbeTracker。
    // 传入：ticket 探测票据（HvmProbeTracker::Observe 发放，Complete 凭它对账）。
    // 探测内容：既有 isHvmMemoryUsable——发一次 QUERY_WINDOW，不碰任何内存，只问私有页表
    //           窗口在不在。任何异常都按"不可用"回报，保证票据一定被 Complete（否则探测
    //           会永远处于在途状态）。
    void StartHvmProbe(const std::uint64_t ticket)
    {
        QThreadPool::globalInstance()->start(QRunnable::create([ticket]() {
            bool usable = false;
            try
            {
                QString reason;
                usable = ksword::memory_backend::isHvmMemoryUsable(&reason);
            }
            catch (...)
            {
                usable = false;
            }
            Gate().hvmTracker.Complete(ticket, usable, NowMs());
        }));
    }

    // CachedProcessHandle：保护查询句柄缓存的一个条目。
    struct CachedProcessHandle
    {
        // handle：以 PROCESS_QUERY_INFORMATION 打开的进程句柄（VirtualQueryEx 的最低要求）。
        HANDLE handle = nullptr;
        // pid：句柄所属进程号。
        std::uint32_t pid = 0U;
        // createTime：打开时从这个句柄读到的进程创建时间（100ns）；0 表示没读到。
        std::uint64_t createTime = 0U;
        // verifiedAtMs：上次核验"这个 pid 仍是同一个进程实例"的时刻。
        std::uint64_t verifiedAtMs = 0U;
    };

    // ProtectionHandleCache：保护查询句柄的小缓存（全进程唯一，故意泄漏）。
    struct ProtectionHandleCache
    {
        // mutex：保护 entries（本函数族只在 UI 线程调用，加锁是为了对误用保持安全）。
        std::mutex mutex;
        // entries：缓存条目，数量不超过 kMaxProtectionHandles。
        std::vector<CachedProcessHandle> entries;
    };

    // HandleCache：取全进程唯一的句柄缓存。
    ProtectionHandleCache& HandleCache()
    {
        static ProtectionHandleCache* const cache = new ProtectionHandleCache();
        return *cache;
    }

    // ReadProcessCreateTime：读进程句柄的创建时间（FILETIME，100ns）。
    // 传入：handle 进程句柄（至少 PROCESS_QUERY_LIMITED_INFORMATION）。传出：创建时间；失败返回 0。
    std::uint64_t ReadProcessCreateTime(const HANDLE handle)
    {
        FILETIME creationTime{};
        FILETIME exitTime{};
        FILETIME kernelTime{};
        FILETIME userTime{};
        if (::GetProcessTimes(handle, &creationTime, &exitTime, &kernelTime, &userTime) == FALSE)
        {
            return 0U;
        }
        return (static_cast<std::uint64_t>(creationTime.dwHighDateTime) << 32U) |
            static_cast<std::uint64_t>(creationTime.dwLowDateTime);
    }

    // ReadCurrentCreateTimeByPid：另开一个临时句柄，读 pid 当前对应进程实例的创建时间。
    // 用途：复核缓存句柄所属的进程是否仍是"现在这个 pid"。传出：创建时间；打不开返回 0。
    std::uint64_t ReadCurrentCreateTimeByPid(const std::uint32_t pid)
    {
        const HANDLE probe = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
        if (probe == nullptr)
        {
            return 0U;
        }
        const std::uint64_t createTime = ReadProcessCreateTime(probe);
        ::CloseHandle(probe);
        return createTime;
    }

    // DropHandleEntry：关闭并移除缓存里 index 处的条目。调用方持有 cache.mutex。
    void DropHandleEntry(ProtectionHandleCache& cache, const std::size_t index)
    {
        if (index >= cache.entries.size())
        {
            return;
        }
        if (cache.entries[index].handle != nullptr)
        {
            ::CloseHandle(cache.entries[index].handle);
        }
        cache.entries.erase(cache.entries.begin() + static_cast<std::ptrdiff_t>(index));
    }

    // AcquireProtectionHandle：取一个可用于 VirtualQueryEx 的进程句柄。调用方持有 cache.mutex。
    // 流程：DecideHandleCache 给出处置——UseCached 直接复用；Revalidate 另开临时句柄读当前
    //       创建时间，与缓存值经 CreateTimesMatch 比对，不符（pid 被复用或进程已退出）则丢弃
    //       重开；Reopen 重新打开并记录创建时间。缓存满时淘汰最久没核验过的条目。
    // 传入：cache 缓存；pid 目标；nowMs 当前时刻。传出：句柄（归缓存所有，调用方不得关闭）；
    //       打不开返回 nullptr。
    HANDLE AcquireProtectionHandle(
        ProtectionHandleCache& cache,
        const std::uint32_t pid,
        const std::uint64_t nowMs)
    {
        // 按 pid 找已有条目。
        std::size_t foundIndex = cache.entries.size();
        for (std::size_t index = 0U; index < cache.entries.size(); ++index)
        {
            if (cache.entries[index].pid == pid)
            {
                foundIndex = index;
                break;
            }
        }
        const bool hasEntry = foundIndex < cache.entries.size();

        const svc::HandleCacheDecision decision = svc::DecideHandleCache(
            hasEntry,
            hasEntry ? cache.entries[foundIndex].pid : 0U,
            pid,
            hasEntry ? cache.entries[foundIndex].verifiedAtMs : 0U,
            nowMs,
            kHandleRevalidateMs);

        if (decision == svc::HandleCacheDecision::UseCached)
        {
            return cache.entries[foundIndex].handle;
        }

        if (decision == svc::HandleCacheDecision::Revalidate)
        {
            const std::uint64_t freshCreateTime = ReadCurrentCreateTimeByPid(pid);
            if (svc::CreateTimesMatch(cache.entries[foundIndex].createTime, freshCreateTime))
            {
                cache.entries[foundIndex].verifiedAtMs = nowMs;
                return cache.entries[foundIndex].handle;
            }
            // 创建时间对不上：这个 pid 现在属于另一个进程实例（或进程已退出），旧句柄作废。
            DropHandleEntry(cache, foundIndex);
        }

        // Reopen（或复核失败后的重开）：VirtualQueryEx 要求 PROCESS_QUERY_INFORMATION。
        const HANDLE opened = ::OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, static_cast<DWORD>(pid));
        if (opened == nullptr)
        {
            return nullptr;
        }

        // 缓存已满：淘汰最久没核验过的条目。
        while (cache.entries.size() >= kMaxProtectionHandles)
        {
            std::size_t oldestIndex = 0U;
            for (std::size_t index = 1U; index < cache.entries.size(); ++index)
            {
                if (cache.entries[index].verifiedAtMs < cache.entries[oldestIndex].verifiedAtMs)
                {
                    oldestIndex = index;
                }
            }
            DropHandleEntry(cache, oldestIndex);
        }

        CachedProcessHandle entry;
        entry.handle = opened;
        entry.pid = pid;
        entry.createTime = ReadProcessCreateTime(opened);
        entry.verifiedAtMs = nowMs;
        cache.entries.push_back(entry);
        return opened;
    }

    // ToStatusRole：纯函数文件里的徽章配色语义 -> 状态条使用的 StatusRole。
    ks::ui::StatusRole ToStatusRole(const svc::BadgeRole role)
    {
        switch (role)
        {
        case svc::BadgeRole::Info:
            return ks::ui::StatusRole::Info;
        case svc::BadgeRole::Success:
            return ks::ui::StatusRole::Success;
        case svc::BadgeRole::Warning:
            return ks::ui::StatusRole::Warning;
        case svc::BadgeRole::Error:
            return ks::ui::StatusRole::Error;
        case svc::BadgeRole::Idle:
            break;
        }
        return ks::ui::StatusRole::Idle;
    }
}

namespace ks::ui::workbench_dock
{
    ksword::memwb::GateInputs QueryGateInputs()
    {
        GateState& state = Gate();
        const std::uint64_t nowMs = NowMs();
        ksword::memwb::GateInputs inputs;

        // hasProcessTarget 故意不填（见头文件）：没有会话参数，本函数不知道当前目标。

        // 驱动是否已加载：1 秒缓存。
        inputs.driverLoaded = QueryDriverLoaded(state, nowMs);

        // HVM 探测：驱动未加载时不探测并复位；加载后在后台探测，未出结论报告 NotProbed
        // （EvaluateChannel 据此判"可用但未知"，不置灰）。
        const svc::HvmProbeTracker::Observation observation =
            state.hvmTracker.Observe(inputs.driverLoaded, nowMs);
        if (observation.action == svc::HvmProbeTracker::Action::StartProbe)
        {
            StartHvmProbe(observation.ticket);
        }
        inputs.hvmProbe = observation.state;

        // DDMA 会话是否就绪：既有 isDdmaUsable 判据（已探测 → 未开内核调试 → 已填 LBA →
        // 已确认覆盖），纯内存读取、没有副作用，每次现算保证与 DDMA 页的改动立刻同步。
        QString ddmaReason;
        inputs.ddmaSessionReady = ksword::memory_backend::isDdmaUsable(
            ksword::memory_backend::currentDdmaSession(),
            &ddmaReason);
        return inputs;
    }

    std::optional<ks::ui::WorkbenchProtectionInfo> QueryProtection(
        const std::uint32_t pid,
        const std::uint64_t address)
    {
        // 没有进程目标，或地址在内核半区（VirtualQueryEx 只能查目标进程的用户态空间）：
        // 该段不显示。
        if (pid == 0U || ksword::memwb::IsKernelVirtualAddress(address))
        {
            return std::nullopt;
        }

        ProtectionHandleCache& cache = HandleCache();
        std::lock_guard<std::mutex> guard(cache.mutex);
        const HANDLE handle = AcquireProtectionHandle(cache, pid, NowMs());
        if (handle == nullptr)
        {
            return std::nullopt;
        }

        MEMORY_BASIC_INFORMATION basicInformation{};
        const SIZE_T queried = ::VirtualQueryEx(
            handle,
            reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(address)),
            &basicInformation,
            sizeof(basicInformation));
        if (queried == 0U)
        {
            // 失败：地址超出目标地址空间（ERROR_INVALID_PARAMETER）不说明句柄有问题；
            // 其它错误（进程已退出、句柄失效）把缓存条目丢掉，下一次重新打开。
            const DWORD queryError = ::GetLastError();
            if (queryError != ERROR_INVALID_PARAMETER)
            {
                for (std::size_t index = 0U; index < cache.entries.size(); ++index)
                {
                    if (cache.entries[index].pid == pid)
                    {
                        DropHandleEntry(cache, index);
                        break;
                    }
                }
            }
            return std::nullopt;
        }

        const svc::ProtectionBadge badge = svc::FormatProtectionBadge(
            static_cast<std::uint32_t>(basicInformation.State),
            static_cast<std::uint32_t>(basicInformation.Protect),
            static_cast<std::uint32_t>(basicInformation.Type));

        // 变量名不用 info：会遮蔽 Framework.h 的全局日志流 info（C4459）。
        ks::ui::WorkbenchProtectionInfo protectionInfo;
        protectionInfo.text = QString::fromStdString(badge.text);
        protectionInfo.role = ToStatusRole(badge.role);
        return protectionInfo;
    }
}
