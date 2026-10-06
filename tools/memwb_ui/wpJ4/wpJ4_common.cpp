// wpJ4_common.cpp
// 作用：wpJ4_common.h 声明的公共设施的实现。

#include "wpJ4_common.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <iostream>
#include <stdexcept>

namespace wpj4_test
{
    int g_checks = 0;
    int g_failures = 0;

    void Report(bool ok, const char* expression, const char* file, int line, const QString& note)
    {
        ++g_checks;
        if (ok)
        {
            return;
        }
        ++g_failures;
        std::cerr << "FAIL: " << expression << "  (" << file << ":" << line << ")";
        if (!note.isEmpty())
        {
            std::cerr << "  " << note.toStdString();
        }
        std::cerr << std::endl;
    }

    void PumpFor(int milliseconds)
    {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < milliseconds)
        {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(2);
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
                return false;
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(2);
        }
        return true;
    }

    // ------------------------------------------------------------
    // FakeConfirmPrompter
    // ------------------------------------------------------------

    bool FakeConfirmPrompter::PromptUiConfirm(
        const ksword::memwb::UiConfirmRequest& /*request*/,
        ksword::memwb::Scope scope,
        ksword::memwb::Channel channel,
        const QString& /*targetDescription*/,
        bool offerDontAskAgain,
        bool& dontAskAgainChecked)
    {
        ++uiConfirmCallCount;
        lastOfferDontAskAgain = offerDontAskAgain;
        lastUiConfirmScope = scope;
        lastUiConfirmChannel = channel;
        if (throwOnUiConfirm)
        {
            throw std::runtime_error("FakeConfirmPrompter::PromptUiConfirm forced failure");
        }
        if (duringUiConfirm)
        {
            // 模拟"确认框的模态事件循环还没返回"这段窗口——真实 QMessageBox::exec()
            // 期间 Qt 事件循环仍会处理排队的信号/计时器，这里用同步调用等价代替
            // （测试自己负责用一次性标记避免无限递归，见各测试用例）。
            duringUiConfirm();
        }
        dontAskAgainChecked = uiConfirmAnswer && offerDontAskAgain && uiConfirmDontAskAgain;
        return uiConfirmAnswer;
    }

    ksword::memwb::ApprovalAnswer FakeConfirmPrompter::PromptApproval(
        const ksword::memwb::ApprovalRequest& /*request*/,
        ksword::memwb::Scope /*scope*/,
        ksword::memwb::Channel /*channel*/,
        const QString& /*targetDescription*/,
        bool offerRestOfBatch)
    {
        ++approvalCallCount;
        lastOfferRestOfBatch = offerRestOfBatch;
        return approvalAnswer;
    }

    ksword::memwb::ModeSwitchDecision FakeConfirmPrompter::PromptModeSwitch(
        ksword::memwb::WriteMode /*fromMode*/,
        ksword::memwb::WriteMode /*toMode*/,
        std::uint64_t /*pendingBytes*/,
        std::uint64_t /*pendingBlocks*/)
    {
        ++modeSwitchCallCount;
        return modeSwitchDecision;
    }

    // ------------------------------------------------------------
    // Harness
    // ------------------------------------------------------------

    namespace
    {
        // MakeFakeServices：构造一份 FakeWorkbenchServices 交给 WorkbenchTarget
        // 持有（unique_ptr 转移所有权），同时把裸指针写回 outRaw，供 Harness 之后
        // 继续配置这份假服务（枚举结果、DDMA 代次等）。
        std::unique_ptr<ks::ui::IWorkbenchServices> MakeFakeServices(
            memwb_wpI_test::FakeWorkbenchServices** outRaw)
        {
            auto services = std::make_unique<memwb_wpI_test::FakeWorkbenchServices>();
            *outRaw = services.get();
            return services;
        }
    }

    Harness::Harness()
        : target(MakeFakeServices(&servicesRaw))
    {
        auto ownedPrompter = std::make_unique<FakeConfirmPrompter>();
        prompter = ownedPrompter.get();
        // dialogParent 传 nullptr：本夹具从不让真实弹框执行器跑起来（prompter 是
        // 假实现），不需要父窗口。
        confirmations = std::make_unique<ks::ui::WorkbenchConfirmations>(
            nullptr, &policy, std::move(ownedPrompter));

        controller.setOverlay(&overlay);
        controller.setTarget(&target);
        controller.setConfirmationSink(confirmations.get());
        controller.setAuditSink(&audit);
        controller.setCanvasReadOnlyHook(
            [this](bool readOnly) { readOnlyCalls.push_back(readOnly); });
        controller.setCommitSuspendHook(
            [this](bool suspended) { commitSuspendCalls.push_back(suspended); });
        controller.setRereadRangeCallback(
            [this](std::uint64_t address, std::uint64_t length)
            {
                rereadCalls.emplace_back(address, length);
            });
        controller.setTickProvider([this]() { return tickValue; });

        controller.setIoPortFactory(
            [this]() -> std::unique_ptr<ksword::memwb::IMemoryIoPort>
            {
                auto port = std::make_unique<MemwbIoTests::FakeMemoryIoPort>();
                rawPort = port.get();
                return port;
            });
        controller.setKernelMutationPortFactory(
            [this]() -> std::unique_ptr<ksword::memwb::IKernelMutationPort>
            {
                if (!kernelPortEnabled)
                {
                    return nullptr;
                }
                auto port = std::make_unique<MemwbIoTests::FakeKernelMutationPort>();
                rawKernelPort = port.get();
                return port;
            });
    }

    Harness::~Harness() = default;

    void Harness::EnsureWired()
    {
        // requestModeSwitch(当前模式) 是一次空操作切换（Switched，什么都不改），
        // 但会先调用 ensureTransaction()，把 rawPort/rawKernelPort/undo_ 都构造
        // 出来，供测试在真正发起编辑之前编排脚本。
        controller.requestModeSwitch(controller.mode());
    }

    void Harness::AttachProcess(std::uint32_t pid, std::uint64_t attachGeneration)
    {
        memwb_wpI_test::AttachFakeProcess(target, pid, attachGeneration);
    }

    void Harness::SwitchToKernel(ksword::memwb::Channel channel)
    {
        target.requestScope(ksword::memwb::Scope::KernelVirtual);
        target.requestChannel(channel);
    }

    void Harness::SwitchToPhysical(ksword::memwb::Channel channel)
    {
        target.requestScope(ksword::memwb::Scope::Physical);
        target.requestChannel(channel);
    }

    void Harness::LoadBaseline(std::uint64_t baseAddress, const std::vector<std::uint8_t>& bytes)
    {
        const std::vector<std::uint8_t> validMask(bytes.size(), 1);
        overlay.LoadBaseline(
            ksword::memwb::IdentityKey(target.session(), baseAddress, bytes.size()),
            baseAddress, bytes, validMask);
    }

    ksword::memwb::StageStatus Harness::Stage(
        std::uint64_t address, const std::vector<std::uint8_t>& bytes)
    {
        return overlay.Stage(address, bytes);
    }
}
