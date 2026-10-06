// ============================================================
// wpJ2_tests.Serialize.cpp
// 作用：覆盖接口文档 §7 的两条关键判断——"并发 RequestPages 时端口侧观察到
// 的调用永不重叠（单线程池）"与"换目标后旧结果必须被丢弃（cancelAllInFlight）"。
// ============================================================

#include "wpJ2_common.h"

using namespace ks::ui;
using namespace ksword::memwb;

namespace wpJ2_test
{
    namespace
    {
        // Test_ConcurrentRequests_NeverOverlapOnPort：背靠背发起两次
        // RequestPages（覆盖不同的页，互不依赖结果），端口对每次调用都睡眠
        // 50ms——如果 ReadPool 不是真正的单线程串行池，这两次调用会在端口内部
        // 同时出现，FakeAsyncMemoryIoPort::MaxConcurrentCalls() 就会量到 2。
        void Test_ConcurrentRequests_NeverOverlapOnPort()
        {
            ProviderFixture fixture = MakeProviderFixture(Channel::StandardDriver);
            fixture.provider->setGateInputsProvider(
                [channel = Channel::StandardDriver]() { return MakeAvailableGateInputs(channel); });
            fixture.portPtr->SetDelayMs(50);
            fixture.portPtr->SetScript(
                {MakeOk(MakePattern(0xA0, static_cast<std::size_t>(kPageSize))),
                 MakeOk(MakePattern(0xB0, static_cast<std::size_t>(kPageSize)))});

            const HexFetchRange rangeA{kFixtureFirstAddress, 1, 0};
            const HexFetchRange rangeB{kFixtureFirstAddress + kPageSize, 1, 0};
            fixture.provider->RequestPages({rangeA}, fixture.canvasRevision);
            fixture.provider->RequestPages({rangeB}, fixture.canvasRevision);

            const bool bothLanded = PumpUntil(
                [&]()
                {
                    return fixture.canvas->cellStateAt(kFixtureFirstAddress).byteState != HexCanvas::ByteState::NotLoaded
                        && fixture.canvas->cellStateAt(kFixtureFirstAddress + kPageSize).byteState
                               != HexCanvas::ByteState::NotLoaded;
                },
                3000);
            WPJ2_CHECK(bothLanded);
            WPJ2_CHECK(fixture.portPtr->CallCount() == 2);
            WPJ2_CHECK_NOTE(
                fixture.portPtr->MaxConcurrentCalls() <= 1,
                QStringLiteral("端口侧观察到了重叠调用，MaxConcurrentCalls=%1")
                    .arg(fixture.portPtr->MaxConcurrentCalls()));

            WPJ2_CHECK(fixture.canvas->cellStateAt(kFixtureFirstAddress).byteState == HexCanvas::ByteState::Valid);
            WPJ2_CHECK(
                fixture.canvas->cellStateAt(kFixtureFirstAddress + kPageSize).byteState == HexCanvas::ByteState::Valid);
        }

        // Test_CancelAllInFlight_DropsLateResult：请求发起后（端口仍在延迟
        // 期间）调用 cancelAllInFlight，断言：这一页最终没有被交付成 Valid
        // （结果被整批丢弃，不碰画布），即使端口脚本本身是成功的 Ok。
        void Test_CancelAllInFlight_DropsLateResult()
        {
            ProviderFixture fixture = MakeProviderFixture(Channel::StandardDriver);
            fixture.provider->setGateInputsProvider(
                [channel = Channel::StandardDriver]() { return MakeAvailableGateInputs(channel); });
            fixture.portPtr->SetDelayMs(80);
            fixture.portPtr->SetScript({MakeOk(MakePattern(0xC0, static_cast<std::size_t>(kPageSize)))});

            const HexFetchRange range{kFixtureFirstAddress, 1, 0};
            fixture.provider->RequestPages({range}, fixture.canvasRevision);
            fixture.provider->cancelAllInFlight();

            // 给够时间让端口完成这一次调用、结果排队落地（即便被丢弃，也要
            // 真正跑过这段事件循环，而不是因为我们没等就误判"没出问题"）。
            PumpUntil([]() { return false; }, 400);

            WPJ2_CHECK_NOTE(
                fixture.canvas->cellStateAt(kFixtureFirstAddress).byteState != HexCanvas::ByteState::Valid,
                QStringLiteral("cancelAllInFlight 之后的在途结果不应该被交付"));
        }
    }

    void RunSerializeTests()
    {
        Test_ConcurrentRequests_NeverOverlapOnPort();
        Test_CancelAllInFlight_DropsLateResult();
    }
}
