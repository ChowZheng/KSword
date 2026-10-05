// ============================================================
// wpJ2_tests.Lifecycle.cpp
// 作用：覆盖自查清单 a 项（析构期间在途任务的 UAF 风险）与接口文档 §7"析构
// 时有在途任务的稳定性循环（200 次）"；外加两条防御性边界：canvas 为空时
// 丢弃结果、portFactory 违约时的占位端口不让构造崩溃。
// ============================================================

#include "wpJ2_common.h"

#include <QElapsedTimer>

#include <iostream>

using namespace ks::ui;
using namespace ksword::memwb;

namespace wpJ2_test
{
    namespace
    {
        // Test_DestructorStabilityLoop_200Iterations：每一轮构造一套全新的
        // target+canvas+provider，发起一次带延迟的读取，**不等它完成**就立刻
        // 把三者都销毁（provider 先、canvas 其次、target 最后——模拟
        // MemoryWorkbenchView 的真实销毁顺序：provider 是 unique_ptr 成员，
        // 在 canvas 这类 QWidget 子对象之前，详见接口文档 §1）。能跑完 200 轮
        // 不崩溃，就证明了"回调落地时 provider 已经不在了"这条路径是安全的
        // （QPointer 的失效由 Qt 的对象销毁事件系统保证，不依赖我们自己判空
        // 的时机）。同时用 QElapsedTimer 量出总耗时，断言它在一个宽松但有限
        // 的上限内——不是"消除等待"，是验证"没有变成无限等待"（ReadPool 析构
        // 时会先丢队列、给正在跑的任务置位取消标志，见 WorkbenchPageProvider.
        // Pool.h 文件头第 4/5 点）。
        void Test_DestructorStabilityLoop_200Iterations()
        {
            QElapsedTimer timer;
            timer.start();

            constexpr int kIterations = 200;
            for (int iteration = 0; iteration < kIterations; ++iteration)
            {
                KernelTargetFixture targetFixture = MakeKernelTarget(Channel::StandardDriver);
                auto* rawPort = new FakeAsyncMemoryIoPort();
                rawPort->SetDelayMs(5);
                rawPort->SetScript({MakeOk(MakePattern(0x11, static_cast<std::size_t>(kPageSize)))});
                WorkbenchPageProvider::IoPortFactory factory =
                    [rawPort]() -> std::unique_ptr<IMemoryIoPort>
                { return std::unique_ptr<IMemoryIoPort>(rawPort); };

                auto provider = std::make_unique<WorkbenchPageProvider>(
                    std::move(factory), targetFixture.target.get());
                auto canvas = std::make_unique<HexCanvas>();
                provider->setCanvas(canvas.get());
                canvas->setPageProvider(provider.get());
                provider->setGateInputsProvider(
                    [channel = Channel::StandardDriver]() { return MakeAvailableGateInputs(channel); });
                canvas->setAddressSpace(kFixtureFirstAddress, kFixtureLastAddress);
                const std::uint64_t revision = canvas->sourceRevision();

                const HexFetchRange range{kFixtureFirstAddress, 1, 0};
                provider->RequestPages({range}, revision);

                // 不 Pump、不等待：立刻按"装配层销毁顺序"拆掉这一轮的对象。
                provider.reset();
                canvas.reset();
                targetFixture.target.reset();
                // services 由 target 内部的 shared_ptr 持有，target.reset() 时
                // 一并释放，这里不需要（也不应该）再手动 delete 它。
            }

            // 轮完之后再跑一轮事件循环，让"晚到的、本该被丢弃"的回调（如果
            // 有）真正执行一次（它们会发现 QPointer 已经失效，安全地直接
            // 返回）——这一步本身如果会崩溃，就正好证明了 UAF 存在。
            PumpUntil([]() { return false; }, 200);

            const qint64 elapsedMs = timer.elapsed();
            WPJ2_CHECK_NOTE(
                elapsedMs < 20000,
                QStringLiteral("200 轮创建销毁耗时 %1 ms，超出宽松上限，怀疑析构阻塞异常")
                    .arg(elapsedMs));
        }

        // Test_CanvasNull_DropsResultSilently：从未调用 setCanvas（canvas_ 恒为
        // 空），Gate 可用、端口脚本是 Ok——请求应该正常走到底，落地时发现
        // canvas_ 为空就安全丢弃，不崩溃、不解引用空指针。
        void Test_CanvasNull_DropsResultSilently()
        {
            KernelTargetFixture targetFixture = MakeKernelTarget(Channel::StandardDriver);
            auto* rawPort = new FakeAsyncMemoryIoPort();
            rawPort->SetScript({MakeOk(MakePattern(0x22, static_cast<std::size_t>(kPageSize)))});
            WorkbenchPageProvider::IoPortFactory factory =
                [rawPort]() -> std::unique_ptr<IMemoryIoPort> { return std::unique_ptr<IMemoryIoPort>(rawPort); };
            auto provider =
                std::make_unique<WorkbenchPageProvider>(std::move(factory), targetFixture.target.get());
            provider->setGateInputsProvider(
                [channel = Channel::StandardDriver]() { return MakeAvailableGateInputs(channel); });
            // 故意不调用 setCanvas。

            const HexFetchRange range{kFixtureFirstAddress, 1, 0};
            provider->RequestPages({range}, targetFixture.target->capture().rev.source);

            const bool portCalled = PumpUntil([&]() { return rawPort->CallCount() >= 1; }, 2000);
            WPJ2_CHECK(portCalled);
            // 再跑一轮事件循环，让排队的 UI 线程回调真正执行一次；能走到这里
            // 没有崩溃，就是本测试的核心断言。
            PumpUntil([]() { return false; }, 100);
            WPJ2_CHECK(true);
        }

        // Test_PortFactoryViolatesContract_FallsBackSafely：portFactory 本身
        // 为空（违反"必须非空可调用"的契约）——构造不应该崩溃，RequestPages
        // 应该走到"端口读取失败"的分支（NullMemoryIoPort 恒报 Failed），而
        // 不是解引用空指针。
        void Test_PortFactoryViolatesContract_FallsBackSafely()
        {
            KernelTargetFixture targetFixture = MakeKernelTarget(Channel::StandardDriver);
            WorkbenchPageProvider::IoPortFactory emptyFactory; // 默认构造的 std::function：空。
            auto provider =
                std::make_unique<WorkbenchPageProvider>(std::move(emptyFactory), targetFixture.target.get());
            auto canvas = std::make_unique<HexCanvas>();
            provider->setCanvas(canvas.get());
            canvas->setPageProvider(provider.get());
            provider->setGateInputsProvider(
                [channel = Channel::StandardDriver]() { return MakeAvailableGateInputs(channel); });
            canvas->setAddressSpace(kFixtureFirstAddress, kFixtureLastAddress);
            const std::uint64_t revision = canvas->sourceRevision();

            bool gotReadFailed = false;
            QObject::connect(
                provider.get(), &WorkbenchPageProvider::readFailed, [&]() { gotReadFailed = true; });

            const HexFetchRange range{kFixtureFirstAddress, 1, 0};
            provider->RequestPages({range}, revision);

            const bool landed = PumpUntil([&]() { return gotReadFailed; }, 2000);
            WPJ2_CHECK_NOTE(landed, QStringLiteral("portFactory 为空时应该安全地落到读取失败分支"));
        }
    }

    void RunLifecycleTests()
    {
        Test_DestructorStabilityLoop_200Iterations();
        Test_CanvasNull_DropsResultSilently();
        Test_PortFactoryViolatesContract_FallsBackSafely();
    }
}
