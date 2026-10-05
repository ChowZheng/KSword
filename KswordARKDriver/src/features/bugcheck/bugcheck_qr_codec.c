// KSQ2：固定 schema、逐字段小端编码、CRC32 和可复制 Base45，不在崩溃路径压缩或分配。
#include "bugcheck_qr_codec.h"
#include "bugcheck_decode.h"

typedef struct _KSWORD_QR_WRITER
{
    UCHAR* Data; // 调用方提供的常驻二进制缓冲。
    ULONG Capacity; // 真实缓冲大小，不依赖结构 sizeof。
    ULONG Length; // 已完整写入的二进制字节数。
    NTSTATUS Status; // 保存首次完整性失败，后续写入不再推进。
} KSWORD_QR_WRITER;

static const CHAR g_KswordQrBase45[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:"; // QR 字母数字模式的完整字符表。

// 所有字段写入最终复用这一个严格边界，不能因超限默默丢掉尾部证据。
static VOID KswordQrByte(_Inout_ KSWORD_QR_WRITER* Writer, _In_ UCHAR Value)
{
    if (!NT_SUCCESS(Writer->Status)) { // 首次失败后不改动部分报告。
        return; // 由入口统一清除输出长度和文本。
    }
    if (Writer->Length >= Writer->Capacity) { // 每一个字节都检查调用方真实容量。
        Writer->Status = STATUS_BUFFER_TOO_SMALL; // 记录完整包无法容纳。
        return; // 不以截断伪装编码成功。
    }
    Writer->Data[Writer->Length++] = Value; // 写入一个完整字段字节。
}

// 数字无条件按小端固定宽度输出，避免主机 ABI、padding 或别名访问差异。
static VOID KswordQrUnsigned(_Inout_ KSWORD_QR_WRITER* Writer, _In_ ULONG64 Value, _In_ ULONG Bytes)
{
    ULONG index; // 当前低位字节编号。
    for (index = 0UL; index < Bytes; ++index) { // Bytes 仅由本模块的固定 4/8 字节调用提供。
        KswordQrByte(Writer, (UCHAR)(Value & 255ULL)); // 输出一个低位字节。
        Value >>= 8UL; // 每次推进八位，不进行类型指针转换。
    }
}

// 原始字符数组仅扫描真实容量；缺少 NUL 时全部字节保留，手机解码后再展示转义。
static VOID KswordQrString(_Inout_ KSWORD_QR_WRITER* Writer,
    _In_reads_(Capacity) const CHAR* Value, _In_ ULONG Capacity, _In_ BOOLEAN RequireNul)
{
    ULONG length = 0UL; // 文本长度最多 160 字节，适合一个无符号长度字节。
    ULONG index; // 当前原始文本字节编号。
    while (length < Capacity && Value[length] != '\0') { // 固定数组从不读取边界之外。
        ++length; // 包含所有 NUL 前的真实文本字节。
    }
    if (Capacity > 255UL || (RequireNul && length == Capacity)) { // 常量名称不得被有限扫描默默截断。
        Writer->Status = STATUS_BUFFER_TOO_SMALL; // 要求调用方扩展 schema 而非丢失语义。
        return; // 数值字段和文本字段仍由完整包失败门禁保护。
    }
    KswordQrByte(Writer, (UCHAR)length); // 一字节前缀明确保存后续真实文本长度。
    for (index = 0UL; index < length; ++index) { // 没有 NUL 的固定数组也完整写入。
        KswordQrByte(Writer, (UCHAR)Value[index]); // 不转义、不改变 UTF-8 或非 ASCII 的原始字节。
    }
}

// 基础诊断按照头文件成员顺序完整输出，并附加原解码器语义及 Stop Code 常量名称。
static VOID KswordQrDiagnostics(_Inout_ KSWORD_QR_WRITER* Writer,
    _In_ const KSWORD_ARK_BUGCHECK_DIAGNOSTICS* Diagnostics, _In_ ULONG CallbackMask, _In_ ULONG ModuleCount)
{
    ULONG index; // 四个参数语义的固定编号。
#define QR_D32(Field) KswordQrUnsigned(Writer, (ULONG)Diagnostics->Field, 4UL) // 完整保存 32 位原始字段。
#define QR_D64(Field) KswordQrUnsigned(Writer, (ULONG64)Diagnostics->Field, 8UL) // 完整保存 64 位值和指针数值。
#define QR_DS(Field) KswordQrString(Writer, Diagnostics->Field, (ULONG)sizeof(Diagnostics->Field), FALSE) // 有界固定数组文本。
    QR_D32(Captured); // 采集发布标志保留其原始 LONG 位模式。
    QR_D32(BugCheckCode); // 原始数值 Stop Code。
    QR_D64(Parameter1); // 参数一包括有语义或保留的零值。
    QR_D64(Parameter2); // 参数二原始位模式。
    QR_D64(Parameter3); // 参数三原始位模式。
    QR_D64(Parameter4); // 参数四原始位模式。
    QR_D64(FaultAddress); // 已安全解码的故障地址。
    QR_D32(FaultParameter); // 地址来自哪个参数。
    QR_DS(FaultMeaning); // 既有纯解码器提供的语义原文。
    QR_D32(LastReason); // 最新崩溃回调原因。
    QR_D32(LastDumpType); // 最新转储类型。
    QR_D32(DumpBufferLength); // 已观察转储块的大小。
    QR_D64(DumpOffset); // 完整转储偏移。
    QR_D32(Irql); // 回调采集 IRQL。
    QR_D32(Cpu); // 原始逻辑处理器编号。
    KswordQrUnsigned(Writer, (ULONG64)Diagnostics->PerfCounter.QuadPart, 8UL); // 不改变原始计数器位模式。
    QR_D64(ProcessObject); // 仅记录数值，不读取该对象。
    QR_D64(ProcessId); // 原始进程标识。
    QR_D32(ProcessSource); // 关键进程和崩溃上下文的来源区别。
    QR_DS(ProcessName); // 全部已缓存进程短名。
    QR_D64(CandidateAddress); // 已有安全归因地址。
    QR_D64(CandidateModuleBase); // 已缓存映像基址。
    QR_D64(CandidateModuleOffset); // 映像内偏移。
    QR_D32(CandidateModuleSize); // 映像范围。
    QR_D32(CandidateParameter); // 候选地址来源参数号。
    QR_D32(CandidateClass); // 原分类不提升为根因声明。
    QR_D32(CandidateConfidence); // 原归因置信度。
    QR_DS(CandidateModule); // 原始候选模块文本。
    QR_DS(CandidateSource); // 原始安全归因来源说明。
#undef QR_D32
#undef QR_D64
#undef QR_DS
    KswordQrUnsigned(Writer, CallbackMask, 4UL); // 当前回调位图。
    KswordQrUnsigned(Writer, ModuleCount, 4UL); // 当前模块缓存计数。
    KswordQrString(Writer, KswordARKBugcheckName(Diagnostics->BugCheckCode), 64UL, TRUE); // Stop Code 名称必须完整。
    for (index = 1UL; index <= 4UL; ++index) { // 完整保留原有四个参数语义，即使参数为零或保留。
        KswordQrString(Writer, KswordARKBugcheckDecodeParameterRole(Diagnostics, index), 32UL, TRUE); // 不扩展对象解引用或归因。
    }
}

// BGP 所有标量、签名和时间线槽位逐项序列化，计数不驱动越界遍历。
static VOID KswordQrBgp(_Inout_ KSWORD_QR_WRITER* Writer, _In_ const KSWORD_ARK_BGP_DUMP_STATE* Bgp)
{
    ULONG index; // 固定数组内的槽位编号。
#define QR_B32(Field) KswordQrUnsigned(Writer, Bgp->Field, 4UL) // 原始 BGP 四字节字段。
    QR_B32(Version); // BGP 转储状态版本。
    QR_B32(Size); // BGP 状态结构声明大小。
    QR_B32(State); // 后端状态机。
    QR_B32(PreparationStage); // 运行期资源准备阶段。
    QR_B32(PreparationStatus); // 准备操作状态。
    QR_B32(Stage); // 当前崩溃绘制阶段。
    QR_B32(LastStatus); // 最新操作状态。
    QR_B32(ClearStatus); // 清屏状态。
    QR_B32(DrawStatus); // 已记录绘制状态。
    QR_B32(FeatureMask); // 私有后端已解析功能。
    QR_B32(ScreenWidth); // 实际显示宽度。
    QR_B32(ScreenHeight); // 实际显示高度。
    QR_B32(ScreenBpp); // 实际位深。
    QR_B32(RequiredWidth); // 准备最低宽度。
    QR_B32(RequiredHeight); // 准备最低高度。
    KswordQrUnsigned(Writer, Bgp->DrawCount, 8UL); // 完整 64 位绘制计数。
    for (index = 0UL; index < KSWORD_ARK_BGP_SIGNATURE_COUNT; ++index) { // 保留全部八个签名槽位。
        KswordQrUnsigned(Writer, Bgp->SignatureFamily[index], 4UL); // 原始签名族编号。
    }
    QR_B32(TimelineCount); // 真实时间线计数独立保留。
    for (index = 0UL; index < KSWORD_ARK_BGP_TIMELINE_COUNT; ++index) { // 固定十六槽位不会受异常计数影响。
        KswordQrUnsigned(Writer, Bgp->Timeline[index].Stage, 4UL); // 该槽位阶段。
        KswordQrUnsigned(Writer, Bgp->Timeline[index].Status, 4UL); // 该槽位状态。
    }
#undef QR_B32
}

// 环境所有字段独立保留状态和有效位，未知值不会冒充已确认的零值。
static VOID KswordQrEnvironment(_Inout_ KSWORD_QR_WRITER* Writer, _In_ const KSWORD_BUGCHECK_ENVIRONMENT* Environment)
{
#define QR_E32(Field) KswordQrUnsigned(Writer, (ULONG)Environment->Field, 4UL) // 原始环境四字节字段。
    QR_E32(Flags); // 各环境证据有效标志。
    QR_E32(VersionStatus); // 系统版本采集状态。
    QR_E32(UbrStatus); // 注册表修订号采集的独立原始状态。
    QR_E32(Major); // Windows 主版本。
    QR_E32(Minor); // Windows 次版本。
    QR_E32(Build); // Windows build。
    QR_E32(Ubr); // Windows 修订号。
    QR_E32(ProductType); // 产品类别。
    QR_E32(ProcessorCount); // 正常运行期处理器数量。
    KswordQrUnsigned(Writer, Environment->SampleTime, 8UL); // 运行期采样时间。
    KswordQrUnsigned(Writer, Environment->PerformanceFrequency, 8UL); // 性能计数器频率。
    QR_E32(CiStatus); // Code Integrity 查询状态。
    QR_E32(CiOptions); // 保留 CI 原始位图。
    QR_E32(SecureBootStatus); // Secure Boot 查询状态。
    QR_E32(SecureBoot); // 开关值需与状态合并解释。
    QR_E32(HypervisorPresent); // CPUID Hypervisor 原始位。
#undef QR_E32
    KswordQrString(Writer, Environment->HypervisorVendor, (ULONG)sizeof(Environment->HypervisorVendor), FALSE); // 完整供应商字节。
    KswordQrString(Writer, Environment->DriverBuild, (ULONG)sizeof(Environment->DriverBuild), FALSE); // 完整驱动构建说明。
}

// 三个映像身份均保存原始 PE/RSDS 字段和路径标志，不根据名称猜测来源。
static VOID KswordQrImage(_Inout_ KSWORD_QR_WRITER* Writer, _In_ const KSWORD_BUGCHECK_IMAGE_ID* Image)
{
    ULONG index; // GUID 原始十六字节索引。
    KswordQrUnsigned(Writer, Image->Flags, 4UL); // PE、RSDS、路径、截断有效位。
    KswordQrUnsigned(Writer, (ULONG)Image->Status, 4UL); // 采集状态原始 NTSTATUS。
    KswordQrUnsigned(Writer, Image->Base, 8UL); // 映像基址。
    KswordQrUnsigned(Writer, Image->ImageSize, 4UL); // PE 映像大小。
    KswordQrUnsigned(Writer, Image->TimeDateStamp, 4UL); // PE 编译时间戳。
    KswordQrUnsigned(Writer, Image->Checksum, 4UL); // PE 原始校验和。
    for (index = 0UL; index < 16UL; ++index) { // GUID 不经过易混淆的主机字节序转换。
        KswordQrByte(Writer, Image->PdbGuid[index]); // 保存 RSDS GUID 原始字节。
    }
    KswordQrUnsigned(Writer, Image->PdbAge, 4UL); // 精确 PDB Age。
    KswordQrString(Writer, Image->Path, (ULONG)sizeof(Image->Path), FALSE); // 原路径与截断标志同时保留。
    KswordQrString(Writer, Image->PdbName, (ULONG)sizeof(Image->PdbName), FALSE); // 完整有界 PDB 名称。
}

// 寄存器和栈始终保留来源、状态、有效位以及全部固定槽位，不把回调现场当故障现场。
static VOID KswordQrContext(_Inout_ KSWORD_QR_WRITER* Writer, _In_ const KSWORD_BUGCHECK_CONTEXT_EVIDENCE* Context)
{
    ULONG index; // 寄存器和栈的固定槽位索引。
    KswordQrUnsigned(Writer, Context->ContextSource, 4UL); // 系统转储头或回调上下文来源。
    KswordQrUnsigned(Writer, (ULONG)Context->ContextStatus, 4UL); // 寄存器解析状态。
    KswordQrUnsigned(Writer, Context->RegisterMask, 4UL); // 各寄存器有效位。
    KswordQrUnsigned(Writer, Context->StackSource, 4UL); // 可靠展开与原始候选的区别。
    KswordQrUnsigned(Writer, (ULONG)Context->StackStatus, 4UL); // 栈读取或解析状态。
    KswordQrUnsigned(Writer, Context->StackCount, 4UL); // 原始采集槽位计数。
    KswordQrUnsigned(Writer, Context->StackThreadId, 8UL); // 运行期栈所属线程独立保留。
    KswordQrUnsigned(Writer, Context->StackSampleTime, 8UL); // 栈采样时间不能混同崩溃采样时间。
    for (index = 0UL; index < KSWORD_BUGCHECK_EVIDENCE_REGISTERS; ++index) { // 全部十八个寄存器槽位。
        KswordQrUnsigned(Writer, Context->Registers[index], 8UL); // 原始寄存器数值。
    }
    for (index = 0UL; index < KSWORD_BUGCHECK_EVIDENCE_STACK; ++index) { // 全部十六个栈槽位。
        KswordQrUnsigned(Writer, Context->Stack[index], 8UL); // 未经转换的地址或值。
    }
}

// 所有最近事件都保留序列、时间、状态、来源和完整字节；计数不决定扫描边界。
static VOID KswordQrEvent(_Inout_ KSWORD_QR_WRITER* Writer, _In_ const KSWORD_BUGCHECK_EVENT* Event)
{
    KswordQrUnsigned(Writer, Event->Sequence, 8UL); // 全局单调序列号。
    KswordQrUnsigned(Writer, Event->Time, 8UL); // 启动后采样时间。
    KswordQrUnsigned(Writer, Event->Kind, 4UL); // 事件类别。
    KswordQrUnsigned(Writer, Event->Code, 4UL); // 原 IOCTL/组件编号。
    KswordQrUnsigned(Writer, (ULONG)Event->Status, 4UL); // 原始操作结果或调试级别。
    KswordQrUnsigned(Writer, Event->Flags, 4UL); // 截断、缺失和上下文有效标志。
    KswordQrUnsigned(Writer, Event->ProcessId, 8UL); // 原进程标识。
    KswordQrUnsigned(Writer, Event->ThreadId, 8UL); // 原线程标识。
    KswordQrString(Writer, Event->Text, (ULONG)sizeof(Event->Text), FALSE); // 保留全部固定摘要字节。
}

// 整个扩展证据逐成员写入，不直接 memcpy 含 padding 的 C 结构。
static VOID KswordQrEvidence(_Inout_ KSWORD_QR_WRITER* Writer, _In_ const KSWORD_BUGCHECK_EVIDENCE* Evidence)
{
    ULONG index; // 最近事件固定槽位编号。
    KswordQrUnsigned(Writer, Evidence->Version, 4UL); // 原扩展证据版本。
    KswordQrUnsigned(Writer, Evidence->Flags, 4UL); // 原证据整体状态。
    KswordQrUnsigned(Writer, Evidence->CaptureTime, 8UL); // 崩溃采样时间。
    KswordQrUnsigned(Writer, Evidence->CpuGroup, 4UL); // 处理器组。
    KswordQrUnsigned(Writer, Evidence->CpuNumber, 4UL); // 组内处理器编号。
    KswordQrUnsigned(Writer, Evidence->ThreadId, 8UL); // 回调上下文线程标识。
    KswordQrUnsigned(Writer, Evidence->ProcessCacheFlags, 4UL); // 进程命中、退出与缓存状态。
    KswordQrUnsigned(Writer, Evidence->ProcessSampleTime, 8UL); // 对应缓存更新时间。
    KswordQrEnvironment(Writer, &Evidence->Environment); // 完整正常运行期环境。
    KswordQrImage(Writer, &Evidence->Kernel); // 内核 PE/RSDS 身份。
    KswordQrImage(Writer, &Evidence->Driver); // KSword 驱动 PE/RSDS 身份。
    KswordQrImage(Writer, &Evidence->Candidate); // 候选模块 PE/RSDS 身份。
    KswordQrContext(Writer, &Evidence->Context); // 寄存器/栈来源及完整固定数组。
    KswordQrUnsigned(Writer, Evidence->EventCount, 4UL); // 真实事件数量。
    KswordQrUnsigned(Writer, Evidence->EventsDropped, 8UL); // 采集环丢弃或覆盖次数。
    KswordQrUnsigned(Writer, Evidence->EventsOverwritten, 8UL); // 正常覆盖旧槽位的独立计数。
    KswordQrUnsigned(Writer, Evidence->EventsDiscarded, 8UL); // 竞争或来源不可用的独立丢弃计数。
    for (index = 0UL; index < KSWORD_BUGCHECK_EVIDENCE_EVENTS; ++index) { // 六个槽位全部保留。
        KswordQrEvent(Writer, &Evidence->Events[index]); // 计数异常也不会引发数组越界。
    }
}

// CRC32/ISO-HDLC 位运算不使用外部运行库或大查找表。
static ULONG KswordQrCrcStep(_In_ ULONG Crc, _In_ UCHAR Byte)
{
    ULONG bit; // 当前输入字节中的位编号。
    Crc ^= Byte; // 先吸收该字节的原始位模式。
    for (bit = 0UL; bit < 8UL; ++bit) { // 每字节固定八次，运行时间有界。
        Crc = (Crc >> 1UL) ^ ((Crc & 1UL) != 0UL ? 0xEDB88320UL : 0UL); // 标准反射多项式。
    }
    return Crc; // 返回未最终异或的滚动状态。
}

ULONG KswordARKBugcheckQrCrc32(_In_reads_bytes_(Length) const UCHAR* Data, _In_ ULONG Length)
{
    ULONG crc = 0xFFFFFFFFUL; // CRC32 标准初值。
    ULONG index; // 当前有界字节偏移。
    for (index = 0UL; index < Length; ++index) { // 不读数据声明边界之外。
        crc = KswordQrCrcStep(crc, Data[index]); // 更新一个字节。
    }
    return crc ^ 0xFFFFFFFFUL; // CRC32 标准最终异或。
}

// 已保留的头字节按同样的小端格式填写，不改变顺序序列化游标。
static VOID KswordQrHeader(_Inout_updates_(KSWORD_ARK_QR_HEADER_BYTES) UCHAR* Data,
    _In_ ULONG Offset, _In_ ULONG Value, _In_ ULONG Bytes)
{
    ULONG index; // 头部内固定偏移。
    for (index = 0UL; index < Bytes; ++index) { // 调用点仅传入二或四字节。
        Data[Offset + index] = (UCHAR)(Value & 255UL); // 写入一个低位字节。
        Value >>= 8UL; // 推进下一个固定字段字节。
    }
}

// 五字符前缀与 Base45 均属于 QR 字母数字集合，普通扫描器可以复制，不是压缩。
static NTSTATUS KswordQrBase45(_In_reads_bytes_(Length) const UCHAR* Binary, _In_ ULONG Length,
    _Out_writes_bytes_(Capacity) CHAR* Text, _In_ ULONG Capacity, _Out_ PULONG TextLength)
{
    ULONG encoded = 5UL + (Length / 2UL) * 3UL + (Length % 2UL) * 2UL; // 偶数字节三字符，余一字节两字符。
    ULONG source = 0UL; // 当前二进制字节位置。
    ULONG target = 5UL; // 跳过固定 ASCII 包前缀。
    if (encoded > 4296UL || encoded >= Capacity) { // 同时保护 QR 硬上限和结尾 NUL。
        return STATUS_BUFFER_TOO_SMALL; // 不输出部分封装。
    }
    Text[0] = 'K'; // 前缀 K。
    Text[1] = 'S'; // 前缀 S。
    Text[2] = 'Q'; // 前缀 Q。
    Text[3] = '2'; // schema 主版本。
    Text[4] = ':'; // Base45 分隔符属于字母数字字符集。
    while (source + 1UL < Length) { // 两字节组有界编码。
        ULONG value = (ULONG)Binary[source] * 256UL + Binary[source + 1UL]; // RFC9285 的双字节大端合成数。
        Text[target++] = g_KswordQrBase45[value % 45UL]; // 最低 Base45 位。
        value /= 45UL; // 推进中间 Base45 位。
        Text[target++] = g_KswordQrBase45[value % 45UL]; // 中间 Base45 位。
        Text[target++] = g_KswordQrBase45[value / 45UL]; // 最高 Base45 位。
        source += 2UL; // 每组恰好消耗两个字节。
    }
    if (source < Length) { // 奇数长度最后一个字节只需两位。
        ULONG value = Binary[source]; // 单字节值不超过 255。
        Text[target++] = g_KswordQrBase45[value % 45UL]; // 单字节的低位。
        Text[target++] = g_KswordQrBase45[value / 45UL]; // 单字节的高位。
    }
    Text[target] = '\0'; // 严格保存最后一个字符之后的 NUL。
    *TextLength = target; // 只有完整封装成功后发布长度。
    return STATUS_SUCCESS; // 手机可复制的完整封装已建立。
}

// 完整性失败由调用方保留明确错误状态；不能把前半份诊断当作完整诊断绘制。
NTSTATUS KswordARKBugcheckQrEncode(
    _In_ const KSWORD_ARK_BUGCHECK_DIAGNOSTICS* Diagnostics,
    _In_ ULONG CallbackMask, _In_ ULONG ModuleCount,
    _In_opt_ const KSWORD_ARK_BGP_DUMP_STATE* Bgp,
    _In_opt_ const KSWORD_BUGCHECK_EVIDENCE* Evidence,
    _Out_writes_bytes_(BinaryCapacity) UCHAR* Binary, _In_ ULONG BinaryCapacity,
    _Out_writes_bytes_(TextCapacity) CHAR* Text, _In_ ULONG TextCapacity,
    _Out_ PULONG BinaryLength, _Out_ PULONG TextLength)
{
    KSWORD_QR_WRITER writer; // 小游标在栈上，大输出由调用方常驻。
    ULONG flags = 0UL; // 可选快照明确写入头部，不用零值猜测是否存在。
    ULONG crc = 0xFFFFFFFFUL; // 头部及正文的滚动 CRC 初值。
    ULONG index; // CRC 和固定头保留槽位索引。
    NTSTATUS status; // 完整 Base45 封装结果。
    if (Diagnostics == NULL || Binary == NULL || Text == NULL || BinaryLength == NULL ||
        TextLength == NULL || BinaryCapacity < KSWORD_ARK_QR_HEADER_BYTES || TextCapacity == 0UL) { // 严格检查写入边界。
        return STATUS_INVALID_PARAMETER; // 无有效缓冲时不进行任何写入。
    }
    *BinaryLength = 0UL; // 失败时始终不发布部分二进制长度。
    *TextLength = 0UL; // 失败时始终不发布部分手机文本长度。
    Text[0] = '\0'; // 防止旧二维码封装被当成新快照。
    writer.Data = Binary; // 绑定调用方固定二进制缓冲。
    writer.Capacity = BinaryCapacity; // 保存实际容量。
    writer.Length = 0UL; // 从固定头开始序列化。
    writer.Status = STATUS_SUCCESS; // 初始没有字段失败。
    for (index = 0UL; index < KSWORD_ARK_QR_HEADER_BYTES; ++index) { // 为版本、长度和 CRC 预留完整头。
        KswordQrByte(&writer, 0U); // 显式初始化全部头字节，不泄露工作区旧内容。
    }
    KswordQrDiagnostics(&writer, Diagnostics, CallbackMask, ModuleCount); // 完整基础诊断和解码语义。
    if (Bgp != NULL) { // 可选 BGP 由有效指针明确标记。
        flags |= KSWORD_ARK_QR_FLAG_BGP; // 头部保留存在位。
        KswordQrBgp(&writer, Bgp); // 所有 BGP 字段和固定数组。
    }
    if (Evidence != NULL) { // 可选扩展证据同样不以字段零值猜测存在。
        flags |= KSWORD_ARK_QR_FLAG_EVIDENCE; // 头部保留存在位。
        KswordQrEvidence(&writer, Evidence); // 完整环境、身份、寄存器、栈和六个事件。
    }
    if (!NT_SUCCESS(writer.Status) || writer.Length > KSWORD_ARK_QR_BINARY_CAPACITY) { // 任意字段超载均整体拒绝。
        return NT_SUCCESS(writer.Status) ? STATUS_BUFFER_TOO_SMALL : writer.Status; // 保留首个精确失败状态。
    }
    Binary[0] = 'K'; // 固定二进制 magic。
    Binary[1] = 'S'; // 固定二进制 magic。
    Binary[2] = 'Q'; // 固定二进制 magic。
    Binary[3] = '2'; // 固定二进制 magic。
    KswordQrHeader(Binary, 4UL, KSWORD_ARK_QR_SCHEMA_VERSION, 2UL); // 明确小端二字节 schema 版本。
    KswordQrHeader(Binary, 6UL, flags, 2UL); // 明确小端二字节快照存在位。
    KswordQrHeader(Binary, 8UL, writer.Length, 4UL); // 保存完整二进制包总长度，包括十六字节头。
    for (index = 0UL; index < writer.Length; ++index) { // CRC 覆盖头部与所有数据。
        if (index < 12UL || index >= 16UL) { // 唯一排除 CRC32 自身的四个字节。
            crc = KswordQrCrcStep(crc, Binary[index]); // 编码字段变更都必须改变 CRC。
        }
    }
    KswordQrHeader(Binary, 12UL, crc ^ 0xFFFFFFFFUL, 4UL); // 写入完整包 CRC32。
    status = KswordQrBase45(Binary, writer.Length, Text, TextCapacity, TextLength); // 转为全 ASCII 可复制封装。
    if (!NT_SUCCESS(status)) { // 封装超载同样不发布残缺包。
        Text[0] = '\0'; // 清除不完整手机文本。
        *TextLength = 0UL; // 不发布部分文本长度。
        return status; // 保留容量失败状态。
    }
    *BinaryLength = writer.Length; // 只在全部字段与 CRC/封装完成后发布二进制长度。
    return STATUS_SUCCESS; // 所有快照完整进入单个二维码封装。
}
