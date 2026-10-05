#include "bugcheck_evidence.h" // 使用唯一内部证据定义，不复制通信协议。

// 静态数据驻留普通非分页节；记录函数不分配、等待、格式化或递归输出日志。
typedef struct _KSWORD_BUGCHECK_TRACE_SLOT // 一个提交序号和一条固定事件组成环槽。
{ // 环槽结构开始。
    volatile LONG64 CommitSequence; // 负值表示正在写入，正值表示完整提交。
    KSWORD_BUGCHECK_EVENT Event; // 按值保存的最近事件，无外部文本指针。
} KSWORD_BUGCHECK_TRACE_SLOT; // 环槽结构结束。

static KSWORD_BUGCHECK_TRACE_SLOT g_KswordTraceSlots[KSWORD_BUGCHECK_EVIDENCE_EVENTS]; // 六个常驻环槽。
static volatile LONG g_KswordTraceWriter = 0L; // 单次 CAS 写占用位，不等待其他 CPU。
static DECLSPEC_ALIGN(8) volatile LONG64 g_KswordTraceLatest = 0LL; // 最近完整发布的全局序号。
static DECLSPEC_ALIGN(8) volatile LONG64 g_KswordTraceDiscarded = 0LL; // 写冲突或序号耗尽的丢弃计数。
static volatile LONG g_KswordTraceDebugCapture = 0L; // 实际 DbgPrint 捕获状态镜像，默认未启用。

// 正常运行期调用；输入类别、编号、真实状态和有界文本，输出为固定环槽或明确丢弃。
VOID // 记录接口无返回值，也不向普通 WDF 日志队列回写。
KswordARKBugcheckTraceRecord( // 在调用者当前上下文记录，而非声称记录故障上下文。
    _In_ ULONG Kind, // 类别区分 handler 开始、返回、调试输出、文字日志及捕获状态。
    _In_ ULONG Code, // 保存 IOCTL、调试组件或文字日志级别等类别相关编号。
    _In_ NTSTATUS Status, // 保存原始 handler 状态或 DbgPrint 级别，文字日志标记缺失。
    _In_opt_ PCSTR Text, // 输入必须是调用时有效的内核缓冲，不解引用 BugCheck 参数。
    _In_ ULONG TextBytes // 表示允许读取的原始字节上限，不要求输入 NUL 终止。
    ) // 参数列表结束。
{ // 写入函数开始。
    KSWORD_BUGCHECK_TRACE_SLOT* slot; // 当前唯一写者要更新的环槽。
    ULONG64 sequence; // 本次事件的正序号，零保留给从未发布。
    ULONG copyLimit; // 留出结尾 NUL 后最多复制的字节数量。
    ULONG textIndex; // 有界文本复制游标。

    if (Kind == KSWORD_BUGCHECK_TRACE_KIND_DBGPRINT_STATE) { // 状态镜像与 ring 写冲突相互独立。
        InterlockedExchange(&g_KswordTraceDebugCapture, Code != 0UL ? 1L : 0L); // 以实际捕获位更新镜像。
    } else if (Kind == KSWORD_BUGCHECK_TRACE_KIND_DBGPRINT) { // 只有已启用 callback 能进入该类别。
        InterlockedExchange(&g_KswordTraceDebugCapture, 1L); // 即使 START 状态记录丢弃也能确认采集曾启用。
    } // 捕获状态镜像结束。

    if (InterlockedCompareExchange(&g_KswordTraceWriter, 1L, 0L) != 0L) { // 任意 IRQL 都只尝试一次。
        InterlockedIncrement64(&g_KswordTraceDiscarded); // 写冲突记录为丢弃，不等待其他 CPU。
        return; // 立即离开，避免高 IRQL 或崩溃窗口形成死锁。
    } // 单写者占用成功。
    sequence = (ULONG64)InterlockedCompareExchange64(&g_KswordTraceLatest, 0LL, 0LL); // 原子读取已提交序号。
    if (sequence == (ULONG64)MAXLONGLONG) { // 保留负提交哨兵的有符号范围，避免计数翻转。
        InterlockedIncrement64(&g_KswordTraceDiscarded); // 序号耗尽也作为真实丢弃报告。
        InterlockedExchange(&g_KswordTraceWriter, 0L); // 退出前发布空闲占用位。
        return; // 不制造重复或负事件序号。
    } // 序号范围验证结束。
    sequence += 1ULL; // 只有拿到写占用的事件才消耗提交序号。
    slot = &g_KswordTraceSlots[(ULONG)((sequence - 1ULL) % KSWORD_BUGCHECK_EVIDENCE_EVENTS)]; // 映射六槽循环位置。
    InterlockedExchange64(&slot->CommitSequence, -((LONG64)sequence)); // 先使覆盖中的旧槽不可读。
    RtlZeroMemory(&slot->Event, sizeof(slot->Event)); // 清除旧内容和所有未填写的字节。
    slot->Event.Sequence = sequence; // 保存用于读取端核对的事件序号。
    slot->Event.Time = KeQueryInterruptTime(); // 记录启动后 100ns 时间，不依赖真实时区。
    slot->Event.Kind = Kind; // 保存类别而不从文字猜测事件来源。
    slot->Event.Code = Code; // 保存类别相关的原始编号。
    slot->Event.Status = Status; // 原样保留真实状态或显式缺失占位。
    slot->Event.ProcessId = (ULONG_PTR)PsGetCurrentProcessId(); // 记录本次采集上下文进程标识。
    slot->Event.ThreadId = (ULONG_PTR)PsGetCurrentThreadId(); // 记录本次采集上下文线程标识。
    slot->Event.Flags = KSWORD_BUGCHECK_TRACE_EVENT_CONTEXT_VALID; // PID/TID/时间是采集时上下文。
    if (Kind == KSWORD_BUGCHECK_TRACE_KIND_DRIVER_LOG || Kind == KSWORD_BUGCHECK_TRACE_KIND_IOCTL_BEGIN) { // 两类事件尚无原操作结果。
        slot->Event.Flags |= KSWORD_BUGCHECK_TRACE_EVENT_STATUS_UNAVAILABLE; // 不把日志严重度或开始哨兵伪装为完成状态。
    } // 状态语义标记结束。

    copyLimit = min(TextBytes, KSWORD_BUGCHECK_EVIDENCE_EVENT_TEXT - 1UL); // 摘要最多 63 字节加 NUL。
    textIndex = 0UL; // 从零开始复制，不扫描整个输入缓冲。
    if (Text != NULL) { // 空文本仍是一条有真实类型和上下文的事件。
        while (textIndex < copyLimit && Text[textIndex] != '\0') { // 只读调用者声明的有界范围。
            slot->Event.Text[textIndex] = Text[textIndex]; // 保留原始字节，不伪造解析后的错误信息。
            textIndex += 1UL; // 推進固定长度游标。
        } // 摘要复制结束。
        if (textIndex == copyLimit && TextBytes > copyLimit && Text[textIndex] != '\0') { // 原始摘要仍有后续内容。
            slot->Event.Flags |= KSWORD_BUGCHECK_TRACE_EVENT_TEXT_TRUNCATED; // 截断必须在事件中显式标记。
        } // 截断判定结束。
    } // 可选文本处理结束。
    slot->Event.Text[textIndex] = '\0'; // 无论来源是否 NUL 终止都生成安全固定副本。
    KeMemoryBarrier(); // 在发布提交标志之前完成全部字段写入。
    InterlockedExchange64(&slot->CommitSequence, (LONG64)sequence); // 完整事件成为读取端可接受的正序号。
    InterlockedExchange64(&g_KswordTraceLatest, (LONG64)sequence); // 最新序号最后发布，不让读取端追到半条事件。
    InterlockedExchange(&g_KswordTraceWriter, 0L); // 原子释放单写者占用位，不调用普通日志锁。
} // 写入函数结束。

// 崩溃阶段调用；仅填充事件字段与专属标志，保留调用方已采集的其他证据。
VOID // 快照接口无返回值，冲突和缺失在 Evidence 中显式发布。
KswordARKBugcheckTraceSnapshot( // 调用方提供固定非分页输出，不在函数内分配内存。
    _Inout_ KSWORD_BUGCHECK_EVIDENCE* Evidence // 最近六条事件及覆盖、丢弃、捕获状态的目标结构。
    ) // 参数列表结束。
{ // 快照函数开始。
    ULONG64 latest; // 快照开始时最近完整提交的事件序号。
    ULONG64 first; // 此次最多六条事件中的最早序号。
    ULONG64 expected; // 当前环槽应当保存的完整序号。
    ULONG64 discarded; // 单次原子读取的写冲突丢弃计数。
    ULONG count; // 本次有界读取要检查的环槽数量。
    ULONG index; // 最多六轮的读取游标。
    ULONG accepted; // 已通过前后提交序号核对的事件数量。
    LONG64 before; // 复制之前的环槽提交序号。
    LONG64 after; // 复制之后的环槽提交序号。
    KSWORD_BUGCHECK_TRACE_SLOT* slot; // 当前读取环槽，绝不向其中写入。

    if (Evidence == NULL) { // 空目标不允许触碰快照状态。
        return; // 不记录日志，也不执行错误恢复。
    } // 输出目标验证结束。
    Evidence->Flags &= ~(KSWORD_BUGCHECK_EVIDENCE_TRACE_PRESENT | // 仅清除本模块负责的环可用标志。
        KSWORD_BUGCHECK_EVIDENCE_TRACE_RACE | KSWORD_BUGCHECK_EVIDENCE_TRACE_OVERWRITTEN | // 清除上次快照冲突和覆盖状态。
        KSWORD_BUGCHECK_EVIDENCE_TRACE_DROPPED | KSWORD_BUGCHECK_EVIDENCE_DBG_CAPTURE_ACTIVE | // 清除上次丢弃与启用状态。
        KSWORD_BUGCHECK_EVIDENCE_DBG_CAPTURE_DISABLED); // 清除上次未启用状态，保留其他模块低位。
    Evidence->Flags |= KSWORD_BUGCHECK_EVIDENCE_TRACE_PRESENT; // 固定环已编入驱动，零事件不等于采集失败。
    Evidence->EventCount = 0UL; // 初始化稳定记录数量，不沿用前次快照。
    RtlZeroMemory(Evidence->Events, sizeof(Evidence->Events)); // 清理所有未成功发布的输出槽。
    latest = (ULONG64)InterlockedCompareExchange64(&g_KswordTraceLatest, 0LL, 0LL); // 只读取完整发布的全局序号。
    discarded = (ULONG64)InterlockedCompareExchange64(&g_KswordTraceDiscarded, 0LL, 0LL); // 丢弃计数无需取写锁。
    Evidence->EventsOverwritten = latest > KSWORD_BUGCHECK_EVIDENCE_EVENTS ? latest - KSWORD_BUGCHECK_EVIDENCE_EVENTS : 0ULL; // 覆盖只统计已提交历史事件。
    Evidence->EventsDiscarded = discarded; // 写冲突丢弃与容量覆盖单独展示。
    Evidence->EventsDropped = Evidence->EventsOverwritten + discarded; // 保留总体丢失字段供既有输出使用。
    if (Evidence->EventsOverwritten != 0ULL) { // 环形容量不足以保存全部历史事件。
        Evidence->Flags |= KSWORD_BUGCHECK_EVIDENCE_TRACE_OVERWRITTEN; // 明确只保留最近六条。
    } // 覆盖标记结束。
    if (discarded != 0ULL) { // 正常运行期出现过 try-lock 冲突或序号耗尽。
        Evidence->Flags |= KSWORD_BUGCHECK_EVIDENCE_TRACE_DROPPED; // 丢失不代表原操作失败。
    } // 丢弃标记结束。
    Evidence->Flags |= InterlockedCompareExchange(&g_KswordTraceDebugCapture, 0L, 0L) != 0L ? KSWORD_BUGCHECK_EVIDENCE_DBG_CAPTURE_ACTIVE : KSWORD_BUGCHECK_EVIDENCE_DBG_CAPTURE_DISABLED; // 未启用状态不能伪装为无调试错误。
    if (InterlockedCompareExchange(&g_KswordTraceWriter, 0L, 0L) != 0L) { // 写者可能停在任意指令，读取端绝不等待。
        Evidence->Flags |= KSWORD_BUGCHECK_EVIDENCE_TRACE_RACE; // 当前快照存在并发写入边界。
    } // 写占用观察结束。

    count = (ULONG)min(latest, (ULONG64)KSWORD_BUGCHECK_EVIDENCE_EVENTS); // 最多读取六个已提交候选。
    first = count != 0UL ? latest - count + 1ULL : 0ULL; // 空环保持零而不生成伪造序号。
    accepted = 0UL; // 仅接受无撕裂副本。
    for (index = 0UL; index < count; index += 1UL) { // 固定上限读取，不追赶新事件、不重复轮询。
        expected = first + index; // 对应快照开始时固定的历史窗口。
        slot = &g_KswordTraceSlots[(ULONG)((expected - 1ULL) % KSWORD_BUGCHECK_EVIDENCE_EVENTS)]; // 映射只读环槽。
        before = InterlockedCompareExchange64(&slot->CommitSequence, 0LL, 0LL); // 负值或其他序号均不可信。
        if (before != (LONG64)expected) { // 槽正在覆盖或已不再属于目标历史窗口。
            Evidence->Flags |= KSWORD_BUGCHECK_EVIDENCE_TRACE_RACE; // 标记冲突而不读取半条事件。
            continue; // 跳过该槽，保持有界非等待行为。
        } // 复制前提交验证结束。
        RtlCopyMemory(&Evidence->Events[accepted], &slot->Event, sizeof(slot->Event)); // 在固定输出中暂存副本。
        KeMemoryBarrier(); // 防止复制被移动到后一次提交读取之后。
        after = InterlockedCompareExchange64(&slot->CommitSequence, 0LL, 0LL); // 再次确认复制期间没有覆盖。
        if (after == before && Evidence->Events[accepted].Sequence == expected) { // 前后提交和内部序号都必须匹配。
            accepted += 1UL; // 仅完整事件进入发布数量。
        } else { // 并发写者改变过此槽，副本不作为真实证据使用。
            RtlZeroMemory(&Evidence->Events[accepted], sizeof(Evidence->Events[accepted])); // 清除可能撕裂的暂存副本。
            Evidence->Flags |= KSWORD_BUGCHECK_EVIDENCE_TRACE_RACE; // 显示本次缺失的真实边界。
        } // 复制后提交验证结束。
    } // 六槽读取结束。
    Evidence->EventCount = accepted; // 按时间顺序发布全部稳定事件。
    if ((ULONG64)InterlockedCompareExchange64(&g_KswordTraceLatest, 0LL, 0LL) != latest) { // 读取期间又有新的完整提交。
        Evidence->Flags |= KSWORD_BUGCHECK_EVIDENCE_TRACE_RACE; // 本次是固定窗口快照而非原子全局现场。
    } // 全局发布状态复核结束。
} // 快照函数结束。
