// ============================================================
// wpJ2_tests.ChannelFailed.cpp
// 作用：覆盖接口文档 §7 的关键判断——"channelFailed 之后不自动重试，必须显式
// retryFailedRanges"；以及 R3 的"channelFailed 为真：整批 cancelPages 并发
// readFailed"。自查清单 e 项的变体——通道失败不是"目标不可读"，不能把它
// 标成 Unreadable（那会让界面显示"这块内存确实读不到"，误导用户去怀疑目标
// 本身，而不是去重试）。
// ============================================================

#include "wpJ2_common.h"

using namespace ks::ui;
using namespace ksword::memwb;

namespace wpJ2_test
{
    namespace
    {
        // Test_ChannelFailed_CancelsPagesAndEmitsReadFailed：端口报 Failed，
        // 断言：页没有被标成 Unreadable（也没有 Valid），readFailed 信号带着
        // 正确的 sourceRevision 与非空的 failureText。
        void Test_ChannelFailed_CancelsPagesAndEmitsReadFailed()
        {
            ProviderFixture fixture = MakeProviderFixture(Channel::StandardDriver);
            fixture.provider->setGateInputsProvider(
                [channel = Channel::StandardDriver]() { return MakeAvailableGateInputs(channel); });
            fixture.portPtr->SetScript({MakeFailed("device unreachable")});

            bool gotReadFailed = false;
            quint64 failedRevision = 0;
            QString failedText;
            QObject::connect(
                fixture.provider.get(),
                &WorkbenchPageProvider::readFailed,
                [&](quint64 revision, QString text)
                {
                    gotReadFailed = true;
                    failedRevision = revision;
                    failedText = text;
                });

            const HexFetchRange range{kFixtureFirstAddress, 1, 0};
            fixture.provider->RequestPages({range}, fixture.canvasRevision);

            const bool landed = PumpUntil([&]() { return gotReadFailed; }, 2000);
            WPJ2_CHECK(landed);
            WPJ2_CHECK(failedRevision == fixture.canvasRevision);
            WPJ2_CHECK(!failedText.isEmpty());

            const HexCanvas::CellState cell = fixture.canvas->cellStateAt(kFixtureFirstAddress);
            WPJ2_CHECK_NOTE(
                cell.byteState != HexCanvas::ByteState::Unreadable,
                QStringLiteral("通道失败不是目标不可读，不应该标成 Unreadable"));
            WPJ2_CHECK(cell.byteState != HexCanvas::ByteState::Valid);
        }

        // Test_NoAutoRetryAfterChannelFailed_ThenExplicitRetrySucceeds：通道
        // 失败一次之后，**不**调用 retryFailedRanges，等一段时间，断言端口
        // 调用次数仍然是 1（没有自动重试）；随后显式调用 retryFailedRanges，
        // 这次脚本换成 Ok，断言端口调用次数变成 2 且页最终交付成 Valid。
        void Test_NoAutoRetryAfterChannelFailed_ThenExplicitRetrySucceeds()
        {
            ProviderFixture fixture = MakeProviderFixture(Channel::StandardDriver);
            fixture.provider->setGateInputsProvider(
                [channel = Channel::StandardDriver]() { return MakeAvailableGateInputs(channel); });
            const std::vector<std::uint8_t> pattern = MakePattern(0x60, static_cast<std::size_t>(kPageSize));
            fixture.portPtr->SetScript({MakeFailed("transient failure"), MakeOk(pattern)});

            bool gotReadFailed = false;
            QObject::connect(
                fixture.provider.get(),
                &WorkbenchPageProvider::readFailed,
                [&]() { gotReadFailed = true; });

            const HexFetchRange range{kFixtureFirstAddress, 1, 0};
            fixture.provider->RequestPages({range}, fixture.canvasRevision);
            WPJ2_CHECK(PumpUntil([&]() { return gotReadFailed; }, 2000));

            // 不自动重试：再等一段明显超过"任何自动重试节奏"的时间，调用次数
            // 必须恒为 1。
            PumpUntil([]() { return false; }, 300);
            WPJ2_CHECK_NOTE(
                fixture.portPtr->CallCount() == 1,
                QStringLiteral("未显式 retryFailedRanges 前端口调用次数应恒为 1，实际 %1")
                    .arg(fixture.portPtr->CallCount()));

            fixture.provider->retryFailedRanges({range});
            const bool landed = PumpUntil(
                [&]() { return fixture.canvas->cellStateAt(kFixtureFirstAddress).byteState == HexCanvas::ByteState::Valid; },
                2000);
            WPJ2_CHECK(landed);
            WPJ2_CHECK_NOTE(
                fixture.portPtr->CallCount() == 2,
                QStringLiteral("显式重试后端口调用次数应变成 2，实际 %1").arg(fixture.portPtr->CallCount()));
        }

        // Test_RetryFailedRanges_EmptyRanges_NoOp：空 ranges 调用
        // retryFailedRanges 不应该触发任何端口调用（防御性边界）。
        void Test_RetryFailedRanges_EmptyRanges_NoOp()
        {
            ProviderFixture fixture = MakeProviderFixture(Channel::StandardDriver);
            fixture.provider->setGateInputsProvider(
                [channel = Channel::StandardDriver]() { return MakeAvailableGateInputs(channel); });
            fixture.provider->retryFailedRanges({});
            PumpUntil([]() { return false; }, 50);
            WPJ2_CHECK(fixture.portPtr->CallCount() == 0);
        }

        // Test_ChannelFailed_PartialSuccessWithinSameRangeStillDelivered（第二轮
        // 审核 N3 的子场景，单范围内部分页已读成功）：同一个 range 跨两页，
        // 第一页读成功、第二页通道失败（Limits 夹到 1 页，逼着逐页发起 Read），
        // 断言：已经读成功的第一页必须正常交付成 Valid，不能因为同一个 range
        // 整体 channelFailed 就被连带丢弃——这与"两个不同 range 一个成功一个
        // 失败"是另一个独立的子场景（跨 range 的那个已有 S2_G20 覆盖）。
        void Test_ChannelFailed_PartialSuccessWithinSameRangeStillDelivered()
        {
            ProviderFixture fixture = MakeProviderFixture(Channel::StandardDriver);
            fixture.provider->setGateInputsProvider(
                [channel = Channel::StandardDriver]() { return MakeAvailableGateInputs(channel); });
            fixture.portPtr->SetLimits(IoLimits{kPageSize, 0});
            fixture.portPtr->SetScript(
                {MakeOk(MakePattern(0x51, static_cast<std::size_t>(kPageSize))), MakeFailed("second page boom")});
            bool failed = false;
            QObject::connect(
                fixture.provider.get(), &WorkbenchPageProvider::readFailed, [&]() { failed = true; });
            const HexFetchRange range{kFixtureFirstAddress, 2, 0};
            fixture.provider->RequestPages({range}, fixture.canvasRevision);
            WPJ2_CHECK(PumpUntil([&]() { return failed; }, 2000));
            WPJ2_CHECK_NOTE(
                fixture.canvas->cellStateAt(kFixtureFirstAddress).byteState == HexCanvas::ByteState::Valid,
                QStringLiteral("同一 range 里已读成功的第一页被连带丢弃"));
            WPJ2_CHECK(
                fixture.canvas->cellStateAt(kFixtureFirstAddress + kPageSize).byteState != HexCanvas::ByteState::Valid);
        }
    }

    void RunChannelFailedTests()
    {
        Test_ChannelFailed_CancelsPagesAndEmitsReadFailed();
        Test_NoAutoRetryAfterChannelFailed_ThenExplicitRetrySucceeds();
        Test_RetryFailedRanges_EmptyRanges_NoOp();
        Test_ChannelFailed_PartialSuccessWithinSameRangeStillDelivered();
    }
}
