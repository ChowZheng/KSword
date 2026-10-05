#pragma once

// ============================================================
// Int3PatchPanel.h
// 作用：
// - 侧栏下半的 int3 补丁面板（ux.md 第 4.4 节）：折叠头部"int3 补丁 (n)"、三个工具钮
//   （写入 / 还原 / 全部还原）、一张列表（地址 · 原字节 · 目标），孤立项单独分组带"清除"。
// - 本面板不持有 Int3Controller 的生命周期：调用方构造并传入指针，面板只连接它的 changed
//   信号、调用它的方法，析构时不删除它。
// - 全部按钮操作零确认（用户决策：int3 补丁零摩擦）；唯一的确认框是退出前的三选一，
//   由 Int3Controller::RequestLeave 负责，不在本面板内触发——本面板只提供"有多少待还原"
//   的展示，退出时机由外部（Dock 分离 / 切换目标 / 主窗口关闭）决定。
// - 地址的"模块+RVA"与目标的"进程名"展示依赖模块目录与进程身份（WP-B / WP-I），本工作包
//   尚未接入：面板接受可选的展示回调，缺省时退化为纯十六进制地址 / "PID <n>"文本，
//   接入时由后续工作包调用 SetAddressFormatter / SetTargetLabelFormatter 即可，不需要改
//   本文件。
// ============================================================

#include "Int3Controller.h"

#include <QWidget>

#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_set>

class QToolButton;
class QTableWidget;
class QLabel;
class QPoint;

namespace ks::ui
{
    // Int3PatchPanel：见文件头说明。
    class Int3PatchPanel final : public QWidget
    {
        Q_OBJECT
    public:
        // AddressFormatter：把地址格式化成"模块+RVA"之类更友好的文本；未注入时退化为十六进制。
        using AddressFormatter = std::function<QString(std::uint64_t address)>;
        // TargetLabelFormatter：把 (pid, 创建时间) 格式化成"进程名 · PID"之类的文本；
        // 未注入时退化为"PID <n>"。orphaned 为真时面板**不会**调用它——"已退出"是结构性事实，
        // 不依赖进程名查询。
        using TargetLabelFormatter = std::function<QString(std::uint32_t pid, std::uint64_t processCreateTime100ns)>;

        // 构造：controller 必须非空，生命周期由调用方管理（面板只读它、连它的信号）。
        explicit Int3PatchPanel(Int3Controller* controller, QWidget* parent = nullptr);

        // SetAddressFormatter / SetTargetLabelFormatter：注入展示回调；传空还原为默认格式化。
        void SetAddressFormatter(AddressFormatter formatter);
        void SetTargetLabelFormatter(TargetLabelFormatter formatter);

        // SetInsertionPoint：外部（HexPane）告知当前插入点地址。hasPoint 为假时"写入"按钮禁用
        // 并给出对应 tooltip；地址本身在 hasPoint 为假时不会被使用。
        void SetInsertionPoint(std::uint64_t address, bool hasPoint);

        // IsCollapsed / SetCollapsed：折叠状态查询与设置。首次写入成功后由调用方（Install 的
        // 结果处理处）调用 SetCollapsed(false) 实现"自动展开"；面板自身不监听 changed 信号来
        // 猜测是否应该展开，避免"每次还原也被动展开"这类意料外的联动。
        bool IsCollapsed() const { return m_collapsed; }
        void SetCollapsed(bool collapsed);

        // 以下四个访问器只供夹具/测试断言内部控件状态，生产代码不应依赖它们做业务判断
        // （业务状态一律以 Int3Controller 为准）。
        QTableWidget* TableForTest() const { return m_table; }
        QToolButton* InstallButtonForTest() const { return m_installButton; }
        QToolButton* RestoreButtonForTest() const { return m_restoreButton; }
        QToolButton* RestoreAllButtonForTest() const { return m_restoreAllButton; }
        QToolButton* HeaderForTest() const { return m_header; }

    signals:
        // resultMessage：一次操作结束后要交给状态条的文案；isError 为真时状态条应标红。
        void resultMessage(const QString& text, bool isError);

    private slots:
        // onLedgerChanged：响应 Int3Controller::changed，重建表格并刷新工具钮可用性。
        void onLedgerChanged();
        // onHeaderClicked：切换折叠状态。
        void onHeaderClicked();
        // onInstallClicked / onRestoreClicked / onRestoreAllClicked：三个工具钮各自的点击处理。
        void onInstallClicked();
        void onRestoreClicked();
        void onRestoreAllClicked();
        // onSelectionChanged：选中行变化时刷新"还原"按钮是否可用（Diverged 行不可还原）。
        void onSelectionChanged();
        // onTableContextMenuRequested：右键菜单——Diverged 行给"丢弃记录"，孤立分组标题行给
        // "清除"（对应 Int3Controller::ClearOrphaned，一次清空整组，账本本身不支持单条清除
        // 孤立项）。
        void onTableContextMenuRequested(const QPoint& pos);

    private:
        // buildUi：搭建折叠头部、工具栏、表格，一次性完成，构造函数里只调用它与连信号。
        void buildUi();
        // rebuildTable：按 Int3Controller::Entries()/OrphanedEntries() 重建全部行；
        // 同时清理 m_divergedIds 里已经不在待还原列表中的 id（已被还原或丢弃）。
        void rebuildTable();
        // updateHeaderText：按当前 Entries().size() 刷新头部文字"int3 补丁 (n)"。
        void updateHeaderText();
        // updateToolbarEnabled：按"是否有插入点""是否有选中行""选中行是否 Diverged""当前路由
        // 是否被拒绝"综合决定三个工具钮的可用性与 tooltip。
        void updateToolbarEnabled();
        // FormatAddress：注入了 m_addressFormatter 则用它，否则输出 0x 前缀十六进制。
        QString FormatAddress(std::uint64_t address) const;
        // FormatTarget：orphaned 为真恒返回"已退出"；否则注入了 m_targetLabelFormatter 则用它，
        // 否则输出"PID <n>"。
        QString FormatTarget(std::uint32_t pid, std::uint64_t processCreateTime100ns, bool orphaned) const;
        // SelectedActiveEntryId：当前选中行若是一条"待还原"条目（非分组标题、非孤立项）则
        // 返回其 id，否则返回 nullopt。
        std::optional<std::uint64_t> SelectedActiveEntryId() const;
        // EmitInstallMessage / EmitRestoreMessage：把账本结果翻译成中文提示并发 resultMessage；
        // Restore 一侧在 Diverged 时还会把 id 记入 m_divergedIds。
        void EmitInstallMessage(const Int3InstallOutcome& outcome, std::uint64_t address);
        void EmitRestoreMessage(const Int3RestoreOutcome& outcome, std::uint64_t id);
        // ChannelGuidance：channel 为标准驱动（R0）时追加"可切换到用户态通道重试"的引导语，
        // 否则返回空串。供 EmitInstallMessage / EmitRestoreMessage 在失败文案后面追加。
        QString ChannelGuidance(ksword::memwb::Channel channel) const;

        Int3Controller* m_controller = nullptr;      // 不持有生命周期
        AddressFormatter m_addressFormatter;          // 地址展示回调（可空）
        TargetLabelFormatter m_targetLabelFormatter;  // 目标展示回调（可空）
        std::optional<std::uint64_t> m_insertionPoint; // 当前插入点；无则 nullopt
        std::unordered_set<std::uint64_t> m_divergedIds; // 曾经还原得到 Diverged 的 id 集合

        QToolButton* m_header = nullptr;        // 折叠头部："int3 补丁 (n)"
        QWidget* m_body = nullptr;              // 折叠/展开的内容容器（工具栏 + 表格）
        QToolButton* m_installButton = nullptr; // 写入（插入点）
        QToolButton* m_restoreButton = nullptr; // 还原（选中行）
        QToolButton* m_restoreAllButton = nullptr; // 全部还原
        QTableWidget* m_table = nullptr;        // 地址 · 原字节 · 目标
        bool m_collapsed = true;                // 默认折叠，首次写入后由外部展开
    };
}
