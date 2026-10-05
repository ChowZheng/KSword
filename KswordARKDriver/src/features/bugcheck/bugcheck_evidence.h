#pragma once

#include "bugcheck_internal.h"

// 扩展证据在正常运行期准备；崩溃阶段只读取固定非分页副本，绝不等待写锁。
#define KSWORD_BUGCHECK_EVIDENCE_VERSION 1UL
#define KSWORD_BUGCHECK_EVIDENCE_PATH_BYTES 160UL
#define KSWORD_BUGCHECK_EVIDENCE_PDB_BYTES 64UL
#define KSWORD_BUGCHECK_EVIDENCE_EVENTS 6UL
#define KSWORD_BUGCHECK_EVIDENCE_EVENT_TEXT 64UL
#define KSWORD_BUGCHECK_EVIDENCE_STACK 16UL
#define KSWORD_BUGCHECK_EVIDENCE_REGISTERS 18UL

// 身份有效位分别表示 PE、RSDS、路径；截断路径必须同时设置 TRUNCATED。
#define KSWORD_BUGCHECK_ID_PE 1UL
#define KSWORD_BUGCHECK_ID_RSDS 2UL
#define KSWORD_BUGCHECK_ID_PATH 4UL
#define KSWORD_BUGCHECK_ID_TRUNCATED 8UL

// 环境字段有效位：只有对应查询成功才设置，失败状态仍随二维码保留。
#define KSWORD_BUGCHECK_ENV_VERSION 1UL
#define KSWORD_BUGCHECK_ENV_UBR 2UL
#define KSWORD_BUGCHECK_ENV_CPU 4UL
#define KSWORD_BUGCHECK_ENV_PERF 8UL
#define KSWORD_BUGCHECK_ENV_CI 16UL
#define KSWORD_BUGCHECK_ENV_SECUREBOOT 32UL
#define KSWORD_BUGCHECK_ENV_HYPERVISOR 64UL
#define KSWORD_BUGCHECK_ENV_DRIVERBUILD 128UL
// 扩展快照和进程缓存状态互相独立，保留采样冲突与退出状态。
#define KSWORD_BUGCHECK_EVIDENCE_VALID 1UL
#define KSWORD_BUGCHECK_EVIDENCE_RACE 2UL
#define KSWORD_BUGCHECK_EVIDENCE_CACHE_PARTIAL 4UL
#define KSWORD_BUGCHECK_PROCESS_HIT 1UL
#define KSWORD_BUGCHECK_PROCESS_EXITING 2UL
#define KSWORD_BUGCHECK_PROCESS_STABLE 4UL
// 环形事件有效位与覆盖/丢弃位不会复用其他证据的低位。
#define KSWORD_BUGCHECK_EVIDENCE_TRACE_PRESENT 0x100UL
#define KSWORD_BUGCHECK_EVIDENCE_TRACE_RACE 0x200UL
#define KSWORD_BUGCHECK_EVIDENCE_TRACE_OVERWRITTEN 0x400UL
#define KSWORD_BUGCHECK_EVIDENCE_TRACE_DROPPED 0x800UL
#define KSWORD_BUGCHECK_EVIDENCE_DBG_CAPTURE_ACTIVE 0x1000UL
#define KSWORD_BUGCHECK_EVIDENCE_DBG_CAPTURE_DISABLED 0x2000UL
#define KSWORD_BUGCHECK_TRACE_KIND_IOCTL_BEGIN 1UL
#define KSWORD_BUGCHECK_TRACE_KIND_IOCTL_END 2UL
#define KSWORD_BUGCHECK_TRACE_KIND_DBGPRINT 3UL
#define KSWORD_BUGCHECK_TRACE_KIND_DRIVER_LOG 4UL
#define KSWORD_BUGCHECK_TRACE_KIND_DBGPRINT_STATE 5UL
#define KSWORD_BUGCHECK_TRACE_EVENT_TEXT_TRUNCATED 1UL
#define KSWORD_BUGCHECK_TRACE_EVENT_CONTEXT_VALID 2UL
#define KSWORD_BUGCHECK_TRACE_EVENT_STATUS_UNAVAILABLE 4UL

typedef struct _KSWORD_BUGCHECK_IMAGE_ID
{
    ULONG Flags; // 指明已取得的身份字段与文本截断状态。
    NTSTATUS Status; // 最近一次身份采集的结果，不把失败表示为零版本。
    ULONG_PTR Base; // 缓存映像的加载基址。
    ULONG ImageSize; // PE 声明的映像范围。
    ULONG TimeDateStamp; // PE 编译时间戳。
    ULONG Checksum; // PE 原始校验和。
    UCHAR PdbGuid[16]; // RSDS 标识的原始 GUID 字节。
    ULONG PdbAge; // RSDS 标识的 Age。
    CHAR Path[KSWORD_BUGCHECK_EVIDENCE_PATH_BYTES]; // 有界 UTF-8/ASCII 路径副本。
    CHAR PdbName[KSWORD_BUGCHECK_EVIDENCE_PDB_BYTES]; // 有界 PDB 文件名称。
} KSWORD_BUGCHECK_IMAGE_ID;

typedef struct _KSWORD_BUGCHECK_ENVIRONMENT
{
    ULONG Flags; // 字段有效位由采集实现与解码器共同描述。
    NTSTATUS VersionStatus; // 系统版本采集结果。
    NTSTATUS UbrStatus; // 系统修订号采集结果，缺失有效位时保留原始失败原因。
    ULONG Major; // Windows 主版本。
    ULONG Minor; // Windows 次版本。
    ULONG Build; // Windows 构建号。
    ULONG Ubr; // Windows 修订号。
    ULONG ProductType; // 客户端或服务器类型。
    ULONG ProcessorCount; // 采集时的逻辑处理器数量。
    ULONG64 SampleTime; // 采集时启动后 100ns 时间。
    ULONG64 PerformanceFrequency; // 性能计数器频率。
    NTSTATUS CiStatus; // Code Integrity 查询结果。
    ULONG CiOptions; // 原始 CI 位图，保留测试签名及 HVCI 位。
    NTSTATUS SecureBootStatus; // Secure Boot 查询结果。
    ULONG SecureBoot; // 安全启动开关，需同时检查状态码。
    ULONG HypervisorPresent; // CPUID Hypervisor 位。
    CHAR HypervisorVendor[16]; // CPUID 提供的有界供应商名称。
    CHAR DriverBuild[48]; // KSword 格式版本与 PE 构建身份；确定性时间戳不解释为日期。
} KSWORD_BUGCHECK_ENVIRONMENT;

typedef struct _KSWORD_BUGCHECK_EVENT
{
    ULONG64 Sequence; // 全局单调事件序号。
    ULONG64 Time; // 启动后 100ns 时间。
    ULONG Kind; // 事件类别：IOCTL、调试输出、进程或映像等。
    ULONG Code; // IOCTL 编号或调试组件编号。
    NTSTATUS Status; // 操作结果或调试级别。
    ULONG Flags; // 截断、缺失和上下文有效位。
    ULONG_PTR ProcessId; // 记录时的进程标识。
    ULONG_PTR ThreadId; // 记录时的线程标识。
    CHAR Text[KSWORD_BUGCHECK_EVIDENCE_EVENT_TEXT]; // 有界原始事件摘要。
} KSWORD_BUGCHECK_EVENT;

// ContextSource 为 0=缺失、1=系统转储头、2=回调上下文；绝不混同故障现场。
typedef struct _KSWORD_BUGCHECK_CONTEXT_EVIDENCE
{
    ULONG ContextSource; // 寄存器证据来源。
    NTSTATUS ContextStatus; // 寄存器缺失或解析失败原因。
    ULONG RegisterMask; // Registers 中有效寄存器的位图。
    ULONG StackSource; // 0=缺失、1=故障展开、2=原始候选、3=最近操作、4=同线程崩溃前操作。
    NTSTATUS StackStatus; // 栈证据缺失或读取失败原因。
    ULONG StackCount; // 已采集的固定栈项数量。
    ULONG64 StackThreadId; // 运行期栈捕获所属线程，不能据此推定故障线程。
    ULONG64 StackSampleTime; // 运行期栈采样时间，启动后 100ns。
    ULONG64 Registers[KSWORD_BUGCHECK_EVIDENCE_REGISTERS]; // RAX..R15、RIP、EFLAGS 固定顺序。
    ULONG64 Stack[KSWORD_BUGCHECK_EVIDENCE_STACK]; // 有界栈项；不把原始候选称为调用栈。
} KSWORD_BUGCHECK_CONTEXT_EVIDENCE;

typedef struct _KSWORD_BUGCHECK_EVIDENCE
{
    ULONG Version; // 扩展证据结构版本。
    ULONG Flags; // 证据整体的有效、冲突及截断标记。
    ULONG64 CaptureTime; // 崩溃回调采样时的启动后时间。
    ULONG CpuGroup; // 回调处理器组。
    ULONG CpuNumber; // 回调组内处理器编号。
    ULONG_PTR ThreadId; // 回调上下文线程标识。
    ULONG ProcessCacheFlags; // 命中、退出及缓存有效状态。
    ULONG64 ProcessSampleTime; // 对应进程缓存更新时间。
    KSWORD_BUGCHECK_ENVIRONMENT Environment; // 运行期环境快照。
    KSWORD_BUGCHECK_IMAGE_ID Kernel; // 当前内核映像身份。
    KSWORD_BUGCHECK_IMAGE_ID Driver; // 当前 KSword 驱动映像身份。
    KSWORD_BUGCHECK_IMAGE_ID Candidate; // 当前候选模块身份。
    KSWORD_BUGCHECK_CONTEXT_EVIDENCE Context; // 系统提供的现场或明确缺失状态。
    ULONG EventCount; // 固定最近事件数量。
    ULONG64 EventsDropped; // 采集环的丢弃或覆盖计数。
    ULONG64 EventsOverwritten; // 正常覆盖旧事件的独立计数。
    ULONG64 EventsDiscarded; // 写入竞争或序号耗尽造成的独立整条丢弃计数。
    KSWORD_BUGCHECK_EVENT Events[KSWORD_BUGCHECK_EVIDENCE_EVENTS]; // 不等待锁的最近事件副本。
} KSWORD_BUGCHECK_EVIDENCE;

// PASSIVE_LEVEL 安装准备调用，建立环境与已加载映像身份；不会创建目录或触发崩溃。
VOID KswordARKBugcheckEvidenceInitialize(_In_ PDRIVER_OBJECT DriverObject);
// 正常运行期映像加载回调调用，FullPath 可为空；只在许可 IRQL 解析 PE/RSDS。
VOID KswordARKBugcheckEvidenceImage(_In_ ULONG_PTR Base, _In_ ULONG Size, _In_opt_ PCUNICODE_STRING FullPath);
// 崩溃采集调用，目标是驱动常驻固定副本；只进行有界原子快照。
VOID KswordARKBugcheckEvidenceCapture(_In_ const KSWORD_ARK_BUGCHECK_DIAGNOSTICS* Diagnostics);
// 返回最近已发布的扩展快照；返回地址常驻非分页，不允许调用方修改。
const KSWORD_BUGCHECK_EVIDENCE* KswordARKBugcheckEvidenceSnapshot(VOID);
// 读取系统提供的 DumpIo 头数据，不解引用 BugCheck 参数中的任意指针。
VOID KswordARKBugcheckEvidenceDumpIo(_In_ const KBUGCHECK_DUMP_IO* DumpIo);
// 安装时重置转储头拼接状态；上下文模块只读取系统提供的固定缓冲。
VOID KswordARKBugcheckContextReset(VOID);
// 把系统转储头寄存器与最近运行期操作调用栈复制到固定证据副本。
VOID KswordARKBugcheckContextSnapshot(_Inout_ KSWORD_BUGCHECK_EVIDENCE* Evidence);
// 正常 PASSIVE_LEVEL 操作开始时记录调用栈；BugCheck 阶段不会调用栈捕获 API。
VOID KswordARKBugcheckContextOperation(VOID);
// 正常回调记录进程缓存退出状态，崩溃阶段不遍历未知进程对象。
VOID KswordARKBugcheckEvidenceProcess(_In_ ULONG_PTR Object, _In_ ULONG_PTR Pid, _In_ BOOLEAN Exiting);
// 最近事件记录和快照接口由独立 trace 模块提供。
VOID KswordARKBugcheckTraceRecord(_In_ ULONG Kind, _In_ ULONG Code, _In_ NTSTATUS Status, _In_opt_ PCSTR Text, _In_ ULONG TextBytes);
VOID KswordARKBugcheckTraceSnapshot(_Inout_ KSWORD_BUGCHECK_EVIDENCE* Evidence);
