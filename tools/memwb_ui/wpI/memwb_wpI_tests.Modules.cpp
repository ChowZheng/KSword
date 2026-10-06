// memwb_wpI_tests.Modules.cpp
// 作用：验证 WorkbenchTarget 异步模块枚举的两条安全性质：
// 1) 陈旧票据丢弃——先发起的枚举如果后完成，不能覆盖期间已经提交的新结果；
// 2) 所有者不串——钉住进程 A 得到的模块记录绝不会被进程 B 的会话解析命中
//    （旧缺陷 N-02：预览进程的缓存被已附加进程借用）。本设计对每次身份变更都
//    同步 BeginLoad 新所有者，结构上不会再出现 MemoryModuleDirectory::Lookup::
//    Kind::OwnerMismatch（目录所有者与会话所有者不一致）这个状态本身——能观察到
//    的是"仍在加载"（ModulesLoading）或"就绪但没有这个模块"（NotFound），
//    两者都不会把 A 的基址答给 B，这正是本测试要钉住的安全性质。
// 另附：范围刚进入内核时的"懒加载一次"——已经 Ready 时不重复发起。

#include "memwb_wpI_common.h"

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchTarget.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QThread>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <memory>

namespace memwb_wpI_test
{
    namespace
    {
        // MakeModuleRecord：拼一条最简模块记录，只关心 name 与 base。
        ksword::memwb::ModuleRecord MakeModuleRecord(const char* name, std::uint64_t base)
        {
            ksword::memwb::ModuleRecord record;
            record.name = name;
            record.fullPath = name;
            record.base = base;
            record.size = 0x1000;
            return record;
        }
    }

    void RunModulesTests()
    {
        constexpr std::uint32_t kPidA = 1001;
        constexpr std::uint32_t kPidB = 1002;

        auto servicesOwned = std::make_unique<FakeWorkbenchServices>();
        FakeWorkbenchServices* fake = servicesOwned.get();

        // A 先发起但故意拖延完成（150ms）；B 后发起但很快完成（5ms）。
        std::vector<ksword::memwb::ModuleRecord> recordsA{MakeModuleRecord("onlyA.dll", 0x1000)};
        std::vector<ksword::memwb::ModuleRecord> recordsB{MakeModuleRecord("onlyB.dll", 0x2000)};
        fake->SetProcessModulesResult(kPidA, ks::ui::ModuleEnumResult{true, recordsA, {}});
        fake->SetProcessEnumDelayMs(kPidA, 150);
        fake->SetProcessModulesResult(kPidB, ks::ui::ModuleEnumResult{true, recordsB, {}});
        fake->SetProcessEnumDelayMs(kPidB, 5);

        ks::ui::WorkbenchTarget target(std::move(servicesOwned));
        GuardRecorder guard;
        guard.SetApproval(true);
        target.setLeaveGuard(guard.asFunction());

        int modulesChangedCount = 0;
        QObject::connect(&target, &ks::ui::WorkbenchTarget::modulesChanged,
            [&](bool kernel) { if (!kernel) { ++modulesChangedCount; } });

        // 先让 Dock"附加"A（发起 ticket1，150ms 后才会完成），几乎立刻再"附加"B
        // （发起 ticket2，5ms 后完成）：B 的 BeginLoad 在 A 的工作线程任务还没跑完
        // 时就已经同步执行，所有者立刻变成 B；A 的结果回来时票据已经不是目录当前
        // 登记的那个。这里用 AttachFakeProcess（onDockAttached）而不是 requestPin：
        // kPidA/kPidB 是纯粹为了区分模块目录所有者而编的号码，不对应真实进程，
        // D6 修复给 requestPin 加了存在性校验后不能再这样用；本测试关心的是模块
        // 目录的陈旧票据/所有者逻辑，不是 pin/锚点语义，onDockAttached 不受影响。
        AttachFakeProcess(target, kPidA, 1);
        AttachFakeProcess(target, kPidB, 2);

        // 等到至少有一次 modulesChanged（B 的提交）落地。
        const bool bLanded = PumpUntil([&] { return modulesChangedCount >= 1; }, 2000);
        WPI_CHECK_NOTE(bLanded, QStringLiteral("等待 B 的模块提交落地超时"));

        // 再多等一段，确保 A 那个延迟 150ms 的结果也有机会被工作线程送回来
        // （即使被丢弃，也要等它真的跑完，才能断言"丢弃"而不是"还没跑到"）。
        QThread::msleep(250);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

        // 安全性质：无论此刻目录是 Ready（B）还是仍在为 B Loading，解析 A 独有的
        // 模块名绝不能得到 A 的基址；B 独有的模块名如果已经 Ready 则必须得到 B 的基址。
        const ksword::memwb::AddressEval evalOnlyA = target.evaluate(QStringLiteral("onlyA.dll"));
        WPI_CHECK(!(evalOnlyA.expr.ok && evalOnlyA.expr.value == 0x1000)); // 绝不是 A 的基址

        const ksword::memwb::AddressEval evalOnlyB = target.evaluate(QStringLiteral("onlyB.dll"));
        if (evalOnlyB.expr.ok)
        {
            WPI_CHECK(evalOnlyB.expr.value == 0x2000); // 若已解出，必须是 B 的基址，不能是别的
        }

        // 多等一轮事件循环，确保目录最终稳定在"就绪且是 B"（A 的陈旧结果确认被丢弃）。
        const bool settledOnB = PumpUntil(
            [&] { return target.evaluate(QStringLiteral("onlyB.dll")).expr.ok; }, 2000);
        WPI_CHECK_NOTE(settledOnB, QStringLiteral("等待目录最终稳定到 B 的模块超时"));
        const ksword::memwb::AddressEval finalOnlyB = target.evaluate(QStringLiteral("onlyB.dll"));
        WPI_CHECK(finalOnlyB.expr.ok && finalOnlyB.expr.value == 0x2000);
        const ksword::memwb::AddressEval finalOnlyA = target.evaluate(QStringLiteral("onlyA.dll"));
        WPI_CHECK(!finalOnlyA.expr.ok || finalOnlyA.issue == ksword::memwb::Issue::NotFound);
        WPI_CHECK(fake->ProcessEnumCallCount() == 2); // 两次枚举都真的被发起了（不是没跑到）
        // T6（锁定 M14）：A 的陈旧结果被工作线程算完送回但被目录丢弃时，不应该
        // 再多发一次 modulesChanged——全程只有 B 的那次 Commit 是真正被接受的。
        WPI_CHECK_NOTE(
            modulesChangedCount == 1,
            QStringLiteral("A 的陈旧结果被丢弃时不应再发 modulesChanged，实际发了 %1 次")
                .arg(modulesChangedCount));
        // T6（锁定 M24）：期望创建时间确实一路传到了服务层，不是随手传了个 0——
        // B 是用 AttachFakeProcess 接上的，锚定句柄是本进程自己的伪句柄，所以
        // 会话里的 processCreateTime100ns 是本进程真实的创建时间（非零）。
        WPI_CHECK(fake->LastExpectCreateTime() == target.session().processCreateTime100ns);
        WPI_CHECK_NOTE(
            fake->LastExpectCreateTime() != 0,
            QStringLiteral("期望创建时间不应该恒为 0（本进程真实创建时间不可能是 0）"));

        // ---- 懒加载：第一次进入内核范围会发起一次；已经 Ready 时再次进入不重复发起 ----
        fake->SetKernelModulesResult(ks::ui::ModuleEnumResult{true, {MakeModuleRecord("ntoskrnl.exe", 0x3000)}, {}});
        int kernelModulesChangedCount = 0;
        QObject::connect(&target, &ks::ui::WorkbenchTarget::modulesChanged,
            [&](bool kernel) { if (kernel) { ++kernelModulesChangedCount; } });

        WPI_CHECK(target.requestScope(ksword::memwb::Scope::KernelVirtual));
        const bool kernelLanded = PumpUntil([&] { return kernelModulesChangedCount >= 1; }, 2000);
        WPI_CHECK_NOTE(kernelLanded, QStringLiteral("等待内核模块首次懒加载落地超时"));
        const int kernelCallsAfterFirstEntry = fake->KernelEnumCallCount();

        // 回到进程范围再切回内核：目录已经 Ready，不应该再发起一次枚举。
        WPI_CHECK(target.requestScope(ksword::memwb::Scope::ProcessVirtual));
        WPI_CHECK(target.requestScope(ksword::memwb::Scope::KernelVirtual));
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        WPI_CHECK(fake->KernelEnumCallCount() == kernelCallsAfterFirstEntry);

        // 显式调用 refreshModules() 则无条件强制重新发起一次。
        WPI_CHECK(kernelModulesChangedCount >= 1);
        target.refreshModules();
        const bool forcedLanded = PumpUntil(
            [&] { return fake->KernelEnumCallCount() == kernelCallsAfterFirstEntry + 1; }, 2000);
        WPI_CHECK_NOTE(forcedLanded, QStringLiteral("等待 refreshModules() 强制重新发起的内核枚举落地超时"));

        // ---- T6（锁定 M13）：进入物理范围不应该触发内核模块枚举 ----
        // 必须用一份全新的 target/fake：上面的 target 已经在 Kernel 范围里懒加载
        // 过一次并落地为 Ready，物理范围的内核目录"已经 Ready"这一事实本身就会
        // 让 refreshKernelModules(force=false) 的懒加载短路生效，把变异（物理
        // 范围也触发枚举）的效果掩盖掉——必须从 Empty 状态触发，才是真正测
        // "进入物理范围该不该发起"而不是"该不该重复发起"。
        {
            auto physicalOwned = std::make_unique<FakeWorkbenchServices>();
            FakeWorkbenchServices* physicalFake = physicalOwned.get();
            physicalFake->SetKernelModulesResult(ks::ui::ModuleEnumResult{true, {}, {}});
            ks::ui::WorkbenchTarget physicalTarget(std::move(physicalOwned));

            const int kernelCallsBeforePhysical = physicalFake->KernelEnumCallCount();
            WPI_CHECK(physicalTarget.requestScope(ksword::memwb::Scope::Physical));
            PumpUntil([] { return false; }, 200);
            WPI_CHECK_NOTE(
                physicalFake->KernelEnumCallCount() == kernelCallsBeforePhysical,
                QStringLiteral("进入物理范围（内核目录此前从未加载过）不应该发起内核模块枚举"));
        }

        // ---- T6（锁定 M16）：Dock 分离（pid 归零）不应该对 pid 0 发起进程模块枚举 ----
        // 用一个独立的全新 target，避免借用上面已经积累了一堆状态的 target。
        {
            auto detachOwned = std::make_unique<FakeWorkbenchServices>();
            FakeWorkbenchServices* detachFake = detachOwned.get();
            constexpr std::uint32_t kDetachPid = 4001;
            detachFake->SetProcessModulesResult(kDetachPid, ks::ui::ModuleEnumResult{true, {}, {}});
            ks::ui::WorkbenchTarget detachTarget(std::move(detachOwned));
            AttachFakeProcess(detachTarget, kDetachPid, 1);
            PumpUntil([&] { return detachFake->ProcessEnumCallCount() >= 1; }, 2000);

            const int callsBeforeDetach = detachFake->ProcessEnumCallCount();
            detachTarget.onDockDetached();
            PumpUntil([] { return false; }, 200); // 给可能误发的异步任务留出时间
            WPI_CHECK_NOTE(
                detachFake->ProcessEnumCallCount() == callsBeforeDetach,
                QStringLiteral("Dock 分离（pid 归零）不应该再对 pid 0 发起进程模块枚举"));
        }

        // ---- T6（锁定 M15）：同一个所有者重复刷新，先发起的旧票据绝不能覆盖
        // 后发起的新票据——哪怕旧票据对应的结果后来才算完送回来。----
        {
            auto raceOwned = std::make_unique<FakeWorkbenchServices>();
            FakeWorkbenchServices* raceFake = raceOwned.get();
            constexpr std::uint32_t kRacePid = 3001;
            raceFake->SetProcessModulesResult(
                kRacePid, ks::ui::ModuleEnumResult{true, {MakeModuleRecord("old.dll", 0x1000)}, {}});
            raceFake->SetProcessEnumDelayMs(kRacePid, 100);
            ks::ui::WorkbenchTarget raceTarget(std::move(raceOwned));

            AttachFakeProcess(raceTarget, kRacePid, 1); // 票据1：配置延迟 100ms，抓走 old.dll。
            QThread::msleep(30); // 让票据1的工作线程任务先真正开始执行（已经读到 old.dll/100ms 的配置）。
            raceFake->SetProcessModulesResult(
                kRacePid, ks::ui::ModuleEnumResult{true, {MakeModuleRecord("fresh.dll", 0x2000)}, {}});
            raceFake->SetProcessEnumDelayMs(kRacePid, 300);
            raceTarget.refreshModules(); // 票据2：同一个所有者强制重刷，配置延迟 300ms，抓走 fresh.dll。

            QThread::msleep(500);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            WPI_CHECK_NOTE(
                raceTarget.evaluate(QStringLiteral("fresh.dll")).expr.ok,
                QStringLiteral("后发起的票据2（fresh.dll）必须最终落地"));
            WPI_CHECK_NOTE(
                !raceTarget.evaluate(QStringLiteral("old.dll")).expr.ok,
                QStringLiteral("先发起的陈旧票据1（old.dll）绝不能覆盖后发起的票据2"));
        }
    }
}
