#include "MemoryDock.Internal.h"
#include "MemoryDock.WorkbenchServices.h"

#include "../Internationalization/LanguageManager.h"
#include "../UI/MemoryWorkbench/MemoryWorkbenchView.h"
#include "../UI/MemoryWorkbench/WorkbenchNavigation.h"
#include "../UI/MemoryWorkbench/WorkbenchSettings.h"
#include "../UI/MemoryWorkbench/WorkbenchTarget.h"

// ============================================================
// MemoryDock.Workbench.cpp
// 作用：
// - 内存工作台（UI/MemoryWorkbench/ 的 MemoryWorkbenchView）在 MemoryDock 里的接线（波 4，3a 并存）：
//   1) 在页签栏插入"内存工作台"页签，视图懒创建（首次切到该页签才创建，不拖慢 Dock 构造）；
//   2) 创建视图时按 MemoryDock.WorkbenchServices.h 的"接线步骤速查"注入全部生产服务；
//   3) 把 Dock 的附加/分离转给视图的 WorkbenchTarget（三个钩子）；
//   4) 统一的"跳到地址"分发器 jumpToAddress：routeJumps 为假（默认）或内嵌窗口里走旧路径；
//   5) 主窗口关闭前的退出守卫入口 confirmWorkbenchQuit。
// - 3a 阶段旧页签的行为一字不差：本文件只新增页签与分发器，不改任何旧页签的数据流。
// ============================================================

namespace
{
    // kWorkbenchTabIndex：内存工作台页签的插入位置（内存搜索之后、旧内存查看器之前）。
    constexpr int kWorkbenchTabIndex = 3;
}

// initializeWorkbenchTab：在 initializeTabs 的图标循环之后调用一次。
// 只建一个空容器页并插入页签，视图本身懒创建；内嵌窗口里这个页签随 setProcessDetailMemoryScope
// 的可见集被隐藏，视图永不创建。
void MemoryDock::initializeWorkbenchTab()
{
    // 整体开关（"enabled" 键，默认开）：关掉就不插页签，也没有视图、没有分发器路由——
    // 这是 3a 的无需发版回退开关。m_tabWorkbench 保持空指针，分发器据此回退旧路径。
    if (!ks::ui::workbench_settings::LoadEnabled())
    {
        return;
    }

    // 容器页：垂直布局、无边距，视图创建后铺满。
    m_tabWorkbench = new QWidget(m_tabWidget);
    QVBoxLayout* const containerLayout = new QVBoxLayout(m_tabWorkbench);
    containerLayout->setContentsMargins(0, 0, 0, 0);
    containerLayout->setSpacing(0);

    // 插入页签并单独设置图标（图标循环按原有页签下标写死，不包含这一页）。
    const int insertedIndex = m_tabWidget->insertTab(
        kWorkbenchTabIndex, m_tabWorkbench, QStringLiteral("内存工作台"));
    m_tabWidget->setTabIcon(insertedIndex, QIcon(QStringLiteral(":/Icon/memwb_tab_hex.svg")));
    ks::i18n::LanguageManager::instance().bindTab(
        m_tabWidget, m_tabWorkbench,
        QStringLiteral("memory.tab.workbench"), QStringLiteral("内存工作台"));

    // 旧入口的跳转是否交给工作台：读持久设置（默认假，3b 才改默认值）。
    m_workbenchRouteJumps = ks::ui::workbench_settings::LoadRouteJumps();

    // 首次切到该页签时创建视图；ensureWorkbenchView 幂等，后续切换不再创建。
    connect(m_tabWidget, &QTabWidget::currentChanged, this, [this](int) {
        if (m_tabWidget != nullptr && m_tabWidget->currentWidget() == m_tabWorkbench)
        {
            ensureWorkbenchView();
        }
    });
}

// ensureWorkbenchView：幂等地创建视图并完成全部接线。
// 顺序有要求：先 ConfigureShared（必须在第一个视图之前，且此前不得有人访问共享地址簿），
// 再创建视图、注入服务、加载设置，最后连接信号并回放"创建之前已经发生的附加"。
void MemoryDock::ensureWorkbenchView()
{
    // 已创建、内嵌实例（不创建）或容器页不存在时直接返回。
    if (m_workbenchView != nullptr || m_workbenchEmbedded || m_tabWorkbench == nullptr)
    {
        return;
    }

    // 1) 共享对象配置（幂等；被拒绝时它自己写 err 日志，不抛异常）。
    ks::ui::workbench_dock::ConfigureShared();

    // 2) 创建视图并铺满容器页。
    m_workbenchView = new ks::ui::MemoryWorkbenchView(m_tabWorkbench);
    m_tabWorkbench->layout()->addWidget(m_workbenchView);
    // view：lambda 捕获用的视图裸指针；视图是本 Dock 的子对象，生命周期不长于 Dock。
    ks::ui::MemoryWorkbenchView* const view = m_workbenchView;

    // 3) 注入生产服务（MemoryDock.WorkbenchServices.h 的接线步骤速查逐条落实）。
    view->setDisasmBackends(
        ks::ui::workbench_dock::MakeDecodeBackend(),
        ks::ui::workbench_dock::MakeAssembleBackend());
    view->setGlobalSkipDangerousConfirmProvider(&ks::ui::workbench_dock::GlobalSkipDangerousConfirm);
    view->setAttachedProcessInfoProvider(&ks::ui::workbench_dock::QueryAttachedProcessInfo);
    // 保护段：视图的回调只带地址，pid 取视图当前会话；内核/物理范围下 pid 恒为 0，返回空。
    view->setProtectionProvider([view](const std::uint64_t address) {
        return ks::ui::workbench_dock::QueryProtection(view->target().session().pid, address);
    });
    // 通道可用性输入：QueryGateInputs 不知道当前目标，hasProcessTarget 必须在这里补上，
    // 否则进程范围下所有通道都被判"需要进程"，页读取全部被取消。
    view->setGateInputsProvider([view]() {
        ksword::memwb::GateInputs inputs = ks::ui::workbench_dock::QueryGateInputs();
        inputs.hasProcessTarget = view->target().session().pid != 0;
        return inputs;
    });

    // 4) 主 Dock 的视图是设置权威（只有它落盘）；注入完成后再加载一次设置。
    view->setSettingsAuthoritative(true);
    view->loadSettings();

    // 5) 信号接线（全部用 lambda，不新增 Qt 槽函数）。
    // 5a) 用户点了目标 chip 的"在 Dock 附加…"：切到进程与模块页并让进程下拉框获得焦点。
    connect(view, &ks::ui::MemoryWorkbenchView::pickTargetRequested, this, [this]() {
        if (m_tabWidget != nullptr && m_tabProcessModule != nullptr)
        {
            m_tabWidget->setCurrentWidget(m_tabProcessModule);
        }
        if (m_processCombo != nullptr)
        {
            m_processCombo->setFocus();
        }
    });
    // 5b) 写入失败的原文：交给既有的提权提示（它自己判断是不是权限问题、已提权时返回 false）。
    connect(view, &ks::ui::MemoryWorkbenchView::writeFailureText, this, [this](const QString& failureText) {
        (void)ks::ui::promptForPrivilegeFailure(this, QStringLiteral("内存工作台写入"), failureText);
    });
    // 5c) 跳转被拒绝：状态条已经报告原因，这里只留一条日志，不再弹窗。
    connect(view, &ks::ui::MemoryWorkbenchView::navigationRefused, this, [](const ks::ui::NavStatus status) {
        kLogEvent refusedEvent;
        warn << refusedEvent
            << "[MemoryDock] 内存工作台拒绝了一次跳转, status="
            << static_cast<int>(status)
            << eol;
    });

    // 6) 回放创建之前已经发生的附加：否则视图看不到"早已附加"的进程。
    if (m_attachedPid != 0 && m_attachedProcessHandle != nullptr)
    {
        workbenchOnAttached();
    }

    kLogEvent createdEvent;
    info << createdEvent
        << "[MemoryDock] 内存工作台视图已创建。"
        << eol;
}

// workbenchOnAttached：附加成功之后调用（句柄、PID、名称、读写标志都已就位）。
// 视图尚未创建时是空操作——创建时由 ensureWorkbenchView 回放。
void MemoryDock::workbenchOnAttached()
{
    if (m_workbenchView == nullptr)
    {
        return;
    }
    // attach：Dock 当前附加的快照；句柄只供视图复制，不会被写入或关闭。
    ks::ui::WorkbenchTarget::DockAttach attach;
    attach.handle = m_attachedProcessHandle;
    attach.pid = m_attachedPid;
    attach.name = m_attachedProcessName;
    attach.attachGeneration = m_processAttachmentGeneration.load();
    attach.hintReadOnly = !m_canReadWriteMemory;
    m_workbenchView->target().onDockAttached(attach);
}

// workbenchOnAboutToDetach：detachProcess 最开头（句柄关闭之前）调用。
void MemoryDock::workbenchOnAboutToDetach()
{
    if (m_workbenchView != nullptr)
    {
        m_workbenchView->target().onDockAboutToDetach();
    }
}

// workbenchOnDetached：detachProcess 最末（旧上下文已清零）调用。
void MemoryDock::workbenchOnDetached()
{
    if (m_workbenchView != nullptr)
    {
        m_workbenchView->target().onDockDetached();
    }
}

// workbenchAllowsProcessChange：附加/分离之前问一次工作台的离开守卫。
// 视图尚未创建时没有任何东西要问；创建后守卫会按需弹暂存三选一与 int3 三选一。
bool MemoryDock::workbenchAllowsProcessChange()
{
    if (m_workbenchView == nullptr)
    {
        return true;
    }
    // 工作台钉在别的进程/内核/物理范围时，Dock 的附加或分离与它的会话无关，不该为此打扰用户：
    // 只有"跟随 Dock 且范围为进程"这类确实会改变会话的情形才问守卫。
    if (!m_workbenchView->target().wouldChangeOnDockAttach())
    {
        return true;
    }
    return m_workbenchView->requestLeave(ks::ui::LeaveReason::DockAttachChange);
}

// confirmWorkbenchQuit：MainWindow::closeEvent 最前调用。
bool MemoryDock::confirmWorkbenchQuit()
{
    if (m_workbenchView == nullptr)
    {
        return true;
    }
    return m_workbenchView->confirmQuit();
}

// shutdownWorkbench：析构路径上先于子对象销毁调用——权威视图把设置落盘。
void MemoryDock::shutdownWorkbench()
{
    if (m_workbenchView != nullptr)
    {
        m_workbenchView->saveSettings();
    }
}

// navigateWorkbench：确保视图存在并切到该页签，再执行一次跳转。
// 传入：request 跳转请求；返回：true=跳转成功（NavStatus::Ok），false=视图不可用或被拒绝
// （拒绝原因已由视图状态条报告）。
bool MemoryDock::navigateWorkbench(const ks::ui::NavRequest& request)
{
    // 内嵌实例没有工作台；容器页缺失同样不可用。
    if (m_workbenchEmbedded || m_tabWorkbench == nullptr || m_tabWidget == nullptr)
    {
        return false;
    }
    ensureWorkbenchView();
    if (m_workbenchView == nullptr)
    {
        return false;
    }
    // 先切页签（会触发 currentChanged，但视图已存在，ensureWorkbenchView 是空操作）再跳转。
    m_tabWidget->setCurrentWidget(m_tabWorkbench);
    return m_workbenchView->openAt(request) == ks::ui::NavStatus::Ok;
}

// jumpToAddress：统一的"跳到地址"分发器。
// - routeJumps 为假（默认）、内嵌窗口、或工作台不可用：走旧内存查看器（行为与改动前完全相同）；
// - routeJumps 为真：交给工作台。此时跳转被拒绝不回退旧页（原因已在状态条里），3a 阶段不会走到。
void MemoryDock::jumpToAddress(const std::uint64_t address)
{
    if (!m_workbenchRouteJumps || m_workbenchEmbedded || m_tabWorkbench == nullptr)
    {
        jumpToAddressLegacy(address);
        return;
    }
    // request：来源标为外部调用（旧入口分发器不区分具体来源页），范围沿用当前会话。
    ks::ui::NavRequest request;
    request.address = address;
    request.origin = ks::ui::NavOrigin::External;
    ensureWorkbenchView();
    if (m_workbenchView == nullptr)
    {
        // 视图创建失败（理论上不会发生）：回退旧路径，保证用户仍能跳转。
        jumpToAddressLegacy(address);
        return;
    }
    request.scope = m_workbenchView->target().session().scope;
    (void)navigateWorkbench(request);
}
