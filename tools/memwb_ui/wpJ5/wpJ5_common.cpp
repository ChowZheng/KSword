// wpJ5_common.cpp
// 作用：wpJ5_common.h 声明的公共设施的实现。

#include "wpJ5_common.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <algorithm>
#include <iostream>

namespace wpj5_test
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
    // FakeMemoryIoPort
    // ------------------------------------------------------------

    FakeMemoryIoPort::FakeMemoryIoPort(std::shared_ptr<FakeMemoryBacking> backing)
        : backing_(std::move(backing))
    {
    }

    ksword::memwb::IoLimits FakeMemoryIoPort::Limits(const ksword::memwb::MemoryTargetSession& /*session*/) const
    {
        // 0/0 = 不限：MemoryIoByteStore/MemoryPageReader 把整段请求当一块处理，
        // 测试断言端口调用次数时不需要再推算分块数。
        return ksword::memwb::IoLimits{};
    }

    ksword::memwb::IoReadResult FakeMemoryIoPort::Read(
        const ksword::memwb::MemoryTargetSession& /*session*/,
        std::uint64_t address,
        std::uint64_t length)
    {
        std::lock_guard<std::mutex> lock(backing_->mutex);
        ++backing_->readCallCount;

        ksword::memwb::IoReadResult result;
        if (length == 0)
        {
            result.status = ksword::memwb::IoReadStatus::Ok;
            return result;
        }
        if (backing_->forceReadUnreadable)
        {
            result.status = ksword::memwb::IoReadStatus::Unreadable;
            result.failure = "fake: forced unreadable";
            return result;
        }

        const std::uint64_t base = backing_->base;
        const std::uint64_t backingSize = static_cast<std::uint64_t>(backing_->bytes.size());
        // 请求范围与后备存储覆盖范围 [base, base+backingSize) 求交集的"可读前缀"。
        if (address < base || address - base >= backingSize)
        {
            // 起点本身就在覆盖范围之外：一个字节都读不到，原因在目标本身。
            result.status = ksword::memwb::IoReadStatus::Unreadable;
            result.failure = "fake: address outside backing";
            return result;
        }
        const std::uint64_t offset = address - base;
        const std::uint64_t available = backingSize - offset;
        const std::uint64_t toRead = (std::min)(available, length);
        result.data.assign(
            backing_->bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            backing_->bytes.begin() + static_cast<std::ptrdiff_t>(offset + toRead));
        result.status = (toRead == length) ? ksword::memwb::IoReadStatus::Ok : ksword::memwb::IoReadStatus::Partial;
        return result;
    }

    ksword::memwb::IoWriteResult FakeMemoryIoPort::Write(
        const ksword::memwb::MemoryTargetSession& /*session*/,
        std::uint64_t address,
        const std::vector<std::uint8_t>& bytes,
        bool /*approved*/)
    {
        std::lock_guard<std::mutex> lock(backing_->mutex);
        ++backing_->writeCallCount;

        ksword::memwb::IoWriteResult result;
        if (bytes.empty())
        {
            result.ok = true;
            return result;
        }
        const std::uint64_t base = backing_->base;
        const std::uint64_t backingSize = static_cast<std::uint64_t>(backing_->bytes.size());
        if (address < base || address - base + bytes.size() > backingSize)
        {
            result.ok = false;
            result.failure = "fake: write outside backing";
            return result;
        }
        const std::uint64_t offset = address - base;
        std::copy(bytes.begin(), bytes.end(), backing_->bytes.begin() + static_cast<std::ptrdiff_t>(offset));
        result.ok = true;
        result.bytesDone = bytes.size();
        return result;
    }

    // ------------------------------------------------------------
    // FakeConfirmationSink / FakeAuditSink
    // ------------------------------------------------------------

    bool FakeConfirmationSink::ConfirmUi(const ksword::memwb::UiConfirmRequest& /*request*/)
    {
        ++uiConfirmCallCount;
        return uiConfirmAnswer;
    }

    ksword::memwb::ApprovalAnswer FakeConfirmationSink::ConfirmApproval(const ksword::memwb::ApprovalRequest& /*request*/)
    {
        ++approvalCallCount;
        return approvalAnswer;
    }

    void FakeAuditSink::Record(const ksword::memwb::AuditRecord& record)
    {
        records.push_back(record);
    }

    // ------------------------------------------------------------
    // Harness
    // ------------------------------------------------------------

    namespace
    {
        // MakeFakeServices：构造一份 FakeWorkbenchServices 交给 WorkbenchTarget
        // 持有（unique_ptr 转移所有权），裸指针写回 outRaw 供 Harness 继续配置。
        std::unique_ptr<ks::ui::IWorkbenchServices> MakeFakeServices(memwb_wpI_test::FakeWorkbenchServices** outRaw)
        {
            auto services = std::make_unique<memwb_wpI_test::FakeWorkbenchServices>();
            *outRaw = services.get();
            return services;
        }
    }

    Harness::Harness(std::uint64_t backingBase, std::vector<std::uint8_t> backingBytes)
        : target(MakeFakeServices(&servicesRaw))
        , backing(std::make_shared<FakeMemoryBacking>())
        , provider(
              [b = backing]() -> std::unique_ptr<ksword::memwb::IMemoryIoPort> {
                  return std::make_unique<FakeMemoryIoPort>(b);
              },
              &target)
    {
        backing->base = backingBase;
        backing->bytes = std::move(backingBytes);

        // ---- 三条管线接到本类（充当装配层/View）与 WorkbenchHexPane ----
        pane.setPageProvider(&provider);
        pane.setBaselineFeeder(&feeder);
        pane.setWriteController(&controller);
        pane.setSourceRevisionProvider([this]() { return sourceRevisionOverride; });

        provider.setGateInputsProvider([this]() {
            ksword::memwb::GateInputs inputs;
            inputs.hasProcessTarget = gateAvailable;
            inputs.driverLoaded = false;
            inputs.ddmaSessionReady = false;
            return inputs;
        });
        QObject::connect(&provider, &ks::ui::WorkbenchPageProvider::jobLanded, &provider, [this]() { ++jobLandedCount; });

        controller.setTarget(&target);
        controller.setConfirmationSink(&confirmSink);
        controller.setAuditSink(&auditSink);
        controller.setIoPortFactory([b = backing]() -> std::unique_ptr<ksword::memwb::IMemoryIoPort> {
            return std::make_unique<FakeMemoryIoPort>(b);
        });
        QObject::connect(&controller, &ks::ui::WorkbenchWriteController::commitFinished, &controller,
            [this](ksword::memwb::CommitReport) { ++commitFinishedCount; });

        // 代办装配层的那条钩子接线（见头文件 readOnlyHookCalls/suspendHookCalls
        // 的说明）：真实 View 把这两个回调接到 pane.setEditable 与
        // feeder.setSuspended，这里原样照做，顺手记一份调用序列供断言。
        controller.setCanvasReadOnlyHook([this](bool readOnlyDuringCommit) {
            readOnlyHookCalls.push_back(readOnlyDuringCommit);
            pane.setEditable(!readOnlyDuringCommit);
        });
        controller.setCommitSuspendHook([this](bool suspendedDuringCommit) {
            suspendHookCalls.push_back(suspendedDuringCommit);
            feeder.setSuspended(suspendedDuringCommit);
        });

        // 装配层另外两条真实接线（装配接口文档 §3），同样由本 Harness 代办：
        // ①画布 editStaged→controller.onEditCompleted（Direct，立即模式下必须
        //   在同一调用栈内决定是否 Commit）；②controller 的局部重读回调接到
        // pane.rereadWindow()（任务书允许的增量③的典型用法之一——"写后局部
        // 重读"，不需要调用方自己算范围，见该方法的声明处注释）。
        QObject::connect(pane.canvas(), &ks::ui::HexCanvas::editStaged, &controller,
            &ks::ui::WorkbenchWriteController::onEditCompleted);
        controller.setRereadRangeCallback(
            [this](std::uint64_t /*address*/, std::uint64_t /*length*/) { pane.rereadWindow(); });
    }

    Harness::~Harness() = default;

    void Harness::AttachProcess(std::uint32_t fakePid, std::uint64_t attachGeneration)
    {
        memwb_wpI_test::AttachFakeProcess(target, fakePid, attachGeneration);
    }

    std::string Harness::CurrentIdentityKey() const
    {
        // target.session() 不是 const（可能拉取 DDMA 代次），但本包只用 UserMode
        // 通道，不触发那条路径；这里用 const_cast 只是为了匹配头文件签名，不会
        // 真的改变任何与 DDMA 相关的状态。
        return ksword::memwb::IdentityKey(const_cast<ks::ui::WorkbenchTarget&>(target).session(), 0, 0);
    }

    void Harness::SetAddressSpace(std::uint64_t first, std::uint64_t last)
    {
        pane.setAddressSpace(first, last, CurrentIdentityKey());
    }

    void Harness::ClearAddressSpace()
    {
        pane.clearAddressSpace();
    }

    bool Harness::WaitUntilSettled(std::uint64_t address, int timeoutMs)
    {
        return PumpUntil(
            [this, address]() {
                const ks::ui::HexCanvas::CellState state = pane.canvas()->cellStateAt(address);
                return state.byteState != ks::ui::HexCanvas::ByteState::NotLoaded
                    && state.byteState != ks::ui::HexCanvas::ByteState::Pending;
            },
            timeoutMs);
    }

    bool Harness::WaitUntilNoInFlight(int timeoutMs)
    {
        return PumpUntil([this]() { return !provider.hasInFlightRequests(); }, timeoutMs);
    }
}
