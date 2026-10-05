#pragma once

// ============================================================
// wpJ5_common.h
// 作用：WP-J5（WorkbenchHexPane）离屏验证夹具的公共设施——轻量断言计数、
//       线程安全的内存模拟假端口（FakeMemoryIoPort，共享一份字节后备存储，
//       供真实 WorkbenchPageProvider 的读线程与真实 WorkbenchWriteController
//       的写入共用同一份"目标内存"）、极简假确认接收器/审计接收器（直接实现
//       ksword::memwb::IConfirmationSink/IAuditSink，不经由 WorkbenchConfirmations
//       ——本包只测 WorkbenchHexPane 自己的接线，不需要真实确认对话框那一层）、
//       以及"搭一套完整装配"的 Harness：真实 WorkbenchTarget（复用 wpI 的
//       FakeWorkbenchServices/AttachFakeProcess，只读引用不修改）+ 真实
//       WorkbenchPageProvider + 真实 WorkbenchBaselineFeeder + 真实
//       WorkbenchWriteController + 真实 WorkbenchHexPane（本包产物）。
//
// 设计取舍（写进报告，供审核核对）：
// - 只测 ProcessVirtual 范围 + UserMode 通道：Gate 判定只需要
//   GateInputs.hasProcessTarget，不需要构造任何驱动/DDMA 状态，把"HexPane 自己
//   的接线对不对"与"后端通道选择对不对"（那是 WP-J2/J4 的职责，已经独立验证
//   过）解耦。内核分步字节事务（IKernelMutationPort）同理不构造——UserMode 写入
//   走 MemoryIoByteStore 的"其余范围/通道"分支，直接调用端口 Write，不需要
//   kernelMutationPort_。
// - FakeMemoryIoPort 的后备存储（FakeMemoryBacking）用 shared_ptr 在
//   WorkbenchPageProvider 的 IoPortFactory 与 WorkbenchWriteController 的
//   IoPortFactory 之间共享——两条管线各自通过自己的工厂函数构造**不同**的
//   FakeMemoryIoPort 对象（各自只调用一次，符合两个工厂"只调用一次"的头文件
//   契约），但指向同一份后备字节，因此"写入之后重读能看到新值"可以用真实的
//   提交+重读路径端到端验证，不需要在测试里手工模拟"写完之后把字节搬进假端口
//   的脚本"。
// - 不走 WorkbenchConfirmations：WorkbenchWriteController::setConfirmationSink
//   接受的是 Core 接口 ksword::memwb::IConfirmationSink*，不是
//   ks::ui::WorkbenchConfirmations*（头文件逐字核对过，WorkbenchWriteController.h
//   不包含 WorkbenchConfirmations.h）。直接实现这个两方法的接口，避免引入
//   WorkbenchMessages.cpp 经 ks::i18n::LanguageManager 查语言包的依赖链。
// - 命名空间 wpj5_test，与既有夹具（wpj2_test/wpj4_test/memwb_wpI_test 等）区分。
// ============================================================

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchBaselineFeeder.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchHexPane.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchPageProvider.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchTarget.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchWriteController.h"
#include "../../../shared/evidence/memory_workbench/MemoryIoPort.h"
#include "../wpI/memwb_wpI_common.h"

#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace wpj5_test
{
    // ---- 断言计数：与既有夹具同样的轻量框架，独立计数器、独立命名空间。----
    extern int g_checks;
    extern int g_failures;

    void Report(bool ok, const char* expression, const char* file, int line, const QString& note);

#define WPJ5_CHECK(expression) \
    ::wpj5_test::Report(static_cast<bool>(expression), #expression, __FILE__, __LINE__, QString())
#define WPJ5_CHECK_NOTE(expression, note) \
    ::wpj5_test::Report(static_cast<bool>(expression), #expression, __FILE__, __LINE__, (note))

    // PumpFor：处理一段时间的 Qt 事件循环，不判定任何条件，只管把时间耗够
    // （某些场景需要确保"这段时间内没有发生任何事"，而不是等到某个条件成立）。
    void PumpFor(int milliseconds);

    // PumpUntil：反复处理一次 Qt 事件循环直到 predicate 为真或超时。
    // 用途：页读取经 QThreadPool 异步完成，结果要排队回 UI 线程，测试需要真正
    // 跑一段事件循环才能等到回调落地。
    // 传出：达成返回 true；超时仍未达成返回 false——调用方必须用 WPJ5_CHECK
    //       显式断言这个返回值，不能把超时静默当成功（自查清单 j）。
    bool PumpUntil(const std::function<bool()>& predicate, int timeoutMs);

    // ------------------------------------------------------------
    // FakeMemoryBacking / FakeMemoryIoPort：共享后备存储的内存模拟假端口。
    // ------------------------------------------------------------

    // FakeMemoryBacking：一段连续的"假目标内存"，base 为起始绝对地址，bytes
    // 为该段内容；线程安全（读线程与 UI 线程都会触碰）。
    struct FakeMemoryBacking
    {
        mutable std::mutex mutex;
        std::uint64_t base = 0;
        std::vector<std::uint8_t> bytes;
        // readCallCount / writeCallCount：端口被真正问到的次数，供"端口侧观察到
        // 的调用次数"一类的断言使用（例如核对 Gate 拒绝时端口调用次数恒为 0）。
        int readCallCount = 0;
        int writeCallCount = 0;
        // forceReadFailed：为真时，Read 恒返回 Failed（通道自身失败），不触碰
        // bytes——供"通道失败时 provider 不自动重试"一类的场景使用。
        bool forceReadUnreadable = false;
    };

    // FakeMemoryIoPort：IMemoryIoPort 的内存模拟实现，见文件头设计取舍一节。
    // Limits 恒返回"不限"（0/0），Read/Write 按 [address, address+length) 与
    // backing 的覆盖范围求交集：完全落在覆盖范围内按 Ok 处理，部分落在范围内
    // 按 Partial/截断写入处理，完全落在范围外按 Unreadable（读）/失败（写）处理。
    class FakeMemoryIoPort final : public ksword::memwb::IMemoryIoPort
    {
    public:
        explicit FakeMemoryIoPort(std::shared_ptr<FakeMemoryBacking> backing);

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
        std::shared_ptr<FakeMemoryBacking> backing_;
    };

    // ------------------------------------------------------------
    // FakeConfirmationSink / FakeAuditSink：直接实现 Core 接口的极简假实现。
    // ------------------------------------------------------------

    class FakeConfirmationSink final : public ksword::memwb::IConfirmationSink
    {
    public:
        bool ConfirmUi(const ksword::memwb::UiConfirmRequest& request) override;
        ksword::memwb::ApprovalAnswer ConfirmApproval(const ksword::memwb::ApprovalRequest& request) override;

        // ---- 脚本/记录 ----
        bool uiConfirmAnswer = true;
        int uiConfirmCallCount = 0;
        ksword::memwb::ApprovalAnswer approvalAnswer = ksword::memwb::ApprovalAnswer::ThisBlockOnly;
        int approvalCallCount = 0;
    };

    class FakeAuditSink final : public ksword::memwb::IAuditSink
    {
    public:
        void Record(const ksword::memwb::AuditRecord& record) override;
        std::vector<ksword::memwb::AuditRecord> records;
    };

    // ------------------------------------------------------------
    // Harness：一套完整装配。声明顺序即构造顺序（C++ 规则），servicesRaw 必须
    // 先于 target、backing 必须先于 provider（provider 的构造函数参数里的
    // IoPortFactory 立即捕获 backing_，不是惰性 setter）。
    // ------------------------------------------------------------
    struct Harness
    {
        // 构造：backingBase/backingBytes 构成初始"假目标内存"；地址空间本身
        // 由测试随后调用 SetAddressSpace 决定（通常就是 [backingBase,
        // backingBase+backingBytes.size()-1]，但两者故意分开传入，方便测试
        // "地址空间比假内存覆盖范围更大，末尾部分读不到"这类场景）。
        explicit Harness(std::uint64_t backingBase, std::vector<std::uint8_t> backingBytes);
        ~Harness();

        // AttachProcess：模拟 Dock 附加了一个假进程（见 memwb_wpI_test::
        // AttachFakeProcess 的说明：不要求 pid 真实存在，只是让会话有一个非零
        // pid，Gate 判定里的 hasProcessTarget 才能为真）。
        void AttachProcess(std::uint32_t fakePid = 4242, std::uint64_t attachGeneration = 1);

        // SetAddressSpace：等价于装配层的 pane.setAddressSpace(first, last,
        // 身份串)，身份串用 ksword::memwb::IdentityKey(target.session(), 0, 0)
        // 统一规则（设计文档 §1："overlay identity=IdentityKey(session,0,0)"，
        // 0,0 是固定占位，不是真实窗口基址/长度——否则每次换窗口都会被当成换
        // 身份）。
        void SetAddressSpace(std::uint64_t first, std::uint64_t last);

        // ClearAddressSpace：等价于 pane.clearAddressSpace()。
        void ClearAddressSpace();

        // CurrentIdentityKey：按上面同一条规则现算一次当前身份串，供测试断言
        // overlay().IdentityKey() 是否与之相符。
        std::string CurrentIdentityKey() const;

        // WaitUntilSettled：反复处理事件循环，直到画布对 address 的页状态不再是
        // NotLoaded/Pending（即 Valid 或 Unreadable，两者都算"已落定"），用于
        // 确认一次异步页读取真正落地。超时返回 false。
        bool WaitUntilSettled(std::uint64_t address, int timeoutMs = 2000);

        // WaitUntilNoInFlight：反复处理事件循环，直到
        // provider.hasInFlightRequests()==false。超时返回 false。
        bool WaitUntilNoInFlight(int timeoutMs = 2000);

        // 声明顺序即构造顺序，注释见本结构体声明处与各成员自身。
        memwb_wpI_test::FakeWorkbenchServices* servicesRaw = nullptr;
        ks::ui::WorkbenchTarget target;
        std::shared_ptr<FakeMemoryBacking> backing;
        ks::ui::WorkbenchHexPane pane;
        ks::ui::WorkbenchPageProvider provider;
        ks::ui::WorkbenchBaselineFeeder feeder;
        FakeConfirmationSink confirmSink;
        FakeAuditSink auditSink;
        ks::ui::WorkbenchWriteController controller;

        // gateAvailable：GateInputs.hasProcessTarget 的当前值（provider 的
        // GateInputsProvider 读取它）；默认 true，测试可以把它改成 false 来
        // 模拟"通道此刻不可用"。
        bool gateAvailable = true;
        // sourceRevisionOverride：pane.setSourceRevisionProvider 注入的回调
        // 读取的值；测试直接改写它来模拟"目标轴来源代次前进"，不需要真的调用
        // target.requestReload()。
        std::uint64_t sourceRevisionOverride = 0;

        // jobLandedCount / commitFinishedCount：计数器，供"恰好落地/提交了几次"
        // 一类的断言使用。
        int jobLandedCount = 0;
        int commitFinishedCount = 0;

        // readOnlyHookCalls / suspendHookCalls：模拟装配层（MemoryWorkbenchView）
        // 把 controller 的两个提交期间钩子接到 pane.setEditable 与
        // feeder.setSuspended 的那条接线（装配接口文档 §3："writeController 的
        // canvasReadOnly/commitSuspend 钩子由 View 接到本类的 setEditable 与
        // feeder.setSuspended，本类只提供 setEditable"——这条接线本身不属于
        // WorkbenchHexPane，但端到端测试"提交期间画布只读"需要有人把它接上，
        // 所以由本 Harness 代办，等价于一个最小 View）。
        std::vector<bool> readOnlyHookCalls;
        std::vector<bool> suspendHookCalls;
    };

    // ---- 各组测试入口（定义在对应的 wpJ5_tests.*.cpp，由 wpJ5_main.cpp 依次调用）----
    void RunWiringTests();
    void RunBaselineTests();
    void RunWriteTests();
    void RunFindTests();
    void RunNavigationTests();
    void RunVisualTests();
}
