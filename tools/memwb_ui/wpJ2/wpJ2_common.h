#pragma once

// ============================================================
// wpJ2_common.h
// 作用：WP-J2（WorkbenchPageProvider）离屏验证夹具的公共设施——轻量断言计数、
//       线程安全的脚本化假端口（FakeAsyncMemoryIoPort）、极简假
//       IWorkbenchServices（FakeServices，只用来撑起一个真实的
//       ks::ui::WorkbenchTarget 实例，不测试 WorkbenchTarget 自身——那是
//       WP-I 的职责）、以及"反复处理事件循环直到条件成立"的 PumpUntil。
//
// 设计取舍（写进报告，供主会话核对）：
// - 不走"附加一个假进程"的路线（WP-I 夹具那条路），而是直接把会话切到
//   KernelVirtual 范围 + StandardDriver/Ddma 通道：pid 恒为 0 对内核范围天然
//   合法，不需要构造任何进程身份/锚点，Gate 判定只需要 GateInputs.driverLoaded
//   （+ Ddma 场景再加 ddmaSessionReady）。这把 WP-J2 的夹具与"进程身份"这个
//   完全不相关的子系统解耦，只测 WorkbenchPageProvider 自己的职责。
// - FakeAsyncMemoryIoPort 与 KswordARKLightTests/MemoryIoTestSupport.h 的
//   FakeMemoryIoPort 同构（脚本按调用顺序消费、用尽报显式失败），但额外加了
//   互斥锁与可配置延迟、并发峰值计数——因为本包的端口会被 ReadPool 的工作
//   线程真正异步调用，不能像 MemoryIoTestSupport.h 那样假设"同步单线程调用"
//   （那份头文件的测试对象 ReadPages 本身确实是同步调用；但在 WP-J2 里，
//   调用 ReadPages 的是 QThreadPool 的工作线程，端口对象在该线程与 UI 线程之间
//   被共享读写，必须自己负责线程安全）。只读引用该头文件的思路，不修改它、
//   不直接复用它的类型（命名空间不同，避免任何混淆）。
// ============================================================

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/HexCanvas.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchPageProvider.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchServices.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchTarget.h"
#include "../../../shared/evidence/memory_workbench/MemoryIoPort.h"

#include <QString>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace wpJ2_test
{
    // ---- 断言计数：与既有夹具同样的轻量框架，独立计数器与命名空间。----
    extern int g_checks;
    extern int g_failures;

    void Report(bool ok, const char* expression, const char* file, int line, const QString& note);

#define WPJ2_CHECK(expression) \
    ::wpJ2_test::Report(static_cast<bool>(expression), #expression, __FILE__, __LINE__, QString())
#define WPJ2_CHECK_NOTE(expression, note) \
    ::wpJ2_test::Report(static_cast<bool>(expression), #expression, __FILE__, __LINE__, (note))

    // PumpUntil：反复处理一次 Qt 事件循环直到 predicate 为真或超时。
    // 用途：WorkbenchPageProvider 的读取经 QThreadPool 异步完成，结果要排队
    // 回 UI 线程，测试需要真正跑一段事件循环才能等到回调落地。
    // 传入：predicate 判定是否已达成；timeoutMs 最长等待毫秒数。
    // 传出：达成返回 true；超时仍未达成返回 false——调用方必须用 WPJ2_CHECK
    //       显式断言这个返回值，不能把超时静默当成功（自查清单 j）。
    bool PumpUntil(const std::function<bool()>& predicate, int timeoutMs);

    // ReadCall：FakeAsyncMemoryIoPort 记录的一次 Read 调用。
    struct ReadCall
    {
        std::uint64_t address = 0;
        std::uint64_t length = 0;
    };

    // FakeAsyncMemoryIoPort：线程安全的脚本化假端口。
    // - script_ 按调用顺序消费，下标从 0 开始，但**不是**"第几次调用"
    //   （calls_.size()）本身——每次 SetScript 都会把消费游标 nextScriptIndex_
    //   重置为 0，这样一个端口实例可以在同一个测试里先后服务多次
    //   RequestPages（例如"命中闩锁→resetScratchLatch→再请求一次"这类场景），
    //   每次 SetScript 之后都从新脚本的第 0 条开始消费，不受之前已经发生过
    //   多少次调用的影响；calls_ 仍然是**全程累计**的调用记录，供
    //   CallCount()/MaxConcurrentCalls() 这类"这个端口一共被问了几次"的断言
    //   使用，两者互不干扰。脚本用尽后返回一条显式标注"脚本用尽"的 Failed
    //   结果，而不是悄悄复用最后一条——这是测试脚本的缺口，不是被测对象的
    //   缺陷，用显式失败文案立刻能看出来。
    // - delayMsPerCall_：每次 Read 调用在"记录调用"之后、"读脚本结果"之前睡眠
    //   的毫秒数，用于人为制造"主线程来得及在读取完成前做点别的事"的窗口
    //   （取消/并发/析构这几类测试都要用到这个窗口）。
    // - concurrentCalls_/maxConcurrentCalls_：进入 Read 时自增、离开前自减，
    //   同时记录历史最大值——直接量出"端口侧观察到的调用是否重叠"，不用猜。
    class FakeAsyncMemoryIoPort final : public ksword::memwb::IMemoryIoPort
    {
    public:
        // SetScript：设置按顺序消费的 Read 结果脚本（整体替换）。
        void SetScript(std::vector<ksword::memwb::IoReadResult> script);

        // SetDelayMs：设置之后每次 Read 调用的睡眠毫秒数（0 表示不睡眠）。
        void SetDelayMs(int delayMs);

        // SetLimits：设置 Limits() 的固定返回值。
        void SetLimits(ksword::memwb::IoLimits limits);

        // Calls：按发生顺序记录的全部 Read 调用（线程安全拷贝）。
        std::vector<ReadCall> Calls() const;

        // CallCount：已发生的 Read 调用次数。
        int CallCount() const;

        // MaxConcurrentCalls：历史上同时处于 Read() 内部的最大调用数——应恒为
        // 0 或 1；大于 1 说明端口侧观察到了重叠调用（不变式 5 被打破）。
        int MaxConcurrentCalls() const;

        // ksword::memwb::IMemoryIoPort
        ksword::memwb::IoLimits Limits(const ksword::memwb::MemoryTargetSession& session) const override;
        ksword::memwb::IoReadResult Read(
            const ksword::memwb::MemoryTargetSession& session,
            std::uint64_t address,
            std::uint64_t length) override;
        ksword::memwb::IoWriteResult Write(
            const ksword::memwb::MemoryTargetSession& session,
            std::uint64_t address,
            const std::vector<std::uint8_t>& bytes,
            bool approved) override;

    private:
        mutable std::mutex mutex_;
        ksword::memwb::IoLimits limits_;
        std::vector<ksword::memwb::IoReadResult> script_;
        // nextScriptIndex_：下一次 Read 要消费 script_ 的哪一条，SetScript 时
        // 重置为 0；与 calls_ 的累计计数是两件事，见本类文件头说明。
        std::size_t nextScriptIndex_ = 0;
        std::vector<ReadCall> calls_;
        int delayMs_ = 0;
        std::atomic<int> concurrentCalls_{0};
        std::atomic<int> maxConcurrentCalls_{0};
    };

    // FakeServices：IWorkbenchServices 的极简假实现，只用来撑起一个真实的
    // WorkbenchTarget（本包不测试模块枚举/指针读取/进程候选这些与
    // WorkbenchPageProvider 无关的职责，统一返回"什么都没有但没出错"）。
    // DDMA 代次是唯一需要测试真正控制的维度，专门给一个可配置的原子计数器。
    class FakeServices final : public ks::ui::IWorkbenchServices
    {
    public:
        ks::ui::ModuleEnumResult enumerateProcessModules(std::uint32_t pid, std::uint64_t expectCreateTime) override;
        ks::ui::ModuleEnumResult enumerateKernelModules() override;
        std::vector<ksword::memwb::ProcessCandidate> processCandidates() override;
        ks::ui::PointerReadResult readPointer(
            const ksword::memwb::MemoryTargetSession& session,
            std::uint64_t address,
            std::uint32_t width) override;
        std::uint64_t ddmaGeneration() override;

        // SetDdmaGeneration：设置下一次 ddmaGeneration() 被调用时返回的值；
        // 测试用它模拟"暂存扇区换代"——没有任何回调，只能靠调用方下一次主动
        // 拉取才会发现。
        void SetDdmaGeneration(std::uint64_t generation);

    private:
        std::atomic<std::uint64_t> ddmaGeneration_{0};
    };

    // KernelTargetFixture：MakeKernelTarget 的返回值。services 是非拥有的
    // 裭指针——真正的生命周期由 target 内部的 shared_ptr<IWorkbenchServices>
    // 托管（WorkbenchTarget 的构造契约就是这样设计的，见 WorkbenchTarget.h），
    // 这里不能再用 unique_ptr 持有第二份"所有权"，否则会和 target 内部的
    // shared_ptr 形成双重释放。只要 target 没有被销毁，services 就有效。
    struct KernelTargetFixture
    {
        std::unique_ptr<ks::ui::WorkbenchTarget> target;
        FakeServices* services = nullptr;
    };

    // MakeKernelTarget：构造一个真实的 ks::ui::WorkbenchTarget 与配套的
    // FakeServices，并把会话切到 KernelVirtual 范围（pid 恒为 0，天然合法，
    // 不需要任何进程身份）、channel 指定的通道。
    // 传出：KernelTargetFixture；调用方后续可以继续调用
    //       target->requestChannel/requestScope 做进一步的身份切换，或者
    //       通过 services 配置 DDMA 代次。
    KernelTargetFixture MakeKernelTarget(ksword::memwb::Channel channel);

    // MakeOk / MakePartial / MakeUnreadable / MakeFailed：构造四种
    // IoReadResult，与 MemoryIoTestSupport.h 的同名函数语义一致，独立实现
    // （避免跨包依赖，这几个纯值构造函数几行代码，没有复用的必要）。
    ksword::memwb::IoReadResult MakeOk(std::vector<std::uint8_t> bytes);
    ksword::memwb::IoReadResult MakePartial(std::vector<std::uint8_t> prefix);
    ksword::memwb::IoReadResult MakeUnreadable(std::string text = "unreadable");
    ksword::memwb::IoReadResult MakeFailed(std::string text = "channel failed");
    ksword::memwb::IoReadResult MakeOkWithScratchDirty(std::vector<std::uint8_t> bytes);

    // MakePattern：生成长度为 length 的确定性字节序列，第 i 个字节 =
    // (startValue+i) mod 256——与断言期望值用同一个函数算，不必分别手抄。
    std::vector<std::uint8_t> MakePattern(std::uint8_t startValue, std::size_t length);

    // kPageSize / kPageCountInFixture / kFixtureFirstAddress / kFixtureLastAddress：
    // 全部测试文件共用的地址空间布局——16 页、从地址 0 开始，足够构造"多段
    // range""跨页混合状态"之类的场景，又不至于让夹具数据量太大。
    inline constexpr std::uint64_t kPageSize = 4096;
    inline constexpr std::uint64_t kPageCountInFixture = 16;
    inline constexpr std::uint64_t kFixtureFirstAddress = 0;
    inline constexpr std::uint64_t kFixtureLastAddress = kPageSize * kPageCountInFixture - 1;

    // ProviderFixture：一组互相匹配的真实对象——FakeAsyncMemoryIoPort（WorkbenchPageProvider
    // 拥有）、KernelTargetFixture（真实 WorkbenchTarget + FakeServices）、真实
    // HexCanvas、真实 WorkbenchPageProvider，三者已经接好线（canvas 的
    // pageProvider 指向 provider，provider 的 canvas 指向 canvas）。
    //
    // **Wave 3 决策 2 落实**：canvas 与 target 的来源代次**故意不对齐**——两条
    // 数轴互相独立，没有任何机制强行让它们相等（生产环境里也没有，详见
    // WorkbenchPageProvider.h 文件头"两条数轴"一节）。canvasRevision 只是
    // MakeProviderFixture 构造时 canvas->setAddressSpace(...) 之后读到的
    // canvas 自己的 sourceRevision()，各测试文件调用
    // provider->RequestPages({...}, fixture.canvasRevision) 时传的就是这个
    // "画布轴"的值；凡是需要"目标轴"的地方（陈旧性核对、DDMA 闩锁键），都由
    // provider 内部自己调用 target_->capture() 取得，测试不需要（也不应该）
    // 自己算一个"两边一致"的代次再传进去——旧版本这里有一个
    // AlignCanvasRevisionToTarget 测试专用辅助函数，已按裁决删除；需要"两轴
    // 一致"的测试场景（如果真的需要）请在该测试函数内自己判断两个代次是否
    // 恰好相等，不要重新引入强制对齐的辅助函数。
    struct ProviderFixture
    {
        KernelTargetFixture targetFixture;
        // portPtr：非拥有裭指针，真正的所有权在 provider 内部的 unique_ptr 里
        // （WorkbenchPageProvider.h 的构造契约：portFactory 恰好调用一次，
        // provider 长期持有返回的端口）。
        FakeAsyncMemoryIoPort* portPtr = nullptr;
        std::unique_ptr<ks::ui::HexCanvas> canvas;
        std::unique_ptr<ks::ui::WorkbenchPageProvider> provider;
        // canvasRevision：构造完成时 canvas->sourceRevision() 的值（"画布轴"）。
        std::uint64_t canvasRevision = 0;
    };

    // MakeProviderFixture：按上面的说明组装一套。channel 决定 target 的通道
    // （scope 恒为 KernelVirtual，见 MakeKernelTarget 的说明）。
    ProviderFixture MakeProviderFixture(ksword::memwb::Channel channel);

    // MakeAvailableGateInputs：构造一组"这个通道此刻可用"的 GateInputs——
    // driverLoaded 恒为真（StandardDriver/Hvm/Ddma 都需要），channel==Ddma 时
    // 额外把 ddmaSessionReady 置真；hasProcessTarget 恒为 false（本包的会话
    // 固定是 KernelVirtual，Gate 判定本来就不检查这个范围的 pid）。
    ksword::memwb::GateInputs MakeAvailableGateInputs(ksword::memwb::Channel channel);

    // 各组测试入口（定义在对应的 wpJ2_tests.*.cpp，由 main.cpp 依次调用）。
    void RunGateTests();
    void RunDeliveryTests();
    void RunStaleTests();
    void RunChannelFailedTests();
    void RunLatchTests();
    void RunSerializeTests();
    void RunLifecycleTests();
    // RunSuppTests（Wave 3 新增）：并入独立审核者验证过的补测（原 Repro 组描述
    // 的缺陷已经按 D1-D11 修复，现在全部作为"必须通过"的回归确认跑在默认套件
    // 里，不再需要 WPJ2_REPRO 环境变量）。定义在 wpJ2_tests.Supp.cpp。
    void RunSuppTests();
    // RunDecision2Tests（Wave 3 新增）：决策 2"两条数轴"落实后的专项覆盖——
    // 画布轴与目标轴故意错开的场景、新增 rereadByteRange/retryBlockedByLatch/
    // D3 通道中毒跳过/D4 代次丢弃等本轮新增判断。定义在 wpJ2_tests.Decision2.cpp。
    void RunDecision2Tests();
    // RunSupp2Tests（Wave 3 第二轮审核修复新增）：并入第二轮独立审核
    // （review2-wpJ2.md）的补测——G1-G17 是审核者原样验证过的用例，
    // G18-G23 是本包把审核者原来的缺陷复现按修复后的真实行为转正的"必须
    // 通过"用例；R 组（WPJ2R2_REPRO=1 才跑）是仍未修复/未定论的已知风险。
    // 定义在 wpJ2_tests.Supp2.cpp。
    void RunSupp2Tests();
}
