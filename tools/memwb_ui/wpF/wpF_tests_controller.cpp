// wpF_tests_controller.cpp
// 作用：Int3Controller 的离屏验证——Install / Restore / RestoreAll 的全部状态分支、
//       路由预检零端口调用、还原通道=安装通道、FORCE_REQUIRED 不自动强制、
//       退出前提示（纯逻辑 + 真实 QMessageBox 三选一 + Esc + 模拟 Enter 驱动默认按钮）、
//       跨目标账本（当前目标口径 vs 全账本口径）、旁路表随 Discard/OnTargetGone/
//       ClearOrphaned 同步清理。
//
// 本文件链接**真实的** ksword::memwb::MemoryPatchByteStore（shared/evidence/memory_workbench），
// 只在最底层换成 wpF_fake_port.h 的假端口：这样这里验证的是生产代码的真实行为。

#include "wpF_fake_port.h"
#include "wpF_tests_common.h"

#include "../../../shared/evidence/memory_workbench/MemoryPatchByteStore.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/Int3Controller.h"

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <iostream>
#include <memory>

namespace wpf_test
{
    using ks::ui::Int3Controller;
    using ks::ui::Int3InstallOutcome;
    using ks::ui::Int3LeaveChoice;
    using ks::ui::Int3LeaveScenario;
    using ks::ui::Int3RestoreOutcome;
    using ks::ui::Int3RouteReject;
    using ksword::memwb::Channel;
    using ksword::memwb::IPatchByteStore;
    using ksword::memwb::InstallStatus;
    using ksword::memwb::kInt3PatchByte;
    using ksword::memwb::MemoryPatchByteStore;
    using ksword::memwb::MemoryTargetSession;
    using ksword::memwb::PatchEntry;
    using ksword::memwb::PatchRestoreOutcome;
    using ksword::memwb::PatchTarget;
    using ksword::memwb::RestoreStatus;
    using ksword::memwb::Scope;

    namespace
    {
        // MakeFactory：把 FakeMemoryIoPort 包成 Int3Controller::ByteStoreFactory。session
        // 必须比每一次返回的 MemoryPatchByteStore 活得更久——这里让 session 由调用方持有，
        // 工厂每次调用只更新它的字段，不新建对象，所以返回的存取器引用始终有效。
        Int3Controller::ByteStoreFactory MakeFactory(FakeMemoryIoPort& port, MemoryTargetSession& session)
        {
            return [&port, &session](const PatchTarget& target, const Channel channel) -> std::unique_ptr<IPatchByteStore> {
                session.scope = Scope::ProcessVirtual;
                session.pid = target.pid;
                session.processCreateTime100ns = target.processCreateTime100ns;
                session.attachGeneration = target.attachGeneration;
                session.channel = channel;
                return std::make_unique<MemoryPatchByteStore>(port, session);
            };
        }
    }

    // RunControllerTests：见文件头说明。
    void RunControllerTests()
    {
        const int checksBefore = g_checks;
        const int failuresBefore = g_failures;

        // ---- Install 全部分支 + 成功还原 ----
        {
            FakeMemoryIoPort port;
            MemoryTargetSession session;
            Int3Controller controller(MakeFactory(port, session));
            const PatchTarget target = MakeTarget(1234, 999);
            controller.SetCurrentContext(target, Scope::ProcessVirtual, Channel::UserMode);
            port.memory[0x1000] = 0x90;

            int changedCount = 0;
            QObject::connect(&controller, &Int3Controller::changed, [&changedCount]() { ++changedCount; });

            const Int3InstallOutcome installed = controller.Install(target, 0x1000, 1);
            CHECK(installed.status == InstallStatus::Installed);
            CHECK(installed.routeReject == Int3RouteReject::None);
            CHECK(port.memory[0x1000] == kInt3PatchByte);
            CHECK(controller.Entries().size() == 1);
            CHECK(controller.InstalledChannel(installed.id).has_value());
            CHECK(controller.InstalledChannel(installed.id).value() == Channel::UserMode);
            CHECK(changedCount == 1);
            CHECK(CountOpenMessageBoxes() == 0);

            const Int3InstallOutcome duplicate = controller.Install(target, 0x1000, 2);
            CHECK(duplicate.status == InstallStatus::Duplicate);

            port.memory[0x2000] = kInt3PatchByte;
            const Int3InstallOutcome already = controller.Install(target, 0x2000, 3);
            CHECK(already.status == InstallStatus::AlreadyContainsPatchByte);

            const Int3InstallOutcome readFailed = controller.Install(target, 0x3000, 4);
            CHECK(readFailed.status == InstallStatus::ReadFailed);
            CHECK(port.writeCalls == 1); // 到此为止只有第一次真正写过

            const Int3RestoreOutcome restored = controller.Restore(installed.id);
            CHECK(restored.status == RestoreStatus::Restored);
            CHECK(port.memory[0x1000] == 0x90);
            CHECK(controller.Entries().empty());
            CHECK(!controller.InstalledChannel(installed.id).has_value());
            CHECK(CountOpenMessageBoxes() == 0);
        }

        // ---- 路由预检：内核/物理范围、磁盘传输通道，零端口调用 ----
        {
            FakeMemoryIoPort port;
            MemoryTargetSession session;
            Int3Controller controller(MakeFactory(port, session));
            const PatchTarget target = MakeTarget(10, 20);

            controller.SetCurrentContext(target, Scope::KernelVirtual, Channel::StandardDriver);
            CHECK(controller.EvaluateCurrentRoute() == Int3RouteReject::UnsupportedScope);
            const Int3InstallOutcome kernelOutcome = controller.Install(target, 0x1000, 1);
            CHECK(kernelOutcome.routeReject == Int3RouteReject::UnsupportedScope);
            CHECK(kernelOutcome.status == InstallStatus::None);
            CHECK(port.readCalls == 0 && port.writeCalls == 0);

            controller.SetCurrentContext(target, Scope::Physical, Channel::StandardDriver);
            const Int3InstallOutcome physicalOutcome = controller.Install(target, 0x1000, 1);
            CHECK(physicalOutcome.routeReject == Int3RouteReject::UnsupportedScope);
            CHECK(port.readCalls == 0 && port.writeCalls == 0);

            controller.SetCurrentContext(target, Scope::ProcessVirtual, Channel::Ddma);
            CHECK(controller.EvaluateCurrentRoute() == Int3RouteReject::UnsupportedChannel);
            const Int3InstallOutcome ddmaOutcome = controller.Install(target, 0x1000, 1);
            CHECK(ddmaOutcome.routeReject == Int3RouteReject::UnsupportedChannel);
            CHECK(port.readCalls == 0 && port.writeCalls == 0);
        }

        // ---- FORCE_REQUIRED 不自动强制：approved 恒为 false ----
        {
            FakeMemoryIoPort port;
            MemoryTargetSession session;
            Int3Controller controller(MakeFactory(port, session));
            const PatchTarget target = MakeTarget(55, 66);
            controller.SetCurrentContext(target, Scope::ProcessVirtual, Channel::StandardDriver);
            port.memory[0x4000] = 0x90;
            port.requireApproval = true;

            const Int3InstallOutcome outcome = controller.Install(target, 0x4000, 1);
            CHECK(outcome.status == InstallStatus::WriteFailed);
            CHECK(port.writeCalls == 1);
            CHECK(port.approvedTrueCount == 0);
            CHECK(port.approvedFalseCount == 1);
            CHECK(port.memory[0x4000] == 0x90);
            CHECK(controller.Entries().empty());
        }

        // ---- VerifyFailed：写入后回读到别的值，不盲写，保留原字节和安装通道 ----
        {
            FakeMemoryIoPort port;
            MemoryTargetSession session;
            Int3Controller controller(MakeFactory(port, session));
            const PatchTarget target = MakeTarget(77, 88);
            controller.SetCurrentContext(target, Scope::ProcessVirtual, Channel::UserMode);
            port.memory[0x5000] = 0x41;
            // 第 1 次 1 字节读是"读原字节"（必须读到真实的 0x41，否则恢复基准就错了）；
            // 第 2 次才是写入后的验证读，这里才覆盖成错误值 0x42，制造 VerifyFailed。
            port.nextReadOverride = 0x42;
            port.overrideAtReadCall = 2;

            const Int3InstallOutcome outcome = controller.Install(target, 0x5000, 1);
            CHECK(outcome.status == InstallStatus::VerifyFailed);
            CHECK(!outcome.rollbackAttempted);
            CHECK(!outcome.rollbackWriteOk);
            CHECK(port.writeCalls == 1);
            CHECK(port.memory[0x5000] == kInt3PatchByte);
            CHECK(outcome.id != 0 && controller.Entries().size() == 1);
            CHECK(controller.InstalledChannel(outcome.id) == Channel::UserMode);
            CHECK(controller.Restore(outcome.id).status == RestoreStatus::Restored);
            CHECK(port.memory[0x5000] == 0x41);
        }

        // ---- 还原通道=安装通道：安装用 UserMode，还原前把当前通道改成 StandardDriver，
        //      还原仍必须用 UserMode（工厂收到的 channel 参数即证据）。----
        {
            FakeMemoryIoPort port;
            MemoryTargetSession session;
            Channel lastRequestedChannel = Channel::UserMode;
            Int3Controller::ByteStoreFactory factory = [&](const PatchTarget& t, const Channel c) {
                lastRequestedChannel = c;
                session.scope = Scope::ProcessVirtual;
                session.pid = t.pid;
                session.processCreateTime100ns = t.processCreateTime100ns;
                session.channel = c;
                return std::unique_ptr<IPatchByteStore>(std::make_unique<MemoryPatchByteStore>(port, session));
            };
            Int3Controller controller(factory);
            const PatchTarget target = MakeTarget(1, 2);
            controller.SetCurrentContext(target, Scope::ProcessVirtual, Channel::UserMode);
            port.memory[0x6000] = 0x50;
            const Int3InstallOutcome outcome = controller.Install(target, 0x6000, 1);
            CHECK(outcome.status == InstallStatus::Installed);
            CHECK(lastRequestedChannel == Channel::UserMode);

            controller.SetCurrentContext(target, Scope::ProcessVirtual, Channel::StandardDriver);
            lastRequestedChannel = Channel::StandardDriver; // 故意弄脏，证明 Restore 会把它改回
            const Int3RestoreOutcome restored = controller.Restore(outcome.id);
            CHECK(restored.status == RestoreStatus::Restored);
            CHECK(lastRequestedChannel == Channel::UserMode);
        }

        // ---- Diverged：仅可丢弃 ----
        {
            FakeMemoryIoPort port;
            MemoryTargetSession session;
            Int3Controller controller(MakeFactory(port, session));
            const PatchTarget target = MakeTarget(3, 4);
            controller.SetCurrentContext(target, Scope::ProcessVirtual, Channel::UserMode);
            port.memory[0x7000] = 0x60;
            const Int3InstallOutcome outcome = controller.Install(target, 0x7000, 1);
            CHECK(outcome.status == InstallStatus::Installed);

            port.memory[0x7000] = 0x77;
            const Int3RestoreOutcome diverged = controller.Restore(outcome.id);
            CHECK(diverged.status == RestoreStatus::Diverged);
            CHECK(diverged.hasObservedByte && diverged.observedByte == 0x77);
            CHECK(port.memory[0x7000] == 0x77);
            CHECK(controller.Entries().size() == 1);

            CHECK(controller.Discard(outcome.id));
            CHECK(controller.Entries().empty());
            CHECK(!controller.Discard(outcome.id));
        }

        // ---- OnTargetGone / 孤立 / RestoreAll 部分成功 / TargetMismatch ----
        {
            FakeMemoryIoPort port;
            MemoryTargetSession session;
            Int3Controller controller(MakeFactory(port, session));
            const PatchTarget target = MakeTarget(5, 6);
            controller.SetCurrentContext(target, Scope::ProcessVirtual, Channel::UserMode);
            port.memory[0x8000] = 0x10;
            port.memory[0x8100] = 0x20;
            const Int3InstallOutcome a = controller.Install(target, 0x8000, 1);
            const Int3InstallOutcome b = controller.Install(target, 0x8100, 2);
            CHECK(a.status == InstallStatus::Installed && b.status == InstallStatus::Installed);

            port.memory[0x8100] = 0x21; // 让第二条 Diverged
            const std::vector<PatchRestoreOutcome> outcomes = controller.RestoreAll();
            CHECK(outcomes.size() == 2);
            int restoredCount = 0;
            int divergedCount = 0;
            for (const PatchRestoreOutcome& outcome : outcomes)
            {
                if (outcome.result.status == RestoreStatus::Restored)
                {
                    ++restoredCount;
                }
                else if (outcome.result.status == RestoreStatus::Diverged)
                {
                    ++divergedCount;
                }
            }
            CHECK(restoredCount == 1 && divergedCount == 1);
            CHECK(controller.Entries().size() == 1);

            const std::vector<PatchEntry> orphaned = controller.OnTargetGone(target.pid, target.processCreateTime100ns);
            CHECK(orphaned.size() == 1);
            CHECK(controller.Entries().empty());
            CHECK(controller.OrphanedEntries().size() == 1);
            CHECK(!controller.HasUnrestored());

            const Int3RestoreOutcome orphanRestore = controller.Restore(orphaned.front().id);
            CHECK(orphanRestore.status == RestoreStatus::Orphaned);

            controller.ClearOrphaned();
            CHECK(controller.OrphanedEntries().empty());
        }
        {
            FakeMemoryIoPort port;
            MemoryTargetSession session;
            Int3Controller controller(MakeFactory(port, session));
            const PatchTarget targetA = MakeTarget(9, 10);
            controller.SetCurrentContext(targetA, Scope::ProcessVirtual, Channel::UserMode);
            port.memory[0x9000] = 0x30;
            const Int3InstallOutcome installed = controller.Install(targetA, 0x9000, 1);
            CHECK(installed.status == InstallStatus::Installed);

            const PatchTarget targetB = MakeTarget(9, 999); // 同 pid，不同创建时间 = 另一个实例
            controller.SetCurrentContext(targetB, Scope::ProcessVirtual, Channel::UserMode);
            const Int3RestoreOutcome mismatch = controller.Restore(installed.id);
            CHECK(mismatch.status == RestoreStatus::TargetMismatch);
            CHECK(port.memory[0x9000] == kInt3PatchByte);
        }

        // ---- 跨目标账本：F-M6/F-M7 的回归测试（2026 年审核报告缺陷 1）。目标 A 上"保留补丁
        //      继续"把条目留在账本里，切到目标 B（故意用同 pid、不同创建时间——这样
        //      pid==pid 为真、createTime 不等，才能把 IsCurrentTarget 里 && 被错改成 || 的
        //      情形与正确实现区分开：|| 会因为 pid 相同就误判 A 的条目属于 B）。
        //      B 自己从未安装过任何补丁，退出判据、文案计数都必须只看 B，不能被 A 的残留
        //      误拦，也不能在"全部还原"里被假装处理掉。----
        {
            FakeMemoryIoPort port;
            MemoryTargetSession session;
            Int3Controller controller(MakeFactory(port, session));

            const PatchTarget targetA = MakeTarget(301, 1);
            controller.SetCurrentContext(targetA, Scope::ProcessVirtual, Channel::UserMode);
            port.memory[0xF000] = 0x90;
            const Int3InstallOutcome a = controller.Install(targetA, 0xF000, 1);
            CHECK(a.status == InstallStatus::Installed);
            CHECK(controller.ApplyLeaveChoice(Int3LeaveChoice::KeepAndContinue)); // 保留，A 的条目留在账本

            const PatchTarget targetB = MakeTarget(301, 2); // 同 pid，不同创建时间 = 另一个实例
            controller.SetCurrentContext(targetB, Scope::ProcessVirtual, Channel::UserMode);

            // "当前目标"口径：B 自己没有残留，不应被 A 的条目误判为"有待还原"。
            CHECK(!controller.HasUnrestoredForCurrentTarget());
            CHECK(controller.CountForCurrentTarget() == 0);
            // 全账本口径仍然如实报告 A 还在——这不是缺陷，是"不假装处理"的证据：A 的
            // 条目没有被隐藏，只是不会再混进"这次离开 B 要处理多少处"的承诺里。
            CHECK(controller.HasUnrestored());
            CHECK(controller.Entries().size() == 1);

            // RequestLeave 在 B 上不应弹框：挂安全网定时器而不是靠"卡死即暴露"——如果判据
            // 回退成全账本口径而误弹框，200ms 后会被强制按 Escape 关掉，断言失败并打印
            // 位置（见 ArmUnexpectedModalWatchdog 注释）。
            ArmUnexpectedModalWatchdog(200);
            CHECK(controller.RequestLeave(nullptr, Int3LeaveScenario::MainWindowClose));
            CHECK(CountOpenMessageBoxes() == 0);

            // 选"全部还原后继续"：B 自己没有残留，调用方得到的是"B 的退出成功"，不是
            // "A 的条目被处理了"——A 的条目原样留在账本里，字节也完全没被碰过。
            CHECK(controller.ApplyLeaveChoice(Int3LeaveChoice::RestoreAllThenContinue));
            CHECK(controller.Entries().size() == 1);
            CHECK(port.memory[0xF000] == kInt3PatchByte);

            // 清理：切回 A 把这条还原掉，不让残留条目影响其它测试文件共享的假设
            // （本文件里其余块都用各自独立的 FakeMemoryIoPort，这里只是保持习惯一致）。
            controller.SetCurrentContext(targetA, Scope::ProcessVirtual, Channel::UserMode);
            CHECK(controller.Restore(a.id).status == RestoreStatus::Restored);
        }

        // ---- 发现 1（2026 年第二轮审核报告）：CountForCurrentTarget() 与退出提示文案的
        //      "正数路径"必须断言具体数值，不能只验证"等于 0"这一种情形（R2A2/R2C4/R2C5/
        //      R2C10 的共同回归点：文案改用全账本计数、++count 改成 (void)count、
        //      .arg(patchCount) 多报一个、遍历对象改成 OrphanedEntries 都要被这里抓住）。
        //      多目标账本下更要断言：当前目标确有残留、账本里还混着另一个目标已保留的
        //      条目时，CountForCurrentTarget() 与弹框文案都必须只报当前目标的真实数量，
        //      不能是账本总数，也不能差 1。----
        {
            FakeMemoryIoPort port;
            MemoryTargetSession session;
            Int3Controller controller(MakeFactory(port, session));

            // 目标 A 上装一条并"保留补丁继续"，留一条干扰条目在账本里。
            const PatchTarget targetA = MakeTarget(501, 1);
            controller.SetCurrentContext(targetA, Scope::ProcessVirtual, Channel::UserMode);
            port.memory[0xF300] = 0x90;
            const Int3InstallOutcome a = controller.Install(targetA, 0xF300, 1);
            CHECK(a.status == InstallStatus::Installed);
            CHECK(controller.ApplyLeaveChoice(Int3LeaveChoice::KeepAndContinue));

            // 切到目标 B，装两条——当前目标确有残留，且数量必须是 2，不是账本总数 3。
            const PatchTarget targetB = MakeTarget(502, 1);
            controller.SetCurrentContext(targetB, Scope::ProcessVirtual, Channel::UserMode);
            port.memory[0xF310] = 0x91;
            port.memory[0xF311] = 0x92;
            const Int3InstallOutcome b1 = controller.Install(targetB, 0xF310, 2);
            const Int3InstallOutcome b2 = controller.Install(targetB, 0xF311, 3);
            CHECK(b1.status == InstallStatus::Installed && b2.status == InstallStatus::Installed);

            // 直接校验具体数值：账本总数是 3（A 的 1 条 + B 的 2 条），当前目标（B）的真实
            // 数量是 2，两者必须能被区分开，不能只验证"不等于 0"这类弱断言。
            CHECK(controller.CountForCurrentTarget() == 2);
            CHECK(controller.Entries().size() == 3);
            CHECK(controller.HasUnrestoredForCurrentTarget());

            // 真实弹框文案里的数字也必须对得上：挂一个定时器在 exec() 的嵌套事件循环里读
            // box 文案，核对"还有 2 处"这个具体数字真的出现在文案里（不是 1、3 或别的数）。
            QString modalText;
            QTimer::singleShot(10, [&modalText]() { modalText = ActiveModalMessageBoxText(); });
            QTimer::singleShot(30, []() { ClickModalButtonByText(QStringLiteral("保留补丁继续")); });
            CHECK(controller.RequestLeave(nullptr, Int3LeaveScenario::DockDetach));
            CHECK(modalText.contains(QStringLiteral("还有 2 处")));
            // 选了"保留"，B 的两条应原样留着，数字应该不变。
            CHECK(controller.CountForCurrentTarget() == 2);

            // 清理：把 A、B 的条目都还原掉，不让残留影响其它测试块。
            controller.SetCurrentContext(targetA, Scope::ProcessVirtual, Channel::UserMode);
            CHECK(controller.Restore(a.id).status == RestoreStatus::Restored);
            controller.SetCurrentContext(targetB, Scope::ProcessVirtual, Channel::UserMode);
            CHECK(controller.Restore(b1.id).status == RestoreStatus::Restored);
            CHECK(controller.Restore(b2.id).status == RestoreStatus::Restored);
        }

        // ---- 发现 2（2026 年第二轮审核报告，第一轮"可疑点 1"的升级确认）：RestoreAll()
        //      自身的目标过滤器即使被完全禁用，也不会被字节/账本大小这类间接判据发现——
        //      下游 Int3PatchLedger::Restore 自己的 TargetMismatch 独立兜底会把混进来的
        //      外来条目吸收掉，不写入任何字节、不报任何错。必须直接断言返回值本身，并
        //      断言端口调用次数恒为 0，证明过滤器在"连端口都不碰"这一层就把外来目标的
        //      条目排除掉了，不是靠下游检测到不一致才放弃（R2C8 的回归点）。----
        {
            FakeMemoryIoPort port;
            MemoryTargetSession session;
            Int3Controller controller(MakeFactory(port, session));

            const PatchTarget targetA = MakeTarget(601, 1);
            controller.SetCurrentContext(targetA, Scope::ProcessVirtual, Channel::UserMode);
            port.memory[0xF400] = 0x90;
            const Int3InstallOutcome a = controller.Install(targetA, 0xF400, 1);
            CHECK(a.status == InstallStatus::Installed);
            CHECK(controller.ApplyLeaveChoice(Int3LeaveChoice::KeepAndContinue)); // 保留，A 的条目留在账本

            const PatchTarget targetB = MakeTarget(601, 2); // 同 pid，不同创建时间：B 自己没有任何残留
            controller.SetCurrentContext(targetB, Scope::ProcessVirtual, Channel::UserMode);
            const int readCallsBefore = port.readCalls;
            const int writeCallsBefore = port.writeCalls;

            const std::vector<PatchRestoreOutcome> outcomes = controller.RestoreAll();
            // 直接断言返回值为空：过滤器一旦被禁用（例如改成 IsCurrentTarget(entry) ||
            // true），会把 A 的条目也收进 snapshot，返回列表就不会是空的——即使下游
            // TargetMismatch 兜底不会损坏数据，这里也必须能独立发现过滤器失效。
            CHECK(outcomes.empty());
            // 端口调用次数恒为 0：证明 A 的条目连"尝试还原"这一步都没有被发起过，
            // 不是发起了但被下游拒绝——过滤器必须在这一层就把 A 排除掉。
            CHECK(port.readCalls == readCallsBefore);
            CHECK(port.writeCalls == writeCallsBefore);
            // A 的条目原样留着，字节也完全没被碰过。
            CHECK(controller.Entries().size() == 1);
            CHECK(port.memory[0xF400] == kInt3PatchByte);

            // 清理。
            controller.SetCurrentContext(targetA, Scope::ProcessVirtual, Channel::UserMode);
            CHECK(controller.Restore(a.id).status == RestoreStatus::Restored);
        }

        // ---- 可疑点 1（2026 年第二轮审核报告）：ClearOrphaned() 里清理旁路表的循环遍历
        //      OrphanedEntries()，在"调用方总是先经过 OnTargetGone"这条当前唯一可达路径
        //      下恒是空操作（OnTargetGone 已经在孤立发生的那一刻清过一遍）。但这不代表
        //      遍历哪个集合不重要：如果遍历对象被错改成 m_ledger.Entries()（R2C7），
        //      ClearOrphaned() 就会连带擦掉"仍待还原、属于另一个目标"条目的旁路表记录——
        //      这是真实的、可观测的行为差异，只是需要跨目标场景才能看到，不需要绕开公开
        //      API。断言：目标 A 已孤立，目标 B 仍是当前目标且有一条待还原记录，调用
        //      ClearOrphaned() 之后，B 那条记录的旁路表必须原样保留——它既没有被孤立，
        //      也不该被"清除孤立项"这个操作波及。----
        {
            FakeMemoryIoPort port;
            MemoryTargetSession session;
            Int3Controller controller(MakeFactory(port, session));

            const PatchTarget targetA = MakeTarget(701, 1);
            controller.SetCurrentContext(targetA, Scope::ProcessVirtual, Channel::UserMode);
            port.memory[0xF500] = 0x93;
            const Int3InstallOutcome a = controller.Install(targetA, 0xF500, 1);
            CHECK(a.status == InstallStatus::Installed);
            controller.OnTargetGone(targetA.pid, targetA.processCreateTime100ns); // A 孤立

            const PatchTarget targetB = MakeTarget(702, 1);
            controller.SetCurrentContext(targetB, Scope::ProcessVirtual, Channel::UserMode);
            port.memory[0xF600] = 0x94;
            const Int3InstallOutcome b = controller.Install(targetB, 0xF600, 1);
            CHECK(b.status == InstallStatus::Installed);
            CHECK(controller.InstalledChannel(b.id).has_value());

            controller.ClearOrphaned();
            CHECK(controller.OrphanedEntries().empty());
            // B 仍是待还原条目，不是孤立项，ClearOrphaned() 不应波及它的旁路表记录。
            CHECK(controller.InstalledChannel(b.id).has_value());
            CHECK(controller.Entries().size() == 1);

            CHECK(controller.Restore(b.id).status == RestoreStatus::Restored); // 清理
        }

        // ---- Discard 后旁路表同步清理（F-M11 的测试缺口：生产代码本就正确——
        //      `if (ok) { m_installChannel.erase(id); }`——这里补上此前从未存在过的断言）。----
        {
            FakeMemoryIoPort port;
            MemoryTargetSession session;
            Int3Controller controller(MakeFactory(port, session));
            const PatchTarget target = MakeTarget(401, 1);
            controller.SetCurrentContext(target, Scope::ProcessVirtual, Channel::UserMode);
            port.memory[0xF100] = 0x33;
            const Int3InstallOutcome outcome = controller.Install(target, 0xF100, 1);
            CHECK(outcome.status == InstallStatus::Installed);
            CHECK(controller.InstalledChannel(outcome.id).has_value());

            port.memory[0xF100] = 0x34; // 别处改过，制造 Diverged，这样才能走到 Discard
            CHECK(controller.Restore(outcome.id).status == RestoreStatus::Diverged);
            CHECK(controller.Discard(outcome.id));
            CHECK(!controller.InstalledChannel(outcome.id).has_value()); // 旁路表同步清理
        }

        // ---- OnTargetGone / ClearOrphaned 同步清理旁路表（2026 年审核报告缺陷 3）：条目一
        //      旦被孤立，账本对它的 Restore 永远直接返回 Orphaned、不会发起任何 I/O，旁路
        //      表里的安装通道记录如果不清，会随"安装→目标退出→清除"的循环无界增长。
        //      断言同时覆盖两个清理点：孤立发生的那一刻（OnTargetGone）就应该已经清掉，
        //      不用等到后面调用 ClearOrphaned。----
        {
            FakeMemoryIoPort port;
            MemoryTargetSession session;
            Int3Controller controller(MakeFactory(port, session));
            const PatchTarget target = MakeTarget(402, 1);
            controller.SetCurrentContext(target, Scope::ProcessVirtual, Channel::UserMode);
            port.memory[0xF200] = 0x35;
            const Int3InstallOutcome outcome = controller.Install(target, 0xF200, 1);
            CHECK(outcome.status == InstallStatus::Installed);
            CHECK(controller.InstalledChannel(outcome.id).has_value());

            controller.OnTargetGone(target.pid, target.processCreateTime100ns);
            CHECK(controller.OrphanedEntries().size() == 1);
            // 修复后：孤立发生的那一刻，旁路表记录就已经被清掉，不用等到 ClearOrphaned。
            CHECK(!controller.InstalledChannel(outcome.id).has_value());

            controller.ClearOrphaned();
            CHECK(controller.OrphanedEntries().empty());
        }

        std::cout << "wpF controller tests: " << (g_checks - checksBefore) << " checks, "
                  << (g_failures - failuresBefore) << " failures" << std::endl;
    }

    // RunLeavePromptTests：退出前提示——纯逻辑 + 真实弹出的 QMessageBox 三选一 + Esc。
    void RunLeavePromptTests()
    {
        const int checksBefore = g_checks;
        const int failuresBefore = g_failures;

        CHECK(Int3Controller::NeedsLeavePrompt(true, Int3LeaveScenario::DockDetach));
        CHECK(Int3Controller::NeedsLeavePrompt(true, Int3LeaveScenario::ProcessChange));
        CHECK(Int3Controller::NeedsLeavePrompt(true, Int3LeaveScenario::MainWindowClose));
        CHECK(!Int3Controller::NeedsLeavePrompt(false, Int3LeaveScenario::DockDetach));

        // ApplyLeaveChoice 纯逻辑（不经弹框）。
        {
            FakeMemoryIoPort port;
            MemoryTargetSession session;
            Int3Controller controller(MakeFactory(port, session));
            const PatchTarget target = MakeTarget(11, 12);
            controller.SetCurrentContext(target, Scope::ProcessVirtual, Channel::UserMode);
            port.memory[0xA000] = 0x40;
            const Int3InstallOutcome outcome = controller.Install(target, 0xA000, 1);
            CHECK(outcome.status == InstallStatus::Installed);

            CHECK(!controller.ApplyLeaveChoice(Int3LeaveChoice::Cancel));
            CHECK(controller.HasUnrestored());

            CHECK(controller.ApplyLeaveChoice(Int3LeaveChoice::KeepAndContinue));
            CHECK(controller.HasUnrestored());

            CHECK(controller.ApplyLeaveChoice(Int3LeaveChoice::RestoreAllThenContinue));
            CHECK(!controller.HasUnrestored());

            port.memory[0xA100] = 0x41;
            const Int3InstallOutcome second = controller.Install(target, 0xA100, 2);
            CHECK(second.status == InstallStatus::Installed);
            port.memory[0xA100] = 0x99; // 让它变成 Diverged
            CHECK(!controller.ApplyLeaveChoice(Int3LeaveChoice::RestoreAllThenContinue));
            CHECK(controller.HasUnrestored());
            controller.Discard(second.id);
        }

        // changed 回调切换当前目标，不得把旧目标的还原失败当成新目标无残留的成功。
        {
            FakeMemoryIoPort port;
            MemoryTargetSession session;
            Int3Controller controller(MakeFactory(port, session));
            const PatchTarget targetA = MakeTarget(31, 32);
            const PatchTarget targetB = MakeTarget(33, 34);
            controller.SetCurrentContext(targetA, Scope::ProcessVirtual, Channel::UserMode);
            port.memory[0xBA00] = 0x61;
            const Int3InstallOutcome installed = controller.Install(targetA, 0xBA00, 1);
            port.memory[0xBA00] = 0x62; // 第三方改变，Restore 必须失败且不写。
            bool contextChanged = false;
            const auto connection = QObject::connect(&controller, &Int3Controller::changed, [&]() {
                if (contextChanged)
                {
                    return;
                }
                contextChanged = true;
                controller.SetCurrentContext(targetB, Scope::ProcessVirtual, Channel::StandardDriver);
            });
            CHECK(!controller.ApplyLeaveChoice(Int3LeaveChoice::RestoreAllThenContinue));
            QObject::disconnect(connection);
            CHECK(controller.CurrentTarget().pid == targetB.pid);
            CHECK(controller.CurrentScope() == Scope::ProcessVirtual);
            CHECK(controller.HasUnrestored() && !controller.HasUnrestoredForCurrentTarget());
            CHECK(port.memory[0xBA00] == 0x62);
            CHECK(controller.Discard(installed.id));
        }

        // changed 回调删除控制器时，退出动作不能继续访问已释放的账本。
        {
            FakeMemoryIoPort port;
            MemoryTargetSession session;
            auto* controller = new Int3Controller(MakeFactory(port, session));
            const QPointer<Int3Controller> guard(controller);
            const PatchTarget target = MakeTarget(35, 36);
            controller->SetCurrentContext(target, Scope::ProcessVirtual, Channel::UserMode);
            port.memory[0xBB00] = 0x63;
            controller->Install(target, 0xBB00, 1);
            QObject::connect(controller, &Int3Controller::changed, [controller]() { delete controller; });
            CHECK(!controller->ApplyLeaveChoice(Int3LeaveChoice::RestoreAllThenContinue));
            CHECK(!guard);
            CHECK(port.memory[0xBB00] == 0x63);
        }

        // RequestLeave：无待还原时不弹框。挂一个安全网定时器（见 ArmUnexpectedModalWatchdog
        // 注释）：如果 NeedsLeavePrompt 被改错而误弹框（审核报告 F-M8 的判断点），200ms 后
        // 会被强制按 Escape 关掉，让下面的 CHECK(controller.RequestLeave(...)) 断言失败并
        // 打印位置，而不是让整个夹具卡死在 exec() 里、只能靠外层跑超时才能发现。
        {
            FakeMemoryIoPort port;
            MemoryTargetSession session;
            Int3Controller controller(MakeFactory(port, session));
            ArmUnexpectedModalWatchdog(200);
            CHECK(controller.RequestLeave(nullptr, Int3LeaveScenario::DockDetach));
            CHECK(CountOpenMessageBoxes() == 0);
        }

        // RequestLeave：三种选择 + Esc，各自驱动一次真实弹出的 QMessageBox。
        {
            FakeMemoryIoPort port;
            MemoryTargetSession session;
            Int3Controller controller(MakeFactory(port, session));
            const PatchTarget target = MakeTarget(21, 22);
            controller.SetCurrentContext(target, Scope::ProcessVirtual, Channel::UserMode);
            port.memory[0xB000] = 0x50;
            controller.Install(target, 0xB000, 1);
            QTimer::singleShot(30, []() { ClickModalButtonByText(QStringLiteral("全部还原后继续")); });
            const bool allowed = controller.RequestLeave(nullptr, Int3LeaveScenario::MainWindowClose);
            CHECK(allowed);
            CHECK(!controller.HasUnrestored());
            CHECK(port.memory[0xB000] == 0x50);
        }
        {
            FakeMemoryIoPort port;
            MemoryTargetSession session;
            Int3Controller controller(MakeFactory(port, session));
            const PatchTarget target = MakeTarget(23, 24);
            controller.SetCurrentContext(target, Scope::ProcessVirtual, Channel::UserMode);
            port.memory[0xB100] = 0x51;
            controller.Install(target, 0xB100, 1);
            QTimer::singleShot(30, []() { ClickModalButtonByText(QStringLiteral("保留补丁继续")); });
            const bool allowed = controller.RequestLeave(nullptr, Int3LeaveScenario::ProcessChange);
            CHECK(allowed);
            CHECK(controller.HasUnrestored());
        }
        {
            FakeMemoryIoPort port;
            MemoryTargetSession session;
            Int3Controller controller(MakeFactory(port, session));
            const PatchTarget target = MakeTarget(25, 26);
            controller.SetCurrentContext(target, Scope::ProcessVirtual, Channel::UserMode);
            port.memory[0xB200] = 0x52;
            controller.Install(target, 0xB200, 1);
            QTimer::singleShot(30, []() { ClickModalButtonByText(QStringLiteral("取消")); });
            const bool allowed = controller.RequestLeave(nullptr, Int3LeaveScenario::DockDetach);
            CHECK(!allowed);
            CHECK(controller.HasUnrestored());
        }
        {
            FakeMemoryIoPort port;
            MemoryTargetSession session;
            Int3Controller controller(MakeFactory(port, session));
            const PatchTarget target = MakeTarget(27, 28);
            controller.SetCurrentContext(target, Scope::ProcessVirtual, Channel::UserMode);
            port.memory[0xB300] = 0x53;
            controller.Install(target, 0xB300, 1);
            QTimer::singleShot(30, []() { PressEscapeOnModal(); });
            const bool allowed = controller.RequestLeave(nullptr, Int3LeaveScenario::DockDetach);
            CHECK(!allowed);
            CHECK(controller.HasUnrestored());
        }

        // RequestLeave：默认按钮必须是"全部还原后继续"（2026 年审核报告缺陷 2——曾经默认
        // 按钮与 Esc 都等价于"取消"，与用户按 Enter 的直觉相反，也与规格明文的"默认全部
        // 还原后继续"相反）。用真实弹出的 QMessageBox，模拟按 Enter 驱动默认按钮，而不是
        // 直接点某个按钮指针——这样断言的是"谁被设成了默认按钮"，不是夹具自己猜的结果。
        {
            FakeMemoryIoPort port;
            MemoryTargetSession session;
            Int3Controller controller(MakeFactory(port, session));
            const PatchTarget target = MakeTarget(29, 30);
            controller.SetCurrentContext(target, Scope::ProcessVirtual, Channel::UserMode);
            port.memory[0xB400] = 0x54;
            controller.Install(target, 0xB400, 1);
            QTimer::singleShot(30, []() { PressEnterOnModal(); });
            const bool allowed = controller.RequestLeave(nullptr, Int3LeaveScenario::MainWindowClose);
            CHECK(allowed);
            CHECK(!controller.HasUnrestored()); // 默认按钮等价于"全部还原后继续"，账本应已清空
            CHECK(port.memory[0xB400] == 0x54); // 真的写回了原字节，不是碰巧 Cancel 也返回 true
        }

        // 模态期间另一工作台激活目标 B：选择只还原提示中的 A，保持 B 的共享上下文。
        {
            FakeMemoryIoPort port;
            MemoryTargetSession session;
            Int3Controller controller(MakeFactory(port, session));
            const PatchTarget targetA = MakeTarget(41, 42);
            const PatchTarget targetB = MakeTarget(43, 44);
            controller.SetCurrentContext(targetA, Scope::ProcessVirtual, Channel::UserMode);
            port.memory[0xBC00] = 0x64;
            controller.Install(targetA, 0xBC00, 1);
            controller.SetCurrentContext(targetB, Scope::ProcessVirtual, Channel::StandardDriver);
            port.memory[0xBD00] = 0x65;
            controller.Install(targetB, 0xBD00, 2);
            controller.SetCurrentContext(targetA, Scope::ProcessVirtual, Channel::UserMode);
            QObject timerContext;
            QTimer::singleShot(0, &timerContext, [&]() {
                CHECK(ActiveModalMessageBoxText().contains(QStringLiteral("1 处")));
                controller.SetCurrentContext(targetB, Scope::ProcessVirtual, Channel::StandardDriver);
                ClickModalButtonByText(QStringLiteral("全部还原后继续"));
            });
            CHECK(controller.RequestLeave(nullptr, Int3LeaveScenario::ProcessChange));
            CHECK(port.memory[0xBC00] == 0x64 && port.memory[0xBD00] == kInt3PatchByte);
            CHECK(session.pid == targetA.pid && session.channel == Channel::UserMode);
            CHECK(controller.Entries().size() == 1 && controller.Entries().front().pid == targetB.pid);
            CHECK(controller.CurrentTarget().pid == targetB.pid);
            CHECK(controller.CurrentScope() == Scope::ProcessVirtual);
            CHECK(controller.CurrentChannel() == Channel::StandardDriver);
            controller.SetCurrentContext(targetB, Scope::ProcessVirtual, Channel::StandardDriver);
            CHECK(controller.RestoreAll().size() == 1);
        }

        // 模态期间 owner 被销毁：对话框关闭后只返回取消，不再读取其成员。
        {
            FakeMemoryIoPort port;
            MemoryTargetSession session;
            auto* controller = new Int3Controller(MakeFactory(port, session));
            const QPointer<Int3Controller> guard(controller);
            const PatchTarget target = MakeTarget(45, 46);
            controller->SetCurrentContext(target, Scope::ProcessVirtual, Channel::UserMode);
            port.memory[0xBE00] = 0x66;
            controller->Install(target, 0xBE00, 1);
            QObject timerContext;
            QTimer::singleShot(0, &timerContext, [controller]() {
                delete controller;
                ClickModalButtonByText(QStringLiteral("全部还原后继续"));
            });
            CHECK(!controller->RequestLeave(nullptr, Int3LeaveScenario::DockDetach));
            CHECK(!guard);
            CHECK(port.memory[0xBE00] == kInt3PatchByte && port.writeCalls == 1);
            CHECK(CountOpenMessageBoxes() == 0);
        }

        // 模态期间父窗口销毁：其 QMessageBox 子对象也被删除，不能再析构栈子对象。
        {
            FakeMemoryIoPort port;
            MemoryTargetSession session;
            Int3Controller controller(MakeFactory(port, session));
            const PatchTarget target = MakeTarget(47, 48);
            controller.SetCurrentContext(target, Scope::ProcessVirtual, Channel::UserMode);
            port.memory[0xBF00] = 0x67;
            controller.Install(target, 0xBF00, 1);
            auto* parent = new QWidget;
            const QPointer<QWidget> parentGuard(parent);
            QObject timerContext;
            QTimer::singleShot(0, &timerContext, [parent]() { delete parent; });
            CHECK(!controller.RequestLeave(parent, Int3LeaveScenario::MainWindowClose));
            CHECK(!parentGuard);
            CHECK(controller.HasUnrestored() && port.writeCalls == 1);
            CHECK(CountOpenMessageBoxes() == 0);
        }

        std::cout << "wpF leave-prompt tests: " << (g_checks - checksBefore) << " checks, "
                  << (g_failures - failuresBefore) << " failures" << std::endl;
    }
}
