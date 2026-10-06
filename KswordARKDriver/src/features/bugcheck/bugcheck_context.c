#include "bugcheck_evidence.h" // 固定非分页证据合同。

NTSYSAPI USHORT NTAPI RtlCaptureStackBackTrace(_In_ ULONG FramesToSkip, _In_ ULONG FramesToCapture, _Out_writes_to_(FramesToCapture,return) PVOID* BackTrace, _Out_opt_ PULONG BackTraceHash); // 与 ntifs.h 系统声明一致，仅正常 PASSIVE_LEVEL 调用。

#define KSWORD_DUMP_CONTEXT_OFFSET 0x348UL // PAGE/DU64 的 AMD64 CONTEXT 固定偏移。
#define KSWORD_DUMP_PREFIX_BYTES 0x448UL // 只保留最后一个所需寄存器之前的头部。
#define KSWORD_CONTEXT_RACE_FLAG 0x4000UL // 明确报告并发采样未取得稳定证据。

typedef struct _KSWORD_OPERATION_STACK // 正常运行期调用栈副本。
{ // 不含动态内存或未知对象指针。
    ULONG Count; // 实际返回的栈项数量。
    NTSTATUS Status; // 采集结果。
    ULONG64 ThreadId; // 栈所属线程。
    ULONG64 Time; // 启动后 100ns 采样时间。
    ULONG64 Frames[KSWORD_BUGCHECK_EVIDENCE_STACK]; // 有界返回地址。
} KSWORD_OPERATION_STACK; // 该类型只在本文件内部使用。

static volatile LONG gContextWriter; // DumpIo 写端只尝试一次，不等待。
static volatile LONG gContextSequence; // 偶数代表头部快照已发布。
static UCHAR gDumpPrefix[KSWORD_DUMP_PREFIX_BYTES]; // 常驻头部原始字节。
static UCHAR gDumpSeen[KSWORD_DUMP_PREFIX_BYTES]; // 每字节是否实际收到，不能把零填充当数据。
static ULONG64 gDumpNextOffset; // Offset=-1 的顺序写入位置。
static KSWORD_BUGCHECK_CONTEXT_EVIDENCE gDumpContext; // 只存已验证的系统头部寄存器。
static volatile LONG gStackWriter; // 运行期栈写端采用单次 CAS。
static volatile LONG gStackSequence; // 最近操作栈的发布序号。
static KSWORD_OPERATION_STACK gOperationStack; // 常驻正常操作调用栈。

static BOOLEAN KswordContextHas(_In_ ULONG Offset, _In_ ULONG Bytes) // 检查指定区间全部收到。
{ // 缺页或不连续写入不能产生伪造寄存器。
    ULONG index; // 有界逐字节索引。
    if (Offset > KSWORD_DUMP_PREFIX_BYTES || Bytes > KSWORD_DUMP_PREFIX_BYTES - Offset) return FALSE; // 防溢出。
    for (index = 0; index < Bytes; ++index) if (gDumpSeen[Offset + index] == 0) return FALSE; // 不接受缺字节。
    return TRUE; // 区间完整。
} // 区间检查结束。

static ULONG KswordContextU32(_In_ ULONG Offset) // 明确解析小端，不依赖对齐或结构 padding。
{ // 所有调用先验证区间。
    return (ULONG)gDumpPrefix[Offset] | ((ULONG)gDumpPrefix[Offset + 1] << 8) | ((ULONG)gDumpPrefix[Offset + 2] << 16) | ((ULONG)gDumpPrefix[Offset + 3] << 24); // 还原四字节。
} // 小端整数结束。

static ULONG64 KswordContextU64(_In_ ULONG Offset) // 解析八字节寄存器。
{ // 不直接解引用不对齐 ULONG64。
    return (ULONG64)KswordContextU32(Offset) | ((ULONG64)KswordContextU32(Offset + 4) << 32); // 拼接高低部分。
} // 八字节整数结束。

static VOID KswordContextParse(VOID) // 只读取系统 DumpIo 头中已收到的字节。
{ // 本函数不走未知 BugCheck 参数中的地址。
    static const ULONG offsets[17] = { 0x78, 0x90, 0x80, 0x88, 0xA8, 0xB0, 0xA0, 0x98, 0xB8, 0xC0, 0xC8, 0xD0, 0xD8, 0xE0, 0xE8, 0xF0, 0xF8 }; // RAX,RBX,RCX,RDX,RSI,RDI,RBP,RSP,R8..R15,RIP。
    ULONG flags; // AMD64 CONTEXT 控制标志。
    ULONG index; // 固定寄存器索引。
    RtlZeroMemory(&gDumpContext, sizeof(gDumpContext)); // 当前头和 flags 必须重新证明有效，不继承旧寄存器。
    gDumpContext.ContextStatus = STATUS_NOT_FOUND; // 不完整头明确表示尚未收到。
    gDumpContext.StackStatus = STATUS_NOT_FOUND; // 运行期栈由独立模块快照合并。
    if (!KswordContextHas(0, 8) || !KswordContextHas(0x30, 4)) return; // 等待签名和机器类型完整。
    if (KswordContextU32(0) != 0x45474150UL || KswordContextU32(4) != 0x34365544UL || KswordContextU32(0x30) != 0x8664UL) // 只接受 PAGE/DU64 AMD64。
    { // 不把其他格式的字节解释成 CONTEXT。
        gDumpContext.ContextStatus = STATUS_INVALID_IMAGE_FORMAT; // 保留明确失败原因。
        return; // 拒绝该格式。
    } // 签名检查结束。
    if (!KswordContextHas(KSWORD_DUMP_CONTEXT_OFFSET + 0x30, 4)) return; // CONTEXT flags 尚未到达。
    flags = KswordContextU32(KSWORD_DUMP_CONTEXT_OFFSET + 0x30); // 读取真实标志。
    if ((flags & 0x00FF0000UL) != 0x00100000UL || (flags & 3UL) == 0) // AMD64 且至少控制/整数分组有效。
    { // 未初始化或 PAGE 填充的区域不能冒充现场。
        gDumpContext.ContextStatus = STATUS_NOT_SUPPORTED; // 来源存在但格式不支持。
        return; // 不发布寄存器。
    } // 标志检查结束。
    for (index = 0; index < RTL_NUMBER_OF(offsets); ++index) // 固定十七个八字节寄存器。
    { // 每个寄存器单独检查有效分组和收到的范围。
        ULONG required = (index == 7 || index == 16) ? 1UL : 2UL; // RSP/RIP 属控制分组，其余属于整数分组。
        ULONG offset = KSWORD_DUMP_CONTEXT_OFFSET + offsets[index]; // 合成已知头内偏移。
        if ((flags & required) != 0 && KswordContextHas(offset, 8)) // 只发布完整寄存器。
        { // 复制不涉及任何其他内存。
            gDumpContext.Registers[index] = KswordContextU64(offset); // 原始值可为零。
            gDumpContext.RegisterMask |= 1UL << index; // 原始零与不可用通过位图区分。
        } // 单寄存器解析结束。
    } // 固定寄存器循环结束。
    if ((flags & 1UL) != 0 && KswordContextHas(KSWORD_DUMP_CONTEXT_OFFSET + 0x44, 4)) // EFLAGS 只有四字节。
    { // 以同一证据位图保存。
        gDumpContext.Registers[17] = KswordContextU32(KSWORD_DUMP_CONTEXT_OFFSET + 0x44); // 保留原始 EFLAGS。
        gDumpContext.RegisterMask |= 1UL << 17; // 该寄存器已取得。
    } // EFLAGS 结束。
    if (gDumpContext.RegisterMask != 0) // 至少一个完整值才承认来源有效。
    { // 系统转储头不承诺等于触发故障的上下文。
        gDumpContext.ContextSource = 1; // 解码器明确标为 DUMP_HEADER_CONTEXT。
        gDumpContext.ContextStatus = STATUS_SUCCESS; // 所有未取得值仍由位图标记。
    } // 发布有效来源结束。
} // 头部解析结束。

VOID KswordARKBugcheckContextReset(VOID) // 安装期间调用，无并发崩溃读者。
{ // 重置常驻缓存并保留缺失原因。
    RtlZeroMemory(gDumpPrefix, sizeof(gDumpPrefix)); // 清空旧头。
    RtlZeroMemory(gDumpSeen, sizeof(gDumpSeen)); // 清空完整性图。
    RtlZeroMemory(&gDumpContext, sizeof(gDumpContext)); // 清空寄存器证据。
    RtlZeroMemory(&gOperationStack, sizeof(gOperationStack)); // 清空运行期调用栈。
    gDumpContext.ContextStatus = STATUS_NOT_FOUND; // 尚未收到系统头。
    gDumpContext.StackStatus = STATUS_NOT_FOUND; // 尚未捕获运行期栈。
    gOperationStack.Status = STATUS_NOT_FOUND; // 明确无栈来源。
    gDumpNextOffset = 0; // 顺序拼接从头开始。
    InterlockedExchange(&gContextSequence, 0); // 偶数为空快照。
    InterlockedExchange(&gStackSequence, 0); // 偶数为空快照。
    InterlockedExchange(&gContextWriter, 0); // 写端可用。
    InterlockedExchange(&gStackWriter, 0); // 写端可用。
} // 安装重置结束。

VOID KswordARKBugcheckEvidenceDumpIo(_In_ const KBUGCHECK_DUMP_IO* DumpIo) // DumpIo 提供的缓冲来自系统转储写入。
{ // HIGH_LEVEL 阶段不分配、不等待锁、不遍历任意地址。
    ULONG64 offset; // 系统写入偏移。
    ULONG bytes; // 有界复制长度。
    ULONG index; // 固定区间索引。
    const UCHAR* source; // 只读系统缓冲。
    if (DumpIo == NULL || DumpIo->Type != KbDumpIoHeader || DumpIo->Buffer == NULL || DumpIo->BufferLength == 0) return; // 非头写入不处理。
    if ((LONG64)DumpIo->Offset < -1LL) return; // 系统只定义全一值为未知顺序偏移，其他负数不解释为位置。
    if (InterlockedCompareExchange(&gContextWriter, 1, 0) != 0) return; // 不等待其他处理器。
    InterlockedIncrement(&gContextSequence); // 将发布序号置为奇数。
    offset = DumpIo->Offset == MAXULONGLONG ? gDumpNextOffset : DumpIo->Offset; // 支持系统 ULONG64 全一的顺序偏移标志。
    if (offset <= MAXULONGLONG - DumpIo->BufferLength) gDumpNextOffset = offset + DumpIo->BufferLength; // 防止下一偏移溢出。
    if (offset < KSWORD_DUMP_PREFIX_BYTES) // 只保存固定前缀。
    { // 输入超出前缀的尾部无需读取。
        if (offset == 0) // 新头从零开始，不能拼入此前转储头的尾部。
        { // 清理收到位图即可防止跨代次组合。
            RtlZeroMemory(gDumpSeen, sizeof(gDumpSeen)); // 所有字段必须由本次头重新收到。
            RtlZeroMemory(gDumpPrefix, sizeof(gDumpPrefix)); // 清理旧原始字节，方便诊断。
        } // 新头代次处理结束。
        bytes = (ULONG)(KSWORD_DUMP_PREFIX_BYTES - offset); // 最大所需长度。
        if (bytes > DumpIo->BufferLength) bytes = DumpIo->BufferLength; // 截到本次有效缓冲。
        source = (const UCHAR*)DumpIo->Buffer; // 缓冲已经由系统提供并常驻。
        for (index = 0; index < bytes; ++index) // 最多 1096 个字节。
        { // 原始字节与完整性标记同步更新。
            gDumpPrefix[(ULONG)offset + index] = source[index]; // 单字节读取不跨越 BufferLength。
            gDumpSeen[(ULONG)offset + index] = 1; // 记录收到的数据位置。
        } // 前缀复制结束。
        KswordContextParse(); // 只解析已完整收到的字段。
    } // 固定前缀范围结束。
    KeMemoryBarrier(); // 字段写入先于发布序号。
    InterlockedIncrement(&gContextSequence); // 偶数表示完成。
    InterlockedExchange(&gContextWriter, 0); // 释放尝试锁。
} // 系统头采集结束。

VOID KswordARKBugcheckContextOperation(VOID) // 正常 IOCTL 执行前记录可分页解析的运行期栈。
{ // 绝不在 BugCheck 回调或提高的 IRQL 调用捕栈 API。
    PVOID frames[KSWORD_BUGCHECK_EVIDENCE_STACK]; // 有界正常运行期局部数组。
    USHORT count = 0; // 默认未取得。
    ULONG index; // 固定栈项索引。
    NTSTATUS status = STATUS_NOT_FOUND; // 默认无帧可用。
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !KswordARKBugcheckTrackingAcquire()) return; // 重装期间不接触会被 ContextReset 清零的写者位和缓存。
    if (InterlockedCompareExchange(&gStackWriter, 1, 0) != 0) // 正常采样只尝试一次 CAS，不能等待其他写者。
    { // 抢占失败仍须对称释放本次运行期引用。
        KswordARKBugcheckTrackingRelease(); // 控制层可继续等待实际写者，而不是失败采样。
        return; // 不采集竞争中的操作栈。
    } // 单写者占用失败处理结束。
    InterlockedIncrement(&gStackSequence); // 写入期间不允许快照接受混合字段。
    __try // 正常期捕栈仍可能遇到已损坏栈。
    { // 系统例程最多返回固定数量。
        count = RtlCaptureStackBackTrace(1, KSWORD_BUGCHECK_EVIDENCE_STACK, frames, NULL); // 只捕当前正常操作栈。
        if (count != 0) status = STATUS_SUCCESS; // 零帧不能声称有效。
    } // 正常捕栈结束。
    __except (EXCEPTION_EXECUTE_HANDLER) // 防止运行期证据采集扩大异常。
    { // 失败清空数量并保留状态码。
        count = 0; // 不发布不完整局部帧。
        status = GetExceptionCode(); // 保留实际异常原因。
    } // 捕获异常结束。
    gOperationStack.Count = count; // 发布取得的数量。
    gOperationStack.Status = status; // 采集状态。
    gOperationStack.ThreadId = (ULONG64)(ULONG_PTR)PsGetCurrentThreadId(); // 捕栈线程。
    gOperationStack.Time = KeQueryInterruptTime(); // 单调启动后时钟。
    for (index = 0; index < KSWORD_BUGCHECK_EVIDENCE_STACK; ++index) gOperationStack.Frames[index] = index < count ? (ULONG64)(ULONG_PTR)frames[index] : 0; // 未用项明确清零。
    KeMemoryBarrier(); // 值先于偶数序号可见。
    InterlockedIncrement(&gStackSequence); // 完成发布。
    InterlockedExchange(&gStackWriter, 0); // 允许下一正常操作采样。
    KswordARKBugcheckTrackingRelease(); // 所有可重置字段和 CAS 状态访问完毕后才允许下一代初始化。
} // 正常操作栈采集结束。

VOID KswordARKBugcheckContextSnapshot(_Inout_ KSWORD_BUGCHECK_EVIDENCE* Evidence) // 崩溃阶段只读取固定缓存。
{ // 不获取写锁，不等待，不进行栈展开。
    LONG before; // 发布序号前读。
    LONG after; // 发布序号后读。
    KSWORD_OPERATION_STACK stack; // 有界临时栈快照。
    if (Evidence == NULL) return; // 防御空调用。
    Evidence->Context.ContextStatus = STATUS_NOT_FOUND; // 默认头未到达。
    Evidence->Context.StackStatus = STATUS_NOT_FOUND; // 默认未捕正常操作栈。
    before = InterlockedCompareExchange(&gContextSequence, 0, 0); // 原子读取开始序号。
    if ((before & 1) == 0) // 写端未处于更新中。
    { // 读取固定寄存器结构。
        RtlCopyMemory(&Evidence->Context, &gDumpContext, sizeof(gDumpContext)); // 不读取其他内存。
        KeMemoryBarrier(); // 完成字段读后复查发布序号。
        after = InterlockedCompareExchange(&gContextSequence, 0, 0); // 获取结束序号。
        if (after != before || (after & 1) != 0) // 发生并发头写入。
        { // 拒绝混合上下文。
            RtlZeroMemory(&Evidence->Context, sizeof(Evidence->Context)); // 不能保留可能混合的寄存器。
            Evidence->Context.ContextStatus = STATUS_RETRY; // 显式报告竞争。
            Evidence->Flags |= KSWORD_CONTEXT_RACE_FLAG; // 总体状态可解码。
        } // 竞争处理结束。
    } // 寄存器快照结束。
    else // 系统头正在更新。
    { // 不等待完成。
        Evidence->Context.ContextStatus = STATUS_RETRY; // 明确未取得稳定源。
        Evidence->Flags |= KSWORD_CONTEXT_RACE_FLAG; // 整体竞争位。
    } // 写入中处理结束。
    before = InterlockedCompareExchange(&gStackSequence, 0, 0); // 读取栈开始序号。
    if ((before & 1) != 0) // 运行期写端可能停在其他处理器。
    { // 不能自旋等待。
        Evidence->Context.StackStatus = STATUS_RETRY; // 栈采样竞争。
        Evidence->Flags |= KSWORD_CONTEXT_RACE_FLAG; // 整体状态。
        return; // 寄存器结果仍保留。
    } // 栈写入中检查结束。
    RtlCopyMemory(&stack, &gOperationStack, sizeof(stack)); // 固定栈数据副本。
    KeMemoryBarrier(); // 复制后验证发布序号。
    after = InterlockedCompareExchange(&gStackSequence, 0, 0); // 栈结束序号。
    if (after != before || (after & 1) != 0) // 栈在复制期间改变。
    { // 拒绝混合栈。
        Evidence->Context.StackStatus = STATUS_RETRY; // 不提供伪造帧。
        Evidence->Flags |= KSWORD_CONTEXT_RACE_FLAG; // 可解释的竞争状态。
        return; // 不等待下一次尝试。
    } // 稳定栈检查结束。
    Evidence->Context.StackStatus = stack.Status; // 保留正常期捕栈失败原因。
    Evidence->Context.StackThreadId = stack.ThreadId; // 明确所有者。
    Evidence->Context.StackSampleTime = stack.Time; // 明确旧缓存时间。
    if (stack.Count == 0 || stack.Count > KSWORD_BUGCHECK_EVIDENCE_STACK || !NT_SUCCESS(stack.Status)) return; // 缺失或损坏数量不能当有效。
    Evidence->Context.StackCount = stack.Count; // 固定数量。
    Evidence->Context.StackSource = stack.ThreadId == (ULONG64)Evidence->ThreadId ? 4UL : 3UL; // 即使同线程也只是崩溃前正常操作栈。
    RtlCopyMemory(Evidence->Context.Stack, stack.Frames, sizeof(stack.Frames)); // 保留全部固定项。
} // 有界上下文快照结束。
