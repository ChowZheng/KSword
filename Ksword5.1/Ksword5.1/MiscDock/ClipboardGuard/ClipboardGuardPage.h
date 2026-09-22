#pragma once

// ============================================================
// ClipboardGuardPage.h
// 作用：
// 1) "剪贴板保护"杂项子页——按 PID/进程名/路径 管理剪贴板读/写/枚举策略；
// 2) 策略权威存储在驱动（ArkDriverClient::setClipboardPolicy），实际拦截发生在
//    被注入到受管进程里的 APIMonitor_x64.dll（复用现有 hook 引擎，见
//    APIMonitor_x64/hook/ClipboardGuardHook.* / ClipboardGuardVTableHook.* /
//    ClipboardGuardWin32u.*）；
// 3) 本页只做：规则管理 UI、按规则匹配当前运行进程并注入/下发配置、
//    接收命名管道事件并展示、右键快捷操作、详情/调用栈弹窗。
// ============================================================

#include "../../Framework.h"
#include "../../ArkDriverClient/ArkDriverClient.h"

#include <QString>
#include <QWidget>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

class QComboBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QMenu;
class QPoint;
class QPushButton;
class QTableWidget;
class QTableWidgetItem;
class QTimer;
class QVBoxLayout;

namespace ks::misc
{
    // ClipboardGuardColumnLayout：事件表 A/B 列组预设；Custom 表示用户已手动
    // 调整过列显隐，此时 A/B 按钮都不再着色（仓库 AGENTS.md 的 A/B 列组规范）。
    enum class ClipboardGuardColumnLayout
    {
        PresetA,
        PresetB,
        Custom
    };

    // ClipboardGuardRule：一条剪贴板策略规则。字段取值直接对应驱动协议
    // shared/driver/KswordArkClipboardPolicyIoctl.h 里的
    // KSWORD_ARK_CLIPBOARD_POLICY_TARGET_KIND_*/ACTION_*，不在 UI 层另建一套编码，
    // 避免下发/回读时来回转换出错。
    struct ClipboardGuardRule
    {
        quint32 ruleId = 0;
        bool enabled = true;
        quint32 targetKind = 0;       // NONE=0 / PID=1 / IMAGE_NAME=2 / IMAGE_PATH=3
        quint32 targetProcessId = 0;
        QString targetImage;
        QString ruleName;
        quint32 readAction = 0;       // ALLOW=0 / BLOCK=1 / LOG_ONLY=2
        quint32 writeAction = 0;
        quint32 enumAction = 0;
    };

    // ClipboardGuardQuickAction：事件表右键菜单里针对"这一行所属进程"的快捷动作。
    // BlockAll 与 LogOnly 都是"一次性设置三个方向"的便捷入口，底层仍然落回同一套
    // 独立的 readAction/writeAction/enumAction 三个字段，不是第四种状态。
    enum class ClipboardGuardQuickAction
    {
        BlockRead,
        BlockWrite,
        BlockAll,
        LogOnly
    };

    // ClipboardGuardEventRow：事件表一行的展示数据，从 Agent 上报事件包的
    // detail 文本（ClipboardGuardCommon.cpp::ReportClipboardEvent 拼出来的
    // key=value 格式）解析得到。
    struct ClipboardGuardEventRow
    {
        QString timeText;
        quint32 pid = 0;
        quint32 tid = 0;
        QString processText;
        QString operationText;   // READ / WRITE / ENUM
        QString formatText;
        QString resultText;      // Allowed / Blocked / LoggedOnly
        QString sessionText;
        QString integrityText;
        QString ownerText;
        quint64 seq = 0;
    };

    class ClipboardGuardPage final : public QWidget
    {
        Q_OBJECT

    public:
        explicit ClipboardGuardPage(QWidget* parent = nullptr);
        ~ClipboardGuardPage() override;

        // notifyPageActivated：页签首次真正显示时按需从驱动回读规则、启动轮询。
        void notifyPageActivated();

        // EventColumn：事件表列索引。前 6 列是 A 组"概览"，后 5 列是 B 组
        // "来源与诊断"；两组共用同一张表，只是显隐不同（AGENTS.md A/B 列组规范
        // 要求两组都是精简视图，不是"A 追加扩展列"）。
        enum EventColumn
        {
            ColumnTime = 0,
            ColumnProcess,
            ColumnPid,
            ColumnOperation,
            ColumnFormat,
            ColumnResult,
            ColumnTid,
            ColumnSession,
            ColumnIntegrity,
            ColumnOwner,
            ColumnSeq,
            ColumnCount
        };

    private:
        // ---- ClipboardGuardPage.cpp：UI 骨架、A/B 列组 ----
        void initializeUi();
        void initializeConnections();
        void applyColumnLayout(ClipboardGuardColumnLayout layout);
        void clearColumnPresetSelection();
        void updateColumnPresetButtons();
        void showHeaderContextMenu(const QPoint& position);
        static QString buildPresetButtonStyle(bool leftButton);
        static QTableWidgetItem* createReadOnlyItem(const QString& textValue);

        // ---- ClipboardGuardPage.RuleEditor.cpp：右键菜单、规则对话框、详情弹窗 ----
        void showEventTableContextMenu(const QPoint& position);
        void openAddProcessRuleDialog();
        void removeSelectedRule();
        void applyQuickActionToSelectedRow(ClipboardGuardQuickAction action);
        void showClipboardContentsForSelectedRow();
        void showCallStackForSelectedRow();
        int selectedEventRow() const;

        // ---- ClipboardGuardPage.Session.cpp：驱动同步、注入编排、命名管道事件 ----
        void refreshRuleTable();
        void syncRulesToDriver();
        void loadRulesFromDriver();
        void refreshProcessListAndSessionsAsync();
        const ClipboardGuardRule* findMatchingRule(const ks::process::ProcessRecord& processRecord) const;
        void ensureProcessProtected(const ks::process::ProcessRecord& processRecord, const ClipboardGuardRule& matchedRule);
        void teardownSession(std::uint32_t pid);
        void startPipeReadThreadForPid(std::uint32_t pid);
        void enqueuePendingRow(ClipboardGuardEventRow rowValue);
        void flushPendingRows();
        void appendEventRow(const ClipboardGuardEventRow& rowValue);
        bool writeSessionConfigForPid(std::uint32_t pid, const ClipboardGuardRule& matchedRule, QString* errorTextOut) const;
        QString resolveProcessNameForPid(quint32 pid);

    private:
        struct Session
        {
            std::uint32_t pid = 0;
            std::unique_ptr<std::thread> pipeThread;
            std::atomic_bool stopFlag{ false };
            std::atomic<std::uintptr_t> pipeHandleValue{ 0 };
        };

        // ---- 布局 ----
        QVBoxLayout* m_rootLayout = nullptr;
        QHBoxLayout* m_toolbarLayout = nullptr;
        QPushButton* m_addRuleButton = nullptr;
        QPushButton* m_removeRuleButton = nullptr;
        QPushButton* m_refreshButton = nullptr;
        QWidget* m_columnPresetWidget = nullptr;
        QPushButton* m_columnPresetAButton = nullptr;
        QPushButton* m_columnPresetBButton = nullptr;
        QLabel* m_statusLabel = nullptr;
        QTableWidget* m_ruleTable = nullptr;
        QTableWidget* m_eventTable = nullptr;
        ClipboardGuardColumnLayout m_columnLayout = ClipboardGuardColumnLayout::PresetA;
        bool m_applyingColumnLayout = false;
        bool m_hasActivatedOnce = false;

        // ---- 规则 ----
        std::vector<ClipboardGuardRule> m_rules;
        quint32 m_nextLocalRuleId = 1;

        // ---- 会话（每个受管进程一个命名管道读取线程）----
        std::vector<std::unique_ptr<Session>> m_sessions;
        std::mutex m_sessionsMutex;

        std::deque<ClipboardGuardEventRow> m_pendingRows;
        std::mutex m_pendingMutex;
        static constexpr std::size_t kPendingRowCapacity = 8000;
        static constexpr std::size_t kUiFlushRowLimit = 160;
        QTimer* m_uiFlushTimer = nullptr;
        QTimer* m_processPollTimer = nullptr;
        std::atomic_bool m_processScanInFlight{ false };

        std::unordered_map<quint32, QString> m_processNameCache;

        ksword::ark::DriverClient m_driverClient;
    };
}
