// memwb_wpI_common.cpp
// 作用：memwb_wpI_common.h 声明的公共设施的实现。

#include "memwb_wpI_common.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <iostream>

namespace memwb_wpI_test
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
    // GuardRecorder
    // ------------------------------------------------------------

    std::function<bool(ks::ui::LeaveReason)> GuardRecorder::asFunction()
    {
        return [this](ks::ui::LeaveReason reason) -> bool
        {
            ++callCount_;
            reasons_.push_back(static_cast<int>(reason));
            return approve_;
        };
    }

    void GuardRecorder::SetApproval(bool approve)
    {
        approve_ = approve;
    }

    int GuardRecorder::CallCount() const
    {
        return callCount_;
    }

    const std::vector<int>& GuardRecorder::Reasons() const
    {
        return reasons_;
    }

    void GuardRecorder::Reset()
    {
        callCount_ = 0;
        reasons_.clear();
    }

    // ------------------------------------------------------------
    // FakeWorkbenchServices
    // ------------------------------------------------------------

    void FakeWorkbenchServices::SetProcessModulesResult(std::uint32_t pid, ks::ui::ModuleEnumResult result)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        processResults_[pid] = std::move(result);
    }

    void FakeWorkbenchServices::SetProcessEnumDelayMs(std::uint32_t pid, int delayMs)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        processDelaysMs_[pid] = delayMs;
    }

    int FakeWorkbenchServices::ProcessEnumCallCount() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return processEnumCallCount_;
    }

    std::uint64_t FakeWorkbenchServices::LastExpectCreateTime() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastExpectCreateTime_;
    }

    void FakeWorkbenchServices::SetKernelModulesResult(ks::ui::ModuleEnumResult result)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        kernelResult_ = std::move(result);
    }

    void FakeWorkbenchServices::SetKernelEnumDelayMs(int delayMs)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        kernelDelayMs_ = delayMs;
    }

    int FakeWorkbenchServices::KernelEnumCallCount() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return kernelEnumCallCount_;
    }

    void FakeWorkbenchServices::SetProcessCandidates(std::vector<ksword::memwb::ProcessCandidate> candidates)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        candidates_ = std::move(candidates);
    }

    void FakeWorkbenchServices::SetPointerReadResult(std::uint64_t address, ks::ui::PointerReadResult result)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pointerResults_[address] = std::move(result);
    }

    int FakeWorkbenchServices::ReadPointerCallCount() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return readPointerCallCount_;
    }

    void FakeWorkbenchServices::SetDdmaGeneration(std::uint64_t generation)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ddmaGeneration_ = generation;
    }

    int FakeWorkbenchServices::DdmaGenerationCallCount() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return ddmaGenerationCallCount_;
    }

    ks::ui::ModuleEnumResult FakeWorkbenchServices::enumerateProcessModules(
        std::uint32_t pid, std::uint64_t expectCreateTime)
    {
        int delayMs = 0;
        ks::ui::ModuleEnumResult result;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++processEnumCallCount_;
            lastExpectCreateTime_ = expectCreateTime; // T6 M24：记录调用方真正传了什么。
            const auto delayIt = processDelaysMs_.find(pid);
            if (delayIt != processDelaysMs_.end())
            {
                delayMs = delayIt->second;
            }
            const auto resultIt = processResults_.find(pid);
            if (resultIt != processResults_.end())
            {
                result = resultIt->second;
            }
            else
            {
                result.ok = false;
                result.failure = "FakeWorkbenchServices: 未为该 pid 配置结果";
            }
        }
        if (delayMs > 0)
        {
            QThread::msleep(static_cast<unsigned long>(delayMs));
        }
        return result;
    }

    ks::ui::ModuleEnumResult FakeWorkbenchServices::enumerateKernelModules()
    {
        int delayMs = 0;
        ks::ui::ModuleEnumResult result;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++kernelEnumCallCount_;
            delayMs = kernelDelayMs_;
            result = kernelResult_;
        }
        if (delayMs > 0)
        {
            QThread::msleep(static_cast<unsigned long>(delayMs));
        }
        return result;
    }

    std::vector<ksword::memwb::ProcessCandidate> FakeWorkbenchServices::processCandidates()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return candidates_;
    }

    ks::ui::PointerReadResult FakeWorkbenchServices::readPointer(
        const ksword::memwb::MemoryTargetSession& /*session*/,
        std::uint64_t address,
        std::uint32_t /*width*/)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++readPointerCallCount_;
        const auto it = pointerResults_.find(address);
        if (it != pointerResults_.end())
        {
            return it->second;
        }
        ks::ui::PointerReadResult result;
        result.ok = false;
        result.failure = "FakeWorkbenchServices: 未为该地址配置指针读取结果";
        return result;
    }

    std::uint64_t FakeWorkbenchServices::ddmaGeneration()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++ddmaGenerationCallCount_;
        return ddmaGeneration_;
    }

    void AttachFakeProcess(
        ks::ui::WorkbenchTarget& target, std::uint32_t fakePid, std::uint64_t attachGeneration)
    {
        ks::ui::WorkbenchTarget::DockAttach attach;
        attach.handle = reinterpret_cast<void*>(::GetCurrentProcess()); // 真实有效的伪句柄，只用于锚定。
        attach.pid = fakePid; // 故意不要求这个 pid 真实存在，见头文件处的说明。
        attach.attachGeneration = attachGeneration;
        target.onDockAttached(attach);
    }
}
