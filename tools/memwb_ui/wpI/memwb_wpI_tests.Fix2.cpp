// memwb_wpI_tests.Fix2.cpp
// 作用：review2-wpI 第二轮审核报告"必须修"的 N1-N5 五条缺陷的回归测试，外加
// 三项装配层拍板的新增能力（revisions() 只读访问器、X20 源码形态闸门已在
// memwb_wpI_tests.Extra.cpp 里、X18 按 N4 决策改写成的确定性测试）。
// 与 memwb_wpI_tests.Extra.cpp（审核者原文，76 checks）分开成独立文件，便于
// 区分"审核者已验证的补测"与"本轮修复新补的回归"，也避免单文件超过约定上限。
//
// 覆盖关系（每节标注修的是哪条缺陷、如果去掉修复会被哪个断言拦住）：
//   F1  N1：requestPin(0) 早返回也必须更新 lastIdentityFailure_（不能残留陈旧原因）。
//   F2  N2：钉住态下的 Dock 事件、空操作 requestFollowDock/requestPin(同一个pid)
//       都不应该让存活指示灯抖动；真正换了探测句柄主人时才重置。
//   F3  N3（requestIdentity 这一半）：离开守卫弹框期间目标被同步销毁，
//       requestIdentity 不能再碰任何成员，必须干净返回 false 而不是 UAF。
//   F4  N3（session() 这一半）：Ddma 通道下 session() 内部同步发出的
//       sessionChanged 期间目标被销毁，同样不能解引用已释放内存。
//   F5  N5：已经停留在 Ddma 通道时，显式 requestChannel(Ddma) 必须真的去问一次
//       services_ 的最新代次，代次漂移时必须问一次离开守卫。
//   F6  N4 决策的确定性测试（原审核报告 X18 的"待决策"版本，现按主会话拍板的
//       "不失败"方向改写，并入默认运行）：expectCreateTime 对 weak 锚点无法
//       核对，requestPin 仍然成功，但 identityAnchored()==false 且
//       lastIdentityFailure() 维持 Ok——调用方据此自行提示"身份未锚定"。
//   F7  装配层新增的 revisions() 只读访问器：引用随会话变化实时更新，与
//       capture() 读到的来源/内容代次一致。
//   F8  N3 的静态兜底闸门：F4 实测证明 session() 这一半的 UAF **不保证崩溃**
//       ——delete 之后那块堆内存在触发 return 语句之前有没有被别的分配覆盖
//       纯属运气（同 A01 的"约 50% 命中率"同一种根因），单靠运行期断言会在
//       "这次刚好没踩中脚"的机器/编译器/CRT 版本上假通过。源码形态闸门不依赖
//       内存分配时机：直接核对 WorkbenchTarget.cpp 里 `if (!self)` 这一判空
//       写法恰好出现在 session()/isStale()/requestIdentity 三处。
//   F9  livenessChanged 同步销毁：Dock 附加/分离与钉住请求返回后不再访问目标。

#include "memwb_wpI_common.h"

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchTarget.h"

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

        // SpawnPingChild：拉起一个短命但足够存活的子进程，供需要"真实进程"的
        // 场景使用（同 Extra.cpp 的 SpawnChild，这里独立实现，避免跨文件依赖
        // 匿名命名空间里的符号）。
        bool SpawnPingChild(const wchar_t* commandLine, PROCESS_INFORMATION& pi)
        {
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            std::vector<wchar_t> cmd(commandLine, commandLine + wcslen(commandLine) + 1);
            return ::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                    nullptr, nullptr, &si, &pi) != FALSE;
        }

        void KillPingChild(PROCESS_INFORMATION& pi)
        {
            ::TerminateProcess(pi.hProcess, 0);
            ::CloseHandle(pi.hProcess);
            ::CloseHandle(pi.hThread);
        }
    }

    void RunFix2Tests()
    {
        const std::uint32_t selfPid = ::GetCurrentProcessId();

        // ==== F1（N1）：requestPin(0) 返回 false 也必须更新 lastIdentityFailure_ ====
        {
            auto owned = std::make_unique<FakeWorkbenchServices>();
            ks::ui::WorkbenchTarget target(std::move(owned));

            // 先留下一个陈旧原因：钉住一个几乎可以确定不存在的 pid，得到 TargetGone。
            WPI_CHECK(!target.requestPin(0x7FFFFFF0));
            WPI_CHECK(target.lastIdentityFailure() == ks::ui::NavStatus::TargetGone);

            // 修复前：requestPin(0) 的早返回完全不碰 lastIdentityFailure_，上面的
            // TargetGone 会原样残留，调用方读到的是一个与"这次调用"无关的陈旧原因。
            WPI_CHECK(!target.requestPin(0));
            WPI_CHECK_NOTE(
                target.lastIdentityFailure() != ks::ui::NavStatus::TargetGone,
                QStringLiteral("N1：requestPin(0) 不应该让上一次调用的陈旧失败原因继续残留"));
            WPI_CHECK_NOTE(
                target.lastIdentityFailure() == ks::ui::NavStatus::Unavailable,
                QStringLiteral("N1：requestPin(0) 本身是一次非法请求，原因应为 Unavailable"));

            // 反过来：先成功一次（Ok），再 requestPin(0)，原因不应该被误报成 Ok。
            WPI_CHECK(target.requestScope(mw::Scope::KernelVirtual));
            WPI_CHECK(target.lastIdentityFailure() == ks::ui::NavStatus::Ok);
            WPI_CHECK(!target.requestPin(0));
            WPI_CHECK_NOTE(
                target.lastIdentityFailure() == ks::ui::NavStatus::Unavailable,
                QStringLiteral("N1：requestPin(0) 之后 lastIdentityFailure() 不应该是 Ok"));
        }

        // ==== F2（N2）：钉住态下的 Dock 事件 / 空操作不应该让存活状态抖动 ====
        {
            PROCESS_INFORMATION child{};
            WPI_CHECK_NOTE(SpawnPingChild(L"cmd.exe /c ping -n 15 127.0.0.1", child),
                QStringLiteral("F2 需要能够拉起一个子进程扮演\"被钉住的另一个进程\""));
            if (child.hProcess != nullptr)
            {
                auto owned = std::make_unique<FakeWorkbenchServices>();
                FakeWorkbenchServices* fake = owned.get();
                fake->SetProcessModulesResult(child.dwProcessId, ks::ui::ModuleEnumResult{true, {}, {}});
                ks::ui::WorkbenchTarget target(std::move(owned));

                std::vector<int> seq;
                QObject::connect(&target, &ks::ui::WorkbenchTarget::livenessChanged,
                    [&](int s) { seq.push_back(s); });

                // 钉住子进程，首次探测得到 Alive。
                WPI_CHECK(target.requestPin(child.dwProcessId));
                target.checkLiveness();
                WPI_CHECK(!seq.empty() && seq.back() == static_cast<int>(ks::ui::LivenessState::Alive));
                const std::size_t countAfterAlive = seq.size();

                // 钉住态下 Dock 附加/分离另一个进程（会话不变）：不应该让存活指示灯抖动。
                ks::ui::WorkbenchTarget::DockAttach other;
                other.handle = reinterpret_cast<void*>(::GetCurrentProcess());
                other.pid = selfPid;
                other.attachGeneration = 1;
                target.onDockAttached(other);
                WPI_CHECK_NOTE(seq.size() == countAfterAlive,
                    QStringLiteral("N2：钉住态下 Dock 附加另一个进程不应该发出 livenessChanged（修复前会误报 Unknown）"));
                target.onDockDetached();
                WPI_CHECK_NOTE(seq.size() == countAfterAlive,
                    QStringLiteral("N2：钉住态下 Dock 分离不应该发出 livenessChanged"));

                // 空操作：再钉住同一个 pid 不是换目标，不应该抖动。
                WPI_CHECK(target.requestPin(child.dwProcessId));
                WPI_CHECK_NOTE(seq.size() == countAfterAlive,
                    QStringLiteral("N2：requestPin(同一个 pid) 是空操作，不应该发出 livenessChanged"));

                KillPingChild(child);
            }
        }

        // ==== F3（N3，requestIdentity 这一半）：离开守卫弹框期间目标被同步销毁 ====
        {
            auto owned = std::make_unique<FakeWorkbenchServices>();
            auto* targetPtr = new ks::ui::WorkbenchTarget(std::move(owned));
            bool guardRan = false;
            targetPtr->setLeaveGuard([&](ks::ui::LeaveReason) -> bool
                {
                    // 模拟守卫弹出的模态框期间，视图被程序性关闭、目标本身被销毁——
                    // 这正是第二轮审核报告 N3 复现的场景（探针 --guard-delete）。
                    guardRan = true;
                    delete targetPtr;
                    return true; // 守卫本身"批准"离开；批准与否都不应该让后续代码碰已销毁对象。
                });
            // 修复前：requestLeave 返回之后紧接着访问 tracker_/pinnedAnchorHandle_
            // 等成员，连续多次实测必定以 0xC0000005 崩溃；修复后应该干净返回 false。
            const bool result = targetPtr->requestScope(mw::Scope::KernelVirtual);
            WPI_CHECK(guardRan);
            WPI_CHECK_NOTE(!result,
                QStringLiteral("N3：目标在离开守卫期间被销毁，requestIdentity 必须返回 false 而不是继续写已销毁对象的成员"));
            // 没有在上一行之前崩溃，本身就是这条回归测试最重要的断言。
        }

        // ==== F4（N3，session() 这一半）：Ddma 通道下 session() 内部同步发出的
        // sessionChanged 期间目标被同步销毁，同样不能解引用已释放内存 ====
        {
            auto owned = std::make_unique<FakeWorkbenchServices>();
            FakeWorkbenchServices* fake = owned.get();
            fake->SetDdmaGeneration(1);
            auto* targetPtr = new ks::ui::WorkbenchTarget(std::move(owned));
            WPI_CHECK(targetPtr->requestChannel(mw::Channel::Ddma)); // 先切进 Ddma，代次落定为 1。

            bool destroyedInSlot = false;
            QObject::connect(targetPtr, &ks::ui::WorkbenchTarget::sessionChanged,
                [&](quint32)
                {
                    destroyedInSlot = true;
                    delete targetPtr; // 同步销毁：模拟订阅者在槅里关闭了视图。
                });
            fake->SetDdmaGeneration(2); // 制造一次代次漂移，下一次 session() 会同步 ObserveDdma→emit。

            const ksword::memwb::MemoryTargetSession& result = targetPtr->session();
            WPI_CHECK(destroyedInSlot);
            WPI_CHECK_NOTE(result.pid == 0,
                QStringLiteral("N3：session() 内部目标被销毁后应返回安全的占位会话，不应解引用已释放内存"));
            // targetPtr 此刻已经是悬垂指针，本测试到此为止不再使用它。
        }

        // ==== F5（N5）：已在 Ddma 通道时，显式 requestChannel(Ddma) 必须真的问一次
        // services_ 的最新代次，代次漂移时必须问一次离开守卫 ====
        {
            auto owned = std::make_unique<FakeWorkbenchServices>();
            FakeWorkbenchServices* fake = owned.get();
            fake->SetDdmaGeneration(5);
            ks::ui::WorkbenchTarget target(std::move(owned));
            GuardRecorder guard;
            guard.SetApproval(true);
            target.setLeaveGuard(guard.asFunction());

            WPI_CHECK(target.requestChannel(mw::Channel::Ddma)); // 首次切入：Channel 位本身已经是身份变化。
            WPI_CHECK(target.session().ddmaGeneration == 5);

            // 模拟暂存扇区换代，且中途没有任何人调用过 session()/capture()/isStale()
            // 主动拉取——这正是 P4a 复现的场景。
            fake->SetDdmaGeneration(6);
            guard.Reset();
            WPI_CHECK(target.requestChannel(mw::Channel::Ddma));
            WPI_CHECK_NOTE(guard.CallCount() == 1,
                QStringLiteral("N5：已在 Ddma 通道时代次漂移，显式 requestChannel(Ddma) 必须问一次守卫（修复前会漏问）"));
            WPI_CHECK(target.session().ddmaGeneration == 6);

            // 代次没有漂移时的空操作仍然不应该问守卫（避免 N5 修复反而让这个场景多问）。
            guard.Reset();
            WPI_CHECK(target.requestChannel(mw::Channel::Ddma));
            WPI_CHECK_NOTE(guard.CallCount() == 0,
                QStringLiteral("N5：代次没有变化时再次请求 Ddma 仍然是空操作，不应该问守卫"));
        }

        // ==== F6（N4 决策，原 X18）：expectCreateTime 对 weak 锚点无法核对——
        // requestPin 仍然成功，但 identityAnchored()==false、lastIdentityFailure
        // 维持 Ok ====
        {
            PROCESS_INFORMATION child{};
            if (SpawnPingChild(L"cmd.exe /c ping -n 8 127.0.0.1", child))
            {
                bool prepared = false;
                PSECURITY_DESCRIPTOR sd = nullptr;
                // D:(A;;0x100000;;;WD) 只给 Everyone SYNCHRONIZE，连
                // PROCESS_QUERY_LIMITED_INFORMATION 都拿不到，逼出 weak 锚点
                // （同 Extra.cpp 的 X17，那里是 0x101000 还留了 QUERY_LIMITED；
                // 这里故意更紧，连 LIMITED 都不给）。
                if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(
                        L"D:(A;;0x100000;;;WD)", SDDL_REVISION_1, &sd, nullptr) != FALSE)
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
                if (prepared)
                {
                    const ks::ui::AnchorInfo probe = ks::ui::AcquireAnchorForPid(child.dwProcessId);
                    WPI_CHECK_NOTE(probe.identityWeak,
                        QStringLiteral("F6：只给 SYNCHRONIZE 的进程必须拿到 weak 锚点（createTime100ns==0）"));
                    ks::ui::ReleaseAnchorHandle(probe.handle);

                    if (probe.identityWeak)
                    {
                        auto owned = std::make_unique<FakeWorkbenchServices>();
                        ks::ui::WorkbenchTarget target(std::move(owned));
                        // N4 决策：即便 expectCreateTime 明显与实际（weak 锚点下恒为 0）
                        // 不符，也不失败——这是"钉住受保护进程走 R0/HVM/DDMA"的头号用法。
                        const bool pinned = target.requestPin(child.dwProcessId, 0x1122334455667788ULL);
                        WPI_CHECK_NOTE(pinned,
                            QStringLiteral("N4：weak 锚点下 expectCreateTime 核对不了时不应该失败"));
                        WPI_CHECK_NOTE(!target.identityAnchored(),
                            QStringLiteral("N4：但必须可观察——identityAnchored() 必须如实报告\"未锚定\""));
                        WPI_CHECK_NOTE(target.lastIdentityFailure() == ks::ui::NavStatus::Ok,
                            QStringLiteral("N4：不应该把\"核对不了\"误报成某种校验失败"));
                        WPI_CHECK(target.session().pid == child.dwProcessId);
                        WPI_CHECK(target.session().processCreateTime100ns == 0);
                    }
                }
                else
                {
                    std::cout << "F6 skipped: 无法仿真\"连 QUERY_LIMITED 都拒绝\"的受保护进程 (prepared="
                              << prepared << ")" << std::endl;
                }
                KillPingChild(child);
            }
        }

        // 发布前审阅回归：capture 的 DDMA 同步通知销毁目标后，返回值必须完全为空。
        {
            auto owned = std::make_unique<FakeWorkbenchServices>();
            FakeWorkbenchServices* fake = owned.get();
            fake->SetDdmaGeneration(1);
            auto* targetPtr = new ks::ui::WorkbenchTarget(std::move(owned));
            WPI_CHECK(targetPtr->requestChannel(mw::Channel::Ddma));
            targetPtr->noteContentChanged(); // 让旧对象两个代次均非零，不能误当空快照。

            bool destroyedInSlot = false;
            QObject::connect(targetPtr, &ks::ui::WorkbenchTarget::sessionChanged,
                [&](quint32) {
                    destroyedInSlot = true;
                    delete targetPtr;
                });
            fake->SetDdmaGeneration(2);
            const ks::ui::TargetCapture result = targetPtr->capture();
            WPI_CHECK(destroyedInSlot);
            WPI_CHECK(result.session.pid == 0);
            WPI_CHECK_NOTE(result.rev.source == 0 && result.rev.content == 0,
                QStringLiteral("capture 的同步通知销毁目标后不得再读取释放对象的代次"));
        }

        // ==== F7：revisions() 只读访问器——引用随会话变化实时更新，与 capture()
        // 读到的来源/内容代次一致 ====
        {
            auto owned = std::make_unique<FakeWorkbenchServices>();
            ks::ui::WorkbenchTarget target(std::move(owned));
            const ksword::memwb::SessionRevisions& live = target.revisions(); // 只取一次引用。

            const ksword::memwb::RevisionSnapshot before = live.Capture();
            const ks::ui::TargetCapture captureBefore = target.capture();
            WPI_CHECK(before == captureBefore.rev); // 同一份状态，取法不同，读到的必须一致。

            // 会话身份变化（切到内核范围）：来源代次必须随之变化，且这份引用立刻
            // 能读到新值，不需要重新调用 target.revisions() 取一次新引用。
            WPI_CHECK(target.requestScope(mw::Scope::KernelVirtual));
            const ksword::memwb::RevisionSnapshot afterScope = live.Capture();
            WPI_CHECK_NOTE(afterScope.source != before.source,
                QStringLiteral("F7：会话身份变化后，revisions() 这份引用上的来源代次必须随之变化"));
            const ks::ui::TargetCapture captureAfterScope = target.capture();
            WPI_CHECK(afterScope == captureAfterScope.rev);

            // 重读（requestReload）：只 bump 来源代次，引用与 capture() 的变化必须一致。
            target.requestReload();
            const ksword::memwb::RevisionSnapshot afterReload = live.Capture();
            WPI_CHECK(afterReload.source != afterScope.source);
            WPI_CHECK(afterReload.content == afterScope.content);
            WPI_CHECK(afterReload == target.capture().rev);

            // 内容变化（noteContentChanged）：只 bump 内容代次，来源代次不动。
            target.noteContentChanged();
            const ksword::memwb::RevisionSnapshot afterContent = live.Capture();
            WPI_CHECK(afterContent.source == afterReload.source);
            WPI_CHECK(afterContent.content != afterReload.content);
            WPI_CHECK(afterContent == target.capture().rev);
        }

        // ==== F9：存活通知的同步订阅者可以销毁目标，三个身份入口必须停止收尾 ====
        for (int operation = 0; operation < 3; ++operation)
        {
            auto target = std::make_unique<ks::ui::WorkbenchTarget>(std::make_unique<FakeWorkbenchServices>());
            const std::uint32_t pid = static_cast<std::uint32_t>(::GetCurrentProcessId());
            ks::ui::WorkbenchTarget::DockAttach attach;
            attach.handle = reinterpret_cast<void*>(::GetCurrentProcess());
            attach.pid = pid;
            attach.attachGeneration = 1;
            target->onDockAttached(attach);
            target->checkLiveness(); // 先得到 Alive，后面的 Unknown 才会真的发出信号。
            bool deletedByNotification = false;
            QObject::connect(target.get(), &ks::ui::WorkbenchTarget::livenessChanged,
                [&](int state) {
                    if (state == static_cast<int>(ks::ui::LivenessState::Unknown))
                    {
                        deletedByNotification = true;
                        target.reset();
                    }
                });
            auto* selected = target.get();
            if (operation == 0)
            {
                attach.attachGeneration = 2;
                selected->onDockAttached(attach);
            }
            else if (operation == 1)
            {
                selected->onDockDetached();
            }
            else
            {
                WPI_CHECK(!selected->requestPin(pid));
            }
            WPI_CHECK(deletedByNotification);
            WPI_CHECK(target == nullptr);
        }

        // ==== F8（N3 静态兜底闸门）：session()/isStale()/requestIdentity 三处
        // 都必须有 `if (!self)` 判空——F4 的运行期断言实测对"反悔 session() 那
        // 一半"这个具体变异不够用（delete 之后紧跟着的那一次读取，碰巧没有踩
        // 中已经被覆盖的那块堆内存，0 failures 假通过），必须靠源码形态兜底 ====
        {
            std::ifstream in("Ksword5.1\\Ksword5.1\\UI\\MemoryWorkbench\\WorkbenchTarget.cpp", std::ios::binary);
            if (!in)
            {
                std::cout << "F8 skipped: 找不到 WorkbenchTarget.cpp（需要从仓库根目录运行）" << std::endl;
            }
            else
            {
                const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                int selfGuardCount = 0;
                std::size_t pos = 0;
                while ((pos = text.find("if (!self)", pos)) != std::string::npos)
                {
                    ++selfGuardCount;
                    pos += 10;
                }
                WPI_CHECK_NOTE(
                    selfGuardCount >= 3,
                    QStringLiteral(
                        "N3：session()/isStale()/requestIdentity 三处都必须用 QPointer self 在"
                        "applyMaskSideEffects/requestLeave 之后判空，实际只找到 %1 处 `if (!self)`"
                        "（可能有一处被回退）")
                        .arg(selfGuardCount));
            }
        }
    }
}
