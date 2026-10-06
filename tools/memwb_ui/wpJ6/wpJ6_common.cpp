// ============================================================
// wpJ6_common.cpp
// 作用：实现 wpJ6_common.h 声明的全部公共设施，见该文件头的说明。
// ============================================================

#include "wpJ6_common.h"

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchShared.h"
#include "../../../Ksword5.1/Ksword5.1/theme.h"

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QPalette>
#include <QTimer>

#include <algorithm>
#include <iostream>

namespace wpj6_test
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
        std::cout << "CHECK FAIL " << file << ":" << line << " " << expression;
        if (!note.isEmpty())
        {
            std::cout << " -- " << note.toStdString();
        }
        std::cout << std::endl;
    }

    void PumpFor(int milliseconds)
    {
        QEventLoop loop;
        QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
        loop.exec();
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
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        return true;
    }

    // ApplyTheme（修复缺陷 7）：与 wpG_common.cpp 的 ApplyTheme 完全同一套调色板
    // 数值，独立一份避免跨夹具耦合（与该文件注释"避免链接整份文件而单独复制
    // 一遍"同一理由）。见头文件声明处的注释：必须在构造视图之前调用。
    void ApplyTheme(const bool dark)
    {
        KswordTheme::SetDarkModeEnabled(dark);
        QPalette palette = QApplication::palette();
        if (dark)
        {
            palette.setColor(QPalette::Window, QColor(32, 32, 36));
            palette.setColor(QPalette::WindowText, QColor(230, 230, 232));
            palette.setColor(QPalette::Base, QColor(24, 24, 28));
            palette.setColor(QPalette::AlternateBase, QColor(40, 40, 44));
            palette.setColor(QPalette::Text, QColor(230, 230, 232));
            palette.setColor(QPalette::Button, QColor(44, 44, 48));
            palette.setColor(QPalette::ButtonText, QColor(230, 230, 232));
            palette.setColor(QPalette::Highlight, QColor(64, 128, 222));
            palette.setColor(QPalette::HighlightedText, QColor(255, 255, 255));
            palette.setColor(QPalette::PlaceholderText, QColor(140, 140, 146));
            palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(120, 120, 124));
            palette.setColor(QPalette::Disabled, QPalette::Text, QColor(120, 120, 124));
            palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(120, 120, 124));
        }
        else
        {
            palette.setColor(QPalette::Window, QColor(244, 244, 246));
            palette.setColor(QPalette::WindowText, QColor(24, 24, 28));
            palette.setColor(QPalette::Base, QColor(255, 255, 255));
            palette.setColor(QPalette::AlternateBase, QColor(238, 238, 240));
            palette.setColor(QPalette::Text, QColor(24, 24, 28));
            palette.setColor(QPalette::Button, QColor(236, 236, 238));
            palette.setColor(QPalette::ButtonText, QColor(24, 24, 28));
            palette.setColor(QPalette::Highlight, QColor(32, 108, 212));
            palette.setColor(QPalette::HighlightedText, QColor(255, 255, 255));
            palette.setColor(QPalette::PlaceholderText, QColor(120, 120, 124));
            palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(170, 170, 174));
            palette.setColor(QPalette::Disabled, QPalette::Text, QColor(170, 170, 174));
            palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(170, 170, 174));
        }
        qApp->setPalette(palette);
    }

    bool WaitForStageable(ks::ui::WorkbenchHexPane* pane, std::uint64_t address, int timeoutMs)
    {
        if (pane == nullptr || pane->canvas() == nullptr)
        {
            return false;
        }
        auto* canvas = pane->canvas();
        const bool canvasReady = PumpUntil(
            [canvas, address]() { return canvas->cellStateAt(address).hasValue; }, timeoutMs);
        if (!canvasReady)
        {
            return false;
        }
        return PumpUntil(
            [pane, address]() { return pane->overlay().BaselineByte(address).has_value(); }, timeoutMs);
    }

    // ------------------------------------------------------------
    // FakeMemoryIoPort
    // ------------------------------------------------------------

    FakeMemoryIoPort::FakeMemoryIoPort(std::shared_ptr<FakeMemoryBacking> backing) : backing_(std::move(backing)) {}

    ksword::memwb::IoLimits FakeMemoryIoPort::Limits(const ksword::memwb::MemoryTargetSession& session) const
    {
        Q_UNUSED(session);
        return ksword::memwb::IoLimits{};
    }

    ksword::memwb::IoReadResult FakeMemoryIoPort::Read(
        const ksword::memwb::MemoryTargetSession& session,
        std::uint64_t address,
        std::uint64_t length)
    {
        Q_UNUSED(session);
        std::lock_guard<std::mutex> lock(backing_->mutex);
        ++backing_->readCallCount;
        ksword::memwb::IoReadResult result;
        if (length == 0)
        {
            result.status = ksword::memwb::IoReadStatus::Ok;
            return result;
        }
        const std::uint64_t backingEnd = backing_->base + backing_->bytes.size();
        if (address < backing_->base || address >= backingEnd)
        {
            // 请求起点本身就落在覆盖范围外：目标不可读（通道是健康的），哪怕
            // 后面的字节技术上"存在"也不构成一个从 address 开始的前缀。
            result.status = ksword::memwb::IoReadStatus::Unreadable;
            return result;
        }
        const std::uint64_t availableFromAddress = backingEnd - address;
        const std::uint64_t prefixLength = (std::min)(length, availableFromAddress);
        result.data.assign(prefixLength, 0);
        for (std::uint64_t i = 0; i < prefixLength; ++i)
        {
            result.data[i] = backing_->bytes[(address - backing_->base) + i];
        }
        result.status = (prefixLength == length)
            ? ksword::memwb::IoReadStatus::Ok
            : ksword::memwb::IoReadStatus::Partial;
        return result;
    }

    ksword::memwb::IoWriteResult FakeMemoryIoPort::Write(
        const ksword::memwb::MemoryTargetSession& session,
        std::uint64_t address,
        const std::vector<std::uint8_t>& bytes,
        bool approved)
    {
        Q_UNUSED(session);
        Q_UNUSED(approved);
        std::lock_guard<std::mutex> lock(backing_->mutex);
        ++backing_->writeCallCount;
        ksword::memwb::IoWriteResult result;
        const std::uint64_t backingEnd = backing_->base + backing_->bytes.size();
        if (address < backing_->base || address + bytes.size() > backingEnd)
        {
            result.ok = false;
            return result;
        }
        for (std::size_t i = 0; i < bytes.size(); ++i)
        {
            backing_->bytes[(address - backing_->base) + i] = bytes[i];
        }
        result.ok = true;
        result.bytesDone = bytes.size();
        return result;
    }

    // ------------------------------------------------------------
    // FakeInt3ByteStore
    // ------------------------------------------------------------

    FakeInt3ByteStore::FakeInt3ByteStore(std::shared_ptr<FakeMemoryBacking> backing) : backing_(std::move(backing)) {}

    bool FakeInt3ByteStore::ReadByte(std::uint64_t address, std::uint8_t& valueOut)
    {
        std::lock_guard<std::mutex> lock(backing_->mutex);
        if (address < backing_->base || address >= backing_->base + backing_->bytes.size())
        {
            return false;
        }
        valueOut = backing_->bytes[address - backing_->base];
        return true;
    }

    bool FakeInt3ByteStore::WriteByte(std::uint64_t address, std::uint8_t value)
    {
        std::lock_guard<std::mutex> lock(backing_->mutex);
        if (address < backing_->base || address >= backing_->base + backing_->bytes.size())
        {
            return false;
        }
        backing_->bytes[address - backing_->base] = value;
        return true;
    }

    // ------------------------------------------------------------
    // FakeAuditSink
    // ------------------------------------------------------------

    void FakeAuditSink::Record(const ksword::memwb::AuditRecord& record)
    {
        records.push_back(record);
    }

    // ------------------------------------------------------------
    // FakeConfirmPrompter
    // ------------------------------------------------------------

    bool FakeConfirmPrompter::PromptUiConfirm(
        const ksword::memwb::UiConfirmRequest& request,
        ksword::memwb::Scope scope,
        ksword::memwb::Channel channel,
        const QString& targetDescription,
        bool offerDontAskAgain,
        bool& dontAskAgainChecked)
    {
        Q_UNUSED(request);
        Q_UNUSED(scope);
        Q_UNUSED(channel);
        Q_UNUSED(targetDescription);
        Q_UNUSED(offerDontAskAgain);
        ++uiConfirmCallCount;
        if (reentrantAction)
        {
            // 在真正返回答案之前先执行重入动作——模拟确认框的事件循环仍在转
            // 这一刻真实可能发生的情况（见 .h 文件头说明）。
            reentrantAction();
        }
        dontAskAgainChecked = false;
        return uiConfirmAnswer;
    }

    ksword::memwb::ApprovalAnswer FakeConfirmPrompter::PromptApproval(
        const ksword::memwb::ApprovalRequest& request,
        ksword::memwb::Scope scope,
        ksword::memwb::Channel channel,
        const QString& targetDescription,
        bool offerRestOfBatch)
    {
        Q_UNUSED(request);
        Q_UNUSED(scope);
        Q_UNUSED(channel);
        Q_UNUSED(targetDescription);
        Q_UNUSED(offerRestOfBatch);
        ++approvalCallCount;
        return approvalAnswer;
    }

    ksword::memwb::ModeSwitchDecision FakeConfirmPrompter::PromptModeSwitch(
        ksword::memwb::WriteMode fromMode,
        ksword::memwb::WriteMode toMode,
        std::uint64_t pendingBytes,
        std::uint64_t pendingBlocks)
    {
        Q_UNUSED(fromMode);
        Q_UNUSED(toMode);
        Q_UNUSED(pendingBytes);
        Q_UNUSED(pendingBlocks);
        ++modeSwitchCallCount;
        if (reentrantAction)
        {
            reentrantAction();
        }
        return modeSwitchDecision;
    }

    ksword::memwb::ModeSwitchDecision FakeConfirmPrompter::PromptLeaveWithPending(
        const std::uint64_t pendingBytes,
        const std::uint64_t pendingBlocks,
        const QString& reasonText)
    {
        ++leaveWithPendingCallCount;
        lastLeavePendingBytes = pendingBytes;
        lastLeavePendingBlocks = pendingBlocks;
        lastLeaveReasonText = reasonText;
        return leaveWithPendingDecision;
    }

    namespace
    {
        // AuditForwarder：Configure() 的 auditSinkFactory 只被调用一次，这里转发
        // 给进程级共享的 FakeAuditSink，让测试能在任意时刻读取 records（不需要
        // 持有 Configure 内部才拿到的那个 unique_ptr）。
        class AuditForwarder final : public ksword::memwb::IAuditSink
        {
        public:
            explicit AuditForwarder(FakeAuditSink* target) : target_(target) {}
            void Record(const ksword::memwb::AuditRecord& record) override
            {
                if (target_ != nullptr)
                {
                    target_->Record(record);
                }
            }

        private:
            FakeAuditSink* target_ = nullptr;
        };
    }

    SharedBackend& ConfigureSharedOnce()
    {
        static SharedBackend backend;
        static bool configured = false;
        if (!configured)
        {
            // base 必须是 0——ProcessVirtual 地址空间从 0 开始，HexCanvas 构造/
            // 换地址空间后默认显示的初始可见范围就在地址 0 附近；假内存必须
            // 覆盖这段范围，否则测试里"暂存一个字节"永远会因为"屏幕上看不到
            // 值"（未读到）被拒绝（见 HexCanvas.h"五、编辑"一节的检查顺序）。
            backend.backing = std::make_shared<FakeMemoryBacking>();
            backend.backing->base = 0ULL;
            backend.backing->bytes.assign(0x20000ULL, 0);

            ks::ui::WorkbenchShared::WorkbenchBackends backends;
            backends.addressBookFilePath = QString();
            backends.int3Factory = [backing = backend.backing](
                                        const ksword::memwb::PatchTarget& target,
                                        ksword::memwb::Channel channel) -> std::unique_ptr<ksword::memwb::IPatchByteStore> {
                Q_UNUSED(target);
                Q_UNUSED(channel);
                return std::make_unique<FakeInt3ByteStore>(backing);
            };
            backends.servicesFactory = []() -> std::unique_ptr<ks::ui::IWorkbenchServices> {
                return std::make_unique<memwb_wpI_test::FakeWorkbenchServices>();
            };
            backends.ioPortFactory = [backing = backend.backing]() -> std::unique_ptr<ksword::memwb::IMemoryIoPort> {
                return std::make_unique<FakeMemoryIoPort>(backing);
            };
            backends.kernelPortFactory = nullptr;
            backends.auditSinkFactory = [sink = &backend.auditSink]() -> std::unique_ptr<ksword::memwb::IAuditSink> {
                return std::make_unique<AuditForwarder>(sink);
            };

            const bool ok = ks::ui::WorkbenchShared::Instance().Configure(std::move(backends));
            if (!ok)
            {
                qWarning() << "wpJ6_test::ConfigureSharedOnce: Configure 失败，诊断="
                           << static_cast<int>(ks::ui::WorkbenchShared::Instance().ConfigureRejectedReason());
            }
            configured = true;
        }
        return backend;
    }

    // ------------------------------------------------------------
    // Harness
    // ------------------------------------------------------------

    Harness::Harness()
    {
        ConfigureSharedOnce();

        // 先装好假执行器，再构造 view——InstallConfirmPrompterFactoryForTest
        // 只消费"下一次"构造，必须严格按这个顺序，否则会退回真实 QMessageBox
        // 实现（见该方法的注释）。raw 指针留给 Harness::prompter，工厂本身把
        // 所有权转给 confirmations_。
        auto* rawPrompter = new FakeConfirmPrompter();
        prompter = rawPrompter;
        ks::ui::MemoryWorkbenchView::InstallConfirmPrompterFactoryForTest(
            [rawPrompter]() -> std::unique_ptr<ks::ui::IConfirmPrompter> {
                return std::unique_ptr<ks::ui::IConfirmPrompter>(rawPrompter);
            });

        view = std::make_unique<ks::ui::MemoryWorkbenchView>();
        view->setGateInputsProvider([this]() -> ksword::memwb::GateInputs {
            ksword::memwb::GateInputs inputs;
            inputs.hasProcessTarget = gate.hasProcessTarget;
            inputs.driverLoaded = gate.driverLoaded;
            inputs.hvmProbe = gate.hvmProbe;
            inputs.ddmaSessionReady = gate.ddmaSessionReady;
            return inputs;
        });
    }

    Harness::~Harness() = default;

    void Harness::AttachProcess(std::uint32_t fakePid, std::uint64_t attachGeneration)
    {
        gate.hasProcessTarget = true;
        memwb_wpI_test::AttachFakeProcess(view->target(), fakePid, attachGeneration);
    }
}
