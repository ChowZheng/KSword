// memwb_wpI_tests.Ddma.cpp
// 作用：验证 WorkbenchTarget 每次取会话（session()/capture()）时对 DDMA 代次的
//       拉取规则——只有通道=Ddma 才调用 services.ddmaGeneration()；代次变化时
//       通过 tracker.ObserveDdma 体现为 sessionChanged(Ddma 位)。
//
// 断言全部用"调用前后差了多少"而不是硬编码绝对值：session() 本身不是纯访问器，
// 每读一次字段都会重新触发一次 ObserveDdma，用绝对值很容易被测试代码自己
// 多读一次字段的写法悄悄破坏；用差值只关心"这一步到底新增了几次调用"。

#include "memwb_wpI_common.h"

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchTarget.h"

#include <memory>

namespace memwb_wpI_test
{
    void RunDdmaTests()
    {
        auto servicesOwned = std::make_unique<FakeWorkbenchServices>();
        FakeWorkbenchServices* fake = servicesOwned.get();
        fake->SetDdmaGeneration(100);

        ks::ui::WorkbenchTarget target(std::move(servicesOwned));

        int sessionChangedCount = 0;
        quint32 lastMask = 0;
        QObject::connect(&target, &ks::ui::WorkbenchTarget::sessionChanged,
            [&](quint32 mask) { ++sessionChangedCount; lastMask = mask; });

        // 通道仍是默认的 UserMode：取一次会话不应该去问 ddmaGeneration()。
        {
            const int before = fake->DdmaGenerationCallCount();
            (void)target.session();
            WPI_CHECK(fake->DdmaGenerationCallCount() == before);
        }

        // 切到 Ddma 通道：D5 修复后 requestChannel 不再接受 ddmaGeneration 参数，
        // 内部自己向 services_ 要当前代次（这里配置的是 100），调用方不需要也
        // 不能再传一份可能过期的值。
        GuardRecorder allowGuard;
        allowGuard.SetApproval(true);
        target.setLeaveGuard(allowGuard.asFunction());
        const int ddmaCallsBeforeSwitch = fake->DdmaGenerationCallCount();
        WPI_CHECK(target.requestChannel(ksword::memwb::Channel::Ddma));
        // D5 回归：切换本身必须恰好调用一次 ddmaGeneration()（取初值），且这一次
        // 切换只应该产生一次 sessionChanged，不应该因为"缺省代次 0 与真实代次不符"
        // 而在紧接着的下一次取会话时又补发一次。
        WPI_CHECK(fake->DdmaGenerationCallCount() == ddmaCallsBeforeSwitch + 1);
        {
            const ksword::memwb::MemoryTargetSession afterSwitch = target.session();
            WPI_CHECK(afterSwitch.channel == ksword::memwb::Channel::Ddma);
            WPI_CHECK(afterSwitch.ddmaGeneration == 100);
        }

        // T5（锁定 D5）：缺省代次 0 与真实代次不符导致"紧跟着第二次 sessionChanged"
        // 的旧缺陷，结构上已经不可能再发生（没有缺省代次这个入参了）；这里直接
        // 断言切换本身只产生了一次信号。
        WPI_CHECK_NOTE(
            sessionChangedCount == 1,
            QStringLiteral("切到 Ddma 通道应当只发一次 sessionChanged，实际发了 %1 次")
                .arg(sessionChangedCount));
        const int sessionChangedAfterSwitch = sessionChangedCount;

        // 代次未变：再取一次会话，应该恰好新增一次 ddmaGeneration() 调用，
        // 但因为代次没变，不应该产生新的 sessionChanged。
        {
            const int before = fake->DdmaGenerationCallCount();
            (void)target.session();
            WPI_CHECK(fake->DdmaGenerationCallCount() == before + 1);
            WPI_CHECK(sessionChangedCount == sessionChangedAfterSwitch);
        }

        // 模拟暂存扇区换代：services 返回的代次变化，下一次取会话应该发一次
        // 带 Ddma 位的 sessionChanged，且读到的会话里代次已经更新。
        fake->SetDdmaGeneration(101);
        {
            const int before = fake->DdmaGenerationCallCount();
            const ksword::memwb::MemoryTargetSession afterBump = target.session();
            WPI_CHECK(fake->DdmaGenerationCallCount() == before + 1);
            WPI_CHECK(sessionChangedCount == sessionChangedAfterSwitch + 1);
            WPI_CHECK((lastMask & static_cast<quint32>(ksword::memwb::TargetChange::Ddma)) != 0);
            WPI_CHECK(afterBump.ddmaGeneration == 101);
        }

        // 再取一次（代次没变）不应再触发信号；capture() 走同一条路径，也只调用一次。
        {
            const int sessionChangedBeforeCapture = sessionChangedCount;
            const int before = fake->DdmaGenerationCallCount();
            const ks::ui::TargetCapture captured = target.capture();
            WPI_CHECK(captured.session.ddmaGeneration == 101);
            WPI_CHECK(fake->DdmaGenerationCallCount() == before + 1);
            WPI_CHECK(sessionChangedCount == sessionChangedBeforeCapture);
        }

        // 离开 Ddma 通道后，再取会话不应再调用 ddmaGeneration()，会话里的代次归零。
        WPI_CHECK(target.requestChannel(ksword::memwb::Channel::UserMode));
        {
            const int before = fake->DdmaGenerationCallCount();
            const ksword::memwb::MemoryTargetSession afterLeaving = target.session();
            WPI_CHECK(fake->DdmaGenerationCallCount() == before);
            WPI_CHECK(afterLeaving.ddmaGeneration == 0);
        }
    }
}
