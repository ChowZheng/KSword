// Int3PatchPanel.cpp
// 作用：Int3PatchPanel.h 的实现。设计动机见该头文件顶部注释。
//
// 文件结构：构造/UI 搭建 -> 展示回调与插入点设置 -> 折叠状态 -> 表格重建与格式化辅助 ->
// 三个工具钮的点击处理 -> 右键菜单 -> 结果文案翻译。

#include "Int3PatchPanel.h"

#include "../../theme.h"

#include <QAbstractItemView>
#include <QAction>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QMenu>
#include <QPoint>
#include <QPointer>
#include <QSizePolicy>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariant>

#include <iterator>

namespace ks::ui
{
    using ksword::memwb::Channel;
    using ksword::memwb::InstallStatus;
    using ksword::memwb::PatchEntry;
    using ksword::memwb::RestoreStatus;

    namespace
    {
        // kIdRole：表格条目 id 存放的用户数据角色。
        constexpr int kIdRole = Qt::UserRole;
        // kOrphanedRole：该行是否属于孤立分组（而不是分组标题本身）的标记角色。
        constexpr int kOrphanedRole = Qt::UserRole + 1;
        // kGroupHeaderRole：该行是否是"孤立补丁"分组标题行（禁用项，没有 id）。
        constexpr int kGroupHeaderRole = Qt::UserRole + 2;

        // ByteHexText：把一个字节格式化成"0xAB"形式的大写十六进制文本。
        QString ByteHexText(const std::uint8_t value)
        {
            return QStringLiteral("0x%1").arg(static_cast<uint>(value), 2, 16, QLatin1Char('0')).toUpper();
        }
    }

    // 构造：搭 UI、连信号，用当前账本状态做一次初始刷新。
    Int3PatchPanel::Int3PatchPanel(Int3Controller* controller, QWidget* parent)
        : QWidget(parent)
        , m_controller(controller)
    {
        buildUi();
        if (m_controller != nullptr)
        {
            connect(m_controller, &Int3Controller::changed, this, &Int3PatchPanel::onLedgerChanged);
        }
        onLedgerChanged();
    }

    // buildUi：折叠头部 + 工具栏 + 表格，一次性搭好。
    void Int3PatchPanel::buildUi()
    {
        auto* rootLayout = new QVBoxLayout(this);
        rootLayout->setContentsMargins(0, 0, 0, 0);
        rootLayout->setSpacing(0);

        // 折叠头部：固定 24px 高，横向撑满；点击切换折叠/展开。
        m_header = new QToolButton(this);
        m_header->setObjectName(QStringLiteral("int3PatchPanelHeader"));
        m_header->setAutoRaise(true);
        m_header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        m_header->setFixedHeight(24);
        m_header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_header->setToolTip(QStringLiteral("展开或折叠 int3 补丁列表"));
        rootLayout->addWidget(m_header);
        connect(m_header, &QToolButton::clicked, this, &Int3PatchPanel::onHeaderClicked);

        // 内容容器：工具栏 + 表格，折叠时整体隐藏。
        m_body = new QWidget(this);
        auto* bodyLayout = new QVBoxLayout(m_body);
        bodyLayout->setContentsMargins(4, 4, 4, 4);
        bodyLayout->setSpacing(4);
        rootLayout->addWidget(m_body);

        // 工具栏：写入（插入点）/ 还原（选中行）/ 全部还原。
        auto* toolbar = new QWidget(m_body);
        auto* toolbarLayout = new QHBoxLayout(toolbar);
        toolbarLayout->setContentsMargins(0, 0, 0, 0);
        toolbarLayout->setSpacing(4);

        m_installButton = new QToolButton(toolbar);
        m_installButton->setIcon(QIcon(QStringLiteral(":/Icon/disk_tools.svg")));
        m_installButton->setToolTip(QStringLiteral("在插入点写入 int3（0xCC），记下原字节以便还原"));
        KswordTheme::ApplyCompactIconButtonMetrics(m_installButton);
        toolbarLayout->addWidget(m_installButton);
        connect(m_installButton, &QToolButton::clicked, this, &Int3PatchPanel::onInstallClicked);

        m_restoreButton = new QToolButton(toolbar);
        m_restoreButton->setIcon(QIcon(QStringLiteral(":/Icon/memwb_restore.svg")));
        m_restoreButton->setToolTip(QStringLiteral("写回选中补丁的原字节"));
        KswordTheme::ApplyCompactIconButtonMetrics(m_restoreButton);
        toolbarLayout->addWidget(m_restoreButton);
        connect(m_restoreButton, &QToolButton::clicked, this, &Int3PatchPanel::onRestoreClicked);

        m_restoreAllButton = new QToolButton(toolbar);
        m_restoreAllButton->setIcon(QIcon(QStringLiteral(":/Icon/memwb_restore_all.svg")));
        m_restoreAllButton->setToolTip(QStringLiteral("还原当前目标上的全部 int3 补丁"));
        KswordTheme::ApplyCompactIconButtonMetrics(m_restoreAllButton);
        toolbarLayout->addWidget(m_restoreAllButton);
        connect(m_restoreAllButton, &QToolButton::clicked, this, &Int3PatchPanel::onRestoreAllClicked);

        toolbarLayout->addStretch(1);
        bodyLayout->addWidget(toolbar);

        // 表格：地址(模块+RVA) · 原字节 · 目标(进程/已退出)。
        m_table = new QTableWidget(0, 3, m_body);
        m_table->setHorizontalHeaderLabels({
            QStringLiteral("地址"),
            QStringLiteral("原字节"),
            QStringLiteral("目标") });
        m_table->verticalHeader()->setVisible(false);
        m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_table->setSelectionMode(QAbstractItemView::SingleSelection);
        m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_table->horizontalHeader()->setStretchLastSection(true);
        m_table->setContextMenuPolicy(Qt::CustomContextMenu);
        bodyLayout->addWidget(m_table);
        connect(m_table, &QTableWidget::itemSelectionChanged, this, &Int3PatchPanel::onSelectionChanged);
        connect(
            m_table,
            &QTableWidget::customContextMenuRequested,
            this,
            &Int3PatchPanel::onTableContextMenuRequested);

        SetCollapsed(true);
    }

    // SetAddressFormatter / SetTargetLabelFormatter：注入展示回调并立即重建表格。
    void Int3PatchPanel::SetAddressFormatter(AddressFormatter formatter)
    {
        m_addressFormatter = std::move(formatter);
        rebuildTable();
    }

    void Int3PatchPanel::SetTargetLabelFormatter(TargetLabelFormatter formatter)
    {
        m_targetLabelFormatter = std::move(formatter);
        rebuildTable();
    }

    // SetInsertionPoint：记下插入点并刷新"写入"按钮可用性。
    void Int3PatchPanel::SetInsertionPoint(const std::uint64_t address, const bool hasPoint)
    {
        m_insertionPoint = hasPoint ? std::make_optional(address) : std::nullopt;
        updateToolbarEnabled();
    }

    // SetCollapsed：切换头部图标与内容容器显隐。
    void Int3PatchPanel::SetCollapsed(const bool collapsed)
    {
        m_collapsed = collapsed;
        if (m_body != nullptr)
        {
            m_body->setVisible(!collapsed);
        }
        if (m_header != nullptr)
        {
            const QString iconPath = collapsed
                ? QStringLiteral(":/Icon/detail_node_collapsed.svg")
                : QStringLiteral(":/Icon/detail_node_expanded.svg");
            m_header->setIcon(QIcon(iconPath));
        }
    }

    // onHeaderClicked：点击头部即切换折叠状态。
    void Int3PatchPanel::onHeaderClicked()
    {
        SetCollapsed(!m_collapsed);
    }

    // onLedgerChanged：账本可能已变化，重建表格、刷新头部文字与工具钮可用性。
    void Int3PatchPanel::onLedgerChanged()
    {
        rebuildTable();
        updateHeaderText();
        updateToolbarEnabled();
    }

    // updateHeaderText：头部文字"int3 补丁 (n)"，n 取待还原条目数（孤立条目不计入，
    // 与 Int3Controller::HasUnrestored 的口径一致）。
    void Int3PatchPanel::updateHeaderText()
    {
        if (m_header == nullptr || m_controller == nullptr)
        {
            return;
        }
        const int count = static_cast<int>(m_controller->Entries().size());
        m_header->setText(QStringLiteral("int3 补丁 (%1)").arg(count));
    }

    // rebuildTable：先列出待还原条目，Diverged 的行追加说明与警示底色；再列出孤立分组
    // （标题行 + 各条目）。同时清理 m_divergedIds 里已经不在待还原列表中的 id。
    void Int3PatchPanel::rebuildTable()
    {
        if (m_table == nullptr || m_controller == nullptr)
        {
            return;
        }

        m_table->setRowCount(0);

        const std::vector<PatchEntry>& entries = m_controller->Entries();
        std::unordered_set<std::uint64_t> stillPending;
        for (const PatchEntry& entry : entries)
        {
            stillPending.insert(entry.id);

            const int row = m_table->rowCount();
            m_table->insertRow(row);

            auto* addressItem = new QTableWidgetItem(FormatAddress(entry.address));
            addressItem->setData(kIdRole, QVariant::fromValue<qulonglong>(entry.id));
            addressItem->setData(kOrphanedRole, false);
            m_table->setItem(row, 0, addressItem);
            m_table->setItem(row, 1, new QTableWidgetItem(ByteHexText(entry.originalByte)));

            auto* targetItem = new QTableWidgetItem(FormatTarget(entry.pid, entry.processCreateTime100ns, false));

            const bool diverged = m_divergedIds.count(entry.id) > 0;
            if (diverged)
            {
                // Diverged：当前字节已不是 CC，三格都标警示色并给出同一条 tooltip，
                // 唯一允许的动作是右键"丢弃记录"。
                const QString note = QStringLiteral("当前字节已不是 CC（被别处改过）");
                addressItem->setToolTip(note);
                m_table->item(row, 1)->setToolTip(note);
                targetItem->setToolTip(note);
                const QColor warningBackground = KswordTheme::WarningBackgroundColor();
                addressItem->setBackground(warningBackground);
                m_table->item(row, 1)->setBackground(warningBackground);
                targetItem->setBackground(warningBackground);
                targetItem->setForeground(KswordTheme::WarningColor());
            }
            m_table->setItem(row, 2, targetItem);
        }

        // 清理已经不在待还原列表中的 diverged 标记（已被还原或丢弃）。
        for (auto it = m_divergedIds.begin(); it != m_divergedIds.end();)
        {
            it = (stillPending.count(*it) > 0) ? std::next(it) : m_divergedIds.erase(it);
        }

        const std::vector<PatchEntry>& orphaned = m_controller->OrphanedEntries();
        if (!orphaned.empty())
        {
            // 分组标题：禁用项当分节文字（本仓库 QMenu::addSection 的文字不显示，表格同理
            // 用禁用项更稳妥一致），不带 id，右键菜单据 kGroupHeaderRole 识别它触发"清除"。
            const int headerRow = m_table->rowCount();
            m_table->insertRow(headerRow);
            auto* headerItem = new QTableWidgetItem(QStringLiteral("孤立补丁（目标已退出）"));
            headerItem->setFlags(Qt::ItemIsEnabled);
            headerItem->setData(kGroupHeaderRole, true);
            headerItem->setForeground(KswordTheme::TextDisabledColor());
            m_table->setItem(headerRow, 0, headerItem);
            m_table->setSpan(headerRow, 0, 1, 3);

            for (const PatchEntry& entry : orphaned)
            {
                const int row = m_table->rowCount();
                m_table->insertRow(row);
                auto* addressItem = new QTableWidgetItem(FormatAddress(entry.address));
                addressItem->setData(kIdRole, QVariant::fromValue<qulonglong>(entry.id));
                addressItem->setData(kOrphanedRole, true);
                addressItem->setForeground(KswordTheme::TextSecondaryColor());
                m_table->setItem(row, 0, addressItem);
                auto* byteItem = new QTableWidgetItem(ByteHexText(entry.originalByte));
                byteItem->setForeground(KswordTheme::TextSecondaryColor());
                m_table->setItem(row, 1, byteItem);
                auto* targetItem = new QTableWidgetItem(FormatTarget(entry.pid, entry.processCreateTime100ns, true));
                targetItem->setForeground(KswordTheme::TextSecondaryColor());
                m_table->setItem(row, 2, targetItem);
            }
        }
    }

    // updateToolbarEnabled：三个工具钮的可用性与 tooltip 随路由/插入点/选中行变化。
    void Int3PatchPanel::updateToolbarEnabled()
    {
        if (m_controller == nullptr)
        {
            return;
        }

        if (m_installButton != nullptr)
        {
            const Int3RouteReject reject = m_controller->EvaluateCurrentRoute();
            if (reject == Int3RouteReject::UnsupportedScope)
            {
                m_installButton->setEnabled(false);
                m_installButton->setToolTip(QStringLiteral("int3 补丁只支持进程范围，当前范围不可写入"));
            }
            else if (reject == Int3RouteReject::UnsupportedChannel)
            {
                m_installButton->setEnabled(false);
                m_installButton->setToolTip(
                    QStringLiteral("int3 补丁不支持磁盘传输通道，请切换到用户态 / 标准驱动 / HVM 通道"));
            }
            else
            {
                m_installButton->setEnabled(m_insertionPoint.has_value());
                m_installButton->setToolTip(
                    m_insertionPoint.has_value()
                        ? QStringLiteral("在插入点写入 int3（0xCC），记下原字节以便还原")
                        : QStringLiteral("请先在十六进制区选中插入点"));
            }
        }

        if (m_restoreButton != nullptr)
        {
            const std::optional<std::uint64_t> selected = SelectedActiveEntryId();
            const bool diverged = selected.has_value() && (m_divergedIds.count(*selected) > 0);
            m_restoreButton->setEnabled(selected.has_value() && !diverged);
            m_restoreButton->setToolTip(
                diverged
                    ? QStringLiteral("当前字节已不是 CC，无法还原；请右键选择\"丢弃记录\"")
                    : QStringLiteral("写回选中补丁的原字节"));
        }

        if (m_restoreAllButton != nullptr)
        {
            m_restoreAllButton->setEnabled(!m_controller->Entries().empty());
        }
    }

    // onSelectionChanged：选中行变化只影响"还原"按钮。
    void Int3PatchPanel::onSelectionChanged()
    {
        updateToolbarEnabled();
    }

    // FormatAddress：注入了回调则用它，否则退化为十六进制。
    QString Int3PatchPanel::FormatAddress(const std::uint64_t address) const
    {
        if (m_addressFormatter)
        {
            return m_addressFormatter(address);
        }
        return QStringLiteral("0x%1").arg(address, 0, 16);
    }

    // FormatTarget：orphaned 为真恒返回"已退出"（结构性事实，不查进程名）；否则注入了回调
    // 则用它，否则退化为"PID <n>"。
    QString Int3PatchPanel::FormatTarget(
        const std::uint32_t pid,
        const std::uint64_t processCreateTime100ns,
        const bool orphaned) const
    {
        if (orphaned)
        {
            return QStringLiteral("已退出");
        }
        if (m_targetLabelFormatter)
        {
            return m_targetLabelFormatter(pid, processCreateTime100ns);
        }
        return QStringLiteral("PID %1").arg(pid);
    }

    // SelectedActiveEntryId：选中行必须带 kIdRole 且不是孤立项、不是分组标题。
    std::optional<std::uint64_t> Int3PatchPanel::SelectedActiveEntryId() const
    {
        if (m_table == nullptr)
        {
            return std::nullopt;
        }
        const QList<QTableWidgetItem*> selected = m_table->selectedItems();
        if (selected.isEmpty())
        {
            return std::nullopt;
        }
        QTableWidgetItem* firstCell = m_table->item(selected.first()->row(), 0);
        if (firstCell == nullptr || firstCell->data(kGroupHeaderRole).toBool())
        {
            return std::nullopt;
        }
        if (firstCell->data(kOrphanedRole).toBool())
        {
            return std::nullopt;
        }
        const QVariant idVariant = firstCell->data(kIdRole);
        if (!idVariant.isValid())
        {
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(idVariant.toULongLong());
    }

    // onInstallClicked：用当前插入点与控制器的当前目标发起 Install。
    void Int3PatchPanel::onInstallClicked()
    {
        if (m_controller == nullptr || !m_insertionPoint.has_value())
        {
            return;
        }
        const std::uint64_t address = *m_insertionPoint;
        // nowTick：int3 账本要求调用方给定时钟读数以保持可测；界面侧用系统时钟即可。
        const std::uint64_t nowTick = static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch());
        const QPointer<Int3PatchPanel> self(this);
        // 先让宿主声明"当前目标"，再读 CurrentTarget()——两者的先后顺序就是本信号存在的理由。
        emit aboutToAct();
        if (!self)
        {
            return;
        }
        const Int3InstallOutcome outcome = m_controller->Install(m_controller->CurrentTarget(), address, nowTick);
        if (!self)
        {
            return;
        }
        if (outcome.status == InstallStatus::Installed && IsCollapsed())
        {
            // 首次写入自动展开（ux.md 第 4.4 节）。
            SetCollapsed(false);
            if (!self)
            {
                return;
            }
        }
        EmitInstallMessage(outcome, address);
    }

    // onRestoreClicked：还原当前选中的待还原条目。
    void Int3PatchPanel::onRestoreClicked()
    {
        const std::optional<std::uint64_t> id = SelectedActiveEntryId();
        if (m_controller == nullptr || !id.has_value())
        {
            return;
        }
        const QPointer<Int3PatchPanel> self(this);
        emit aboutToAct();
        if (!self)
        {
            return;
        }
        const Int3RestoreOutcome outcome = m_controller->Restore(*id);
        if (!self)
        {
            return;
        }
        EmitRestoreMessage(outcome, *id);
    }

    // onRestoreAllClicked：还原当前目标上的全部待还原条目，汇总一条文案。
    void Int3PatchPanel::onRestoreAllClicked()
    {
        if (m_controller == nullptr)
        {
            return;
        }
        const QPointer<Int3PatchPanel> self(this);
        emit aboutToAct();
        if (!self)
        {
            return;
        }
        const std::vector<ksword::memwb::PatchRestoreOutcome> outcomes = m_controller->RestoreAll();
        if (!self)
        {
            return;
        }
        int restoredCount = 0;
        int divergedCount = 0;
        int otherFailureCount = 0;
        for (const ksword::memwb::PatchRestoreOutcome& outcome : outcomes)
        {
            if (outcome.result.status == RestoreStatus::Restored)
            {
                ++restoredCount;
            }
            else if (outcome.result.status == RestoreStatus::Diverged)
            {
                ++divergedCount;
                m_divergedIds.insert(outcome.id);
            }
            else
            {
                ++otherFailureCount;
            }
        }

        if (outcomes.empty())
        {
            emit resultMessage(QStringLiteral("当前目标没有待还原的 int3 补丁"), false);
        }
        else if (divergedCount == 0 && otherFailureCount == 0)
        {
            emit resultMessage(QStringLiteral("已全部还原（%1 处）").arg(restoredCount), false);
        }
        else
        {
            emit resultMessage(
                QStringLiteral("已还原 %1 处，%2 处字节被别处改过，%3 处失败")
                    .arg(restoredCount)
                    .arg(divergedCount)
                    .arg(otherFailureCount),
                true);
        }
        if (!self)
        {
            return;
        }
        rebuildTable();
    }

    // onTableContextMenuRequested：分组标题行给"清除"，Diverged 行给"丢弃记录"，其余行不弹菜单。
    void Int3PatchPanel::onTableContextMenuRequested(const QPoint& pos)
    {
        if (m_table == nullptr || m_controller == nullptr)
        {
            return;
        }
        const QTableWidgetItem* clicked = m_table->itemAt(pos);
        if (clicked == nullptr)
        {
            return;
        }
        QTableWidgetItem* firstCell = m_table->item(clicked->row(), 0);
        if (firstCell == nullptr)
        {
            return;
        }

        QMenu menu(this);
        // 右键菜单是高风险点：显式设置不透明背景、文字、选中态、禁用态，全部用主题静态色。
        menu.setAttribute(Qt::WA_TranslucentBackground, false);
        menu.setAutoFillBackground(true);
        menu.setStyleSheet(QStringLiteral(
            "QMenu{background-color:%1;color:%2;border:1px solid %3;padding:3px;}"
            "QMenu::item{color:%2;background-color:transparent;padding:5px 20px 5px 24px;}"
            "QMenu::item:selected{background-color:%4;color:%5;}"
            "QMenu::item:disabled{color:%6;background-color:transparent;}")
            .arg(KswordTheme::SurfaceColorHex())
            .arg(KswordTheme::TextPrimaryColorHex())
            .arg(KswordTheme::BorderColorHex())
            .arg(KswordTheme::ThemeColorName(KswordTheme::PrimaryAccentColor()))
            .arg(KswordTheme::OnAccentHex())
            .arg(KswordTheme::TextDisabledColorHex()));

        if (firstCell->data(kGroupHeaderRole).toBool())
        {
            QAction* clearAction = menu.addAction(QIcon(QStringLiteral(":/Icon/log_clear.svg")), QStringLiteral("清除"));
            clearAction->setToolTip(QStringLiteral("清除全部孤立补丁记录（目标已不存在，仅清除记录，不做任何写入）"));
            connect(clearAction, &QAction::triggered, this, [this]() {
                m_controller->ClearOrphaned();
                emit resultMessage(QStringLiteral("已清除孤立补丁记录"), false);
            });
        }
        else if (!firstCell->data(kOrphanedRole).toBool())
        {
            const QVariant idVariant = firstCell->data(kIdRole);
            if (!idVariant.isValid())
            {
                return;
            }
            const std::uint64_t id = static_cast<std::uint64_t>(idVariant.toULongLong());
            const bool diverged = m_divergedIds.count(id) > 0;
            if (!diverged)
            {
                return;
            }
            QAction* discardAction =
                menu.addAction(QIcon(QStringLiteral(":/Icon/log_clear.svg")), QStringLiteral("丢弃记录"));
            discardAction->setToolTip(QStringLiteral("只删记录不写内存（当前字节已被别处改过时用）"));
            connect(discardAction, &QAction::triggered, this, [this, id]() {
                const bool ok = m_controller->Discard(id);
                m_divergedIds.erase(id);
                emit resultMessage(
                    ok ? QStringLiteral("已丢弃该条记录") : QStringLiteral("丢弃失败：记录已不存在"),
                    !ok);
            });
        }
        else
        {
            return;
        }

        menu.exec(m_table->viewport()->mapToGlobal(pos));
    }

    // ChannelGuidance：标准驱动通道的失败追加引导语；其余通道返回空串。
    QString Int3PatchPanel::ChannelGuidance(const Channel channel) const
    {
        if (channel == Channel::StandardDriver)
        {
            return QStringLiteral("（标准驱动通道可能要求强制同意，int3 补丁不会自动确认；可切换到用户态通道重试）");
        }
        return QString();
    }

    // EmitInstallMessage：把 Install 的结果翻译成中文提示。
    void Int3PatchPanel::EmitInstallMessage(const Int3InstallOutcome& outcome, const std::uint64_t address)
    {
        if (outcome.routeReject == Int3RouteReject::UnsupportedScope)
        {
            emit resultMessage(QStringLiteral("int3 补丁只支持进程范围，当前范围不可写入"), true);
            return;
        }
        if (outcome.routeReject == Int3RouteReject::UnsupportedChannel)
        {
            emit resultMessage(
                QStringLiteral("int3 补丁不支持磁盘传输通道，请切换到用户态 / 标准驱动 / HVM 通道"), true);
            return;
        }

        const Channel channel = m_controller != nullptr ? m_controller->CurrentChannel() : Channel::UserMode;
        switch (outcome.status)
        {
        case InstallStatus::Installed:
            emit resultMessage(QStringLiteral("已写入 int3（%1）").arg(FormatAddress(address)), false);
            return;
        case InstallStatus::Duplicate:
            emit resultMessage(QStringLiteral("该地址已经写过 int3，无需重复写入"), true);
            return;
        case InstallStatus::ReadFailed:
            emit resultMessage(QStringLiteral("写入前读取原字节失败，目标可能不可读") + ChannelGuidance(channel), true);
            return;
        case InstallStatus::AlreadyContainsPatchByte:
            emit resultMessage(QStringLiteral("该地址原本就是 0xCC，无法安全记账，已拒绝写入"), true);
            return;
        case InstallStatus::WriteFailed:
            emit resultMessage(QStringLiteral("写入失败，目标可能不可写") + ChannelGuidance(channel), true);
            return;
        case InstallStatus::VerifyFailed:
            emit resultMessage(
                outcome.rollbackAttempted
                    ? (outcome.rollbackWriteOk
                           ? QStringLiteral("写入后回读不符，已尝试写回原字节")
                           : QStringLiteral("写入后回读不符，写回原字节也失败，请立即检查目标"))
                    : QStringLiteral("写入后回读不符"),
                true);
            return;
        case InstallStatus::None:
        default:
            emit resultMessage(QStringLiteral("写入未执行"), true);
            return;
        }
    }

    // EmitRestoreMessage：把 Restore 的结果翻译成中文提示；Diverged 时记入 m_divergedIds
    // 并重建表格以显示警示态与 tooltip。
    void Int3PatchPanel::EmitRestoreMessage(const Int3RestoreOutcome& outcome, const std::uint64_t id)
    {
        const Channel channel =
            m_controller != nullptr && m_controller->InstalledChannel(id).has_value()
                ? *m_controller->InstalledChannel(id)
                : Channel::UserMode;

        switch (outcome.status)
        {
        case RestoreStatus::Restored:
            emit resultMessage(QStringLiteral("已还原"), false);
            return;
        case RestoreStatus::NotFound:
            emit resultMessage(QStringLiteral("未找到该记录，可能已被还原或丢弃"), true);
            return;
        case RestoreStatus::Orphaned:
            emit resultMessage(QStringLiteral("目标已退出，无法还原，请使用\"清除\""), true);
            return;
        case RestoreStatus::TargetMismatch:
            emit resultMessage(QStringLiteral("当前目标与这条记录的目标不一致，拒绝还原"), true);
            return;
        case RestoreStatus::ReadFailed:
            emit resultMessage(QStringLiteral("读取当前字节失败") + ChannelGuidance(channel), true);
            return;
        case RestoreStatus::Diverged:
        {
            const QPointer<Int3PatchPanel> self(this);
            m_divergedIds.insert(id);
            rebuildTable();
            if (!self)
            {
                return;
            }
            emit resultMessage(QStringLiteral("当前字节已不是 CC（被别处改过），仅可丢弃记录"), true);
            return;
        }
        case RestoreStatus::WriteFailed:
            emit resultMessage(QStringLiteral("写回失败") + ChannelGuidance(channel), true);
            return;
        case RestoreStatus::VerifyFailed:
            emit resultMessage(QStringLiteral("写回后回读不符") + ChannelGuidance(channel), true);
            return;
        case RestoreStatus::None:
        default:
            emit resultMessage(QStringLiteral("还原未执行"), true);
            return;
        }
    }
}
