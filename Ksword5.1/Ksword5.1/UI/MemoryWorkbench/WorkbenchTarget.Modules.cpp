// ============================================================
// WorkbenchTarget.Modules.cpp
// 作用：
// - WorkbenchTarget 的异步模块枚举：在 QThreadPool 的工作线程上调用
//   IWorkbenchServices::enumerateProcessModules/enumerateKernelModules，
//   完成后用 QPointer 守卫 + 票据 + 所有者核对，安全地把结果落回 UI 线程并
//   提交到对应的 MemoryModuleDirectory。
// - 不采用旧代码 refreshKernelModuleCacheAsync 那种"在工作线程 lambda 里直接
//   捕获裸 this"的写法（见 target.md 2.4）：这里工作线程只持有一份独立的
//   services_ shared_ptr 与请求参数的值拷贝，不触碰 WorkbenchTarget 本身。
// - **D1 修复（释放后使用 / UAF）**：旧版本在工作线程里把 QPointer::data() 取出
//   的裸指针当作 QMetaObject::invokeMethod 的 context 参数传进去，并假设
//   "Qt 保证回调不会在已销毁对象上执行"——这个保证只覆盖"回调已经排队之后"，
//   不覆盖"data() 取出裸指针到调用真正排队之间"的窗口：如果 WorkbenchTarget
//   恰好在这个窗口里被销毁，invokeMethod 内部对 context->thread() 的访问就是
//   对已释放堆内存的解引用（cdb 实测栈为 Qt6Core!QObject::thread ←
//   QMetaObject::invokeMethodImpl ← ModuleEnumTask::run）。正确做法：
//   context 参数改用进程内长期存活的 QCoreApplication::instance()（它只决定
//   "回调在哪个线程的事件循环里执行"，不代表回调会碰它），真正的
//   WorkbenchTarget 只以 QPointer 的值被捕获进 lambda，回调真正落到 UI 线程时
//   才在那一刻重新取 data() 判空——这才是唯一安全的读取时机（QPointer 的失效
//   通知与这段代码同在 UI 线程，不存在竞态）。
// ============================================================

#include "WorkbenchTarget.h"

#include <QCoreApplication>
#include <QPointer>
#include <QRunnable>
#include <QThreadPool>

#include <utility>

namespace ks::ui
{
    std::vector<ksword::memwb::ModuleRecord> WorkbenchTarget::pointerChainModules()
    {
        const QPointer<WorkbenchTarget> self(this);
        const auto snapshot = session();
        if (!self) return {};
        const auto owner = ksword::memwb::ModuleOwnerForSession(snapshot);
        if (owner.scope != ksword::memwb::Scope::ProcessVirtual || owner.pid == 0 || owner.createTime == 0
            || processDirectory_.GetState() != ksword::memwb::MemoryModuleDirectory::State::Ready
            || processDirectory_.Owner() != owner) return {};
        return processDirectory_.Records();
    }

    namespace
    {
        // LowerAscii：模块名比较用的 ASCII 小写副本（模块名只有 ASCII 需要不区分大小写）。
        std::string LowerAscii(std::string text)
        {
            for (char& ch : text)
            {
                if (ch >= 'A' && ch <= 'Z')
                {
                    ch = static_cast<char>(ch - 'A' + 'a');
                }
            }
            return text;
        }

        // EndsWithLower：小写文本是否以小写后缀结尾。
        bool EndsWithLower(const std::string& lowerText, const std::string& lowerSuffix)
        {
            return lowerText.size() >= lowerSuffix.size()
                && lowerText.compare(lowerText.size() - lowerSuffix.size(), lowerSuffix.size(), lowerSuffix) == 0;
        }
    }

    // primaryModule：见头文件。记录按基址升序（目录的约定），所以"最低基址"就是第一条/第一个符合条件的。
    WorkbenchTarget::PrimaryModuleResult WorkbenchTarget::primaryModule(const QString& processNameHint) const
    {
        PrimaryModuleResult result;
        const ksword::memwb::MemoryTargetSession& session = tracker_.Session();
        // 物理范围没有模块；进程范围还没附加进程时也没有。
        if (session.scope == ksword::memwb::Scope::Physical
            || (session.scope == ksword::memwb::Scope::ProcessVirtual && session.pid == 0))
        {
            return result;
        }
        const bool kernel = session.scope == ksword::memwb::Scope::KernelVirtual;
        const ksword::memwb::MemoryModuleDirectory& directory = kernel ? kernelDirectory_ : processDirectory_;
        // 所有者核对用宽松比较：创建时间未锚定（0）的进程也能用，但 pid 必须一致，绝不拿别人的基址作答。
        const ksword::memwb::ModuleOwner owner = ksword::memwb::ModuleOwnerForSession(session);
        if (!ksword::memwb::IsSameModuleOwner(directory.Owner(), owner))
        {
            return result;
        }
        if (directory.GetState() == ksword::memwb::MemoryModuleDirectory::State::Loading)
        {
            result.state = PrimaryModuleState::Loading;
            return result;
        }
        const std::vector<ksword::memwb::ModuleRecord>& records = directory.Records();
        if (directory.GetState() != ksword::memwb::MemoryModuleDirectory::State::Ready || records.empty())
        {
            return result;
        }

        // wanted：优先按名字命中的模块名（小写）。内核固定 ntoskrnl.exe；进程用宿主给的进程名。
        const std::string wanted = kernel ? std::string("ntoskrnl.exe") : LowerAscii(processNameHint.toUtf8().toStdString());
        const ksword::memwb::ModuleRecord* chosen = nullptr;
        if (!wanted.empty())
        {
            for (const ksword::memwb::ModuleRecord& record : records)
            {
                if (LowerAscii(record.name) == wanted)
                {
                    chosen = &record;
                    break;
                }
            }
        }
        // 进程范围没按名字命中：取名字以 .exe 结尾的最低基址模块。
        if (chosen == nullptr && !kernel)
        {
            for (const ksword::memwb::ModuleRecord& record : records)
            {
                if (EndsWithLower(LowerAscii(record.name), ".exe"))
                {
                    chosen = &record;
                    break;
                }
            }
        }
        // 最后兜底：最低基址的模块。
        if (chosen == nullptr)
        {
            chosen = &records.front();
        }
        result.state = PrimaryModuleState::Ready;
        result.record = *chosen;
        return result;
    }

    // ModuleEnumTask：一次模块枚举任务。setAutoDelete(true)（默认值），交给
    // QThreadPool 后由线程池在 run() 返回时自动释放，调用方不持有也不需要释放它。
    class WorkbenchTarget::ModuleEnumTask final : public QRunnable
    {
    public:
        // Kind：这次任务枚举的是进程模块还是内核模块。
        enum class Kind
        {
            Process,
            Kernel,
        };

        // 构造：把这次枚举需要的一切都按值/独立引用计数拷贝进来，run() 执行期间
        // 不再读写 WorkbenchTarget 的任何成员（直到结果需要落地才通过 target_ 间接访问）。
        ModuleEnumTask(
            QPointer<WorkbenchTarget> target,
            std::shared_ptr<IWorkbenchServices> services,
            Kind kind,
            ksword::memwb::ModuleOwner owner,
            std::uint64_t ticket,
            std::uint32_t pid,
            std::uint64_t expectCreateTime)
            : target_(std::move(target))
            , services_(std::move(services))
            , kind_(kind)
            , owner_(owner)
            , ticket_(ticket)
            , pid_(pid)
            , expectCreateTime_(expectCreateTime)
        {
            setAutoDelete(true);
        }

        // run：QThreadPool 的工作线程调用。先做真正的阻塞枚举（这正是把它扔到
        // 工作线程的原因），再把结果安全地投递回 UI 线程。
        void run() override
        {
            ModuleEnumResult result = (kind_ == Kind::Process)
                ? services_->enumerateProcessModules(pid_, expectCreateTime_)
                : services_->enumerateKernelModules();

            // QPointer 可以从任意线程安全读取（失效通知走 Qt 内部的原子守卫），
            // 这里做一次廉价的"目标显然已经不在了"短路，省掉一次不会有结果的
            // 排队调用；这是本函数里唯一允许出现的对 target_ 的"读"，且只用来
            // 判空，不会解引用（D1 修复：旧版本在这个短路之外又额外取出了一次
            // 裸指针用作 invokeMethod 的 context，那才是真正的 UAF 来源）。
            if (!target_)
            {
                return;
            }

            // D1 修复：context 用进程内长期存活的 QCoreApplication::instance()，
            // 不用从 QPointer 取出的裸指针——真正的目标只以 QPointer 的值（不是
            // data() 取出的裸指针）被捕获进 lambda，等真正落到 UI 线程执行时才
            // 重新判空取用，那一刻才是安全的读取时机。
            const QPointer<WorkbenchTarget> targetPointer = target_;
            const ksword::memwb::ModuleOwner owner = owner_;
            const std::uint64_t ticket = ticket_;
            const Kind kind = kind_;
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                [targetPointer, owner, ticket, kind, movedResult = std::move(result)]() mutable
                {
                    // 已经在 UI 线程：现在才取 data() 判空是安全的——QPointer 的
                    // 失效通知与这段 lambda 同在 UI 线程，不存在竞态窗口。若
                    // WorkbenchTarget 在排队期间已经被销毁，target 为空，直接
                    // 丢弃这次迟到的结果（这是设计好的正常路径，不是缺陷）。
                    WorkbenchTarget* target = targetPointer.data();
                    if (!target)
                    {
                        return;
                    }
                    if (kind == Kind::Process)
                    {
                        target->handleProcessModulesReady(owner, ticket, std::move(movedResult));
                    }
                    else
                    {
                        target->handleKernelModulesReady(owner, ticket, std::move(movedResult));
                    }
                },
                Qt::QueuedConnection);
        }

    private:
        QPointer<WorkbenchTarget> target_;
        std::shared_ptr<IWorkbenchServices> services_;
        Kind kind_;
        ksword::memwb::ModuleOwner owner_;
        std::uint64_t ticket_;
        std::uint32_t pid_;
        std::uint64_t expectCreateTime_;
    };

    void WorkbenchTarget::refreshProcessModules(bool force)
    {
        const ksword::memwb::MemoryTargetSession& current = tracker_.Session();
        if (current.scope != ksword::memwb::Scope::ProcessVirtual || current.pid == 0)
        {
            return; // 没有进程目标，没什么可刷新的。
        }
        if (!force)
        {
            const ksword::memwb::MemoryModuleDirectory::State state = processDirectory_.GetState();
            if (state == ksword::memwb::MemoryModuleDirectory::State::Ready
                || state == ksword::memwb::MemoryModuleDirectory::State::Loading)
            {
                return; // 懒加载：已经有可用或正在加载的结果，不重复发起。
            }
        }

        // BeginLoad 同步执行：调用返回后目录立刻处于"为新所有者 Loading"的状态，
        // 旧票据（哪怕它的异步结果还没回来）此后一律被 Commit/Fail 拒绝。
        const ksword::memwb::ModuleOwner owner = ksword::memwb::ModuleOwnerForSession(current);
        const std::uint64_t ticket = ++processModuleTicket_;
        processDirectory_.BeginLoad(owner, ticket);

        auto* task = new ModuleEnumTask(
            QPointer<WorkbenchTarget>(this),
            services_,
            ModuleEnumTask::Kind::Process,
            owner,
            ticket,
            current.pid,
            current.processCreateTime100ns);
        QThreadPool::globalInstance()->start(task);
    }

    void WorkbenchTarget::refreshKernelModules(bool force)
    {
        if (!force)
        {
            const ksword::memwb::MemoryModuleDirectory::State state = kernelDirectory_.GetState();
            if (state == ksword::memwb::MemoryModuleDirectory::State::Ready
                || state == ksword::memwb::MemoryModuleDirectory::State::Loading)
            {
                return;
            }
        }

        const ksword::memwb::ModuleOwner owner{ksword::memwb::Scope::KernelVirtual, 0, 0};
        const std::uint64_t ticket = ++kernelModuleTicket_;
        kernelDirectory_.BeginLoad(owner, ticket);

        auto* task = new ModuleEnumTask(
            QPointer<WorkbenchTarget>(this),
            services_,
            ModuleEnumTask::Kind::Kernel,
            owner,
            ticket,
            0,
            0);
        QThreadPool::globalInstance()->start(task);
    }

    void WorkbenchTarget::handleProcessModulesReady(
        const ksword::memwb::ModuleOwner& owner,
        std::uint64_t ticket,
        ModuleEnumResult result)
    {
        if (result.ok)
        {
            // Commit 返回 false 意味着这是一个陈旧票据或所有者已经不符
            // （期间又发生了一次新的身份变更），按设计静默丢弃，不是缺陷。
            if (processDirectory_.Commit(owner, ticket, std::move(result.records)))
            {
                emit modulesChanged(false);
            }
        }
        else
        {
            // 可疑点 #3 修复：Fail 现在也会发出 modulesFailed（仅当 Fail() 真的
            // 被接受——不是陈旧票据/所有者不符被静默丢弃的那种），界面"加载中"
            // 的指示灯可以直接订阅它来熄灭，不必自己去轮询 GetState()==Failed。
            if (processDirectory_.Fail(owner, ticket, result.failure))
            {
                emit modulesFailed(false);
            }
        }
    }

    void WorkbenchTarget::handleKernelModulesReady(
        const ksword::memwb::ModuleOwner& owner,
        std::uint64_t ticket,
        ModuleEnumResult result)
    {
        if (result.ok)
        {
            if (kernelDirectory_.Commit(owner, ticket, std::move(result.records)))
            {
                emit modulesChanged(true);
            }
        }
        else
        {
            if (kernelDirectory_.Fail(owner, ticket, result.failure))
            {
                emit modulesFailed(true);
            }
        }
    }
}
