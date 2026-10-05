// ============================================================
// wpJ2_tests.Latch.cpp
// 作用：覆盖接口文档 §7 的关键判断——"同一来源代次内第一次 DDMA
// scratchAreaDirty 之后同代次请求端口调用次数恒为 0，resetScratchLatch 后
// 恢复"。对应不变式 14"红 chip + DDMA 读故障闩锁"的读侧落点。
// ============================================================

#include "wpJ2_common.h"

using namespace ks::ui;
using namespace ksword::memwb;

namespace wpJ2_test
{
    namespace
    {
        // Test_ScratchDirty_LatchesAndBlocksSameRevision：第一次读取报
        // scratchAreaDirty=true（即便本身是 Ok），应该：①正常交付这一次的
        // 页（脏信号不等于这次读取本身失败）；②只发一次
        // scratchAreaDirtyLatched；③同一来源代次内再发起的请求，端口调用
        // 次数恒为 0（直接 cancelPages，不问端口）。
        void Test_ScratchDirty_LatchesAndBlocksSameRevision()
        {
            ProviderFixture fixture = MakeProviderFixture(Channel::Ddma);
            fixture.provider->setGateInputsProvider(
                [channel = Channel::Ddma]() { return MakeAvailableGateInputs(channel); });

            int latchedSignalCount = 0;
            QObject::connect(
                fixture.provider.get(),
                &WorkbenchPageProvider::scratchAreaDirtyLatched,
                [&]() { ++latchedSignalCount; });

            const std::vector<std::uint8_t> pattern = MakePattern(0x70, static_cast<std::size_t>(kPageSize));
            fixture.portPtr->SetScript({MakeOkWithScratchDirty(pattern)});

            const HexFetchRange firstRange{kFixtureFirstAddress, 1, 0};
            fixture.provider->RequestPages({firstRange}, fixture.canvasRevision);
            const bool firstLanded = PumpUntil(
                [&]() { return fixture.canvas->cellStateAt(kFixtureFirstAddress).byteState != HexCanvas::ByteState::NotLoaded; },
                2000);
            WPJ2_CHECK(firstLanded);

            // ①脏信号不等于这次读取失败：这一页本身是 Ok，应该正常交付成 Valid。
            WPJ2_CHECK_NOTE(
                fixture.canvas->cellStateAt(kFixtureFirstAddress).byteState == HexCanvas::ByteState::Valid,
                QStringLiteral("scratchAreaDirty 为真但本页是 Ok，仍应正常交付"));
            // ②只发一次。
            WPJ2_CHECK_NOTE(
                latchedSignalCount == 1,
                QStringLiteral("scratchAreaDirtyLatched 应恰好发一次，实际 %1 次").arg(latchedSignalCount));

            // ③同代次内再请求另一页，端口调用次数不应增加（闩锁直接拦截）。
            const int callCountBeforeSecondRequest = fixture.portPtr->CallCount();
            const HexFetchRange secondRange{kFixtureFirstAddress + kPageSize, 1, 0};
            fixture.provider->RequestPages({secondRange}, fixture.canvasRevision);
            PumpUntil([]() { return false; }, 150);
            WPJ2_CHECK_NOTE(
                fixture.portPtr->CallCount() == callCountBeforeSecondRequest,
                QStringLiteral("闩锁命中后同代次内的请求不应再问端口"));
            WPJ2_CHECK(
                fixture.canvas->cellStateAt(kFixtureFirstAddress + kPageSize).byteState == HexCanvas::ByteState::NotLoaded);
            // 再触发一次，latched 信号不应该重复发出（同代次内只发一次）。
            WPJ2_CHECK(latchedSignalCount == 1);
        }

        // Test_BackToBackScratchDirty_SecondJobSkippedAndLatchEmitsOnce：两次
        // 请求背靠背发起（第二次在第一次落地**之前**就已经通过了 RequestPages
        // 开头的 Gate/闩锁前置判定——此刻 scratchLatched_ 还是 false，两次都
        // 会被正常提交给读池）。
        //
        // Wave 3 审核 D3 落实之后，这条测试的行为与旧版本不同：旧实现里两个
        // 任务都会真的去问端口，第二个任务也会报 scratchAreaDirty，因此需要
        // 专门验证"只有第一次落地才置位闩锁并发信号"；现在 D3 的共享"通道
        // 中毒"标志会让第二个任务在它真正开始跑之前就发现第一个任务已经把
        // 通道标脏，整段跳过、一次端口调用都不发起——"只发一次信号"这件事
        // 现在是 D3 的直接结果，不再需要靠"两次都真的落地"来构造。这里同时
        // 覆盖两件事：①端口调用次数恒为 1（第二个任务被跳过，不是因为闩锁
        // 前置判定——此刻 scratchLatched_ 还没来得及在 UI 线程置位，是 D3 的
        // 工作线程共享标志起的作用）；②闩锁信号只发一次。
        void Test_BackToBackScratchDirty_SecondJobSkippedAndLatchEmitsOnce()
        {
            ProviderFixture fixture = MakeProviderFixture(Channel::Ddma);
            fixture.provider->setGateInputsProvider(
                [channel = Channel::Ddma]() { return MakeAvailableGateInputs(channel); });

            int latchedSignalCount = 0;
            QObject::connect(
                fixture.provider.get(),
                &WorkbenchPageProvider::scratchAreaDirtyLatched,
                [&]() { ++latchedSignalCount; });

            // 端口调用要有可观察的延迟，确保第二次 RequestPages 在第一次真正
            // 开始读之前就已经发起（单线程池下第二个任务会排在队列里，但
            // "排队"这件事本身就已经证明它通过了前置判定——前置判定在
            // RequestPages 开头同步执行，不会等端口真正跑完）。脚本只给一条
            // 结果：如果 D3 没有生效，第二个任务会真的去问端口，消费掉这唯一
            // 的一条脚本之外的内容，得到"脚本用尽"的显式失败，而不是维持
            // CallCount()==1。
            fixture.portPtr->SetDelayMs(60);
            fixture.portPtr->SetScript(
                {MakeOkWithScratchDirty(MakePattern(0xB1, static_cast<std::size_t>(kPageSize)))});

            const HexFetchRange rangeA{kFixtureFirstAddress, 1, 0};
            const HexFetchRange rangeB{kFixtureFirstAddress + kPageSize, 1, 0};
            fixture.provider->RequestPages({rangeA}, fixture.canvasRevision);
            fixture.provider->RequestPages({rangeB}, fixture.canvasRevision);

            const bool firstLanded = PumpUntil(
                [&]() { return fixture.canvas->cellStateAt(kFixtureFirstAddress).byteState != HexCanvas::ByteState::NotLoaded; },
                3000);
            WPJ2_CHECK(firstLanded);
            // 第二个任务也会异步落地（哪怕什么都没做），再等一轮确保它已经
            // 处理完，不是因为我们没等够。
            PumpUntil([]() { return false; }, 200);

            WPJ2_CHECK_NOTE(
                fixture.portPtr->CallCount() == 1,
                QStringLiteral("D3：第二个已排队的任务不应该再碰已知中毒的通道，实际端口调用 %1 次")
                    .arg(fixture.portPtr->CallCount()));
            WPJ2_CHECK_NOTE(
                latchedSignalCount == 1,
                QStringLiteral("scratchAreaDirtyLatched 应恰好发一次，实际 %1 次").arg(latchedSignalCount));
            // 第二个 range 从未被真正问过端口，也没有被误标成 Unreadable/Valid
            // （D3：skippedDueToPoison 的 pages 为空，逐页回填循环什么都不做）。
            // 这里直接调用 provider->RequestPages，没有经过画布自己的
            // MarkInFlight，所以状态停在 NotLoaded（不是 Pending）——与
            // Test_GateUnavailable_NoPortCalls 等同样直接调用 provider 的测试
            // 一致。
            WPJ2_CHECK(
                fixture.canvas->cellStateAt(kFixtureFirstAddress + kPageSize).byteState == HexCanvas::ByteState::NotLoaded);
        }

        // Test_ResetScratchLatch_RestoresNormalReads：命中闩锁之后调用
        // resetScratchLatch，同一个目标代次下的请求应该恢复正常问端口。
        //
        // **决策 2 之后的一处已知覆盖退化（写进 fix 报告，不是回避）**：旧版本
        // 这里靠"把 sourceRevision 参数显式传成 0"来隔离 resetScratchLatch
        // 两行各自的正确性（第二行把 scratchLatchedForRevision_ 清零，单独
        // 这一条就足以让 AND 表达式的右操作数在"参数传 0"时恒为真，从而掩盖
        // 第一行 scratchLatched_ 是否真的被清成 false）。闩锁的键已经从"调用方
        // 传入的参数"改成"provider 自己拉取的目标轴"（target_->capture().
        // rev.source），测试再也无法从外部注入一个"0"去单独隔离某一行——
        // 两行只能作为一个整体黑盒验证。唯一能让测试自己观察到的组合是
        // "两行都没生效"（这种情况下第二次请求仍会被拦截）；"只有第一行没
        // 生效"这一种子情况在黑盒行为测试里不再可区分，这是把闩锁键换成
        // 单调递增、调用方不可控的目标代次之后的结构性代价，不是本测试偷懶。
        void Test_ResetScratchLatch_RestoresNormalReads()
        {
            ProviderFixture fixture = MakeProviderFixture(Channel::Ddma);
            fixture.provider->setGateInputsProvider(
                [channel = Channel::Ddma]() { return MakeAvailableGateInputs(channel); });

            fixture.portPtr->SetScript({MakeOkWithScratchDirty(MakePattern(0x80, static_cast<std::size_t>(kPageSize)))});
            const HexFetchRange firstRange{kFixtureFirstAddress, 1, 0};
            fixture.provider->RequestPages({firstRange}, fixture.canvasRevision);
            WPJ2_CHECK(PumpUntil(
                [&]() { return fixture.canvas->cellStateAt(kFixtureFirstAddress).byteState != HexCanvas::ByteState::NotLoaded; },
                2000));

            // 命中闩锁之后、重置之前：同一目标代次的另一个 range 应该被直接
            // 拦截，端口调用次数不变——这是重置生效与否的对照基线。
            const int callCountAfterLatch = fixture.portPtr->CallCount();
            const HexFetchRange probeRange{kFixtureFirstAddress + kPageSize * 2, 1, 0};
            fixture.provider->RequestPages({probeRange}, fixture.canvasRevision);
            PumpUntil([]() { return false; }, 100);
            WPJ2_CHECK_NOTE(
                fixture.portPtr->CallCount() == callCountAfterLatch,
                QStringLiteral("重置之前，同一目标代次的请求应该被闩锁拦截"));

            fixture.provider->resetScratchLatch();

            fixture.portPtr->SetScript({MakeOk(MakePattern(0x90, static_cast<std::size_t>(kPageSize)))});
            const HexFetchRange secondRange{kFixtureFirstAddress + kPageSize, 1, 0};
            fixture.provider->RequestPages({secondRange}, fixture.canvasRevision);
            const bool secondLanded = PumpUntil(
                [&]()
                {
                    return fixture.canvas->cellStateAt(kFixtureFirstAddress + kPageSize).byteState
                        == HexCanvas::ByteState::Valid;
                },
                2000);
            WPJ2_CHECK_NOTE(secondLanded, QStringLiteral("resetScratchLatch 之后应该恢复正常读取"));
            WPJ2_CHECK_NOTE(
                fixture.portPtr->CallCount() == callCountAfterLatch + 1,
                QStringLiteral("resetScratchLatch 之后端口调用次数应该恰好增加一次，实际 %1 -> %2")
                    .arg(callCountAfterLatch)
                    .arg(fixture.portPtr->CallCount()));
        }
    }

    void RunLatchTests()
    {
        Test_ScratchDirty_LatchesAndBlocksSameRevision();
        Test_BackToBackScratchDirty_SecondJobSkippedAndLatchEmitsOnce();
        Test_ResetScratchLatch_RestoresNormalReads();
    }
}
