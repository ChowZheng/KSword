// memwb_wpI_tests.Extra.cpp —— review2-wpI 的补测（覆盖盖在仓库夹具之外，用来验证"能杀死幸存变异"）。
// 约定：只用夹具已有设施（WPI_CHECK / GuardRecorder / FakeWorkbenchServices / PumpUntil / AttachFakeProcess）。
// 每个小节标注它要杀死的变异 id（A* 来自回注集，C* 来自新变异集）。

#include "memwb_wpI_common.h"

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchTarget.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QThread>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace memwb_wpI_test
{
    namespace
    {
        namespace mw = ksword::memwb;

        // g_spinSink：空转抖动的结果汇点，volatile 防止整段循环被优化掉。
        volatile unsigned g_spinSink = 0;

        // SpawnChild：拉起子进程；成功返回 true，调用方负责关闭 pi 里的两个句柄。
        bool SpawnChild(const wchar_t* commandLine, PROCESS_INFORMATION& pi)
        {
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            std::vector<wchar_t> cmd(commandLine, commandLine + wcslen(commandLine) + 1);
            return ::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                    nullptr, nullptr, &si, &pi) != FALSE;
        }

        void KillChild(PROCESS_INFORMATION& pi)
        {
            ::TerminateProcess(pi.hProcess, 0);
            ::CloseHandle(pi.hProcess);
            ::CloseHandle(pi.hThread);
        }

        DWORD HandleCount()
        {
            DWORD n = 0;
            ::GetProcessHandleCount(::GetCurrentProcess(), &n);
            return n;
        }

        ks::ui::WorkbenchTarget::DockAttach SelfDock(std::uint64_t generation)
        {
            ks::ui::WorkbenchTarget::DockAttach a;
            a.handle = reinterpret_cast<void*>(::GetCurrentProcess());
            a.pid = ::GetCurrentProcessId();
            a.attachGeneration = generation;
            return a;
        }

        bool HasHighByte(const std::string& s)
        {
            for (const char c : s)
            {
                if (static_cast<unsigned char>(c) >= 0x80) { return true; }
            }
            return false;
        }
    }

    void RunExtraTests()
    {
        const std::uint32_t selfPid = ::GetCurrentProcessId();

        // ---- X1（杀 A08 A08b）：非法枚举——单维与合并请求都必须整体拒绝，不问守卫、不部分应用 ----
        {
            auto owned = std::make_unique<FakeWorkbenchServices>();
            ks::ui::WorkbenchTarget target(std::move(owned));
            GuardRecorder guard;
            guard.SetApproval(true);
            target.setLeaveGuard(guard.asFunction());

            WPI_CHECK(!target.requestScope(static_cast<mw::Scope>(7)));
            WPI_CHECK(!target.requestChannel(static_cast<mw::Channel>(9)));
            WPI_CHECK(guard.CallCount() == 0);
            WPI_CHECK(target.lastIdentityFailure() == ks::ui::NavStatus::Unavailable);

            // 合并请求：范围合法、通道非法。整体拒绝，范围也不能被先应用掉。
            ks::ui::IdentityRequest combined;
            combined.scope = mw::Scope::KernelVirtual;
            combined.channel = static_cast<mw::Channel>(9);
            WPI_CHECK(!target.requestIdentity(combined, ks::ui::LeaveReason::ScopeChange));
            WPI_CHECK(guard.CallCount() == 0);
            WPI_CHECK(target.session().scope == mw::Scope::ProcessVirtual);

            ks::ui::IdentityRequest combined2;
            combined2.scope = static_cast<mw::Scope>(7);
            combined2.channel = mw::Channel::StandardDriver;
            WPI_CHECK(!target.requestIdentity(combined2, ks::ui::LeaveReason::ScopeChange));
            WPI_CHECK(guard.CallCount() == 0);
            WPI_CHECK(target.session().channel == mw::Channel::UserMode);
        }

        // ---- X2（杀 A09）：Dock 是 weak（句柄空，创建时间 0），钉住同一个 pid 得到强锚点 ⇒ 身份变了，必须问一次 ----
        {
            auto owned = std::make_unique<FakeWorkbenchServices>();
            ks::ui::WorkbenchTarget target(std::move(owned));
            GuardRecorder guard;
            guard.SetApproval(true);
            target.setLeaveGuard(guard.asFunction());
            ks::ui::WorkbenchTarget::DockAttach weakDock;
            weakDock.handle = nullptr;
            weakDock.pid = selfPid;
            weakDock.attachGeneration = 1;
            target.onDockAttached(weakDock);
            WPI_CHECK(target.session().processCreateTime100ns == 0);
            guard.Reset();
            WPI_CHECK(target.requestPin(selfPid));
            WPI_CHECK_NOTE(guard.CallCount() == 1, QStringLiteral("创建时间 0→真实值是身份变化，钉住前必须问一次守卫"));
            WPI_CHECK(target.session().processCreateTime100ns != 0);
        }

        // ---- X3（杀 A10）：守卫在被调用期间注销自己，被调用的那份 std::function 不能随之销毁 ----
        {
            auto owned = std::make_unique<FakeWorkbenchServices>();
            ks::ui::WorkbenchTarget target(std::move(owned));
            ks::ui::WorkbenchTarget* targetPtr = &target;
            struct Token
            {
                bool* flag;
                explicit Token(bool* f) : flag(f) {}
                ~Token() { *flag = false; }
            };
            bool tokenAlive = true;
            bool aliveAfterSelfReset = false;
            auto token = std::make_shared<Token>(&tokenAlive);
            target.setLeaveGuard([token, targetPtr, &tokenAlive, &aliveAfterSelfReset](ks::ui::LeaveReason) -> bool
                {
                    // 先把后面要用的东西拷成局部变量，避免读已销毁闭包里的成员。
                    bool* aliveFlag = &tokenAlive;
                    bool* resultFlag = &aliveAfterSelfReset;
                    targetPtr->setLeaveGuard(nullptr);
                    *resultFlag = *aliveFlag;
                    return true;
                });
            token.reset();
            WPI_CHECK(target.requestScope(mw::Scope::KernelVirtual));
            WPI_CHECK_NOTE(aliveAfterSelfReset, QStringLiteral("requestLeave 必须拷贝守卫：被调用的闭包不能在调用期间被销毁"));
        }

        // ---- X4（杀 A11）：aboutToDetach 只在"跟随 Dock 且范围=进程且有非零 pid"时发 ----
        {
            PROCESS_INFORMATION child{};
            if (SpawnChild(L"cmd.exe /c ping -n 8 127.0.0.1", child))
            {
                auto fires = [](ks::ui::WorkbenchTarget& t) -> int
                {
                    int n = 0;
                    const QMetaObject::Connection c = QObject::connect(&t, &ks::ui::WorkbenchTarget::aboutToDetach, [&]() { ++n; });
                    t.onDockAboutToDetach();
                    QObject::disconnect(c);
                    return n;
                };
                {
                    auto o = std::make_unique<FakeWorkbenchServices>();
                    ks::ui::WorkbenchTarget t(std::move(o));
                    t.onDockAttached(SelfDock(1));
                    WPI_CHECK(fires(t) == 1);               // 跟随 Dock：会改变会话，必须发
                }
                {
                    auto o = std::make_unique<FakeWorkbenchServices>();
                    ks::ui::WorkbenchTarget t(std::move(o));
                    t.onDockAttached(SelfDock(1));
                    WPI_CHECK(t.requestPin(child.dwProcessId));
                    WPI_CHECK_NOTE(fires(t) == 0, QStringLiteral("钉在另一个进程：Dock 分离与会话无关，不能发"));
                }
                {
                    auto o = std::make_unique<FakeWorkbenchServices>();
                    ks::ui::WorkbenchTarget t(std::move(o));
                    t.onDockAttached(SelfDock(1));
                    WPI_CHECK(t.requestPin(selfPid));
                    WPI_CHECK_NOTE(fires(t) == 0, QStringLiteral("钉在与 Dock 相同的 pid：会话不会因分离而变，不能发"));
                }
                KillChild(child);
            }
        }

        // ---- X5（杀 A12 A13 C25 C26 C27）：sessionChanged / modulesFailed 槽里同步 evaluate 看到的目录状态与信号语义 ----
        {
            auto owned = std::make_unique<FakeWorkbenchServices>();
            FakeWorkbenchServices* fake = owned.get();
            fake->SetProcessModulesResult(4242, ks::ui::ModuleEnumResult{false, {}, "fake failure"});
            fake->SetProcessModulesResult(4243, ks::ui::ModuleEnumResult{true, {}, {}});
            ks::ui::WorkbenchTarget target(std::move(owned));
            ks::ui::WorkbenchTarget* tp = &target;
            std::vector<mw::Issue> inSession;
            QObject::connect(&target, &ks::ui::WorkbenchTarget::sessionChanged,
                [&](quint32) { inSession.push_back(tp->evaluate(QStringLiteral("x")).issue); });
            int failedCount = 0;
            bool failedKernelFlag = true;
            mw::Issue inFailed = mw::Issue::None;
            QObject::connect(&target, &ks::ui::WorkbenchTarget::modulesFailed,
                [&](bool kernel) { ++failedCount; failedKernelFlag = kernel; inFailed = tp->evaluate(QStringLiteral("x")).issue; });

            AttachFakeProcess(target, 4242, 1);
            WPI_CHECK(inSession.size() == 1);
            WPI_CHECK_NOTE(!inSession.empty() && inSession[0] == mw::Issue::ModulesLoading,
                QStringLiteral("sessionChanged 槽里目录必须已经是新所有者的 Loading"));
            WPI_CHECK(PumpUntil([&] { return failedCount >= 1; }, 2000));
            WPI_CHECK(failedCount == 1);
            WPI_CHECK(!failedKernelFlag);
            WPI_CHECK_NOTE(inFailed == mw::Issue::ModulesNotLoaded, QStringLiteral("modulesFailed 槽里目录处于 Failed 状态"));
        }
        {
            // 陈旧的失败结果不能再发 modulesFailed：A(慢、失败)被 B(快、成功)顶掉。
            auto owned = std::make_unique<FakeWorkbenchServices>();
            FakeWorkbenchServices* fake = owned.get();
            fake->SetProcessModulesResult(5001, ks::ui::ModuleEnumResult{false, {}, "slow stale failure"});
            fake->SetProcessEnumDelayMs(5001, 150);
            fake->SetProcessModulesResult(5002, ks::ui::ModuleEnumResult{true, {}, {}});
            ks::ui::WorkbenchTarget target(std::move(owned));
            int failedCount = 0;
            QObject::connect(&target, &ks::ui::WorkbenchTarget::modulesFailed, [&](bool) { ++failedCount; });
            AttachFakeProcess(target, 5001, 1);
            AttachFakeProcess(target, 5002, 2);
            QThread::msleep(300);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            WPI_CHECK_NOTE(failedCount == 0, QStringLiteral("被新票据顶掉的失败结果不应发 modulesFailed"));
        }

        // ---- X6（杀 A14）：weak 身份（句柄空）下 identityAnchored 必须为 false ----
        {
            auto owned = std::make_unique<FakeWorkbenchServices>();
            ks::ui::WorkbenchTarget target(std::move(owned));
            ks::ui::WorkbenchTarget::DockAttach weakDock;
            weakDock.handle = nullptr;
            weakDock.pid = selfPid;
            weakDock.attachGeneration = 1;
            target.onDockAttached(weakDock);
            WPI_CHECK(target.session().pid == selfPid);
            WPI_CHECK(!target.identityAnchored());
        }

        // ---- X7（杀 A15 A15b A15c）：换目标把存活状态重置为 Unknown 时必须发信号 ----
        {
            auto owned = std::make_unique<FakeWorkbenchServices>();
            ks::ui::WorkbenchTarget target(std::move(owned));
            int last = -1;
            int count = 0;
            QObject::connect(&target, &ks::ui::WorkbenchTarget::livenessChanged, [&](int s) { last = s; ++count; });
            target.onDockAttached(SelfDock(1));
            target.checkLiveness();
            WPI_CHECK(last == static_cast<int>(ks::ui::LivenessState::Alive));
            // 换附加另一个进程（弱锚点）：状态重置为 Unknown 并通知。
            ks::ui::WorkbenchTarget::DockAttach other;
            other.handle = nullptr;
            other.pid = 9999;
            other.attachGeneration = 2;
            target.onDockAttached(other);
            WPI_CHECK_NOTE(last == static_cast<int>(ks::ui::LivenessState::Unknown), QStringLiteral("附加换目标后应通知 Unknown"));

            // 分离：先回到 Alive，再分离。
            target.onDockAttached(SelfDock(3));
            target.checkLiveness();
            WPI_CHECK(last == static_cast<int>(ks::ui::LivenessState::Alive));
            target.onDockDetached();
            WPI_CHECK_NOTE(last == static_cast<int>(ks::ui::LivenessState::Unknown), QStringLiteral("分离后应通知 Unknown"));

            // 钉住/解除钉住：先 Alive，再解除钉住。
            PROCESS_INFORMATION child{};
            if (SpawnChild(L"cmd.exe /c ping -n 8 127.0.0.1", child))
            {
                WPI_CHECK(target.requestPin(child.dwProcessId));
                target.checkLiveness();
                WPI_CHECK(last == static_cast<int>(ks::ui::LivenessState::Alive));
                WPI_CHECK(target.requestFollowDock());
                WPI_CHECK_NOTE(last == static_cast<int>(ks::ui::LivenessState::Unknown), QStringLiteral("解除钉住后应通知 Unknown"));
                KillChild(child);
            }
            (void)count;
        }

        // ---- X8（杀 A16）：构造时 services 传空的占位实现，所有失败 detail 都必须是纯 ASCII 技术短语 ----
        {
            ks::ui::WorkbenchTarget target(nullptr);
            AttachFakeProcess(target, 777, 1);
            const mw::AddressEval deref = target.evaluate(QStringLiteral("[0x1000]"));
            WPI_CHECK_NOTE(!HasHighByte(deref.detail), QStringLiteral("deref 失败 detail 不得含汉字/非 ASCII"));
            WPI_CHECK(target.requestScope(mw::Scope::KernelVirtual));
            PumpUntil([&] { return target.evaluate(QStringLiteral("x")).issue != mw::Issue::ModulesLoading; }, 2000);
            const mw::AddressEval kernelEval = target.evaluate(QStringLiteral("x"));
            WPI_CHECK_NOTE(!HasHighByte(kernelEval.detail), QStringLiteral("内核枚举失败 detail 不得含汉字/非 ASCII"));
        }

        // ---- X9（杀 C10 C11）：lastIdentityFailure 的取值映射 ----
        {
            auto owned = std::make_unique<FakeWorkbenchServices>();
            ks::ui::WorkbenchTarget target(std::move(owned));
            GuardRecorder guard;
            guard.SetApproval(false);
            target.setLeaveGuard(guard.asFunction());
            WPI_CHECK(!target.requestScope(mw::Scope::KernelVirtual));
            WPI_CHECK(target.lastIdentityFailure() == ks::ui::NavStatus::LeaveRefused);

            ks::ui::WorkbenchTarget::Policy policy;
            policy.lockToDock = true;
            target.setPolicy(policy);
            WPI_CHECK(!target.requestPin(selfPid));
            WPI_CHECK(target.lastIdentityFailure() == ks::ui::NavStatus::Unavailable);
        }

        // ---- X10（杀 C12）：Hvm 通道是合法值，批准后应切换成功 ----
        {
            auto owned = std::make_unique<FakeWorkbenchServices>();
            ks::ui::WorkbenchTarget target(std::move(owned));
            WPI_CHECK(target.requestChannel(mw::Channel::Hvm));
            WPI_CHECK(target.session().channel == mw::Channel::Hvm);
        }

        // ---- X11（杀 C04 C05）：期望创建时间"比实际更旧/更新"两个方向都必须判 TargetMismatch ----
        {
            PROCESS_INFORMATION child{};
            if (SpawnChild(L"cmd.exe /c ping -n 8 127.0.0.1", child))
            {
                const ks::ui::AnchorInfo real = ks::ui::AcquireAnchorForPid(child.dwProcessId);
                WPI_CHECK(!real.identityWeak);
                ks::ui::ReleaseAnchorHandle(real.handle);
                auto owned = std::make_unique<FakeWorkbenchServices>();
                ks::ui::WorkbenchTarget target(std::move(owned));
                WPI_CHECK(!target.requestPin(child.dwProcessId, real.createTime100ns - 1));
                WPI_CHECK(target.lastIdentityFailure() == ks::ui::NavStatus::TargetMismatch);
                WPI_CHECK(!target.requestPin(child.dwProcessId, real.createTime100ns + 1));
                WPI_CHECK(target.lastIdentityFailure() == ks::ui::NavStatus::TargetMismatch);
                // PID 复用的真实形态：旧实例创建时间早得多。
                WPI_CHECK(!target.requestPin(child.dwProcessId, real.createTime100ns - 100000000ULL));
                WPI_CHECK(target.requestPin(child.dwProcessId, real.createTime100ns));
                KillChild(child);
            }
        }

        // ---- X12（杀 C14 C15 C16 C17）：各失败/替换路径不泄漏钉住/Dock 句柄 ----
        {
            PROCESS_INFORMATION childA{};
            PROCESS_INFORMATION childB{};
            if (SpawnChild(L"cmd.exe /c ping -n 40 127.0.0.1", childA) && SpawnChild(L"cmd.exe /c ping -n 40 127.0.0.1", childB))
            {
                auto owned = std::make_unique<FakeWorkbenchServices>();
                ks::ui::WorkbenchTarget target(std::move(owned));
                GuardRecorder guard;
                guard.SetApproval(true);
                target.setLeaveGuard(guard.asFunction());
                const ks::ui::AnchorInfo realA = ks::ui::AcquireAnchorForPid(childA.dwProcessId);
                ks::ui::ReleaseAnchorHandle(realA.handle);
                constexpr int kIter = 120;
                constexpr DWORD kMargin = 40;   // 远小于 kIter，远大于线程池抖动
                // 换钉路径每次都会触发一次模块枚举（线程池），句柄数有 ±60 的线程池抖动，所以迭代数与余量都放大。
                constexpr int kIterRepin = 500;
                constexpr DWORD kMarginRepin = 200;

                // a) 守卫否决（C14）。
                guard.SetApproval(false);
                DWORD before = HandleCount();
                for (int i = 0; i < kIter; ++i) { (void)target.requestPin(childA.dwProcessId); }
                DWORD after = HandleCount();
                WPI_CHECK_NOTE(after <= before + kMargin, QStringLiteral("守卫否决路径泄漏句柄 %1→%2").arg(before).arg(after));

                // b) TargetMismatch（C15）。
                guard.SetApproval(true);
                before = HandleCount();
                for (int i = 0; i < kIter; ++i) { (void)target.requestPin(childA.dwProcessId, realA.createTime100ns + 1); }
                after = HandleCount();
                WPI_CHECK_NOTE(after <= before + kMargin, QStringLiteral("TargetMismatch 路径泄漏句柄 %1→%2").arg(before).arg(after));

                // c) 反复换钉 A/B（C16）。
                WPI_CHECK(target.requestPin(childA.dwProcessId));
                PumpUntil([] { return false; }, 100);
                before = HandleCount();
                for (int i = 0; i < kIterRepin; ++i) { (void)target.requestPin((i % 2 == 0) ? childB.dwProcessId : childA.dwProcessId); }
                PumpUntil([] { return false; }, 100);
                after = HandleCount();
                WPI_CHECK_NOTE(after <= before + kMarginRepin, QStringLiteral("换钉路径泄漏句柄 %1→%2").arg(before).arg(after));
                KillChild(childA);
                KillChild(childB);
            }
        }
        {
            // d) Dock 附加后分离，重复（C17）。
            auto owned = std::make_unique<FakeWorkbenchServices>();
            owned->SetProcessModulesResult(::GetCurrentProcessId(), ks::ui::ModuleEnumResult{true, {}, {}});
            ks::ui::WorkbenchTarget target(std::move(owned));
            for (int i = 0; i < 60; ++i) { target.onDockAttached(SelfDock(2 + static_cast<std::uint64_t>(i))); target.onDockDetached(); }
            PumpUntil([] { return false; }, 300);
            const DWORD before = HandleCount();
            for (int i = 0; i < 700; ++i) { target.onDockAttached(SelfDock(1000 + static_cast<std::uint64_t>(i))); target.onDockDetached(); }
            PumpUntil([] { return false; }, 300);
            const DWORD after = HandleCount();
            WPI_CHECK_NOTE(after <= before + 250, QStringLiteral("附加/分离路径泄漏句柄 %1→%2").arg(before).arg(after));
        }

        // ---- X13（杀 C20）：物理范围下存活探测也必须是 Unknown ----
        {
            auto owned = std::make_unique<FakeWorkbenchServices>();
            ks::ui::WorkbenchTarget target(std::move(owned));
            int last = -1;
            QObject::connect(&target, &ks::ui::WorkbenchTarget::livenessChanged, [&](int s) { last = s; });
            target.onDockAttached(SelfDock(1));
            target.checkLiveness();
            WPI_CHECK(last == static_cast<int>(ks::ui::LivenessState::Alive));
            WPI_CHECK(target.requestScope(mw::Scope::Physical));
            target.checkLiveness();
            WPI_CHECK(last == static_cast<int>(ks::ui::LivenessState::Unknown));
        }

        // ---- X14（杀 C21 / M20）：Dock 给了一个能复制但不是进程的句柄 ⇒ 查询失败，必须保持 Unknown，不得报 Exited ----
        {
            HANDLE ev = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
            auto owned = std::make_unique<FakeWorkbenchServices>();
            ks::ui::WorkbenchTarget target(std::move(owned));
            std::string seq;
            QObject::connect(&target, &ks::ui::WorkbenchTarget::livenessChanged, [&](int s) { seq += std::to_string(s) + ","; });
            ks::ui::WorkbenchTarget::DockAttach d;
            d.handle = ev;
            d.pid = 777;
            d.attachGeneration = 1;
            target.onDockAttached(d);
            target.checkLiveness();
            WPI_CHECK_NOTE(seq.empty(), QStringLiteral("查询调用本身失败时不得报告任何存活状态变化"));
            ::CloseHandle(ev);
        }

        // ---- X15（杀 C09）：已在 Ddma 通道再请求 Ddma（代次没变）是无变化，不问守卫 ----
        {
            auto owned = std::make_unique<FakeWorkbenchServices>();
            FakeWorkbenchServices* fake = owned.get();
            fake->SetDdmaGeneration(100);
            ks::ui::WorkbenchTarget target(std::move(owned));
            GuardRecorder guard;
            guard.SetApproval(true);
            target.setLeaveGuard(guard.asFunction());
            WPI_CHECK(target.requestChannel(mw::Channel::Ddma));
            guard.Reset();
            WPI_CHECK(target.requestChannel(mw::Channel::Ddma));
            WPI_CHECK_NOTE(guard.CallCount() == 0, QStringLiteral("Ddma→Ddma 且代次未变：无变化，不应问守卫"));
        }

        // ---- X17（杀 A03，不依赖机器上有没有 audiodg）：把子进程 DACL 改成只允许 QUERY_LIMITED，
        // 精确复现"受保护进程：QUERY_INFORMATION 被拒、LIMITED 通过"的条件 ----
        {
            PROCESS_INFORMATION child{};
            if (SpawnChild(L"cmd.exe /c ping -n 8 127.0.0.1", child))
            {
                bool prepared = false;
                PSECURITY_DESCRIPTOR sd = nullptr;
                if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;0x101000;;;WD)", SDDL_REVISION_1, &sd, nullptr) != FALSE)
                {
                    BOOL present = FALSE;
                    BOOL defaulted = FALSE;
                    PACL dacl = nullptr;
                    if (::GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) != FALSE && present != FALSE)
                    {
                        prepared = (::SetSecurityInfo(child.hProcess, SE_KERNEL_OBJECT,
                                        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                        nullptr, nullptr, dacl, nullptr) == ERROR_SUCCESS);
                    }
                    ::LocalFree(sd);
                }
                HANDLE full = ::OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, child.dwProcessId);
                const bool fullDenied = (full == nullptr);
                if (full != nullptr) { ::CloseHandle(full); }
                if (prepared && fullDenied)
                {
                    const ks::ui::AnchorInfo a = ks::ui::AcquireAnchorForPid(child.dwProcessId);
                    WPI_CHECK_NOTE(!a.identityWeak, QStringLiteral("D3：只给 QUERY_LIMITED 的进程必须能被锚定"));
                    WPI_CHECK(a.createTime100ns != 0);
                    ks::ui::ReleaseAnchorHandle(a.handle);

                    auto owned = std::make_unique<FakeWorkbenchServices>();
                    ks::ui::WorkbenchTarget target(std::move(owned));
                    WPI_CHECK(target.requestPin(child.dwProcessId));
                    WPI_CHECK(target.identityAnchored());
                }
                else
                {
                    std::cout << "X17 skipped: 无法仿真受保护进程 (prepared=" << prepared << " fullDenied=" << fullDenied << ")" << std::endl;
                }
                KillChild(child);
            }
        }

        // ---- X19（杀 C32）：Dock 给了一个根本无效的句柄 ⇒ weak、无句柄、lastError 带回系统错误码 ----
        {
            const ks::ui::AnchorInfo bad = ks::ui::AcquireAnchorFromDockHandle(
                reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xDEADBEE4u)));
            WPI_CHECK(bad.identityWeak);
            WPI_CHECK(bad.handle == nullptr);
            WPI_CHECK_NOTE(bad.lastError != 0, QStringLiteral("复制句柄失败时 lastError 必须带回 GetLastError()"));
        }

        // ---- X16（杀 A01）：D1 稳定性放大版——4000 次、随机微延迟 ----
        {
            unsigned seed = 12345;
            for (int i = 0; i < 600; ++i)
            {
                auto owned = std::make_unique<FakeWorkbenchServices>();
                owned->SetKernelModulesResult(ks::ui::ModuleEnumResult{true, {}, {}});
                owned->SetProcessModulesResult(selfPid, ks::ui::ModuleEnumResult{true, {}, {}});
                auto target = std::make_unique<ks::ui::WorkbenchTarget>(std::move(owned));
                if (i % 3 == 0) { AttachFakeProcess(*target, selfPid, 1); }
                else { target->requestScope(mw::Scope::KernelVirtual); }
                // 空转抖动（不用 QThread::usleep：Windows 上它会被取整到 1ms 以上，4000 次要跑几十秒）。
                seed = seed * 1103515245u + 12345u;
                const unsigned spins = (seed >> 16) % 6000u;
                unsigned acc = seed;
                for (unsigned s = 0; s < spins; ++s) { acc = acc * 1664525u + 1013904223u; }
                g_spinSink = acc;
                if (i % 500 == 0) { QCoreApplication::processEvents(QEventLoop::AllEvents, 5); }
                target.reset();
            }
            WPI_CHECK(true);
        }

        // ---- X20（杀 A01，确定性）：D1 是纳秒级竞态，运行期压力每轮只有约 4 成概率撞上，
        // 所以另加一道源码形态闸门：ModuleEnumTask 里任何 QMetaObject::invokeMethod 的 context（第一个实参）
        // 都不得是从 QPointer 取出的裸指针（含 data() / target_ / rawTarget / this）。 ----
        {
            std::ifstream in("Ksword5.1\\Ksword5.1\\UI\\MemoryWorkbench\\WorkbenchTarget.Modules.cpp", std::ios::binary);
            if (!in)
            {
                std::cout << "X20 skipped: 找不到 WorkbenchTarget.Modules.cpp（需要从仓库根目录运行）" << std::endl;
            }
            else
            {
                const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                int invokeCount = 0;
                bool badContext = false;
                std::size_t pos = 0;
                while ((pos = text.find("QMetaObject::invokeMethod(", pos)) != std::string::npos)
                {
                    pos += std::string("QMetaObject::invokeMethod(").size();
                    // 跳过注释里出现的字样：同一行里 "//" 在它之前就算注释。
                    const std::size_t lineStart = text.rfind('\n', pos);
                    const std::string lead = text.substr(lineStart == std::string::npos ? 0 : lineStart, pos - (lineStart == std::string::npos ? 0 : lineStart));
                    if (lead.find("//") != std::string::npos) { continue; }
                    const std::size_t comma = text.find(',', pos);
                    const std::string context = text.substr(pos, comma - pos);
                    ++invokeCount;
                    if (context.find("data()") != std::string::npos || context.find("target_") != std::string::npos
                        || context.find("rawTarget") != std::string::npos || context.find("this") != std::string::npos)
                    {
                        badContext = true;
                    }
                }
                WPI_CHECK_NOTE(invokeCount >= 1, QStringLiteral("闸门没有找到任何 invokeMethod 调用，扫描规则失效"));
                WPI_CHECK_NOTE(!badContext, QStringLiteral("invokeMethod 的 context 不得是工作线程里取出的裸 WorkbenchTarget 指针（D1）"));
            }
        }
    }
}
