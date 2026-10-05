// ============================================================
// wpJ6_tests.Gate.cpp
// 作用：Gate 判不可用时端口调用 0 次，且不自动换通道（ux.md §2 第 1 条
// "不可用时保持选中并报红，不替用户换"）。
// ============================================================

#include "wpJ6_common.h"

namespace wpj6_test
{
    // TestGateUnavailableZeroPortCallsNoAutoSwitch：切到标准驱动通道但
    // driverLoaded=false（R0 不可用），核对：①通道确实切换成功（身份层不检查
    // Gate）；②后续没有任何端口读调用发生在这个不可用的通道上；③会话通道
    // 本身没有被静默换回 R3。
    void TestGateUnavailableZeroPortCallsNoAutoSwitch()
    {
        auto& backend = ConfigureSharedOnce();
        Harness harness;
        harness.AttachProcess();
        PumpUntil([&]() { return true; }, 10);

        harness.gate.driverLoaded = false;
        const bool switched = harness.view->target().requestChannel(ksword::memwb::Channel::StandardDriver);
        WPJ6_CHECK(switched);
        WPJ6_CHECK(harness.view->target().session().channel == ksword::memwb::Channel::StandardDriver);

        int readsBefore = 0;
        int writesBefore = 0;
        {
            std::lock_guard<std::mutex> lock(backend.backing->mutex);
            readsBefore = backend.backing->readCallCount;
            writesBefore = backend.backing->writeCallCount;
        }

        // 画布此刻仍会尝试请求可见页（R0 不可用不代表画布会停止工作），但
        // WorkbenchPageProvider 的 Gate 判定应该拦在端口调用之前。给够时间让
        // 任何"本该发生但不该发生"的端口调用有机会出现。
        harness.view->hexPaneForTest()->rereadWindow();
        PumpFor(300);

        int readsAfter = 0;
        int writesAfter = 0;
        {
            std::lock_guard<std::mutex> lock(backend.backing->mutex);
            readsAfter = backend.backing->readCallCount;
            writesAfter = backend.backing->writeCallCount;
        }
        WPJ6_CHECK_NOTE(
            readsAfter == readsBefore,
            QStringLiteral("Gate 不可用时端口 Read 调用次数应恒为 0（before=%1 after=%2）").arg(readsBefore).arg(readsAfter));
        WPJ6_CHECK(writesAfter == writesBefore);

        // 不自动换通道：会话通道仍然是用户选的那个，不会被悄悄换回 R3。
        WPJ6_CHECK(harness.view->target().session().channel == ksword::memwb::Channel::StandardDriver);
    }

    // TestChannelGateCoversAllFourSegments（纳入 M13 的教训，并入审核报告
    // wpJ6/wave3 的 P2 探针判据）：refreshChannelGateDisplay 对四个通道各评估
    // 一次并写回会话条四个分段按钮的可用性；如果循环少算一个通道（例如
    // i<3U 而不是 i<4U，即 M13 变异的做法），最后一段（Ddma，下标 3）永远
    // 不会被这次评估触碰，停在构造期的默认可用状态上。用
    // sessionBar->channelSegmented()->isSegmentEnabled(3) 直接核对——这两个
    // 都是既有公开方法，不需要新增任何跨包访问器（审核报告纠正了实现者报告
    // "SURVIVED 需要新增访问器"的误判：WorkbenchSessionBar::channelSegmented()
    // 本身已公开返回 HexViewSegmented*，isSegmentEnabled(int) 是 WP-0 就有的
    // 既有公开方法）。
    void TestChannelGateCoversAllFourSegments()
    {
        Harness harness;
        harness.AttachProcess();
        PumpUntil([&]() { return true; }, 10);

        auto* sessionBar = harness.view->sessionBarForTest();
        WPJ6_CHECK(sessionBar != nullptr);
        auto* segmented = sessionBar != nullptr ? sessionBar->channelSegmented() : nullptr;
        WPJ6_CHECK(segmented != nullptr);
        if (segmented == nullptr)
        {
            return;
        }

        // 先确保 Ddma（下标 3）一开始可用——DDMA 的判据是"先驱动、再会话
        // 就绪"两者都要满足（MemoryChannelGate.cpp::EvaluateChannel），
        // GateState 默认 driverLoaded=false，必须一并置真，否则预置状态
        // 本身就不可用，核对不出"从可用变不可用"这一步有没有真的发生。
        // 重新注入 provider 会立即触发一次刷新（见 setGateInputsProvider
        // 的实现）。
        harness.gate.driverLoaded = true;
        harness.gate.ddmaSessionReady = true;
        harness.view->setGateInputsProvider([&harness]() -> ksword::memwb::GateInputs {
            ksword::memwb::GateInputs inputs;
            inputs.hasProcessTarget = harness.gate.hasProcessTarget;
            inputs.driverLoaded = harness.gate.driverLoaded;
            inputs.hvmProbe = harness.gate.hvmProbe;
            inputs.ddmaSessionReady = harness.gate.ddmaSessionReady;
            return inputs;
        });
        WPJ6_CHECK_NOTE(
            segmented->isSegmentEnabled(3), QStringLiteral("预置 ddmaSessionReady=true 时 Ddma 段应可用"));

        // 翻转为不可用，再重新注入一次触发刷新。
        harness.gate.ddmaSessionReady = false;
        harness.view->setGateInputsProvider([&harness]() -> ksword::memwb::GateInputs {
            ksword::memwb::GateInputs inputs;
            inputs.hasProcessTarget = harness.gate.hasProcessTarget;
            inputs.driverLoaded = harness.gate.driverLoaded;
            inputs.hvmProbe = harness.gate.hvmProbe;
            inputs.ddmaSessionReady = harness.gate.ddmaSessionReady;
            return inputs;
        });
        WPJ6_CHECK_NOTE(
            !segmented->isSegmentEnabled(3),
            QStringLiteral("ddmaSessionReady 翻为 false 后 Ddma 段（下标 3）必须跟着变不可用——"
                           "若循环漏算这个下标，这里会保持上一步的可用状态"));
    }

    void RunGateTests()
    {
        TestGateUnavailableZeroPortCallsNoAutoSwitch();
        TestChannelGateCoversAllFourSegments();
    }
}
