// ============================================================
// wpJ2_tests.Stale.cpp
// 作用：覆盖接口文档 §7 的两条关键判断——"整批陈旧丢弃（请求后改来源代次）"
// 与"DDMA 换代后落地的旧页被判陈旧（先拉取再比较）"。后者正是
// WorkbenchPageProvider.h 对 isSourceFresh 的契约重点（自查清单 f 项）：
// DDMA 暂存扇区代次没有可靠回调，比较之前必须先调用一次 target_->session()
// 把最新代次拉回来，否则会把"换了暂存扇区之后才落地的旧页"误判成新鲜。
// ============================================================

#include "wpJ2_common.h"

using namespace ks::ui;
using namespace ksword::memwb;

namespace wpJ2_test
{
    namespace
    {
        // Test_SourceRevisionChangedBeforeLanding_CancelsAll：请求发起后、结果
        // 落地前，target 发生一次"来源代次 +1"的身份类变更（这里用
        // requestReload 最简单地触发它，不牵扯任何策略/守卫）。断言：原本会
        // 交付的 Valid 页最终没有被交付（画布仍是 NotLoaded），而不是"反正内容
        // 一样就算了"——isSourceFresh 的核对必须是严格的代次比较，不是内容
        // 比较。
        void Test_SourceRevisionChangedBeforeLanding_CancelsAll()
        {
            ProviderFixture fixture = MakeProviderFixture(Channel::StandardDriver);
            fixture.provider->setGateInputsProvider(
                [channel = Channel::StandardDriver]() { return MakeAvailableGateInputs(channel); });
            // 加一点延迟，确保主线程有机会在"端口正在读"的窗口里推进 target
            // 的代次，而不是让读取在我们还没来得及改之前就已经落地。
            fixture.portPtr->SetDelayMs(80);
            fixture.portPtr->SetScript({MakeOk(MakePattern(0x30, static_cast<std::size_t>(kPageSize)))});

            // revisionBeforeReload：**目标轴**在发起请求那一刻的值——陈旧性
            // 核对比较的是这个轴，不是传给 RequestPages 的 canvasRevision
            // 参数（Wave 3 决策 2：两条数轴独立，provider 内部自己 capture()，
            // 测试这里只是为了下面的"确实变化了"断言才显式读一次）。
            const std::uint64_t revisionBeforeReload = fixture.targetFixture.target->capture().rev.source;
            const HexFetchRange range{kFixtureFirstAddress, 1, 0};
            fixture.provider->RequestPages({range}, fixture.canvasRevision);

            // 在读取仍在"端口延迟"期间，让 target 的来源代次往前走一步（不涉及
            // 任何身份字段变化，纯粹是"用户按了重读"这类事件）。
            fixture.targetFixture.target->requestReload();
            const std::uint64_t revisionAfterReload = fixture.targetFixture.target->capture().rev.source;
            WPJ2_CHECK_NOTE(
                revisionAfterReload != revisionBeforeReload,
                QStringLiteral("requestReload 之后目标轴来源代次应该变化，测试前提不成立"));

            // 等端口真正完成这一次调用（给够时间，80ms 延迟 + 事件循环调度）。
            const bool portCalled = PumpUntil([&]() { return fixture.portPtr->CallCount() >= 1; }, 2000);
            WPJ2_CHECK(portCalled);
            // 再等一轮事件循环，确保排队的 UI 线程回调已经处理完。
            PumpUntil([]() { return false; }, 100);

            const HexCanvas::CellState cell = fixture.canvas->cellStateAt(kFixtureFirstAddress);
            WPJ2_CHECK_NOTE(
                cell.byteState != HexCanvas::ByteState::Valid,
                QStringLiteral("来源代次已变，陈旧结果不应该被交付成 Valid"));
        }

        // Test_DdmaGenerationChange_DetectedViaPullBeforeCompare：通道切到
        // Ddma，请求发起时 FakeServices 的 ddmaGeneration()==1；在读取仍处于
        // 延迟期间，把 ddmaGeneration 改成 2——这个变化没有任何回调通知
        // WorkbenchTarget，必须靠 isSourceFresh 内部再主动拉一次才能发现。
        // 断言：最终结果被当成陈旧丢弃，不是误判成新鲜。
        void Test_DdmaGenerationChange_DetectedViaPullBeforeCompare()
        {
            ProviderFixture fixture = MakeProviderFixture(Channel::Ddma);
            fixture.provider->setGateInputsProvider(
                [channel = Channel::Ddma]() { return MakeAvailableGateInputs(channel); });
            fixture.targetFixture.services->SetDdmaGeneration(1);
            // 显式拉一次会话，确保接下来 RequestPages 开头的 capture() 读到的
            // 目标轴对应的就是"DDMA 代次=1"这个状态。
            (void)fixture.targetFixture.target->session();

            fixture.portPtr->SetDelayMs(80);
            fixture.portPtr->SetScript({MakeOk(MakePattern(0x40, static_cast<std::size_t>(kPageSize)))});

            const HexFetchRange range{kFixtureFirstAddress, 1, 0};
            fixture.provider->RequestPages({range}, fixture.canvasRevision);

            // 没有任何回调，直接改变量——这正是"没有可靠回调"这件事本身。
            fixture.targetFixture.services->SetDdmaGeneration(2);

            const bool portCalled = PumpUntil([&]() { return fixture.portPtr->CallCount() >= 1; }, 2000);
            WPJ2_CHECK(portCalled);
            PumpUntil([]() { return false; }, 100);

            const HexCanvas::CellState cell = fixture.canvas->cellStateAt(kFixtureFirstAddress);
            WPJ2_CHECK_NOTE(
                cell.byteState != HexCanvas::ByteState::Valid,
                QStringLiteral("DDMA 换代后应该判陈旧，不应该把旧代次的页交付成 Valid"));
        }
    }

    void RunStaleTests()
    {
        Test_SourceRevisionChangedBeforeLanding_CancelsAll();
        Test_DdmaGenerationChange_DetectedViaPullBeforeCompare();
    }
}
