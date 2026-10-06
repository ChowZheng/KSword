// memwb_wpI_tests.Evaluate.cpp
// 作用：验证 WorkbenchTarget::evaluate 对 SessionAddressResolver::EvaluateForSession
//       的接线——逐个走到 NoTarget / ModulesLoading / ScopeHasNoModules /
//       DerefDeniedScope / DerefDeniedDdma / NotFound / FoundInKernelOnly /
//       建议切换范围 / 解引用成功 这几条 Issue 分支；以及 matchProcess 对
//       services.processCandidates() 的接线（薄封装，不重复 Core 已有的穷尽测试）。

#include "memwb_wpI_common.h"

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchTarget.h"

#include <memory>

namespace memwb_wpI_test
{
    namespace
    {
        ksword::memwb::ModuleRecord MakeModuleRecord(const char* name, std::uint64_t base)
        {
            ksword::memwb::ModuleRecord record;
            record.name = name;
            record.fullPath = name;
            record.base = base;
            record.size = 0x1000;
            return record;
        }

        // MakeApprovingTarget：构造一个干净的 WorkbenchTarget，离开守卫恒允许
        // （用于本文件专注测 evaluate 本身，不是测守卫）。
        std::unique_ptr<ks::ui::WorkbenchTarget> MakeApprovingTarget(
            FakeWorkbenchServices** outFake, GuardRecorder* outGuard)
        {
            auto servicesOwned = std::make_unique<FakeWorkbenchServices>();
            FakeWorkbenchServices* fake = servicesOwned.get();
            auto target = std::make_unique<ks::ui::WorkbenchTarget>(std::move(servicesOwned));
            outGuard->SetApproval(true);
            target->setLeaveGuard(outGuard->asFunction());
            if (outFake)
            {
                *outFake = fake;
            }
            return target;
        }
    }

    void RunEvaluateTests()
    {
        // ---- NoTarget：没有选目标时，数字表达式与模块表达式都应落在 NoTarget ----
        {
            GuardRecorder guard;
            FakeWorkbenchServices* fake = nullptr;
            auto target = MakeApprovingTarget(&fake, &guard);

            const ksword::memwb::AddressEval numeric = target->evaluate(QStringLiteral("0x1000"));
            WPI_CHECK(numeric.expr.ok);             // 纯数字求值本身成功
            WPI_CHECK(!numeric.inScopeSpace);
            WPI_CHECK(numeric.issue == ksword::memwb::Issue::NoTarget);

            const ksword::memwb::AddressEval withModule = target->evaluate(QStringLiteral("foo.dll"));
            WPI_CHECK(!withModule.expr.ok);          // 模块名求值在 LookupModule 第一关就被拦下
            WPI_CHECK(withModule.issue == ksword::memwb::Issue::NoTarget);
        }

        // ---- ModulesLoading：BeginLoad 同步发生，异步结果还没落回 UI 线程之前 ----
        {
            GuardRecorder guard;
            FakeWorkbenchServices* fake = nullptr;
            auto target = MakeApprovingTarget(&fake, &guard);
            constexpr std::uint32_t pidY = 2001;
            fake->SetProcessModulesResult(pidY, ks::ui::ModuleEnumResult{true, {}, {}});

            // 用 AttachFakeProcess（onDockAttached）而不是 requestPin：pidY 是编的
            // 号码不对应真实进程，D6 修复给 requestPin 加了存在性校验后不能再
            // 这样用；本文件只关心 evaluate() 的接线，不关心 pin/锚点语义。
            AttachFakeProcess(*target, pidY, 1);
            // 刻意不调用任何事件循环：BeginLoad 在 onDockAttached 内部同步执行，此刻
            // 工作线程的结果纵使已经算完也还排在队列里，没有机会落地。
            const ksword::memwb::AddressEval duringLoad = target->evaluate(QStringLiteral("foo.dll"));
            WPI_CHECK(duringLoad.issue == ksword::memwb::Issue::ModulesLoading);

            // 收尾：让这次枚举真正跑完，避免把悬空的异步任务留到夹具退出。
            PumpUntil(
                [&] { return target->evaluate(QStringLiteral("foo.dll")).issue != ksword::memwb::Issue::ModulesLoading; },
                2000);
        }

        // ---- NotFound 与 FoundInKernelOnly：进程目录就绪但没有，内核目录里有唯一命中 ----
        {
            GuardRecorder guard;
            FakeWorkbenchServices* fake = nullptr;
            auto target = MakeApprovingTarget(&fake, &guard);
            constexpr std::uint32_t pidZ = 2002;
            fake->SetProcessModulesResult(pidZ, ks::ui::ModuleEnumResult{true, {}, {}});
            fake->SetKernelModulesResult(ks::ui::ModuleEnumResult{true, {MakeModuleRecord("CI.dll", 0x5000)}, {}});

            AttachFakeProcess(*target, pidZ, 1); // pidZ 是编的号码，见上面 pidY 处的说明。
            const bool processReady = PumpUntil(
                [&] { return target->evaluate(QStringLiteral("zzz_absent.dll")).issue == ksword::memwb::Issue::NotFound; },
                2000);
            WPI_CHECK_NOTE(processReady, QStringLiteral("等待进程模块目录就绪超时"));

            // 切到内核范围触发一次懒加载，等它就绪（用 CI.dll 本身能否解出作判据）。
            WPI_CHECK(target->requestScope(ksword::memwb::Scope::KernelVirtual));
            const bool kernelReady = PumpUntil(
                [&] { return target->evaluate(QStringLiteral("CI.dll")).expr.ok; }, 2000);
            WPI_CHECK_NOTE(kernelReady, QStringLiteral("等待内核模块目录就绪超时"));

            // 切回进程范围：pid 恢复会再触发一次进程模块刷新，先等它重新就绪。
            WPI_CHECK(target->requestScope(ksword::memwb::Scope::ProcessVirtual));
            const bool processReadyAgain = PumpUntil(
                [&] { return target->evaluate(QStringLiteral("zzz_absent.dll")).issue == ksword::memwb::Issue::NotFound; },
                2000);
            WPI_CHECK_NOTE(processReadyAgain, QStringLiteral("等待进程模块目录重新就绪超时"));

            const ksword::memwb::AddressEval foundInKernel = target->evaluate(QStringLiteral("CI.dll"));
            WPI_CHECK(!foundInKernel.expr.ok);
            WPI_CHECK(foundInKernel.issue == ksword::memwb::Issue::FoundInKernelOnly);
            WPI_CHECK(foundInKernel.detail == QStringLiteral("0x5000").toStdString());
        }

        // ---- ScopeHasNoModules 与 DerefDeniedScope：物理范围 ----
        {
            GuardRecorder guard;
            FakeWorkbenchServices* fake = nullptr;
            auto target = MakeApprovingTarget(&fake, &guard);

            WPI_CHECK(target->requestScope(ksword::memwb::Scope::Physical));
            const ksword::memwb::AddressEval moduleEval = target->evaluate(QStringLiteral("foo.dll"));
            WPI_CHECK(moduleEval.issue == ksword::memwb::Issue::ScopeHasNoModules);

            const ksword::memwb::AddressEval derefEval = target->evaluate(QStringLiteral("[0x1000]"));
            WPI_CHECK(derefEval.issue == ksword::memwb::Issue::DerefDeniedScope);
            WPI_CHECK(fake->ReadPointerCallCount() == 0); // 门控拦下，读取器一次都没被调用
        }

        // ---- DerefDeniedDdma：进程范围 + Ddma 通道 ----
        {
            GuardRecorder guard;
            FakeWorkbenchServices* fake = nullptr;
            auto target = MakeApprovingTarget(&fake, &guard);
            constexpr std::uint32_t pidW = 2003;
            fake->SetProcessModulesResult(pidW, ks::ui::ModuleEnumResult{true, {}, {}});

            AttachFakeProcess(*target, pidW, 1); // pidW 是编的号码，见上面 pidY 处的说明。
            fake->SetDdmaGeneration(1); // D5 修复后代次改由 requestChannel 自己向 services_ 取。
            WPI_CHECK(target->requestChannel(ksword::memwb::Channel::Ddma));
            const ksword::memwb::AddressEval derefEval = target->evaluate(QStringLiteral("[0x1000]"));
            WPI_CHECK(derefEval.issue == ksword::memwb::Issue::DerefDeniedDdma);
            WPI_CHECK(fake->ReadPointerCallCount() == 0);
        }

        // ---- 成功路径：建议切换范围、解引用成功 ----
        {
            GuardRecorder guard;
            FakeWorkbenchServices* fake = nullptr;
            auto target = MakeApprovingTarget(&fake, &guard);
            constexpr std::uint32_t pidV = 2004;
            fake->SetProcessModulesResult(pidV, ks::ui::ModuleEnumResult{true, {}, {}});
            fake->SetPointerReadResult(0x2000, ks::ui::PointerReadResult{true, 0x3000, {}});

            AttachFakeProcess(*target, pidV, 1); // pidV 是编的号码，见上面 pidY 处的说明。

            // 进程范围下输入内核半区地址：求值成功但越界，应建议切到内核范围，不静默改范围。
            const ksword::memwb::AddressEval needsKernel =
                target->evaluate(QStringLiteral("0xFFFF800000000000"));
            WPI_CHECK(needsKernel.expr.ok);
            WPI_CHECK(!needsKernel.inScopeSpace);
            WPI_CHECK(needsKernel.suggestScope.has_value());
            WPI_CHECK(needsKernel.suggestScope == ksword::memwb::Scope::KernelVirtual);
            WPI_CHECK(target->session().scope == ksword::memwb::Scope::ProcessVirtual); // 范围没有被静默改掉

            // 解引用成功：读取器被调用恰好一次，取到的值落在进程地址空间内。
            const ksword::memwb::AddressEval derefOk = target->evaluate(QStringLiteral("[0x2000]"));
            WPI_CHECK(derefOk.Accepted());
            WPI_CHECK(derefOk.expr.value == 0x3000);
            WPI_CHECK(fake->ReadPointerCallCount() == 1);
        }

        // ---- matchProcess：薄封装接线的烟雾测试（穷尽规则已在 Core 测过） ----
        {
            GuardRecorder guard;
            FakeWorkbenchServices* fake = nullptr;
            auto target = MakeApprovingTarget(&fake, &guard);
            fake->SetProcessCandidates({
                {100, "notepad.exe"},
                {200, "notepad.exe"},
                {300, "chrome.exe"},
            });

            WPI_CHECK(target->matchProcess(QStringLiteral("")).kind == ksword::memwb::ProcessMatchKind::Empty);

            const ksword::memwb::ProcessMatchResult byPid = target->matchProcess(QStringLiteral("300"));
            WPI_CHECK(byPid.kind == ksword::memwb::ProcessMatchKind::Unique);
            WPI_CHECK(byPid.pid == 300);

            const ksword::memwb::ProcessMatchResult duplicate = target->matchProcess(QStringLiteral("notepad.exe"));
            WPI_CHECK(duplicate.kind == ksword::memwb::ProcessMatchKind::Multiple);
            WPI_CHECK(duplicate.candidates.size() == 2);

            const ksword::memwb::ProcessMatchResult substring = target->matchProcess(QStringLiteral("chrome"));
            WPI_CHECK(substring.kind == ksword::memwb::ProcessMatchKind::Unique);
            WPI_CHECK(substring.pid == 300);
        }
    }
}
