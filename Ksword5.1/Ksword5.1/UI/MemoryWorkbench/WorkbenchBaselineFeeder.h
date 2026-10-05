#pragma once

// ============================================================
// WorkbenchBaselineFeeder.h
// 作用：
// - "基线窗口"喂入器（见 docs/内存工作台Phase3集成设计.md §1"基线窗口"一节、
//   docs/内存工作台Phase3装配接口.md §2）：围绕插入点选取"已落定页"组成的连续跨度
//   （ksword::memwb::SelectBaselineSpan），从画布的页缓存里原样拷出字节
//   （HexCanvas::copyCachedRangeWithMask，不叠加暂存补丁），喂给
//   MemoryDiffOverlay::RefreshBaseline；首次载入/换目标走 LoadBaseline，由
//   WorkbenchHexPane 在换目标时直接调用 overlay（不经本类），本类只负责"之后"的
//   跟随与重喂。
// - 防抖：仅在"脏（窗口或来源代次变化）且当前没有在途页读取请求"时，延迟 50 ms
//   触发一次重算/重喂；非脏不重建——否则 SelfWritten 标记每次都会被清掉，青色
//   高亮也会被反复打断（设计文档 §1 第 2 条）。
// - 本类不持有叠加层的所有权（唯一一份由 WorkbenchHexPane 持有并转交写事务/画布/
//   反汇编等子页使用），只持非拥有指针；本类自己永远不写目标内存、不做任何网络/
//   驱动 I/O，只读画布已经缓存好的字节。
// ------------------------------------------------------------
// HexCanvas::settledPageStartsInRange 已经落地（审核修复批次更新）
// ------------------------------------------------------------
// - 之前这里写的是"假定 HexCanvas 会新增"的占位说明；该方法现已由 HexCanvas 实现
//   （见 HexCanvas.h"四之四"小节），语义与本类的调用方式都已对齐，不再是缺口。
// ------------------------------------------------------------
// 挂起入口 setSuspended（独立审核 D3 修复）
// ------------------------------------------------------------
// - 写事务的确认框/审计弹窗是嵌套事件循环，期间页回填仍会照常到达并可能触发 LRU
//   挤页，若本类此时照常重算，会用挤页后的旧页内容覆盖刚被 AcceptWrite 标记为
//   "自己写入"的基线字节（独立审核报告 D3，探针 Q4）。装配层（WorkbenchHexPane/
//   WorkbenchWriteController）必须在进入这类嵌套事件循环前调用
//   setSuspended(true)，循环退出后调用 setSuspended(false)。
// ============================================================

#include "HexCanvas.h"

#include "../../../../shared/evidence/memory_workbench/MemoryBaselineWindow.h"
#include "../../../../shared/evidence/memory_workbench/MemoryDiffOverlay.h"

#include <QObject>
#include <QPointer>

#include <cstdint>
#include <set>
#include <string>

class QTimer;

namespace ks::ui
{
    // WorkbenchBaselineFeeder：基线窗口喂入器，详见文件头。全部公开函数只在 UI 线程
    // 调用。
    class WorkbenchBaselineFeeder final : public QObject
    {
        Q_OBJECT

    public:
        explicit WorkbenchBaselineFeeder(QObject* parent = nullptr);
        ~WorkbenchBaselineFeeder() override;

        // setCanvas / setOverlay：非拥有指针；任一为空时 noteDirty/flushNow 直接忽略
        // （不崩溃，也不产生任何喂入）。
        void setCanvas(HexCanvas* canvas);
        void setOverlay(ksword::memwb::MemoryDiffOverlay* overlay);

        // setPolicy：页大小与窗口页数上限。默认取 Core 的暂定常量
        // （kBaselineWindowDefaultPageSize/kBaselineWindowDefaultMaxPages）；
        // 渲染基准确认 256 页是否合适之后，由 WorkbenchSettings 持久化并在此调用。
        void setPolicy(const ksword::memwb::BaselineWindowPolicy& policy);

        // setAddressSpaceBounds：当前会话范围对应的地址空间闭区间（随换范围/换目标
        // 同步更新，取自 ksword::memwb::SessionAddressResolver::ScopeAddressSpace 或
        // 等价计算）。换边界通常意味着换目标：同 setIdentityKey，会取消已起算的
        // 防抖定时器并清空待处理的锚点/代次（独立审核报告 D4），并复位
        // pendingInFlight_（第二轮审核报告 N1：旧目标的在途位对新目标没有意义）。
        void setAddressSpaceBounds(const ksword::memwb::AddressSpaceBounds& bounds);

        // setIdentityKey：换目标/换范围/换通道时调用，更新本类内部记住的
        // identityKey（必须与 overlay 当前基线的 identityKey 规则一致）。调用方须
        // 保证：identityKey 变化之前或同一时刻，overlay 已经完成一次 LoadBaseline
        // （本类不替宿主载入首个基线，只负责之后的跟随重喂，见文件头）。
        // 身份真的变化时（与上次不同）：除了让旧窗口记录失效，还会取消已起算的
        // 防抖定时器并清空待处理的锚点/代次——旧目标的待处理请求对新目标没有意义，
        // 不清空会让 flushNow 或旧定时器到期时用旧目标的锚点在新目标上误判（独立
        // 审核报告 D4，探针 P4a/P4b/Q6），并复位 pendingInFlight_（第二轮审核报告
        // N1：旧目标的在途位对新目标没有意义）。
        void setIdentityKey(const std::string& identityKey);

        // noteDirty：告知"窗口或来源代次可能需要重算"（视口滚动、页回填、重读、
        // 插入点移动、地址空间/策略变化均应调用）。
        // 传入：anchorAddress 当前锚点（插入点或视口中心）；currentSourceRevision
        //       当前来源代次；hasInFlightRequests 当前是否存在未完成的页读取请求——
        //       为真时本次不计时，等下一次 hasInFlightRequests=false 的 noteDirty
        //       到来才重新起算防抖（在途页暂不落定，提前判定会误判为"需要重算"）。
        // 独立审核 D1 修复：hasInFlightRequests 为真时，除了不新起算，还会取消此前
        // 已经起算、尚未到期的那次防抖定时器——否则它到期时仍会按抢跑前的旧锚点/旧
        // 代次重算，把还没读完的窗口错误地判成"已经是最终结果"，基线从此停在陈旧
        // 字节上（独立审核报告 D1，探针 Q1/Q2）。isSuspended() 为真时同样不计时。
        // 独立审核第二轮 N1/N2 修复：本方法每次都会记下"这一次调用的在途位"
        // （pendingInFlight_），供 setSuspended(false) 恢复时判断——如果最后一次
        // noteDirty 仍是在途（hasInFlightRequests=true），恢复时绝不能凭旧缓存起算，
        // 必须继续等真正不在途的 noteDirty（第二轮审核报告 N1，探针 Q1/R2a/R2b）。
        // 本方法若恰好发生在挂起期间，还会把 dirtySinceSuspend_ 置真——标记"挂起期间
        // 确实发生过一次需要补回的脏事件"，供恢复时判断是否应该重新起算（第二轮审核
        // 报告 N2）。
        void noteDirty(
            std::uint64_t anchorAddress,
            std::uint64_t currentSourceRevision,
            bool hasInFlightRequests);

        // flushNow：跳过防抖立即执行一次（测试断言，以及"立即需要基线"的场景，例如
        // 写前复核前想确保窗口已经覆盖目标范围）。
        // 独立审核 D4 修复：没有任何有效的待处理请求时（从未调用过 noteDirty，或刚
        // 经历过 setAddressSpaceBounds/setIdentityKey 换目标而被清空）是空操作，不
        // 会再使用没有意义的默认锚点/代次 0 去判定——那会在从未请求读取的地址上报出
        // 一个虚假的"该处尚未读取"（独立审核报告 D4，探针 P4a/P4b/Q6）。
        // 独立审核第二轮 N3 修复：挂起期间同样是空操作——不受挂起门控会让"写前复核"
        // 这类典型调用场景（本身就发生在写事务挂起期间）绕过 setSuspended，照常用
        // 挤页后的旧内容重算并覆盖刚确认的基线字节（第二轮审核报告 N3，探针 R3/Q2）。
        // 调用方此次的待处理请求（若有）已经由 noteDirty 记下，不会因为这里提前返回
        // 而丢失，恢复后会按 setSuspended 的恢复规则自然补上。
        void flushNow();

        // setSuspended：独立审核 D3 新增的挂起入口。挂起期间 noteDirty 只记录待处理
        // 请求（含 pendingInFlight_/dirtySinceSuspend_ 两个判断位）、不起算也不触发
        // 任何重算；已经起算的防抖定时器会被取消。
        // 独立审核第二轮 N1/N2 修复：恢复（suspended=false）时不再无条件按
        // pendingValid_ 起算，而是同时核对 pendingInFlight_（最后一次 noteDirty 是否
        // 仍在途——在途就必须继续等，不能凭旧缓存重算，否则 D1 的后果经恢复路径复发，
        // 第二轮审核报告 N1）与 dirtySinceSuspend_（挂起期间是否确实发生过需要补回的
        // 脏事件，或挂起那一刻已经有一次起算被取消——没有则是纯粹的"挂起又恢复、什么
        // 都没变"，不应该凭空产生一次判定，否则挤页后的旧内容会在恢复后覆盖刚确认的
        // 基线字节，第二轮审核报告 N2）。两项都满足才按正常防抖规则重新起算一次。
        // 装配层（WorkbenchWriteController）在写事务的确认框/审计这类嵌套事件循环
        // 期间调用 setSuspended(true)，循环结束后调用 setSuspended(false)——否则
        // 循环中途到达的页回填会触发 LRU 挤页与重算，用挤页后的旧内容覆盖刚被
        // AcceptWrite 标记为"自己写入"的基线字节（独立审核报告 D3，探针 Q4）。
        // 残留风险（第二轮审核报告 N2 的限制）：如果挂起期间最后一次 noteDirty 恰好
        // 是"不在途"（例如局部重读已经登记完成），本类会在恢复后正常起算并重算一次；
        // 若那次重读其实还没真正开始（装配层过早调用了 noteDirty(false)），本类无法
        // 从接口层面分辨，需要装配层保证调用顺序——这部分不在本包范围内，见修复说明。
        void setSuspended(bool suspended);

        // isSuspended：当前是否处于挂起状态，供诊断与夹具断言只读查询。
        bool isSuspended() const noexcept;

        // lastWindow：最近一次成功喂入的窗口记录，供诊断与夹具断言只读查询。
        const ksword::memwb::BaselineWindowRecord& lastWindow() const noexcept;

    signals:
        // baselineRefreshed：一次 RefreshBaseline/RefreshSameSpan 成功后发出；装配层
        // 据此调用 canvas_->notifyOverlayChanged()（画布不知道叠加层被绕过改了基线，
        // 见 HexCanvas.h"四之二"）。
        void baselineRefreshed(quint64 baseAddress, quint64 length);

        // baselineUnavailable：插入点所在页尚未落定（InsertionPageNotSettled）或超出
        // 地址空间（InsertionOutsideSpace/InvalidArgument）时发出，调用方据此在状态条
        // 提示"该处尚未读取"，这不是错误。
        void baselineUnavailable(ksword::memwb::BaselineSpanStatus status);

    private slots:
        // onDebounceTimeout：50 ms 单次防抖到期，调用 recomputeAndFeed。
        void onDebounceTimeout();

    private:
        // recomputeAndFeed：按 DecideBaselineRefeed 的判定结果执行 Keep/
        // RefreshSameSpan/Recompute 三者之一，成功时更新 record_ 并发
        // baselineRefreshed；被 flushNow 与 onDebounceTimeout 共用。
        // 独立审核 D2 修复：判定之前先核对 record_ 与叠加层*当前真实基线*
        // （BaseAddress/BaselineSize/IdentityKey）是否一致；只要有一项不符（宿主绕过
        // 本类直接调用了 overlay_->LoadBaseline，或换了一个全新的 overlay 对象），
        // 就把 record_ 当作失效处理（Reset），强制走 Recompute 重新对齐，不会对着一
        // 份自己已经不知情的旧窗口返回 Keep（独立审核报告 D2，探针 P9/Q3a）。
        void recomputeAndFeed();

        // settledPageStartsInRange：见文件头"需要底座/组件新增"一节。
        std::set<std::uint64_t> settledPageStartsInRange(
            std::uint64_t firstAddress,
            std::uint64_t lastAddress) const;

        // canvas_ / overlay_：均非拥有；overlay_ 唯一一份由 WorkbenchHexPane 持有。
        QPointer<HexCanvas> canvas_;
        ksword::memwb::MemoryDiffOverlay* overlay_ = nullptr;
        // policy_ / bounds_：窗口选取参数与当前地址空间。
        ksword::memwb::BaselineWindowPolicy policy_;
        ksword::memwb::AddressSpaceBounds bounds_;
        // record_：上一次成功喂入的窗口记录（DecideBaselineRefeed 的输入之一）。
        ksword::memwb::BaselineWindowRecord record_;
        // identityKey_：当前基线身份串，随 setIdentityKey 更新。
        std::string identityKey_;
        // debounceTimer_：50 ms 单次防抖定时器，在构造函数体内 new 出来（以 this 为
        // 父对象，是指针成员而不是内联值成员，所以放进构造函数体，不是"懒创建"到
        // 首次使用时才建）。
        QTimer* debounceTimer_ = nullptr;
        // pendingAnchor_ / pendingSourceRevision_ / pendingValid_：最近一次 noteDirty
        // 记下的待处理请求；hasInFlightRequests 或 suspended_ 为真时只更新这三项、
        // 不启动计时器。pendingValid_ 同时被 flushNow/recomputeAndFeed 读取（独立
        // 审核 D4 修复）：没有有效的待处理请求时整次判定直接跳过，不使用没有意义的
        // 默认锚点/代次 0；也被 setAddressSpaceBounds/setIdentityKey（换目标）与
        // setSuspended（挂起/恢复）清空或查询。
        std::uint64_t pendingAnchor_ = 0;
        std::uint64_t pendingSourceRevision_ = 0;
        bool pendingValid_ = false;
        // suspended_：独立审核 D3 新增的挂起开关，见 setSuspended 注释。
        bool suspended_ = false;
        // pendingInFlight_：独立审核第二轮 N1 新增，记录"最近一次 noteDirty 调用时
        // 传入的 hasInFlightRequests"。setSuspended(false) 恢复时据此判断——若最后
        // 一次 noteDirty 仍是在途，绝不能起算（否则会用还没读完的窗口凭旧缓存重算，
        // D1 的后果经恢复路径复发）。随每次 noteDirty 更新，setAddressSpaceBounds/
        // setIdentityKey 换目标时复位。
        bool pendingInFlight_ = false;
        // dirtySinceSuspend_：独立审核第二轮 N2 新增，记录"挂起期间是否发生过需要在
        // 恢复时补回的脏事件"。进入挂起（setSuspended(true)）那一刻，若恰好有一次
        // 已起算、被取消的防抖定时器，置真（那次起算的判定还没来得及执行，恢复后要
        // 补回）；挂起期间任何一次 noteDirty 也会置真。恢复时只有这一位为真才重新
        // 起算，纯粹"挂起又恢复、期间什么都没发生"不会凭空产生一次判定（否则挤页后
        // 的旧内容会在恢复后覆盖刚确认的基线字节）。
        bool dirtySinceSuspend_ = false;
    };
}
