// ============================================================
// wpJ2_tests.Gate.cpp
// 作用：覆盖 WorkbenchPageProvider 读路径 R1 的 Gate 判定——接口文档 §7 对本包
// 的关键判断第一条："Gate 不可用时端口 Read 调用次数恒为 0"，以及对称的
// "Gate 可用时正常发起读取并逐页回填"的happy path（后者是后续陈旧性/状态
// 映射等测试能够成立的地基，这里先钉住最基本的一条）。
// ============================================================

#include "wpJ2_common.h"

using namespace ks::ui;
using namespace ksword::memwb;

namespace wpJ2_test
{
    namespace
    {
        // Test_GateUnavailable_NoPortCalls：没有注入 gateInputsProvider_（也就是
        // "全部最保守"的默认 GateInputs），对 KernelVirtual+StandardDriver 发起
        // 请求——driverLoaded 默认为 false，Gate 判定必定不可用（DriverNotLoaded）。
        // 断言：端口 Read 调用次数恒为 0；对应的页在画布里仍是"未落定"；发出的
        // channelUnavailable 带着正确的 reason。
        void Test_GateUnavailable_NoPortCalls()
        {
            ProviderFixture fixture = MakeProviderFixture(Channel::StandardDriver);
            // MakeProviderFixture 内部的 setAddressSpace 已经立即触发过一次
            // requestVisiblePages()，当时还没注入 gateInputsProvider_，所以那
            // 一次也走了"不可用"分支，已经消耗掉 D2 去抖记忆的第一个槛位
            // （同一个 GateReason 连续命中只发一次）。这里先用一次"可用"的
            // 请求把去抖记忆重置（Gate 判定可用会清空记忆），再切回"未注入"
            // 的保守默认发起本测试真正要观察的那一次请求，确保接下来断言的
            // 是一次"新的"不可用事件，不会被构造阶段的内部请求提前吞掉。
            fixture.provider->setGateInputsProvider(
                [channel = Channel::StandardDriver]() { return MakeAvailableGateInputs(channel); });
            fixture.portPtr->SetScript({MakeOk(MakePattern(0x01, static_cast<std::size_t>(kPageSize)))});
            fixture.provider->RequestPages(
                {HexFetchRange{kFixtureFirstAddress + 4 * kPageSize, 1, 0}}, fixture.canvasRevision);
            WPJ2_CHECK(PumpUntil([&]() { return fixture.portPtr->CallCount() >= 1; }, 1000));
            fixture.provider->setGateInputsProvider(nullptr); // 还原成"未注入"的保守默认。

            bool gotUnavailable = false;
            GateVerdict capturedVerdict;
            QObject::connect(
                fixture.provider.get(),
                &WorkbenchPageProvider::channelUnavailable,
                [&](GateVerdict verdict)
                {
                    gotUnavailable = true;
                    capturedVerdict = verdict;
                });

            const int callCountBeforeCheck = fixture.portPtr->CallCount();
            const HexFetchRange range{kFixtureFirstAddress, 1, 0};
            fixture.provider->RequestPages({range}, fixture.canvasRevision);

            WPJ2_CHECK(gotUnavailable);
            WPJ2_CHECK(!capturedVerdict.available);
            WPJ2_CHECK(capturedVerdict.reason == GateReason::DriverNotLoaded);
            WPJ2_CHECK_NOTE(
                fixture.portPtr->CallCount() == callCountBeforeCheck,
                QStringLiteral("Gate 不可用时端口调用次数不应增加，实际 %1 -> %2")
                    .arg(callCountBeforeCheck)
                    .arg(fixture.portPtr->CallCount()));

            // 画布侧：这一页从未被交付，cellStateAt 应仍是 NotLoaded（没有伪造
            // 任何字节，也没有误标成 Unreadable）。
            const HexCanvas::CellState cell = fixture.canvas->cellStateAt(kFixtureFirstAddress);
            WPJ2_CHECK(cell.byteState == HexCanvas::ByteState::NotLoaded);
            WPJ2_CHECK(!cell.hasValue);
        }

        // Test_GateAvailable_HappyPathDeliversValidPage：Gate 判定可用时，正常
        // 发起一次端口读取，结果为 Ok（整页 4096 字节全部读到），落地后画布应
        // 显示 Valid、字节内容与脚本一致。这是后续"陈旧/latch/channelFailed"
        // 测试共同依赖的地基用例。
        void Test_GateAvailable_HappyPathDeliversValidPage()
        {
            ProviderFixture fixture = MakeProviderFixture(Channel::StandardDriver);
            fixture.provider->setGateInputsProvider(
                [channel = Channel::StandardDriver]() { return MakeAvailableGateInputs(channel); });

            const std::vector<std::uint8_t> pattern = MakePattern(0x10, static_cast<std::size_t>(kPageSize));
            fixture.portPtr->SetScript({MakeOk(pattern)});

            const HexFetchRange range{kFixtureFirstAddress, 1, 0};
            fixture.provider->RequestPages({range}, fixture.canvasRevision);

            const bool landed = PumpUntil(
                [&]() { return fixture.canvas->cellStateAt(kFixtureFirstAddress).byteState != HexCanvas::ByteState::NotLoaded; },
                2000);
            WPJ2_CHECK(landed);
            WPJ2_CHECK(fixture.portPtr->CallCount() == 1);

            const HexCanvas::CellState first = fixture.canvas->cellStateAt(kFixtureFirstAddress);
            WPJ2_CHECK(first.byteState == HexCanvas::ByteState::Valid);
            WPJ2_CHECK(first.hasValue);
            WPJ2_CHECK_NOTE(
                first.value == pattern[0],
                QStringLiteral("第一个字节应为 0x%1，实际 0x%2")
                    .arg(static_cast<int>(pattern[0]), 2, 16, QLatin1Char('0'))
                    .arg(static_cast<int>(first.value), 2, 16, QLatin1Char('0')));

            const HexCanvas::CellState last = fixture.canvas->cellStateAt(kFixtureFirstAddress + kPageSize - 1);
            WPJ2_CHECK(last.byteState == HexCanvas::ByteState::Valid);
            WPJ2_CHECK_NOTE(
                last.value == pattern[static_cast<std::size_t>(kPageSize - 1)],
                QStringLiteral("最后一个字节应与脚本一致"));
        }

        // Test_EmptyRangesRequest_NoOp：空 ranges 向量——不应触碰 Gate、不应
        // 发起任何端口调用、也不应发出任何信号。
        void Test_EmptyRangesRequest_NoOp()
        {
            ProviderFixture fixture = MakeProviderFixture(Channel::StandardDriver);
            fixture.provider->setGateInputsProvider(
                [channel = Channel::StandardDriver]() { return MakeAvailableGateInputs(channel); });

            bool gotUnavailable = false;
            QObject::connect(
                fixture.provider.get(),
                &WorkbenchPageProvider::channelUnavailable,
                [&]() { gotUnavailable = true; });

            fixture.provider->RequestPages({}, fixture.canvasRevision);

            WPJ2_CHECK(!gotUnavailable);
            WPJ2_CHECK(fixture.portPtr->CallCount() == 0);
        }

        // Test_EmptyRangesRequest_GateUnavailable_StillNoOp：同上，但故意**不**
        // 注入 gateInputsProvider_（Gate 判定不可用）。如果 RequestPages 开头
        // 缺了"ranges 为空就直接返回"这一步，代码会继续往下跑到 Gate
        // 不可用的分支，对空 ranges 空转一轮 for 循环之后仍然会
        // emit channelUnavailable——这是一次"看起来没事"（没有任何 range
        // 被误 cancelPages，因为循环体本身是空的）但确实多发了一个不该发的
        // 信号的回归，只用"端口调用次数是否为 0"测不出来（两种写法下都是
        // 0），必须专门盯住这个信号本身。
        void Test_EmptyRangesRequest_GateUnavailable_StillNoOp()
        {
            ProviderFixture fixture = MakeProviderFixture(Channel::StandardDriver);
            // 故意不调用 setGateInputsProvider——Gate 判定保持"最保守不可用"。

            bool gotUnavailable = false;
            QObject::connect(
                fixture.provider.get(),
                &WorkbenchPageProvider::channelUnavailable,
                [&]() { gotUnavailable = true; });

            fixture.provider->RequestPages({}, fixture.canvasRevision);

            WPJ2_CHECK_NOTE(
                !gotUnavailable,
                QStringLiteral("空 ranges 请求不应该触碰 Gate、更不应该发出 channelUnavailable"));
            WPJ2_CHECK(fixture.portPtr->CallCount() == 0);
        }
    }

    void RunGateTests()
    {
        Test_GateUnavailable_NoPortCalls();
        Test_GateAvailable_HappyPathDeliversValidPage();
        Test_EmptyRangesRequest_NoOp();
        Test_EmptyRangesRequest_GateUnavailable_StillNoOp();
    }
}
