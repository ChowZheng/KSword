// WorkbenchBaselineFeeder.cpp
// 作用：WorkbenchBaselineFeeder.h 的实现。设计动机、防抖规则、与 Core 两个纯函数
// （SelectBaselineSpan/DecideBaselineRefeed）的配合方式都写在该头文件顶部注释里，
// 本文件只实现、不重复抄写那些说明。
//
// 文件结构：构造/析构 -> 装配点 set* -> noteDirty/flushNow（驱动防抖） ->
// setSuspended/isSuspended（独立审核 D3 新增的挂起入口） ->
// onDebounceTimeout（定时器槛） -> recomputeAndFeed（核心判定与喂入） ->
// settledPageStartsInRange（私有，包一层画布判空）-> lastWindow。

#include "WorkbenchBaselineFeeder.h"

#include <QTimer>
#include <QtGlobal>

namespace ks::ui
{
    // 构造：50 ms 单次防抖定时器直接在构造函数体内创建（以 this 为父对象，随本对象
    // 销毁时由 Qt 的父子关系一并释放，不需要在析构函数里手动 delete）。
    // 传入：parent 父对象（可为空）。
    WorkbenchBaselineFeeder::WorkbenchBaselineFeeder(QObject* parent)
        : QObject(parent)
    {
        debounceTimer_ = new QTimer(this);
        debounceTimer_->setSingleShot(true);
        debounceTimer_->setInterval(50);
        connect(debounceTimer_, &QTimer::timeout, this, &WorkbenchBaselineFeeder::onDebounceTimeout);
    }

    // 析构：debounceTimer_ 是 QObject 子对象，由基类析构阶段统一释放；本类自己没有
    // 其它需要手动清理的资源（canvas_/overlay_ 都是非拥有指针）。
    WorkbenchBaselineFeeder::~WorkbenchBaselineFeeder() = default;

    // ======================== 装配点 ========================

    // setCanvas：非拥有指针，只记录不管理生命周期。QPointer 在画布被外部销毁时自动
    // 置空，后续 noteDirty/flushNow/recomputeAndFeed 的判空读到的都是最新状态，不会
    // 解引用悬空指针。
    void WorkbenchBaselineFeeder::setCanvas(HexCanvas* canvas)
    {
        canvas_ = canvas;
    }

    // setOverlay：同样是非拥有指针；叠加层唯一一份由 WorkbenchHexPane 持有，本类只
    // 拿来调用 RefreshBaseline，从不构造、也从不释放它。
    void WorkbenchBaselineFeeder::setOverlay(ksword::memwb::MemoryDiffOverlay* overlay)
    {
        overlay_ = overlay;
    }

    // setPolicy：更新窗口选取策略（页大小/页数上限）。
    // 旧记录是按旧策略选出来的，换了策略后可能已经不满足新策略（例如窗口页数超过
    // 新的上限），任务书"设置/策略/地址空间边界变化要使 record 失效"——这里直接清空
    // 记录，强制下一次 noteDirty/flushNow 判定时走 Recompute，不会沿用一个按旧策略
    // 选出、现在已经不自洽的窗口。
    //
    // 独立审核 S2 诊断：画布的页缓存粒度固定是 HexViewport::kPageBytes（4096），
    // settledPageStartsInRange 按这个粒度枚举；如果调用方传入不一致的 pageSize，
    // SelectBaselineSpan/DecideBaselineRefeed 的页序号换算会与画布实际缓存的页错位
    // （例如 pageSize=8192 时只核对到画布一半页的落定状态），且这个错位目前完全
    // 静默——这里只做诊断告警，不改变任何行为（调用方传入的 policy 仍然原样生效），
    // 方便装配期排查，不替调用方做决定。
    // 独立审核第二轮报告 S5 措辞纠正：上一句曾写"一次性诊断告警"，容易被读成
    // "只在第一次调用时告警"，但本方法没有任何"只警一次"的状态——每次调用传入
    // 不一致的 pageSize 都会各自告警一次（连续调用 3 次错误策略就是 3 条独立警
    // 告），这是故意的（调用方每次都应该看到最新一次配置的诊断，不应该因为"之
    // 前警过一次"就把后面的错误配置静默掉）；"一次性"原意是指"每次调用里最多
    // 做一次判断、不会同一次调用里重复告警"，现改写措辞避免这种歧义。
    void WorkbenchBaselineFeeder::setPolicy(const ksword::memwb::BaselineWindowPolicy& policy)
    {
        if (policy.pageSize != ksword::memwb::HexViewport::kPageBytes)
        {
            qWarning(
                "WorkbenchBaselineFeeder::setPolicy: pageSize=%llu 与画布页缓存粒度 %llu 不一致，"
                "已落定页查询会按画布的粒度核对，可能与本策略的页边界错位",
                static_cast<unsigned long long>(policy.pageSize),
                static_cast<unsigned long long>(ksword::memwb::HexViewport::kPageBytes));
        }
        policy_ = policy;
        record_.Reset();
    }

    // setAddressSpaceBounds：更新当前会话范围对应的地址空间闭区间。
    // 理由同 setPolicy：换范围/换边界后旧窗口可能已经越界（不再是"整页都落在边界内"
    // 的合法页），同样清空记录，不沿用旧边界下选出的窗口。
    //
    // 独立审核 D4 修复：换边界通常对应换目标（旧目标的待处理锚点/代次对新目标毫无
    // 意义）。除了清空 record_，这里还要取消已经起算、尚未到期的防抖定时器，并清空
    // pendingAnchor_/pendingSourceRevision_/pendingValid_——否则旧定时器到期时会用
    // 旧目标的锚点在新边界上误判（可能报出虚假的"该处尚未读取"），或者调用方紧接着
    // flushNow 时同样会被旧锚点带偏（独立审核报告 D4，探针 P4a/P4b/Q6）。
    void WorkbenchBaselineFeeder::setAddressSpaceBounds(const ksword::memwb::AddressSpaceBounds& bounds)
    {
        bounds_ = bounds;
        record_.Reset();
        debounceTimer_->stop();
        pendingValid_ = false;
        pendingAnchor_ = 0;
        pendingSourceRevision_ = 0;
        // 第二轮审核报告 N1：旧目标记下的在途位对新目标没有意义，一并复位，避免换
        // 目标后第一次 noteDirty 之前，残留的旧在途位意外影响任何判断。
        pendingInFlight_ = false;
    }

    // setIdentityKey：换目标/换范围/换通道时调用。
    // 传入：identityKey 新的身份串。
    void WorkbenchBaselineFeeder::setIdentityKey(const std::string& identityKey)
    {
        if (identityKey_ == identityKey)
        {
            // 身份没变：不打断任何正在进行的窗口记录（例如同一目标内的普通滚动触发的
            // noteDirty 序列），什么都不做。
            return;
        }
        identityKey_ = identityKey;

        // identityKey 变化后下一次必须是 RefreshSameSpan 或 Recompute，不得对旧身份的
        // 窗口直接续喂（装配接口文档 §7 对本类的关键判断）：把 record_ 清空，让
        // DecideBaselineRefeed 第一步就判定"无窗口"从而走 Recompute。调用方（本类头
        // 文件 setIdentityKey 注释）已经保证 overlay 在此之前或此刻已经完成一次
        // LoadBaseline，本类不负责载入首个基线，只需要保证自己的记录不跨身份续用。
        record_.Reset();

        // 独立审核 D4 修复：身份变化同样通常意味着换目标，理由与 setAddressSpaceBounds
        // 完全一致——取消已起算的防抖定时器，清空待处理的旧目标锚点/代次。
        debounceTimer_->stop();
        pendingValid_ = false;
        pendingAnchor_ = 0;
        pendingSourceRevision_ = 0;
        // 第二轮审核报告 N1：旧目标记下的在途位对新目标没有意义，一并复位。
        pendingInFlight_ = false;
    }

    // ======================== 脏事件与防抖 ========================

    // noteDirty：记录一次"窗口可能需要重算"的脏事件，决定是否起算 50 ms 防抖。
    // 传入：anchorAddress 当前锚点；currentSourceRevision 当前来源代次；
    //       hasInFlightRequests 当前是否存在未完成的页读取请求。
    void WorkbenchBaselineFeeder::noteDirty(
        std::uint64_t anchorAddress,
        std::uint64_t currentSourceRevision,
        bool hasInFlightRequests)
    {
        // 画布或叠加层任一为空时整个方法是空操作：既不记录待处理请求，也不启动计时
        // 器（头文件 setCanvas/setOverlay 注释："任一为空时 noteDirty/flushNow 直接
        // 忽略，不崩溃，也不产生任何喂入"）。调用方（WorkbenchHexPane）因此不需要
        // 自己先判空再决定要不要调用本方法。
        if (canvas_.isNull() || overlay_ == nullptr)
        {
            return;
        }

        // 记录这次脏事件的锚点与来源代次，供 flushNow/onDebounceTimeout 共用；
        // pendingValid_ 标记"确实有过一次有效的 noteDirty"——独立审核 D4 修复后，
        // recomputeAndFeed 开头会核对这个标记，没有任何有效待处理请求时直接跳过，
        // 不会用默认锚点/代次 0 去判定（见 recomputeAndFeed 与头文件 flushNow 注释）。
        pendingAnchor_ = anchorAddress;
        pendingSourceRevision_ = currentSourceRevision;
        pendingValid_ = true;
        // 第二轮审核报告 N1：记下这一次调用的在途位，供 setSuspended(false) 恢复时
        // 判断——如果最后一次 noteDirty 仍是在途，恢复时绝不能凭旧缓存起算（见
        // setSuspended 恢复分支）。
        pendingInFlight_ = hasInFlightRequests;
        if (suspended_)
        {
            // 第二轮审核报告 N2：挂起期间发生的这次脏事件需要在恢复时补回，记下
            // dirtySinceSuspend_；setSuspended 恢复分支据此判断要不要重新起算。
            dirtySinceSuspend_ = true;
        }

        if (hasInFlightRequests || suspended_)
        {
            // 在途页读取还没结束，或本类正被挂起（独立审核 D3，见 setSuspended）：这
            // 次只更新上面几项，不起算防抖。
            //
            // 独立审核 D1 修复：同时必须取消此前可能已经起算、尚未到期的那次防抖
            // 定时器——否则它到期时 onDebounceTimeout 仍会用"抢跑前"的旧判定结果
            // 重算一次，把这次还没读完/被挂起的窗口错误地当成最终结果喂出去，基线
            // 从此停在陈旧字节上（独立审核报告 D1，探针 Q1/Q2：连续滚轮越过预取
            // 边界、或点一下插入点后紧接着按 F5 都会触发）。等下一次
            // hasInFlightRequests=false 且未挂起的 noteDirty 到来才重新计时。
            debounceTimer_->stop();
            return;
        }

        // QTimer::start()（无参）在计时器已经在跑时会先停止再用同一个 interval 重新
        // 开始计时，天然实现"重置防抖"：50 ms 内连续多次 noteDirty 只会在最后一次
        // 之后的 50 ms 触发一次 recomputeAndFeed（"防抖合并"的夹具断言据此设计）。
        debounceTimer_->start();
    }

    // flushNow：跳过防抖立即执行一次。
    // 独立审核 D4 修复：是否存在"有效的待处理请求"（pendingValid_）的判断移到了
    // recomputeAndFeed 内部统一处理——没有待处理请求时整次判定直接跳过，不再使用
    // 没有意义的默认锚点/代次 0（旧行为见头文件注释历史版本），理由与具体读数见
    // recomputeAndFeed 开头与头文件 flushNow 注释。
    void WorkbenchBaselineFeeder::flushNow()
    {
        if (canvas_.isNull() || overlay_ == nullptr)
        {
            return;
        }

        // 第二轮审核报告 N3：挂起期间 flushNow 也必须是空操作——"写前复核前想确保
        // 窗口已经覆盖目标范围"这个典型用途本身就常常发生在写事务挂起期间，若不受
        // 挂起门控，会绕过 setSuspended(true) 照常用挤页后的旧内容重算，覆盖刚被
        // AcceptWrite 确认的基线字节（第二轮审核报告 N3，探针 R3/Q2）。此次的待处理
        // 请求（若有）已经由 noteDirty 记下（含 dirtySinceSuspend_），不会因为这里
        // 提前返回而丢失，恢复后会按 setSuspended 的恢复规则自然补上。
        if (suspended_)
        {
            return;
        }

        // 取消尚未到期的那次防抖定时器，改为立刻执行一次：避免"现在喂一次，稍后
        // 50 ms 到期又喂一次"的重复喂入。debounceTimer_ 未在跑时 stop() 是空操作。
        debounceTimer_->stop();
        recomputeAndFeed();
    }

    // onDebounceTimeout：50 ms 单次防抖到期的槛，直接执行一次判定与喂入。
    void WorkbenchBaselineFeeder::onDebounceTimeout()
    {
        recomputeAndFeed();
    }

    // setSuspended：独立审核 D3 新增的挂起入口，详见头文件注释。
    // 传入：suspended 新状态；与当前值相同时整个方法是空操作（不会重复 stop/start）。
    void WorkbenchBaselineFeeder::setSuspended(bool suspended)
    {
        if (suspended_ == suspended)
        {
            return;
        }
        suspended_ = suspended;

        if (suspended_)
        {
            // 第二轮审核报告 N2：进入挂起这一刻，若防抖定时器恰好还在跑（刚起算、
            // 尚未到期），说明有一次判定被这次挂起打断、还没来得及执行，恢复时要把
            // 它补回——记下 dirtySinceSuspend_。之后 stop() 会让 isActive() 变假，
            // 所以必须先读后停。
            dirtySinceSuspend_ = debounceTimer_->isActive();
            // 取消已经起算、尚未到期的防抖定时器，之后的 noteDirty 只记录不计时
            // （见 noteDirty 里 hasInFlightRequests || suspended_ 分支）。
            debounceTimer_->stop();
            return;
        }

        // 恢复：第二轮审核报告 N1/N2 修复——不再只看 pendingValid_，还要同时核对：
        // - !pendingInFlight_：最后一次 noteDirty 若仍是在途，说明真正的内容还没读
        //   完，绝不能凭这份旧缓存起算（否则 D1 的后果经恢复路径复发，第二轮审核
        //   报告 N1，探针 Q1/R2a/R2b）；
        // - dirtySinceSuspend_：挂起期间（或挂起那一刻）确实发生过需要补回的脏
        //   事件；没有的话说明"挂起又恢复、期间什么都没变"，不应该凭空产生一次
        //   判定（否则挤页后的旧内容会在恢复后覆盖刚确认的基线字节，第二轮审核报告
        //   N2，探针 R1/Q3）。
        // 三者都满足，并且画布/叠加层任一不为空（与 noteDirty 的判空语义一致）时，
        // 才按正常防抖规则重新起算一次；不满足时什么都不做，等下一次真正的
        // noteDirty 到来再说。
        // 不在这里额外清零 dirtySinceSuspend_：它只在上面这一处被读取，而读取之前
        // 必然先经过本方法的挂起分支重新赋值一次（isSuspended()/suspended_ 的状态
        // 机决定了"恢复"必然紧跟在"挂起"之后发生，中间不会有第二次挂起把它漏掉），
        // 所以这里不存在"用上一轮残留值"的风险，不需要再加一行清零（没有读数要求
        // 这行代码，加了也测不出区别，见修复说明 N1/N2 片段）。
        if (pendingValid_ && !pendingInFlight_ && dirtySinceSuspend_ && !canvas_.isNull() && overlay_ != nullptr)
        {
            debounceTimer_->start();
        }
    }

    // isSuspended：当前是否处于挂起状态，只读查询。
    bool WorkbenchBaselineFeeder::isSuspended() const noexcept
    {
        return suspended_;
    }

    // ======================== 核心判定与喂入 ========================

    // recomputeAndFeed：按 DecideBaselineRefeed 的判定结果执行 Keep/RefreshSameSpan/
    // Recompute 三者之一，被 flushNow 与 onDebounceTimeout 共用。
    void WorkbenchBaselineFeeder::recomputeAndFeed()
    {
        // 双重判空：onDebounceTimeout 在计时器到期时才执行，这中间宿主可能已经把
        // 画布或叠加层置空（例如换目标流程里的短暂中间态），这里必须再确认一次，
        // 不能假定 noteDirty 当时的判空结果到现在仍然有效。
        if (canvas_.isNull() || overlay_ == nullptr)
        {
            return;
        }

        // 独立审核 D4 修复：没有任何有效的待处理请求（从未调用过 noteDirty；或刚经历
        // 过 setAddressSpaceBounds/setIdentityKey 换目标，旧的待处理请求已被清空）时
        // 整次判定直接跳过——不再使用没有意义的默认锚点/代次 0。旧行为会在换到内核
        // 这类不含地址 0 的地址空间后，从未 noteDirty 就 flushNow 时，用默认锚点 0
        // 报出一个虚假的"该处尚未读取"（独立审核报告 D4，探针 Q6；P4a/P4b 是换目标
        // 后旧锚点继续生效的另一种表现）。
        if (!pendingValid_)
        {
            return;
        }

        // 独立审核 D2 修复：判定之前先核对 record_ 与叠加层*当前真实基线*是否一致。
        // record_ 只在本类自己的四个入口（setPolicy/setAddressSpaceBounds/
        // setIdentityKey/成功喂入后的 Set）被更新，如果宿主绕过本类直接调用了
        // overlay_->LoadBaseline（例如同一 identityKey 下重新载入），或者整个换成了
        // 一个全新的 overlay 对象，本类对此毫不知情——record_ 仍然指着一个叠加层早已
        // 不认的旧窗口，DecideBaselineRefeed 会在"锚点仍在窗口内、代次未变"时错误地
        // 判成 Keep，窗口永远空着（独立审核报告 D2，探针 P9/Q3a）。三项任一不符就把
        // record_ 当失效处理，强制走 Recompute 重新对齐。
        if (record_.hasWindow
            && (overlay_->BaseAddress() != record_.base
                || overlay_->BaselineSize() != record_.length
                || overlay_->IdentityKey() != identityKey_))
        {
            record_.Reset();
        }

        // anchor/revision：取最近一次 noteDirty 记下的值（上面的 pendingValid_ 判断
        // 已经保证这不是构造时的默认值 0/0）。
        const std::uint64_t anchor = pendingAnchor_;
        const std::uint64_t revision = pendingSourceRevision_;

        // 已落定页集合：查询整个当前地址空间边界，而不是只查锚点附近一小段——本类
        // settledPageStartsInRange 的实际开销只随"已缓存页数"（上限 kMaxCachedPages，
        // 默认 256）变化，与查询区间大小无关（见 HexCanvas::settledPageStartsInRange
        // 的实现：过滤已缓存页起始地址列表，不逐页遍历查询区间），所以查全地址空间
        // 既不费时，又能同时满足 SelectBaselineSpan 向锚点两侧扫描、以及
        // DecideBaselineRefeed 核对"旧窗口整段是否仍然落定"这两种需求，不必费力去
        // 凑一个"刚好够用、又不多不少"的区间。
        const std::set<std::uint64_t> settled = settledPageStartsInRange(bounds_.lowest, bounds_.highest);

        // 判定：Keep/RefreshSameSpan/Recompute 三者之一，判定顺序见 Core 头文件注释。
        const ksword::memwb::BaselineRefeedDecision decision =
            ksword::memwb::DecideBaselineRefeed(record_, anchor, revision, settled, policy_);

        if (decision == ksword::memwb::BaselineRefeedDecision::Keep)
        {
            // 窗口与来源都没变：什么都不做，不触碰 overlay、不发任何信号。这正是
            // "非脏不重建"的落点——如果这里误触发了 RefreshBaseline，叠加层的
            // SelfWritten 标记与"上次读取"都会被无条件清掉一次，青色高亮会被反复
            // 打断（头文件文件头"防抖"一节）。
            return;
        }

        // base/length：RefreshSameSpan 原位沿用上次记录（位置不变，只是来源代次变了，
        // 典型场景是重读）；Recompute 重新选取一次（尚无窗口、锚点滚出窗口、窗口内有
        // 页不再落定等）。
        std::uint64_t base = 0;
        std::uint64_t length = 0;
        if (decision == ksword::memwb::BaselineRefeedDecision::RefreshSameSpan)
        {
            base = record_.base;
            length = record_.length;
        }
        else
        {
            const ksword::memwb::BaselineSpan span =
                ksword::memwb::SelectBaselineSpan(anchor, settled, bounds_, policy_);
            if (span.status != ksword::memwb::BaselineSpanStatus::Ok)
            {
                // 插入点所在页未落定（InsertionPageNotSettled），或插入点本身不在合法
                // 页范围内（InsertionOutsideSpace/InvalidArgument）：按头文件约定发
                // baselineUnavailable，不调用 RefreshBaseline，也不改动 record_——旧
                // 记录（如果存在）此刻仍与 overlay_ 的真实基线状态一致，保持不动，等
                // 下一次脏事件（锚点移回已落定区域）自然重试即可，不需要在这里主动
                // 清空它来换取什么好处。
                emit baselineUnavailable(span.status);
                return;
            }
            base = span.base;
            length = span.length;
        }

        // 从画布页缓存取字节（掩码版，不叠加暂存补丁），喂给叠加层的 RefreshBaseline。
        // base/length 两条路径分别来自 SelectBaselineSpan 的合法输出、或上一次成功
        // 记录，二者都保证 base + length 不回绕（SelectBaselineSpan 文件头"终点不含"
        // 一节；record_ 只会被 Set() 写入同样合法的跨度），这里再减一换算闭区间末
        // 地址不会下溢（两条路径下 length 都必然非零）。
        const std::uint64_t lastAddress = base + (length - 1ULL);
        const std::optional<HexCanvas::CachedRangeCopy> copy =
            canvas_->copyCachedRangeWithMask(base, lastAddress);
        if (!copy.has_value())
        {
            // 理论上不会发生：settled 集合里的每一页都已经是"缓存里的整页"，
            // copyCachedRangeWithMask 对这样的区间只会在"还没落定"时才整体失败。
            // 保守起见仍不崩溃、不喂半截数据——静默跳过，等下一次脏事件自然重试，
            // 不为这种不该出现的分支伪造一个 BaselineSpanStatus 发出去（那会把排查
            // 问题的人指向错误的方向）。
            return;
        }

        const ksword::memwb::BaselineLoadStatus loadStatus =
            overlay_->RefreshBaseline(identityKey_, base, copy->bytes, copy->validMask);
        if (loadStatus != ksword::memwb::BaselineLoadStatus::Ok)
        {
            // 同样理论上不会发生：bytes/validMask 等长（来自同一次 copy），base+length
            // 不回绕（上面已核对）。保守起见不崩溃、不发任何信号，不改动 record_。
            return;
        }

        // 成功：记下这次窗口供下次判定复用（Set 内部按 revision 一起存），并通知
        // 订阅者——装配层据此调用 canvas_->notifyOverlayChanged()（头文件
        // baselineRefreshed 注释：画布不知道叠加层被绕过改了基线）。
        ksword::memwb::BaselineSpan fedSpan;
        fedSpan.status = ksword::memwb::BaselineSpanStatus::Ok;
        fedSpan.base = base;
        fedSpan.length = length;
        record_.Set(fedSpan, revision);
        emit baselineRefreshed(base, length);
    }

    // settledPageStartsInRange（私有）：包一层画布判空的转发，canvas_ 为空（未设置，
    // 或已被外部销毁致 QPointer 自动置空）时返回空集，不崩溃。本类所有需要查询"已
    // 落定页"的地方（目前只有 recomputeAndFeed 一处）都经过这一个入口，判空逻辑
    // 只写一遍。
    // 传入：闭区间两端；传出：命中页起始地址集合（转发 HexCanvas::settledPageStartsInRange
    // 的结果，语义见该方法声明处注释）。
    std::set<std::uint64_t> WorkbenchBaselineFeeder::settledPageStartsInRange(
        std::uint64_t firstAddress,
        std::uint64_t lastAddress) const
    {
        if (canvas_.isNull())
        {
            return {};
        }
        return canvas_->settledPageStartsInRange(firstAddress, lastAddress);
    }

    // lastWindow：最近一次成功喂入的窗口记录，只读查询，供诊断与夹具断言使用。
    const ksword::memwb::BaselineWindowRecord& WorkbenchBaselineFeeder::lastWindow() const noexcept
    {
        return record_;
    }
}
