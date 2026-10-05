// memwb_wpI_tests.Guard.cpp
// 作用：验证 WorkbenchTarget 的离开守卫——否决时完全不调用 tracker；身份类变更
//       （范围/钉住/通道）分开请求时各问一次，合并到一次 requestIdentity 调用时
//       只问一次；策略门（lockToDock/allowKernelPhysical）在问守卫之前就拒绝。
// 每个场景各用一份独立的 WorkbenchTarget + FakeWorkbenchServices + GuardRecorder，
// 避免状态/计数互相干扰，让每个断言都对应唯一一个清楚的前因。

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
    namespace
    {
        // MakeTarget：构造一个干净的 WorkbenchTarget 与它的假服务，内核/进程模块
        // 枚举都配置成"立即成功、空列表"，避免异步枚举的细节干扰本文件关心的
        // 守卫/策略判定。
        std::unique_ptr<ks::ui::WorkbenchTarget> MakeTarget(FakeWorkbenchServices** outFake)
        {
            auto servicesOwned = std::make_unique<FakeWorkbenchServices>();
            FakeWorkbenchServices* fake = servicesOwned.get();
            const std::uint32_t selfPid = static_cast<std::uint32_t>(::GetCurrentProcessId());
            fake->SetProcessModulesResult(selfPid, ks::ui::ModuleEnumResult{true, {}, {}});
            fake->SetKernelModulesResult(ks::ui::ModuleEnumResult{true, {}, {}});
            auto target = std::make_unique<ks::ui::WorkbenchTarget>(std::move(servicesOwned));
            if (outFake)
            {
                *outFake = fake;
            }
            return target;
        }
    }

    void RunGuardTests()
    {
        const std::uint32_t selfPid = static_cast<std::uint32_t>(::GetCurrentProcessId());

        // ---- 场景一：守卫否决，tracker 完全不变 ----
        {
            auto target = MakeTarget(nullptr);
            GuardRecorder guard;
            guard.SetApproval(false);
            target->setLeaveGuard(guard.asFunction());

            const ksword::memwb::Scope before = target->session().scope;
            const bool changed = target->requestScope(ksword::memwb::Scope::KernelVirtual);
            WPI_CHECK(!changed);
            WPI_CHECK(guard.CallCount() == 1);
            WPI_CHECK(target->session().scope == before); // 仍是 ProcessVirtual，守卫否决后状态原样
        }

        // ---- 场景二：分开请求，各问一次（范围 + 通道 = 2 次独立询问） ----
        {
            auto target = MakeTarget(nullptr);
            GuardRecorder guard;
            guard.SetApproval(true);
            target->setLeaveGuard(guard.asFunction());

            WPI_CHECK(target->requestScope(ksword::memwb::Scope::KernelVirtual));
            WPI_CHECK(guard.CallCount() == 1);
            WPI_CHECK(target->requestChannel(ksword::memwb::Channel::StandardDriver));
            WPI_CHECK(guard.CallCount() == 2); // 第二次独立调用又问了一次，合计 2 次
        }

        // ---- 场景三：同一次 requestIdentity 合并范围+通道两个维度，只问一次 ----
        {
            auto target = MakeTarget(nullptr);
            GuardRecorder guard;
            guard.SetApproval(true);
            target->setLeaveGuard(guard.asFunction());

            ks::ui::IdentityRequest combined;
            combined.scope = ksword::memwb::Scope::KernelVirtual;   // 相对默认 ProcessVirtual 确实会变
            combined.channel = ksword::memwb::Channel::StandardDriver; // 相对默认 UserMode 确实会变
            const bool ok = target->requestIdentity(combined, ks::ui::LeaveReason::ScopeChange);
            WPI_CHECK(ok);
            WPI_CHECK_NOTE(
                guard.CallCount() == 1,
                QStringLiteral("范围+通道两个维度一起变更应当只问一次离开守卫，实际问了 %1 次")
                    .arg(guard.CallCount()));
            WPI_CHECK(target->session().scope == ksword::memwb::Scope::KernelVirtual);
            WPI_CHECK(target->session().channel == ksword::memwb::Channel::StandardDriver);
        }

        // ---- 场景四：策略门先于离开守卫拒绝，不问守卫 ----
        {
            auto target = MakeTarget(nullptr);
            GuardRecorder guard;
            guard.SetApproval(true);
            target->setLeaveGuard(guard.asFunction());

            ks::ui::WorkbenchTarget::Policy policy;
            policy.lockToDock = true;
            policy.allowKernelPhysical = false;
            target->setPolicy(policy);

            const bool pinBlocked = target->requestPin(selfPid);
            WPI_CHECK(!pinBlocked);
            WPI_CHECK(guard.CallCount() == 0); // 策略拒绝在先，根本没问守卫

            const bool kernelBlocked = target->requestScope(ksword::memwb::Scope::KernelVirtual);
            WPI_CHECK(!kernelBlocked);
            WPI_CHECK(guard.CallCount() == 0);

            const bool physicalBlocked = target->requestScope(ksword::memwb::Scope::Physical);
            WPI_CHECK(!physicalBlocked);
            WPI_CHECK(guard.CallCount() == 0);

            // 回到/留在进程范围不受 allowKernelPhysical 影响，应当正常工作。
            const bool backToProcess = target->requestScope(ksword::memwb::Scope::ProcessVirtual);
            WPI_CHECK(backToProcess);
        }

        // ---- T1（锁定 M01 M02 M03 M05 M37）：守卫语义的精细断言 ----
        // 审核报告 §5 T1：旧夹具从没断言过"无变化不问守卫""钉住/解除钉住各问
        // 一次且理由对""requestPin(0) 非法"这几条，M01/M02/M03/M05/M37 五个
        // 变异因此全部幸存。这里把报告给出的断言并入，但"钉住另一个 pid"必须用
        // 一个**真实存在**的进程——D6 修复后 requestPin 会在问守卫之前先验证
        // pid 是否存在，传一个虚构 pid 会被 D6 的"目标已不存在"短路直接拒绝，
        // 根本问不到守卫，不能再用来测"守卫被问了没有"。这里拉起一个短命但
        // 足够存活的子进程来扮演这个"另一个真实进程"。
        {
            auto target = MakeTarget(nullptr);
            GuardRecorder guard;
            guard.SetApproval(true);
            target->setLeaveGuard(guard.asFunction());

            // 先附加一个"Dock 进程"（本进程伪句柄），让 pid 非 0。
            ks::ui::WorkbenchTarget::DockAttach attach;
            attach.handle = reinterpret_cast<void*>(::GetCurrentProcess());
            attach.pid = selfPid;
            attach.attachGeneration = 1;
            target->onDockAttached(attach);
            // onDockAttached 会自动触发一次进程模块刷新；等它落地，避免干扰
            // 后面对 guard 调用次数的精确计数（枚举本身不问守卫，但不等它
            // 落地会让后续断言的时序更难读）。
            PumpUntil([&] { return target->evaluate(QStringLiteral("x")).issue
                != ksword::memwb::Issue::ModulesLoading; }, 2000);

            // 无变化的请求不问守卫（M01）。
            WPI_CHECK(target->requestScope(ksword::memwb::Scope::ProcessVirtual));
            WPI_CHECK(target->requestChannel(ksword::memwb::Channel::UserMode));
            WPI_CHECK(target->requestFollowDock());           // 本来就在跟随
            WPI_CHECK(target->requestPin(selfPid));            // 钉住与 Dock 同一个进程：只改 Policy，不问
            WPI_CHECK(guard.CallCount() == 0);

            // requestPin(0) 非法：返回 false、不问守卫、不动（M05）。
            WPI_CHECK(!target->requestPin(0));
            WPI_CHECK(guard.CallCount() == 0);

            // 拉起一个真实存在、足够存活的子进程，扮演"另一个进程"。
            STARTUPINFOW startupInfo{};
            startupInfo.cb = sizeof(startupInfo);
            PROCESS_INFORMATION processInfo{};
            std::wstring commandLine = L"cmd.exe /c ping -n 20 127.0.0.1";
            std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
            mutableCommandLine.push_back(L'\0');
            const BOOL spawned = ::CreateProcessW(
                nullptr, mutableCommandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                nullptr, nullptr, &startupInfo, &processInfo);
            WPI_CHECK_NOTE(spawned != FALSE, QStringLiteral("T1 需要拉起一个子进程扮演\"另一个真实进程\""));
            if (spawned != FALSE)
            {
                const std::uint32_t otherPid = processInfo.dwProcessId;

                // 钉住另一个真实存在的 pid 要问一次，原因 = PinChange(3)（M02、M37）。
                guard.Reset();
                WPI_CHECK(target->requestPin(otherPid));
                WPI_CHECK(guard.CallCount() == 1);
                WPI_CHECK(guard.Reasons().size() == 1 && guard.Reasons()[0] == 3);

                // 解除钉住要问一次（M03）；再解除一次不问。
                guard.Reset();
                WPI_CHECK(target->requestFollowDock());
                WPI_CHECK(guard.CallCount() == 1);
                WPI_CHECK(target->requestFollowDock());
                WPI_CHECK(guard.CallCount() == 1);

                // 三个单维度包装的原因映射（M37）：范围=1、通道=2。
                guard.Reset();
                WPI_CHECK(target->requestScope(ksword::memwb::Scope::KernelVirtual));
                WPI_CHECK(target->requestChannel(ksword::memwb::Channel::StandardDriver));
                WPI_CHECK(guard.Reasons().size() == 2);
                WPI_CHECK(guard.Reasons()[0] == 1 && guard.Reasons()[1] == 2);

                // 回到进程范围，才能继续用"钉住 otherPid"测试守卫否决——D8 修复后
                // predictsIdentityChange 真的在 tracker 规则上试应用：内核/物理
                // 范围下会话的 pid 恒为 0，钉住不会改变身份位，根本不会问守卫
                // （这正是 D8 要修的"多问"偏差，见 WorkbenchTarget.cpp 注释），
                // 不能再借内核范围来测"守卫被否决"。
                guard.Reset();
                WPI_CHECK(target->requestScope(ksword::memwb::Scope::ProcessVirtual));

                // 守卫否决 pin / 通道 时 tracker 也完全不动（钉住目标仍是上面那个
                // 真实子进程，保证能走到"问守卫"这一步而不是被 D6 的存在性检查
                // 提前拦下）。
                guard.SetApproval(false);
                const auto before = target->session();
                WPI_CHECK(!target->requestPin(otherPid));
                WPI_CHECK(!target->requestChannel(ksword::memwb::Channel::Hvm));
                WPI_CHECK(ksword::memwb::SameTarget(before, target->session()));

                ::TerminateProcess(processInfo.hProcess, 0);
                ::CloseHandle(processInfo.hProcess);
                ::CloseHandle(processInfo.hThread);
            }
        }

        // ---- T2（锁定 M04）：lockToDock 下"回到跟随"是合法的空操作 ----
        {
            auto target = MakeTarget(nullptr);
            GuardRecorder guard;
            guard.SetApproval(true);
            target->setLeaveGuard(guard.asFunction());
            ks::ui::WorkbenchTarget::Policy policy;
            policy.lockToDock = true;
            target->setPolicy(policy);

            // 本来就在跟随：回到跟随应当成功且不问守卫（M04：如果 lockToDock 把
            // pinPid==0 这个"回到跟随"的请求也一并拒绝，这里会变成失败）。
            WPI_CHECK(target->requestFollowDock());
            WPI_CHECK(guard.CallCount() == 0);
        }

        // ---- T3（锁定 M12）：非法钩子参数（pid=0 的 onDockAttached）不发信号 ----
        {
            auto target = MakeTarget(nullptr);
            int changed = 0;
            QObject::connect(target.get(), &ks::ui::WorkbenchTarget::sessionChanged,
                [&](quint32) { ++changed; });
            ks::ui::WorkbenchTarget::DockAttach bad;   // pid=0 → tracker 内部拒绝（Rejected）
            bad.handle = nullptr;
            bad.pid = 0;
            bad.attachGeneration = 1;
            target->onDockAttached(bad);
            WPI_CHECK_NOTE(changed == 0, QStringLiteral("pid=0 的附加请求不应该发出 sessionChanged"));
        }
    }
}
