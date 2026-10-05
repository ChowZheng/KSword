#pragma once

// ============================================================
// wpJ4_common.h
// 作用：WP-J4（WorkbenchWriteController + WorkbenchUndoCoordinator）离屏验证夹具
//       的公共设施——轻量断言计数、可脚本化的 IConfirmPrompter 假弹框执行器、
//       可记录的 IAuditSink、以及"搭一套完整装配"的 Harness（真实
//       WorkbenchTarget + FakeWorkbenchServices + 真实 WorkbenchConfirmations +
//       假端口 + 真实 WorkbenchWriteController）。
// 说明：
// - 本目录（tools/memwb_ui/wpJ4/）是 WP-J4 专属夹具目录；FakeWorkbenchServices/
//   AttachFakeProcess 只读引用自 tools/memwb_ui/wpI/memwb_wpI_common.h（不修改
//   它），FakeMemoryIoPort/FakeKernelMutationPort 只读引用自
//   KswordARKLightTests/MemoryIoTestSupport.h（同样不修改）。
// - 命名空间 wpj4_test，与既有夹具（memwb_test / memwb_wpI_test）区分。
// ============================================================

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchConfirmations.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchTarget.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchWriteController.h"
#include "../../../shared/evidence/memory_workbench/MemoryWritePolicy.h"
#include "../wpI/memwb_wpI_common.h"
#include "MemoryIoTestSupport.h"

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace wpj4_test
{
    // ---- 断言计数：独立计数器，不与其它包/目录共享。----
    extern int g_checks;
    extern int g_failures;

    void Report(bool ok, const char* expression, const char* file, int line, const QString& note);

#define WPJ4_CHECK(expression) \
    ::wpj4_test::Report(static_cast<bool>(expression), #expression, __FILE__, __LINE__, QString())
#define WPJ4_CHECK_NOTE(expression, note) \
    ::wpj4_test::Report(static_cast<bool>(expression), #expression, __FILE__, __LINE__, (note))

    // PumpFor：处理一段时间的 Qt 事件循环（PendingStage 的 2 秒超时计时器需要真实
    // 的事件循环才能触发；本函数不判定任何 predicate，只管把时间耗够）。
    void PumpFor(int milliseconds);

    // PumpUntil：反复处理事件循环直到 predicate 为真或超时；超时仍未达成返回
    // false（调用方应据此判失败，不把超时静默当成功）。
    bool PumpUntil(const std::function<bool()>& predicate, int timeoutMs);

    // ------------------------------------------------------------
    // FakeConfirmPrompter：IConfirmPrompter 的脚本化假实现，供真实
    // WorkbenchConfirmations 注入，避免测试真的弹出模态 QMessageBox。
    // ------------------------------------------------------------
    class FakeConfirmPrompter final : public ks::ui::IConfirmPrompter
    {
    public:
        // ---- 脚本：普通确认 ----
        bool uiConfirmAnswer = true;
        bool uiConfirmDontAskAgain = false;
        bool throwOnUiConfirm = false; // 为真时 PromptUiConfirm 抛异常，供异常安全测试。
        int uiConfirmCallCount = 0;
        // duringUiConfirm（D3 回归测试用）：非空时，在计数/脚本应答之前同步调用一次
        // ——供测试模拟"确认框的模态事件循环还没返回，别的入口在这个窗口里被
        // 触发"这类重入场景（复现 review-wpJ4.md 的探针 reentrant-edit/
        // reentrant-undo），不需要为每个场景各写一个专门的 IConfirmPrompter 子类。
        std::function<void()> duringUiConfirm;
        // 最近一次调用的 offerDontAskAgain 实参，供断言"确认策略表"用。
        bool lastOfferDontAskAgain = false;
        ksword::memwb::Scope lastUiConfirmScope = ksword::memwb::Scope::ProcessVirtual;
        ksword::memwb::Channel lastUiConfirmChannel = ksword::memwb::Channel::UserMode;

        // ---- 脚本：强制同意 ----
        ksword::memwb::ApprovalAnswer approvalAnswer = ksword::memwb::ApprovalAnswer::ThisBlockOnly;
        int approvalCallCount = 0;
        bool lastOfferRestOfBatch = false;

        // ---- 脚本：写入模式三选一 ----
        ksword::memwb::ModeSwitchDecision modeSwitchDecision = ksword::memwb::ModeSwitchDecision::Cancel;
        int modeSwitchCallCount = 0;

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
    };

    // ------------------------------------------------------------
    // FakeAuditSink：记录全部审计事件，供断言"至少有 CommitStarted/CommitFinished"
    // 与"确认被抑制时审计仍然照写"等场景。
    // ------------------------------------------------------------
    class FakeAuditSink final : public ksword::memwb::IAuditSink
    {
    public:
        std::vector<ksword::memwb::AuditRecord> records;

        void Record(const ksword::memwb::AuditRecord& record) override
        {
            records.push_back(record);
        }

        int CountOf(ksword::memwb::AuditEvent event) const
        {
            int count = 0;
            for (const auto& record : records)
            {
                if (record.event == event)
                {
                    ++count;
                }
            }
            return count;
        }
    };

    // ------------------------------------------------------------
    // Harness：一套完整装配——真实 WorkbenchTarget（FakeWorkbenchServices 驱动）、
    // 真实 MemoryDiffOverlay、真实 WorkbenchConfirmations（FakeConfirmPrompter
    // 注入）、FakeAuditSink、假端口（port/kernelPort，由 WorkbenchWriteController
    // 的 ensureTransaction 惰性构造，Harness 持有裸指针供测试脚本化）、真实
    // WorkbenchWriteController。
    // ------------------------------------------------------------
    struct Harness
    {
        Harness();
        ~Harness();

        // Attach：以 ProcessVirtual 范围、UserMode 通道附加一个假进程（pid 可以不
        // 真实存在，锚点句柄用本进程的伪句柄）。
        void AttachProcess(std::uint32_t pid, std::uint64_t attachGeneration = 1);

        // SwitchToKernel / SwitchToPhysical：切到不需要 pid 的范围，供内核分步事务/
        // 物理范围的用例使用。
        void SwitchToKernel(ksword::memwb::Channel channel = ksword::memwb::Channel::StandardDriver);
        void SwitchToPhysical(ksword::memwb::Channel channel = ksword::memwb::Channel::UserMode);

        // LoadBaseline：往 overlay 载入一段基线（全部字节真实读到），供 Stage 用。
        void LoadBaseline(std::uint64_t baseAddress, const std::vector<std::uint8_t>& bytes);

        // Stage：薄转发 overlay.Stage，调用方自己决定要不要紧接着调用
        // controller->onEditCompleted()。
        ksword::memwb::StageStatus Stage(std::uint64_t address, const std::vector<std::uint8_t>& bytes);

        // EnsureWired：强制触发一次 ensureTransaction()（用一次"切到当前模式"
        // 的空操作 requestModeSwitch），让 rawPort/rawKernelPort 在测试真正发起
        // 编辑之前就已经就位，可以提前编排脚本。
        void EnsureWired();

        // 声明顺序即构造顺序：servicesRaw 必须先于 target 声明（target 的构造
        // 依赖 MakeFakeServices 把真实对象的裸指针写进 servicesRaw）。
        memwb_wpI_test::FakeWorkbenchServices* servicesRaw = nullptr;
        ks::ui::WorkbenchTarget target;
        ksword::memwb::MemoryDiffOverlay overlay;
        FakeConfirmPrompter* prompter = nullptr; // 非拥有，所有权在 confirmations 内部。
        ksword::memwb::MemoryWritePolicy policy;
        std::unique_ptr<ks::ui::WorkbenchConfirmations> confirmations;
        FakeAuditSink audit;
        ks::ui::WorkbenchWriteController controller;

        // rawPort / rawKernelPort：ioPortFactory_/kernelPortFactory_ 构造出来的假
        // 端口的裸指针，供测试在触发第一次 Commit 之前编排脚本；所有权已经转交给
        // controller（见 wpJ4_common.cpp 的工厂实现注释）。
        MemwbIoTests::FakeMemoryIoPort* rawPort = nullptr;
        MemwbIoTests::FakeKernelMutationPort* rawKernelPort = nullptr;
        bool kernelPortEnabled = true;

        // readOnlyCalls：canvasReadOnlyHook 被调用的参数序列，供断言"恰好两次,
        // true 后 false"。
        std::vector<bool> readOnlyCalls;
        // commitSuspendCalls（COMMON 裁决第 3 项新增 setCommitSuspendHook 的夹具
        // 记录）：与 readOnlyCalls 分开的另一路挂起通知序列，装配层用它挂起
        // BaselineFeeder；测试断言它与 readOnlyCalls 同步（都只在最外层 Commit
        // 的进入/退出触发一次）。
        std::vector<bool> commitSuspendCalls;
        // rereadCalls：rereadRangeCallback 被调用的 (address,length) 序列。
        std::vector<std::pair<std::uint64_t, std::uint64_t>> rereadCalls;
        // tickValue：被 setTickProvider 注入的固定计时器当前值，测试可以直接改它
        // 来控制 journal 合并窗口判断（而不必真的 sleep）。
        std::uint64_t tickValue = 0;
    };

    // ---- 各组测试入口（定义在对应的 wpJ4_tests.*.cpp，由 wpJ4_main.cpp 依次调用）----
    void RunLifecycleTests();
    void RunCommitTests();
    void RunPendingStageTests();
    void RunUndoTests();
    void RunConfirmationTests();
}
