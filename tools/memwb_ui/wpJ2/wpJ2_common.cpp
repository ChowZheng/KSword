// ============================================================
// wpJ2_common.cpp
// 作用：实现 wpJ2_common.h 声明的全部公共设施，详见该头文件的说明。
// ============================================================

#include "wpJ2_common.h"

#include <QCoreApplication>
#include <QElapsedTimer>

#include <chrono>
#include <cstdio>
#include <thread>

namespace wpJ2_test
{
    int g_checks = 0;
    int g_failures = 0;

    void Report(bool ok, const char* expression, const char* file, int line, const QString& note)
    {
        ++g_checks;
        if (!ok)
        {
            ++g_failures;
            std::fprintf(
                stderr,
                "[FAIL] %s(%d): %s%s%s\n",
                file,
                line,
                expression,
                note.isEmpty() ? "" : " -- ",
                note.isEmpty() ? "" : note.toUtf8().constData());
        }
    }

    bool PumpUntil(const std::function<bool()>& predicate, int timeoutMs)
    {
        QElapsedTimer timer;
        timer.start();
        while (!predicate())
        {
            if (timer.elapsed() >= timeoutMs)
            {
                return predicate();
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    // ---------------- FakeAsyncMemoryIoPort ----------------

    void FakeAsyncMemoryIoPort::SetScript(std::vector<ksword::memwb::IoReadResult> script)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        script_ = std::move(script);
        // 消费游标随新脚本重置——同一个端口实例服务第二批请求时，应该从新
        // 脚本的第 0 条开始消费，不被之前累计的调用次数影响。
        nextScriptIndex_ = 0;
    }

    void FakeAsyncMemoryIoPort::SetDelayMs(int delayMs)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        delayMs_ = delayMs;
    }

    void FakeAsyncMemoryIoPort::SetLimits(ksword::memwb::IoLimits limits)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        limits_ = limits;
    }

    std::vector<ReadCall> FakeAsyncMemoryIoPort::Calls() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return calls_;
    }

    int FakeAsyncMemoryIoPort::CallCount() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int>(calls_.size());
    }

    int FakeAsyncMemoryIoPort::MaxConcurrentCalls() const
    {
        return maxConcurrentCalls_.load();
    }

    ksword::memwb::IoLimits FakeAsyncMemoryIoPort::Limits(
        const ksword::memwb::MemoryTargetSession& /*session*/) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return limits_;
    }

    ksword::memwb::IoReadResult FakeAsyncMemoryIoPort::Read(
        const ksword::memwb::MemoryTargetSession& /*session*/,
        std::uint64_t address,
        std::uint64_t length)
    {
        // 并发峰值计数：进入时自增、离开前自减；用 fetch_add 的返回值（自增
        // 之前的旧值）加一就是"这次调用让同时在场的调用数变成了多少"，
        // 不需要额外的比较-交换循环。
        const int concurrentNow = concurrentCalls_.fetch_add(1) + 1;
        int previousMax = maxConcurrentCalls_.load();
        while (concurrentNow > previousMax
               && !maxConcurrentCalls_.compare_exchange_weak(previousMax, concurrentNow))
        {
            // 自旋重试：另一个线程同时在更新 maxConcurrentCalls_，重新读一次
            // 再比较，直到写入成功或者已经不再需要更新。
        }

        int delayMs = 0;
        ksword::memwb::IoReadResult result;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ReadCall call;
            call.address = address;
            call.length = length;
            calls_.push_back(call);
            delayMs = delayMs_;
            if (nextScriptIndex_ < script_.size())
            {
                result = script_[nextScriptIndex_];
                ++nextScriptIndex_;
            }
            else
            {
                result.status = ksword::memwb::IoReadStatus::Failed;
                result.failure = "FakeAsyncMemoryIoPort::Read script exhausted";
            }
        }

        if (delayMs > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        }

        concurrentCalls_.fetch_sub(1);
        return result;
    }

    ksword::memwb::IoWriteResult FakeAsyncMemoryIoPort::Write(
        const ksword::memwb::MemoryTargetSession& /*session*/,
        std::uint64_t /*address*/,
        const std::vector<std::uint8_t>& /*bytes*/,
        bool /*approved*/)
    {
        // WorkbenchPageProvider 只读取，不写入；本包没有任何场景会调用它，
        // 这里只给出一个明确失败的占位，万一哪天误调用能立刻从失败文案看出来。
        ksword::memwb::IoWriteResult result;
        result.failure = "FakeAsyncMemoryIoPort::Write should never be called by WorkbenchPageProvider";
        return result;
    }

    // ---------------- FakeServices ----------------

    ks::ui::ModuleEnumResult FakeServices::enumerateProcessModules(
        std::uint32_t /*pid*/, std::uint64_t /*expectCreateTime*/)
    {
        ks::ui::ModuleEnumResult result;
        result.ok = true;
        return result;
    }

    ks::ui::ModuleEnumResult FakeServices::enumerateKernelModules()
    {
        ks::ui::ModuleEnumResult result;
        result.ok = true;
        return result;
    }

    std::vector<ksword::memwb::ProcessCandidate> FakeServices::processCandidates()
    {
        return {};
    }

    ks::ui::PointerReadResult FakeServices::readPointer(
        const ksword::memwb::MemoryTargetSession& /*session*/,
        std::uint64_t /*address*/,
        std::uint32_t /*width*/)
    {
        ks::ui::PointerReadResult result;
        result.ok = false;
        result.failure = "FakeServices::readPointer not used by WP-J2 fixture";
        return result;
    }

    std::uint64_t FakeServices::ddmaGeneration()
    {
        return ddmaGeneration_.load();
    }

    void FakeServices::SetDdmaGeneration(std::uint64_t generation)
    {
        ddmaGeneration_.store(generation);
    }

    // ---------------- 构造函数 ----------------

    KernelTargetFixture MakeKernelTarget(ksword::memwb::Channel channel)
    {
        // services 堆分配、所有权整个交给 WorkbenchTarget 的构造函数（它内部
        // 转存成 shared_ptr）；本函数只留一份非拥有的裭指针给调用方配置
        // DDMA 代次，真正的生命周期由 target 托管，不会出现双重释放。
        auto* services = new FakeServices();
        std::unique_ptr<ks::ui::IWorkbenchServices> owned(services);
        auto target = std::make_unique<ks::ui::WorkbenchTarget>(std::move(owned));
        target->requestScope(ksword::memwb::Scope::KernelVirtual);
        target->requestChannel(channel);

        KernelTargetFixture fixture;
        fixture.target = std::move(target);
        fixture.services = services;
        return fixture;
    }

    ksword::memwb::IoReadResult MakeOk(std::vector<std::uint8_t> bytes)
    {
        ksword::memwb::IoReadResult result;
        result.status = ksword::memwb::IoReadStatus::Ok;
        result.data = std::move(bytes);
        return result;
    }

    ksword::memwb::IoReadResult MakePartial(std::vector<std::uint8_t> prefix)
    {
        ksword::memwb::IoReadResult result;
        result.status = ksword::memwb::IoReadStatus::Partial;
        result.data = std::move(prefix);
        return result;
    }

    ksword::memwb::IoReadResult MakeUnreadable(std::string text)
    {
        ksword::memwb::IoReadResult result;
        result.status = ksword::memwb::IoReadStatus::Unreadable;
        result.failure = std::move(text);
        return result;
    }

    ksword::memwb::IoReadResult MakeFailed(std::string text)
    {
        ksword::memwb::IoReadResult result;
        result.status = ksword::memwb::IoReadStatus::Failed;
        result.failure = std::move(text);
        return result;
    }

    ksword::memwb::IoReadResult MakeOkWithScratchDirty(std::vector<std::uint8_t> bytes)
    {
        ksword::memwb::IoReadResult result;
        result.status = ksword::memwb::IoReadStatus::Ok;
        result.data = std::move(bytes);
        result.scratchAreaDirty = true;
        return result;
    }

    std::vector<std::uint8_t> MakePattern(std::uint8_t startValue, std::size_t length)
    {
        std::vector<std::uint8_t> data(length);
        for (std::size_t index = 0; index < length; ++index)
        {
            data[index] = static_cast<std::uint8_t>(startValue + static_cast<std::uint8_t>(index));
        }
        return data;
    }

    ProviderFixture MakeProviderFixture(ksword::memwb::Channel channel)
    {
        ProviderFixture fixture;
        fixture.targetFixture = MakeKernelTarget(channel);

        auto* rawPort = new FakeAsyncMemoryIoPort();
        fixture.portPtr = rawPort;
        ks::ui::WorkbenchPageProvider::IoPortFactory factory =
            [rawPort]() -> std::unique_ptr<ksword::memwb::IMemoryIoPort>
        {
            // 契约要求"恰好调用一次"；MakeProviderFixture 对每个 fixture 只
            // 构造一个 WorkbenchPageProvider，这条契约由调用方（本函数）保证。
            return std::unique_ptr<ksword::memwb::IMemoryIoPort>(rawPort);
        };

        fixture.provider = std::make_unique<ks::ui::WorkbenchPageProvider>(
            std::move(factory), fixture.targetFixture.target.get());
        fixture.canvas = std::make_unique<ks::ui::HexCanvas>();
        fixture.provider->setCanvas(fixture.canvas.get());
        fixture.canvas->setPageProvider(fixture.provider.get());
        // Wave 3 决策 2：不再强行把两条数轴拉到同一个值——canvas 自己的
        // setAddressSpace 换代，与 target 的来源代次是两个独立的计数器，生产
        // 环境里也不会对齐。这里只需要给 canvas 装一个真实地址空间（会立刻
        // 触发一次 requestVisiblePages()，用的是此刻尚未配置的保守默认 Gate
        // 输入，这个副作用本身也是一次合法的 Gate 判定，不需要刻意回避），
        // 记下它此刻的 sourceRevision() 供测试文件原样传给 RequestPages。
        fixture.canvas->setAddressSpace(kFixtureFirstAddress, kFixtureLastAddress);
        fixture.canvasRevision = fixture.canvas->sourceRevision();
        return fixture;
    }

    ksword::memwb::GateInputs MakeAvailableGateInputs(ksword::memwb::Channel channel)
    {
        ksword::memwb::GateInputs inputs;
        inputs.driverLoaded = true;
        if (channel == ksword::memwb::Channel::Ddma)
        {
            inputs.ddmaSessionReady = true;
        }
        return inputs;
    }
}
