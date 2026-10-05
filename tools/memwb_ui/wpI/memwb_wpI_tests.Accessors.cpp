// memwb_wpI_tests.Accessors.cpp
// 作用：补齐几个修复波新增接口/判据自己的覆盖缺口（这些缺口是我在变异重放前
// 自查发现的，不是审核报告列出的 32 个变异，用来支撑"自己补的 8 个新变异"）：
// 1) D6 两条前置校验本身——目标已不存在（TargetGone）、创建时间不符
//    （TargetMismatch）——此前只验证了"存在且匹配"的正常路径；
// 2) identityAnchored() 访问器（可疑点 #5 新增）；
// 3) lastIdentityFailure() 在下一次成功调用后应该复位为 Ok；
// 4) modulesFailed 信号（可疑点 #3 新增，内核枚举失败路径）；
// 5) checkLiveness 的范围门（可疑点 #8）；
// 6) onDockAboutToDetach 在"从未附加过"时不应该发信号（可疑点 #2 的另一半）。

#include "memwb_wpI_common.h"

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchTarget.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <memory>
#include <string>
#include <vector>

namespace memwb_wpI_test
{
    void RunAccessorTests()
    {
        // ---- D6 回归之一：钉住一个几乎可以确定不存在的 pid 必须直接失败
        // （TargetGone），且不应该问到离开守卫 ----
        {
            auto owned = std::make_unique<FakeWorkbenchServices>();
            ks::ui::WorkbenchTarget target(std::move(owned));
            GuardRecorder guard;
            guard.SetApproval(true);
            target.setLeaveGuard(guard.asFunction());

            constexpr std::uint32_t kAlmostCertainlyNonexistentPid = 0x7FFFFFF0; // 同审核报告 P15 探针用值。
            const auto before = target.session();
            WPI_CHECK(!target.requestPin(kAlmostCertainlyNonexistentPid));
            WPI_CHECK_NOTE(
                target.lastIdentityFailure() == ks::ui::NavStatus::TargetGone,
                QStringLiteral("D6：钉住不存在的 pid 应该报告 TargetGone"));
            WPI_CHECK_NOTE(guard.CallCount() == 0, QStringLiteral("D6：目标不存在时不应该问到离开守卫"));
            WPI_CHECK(ksword::memwb::SameTarget(before, target.session()));
        }

        // ---- D6 回归之二：钉住一个真实存在的进程，但期望创建时间故意写错，
        // 必须报告 TargetMismatch 且不问守卫 ----
        {
            STARTUPINFOW startupInfo{};
            startupInfo.cb = sizeof(startupInfo);
            PROCESS_INFORMATION processInfo{};
            std::wstring commandLine = L"cmd.exe /c ping -n 10 127.0.0.1";
            std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
            mutableCommandLine.push_back(L'\0');
            const BOOL spawned = ::CreateProcessW(
                nullptr, mutableCommandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                nullptr, nullptr, &startupInfo, &processInfo);
            WPI_CHECK_NOTE(spawned != FALSE, QStringLiteral("D6 回归之二需要能够拉起 cmd.exe"));
            if (spawned != FALSE)
            {
                const ks::ui::AnchorInfo realAnchor = ks::ui::AcquireAnchorForPid(processInfo.dwProcessId);
                WPI_CHECK(!realAnchor.identityWeak);
                ks::ui::ReleaseAnchorHandle(realAnchor.handle);

                auto owned = std::make_unique<FakeWorkbenchServices>();
                ks::ui::WorkbenchTarget target(std::move(owned));
                GuardRecorder guard;
                guard.SetApproval(true);
                target.setLeaveGuard(guard.asFunction());

                const std::uint64_t wrongCreateTime = realAnchor.createTime100ns + 1;
                WPI_CHECK(!target.requestPin(processInfo.dwProcessId, wrongCreateTime));
                WPI_CHECK_NOTE(
                    target.lastIdentityFailure() == ks::ui::NavStatus::TargetMismatch,
                    QStringLiteral("D6：创建时间不符应该报告 TargetMismatch"));
                WPI_CHECK_NOTE(guard.CallCount() == 0, QStringLiteral("D6：创建时间不符时不应该问到离开守卫"));

                // 期望创建时间填对就应该成功。
                WPI_CHECK(target.requestPin(processInfo.dwProcessId, realAnchor.createTime100ns));
                WPI_CHECK(target.lastIdentityFailure() == ks::ui::NavStatus::Ok);

                ::TerminateProcess(processInfo.hProcess, 0);
                ::CloseHandle(processInfo.hProcess);
                ::CloseHandle(processInfo.hThread);
            }
        }

        // ---- identityAnchored()：无目标/内核范围为 false，真实锚定的进程范围为 true ----
        {
            auto owned = std::make_unique<FakeWorkbenchServices>();
            ks::ui::WorkbenchTarget target(std::move(owned));
            WPI_CHECK(!target.identityAnchored()); // 刚构造：无目标。

            ks::ui::WorkbenchTarget::DockAttach attach;
            attach.handle = reinterpret_cast<void*>(::GetCurrentProcess());
            attach.pid = ::GetCurrentProcessId();
            attach.attachGeneration = 1;
            target.onDockAttached(attach);
            WPI_CHECK(target.identityAnchored()); // 本进程锚点真实可用。

            WPI_CHECK(target.requestScope(ksword::memwb::Scope::KernelVirtual));
            WPI_CHECK_NOTE(
                !target.identityAnchored(),
                QStringLiteral("内核范围下 pid 恒为 0，身份不应该被认为已锚定"));
        }

        // ---- lastIdentityFailure()：下一次成功调用后应该复位为 Ok ----
        {
            auto owned = std::make_unique<FakeWorkbenchServices>();
            ks::ui::WorkbenchTarget target(std::move(owned));
            WPI_CHECK(!target.requestPin(0x7FFFFFF0));
            WPI_CHECK(target.lastIdentityFailure() == ks::ui::NavStatus::TargetGone);

            WPI_CHECK(target.requestScope(ksword::memwb::Scope::KernelVirtual)); // 一次正常成功的请求。
            WPI_CHECK_NOTE(
                target.lastIdentityFailure() == ks::ui::NavStatus::Ok,
                QStringLiteral("上一次失败的原因不应该残留到下一次成功调用之后"));
        }

        // ---- modulesFailed：内核枚举失败也应该发出信号 ----
        {
            auto owned = std::make_unique<FakeWorkbenchServices>();
            FakeWorkbenchServices* fake = owned.get();
            fake->SetKernelModulesResult(ks::ui::ModuleEnumResult{false, {}, "fake kernel enum failure"});
            ks::ui::WorkbenchTarget target(std::move(owned));
            int kernelFailedCount = 0;
            QObject::connect(&target, &ks::ui::WorkbenchTarget::modulesFailed,
                [&](bool kernel) { if (kernel) { ++kernelFailedCount; } });

            WPI_CHECK(target.requestScope(ksword::memwb::Scope::KernelVirtual));
            const bool failed = PumpUntil([&] { return kernelFailedCount >= 1; }, 2000);
            WPI_CHECK_NOTE(failed, QStringLiteral("内核模块枚举失败时应该发出 modulesFailed(true)"));
        }

        // ---- checkLiveness 的范围门：内核范围下不应该探测 Dock 当前附着的进程 ----
        {
            auto owned = std::make_unique<FakeWorkbenchServices>();
            ks::ui::WorkbenchTarget target(std::move(owned));
            int lastState = -1;
            QObject::connect(&target, &ks::ui::WorkbenchTarget::livenessChanged,
                [&](int state) { lastState = state; });

            ks::ui::WorkbenchTarget::DockAttach attach;
            attach.handle = reinterpret_cast<void*>(::GetCurrentProcess());
            attach.pid = ::GetCurrentProcessId();
            attach.attachGeneration = 1;
            target.onDockAttached(attach);
            target.checkLiveness();
            WPI_CHECK(lastState == static_cast<int>(ks::ui::LivenessState::Alive));

            WPI_CHECK(target.requestScope(ksword::memwb::Scope::KernelVirtual));
            target.checkLiveness();
            WPI_CHECK_NOTE(
                lastState == static_cast<int>(ks::ui::LivenessState::Unknown),
                QStringLiteral("内核范围下存活探测应该报告 Unknown，不应该探测 Dock 当前附着的进程"));
        }

        // ---- onDockAboutToDetach：从未附加过任何进程时不应该发信号 ----
        {
            auto owned = std::make_unique<FakeWorkbenchServices>();
            ks::ui::WorkbenchTarget target(std::move(owned));
            bool fired = false;
            QObject::connect(&target, &ks::ui::WorkbenchTarget::aboutToDetach, [&]() { fired = true; });
            target.onDockAboutToDetach();
            WPI_CHECK_NOTE(!fired, QStringLiteral("从未附加过任何进程时 onDockAboutToDetach 不应该发信号"));
        }
    }
}
