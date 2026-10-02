#include "WinAPIDock.h"
#include "../UI/ThemeStatusRole.h"
#include <QApplication>
#include <algorithm>
#include <QComboBox>
#include <QCryptographicHash>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTabWidget>

namespace
{
    QString coverageText(const wchar_t* text, std::size_t capacity)
    { return QString::fromWCharArray(text, static_cast<int>(wcsnlen_s(text, capacity))); }
    QString coverageStateText(std::uint32_t state)
    {
        using ks::winapi_monitor::CoverageState;
        switch (static_cast<CoverageState>(state))
        {
        case CoverageState::Installed: return QStringLiteral("已安装");
        case CoverageState::SharedEntry: return QStringLiteral("共享入口");
        case CoverageState::CategoryDisabled: return QStringLiteral("分类关闭");
        case CoverageState::WaitingModule: return QStringLiteral("等待模块");
        case CoverageState::ExportMissing: return QStringLiteral("导出缺失");
        case CoverageState::RuleExcluded: return QStringLiteral("规则排除");
        case CoverageState::RetryableFailure: return QStringLiteral("可重试失败");
        case CoverageState::Unsupported: return QStringLiteral("不支持");
        case CoverageState::Removed: return QStringLiteral("已移除");
        }
        return QStringLiteral("未知");
    }
    QString coverageKindText(std::uint32_t kind)
    {
        using ks::winapi_monitor::HookKind;
        switch (static_cast<HookKind>(kind))
        {
        case HookKind::Strong: return QStringLiteral("强类型");
        case HookKind::Raw: return QString::fromLatin1("Raw");
        case HookKind::Fake: return QString::fromLatin1("Fake");
        case HookKind::DynamicExtension: return QStringLiteral("动态扩展");
        }
        return QStringLiteral("未知");
    }
}

void WinAPIDock::queueCoverageSnapshot(std::uint32_t pid, std::uint64_t session, std::uint64_t revision,
    std::vector<ks::winapi_monitor::ApiMonitorEventPacket> rows, bool stale, std::uint64_t generation)
{
    const std::lock_guard<std::mutex> lock(m_pendingMutex);
    if (generation != m_sessionGeneration.load()) return;
    auto& pending = m_pendingCoverage[pid];
    if (pending.session && (pending.session != session || pending.revision > revision)) return;
    pending.session = session;
    pending.revision = revision;
    pending.stale = stale;
    if (!stale) { pending.rows = std::move(rows); pending.complete = true; }
}

void WinAPIDock::flushCoverageUpdates()
{
    std::unordered_map<std::uint32_t, CoverageViewState> updates;
    {
        const std::lock_guard<std::mutex> lock(m_pendingMutex);
        updates.swap(m_pendingCoverage);
    }
    bool changed = false;
    for (auto& [pid, update] : updates)
    {
        auto& visible = m_coverageViews[pid];
        if (visible.session && (visible.session != update.session || visible.revision > update.revision)) continue;
        visible.session = update.session;
        if (update.complete) { visible.revision = update.revision; visible.rows = std::move(update.rows); visible.complete = true; }
        visible.stale = update.stale;
        if (pid == m_currentSessionPid && !visible.rows.empty())
        {
            QFile definitions(QApplication::applicationDirPath() + QStringLiteral("/profiles/api_monitor_definitions.json"));
            const auto hash = definitions.open(QIODevice::ReadOnly)
                ? QCryptographicHash::hash(definitions.readAll(), QCryptographicHash::Sha256).toHex() : QByteArray();
            m_definitionMismatch = hash.isEmpty() || hash != QByteArray(visible.rows.front().definitionSha256, 64);
        }
        changed = true;
    }
    if (!changed) return;
    m_coverageDirty = true;
    const QSignalBlocker blocker(m_coveragePidCombo);
    const auto previousPid = m_coveragePidCombo->currentData().toUInt();
    m_coveragePidCombo->clear();
    std::vector<std::uint32_t> pids;
    for (const auto& [pid, view] : m_coverageViews) pids.push_back(pid);
    std::sort(pids.begin(), pids.end());
    for (const auto pid : pids) m_coveragePidCombo->addItem(QString::number(pid), pid);
    const int selected = m_coveragePidCombo->findData(previousPid);
    if (selected >= 0) m_coveragePidCombo->setCurrentIndex(selected);
    if (m_resultTabs->currentIndex() == 1) updateCoverageView();
}

void WinAPIDock::updateCoverageView()
{
    if (!m_coverageTable || !m_coveragePidCombo || !m_coverageStatusLabel) return;
    const auto pid = m_coveragePidCombo->currentData().toUInt();
    const auto iterator = m_coverageViews.find(pid);
    if (iterator == m_coverageViews.end())
    {
        m_coverageTable->setRowCount(0);
        m_coverageStatusLabel->setText(QStringLiteral("覆盖快照：等待 Agent 完整快照"));
        return;
    }
    const auto& view = iterator->second;
    QFile definitions(QApplication::applicationDirPath() + QStringLiteral("/profiles/api_monitor_definitions.json"));
    const auto localHash = definitions.open(QIODevice::ReadOnly)
        ? QCryptographicHash::hash(definitions.readAll(), QCryptographicHash::Sha256).toHex() : QByteArray();
    const auto remoteHash = view.rows.empty() ? QByteArray() : QByteArray(view.rows.front().definitionSha256, 64);
    const bool mismatch = localHash.isEmpty() || remoteHash != localHash;
    if (pid == m_currentSessionPid) m_definitionMismatch = mismatch;
    QString status = view.stale ? QStringLiteral("覆盖快照：已过期或不完整") : QStringLiteral("覆盖快照：完整");
    status += QStringLiteral(" | PID=%1，修订=%2，API=%3").arg(pid).arg(static_cast<qulonglong>(view.revision)).arg(view.rows.size());
    if (mismatch) status += QStringLiteral(" | API 定义不匹配或缺失");
    m_coverageStatusLabel->setText(status);
    ks::ui::ApplyStatusRole(m_coverageStatusLabel, view.stale || mismatch ? ks::ui::StatusRole::Warning : ks::ui::StatusRole::Info);
    const auto filter = m_coverageFilterEdit->text().trimmed();
    m_coverageTable->setUpdatesEnabled(false);
    m_coverageTable->setRowCount(0);
    for (const auto& packet : view.rows)
    {
        const auto api = coverageText(packet.moduleName, std::size(packet.moduleName)) + QString::fromLatin1("!")
            + coverageText(packet.apiName, std::size(packet.apiName));
        const auto state = coverageStateText(packet.coverageState);
        const auto kind = coverageKindText(packet.hookKind);
        const auto detail = coverageText(packet.detailText, std::size(packet.detailText));
        if (!filter.isEmpty() && !(api + state + kind + detail).contains(filter, Qt::CaseInsensitive)) continue;
        const int row = m_coverageTable->rowCount();
        m_coverageTable->insertRow(row);
        const QStringList columns{QString::number(pid), QString::number(packet.apiId), kind, api, state,
            QString::fromLatin1("0x%1").arg(static_cast<qulonglong>(packet.hookAddress), 0, 16), detail};
        for (int col = 0; col < columns.size(); ++col)
            m_coverageTable->setItem(row, col, createReadOnlyItem(columns[col]));
    }
    m_coverageTable->setUpdatesEnabled(true);
    m_coverageDirty = false;
}

void WinAPIDock::resetCoverageView()
{
    {
        const std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_pendingCoverage.clear();
    }
    m_coverageViews.clear();
    m_coveragePidCombo->clear();
    m_coverageDirty = true;
    updateCoverageView();
}

void WinAPIDock::markCoverageStale()
{
    flushCoverageUpdates();
    for (auto& [pid, view] : m_coverageViews) view.stale = true;
    m_coverageDirty = true;
    if (m_resultTabs->currentIndex() == 1) updateCoverageView();
}
