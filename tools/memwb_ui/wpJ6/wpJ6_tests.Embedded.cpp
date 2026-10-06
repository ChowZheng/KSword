// ============================================================
// wpJ6_tests.Embedded.cpp
// 作用：setEmbeddedProcessMode(true) 后对内核/物理范围的 requestIdentity 恒失败
//       （Policy::allowKernelPhysical=false），侧栏隐藏。
// ============================================================

#include "wpJ6_common.h"

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchSettings.h"

namespace wpj6_test
{
    // TestSettingsAuthorityOnlyAuthoritativeSaves：决策 1——只有权威视图的
    // saveSettings() 真正写盘，非权威视图是空操作。用子页签下标
    // （subTab，QStackedWidget::setCurrentIndex 不依赖任何布局/显示状态，
    // 比侧栏宽度那种依赖 QSplitter 真实几何的键更稳）做可观察判据：
    // 非权威视图保存前后该键不变；权威视图保存后该键变成它当前的子页签。
    void TestSettingsAuthorityOnlyAuthoritativeSaves()
    {
        using namespace ks::ui::workbench_settings;

        Harness nonAuthoritative;
        WPJ6_CHECK(!nonAuthoritative.view->isSettingsAuthoritative());
        SaveSubTab(1);
        nonAuthoritative.view->saveSettings();
        WPJ6_CHECK_NOTE(LoadSubTab() == 1, QStringLiteral("非权威视图 saveSettings 不应写盘"));

        Harness authoritative;
        authoritative.view->setSettingsAuthoritative(true);
        WPJ6_CHECK(authoritative.view->isSettingsAuthoritative());
        if (auto* stack = authoritative.view->subTabStackForTest())
        {
            stack->setCurrentIndex(3);
        }
        authoritative.view->saveSettings();
        WPJ6_CHECK_NOTE(LoadSubTab() == 3, QStringLiteral("权威视图 saveSettings 应该真的写盘"));

        // 还原，避免污染本机这个测试专用的注册表项给后续其它调用。
        SaveSubTab(0);
    }

    void TestEmbeddedModeBlocksKernelAndPhysical()
    {
        Harness harness;
        harness.AttachProcess();
        PumpUntil([&]() { return true; }, 10);

        harness.view->setEmbeddedProcessMode(true);
        WPJ6_CHECK(!harness.view->target().policy().allowKernelPhysical);

        ks::ui::NavRequest toKernel;
        toKernel.scope = ksword::memwb::Scope::KernelVirtual;
        toKernel.address = 0xFFFFF78000000000ULL;
        const auto kernelStatus = harness.view->openAt(toKernel);
        WPJ6_CHECK_NOTE(kernelStatus != ks::ui::NavStatus::Ok, QStringLiteral("内嵌模式下不应能切到内核范围"));
        WPJ6_CHECK(harness.view->target().session().scope == ksword::memwb::Scope::ProcessVirtual);

        ks::ui::NavRequest toPhysical;
        toPhysical.scope = ksword::memwb::Scope::Physical;
        toPhysical.address = 0x1000ULL;
        const auto physicalStatus = harness.view->openAt(toPhysical);
        WPJ6_CHECK_NOTE(physicalStatus != ks::ui::NavStatus::Ok, QStringLiteral("内嵌模式下不应能切到物理范围"));
        WPJ6_CHECK(harness.view->target().session().scope == ksword::memwb::Scope::ProcessVirtual);

        // 注意：Harness 里的 view 从未 .show() 过，QWidget::isVisible() 在"控件
        // 没 show 过就读错"（仓库已知坑，见 .claude/memory 里的提醒），这里改用
        // isHidden()——它只看"有没有对这个控件本身调用过 setVisible(false)"，
        // 不受顶层窗口是否显示过影响。
        auto* addressBookParent = harness.view->addressBookPanelForTest()->parentWidget();
        WPJ6_CHECK(addressBookParent != nullptr);
        if (addressBookParent != nullptr)
        {
            WPJ6_CHECK_NOTE(addressBookParent->isHidden(), QStringLiteral("内嵌模式下侧栏容器应隐藏"));
        }
    }

    void RunEmbeddedTests()
    {
        TestSettingsAuthorityOnlyAuthoritativeSaves();
        TestEmbeddedModeBlocksKernelAndPhysical();
    }
}
