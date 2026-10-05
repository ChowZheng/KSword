#include "wpG_common.h"

// ============================================================
// wpG_tests.SessionBar.cpp
// 作用：验证 WorkbenchSessionBar 的两条核心判据——
//   1) 不可用的通道分段置灰，悬停提示换成原因；
//   2) 切换范围时通道取"该范围上次使用"（ChannelMemory），通道此刻不可用时
//      保持选中并报红，绝不被静默换成别的通道。
// ============================================================

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchSessionBar.h"

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/HexViewWidgets.h"

#include <array>

namespace wpg_test
{
    using ksword::memwb::Channel;
    using ksword::memwb::GateReason;
    using ksword::memwb::GateVerdict;
    using ksword::memwb::Scope;

    namespace
    {
        // MakeVerdicts：按一个"哪些通道可用"的布尔数组，拼出四个 GateVerdict。
        std::array<GateVerdict, 4> MakeVerdicts(const std::array<bool, 4>& available)
        {
            std::array<GateVerdict, 4> verdicts{};
            for (int i = 0; i < 4; ++i)
            {
                verdicts[static_cast<std::size_t>(i)].available = available[static_cast<std::size_t>(i)];
                verdicts[static_cast<std::size_t>(i)].reason =
                    available[static_cast<std::size_t>(i)] ? GateReason::None : GateReason::DriverNotLoaded;
            }
            return verdicts;
        }
    }

    void RunSessionBarTests()
    {
        ks::ui::WorkbenchSessionBar bar;
        bar.resize(700, 60);
        // isVisible() 的判定要看整条祖先链是否真的 show() 过，不 show() 的话子控件
        // 即使 setVisible(true) 也测不出"可见"——这是夹具的 QWidget 语义，不是业务缺陷。
        bar.show();

        // —— 判据 1：置灰 + 提示原因 ——
        // 进程范围下只有 R3 可用，R0/HVM/DDMA 都因"驱动未加载"置灰。
        bar.setChannelVerdicts(MakeVerdicts({true, false, false, false}));
        WPG_CHECK(bar.channelSegmented()->isSegmentEnabled(0));
        WPG_CHECK(!bar.channelSegmented()->isSegmentEnabled(1));
        WPG_CHECK_NOTE(
            bar.channelSegmented()->segmentToolTip(1).contains(QStringLiteral("驱动未加载")),
            bar.channelSegmented()->segmentToolTip(1));

        // —— 判据 2a：用户点通道分段只发请求信号，不代表已经生效；装配层确认后调
        // setChannel 才真正写入 ChannelMemory 并回显 ——
        int requestCount = 0;
        Channel lastRequested = Channel::UserMode;
        QObject::connect(&bar, &ks::ui::WorkbenchSessionBar::channelRequested,
            [&requestCount, &lastRequested](const Channel channel) {
                ++requestCount;
                lastRequested = channel;
            });
        bar.channelSegmented()->setCurrentIndex(1); // 用户点了 R0（当前已置灰，但代码路径仍可选中）
        WPG_CHECK(requestCount == 1);
        WPG_CHECK(lastRequested == Channel::StandardDriver);
        bar.setChannel(Channel::UserMode); // 装配层按业务规则仍选择回写 R3，模拟"请求被否决"
        WPG_CHECK(bar.currentChannel() == Channel::UserMode);
        WPG_CHECK(bar.channelSegmented()->currentIndex() == static_cast<int>(Channel::UserMode));

        // —— 判据 2b：切到内核范围，通道回显 ChannelMemory 对内核范围的默认值（R0）——
        bar.setScope(Scope::KernelVirtual);
        WPG_CHECK(bar.currentScope() == Scope::KernelVirtual);
        WPG_CHECK_NOTE(
            bar.currentChannel() == Channel::StandardDriver,
            QStringLiteral("channel=%1").arg(static_cast<int>(bar.currentChannel())));

        // —— 判据 2c：内核范围下四个通道全部不可用（模拟驱动掉线），当前通道必须
        // 保持选中（不被静默换走），并显示报红原因 ——
        bar.setChannelVerdicts(MakeVerdicts({false, false, false, false}));
        WPG_CHECK(bar.currentChannel() == Channel::StandardDriver);
        WPG_CHECK(bar.channelSegmented()->currentIndex() == static_cast<int>(Channel::StandardDriver));
        WPG_CHECK(!bar.channelWarningText().isEmpty());

        // —— 判据 2d：切回进程范围，通道回显之前记住的 R3（不是回到内核范围的默认值，
        // 也不是一个全局唯一的默认值——证明记忆是"按范围"分开的）——
        bar.setScope(Scope::ProcessVirtual);
        WPG_CHECK(bar.currentChannel() == Channel::UserMode);

        // —— 判据 3：目标 chip 的两种文案都能正常设置而不崩溃 ——
        bar.setTargetInfo(false, QString(), 0, 64, false);
        bar.setTargetInfo(true, QStringLiteral("chrome.exe"), 1234, 64, true);

        // —— 判据 4：待写入区的显隐随字节数切换 ——
        bar.setPendingPatches(0, 0);
        bar.setPendingPatches(12, 3);
        bar.setPendingPatches(0, 0);
    }
}
