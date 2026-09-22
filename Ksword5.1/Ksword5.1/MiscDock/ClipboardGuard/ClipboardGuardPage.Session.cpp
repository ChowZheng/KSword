#include "ClipboardGuardPage.h"
#include "../../theme.h"
#include "../../MonitorDock/WinApiMonitorProtocol.h"
#include "../../Framework/PrivilegeElevationPrompt.h"

#include <QApplication>
#include <QBrush>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextStream>
#include <QThreadPool>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>

// ============================================================
// ClipboardGuardPage.Session.cpp
// 作用：
// 1) 规则表与驱动的双向同步（KSWORD_ARK_CLIPBOARD_POLICY_RULE 整表下发/回读）；
// 2) 按规则匹配当前运行进程，注入 APIMonitor_x64.dll 并写会话 INI；
// 3) 每个受管进程一条命名管道读取线程，事件解析后送进共享队列，
//    由 QTimer 批量刷进事件表（照抄 WinAPIDock 的 child-pipe 范式）。
// ============================================================

namespace ks::misc
{
    namespace
    {
        constexpr DWORD kPacketSize = static_cast<DWORD>(sizeof(ks::winapi_monitor::ApiMonitorEventPacket));

        template <std::size_t kCount>
        QString wideBufferToText(const wchar_t(&bufferValue)[kCount])
        {
            QString textValue = QString::fromWCharArray(bufferValue, static_cast<int>(kCount));
            const int nullIndex = textValue.indexOf(QChar(u'\0'));
            if (nullIndex >= 0)
            {
                textValue.truncate(nullIndex);
            }
            return textValue.trimmed();
        }

        // parseDetailField 作用：从 "key1=value1 key2=value2 ..." 形式的 detail 文本里
        // 取出某个 key 的值。ReportClipboardEvent（APIMonitor_x64 侧）用同样的分隔约定拼串，
        // 双方约定好格式即可，不需要一套共享的序列化库。
        QString parseDetailField(const QString& detailText, const QString& keyName)
        {
            const QString markerText = keyName + QStringLiteral("=");
            const int markerIndex = detailText.indexOf(markerText);
            if (markerIndex < 0)
            {
                return QString();
            }
            const int valueStart = markerIndex + markerText.size();
            int valueEnd = detailText.indexOf(QChar(u' '), valueStart);
            if (valueEnd < 0)
            {
                valueEnd = detailText.size();
            }
            return detailText.mid(valueStart, valueEnd - valueStart);
        }

        // packetToEventRow 作用：把 Agent 固定事件包解析成表格展示用的结构。
        ClipboardGuardEventRow packetToEventRow(const ks::winapi_monitor::ApiMonitorEventPacket& packetValue, const QString& processText)
        {
            ClipboardGuardEventRow rowValue;
            rowValue.timeText = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
            rowValue.pid = packetValue.pid;
            rowValue.tid = packetValue.tid;
            rowValue.processText = processText;

            const QString detailText = wideBufferToText(packetValue.detailText);
            rowValue.operationText = parseDetailField(detailText, QStringLiteral("op"));
            rowValue.formatText = parseDetailField(detailText, QStringLiteral("fmt"));
            rowValue.resultText = parseDetailField(detailText, QStringLiteral("action"));
            rowValue.sessionText = parseDetailField(detailText, QStringLiteral("session"));
            rowValue.integrityText = parseDetailField(detailText, QStringLiteral("integrity"));
            rowValue.ownerText = parseDetailField(detailText, QStringLiteral("owner"));
            rowValue.seq = parseDetailField(detailText, QStringLiteral("seq")).toULongLong();
            return rowValue;
        }
    }

    void ClipboardGuardPage::refreshRuleTable()
    {
        if (m_ruleTable == nullptr)
        {
            return;
        }
        m_ruleTable->setRowCount(static_cast<int>(m_rules.size()));
        for (std::size_t rowIndex = 0; rowIndex < m_rules.size(); ++rowIndex)
        {
            const ClipboardGuardRule& ruleValue = m_rules[rowIndex];
            const int row = static_cast<int>(rowIndex);
            QString targetText;
            QString kindText;
            switch (ruleValue.targetKind)
            {
            case 1: targetText = QString::number(ruleValue.targetProcessId); kindText = QStringLiteral("PID"); break;
            case 2: targetText = ruleValue.targetImage; kindText = QStringLiteral("映像名"); break;
            case 3: targetText = ruleValue.targetImage; kindText = QStringLiteral("完整路径"); break;
            default: kindText = QStringLiteral("未设置"); break;
            }
            const auto actionText = [](const quint32 actionValue) {
                switch (actionValue)
                {
                case 1: return QStringLiteral("拦截");
                case 2: return QStringLiteral("仅记录");
                default: return QStringLiteral("允许");
                }
            };
            m_ruleTable->setItem(row, 0, createReadOnlyItem(targetText));
            m_ruleTable->setItem(row, 1, createReadOnlyItem(kindText));
            m_ruleTable->setItem(row, 2, createReadOnlyItem(ruleValue.ruleName));
            m_ruleTable->setItem(row, 3, createReadOnlyItem(actionText(ruleValue.readAction)));
            m_ruleTable->setItem(row, 4, createReadOnlyItem(actionText(ruleValue.writeAction)));
            m_ruleTable->setItem(row, 5, createReadOnlyItem(actionText(ruleValue.enumAction)));
            m_ruleTable->setItem(row, 6, createReadOnlyItem(ruleValue.enabled ? QStringLiteral("是") : QStringLiteral("否")));
        }
    }

    void ClipboardGuardPage::syncRulesToDriver()
    {
        std::vector<KSWORD_ARK_CLIPBOARD_POLICY_RULE> driverRules;
        driverRules.reserve(m_rules.size());
        for (const ClipboardGuardRule& ruleValue : m_rules)
        {
            KSWORD_ARK_CLIPBOARD_POLICY_RULE driverRule{};
            driverRule.ruleId = ruleValue.ruleId;
            driverRule.flags = ruleValue.enabled ? KSWORD_ARK_CLIPBOARD_POLICY_RULE_FLAG_ENABLED : 0UL;
            driverRule.targetKind = ruleValue.targetKind;
            driverRule.targetProcessId = ruleValue.targetProcessId;
            driverRule.readAction = ruleValue.readAction;
            driverRule.writeAction = ruleValue.writeAction;
            driverRule.enumAction = ruleValue.enumAction;
            const std::wstring targetImageText = ruleValue.targetImage.toStdWString();
            wcsncpy_s(driverRule.targetImage, targetImageText.c_str(), KSWORD_ARK_CLIPBOARD_POLICY_IMAGE_CHARS - 1U);
            const std::wstring ruleNameText = ruleValue.ruleName.toStdWString();
            wcsncpy_s(driverRule.ruleName, ruleNameText.c_str(), KSWORD_ARK_CLIPBOARD_POLICY_NAME_CHARS - 1U);
            driverRules.push_back(driverRule);
        }

        const ksword::ark::IoResult result = m_driverClient.setClipboardPolicy(
            KSWORD_ARK_CLIPBOARD_POLICY_FLAG_ENABLED, driverRules);
        if (!result.ok && m_statusLabel != nullptr)
        {
            m_statusLabel->setText(QStringLiteral("规则下发驱动失败：%1").arg(QString::fromStdString(result.message)));
        }
    }

    void ClipboardGuardPage::loadRulesFromDriver()
    {
        const ksword::ark::ClipboardPolicyStateResult result = m_driverClient.queryClipboardPolicyState();
        if (!result.io.ok)
        {
            if (m_statusLabel != nullptr)
            {
                m_statusLabel->setText(QStringLiteral("驱动策略回读失败：%1（可能驱动未加载，规则仍可在本页新增，下发时会再次报错）")
                    .arg(QString::fromStdString(result.io.message)));
            }
            return;
        }

        m_rules.clear();
        quint32 maxRuleId = 0;
        for (unsigned long ruleIndex = 0; ruleIndex < result.response.ruleCount; ++ruleIndex)
        {
            const KSWORD_ARK_CLIPBOARD_POLICY_RULE& driverRule = result.response.rules[ruleIndex];
            ClipboardGuardRule ruleValue;
            ruleValue.ruleId = driverRule.ruleId;
            ruleValue.enabled = (driverRule.flags & KSWORD_ARK_CLIPBOARD_POLICY_RULE_FLAG_ENABLED) != 0UL;
            ruleValue.targetKind = driverRule.targetKind;
            ruleValue.targetProcessId = driverRule.targetProcessId;
            ruleValue.targetImage = QString::fromWCharArray(driverRule.targetImage);
            ruleValue.ruleName = QString::fromWCharArray(driverRule.ruleName);
            ruleValue.readAction = driverRule.readAction;
            ruleValue.writeAction = driverRule.writeAction;
            ruleValue.enumAction = driverRule.enumAction;
            maxRuleId = std::max(maxRuleId, ruleValue.ruleId);
            m_rules.push_back(ruleValue);
        }
        m_nextLocalRuleId = maxRuleId + 1U;
        refreshRuleTable();
        if (m_statusLabel != nullptr)
        {
            m_statusLabel->setText(QStringLiteral("已从驱动加载 %1 条规则。").arg(m_rules.size()));
        }
    }

    const ClipboardGuardRule* ClipboardGuardPage::findMatchingRule(const ks::process::ProcessRecord& processRecord) const
    {
        const QString imagePath = QString::fromStdString(processRecord.imagePath);
        const QString imageName = QFileInfo(imagePath).fileName();
        for (const ClipboardGuardRule& ruleValue : m_rules)
        {
            if (!ruleValue.enabled)
            {
                continue;
            }
            switch (ruleValue.targetKind)
            {
            case 1: // PID
                if (ruleValue.targetProcessId == processRecord.pid)
                {
                    return &ruleValue;
                }
                break;
            case 2: // IMAGE_NAME
                if (!imageName.isEmpty() && imageName.compare(ruleValue.targetImage, Qt::CaseInsensitive) == 0)
                {
                    return &ruleValue;
                }
                break;
            case 3: // IMAGE_PATH：允许规则写完整路径或去盘符的路径尾部，大小写不敏感后缀匹配。
                if (!imagePath.isEmpty() && !ruleValue.targetImage.isEmpty()
                    && imagePath.endsWith(ruleValue.targetImage, Qt::CaseInsensitive))
                {
                    return &ruleValue;
                }
                break;
            default:
                break;
            }
        }
        return nullptr;
    }

    bool ClipboardGuardPage::writeSessionConfigForPid(
        const std::uint32_t pid, const ClipboardGuardRule& matchedRule, QString* const errorTextOut) const
    {
        const QString configPath = QString::fromStdWString(ks::winapi_monitor::buildConfigPathForPid(pid));
        QDir().mkpath(QFileInfo(configPath).absolutePath());

        QSaveFile configFile(configPath);
        if (!configFile.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("无法写入会话配置：%1").arg(configPath);
            }
            return false;
        }

        const auto actionKeyword = [](const quint32 actionValue) {
            switch (actionValue)
            {
            case 1: return QStringLiteral("block");
            case 2: return QStringLiteral("log_only");
            default: return QStringLiteral("allow");
            }
        };

        QTextStream outputStream(&configFile);
        outputStream << "[monitor]\n";
        outputStream << "pipe_name=" << QString::fromStdWString(ks::winapi_monitor::buildPipeNameForPid(pid)) << '\n';
        outputStream << "stop_flag_path=" << QString::fromStdWString(ks::winapi_monitor::buildStopFlagPathForPid(pid)) << '\n';
        outputStream << "session_id=clipboard_guard_" << pid << "_" << QDateTime::currentMSecsSinceEpoch() << '\n';
        outputStream << "agent_dll_path=" << QDir::toNativeSeparators(QCoreApplication::applicationDirPath() + QStringLiteral("/APIMonitor_x64.dll")) << '\n';
        // 只开剪贴板分类，不随手把通用文件/注册表/网络/进程/加载器监控一起打开——
        // "剪贴板保护"和通用 API 监控是两个不同的使用场景，这里只要剪贴板这一份。
        outputStream << "enable_file=0\n";
        outputStream << "enable_registry=0\n";
        outputStream << "enable_network=0\n";
        outputStream << "enable_process=0\n";
        outputStream << "enable_loader=0\n";
        outputStream << "enable_raw_fallback=0\n";
        outputStream << "fake_success_enabled=0\n";
        outputStream << "enable_clipboard=1\n";
        outputStream << "clipboard_read_action=" << actionKeyword(matchedRule.readAction) << '\n';
        outputStream << "clipboard_write_action=" << actionKeyword(matchedRule.writeAction) << '\n';
        outputStream << "clipboard_enum_action=" << actionKeyword(matchedRule.enumAction) << '\n';

        if (!configFile.commit())
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("会话配置提交失败：%1").arg(configPath);
            }
            return false;
        }
        return true;
    }

    void ClipboardGuardPage::ensureProcessProtected(const ks::process::ProcessRecord& processRecord, const ClipboardGuardRule& matchedRule)
    {
        bool alreadySessioned = false;
        {
            std::lock_guard<std::mutex> lock(m_sessionsMutex);
            alreadySessioned = std::any_of(m_sessions.cbegin(), m_sessions.cend(),
                [pid = processRecord.pid](const std::unique_ptr<Session>& sessionPointer) {
                    return sessionPointer != nullptr && sessionPointer->pid == pid;
                });
        }

        if (!alreadySessioned)
        {
            const QString agentDllPath = QDir::toNativeSeparators(QCoreApplication::applicationDirPath() + QStringLiteral("/APIMonitor_x64.dll"));
            std::string injectError;
            if (!ks::process::InjectDllByPath(processRecord.pid, agentDllPath.toStdString(), &injectError))
            {
                if (m_statusLabel != nullptr)
                {
                    m_statusLabel->setText(QStringLiteral("注入 PID=%1 失败：%2")
                        .arg(processRecord.pid).arg(QString::fromStdString(injectError)));
                }
                return;
            }
        }

        QString writeError;
        if (!writeSessionConfigForPid(processRecord.pid, matchedRule, &writeError))
        {
            if (m_statusLabel != nullptr)
            {
                m_statusLabel->setText(writeError);
            }
            return;
        }

        if (!alreadySessioned)
        {
            startPipeReadThreadForPid(processRecord.pid);
        }
    }

    void ClipboardGuardPage::teardownSession(const std::uint32_t pid)
    {
        std::unique_ptr<Session> sessionPointer;
        {
            std::lock_guard<std::mutex> lock(m_sessionsMutex);
            const auto sessionIt = std::find_if(m_sessions.begin(), m_sessions.end(),
                [pid](const std::unique_ptr<Session>& candidate) { return candidate != nullptr && candidate->pid == pid; });
            if (sessionIt == m_sessions.end())
            {
                return;
            }
            sessionPointer = std::move(*sessionIt);
            m_sessions.erase(sessionIt);
        }

        // 写停止标记，让 Agent 自己的 250ms 轮询发现后主动卸载 hook、退出会话。
        const QString stopFlagPath = QString::fromStdWString(ks::winapi_monitor::buildStopFlagPathForPid(pid));
        QFile stopFile(stopFlagPath);
        if (stopFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            stopFile.write("stop");
            stopFile.close();
        }

        sessionPointer->stopFlag.store(true);
        const std::uintptr_t handleValue = sessionPointer->pipeHandleValue.exchange(0);
        if (handleValue != 0)
        {
            // 主动关闭句柄打断阻塞中的 ReadFile，避免等目标进程自然退出才能 join。
            ::CloseHandle(reinterpret_cast<HANDLE>(handleValue));
        }
        if (sessionPointer->pipeThread != nullptr && sessionPointer->pipeThread->joinable())
        {
            sessionPointer->pipeThread->join();
        }
    }

    void ClipboardGuardPage::startPipeReadThreadForPid(const std::uint32_t pid)
    {
        auto sessionPointer = std::make_unique<Session>();
        sessionPointer->pid = pid;
        Session* const sessionRawPointer = sessionPointer.get();

        const QString pipeNameText = QString::fromStdWString(ks::winapi_monitor::buildPipeNameForPid(pid));
        QPointer<ClipboardGuardPage> guardThis(this);

        sessionPointer->pipeThread = std::make_unique<std::thread>([guardThis, sessionRawPointer, pipeNameText, pid]() {
            HANDLE pipeHandle = INVALID_HANDLE_VALUE;
            for (int attempt = 0; attempt < 120; ++attempt)
            {
                if (sessionRawPointer->stopFlag.load())
                {
                    return;
                }
                pipeHandle = ::CreateFileW(
                    reinterpret_cast<LPCWSTR>(pipeNameText.utf16()), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (pipeHandle != INVALID_HANDLE_VALUE)
                {
                    break;
                }
                const DWORD lastError = ::GetLastError();
                if (lastError != ERROR_FILE_NOT_FOUND && lastError != ERROR_PIPE_BUSY)
                {
                    return;
                }
                ::WaitNamedPipeW(reinterpret_cast<LPCWSTR>(pipeNameText.utf16()), 250);
                ::Sleep(120);
            }
            if (pipeHandle == INVALID_HANDLE_VALUE)
            {
                return;
            }

            sessionRawPointer->pipeHandleValue.store(reinterpret_cast<std::uintptr_t>(pipeHandle));

            while (!sessionRawPointer->stopFlag.load())
            {
                ks::winapi_monitor::ApiMonitorEventPacket packetValue{};
                DWORD bytesRead = 0;
                const BOOL readOk = ::ReadFile(pipeHandle, &packetValue, kPacketSize, &bytesRead, nullptr);
                if (readOk == FALSE || bytesRead == 0)
                {
                    break;
                }
                if (bytesRead < sizeof(packetValue) || packetValue.size != sizeof(packetValue)
                    || packetValue.version != ks::winapi_monitor::kProtocolVersion)
                {
                    continue;
                }
                // 只关心剪贴板分类；本会话的 INI 已经只开了这一类，这里再过滤一层双保险。
                if (packetValue.category != static_cast<std::uint32_t>(ks::winapi_monitor::EventCategory::Clipboard))
                {
                    continue;
                }

                QMetaObject::invokeMethod(qApp, [guardThis, packetValue, pid]() {
                    if (guardThis.isNull())
                    {
                        return;
                    }
                    const QString processText = guardThis->resolveProcessNameForPid(pid);
                    guardThis->enqueuePendingRow(packetToEventRow(packetValue, processText));
                }, Qt::QueuedConnection);
            }

            const std::uintptr_t storedHandleValue = sessionRawPointer->pipeHandleValue.exchange(0);
            if (storedHandleValue != 0)
            {
                ::CloseHandle(reinterpret_cast<HANDLE>(storedHandleValue));
            }
        });

        std::lock_guard<std::mutex> lock(m_sessionsMutex);
        m_sessions.push_back(std::move(sessionPointer));
    }

    void ClipboardGuardPage::enqueuePendingRow(ClipboardGuardEventRow rowValue)
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        if (m_pendingRows.size() >= kPendingRowCapacity)
        {
            m_pendingRows.pop_front();
        }
        m_pendingRows.push_back(std::move(rowValue));
    }

    void ClipboardGuardPage::flushPendingRows()
    {
        std::vector<ClipboardGuardEventRow> rowList;
        {
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            const std::size_t takeCount = std::min(kUiFlushRowLimit, m_pendingRows.size());
            rowList.reserve(takeCount);
            for (std::size_t index = 0; index < takeCount; ++index)
            {
                rowList.push_back(std::move(m_pendingRows.front()));
                m_pendingRows.pop_front();
            }
        }
        if (rowList.empty() || m_eventTable == nullptr)
        {
            return;
        }

        const bool updatesEnabled = m_eventTable->updatesEnabled();
        m_eventTable->setUpdatesEnabled(false);
        for (const ClipboardGuardEventRow& rowValue : rowList)
        {
            appendEventRow(rowValue);
        }
        const int removeCount = std::max(0, m_eventTable->rowCount() - 6000);
        if (removeCount > 0 && m_eventTable->model() != nullptr)
        {
            m_eventTable->model()->removeRows(0, removeCount);
        }
        m_eventTable->setUpdatesEnabled(updatesEnabled);
        m_eventTable->scrollToBottom();
    }

    void ClipboardGuardPage::appendEventRow(const ClipboardGuardEventRow& rowValue)
    {
        const int row = m_eventTable->rowCount();
        m_eventTable->insertRow(row);
        m_eventTable->setItem(row, ColumnTime, createReadOnlyItem(rowValue.timeText));
        m_eventTable->setItem(row, ColumnProcess, createReadOnlyItem(rowValue.processText));
        m_eventTable->setItem(row, ColumnPid, createReadOnlyItem(QString::number(rowValue.pid)));
        m_eventTable->setItem(row, ColumnOperation, createReadOnlyItem(rowValue.operationText));
        m_eventTable->setItem(row, ColumnFormat, createReadOnlyItem(rowValue.formatText));
        QTableWidgetItem* const resultItem = createReadOnlyItem(rowValue.resultText);
        if (rowValue.resultText == QStringLiteral("Blocked"))
        {
            resultItem->setForeground(QBrush(KswordTheme::ErrorColor()));
        }
        m_eventTable->setItem(row, ColumnResult, resultItem);
        m_eventTable->setItem(row, ColumnTid, createReadOnlyItem(QString::number(rowValue.tid)));
        m_eventTable->setItem(row, ColumnSession, createReadOnlyItem(rowValue.sessionText));
        m_eventTable->setItem(row, ColumnIntegrity, createReadOnlyItem(rowValue.integrityText));
        m_eventTable->setItem(row, ColumnOwner, createReadOnlyItem(rowValue.ownerText));
        QTableWidgetItem* const seqItem = createReadOnlyItem(QString::number(rowValue.seq));
        seqItem->setData(Qt::UserRole, static_cast<qulonglong>(rowValue.seq));
        m_eventTable->setItem(row, ColumnSeq, seqItem);
        m_eventTable->item(row, ColumnPid)->setData(Qt::UserRole, rowValue.pid);
    }

    QString ClipboardGuardPage::resolveProcessNameForPid(const quint32 pid)
    {
        const auto cacheIt = m_processNameCache.find(pid);
        if (cacheIt != m_processNameCache.end())
        {
            return cacheIt->second;
        }
        return QStringLiteral("PID %1").arg(pid);
    }

    void ClipboardGuardPage::refreshProcessListAndSessionsAsync()
    {
        if (m_processScanInFlight.exchange(true))
        {
            // 上一轮扫描还没结束（进程数很多或磁盘慢），跳过本轮，避免任务在线程池里堆积。
            return;
        }

        QPointer<ClipboardGuardPage> guardThis(this);
        QThreadPool::globalInstance()->start([guardThis]() {
            std::vector<ks::process::ProcessRecord> processList =
                ks::process::EnumerateProcesses(ks::process::ProcessEnumStrategy::Auto);

            QMetaObject::invokeMethod(qApp, [guardThis, processList = std::move(processList)]() mutable {
                if (guardThis.isNull())
                {
                    guardThis->m_processScanInFlight.store(false);
                    return;
                }

                guardThis->m_processNameCache.clear();
                for (const ks::process::ProcessRecord& processRecord : processList)
                {
                    guardThis->m_processNameCache.emplace(
                        processRecord.pid, QString::fromStdString(processRecord.processName));
                }

                // 按当前规则逐一核对：命中就确保已保护，退出的进程回收会话。
                std::vector<std::uint32_t> matchedPids;
                for (const ks::process::ProcessRecord& processRecord : processList)
                {
                    const ClipboardGuardRule* const matchedRule = guardThis->findMatchingRule(processRecord);
                    if (matchedRule != nullptr)
                    {
                        matchedPids.push_back(processRecord.pid);
                        guardThis->ensureProcessProtected(processRecord, *matchedRule);
                    }
                }

                std::vector<std::uint32_t> sessionPids;
                {
                    std::lock_guard<std::mutex> lock(guardThis->m_sessionsMutex);
                    for (const std::unique_ptr<Session>& sessionPointer : guardThis->m_sessions)
                    {
                        if (sessionPointer != nullptr)
                        {
                            sessionPids.push_back(sessionPointer->pid);
                        }
                    }
                }
                for (const std::uint32_t sessionPid : sessionPids)
                {
                    if (std::find(matchedPids.cbegin(), matchedPids.cend(), sessionPid) == matchedPids.cend())
                    {
                        // 进程退出或规则被删除/禁用：回收会话。写停止标记对已退出的进程是安全的空操作。
                        guardThis->teardownSession(sessionPid);
                    }
                }

                if (guardThis->m_statusLabel != nullptr)
                {
                    guardThis->m_statusLabel->setText(QStringLiteral("规则 %1 条，当前受保护进程 %2 个。")
                        .arg(guardThis->m_rules.size()).arg(matchedPids.size()));
                }
                guardThis->m_processScanInFlight.store(false);
            }, Qt::QueuedConnection);
        });
    }
}
