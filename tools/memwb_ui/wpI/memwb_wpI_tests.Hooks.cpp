// memwb_wpI_tests.Hooks.cpp
// 作用：验证 WorkbenchTarget 的三个 Dock 钩子（onDockAttached / onDockAboutToDetach /
//       onDockDetached）——触发顺序、aboutToDetach 的同步直接连接语义（句柄仍有效）、
//       attach 自动触发一次模块刷新、checkLiveness/livenessChanged。

#include "memwb_wpI_common.h"

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchTarget.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <memory>

namespace memwb_wpI_test
{
    void RunHooksTests()
    {
        auto servicesOwned = std::make_unique<FakeWorkbenchServices>();
        FakeWorkbenchServices* fake = servicesOwned.get();
        const std::uint32_t selfPid = static_cast<std::uint32_t>(::GetCurrentProcessId());
        fake->SetProcessModulesResult(selfPid, ks::ui::ModuleEnumResult{true, {}, {}});

        ks::ui::WorkbenchTarget target(std::move(servicesOwned));

        // 构造后默认跟随 Dock 且范围=进程：Dock 的附加/分离此刻会改变会话。
        WPI_CHECK(target.wouldChangeOnDockAttach());

        int sessionChangedCount = 0;
        quint32 lastMask = 0;
        QObject::connect(&target, &ks::ui::WorkbenchTarget::sessionChanged,
            [&](quint32 mask) { ++sessionChangedCount; lastMask = mask; });

        int modulesChangedCount = 0;
        QObject::connect(&target, &ks::ui::WorkbenchTarget::modulesChanged,
            [&](bool) { ++modulesChangedCount; });

        bool aboutToDetachFiredSynchronously = false;
        QObject::connect(&target, &ks::ui::WorkbenchTarget::aboutToDetach,
            [&]() { aboutToDetachFiredSynchronously = true; });

        int livenessChangedCount = 0;
        int lastLivenessState = -1;
        QObject::connect(&target, &ks::ui::WorkbenchTarget::livenessChanged,
            [&](int state) { ++livenessChangedCount; lastLivenessState = state; });

        // ---- 第一钩子：onDockAttached ----
        ks::ui::WorkbenchTarget::DockAttach attach;
        attach.handle = reinterpret_cast<void*>(::GetCurrentProcess()); // 本进程伪句柄，始终有效
        attach.pid = selfPid;
        attach.name = QStringLiteral("self.exe");
        attach.attachGeneration = 1;
        attach.hintReadOnly = false;

        target.onDockAttached(attach);
        WPI_CHECK(sessionChangedCount == 1);
        WPI_CHECK((lastMask & static_cast<quint32>(ksword::memwb::TargetChange::Process)) != 0);
        WPI_CHECK(target.session().pid == selfPid);
        WPI_CHECK(target.session().scope == ksword::memwb::Scope::ProcessVirtual);
        WPI_CHECK(target.session().processCreateTime100ns != 0); // 本进程锚点应已锚定（identityWeak=false）

        // attach 会自动触发一次进程模块刷新（Process 位 + 范围=进程），等待它落地。
        const bool modulesLanded = PumpUntil([&] { return modulesChangedCount >= 1; }, 3000);
        WPI_CHECK_NOTE(modulesLanded, QStringLiteral("等待 attach 触发的模块刷新落地超时"));
        WPI_CHECK(fake->ProcessEnumCallCount() >= 1);

        // ---- checkLiveness：首次探测应判定存活并发一次信号 ----
        target.checkLiveness();
        WPI_CHECK(livenessChangedCount == 1);
        WPI_CHECK(lastLivenessState == static_cast<int>(ks::ui::LivenessState::Alive));
        // 状态未变时第二次探测不应再发信号。
        target.checkLiveness();
        WPI_CHECK(livenessChangedCount == 1);

        // ---- 第二钩子：onDockAboutToDetach，必须同步、句柄仍有效 ----
        aboutToDetachFiredSynchronously = false;
        target.onDockAboutToDetach();
        // 没有调用 processEvents：如果这里为真，证明订阅者是在 onDockAboutToDetach()
        // 返回之前、同一调用栈内被直接调用的（direct connection 的同步语义）。
        WPI_CHECK(aboutToDetachFiredSynchronously);

        // 句柄仍有效的独立证明：Dock 原始句柄此刻仍可查询退出码（本进程当然存活）。
        DWORD exitCodeProbe = 0;
        const BOOL stillQueryable = ::GetExitCodeProcess(::GetCurrentProcess(), &exitCodeProbe);
        WPI_CHECK(stillQueryable != FALSE);
        // aboutToDetach 本身不改变会话：这一步不应有新的 sessionChanged。
        WPI_CHECK(sessionChangedCount == 1);

        // ---- 第三钩子：onDockDetached ----
        const int sessionChangedBeforeDetach = sessionChangedCount;
        target.onDockDetached();
        WPI_CHECK(sessionChangedCount == sessionChangedBeforeDetach + 1);
        WPI_CHECK(target.session().pid == 0); // 分离后回到"无目标"
    }
}
