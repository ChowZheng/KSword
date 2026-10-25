#include "MemoryDock.Internal.h"
#include "../UI/AsyncOperation.h"
#include "../Internationalization/LanguageManager.h"

#include <QSaveFile>

#include <algorithm>
#include <limits>

using namespace ksword::memory_dock_internal;

// 后台仅返回内部原因码和接口详情；用户提示模板统一在 UI 线程翻译。
bool MemoryDock::dumpMemoryRegionToFile(
    const std::uint32_t pid,
    const std::shared_ptr<void>& targetProcess,
    const std::uint64_t creationTime,
    const RegionEntry& region,
    const ksword::memory_backend::MemoryAccessBackend backend,
    const QString& dumpFilePath,
    const int progressPid,
    const std::function<bool()>& canceled,
    QString& errorTextOut,
    const std::function<bool()>& beginCommit,
    const std::function<void(std::uint64_t, std::uint64_t)>& progress,
    const bool moduleImage)
{
    errorTextOut.clear();
    using namespace ksword::memory_backend;
    if (backend != MemoryAccessBackend::UserMode && backend != MemoryAccessBackend::StandardDriver)
    {
        errorTextOut = QStringLiteral("backend_unsupported");
        return false;
    }

    // 范围来自菜单前快照；拒绝空区域、未提交区域、地址溢出及文件长度溢出。
    if (pid == 0 || creationTime == 0 || !targetProcess || region.regionSize == 0
        || region.state != MEM_COMMIT
        || region.regionSize > std::numeric_limits<std::uint64_t>::max() - region.baseAddress
        || region.regionSize > static_cast<std::uint64_t>(std::numeric_limits<qint64>::max()))
    {
        errorTextOut = QStringLiteral("invalid_range");
        return false;
    }
    const HANDLE processHandle = targetProcess.get(); // 独立租约不随 UI 分离操作关闭/复用。
    const auto checkTarget = [&]()
    {
        if (canceled && canceled())
        {
            errorTextOut = QStringLiteral("canceled");
            return false;
        }
        FILETIME created{}, exited{}, kernel{}, user{}; // 每个边界复核创建时间与存活状态。
        if (::GetProcessId(processHandle) != toDwordPid(pid)
            || ::WaitForSingleObject(processHandle, 0) != WAIT_TIMEOUT
            || !::GetProcessTimes(processHandle, &created, &exited, &kernel, &user)
            || ((static_cast<std::uint64_t>(created.dwHighDateTime) << 32U) | created.dwLowDateTime) != creationTime)
        {
            errorTextOut = QStringLiteral("identity_changed");
            return false;
        }
        return true;
    };
    // 模块可跨多个 VQE 区段；冻结完整区段的真实边界，不能把整个映像伪装成一个区域。
    std::vector<MEMORY_BASIC_INFORMATION> moduleRegions; // 仅模块请求保存各区段元数据。
    const auto checkRegion = [&](const std::size_t moduleSegment = std::numeric_limits<std::size_t>::max())
    {
        if (moduleImage)
        {
            const bool allSegments = moduleSegment == std::numeric_limits<std::size_t>::max();
            const std::size_t first = allSegments ? 0U : moduleSegment;
            const std::size_t end = allSegments ? moduleRegions.size() : moduleSegment + 1U;
            if (first >= moduleRegions.size())
            {
                errorTextOut = QStringLiteral("region_changed");
                return false;
            }
            for (std::size_t index = first; index < end; ++index)
            {
                const MEMORY_BASIC_INFORMATION& expected = moduleRegions[index]; // 单次只复核正在读取区段。
                MEMORY_BASIC_INFORMATION current{}; // 逐段复核映像身份、边界、提交和保护状态。
                if (::VirtualQueryEx(processHandle, expected.BaseAddress, &current, sizeof(current)) != sizeof(current)
                    || current.BaseAddress != expected.BaseAddress || current.AllocationBase != expected.AllocationBase
                    || current.AllocationProtect != expected.AllocationProtect || current.RegionSize != expected.RegionSize
                    || current.State != expected.State || current.Protect != expected.Protect || current.Type != expected.Type)
                {
                    errorTextOut = QStringLiteral("region_changed");
                    return false;
                }
            }
            return !moduleRegions.empty();
        }
        MEMORY_BASIC_INFORMATION current{}; // 重新查询该地址，拒绝已释放/拆分/合并/改保护的区域。
        if (::VirtualQueryEx(processHandle, reinterpret_cast<LPCVOID>(region.baseAddress),
                &current, sizeof(current)) != sizeof(current)
            || reinterpret_cast<std::uintptr_t>(current.BaseAddress) != region.baseAddress
            || current.RegionSize != region.regionSize || current.Protect != region.protect
            || current.State != region.state || current.Type != region.type)
        {
            errorTextOut = QStringLiteral("region_changed");
            return false;
        }
        return true;
    };
    if (moduleImage)
    {
        constexpr std::uint64_t maximumModuleBytes = 512ULL * 1024ULL * 1024ULL; // 有界模块快照。
        constexpr std::size_t maximumModuleRegions = 65536U; // 拒绝异常碎片映像导致的无界元数据。
        if (region.regionSize > maximumModuleBytes || !checkTarget())
        {
            if (errorTextOut.isEmpty())
            {
                errorTextOut = QStringLiteral("invalid_range");
            }
            return false;
        }
        const std::uint64_t endAddress = region.baseAddress + region.regionSize; // 已检查溢出。
        std::uint64_t address = region.baseAddress; // 沿真实 VQE 边界遍历整个映像。
        while (address < endAddress)
        {
            MEMORY_BASIC_INFORMATION current{}; // 快照必须全部来自同一映像分配。
            if (!checkTarget())
            {
                return false;
            }
            if (::VirtualQueryEx(processHandle, reinterpret_cast<LPCVOID>(address), &current, sizeof(current)) != sizeof(current)
                || reinterpret_cast<std::uintptr_t>(current.BaseAddress) != address
                || reinterpret_cast<std::uintptr_t>(current.AllocationBase) != region.baseAddress
                || current.State != MEM_COMMIT || current.Type != MEM_IMAGE || current.RegionSize == 0
                || current.RegionSize > std::numeric_limits<std::uint64_t>::max() - address
                || current.RegionSize > endAddress - address || moduleRegions.size() >= maximumModuleRegions)
            {
                errorTextOut = QStringLiteral("region_changed");
                return false;
            }
            moduleRegions.push_back(current);
            address += current.RegionSize;
        }
        // 选中范围必须覆盖完整映像分配，拒绝在区段边界提前结束的过期模块大小。
        MEMORY_BASIC_INFORMATION following{};
        if (::VirtualQueryEx(processHandle, reinterpret_cast<LPCVOID>(endAddress), &following, sizeof(following)) != sizeof(following)
            || reinterpret_cast<std::uintptr_t>(following.AllocationBase) == region.baseAddress)
        {
            errorTextOut = QStringLiteral("region_changed");
            return false;
        }
    }
    if (!checkTarget() || !checkRegion())
    {
        return false;
    }

    // 禁止直接写回退；任何失败或取消都只丢弃临时数据，不留下半个目标文件。
    QSaveFile outputFile(dumpFilePath);
    outputFile.setDirectWriteFallback(false);
    if (!outputFile.open(QIODevice::WriteOnly))
    {
        errorTextOut = QStringLiteral("open_failed\n") + outputFile.errorString();
        return false;
    }
    const DdmaSession unusedSession{}; // 本任务只允许 R3/R0，不读取或切换 DDMA/HVM 配置。
    std::uint64_t offset = 0; // 仅保留一个分块的字节缓冲，不分配整个区域大小。
    std::size_t moduleSegment = 0U; // 读取顺序固定，区段游标只向前推进，避免逐块遍历全部映像。
    while (offset < region.regionSize)
    {
        const std::uint64_t address = region.baseAddress + offset; // 本次块的确切起点。
        if (moduleImage)
        {
            while (moduleSegment < moduleRegions.size()
                && address - reinterpret_cast<std::uintptr_t>(moduleRegions[moduleSegment].BaseAddress)
                    >= moduleRegions[moduleSegment].RegionSize)
            {
                ++moduleSegment;
            }
        }
        if (!checkTarget() || !checkRegion(moduleSegment))
        {
            return false;
        }
        std::uint64_t length = std::min<std::uint64_t>( // 沿用共享协议的单次读取上限。
            region.regionSize - offset, KSWORD_ARK_MEMORY_READ_MAX_BYTES);
        if (moduleImage)
        {
            const MEMORY_BASIC_INFORMATION& segment = moduleRegions[moduleSegment]; // 分块不跨保护边界。
            length = std::min<std::uint64_t>(length,
                reinterpret_cast<std::uintptr_t>(segment.BaseAddress) + segment.RegionSize - address);
        }
        const AccessOutcome outcome = readVirtual(backend, unusedSession, pid,
            region.baseAddress + offset, length);
        if (!checkTarget() || !checkRegion(moduleSegment))
        {
            return false;
        }
        // PARTIAL_COPY、补零状态、短响应或长度不一致均不能当作有效 Dump 保存。
        if (!outcome.ok || outcome.partial || outcome.bytesDone != length
            || static_cast<std::uint64_t>(outcome.data.size()) != length)
        {
            errorTextOut = QStringLiteral("read_failed\n0x%1: %2/%3\n%4")
                .arg(region.baseAddress + offset, 0, 16).arg(outcome.bytesDone).arg(length).arg(outcome.failureText);
            return false;
        }
        if (outputFile.write(outcome.data.constData(), outcome.data.size()) != outcome.data.size())
        {
            errorTextOut = QStringLiteral("write_failed\n") + outputFile.errorString();
            return false;
        }
        offset += length;
        // 生产回调携带进度轮次；比例始终低于 1，完成必须由文件提交后的真实终态决定。
        if (progress)
        {
            progress(offset, region.regionSize);
        }
        else
        {
            kPro.set(progressPid, "正在导出内存区域", 0,
                static_cast<float>(static_cast<double>(offset) / static_cast<double>(region.regionSize) * 0.99));
        }
    }
    if (!checkTarget() || !checkRegion())
    {
        return false;
    }
    // 取消与进入提交由同一原子状态仲裁；进入提交后不能再承诺“取消保留旧文件”。
    if (beginCommit && !beginCommit())
    {
        errorTextOut = QStringLiteral("canceled");
        return false;
    }
    if (!outputFile.commit())
    {
        errorTextOut = QStringLiteral("commit_failed\n") + outputFile.errorString();
        return false;
    }
    return true;
}

bool MemoryDock::tryCancelMemoryRegionDump()
{
    if (!m_regionDumpPhase || m_regionDumpOperation == nullptr)
    {
        return false;
    }
    RegionDumpPhase expected = RegionDumpPhase::Reading; // 仅读取阶段能接受取消。
    if (m_regionDumpPhase->compare_exchange_strong(expected, RegionDumpPhase::CancelRequested))
    {
        m_regionDumpOperation->cancel();
        return true;
    }
    return expected == RegionDumpPhase::CancelRequested;
}

// DLL 菜单入口：固定实例与模块范围，复用单区导出的后台任务和文件生命周期。
void MemoryDock::requestDumpProcessModule(
    const std::uint32_t pid,
    const std::uint64_t creationTime,
    const std::uint64_t moduleBase,
    const std::uint64_t moduleSize,
    const QString& moduleName,
    const ksword::memory_backend::MemoryAccessBackend backend)
{
    const QString title = ks::i18n::sourceText(QStringLiteral("导出模块内存（原始布局）"));
    if (pid == 0U || creationTime == 0U || moduleBase == 0U || moduleSize == 0U
        || moduleSize > 512ULL * 1024ULL * 1024ULL
        || moduleSize > std::numeric_limits<std::uint64_t>::max() - moduleBase
        || (backend != ksword::memory_backend::MemoryAccessBackend::UserMode
            && backend != ksword::memory_backend::MemoryAccessBackend::StandardDriver))
    {
        QMessageBox::warning(this, title, ks::i18n::sourceText(
            QStringLiteral("模块范围无效或超过 512 MiB，无法导出。")));
        return;
    }
    const DWORD access = PROCESS_QUERY_INFORMATION | SYNCHRONIZE
        | (backend == ksword::memory_backend::MemoryAccessBackend::UserMode ? PROCESS_VM_READ : 0U);
    const HANDLE handle = ::OpenProcess(access, FALSE, pid); // 独立租约保留到后台物理完成。
    if (handle == nullptr)
    {
        const DWORD error = ::GetLastError(); // 保留原生错误，不能把打开失败当作空模块。
        QMessageBox::warning(this, title, ks::i18n::sourceText(
            QStringLiteral("无法打开目标进程（Win32=%1）。")).arg(error));
        return;
    }
    const std::shared_ptr<void> target(handle, [](void* value) { ::CloseHandle(value); });
    FILETIME created{}, exited{}, kernel{}, user{}; // 在保存框前拒绝 PID 复用或已退出对象。
    if (::WaitForSingleObject(handle, 0) != WAIT_TIMEOUT
        || !::GetProcessTimes(handle, &created, &exited, &kernel, &user)
        || ((static_cast<std::uint64_t>(created.dwHighDateTime) << 32U) | created.dwLowDateTime) != creationTime)
    {
        QMessageBox::warning(this, title, ks::i18n::sourceText(
            QStringLiteral("目标进程已退出或身份已改变，导出已取消。")));
        return;
    }
    RegionEntry range{}; // 共享请求仅表达导出范围；worker 的模块模式负责真实区段快照。
    range.baseAddress = moduleBase;
    range.regionSize = moduleSize;
    range.state = MEM_COMMIT;
    range.type = MEM_IMAGE;
    QString safeName = QFileInfo(moduleName).fileName(); // 模块名只参与文件名，不成为输出目录。
    for (QChar& character : safeName)
    {
        if (character.unicode() < 32U || QStringLiteral("<>:\"/\\|?*").contains(character))
        {
            character = QLatin1Char('_');
        }
    }
    requestDumpMemoryRegion(pid, safeName, target, creationTime, range, backend, true);
}

// 菜单入口只负责固定范围、选路径和提交共享异步任务，真正读取在上述静态函数中执行。
void MemoryDock::requestDumpMemoryRegion(
    const std::uint32_t pid,
    const QString& processName,
    const std::shared_ptr<void>& targetProcess,
    const std::uint64_t creationTime,
    const RegionEntry& region,
    const ksword::memory_backend::MemoryAccessBackend backend,
    const bool moduleImage)
{
    const QString title = moduleImage
        ? ks::i18n::sourceText(QStringLiteral("导出模块内存（原始布局）"))
        : ks::i18n::sourceText(QStringLiteral("导出内存区域")); // 本次语言快照。
    if (m_regionDumpOperation != nullptr
        && (m_regionDumpOperation->isInFlight() || m_regionDumpOperation->hasPending()))
    {
        QMessageBox::information(this, title,
            ks::i18n::sourceText(QStringLiteral("已有区域导出正在进行，请等待完成或取消。")));
        return;
    }
    const auto targetStillSelected = [&]()
    {
        FILETIME created{}, exited{}, kernel{}, user{}; // 模块转储使用独立租约，不依赖 Dock 附加状态。
        if (moduleImage)
        {
            return targetProcess && ::GetProcessId(targetProcess.get()) == pid
                && ::WaitForSingleObject(targetProcess.get(), 0) == WAIT_TIMEOUT
                && ::GetProcessTimes(targetProcess.get(), &created, &exited, &kernel, &user)
                && ((static_cast<std::uint64_t>(created.dwHighDateTime) << 32U) | created.dwLowDateTime) == creationTime;
        }
        return targetProcess && m_attachedPid == pid && m_attachedProcessHandle != nullptr
            && ::GetProcessId(m_attachedProcessHandle) == pid
            && ::WaitForSingleObject(targetProcess.get(), 0) == WAIT_TIMEOUT
            && ::GetProcessTimes(m_attachedProcessHandle, &created, &exited, &kernel, &user)
            && ((static_cast<std::uint64_t>(created.dwHighDateTime) << 32U) | created.dwLowDateTime) == creationTime;
    };
    if (!targetStillSelected())
    {
        QMessageBox::warning(this, title,
            ks::i18n::sourceText(QStringLiteral("目标进程已改变，请重新选择区域。")));
        return;
    }

    // 后端由点击的菜单动作决定，不再次让用户选择、不回退到其它通道。
    const QString backendName = ks::i18n::sourceText(ksword::memory_backend::backendDisplayName(backend));
    const QString defaultName = QStringLiteral("%1_pid%2_0x%3_%4_%5.bin")
        .arg(processName.isEmpty() ? QStringLiteral("process") : processName).arg(pid)
        .arg(region.baseAddress, 0, 16).arg(backend == ksword::memory_backend::MemoryAccessBackend::UserMode
            ? QStringLiteral("R3") : QStringLiteral("R0"))
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    QPointer<MemoryDock> guardedOwner(this); // 保存框可运行嵌套事件循环并销毁宿主。
    const QString outputPath = QFileDialog::getSaveFileName(this, title, defaultName,
        ks::i18n::sourceText(QStringLiteral("原始内存转储 (*.bin);;所有文件 (*.*)")));
    if (guardedOwner.isNull() || outputPath.trimmed().isEmpty())
    {
        return;
    }
    if (!targetStillSelected())
    {
        QMessageBox::warning(this, title,
            ks::i18n::sourceText(QStringLiteral("目标进程已改变，请重新选择区域。")));
        return;
    }
    if (m_regionDumpOperation == nullptr)
    {
        m_regionDumpOperation = new ks::ui::AsyncOperation(this);
    }
    if (m_regionDumpOperation->isInFlight() || m_regionDumpOperation->hasPending())
    {
        return; // 防止保存框期间重入，两个导出不能共用进度或替换同一目标文件。
    }
    if (m_regionDumpProgressPid == 0)
    {
        m_regionDumpProgressPid = kPro.addReusable(this, "内存区域导出", "Dump内存区域");
    }
    const int progressPid = m_regionDumpProgressPid; // 后台与收尾只持值编号。
    const std::uint64_t progressGeneration = kPro.beginCycle(progressPid, "准备读取内存区域");
    m_regionDumpPhase = std::make_shared<std::atomic<RegionDumpPhase>>(RegionDumpPhase::Reading);
    const auto phase = m_regionDumpPhase; // UI 和 worker 仅共享原子值状态。

    // 错误标题和模板在 UI 线程预先翻译，后台只产生稳定原因码和外部接口详情。
    const QHash<QString, QString> errorTexts{
        {QStringLiteral("backend_unsupported"), ks::i18n::sourceText(QStringLiteral("区域导出仅支持 R3 或 R0。"))},
        {QStringLiteral("invalid_range"), ks::i18n::sourceText(QStringLiteral("区域范围无效或尚未提交，无法导出。"))},
        {QStringLiteral("identity_changed"), ks::i18n::sourceText(QStringLiteral("目标进程已退出或身份已改变，导出已取消。"))},
        {QStringLiteral("region_changed"), ks::i18n::sourceText(QStringLiteral("内存区域已变化或无法查询，导出已取消。"))},
        {QStringLiteral("canceled"), ks::i18n::sourceText(QStringLiteral("区域导出已取消。"))},
        {QStringLiteral("open_failed"), ks::i18n::sourceText(QStringLiteral("无法创建区域转储文件。"))},
        {QStringLiteral("read_failed"), ks::i18n::sourceText(QStringLiteral("内存读取未完成，区域转储未保存。"))},
        {QStringLiteral("write_failed"), ks::i18n::sourceText(QStringLiteral("区域转储文件写入失败。"))},
        {QStringLiteral("commit_failed"), ks::i18n::sourceText(QStringLiteral("区域转储文件提交失败。"))}
    };
    const QString successTitle = moduleImage
        ? ks::i18n::sourceText(QStringLiteral("模块内存导出完成"))
        : ks::i18n::sourceText(QStringLiteral("区域导出完成"));
    const QString failureTitle = moduleImage
        ? ks::i18n::sourceText(QStringLiteral("模块内存导出失败"))
        : ks::i18n::sourceText(QStringLiteral("区域导出失败"));
    const QString successText = moduleImage
        ? ks::i18n::sourceText(QStringLiteral("PID %1，模块 0x%2（%3 字节）已通过 %4 保存为原始内存布局：\n%5\n未进行 PE 文件重建。"))
        : ks::i18n::sourceText(QStringLiteral("PID %1，区域 0x%2（%3 字节）已通过 %4 保存到：\n%5"));
    const QString failureText = ks::i18n::sourceText(QStringLiteral("PID %1，区域 0x%2（%3 字节），后端 %4。\n%5"));
    const QString exceptionText = ks::i18n::sourceText(QStringLiteral("区域导出发生异常，未完成保存。"));

    struct Request final // 后台请求不持 UI 指针，不借用可被分离流程关闭的原句柄。
    {
        std::uint32_t pid; // 请求所属 PID。
        std::shared_ptr<void> target; // 独立进程对象租约。
        std::uint64_t created; // 防 PID 复用的创建时间。
        RegionEntry region; // 菜单前冻结的区域元数据。
        ksword::memory_backend::MemoryAccessBackend backend; // 用户明确选定的 R3/R0。
        QString path; // 保存框确认的目标文件。
        int progress; // 本次有限任务的进度编号。
        std::uint64_t progressGeneration; // 拒绝过期任务更新当前进度。
        std::shared_ptr<std::atomic<RegionDumpPhase>> phase; // 提交/取消仲裁。
        std::string readStep; // UI 线程预译的步骤文本。
        bool moduleImage; // true 时冻结/复核映像全部区段，false 保留单区导出语义。
    };
    struct Result final // 后台终态数据由共享控制器投递到所属 UI 线程。
    {
        bool ok = false; // 只有完成原子提交后才为真。
        QString error; // 内部原因码与外部接口详情。
    };
    const Request request{pid, targetProcess, creationTime, region, backend, outputPath, progressPid,
        progressGeneration, phase, ks::i18n::sourceText(QStringLiteral("正在导出内存区域")).toStdString(), moduleImage};
    m_regionDumpOperation->submit<Request, Result>(request,
        [](const Request& job, const ks::ui::AsyncOperationToken& token)
        {
            Result result;
            try
            {
                result.ok = MemoryDock::dumpMemoryRegionToFile(job.pid, job.target, job.created,
                    job.region, job.backend, job.path, job.progress,
                    [token]() { return token.isCanceled(); }, result.error,
                    [phase = job.phase]()
                    {
                        RegionDumpPhase expected = RegionDumpPhase::Reading;
                        return phase->compare_exchange_strong(expected, RegionDumpPhase::Committing);
                    },
                    [progressPid = job.progress, generation = job.progressGeneration, step = job.readStep]
                    (const std::uint64_t done, const std::uint64_t total)
                    {
                        kPro.set(progressPid, generation, step, 0,
                            static_cast<float>(static_cast<double>(done) / static_cast<double>(total) * 0.99));
                    }, job.moduleImage);
            }
            catch (...)
            {
                job.phase->store(RegionDumpPhase::Failed);
                kPro.finish(job.progress, job.progressGeneration, kProgressState::Failure, "区域导出失败");
                throw;
            }
            // 即使宿主已关闭，提交后的真实终态仍由持值 worker 收尾，不把已保存结果记为取消。
            const bool wasCanceled = result.error == QStringLiteral("canceled");
            job.phase->store(result.ok ? RegionDumpPhase::Committed
                : (wasCanceled ? RegionDumpPhase::CancelRequested : RegionDumpPhase::Failed));
            kPro.finish(job.progress, job.progressGeneration,
                result.ok ? kProgressState::Success : (wasCanceled ? kProgressState::Canceled : kProgressState::Failure),
                result.ok ? "区域导出完成" : (wasCanceled ? "区域导出已取消" : "区域导出失败"));
            return result;
        },
        [this, request, backendName, errorTexts, successTitle, failureTitle, successText, failureText]
        (const Result& result, std::uint64_t)
        {
            if (result.ok)
            {
                QMessageBox::information(this, successTitle, successText.arg(request.pid)
                    .arg(request.region.baseAddress, 0, 16).arg(request.region.regionSize)
                    .arg(backendName, request.path));
            }
            else
            {
                const QString code = result.error.section(QLatin1Char('\n'), 0, 0); // 内部原因码。
                const QString detail = result.error.section(QLatin1Char('\n'), 1); // 接口原始错误详情。
                QString explanation = errorTexts.value(code, failureTitle);
                if (!detail.isEmpty())
                {
                    explanation += QLatin1Char('\n') + detail;
                }
                QMessageBox::warning(this, failureTitle, failureText.arg(request.pid)
                    .arg(request.region.baseAddress, 0, 16).arg(request.region.regionSize)
                    .arg(backendName, explanation));
            }
            return true;
        },
        [this, progressPid, progressGeneration, failureTitle, exceptionText](std::exception_ptr)
        {
            kPro.finish(progressPid, progressGeneration, kProgressState::Failure, "区域导出失败");
            QMessageBox::warning(this, failureTitle, exceptionText);
        },
        [progressPid, progressGeneration, phase](const ks::ui::AsyncOperationOutcome outcome)
        {
            // 被取消/关窗/丢队列的请求也结束进度，收尾不访问任何页面 QObject。
            if (outcome == ks::ui::AsyncOperationOutcome::Canceled
                || outcome == ks::ui::AsyncOperationOutcome::OwnerClosed
                || outcome == ks::ui::AsyncOperationOutcome::DeliveryDropped
                || outcome == ks::ui::AsyncOperationOutcome::Superseded)
            {
                RegionDumpPhase expected = RegionDumpPhase::Reading;
                phase->compare_exchange_strong(expected, RegionDumpPhase::CancelRequested);
                if (phase->load() == RegionDumpPhase::CancelRequested)
                {
                    kPro.finish(progressPid, progressGeneration, kProgressState::Canceled, "区域导出已取消");
                }
            }
        });
}
