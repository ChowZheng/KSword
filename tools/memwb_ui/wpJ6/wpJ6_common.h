#pragma once

// ============================================================
// wpJ6_common.h
// 作用：WP-J6（MemoryWorkbenchView + WorkbenchDiagnosticsHost）离屏验证夹具的
//       公共设施——轻量断言计数、共享内存模拟假端口（FakeMemoryIoPort，复用
//       wpJ5 同一套写法但独立一份，保持本包与其它包夹具互不依赖）、极简假
//       int3 字节存储（FakeInt3ByteStore，直接对同一份假内存读写单字节，不经
//       MemoryPatchByteStore——那个类要求一个"活得比它久"的 MemoryTargetSession
//       引用，与 Int3Controller::ByteStoreFactory 每次调用都重新构造一个临时
//       store 的契约不兼容，直接实现更简单也更贴合测试意图）、假确认/审计接收器
//       （直接实现 Core 接口）、以及"搭一整套真实装配"的 Harness：
//       WorkbenchShared::Configure（主进程只调一次，在 main() 里完成）+ 真实
//       MemoryWorkbenchView（本包产物，内部装配全部阶段 1/2 真实类）。
//
// 设计取舍（写进报告）：
// - 只测 ProcessVirtual 范围 + UserMode 通道为主，Gate 判定只需要
//   GateInputs.hasProcessTarget；需要测 Gate 不可用/内核范围等场景时按需
//   构造对应的 GateInputs。
// - WorkbenchShared 是进程级单例，Configure 只能成功一次：本夹具的 main()
//   在跑任何测试之前调用一次 ConfigureSharedOnce()，所有 Harness 共用同一套
//   注入的工厂（各自 CreateServices()/CreateIoPort() 仍各自产出独立实例）。
// - 命名空间 wpj6_test，与既有夹具（wpj5_test/wpg_test/memwb_wpI_test 等）区分。
// ============================================================

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/AddressBookPanel.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/Int3PatchPanel.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/MemoryWorkbenchView.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchConfirmations.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchSessionBar.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchStatusBar.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchWriteController.h"
#include "../../../shared/evidence/memory_workbench/Int3PatchLedger.h"
#include "../../../shared/evidence/memory_workbench/MemoryIoPort.h"
#include "../../../shared/evidence/memory_workbench/MemoryWriteTransaction.h"
#include "../wpI/memwb_wpI_common.h"

#include <QCheckBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QString>
#include <QToolButton>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace wpj6_test
{
    // ---- 断言计数 ----
    extern int g_checks;
    extern int g_failures;

    void Report(bool ok, const char* expression, const char* file, int line, const QString& note);

#define WPJ6_CHECK(expression) \
    ::wpj6_test::Report(static_cast<bool>(expression), #expression, __FILE__, __LINE__, QString())
#define WPJ6_CHECK_NOTE(expression, note) \
    ::wpj6_test::Report(static_cast<bool>(expression), #expression, __FILE__, __LINE__, (note))

    // PumpFor / PumpUntil：事件循环驱动，语义同 wpJ5（见该包同名函数的注释）。
    void PumpFor(int milliseconds);
    bool PumpUntil(const std::function<bool()>& predicate, int timeoutMs);

    // WaitForStageable：等到某个地址"可以被 stageBytes 接受"——这需要两层都
    // 落定，不是一层：①画布自己的页缓存已经读到该字节（cellStateAt().hasValue，
    // 由 WorkbenchPageProvider 的异步读线程驱动）；②叠加层的基线窗口已经覆盖
    // 该地址（overlay 的 RefreshBaseline 由 WorkbenchBaselineFeeder 的 50ms
    // 防抖定时器驱动，是在①之后再延迟一步的第二个异步环节）。只等①会让
    // stageBytes 报"写入范围超出当前已读取的数据窗口"——canvas 已经读到了，
    // 但 overlay 的基线窗口还没跟上。
    bool WaitForStageable(ks::ui::WorkbenchHexPane* pane, std::uint64_t address, int timeoutMs = 2000);

    // ------------------------------------------------------------
    // FakeMemoryBacking / FakeMemoryIoPort：共享后备存储的内存模拟假端口。
    // ------------------------------------------------------------
    struct FakeMemoryBacking
    {
        mutable std::mutex mutex;
        std::uint64_t base = 0;
        std::vector<std::uint8_t> bytes;
        int readCallCount = 0;
        int writeCallCount = 0;
    };

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

    // FakeInt3ByteStore：见文件头说明，直接对同一份假内存读写单字节。
    class FakeInt3ByteStore final : public ksword::memwb::IPatchByteStore
    {
    public:
        explicit FakeInt3ByteStore(std::shared_ptr<FakeMemoryBacking> backing);
        bool ReadByte(std::uint64_t address, std::uint8_t& valueOut) override;
        bool WriteByte(std::uint64_t address, std::uint8_t value) override;

    private:
        std::shared_ptr<FakeMemoryBacking> backing_;
    };

    // FakeAuditSink：记录全部审计事件，供"审计没有漏接线"一类的断言使用。
    class FakeAuditSink final : public ksword::memwb::IAuditSink
    {
    public:
        void Record(const ksword::memwb::AuditRecord& record) override;
        std::vector<ksword::memwb::AuditRecord> records;
    };

    // FakeConfirmPrompter：全脚本化的 IConfirmPrompter，直接返回预设答案、不弹
    // 任何真实对话框——离屏自动化测试绝不能触发真实 QMessageBox::exec()（会
    // 用一个真正的嵌套事件循环卡住整个测试进程，直到有人去点它，而离屏环境里
    // 没有人能点）。计数器供"确认框期间重入不嵌套提交"一类的断言使用：测试
    // 可以在 uiConfirmHook/modeSwitchHook 回调里主动触发第二次编辑/撤销，
    // 模拟"确认框的事件循环仍在转"这一刻真实发生的重入。
    class FakeConfirmPrompter final : public ks::ui::IConfirmPrompter
    {
    public:
        bool PromptUiConfirm(
            const ksword::memwb::UiConfirmRequest& request,
            ksword::memwb::Scope scope,
            ksword::memwb::Channel channel,
            const QString& targetDescription,
            bool offerDontAskAgain,
            bool& dontAskAgainChecked) override;
        ksword::memwb::ApprovalAnswer PromptApproval(
            const ksword::memwb::ApprovalRequest& request,
            ksword::memwb::Scope scope,
            ksword::memwb::Channel channel,
            const QString& targetDescription,
            bool offerRestOfBatch) override;
        ksword::memwb::ModeSwitchDecision PromptModeSwitch(
            ksword::memwb::WriteMode fromMode,
            ksword::memwb::WriteMode toMode,
            std::uint64_t pendingBytes,
            std::uint64_t pendingBlocks) override;

        // PromptLeaveWithPending（修复缺陷 3，Wave 3 新增）：覆写
        // IConfirmPrompter 的保守默认实现（恒 Cancel），脚本化返回预设答案并
        // 记录调用次数与收到的参数——供"离开守卫改用专门的三选一框，不再误用
        // PromptModeSwitch"这条修复的测试核对。
        ksword::memwb::ModeSwitchDecision PromptLeaveWithPending(
            std::uint64_t pendingBytes,
            std::uint64_t pendingBlocks,
            const QString& reasonText) override;

        // ---- 脚本/记录 ----
        bool uiConfirmAnswer = true;
        int uiConfirmCallCount = 0;
        ksword::memwb::ApprovalAnswer approvalAnswer = ksword::memwb::ApprovalAnswer::ThisBlockOnly;
        int approvalCallCount = 0;
        ksword::memwb::ModeSwitchDecision modeSwitchDecision = ksword::memwb::ModeSwitchDecision::ApplyThenSwitch;
        int modeSwitchCallCount = 0;
        // leaveWithPendingDecision/leaveWithPendingCallCount/lastLeaveReasonText/
        // lastLeavePendingBytes/lastLeavePendingBlocks：PromptLeaveWithPending
        // 的脚本答案与收到的全部参数，供测试核对"只问一次""正文参数没传错"。
        ksword::memwb::ModeSwitchDecision leaveWithPendingDecision = ksword::memwb::ModeSwitchDecision::Cancel;
        int leaveWithPendingCallCount = 0;
        QString lastLeaveReasonText;
        std::uint64_t lastLeavePendingBytes = 0;
        std::uint64_t lastLeavePendingBlocks = 0;
        // reentrantEdit：非空时，PromptUiConfirm 被调用的那一刻会先执行它一次
        // （模拟"确认框的事件循环仍在转，用户又触发了一次编辑"的真实时序），
        // 再返回 uiConfirmAnswer——不是事后补一次，是在"确认框还没返回"这个
        // 时间点上真正发生。
        std::function<void()> reentrantAction;
    };

    // GateState：进程级共享的 Gate 可用性输入，由测试直接改写，
    // setGateInputsProvider 注入的回调读取它。
    struct GateState
    {
        bool hasProcessTarget = true;
        bool driverLoaded = false;
        ksword::memwb::ProbeState hvmProbe = ksword::memwb::ProbeState::NotProbed;
        bool ddmaSessionReady = false;
    };

    // SharedBackend：ConfigureSharedOnce 一次性注入的全部对象，供各测试文件按需
    // 读取/改写（例如改写 backing 的内容模拟"目标内存"）。
    struct SharedBackend
    {
        std::shared_ptr<FakeMemoryBacking> backing;
        FakeAuditSink auditSink;
    };

    // ConfigureSharedOnce：main() 里唯一调用一次，返回注入的后备存储/审计接收器
    // 供测试读取；重复调用是安全的空操作（只有第一次真正生效，返回同一个实例）。
    SharedBackend& ConfigureSharedOnce();

    // ApplyTheme（修复缺陷 7）：切换深浅主题并同步应用完整调色板，与其它
    // memwb_ui 夹具（wpG_common.cpp 等）同一套惯例——先切 KswordTheme 的深浅
    // 状态（控件 paint 里现取的静态颜色访问器读的是它，不是 QPalette），再
    // 切完整 QApplication 调色板。**必须在构造 MemoryWorkbenchView 之前调用**，
    // 不要对已经构造好、已经在浅色主题下展示过的视图运行期切换——那样窗口
    // 背景仍是浅色而文字取了深色主题的浅色文字色，会"字画在同色底上"，造成
    // 产品缺陷的假象（审核报告 wpJ6/wave3 对本坑的描述；Wave 2 截图曾踩过）。
    void ApplyTheme(bool dark);

    // ------------------------------------------------------------
    // Harness：一整套真实装配——WorkbenchShared（已由 ConfigureSharedOnce 配置）
    // + 真实 MemoryWorkbenchView。
    // ------------------------------------------------------------
    struct Harness
    {
        Harness();
        ~Harness();

        // AttachProcess：等价于 Dock 附加了一个假进程（转发
        // memwb_wpI_test::AttachFakeProcess(view.target(), ...)）。
        void AttachProcess(std::uint32_t fakePid = 4242, std::uint64_t attachGeneration = 1);

        // view：构造好的顶层视图。
        std::unique_ptr<ks::ui::MemoryWorkbenchView> view;

        // gate：本 Harness 的 Gate 输入脚本，经 setGateInputsProvider 注入。
        GateState gate;

        // prompter：view 内部 confirmations_ 持有的假执行器的裸指针（生命周期
        // 由 confirmations_ 管理，本指针只读，不持有）；构造时经
        // InstallConfirmPrompterFactoryForTest 注入，测试据此改写脚本/读计数。
        FakeConfirmPrompter* prompter = nullptr;
    };
}
