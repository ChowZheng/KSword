// 扩展蓝屏证据：运行期采样与固定缓存，崩溃期只做有界非阻塞快照。
#include "bugcheck_evidence.h" // 引入版本化固定证据合同。
#include "bugcheck_identity.h" // PE 身份解析只供运行期使用。
#include "ark/ark_trust.h" // 优先复用已经建立的 CI 和 Secure Boot 全局快照。
#include <ntstrsafe.h> // 正常运行期有界常量字符串复制。
#include <intrin.h> // 正常运行期 CPUID 环境采样。

typedef struct _KSWORD_EVIDENCE_IMAGE_SLOT // 每个映像条目独立采用奇偶序列发布。
{
    volatile LONG Sequence; // 奇数正在写入，偶数为稳定快照。
    KSWORD_BUGCHECK_IMAGE_ID Identity; // resident 固定身份副本。
} KSWORD_EVIDENCE_IMAGE_SLOT;

typedef struct _KSWORD_EVIDENCE_PROCESS_SLOT // 不保留进程引用，只保留已有回调提供的数值身份。
{
    volatile LONG Sequence; // 奇偶发布标记。
    ULONG_PTR Object; // 用于匹配已有诊断中的对象数值。
    ULONG_PTR Pid; // 防止对象地址复用混入旧进程。
    BOOLEAN Exiting; // 已观察到退出通知。
    ULONG64 Time; // 最后更新时间，启动后 100ns。
} KSWORD_EVIDENCE_PROCESS_SLOT;

typedef struct _KSWORD_EVIDENCE_CACHE // 默认静态数据均常驻非分页。
{
    volatile LONG Ready; // 完整初始化后才接受通知。
    volatile LONG CaptureBusy; // 崩溃期只尝试一次 CAS，不等待其他处理器。
    volatile LONG Published; // 双缓冲中最近完成的证据槽。
    KSPIN_LOCK ImageLock; // 只供正常运行期写入使用。
    KSPIN_LOCK ProcessLock; // 崩溃读者从不取得此锁。
    ULONG NextImage; // 固定映像表满后的循环替换下标。
    ULONG NextProcess; // 固定进程表满后的循环替换下标。
    volatile LONG64 ImageEvictions; // 报告缓存覆盖而不假装全集完整。
    ULONG_PTR DriverBase; // 本驱动加载范围起点。
    ULONG_PTR KernelAddress; // 正常运行期解析的内核导出地址数值。
    KSWORD_BUGCHECK_ENVIRONMENT Environment; // 初始化后不可变的环境副本。
    KSWORD_EVIDENCE_IMAGE_SLOT Kernel; // 内核身份独立保留，不被循环表覆盖。
    KSWORD_EVIDENCE_IMAGE_SLOT Driver; // 本驱动身份独立保留。
    KSWORD_EVIDENCE_IMAGE_SLOT Images[KSWORD_ARK_BUGCHECK_MODULE_CACHE_COUNT]; // 固定 512 个映像槽。
    KSWORD_EVIDENCE_PROCESS_SLOT Processes[KSWORD_ARK_BUGCHECK_PROCESS_CACHE_COUNT]; // 固定 1024 个进程槽。
    KSWORD_BUGCHECK_EVIDENCE Snapshots[2]; // 大证据结构从不放在内核栈上。
} KSWORD_EVIDENCE_CACHE;

static KSWORD_EVIDENCE_CACHE g_KswordEvidence; // 默认 .data，不放入 PAGE 节。
static KSWORD_ARK_QUERY_IMAGE_TRUST_REQUEST g_KswordEvidenceTrustRequest; // 避免大路径请求占用栈。
static KSWORD_ARK_QUERY_IMAGE_TRUST_RESPONSE g_KswordEvidenceTrustResponse; // 正常安装工作项串行使用。

NTSYSAPI NTSTATUS NTAPI ZwQuerySystemInformation(ULONG Class, PVOID Buffer,
    ULONG Bytes, PULONG Returned); // 仅 PASSIVE_LEVEL 的环境回退查询。

// 安装准备时查询 UBR，不在 BugCheck 访问注册表。
static VOID KswordEvidenceUbr(KSWORD_BUGCHECK_ENVIRONMENT* Environment)
{
    UNICODE_STRING path; // 固定注册表路径。
    UNICODE_STRING name; // 固定值名称。
    OBJECT_ATTRIBUTES attributes; // 内核句柄属性。
    HANDLE key = NULL; // 失败时不会关闭未知句柄。
    ULONG returned = 0UL; // 实际注册表值长度。
    union { ULONG Alignment; UCHAR Bytes[FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) + sizeof(ULONG)]; } data; // 保证查询头和 DWORD 均四字节对齐。
    KEY_VALUE_PARTIAL_INFORMATION* value; // 固定输出视图。
    NTSTATUS status; // 采集失败通过有效位表示缺失。
    Environment->UbrStatus = STATUS_NOT_FOUND; // 初始化缺失状态，不把失败查询当成有效零修订号。
    RtlInitUnicodeString(&path, L"\\Registry\\Machine\\Software\\Microsoft\\Windows NT\\CurrentVersion"); // 不枚举注册表。
    RtlInitUnicodeString(&name, L"UBR"); // 只查询系统修订号。
    InitializeObjectAttributes(&attributes, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL); // 使用内核句柄。
    status = ZwOpenKey(&key, KEY_QUERY_VALUE, &attributes); // 运行期允许打开固定键。
    Environment->UbrStatus = status; // 保留打开键的失败原因。
    if (!NT_SUCCESS(status)) { // 键不存在或访问失败。
        return; // 不把失败转换为有效零值。
    }
    status = ZwQueryValueKey(key, &name, KeyValuePartialInformation, data.Bytes,
        (ULONG)sizeof(data.Bytes), &returned); // 只获取单个 DWORD。
    Environment->UbrStatus = status; // 保留值不存在、拒绝访问或缓冲不足等真实状态。
    value = (KEY_VALUE_PARTIAL_INFORMATION*)data.Bytes; // 查询结果仍由长度门禁验证。
    if (NT_SUCCESS(status) && returned >= sizeof(data.Bytes) &&
        value->Type == REG_DWORD && value->DataLength == sizeof(ULONG)) { // 必须是完整 DWORD。
        RtlCopyMemory(&Environment->Ubr, value->Data, sizeof(Environment->Ubr)); // 避免非对齐读取。
        Environment->Flags |= KSWORD_BUGCHECK_ENV_UBR; // 仅成功后发布有效位。
    } else if (NT_SUCCESS(status)) { // 系统返回成功也必须验证类型与完整长度。
        Environment->UbrStatus = returned < sizeof(data.Bytes) || value->DataLength != sizeof(ULONG) // 区分短结果或长度不符与非 DWORD 值。
            ? STATUS_INVALID_BUFFER_SIZE : STATUS_OBJECT_TYPE_MISMATCH; // 保留成功返回中的结构校验失败原因。
    }
    (VOID)ZwClose(key); // 所有成功打开路径都关闭句柄。
}

// 优先复用现有 CI 快照，失败才使用正常运行期的小型系统查询。
static VOID KswordEvidenceSecurity(KSWORD_BUGCHECK_ENVIRONMENT* Environment)
{
    struct { ULONG Length; ULONG Options; } ci; // 固定 SystemCodeIntegrityInformation。
    struct { BOOLEAN Enabled; BOOLEAN Capable; } secureBoot; // 固定 SystemSecureBootInformation。
    SIZE_T bytes = 0U; // 现有接口实际返回大小。
    NTSTATUS status; // 查询已有非分页全局快照。
    RtlZeroMemory(&g_KswordEvidenceTrustRequest, sizeof(g_KswordEvidenceTrustRequest)); // 空路径禁止文件签名查询。
    g_KswordEvidenceTrustRequest.flags = KSWORD_ARK_TRUST_QUERY_FLAG_INCLUDE_GLOBAL_CI; // 只读取已有系统策略副本。
    status = KswordARKDriverQueryImageTrust(&g_KswordEvidenceTrustResponse,
        sizeof(g_KswordEvidenceTrustResponse), &g_KswordEvidenceTrustRequest, &bytes); // 本调用只发生在 PASSIVE_LEVEL。
    Environment->CiStatus = STATUS_NOT_SUPPORTED; // 默认缺失不能表示为关闭。
    Environment->SecureBootStatus = STATUS_NOT_SUPPORTED; // 保留失败状态。
    if (NT_SUCCESS(status) && bytes >= sizeof(g_KswordEvidenceTrustResponse)) { // 接口返回完整固定结构。
        Environment->CiStatus = g_KswordEvidenceTrustResponse.codeIntegrityStatus; // 保留初始化查询结果。
        Environment->CiOptions = g_KswordEvidenceTrustResponse.codeIntegrityOptions; // 原始位图包含 HVCI 和测试签名。
        Environment->SecureBootStatus = g_KswordEvidenceTrustResponse.secureBootStatus; // 保留安全启动查询结果。
        Environment->SecureBoot = g_KswordEvidenceTrustResponse.secureBootEnabled; // 需同时使用状态码。
    }
    if (!NT_SUCCESS(Environment->CiStatus)) { // 已有快照不可用时才查询小型结构。
        RtlZeroMemory(&ci, sizeof(ci)); // 防止系统查询失败泄漏栈。
        ci.Length = sizeof(ci); // 固定系统结构长度。
        Environment->CiStatus = ZwQuerySystemInformation(103UL, &ci, sizeof(ci), NULL); // 运行期 CI 查询。
        if (NT_SUCCESS(Environment->CiStatus)) { // 只有成功才采用返回位图。
            Environment->CiOptions = ci.Options; // 不推断不存在的策略位。
        }
    }
    if (!NT_SUCCESS(Environment->SecureBootStatus)) { // 安全启动副本缺失时进行一次正常查询。
        RtlZeroMemory(&secureBoot, sizeof(secureBoot)); // 清空未初始化字段。
        Environment->SecureBootStatus = ZwQuerySystemInformation(145UL, &secureBoot, sizeof(secureBoot), NULL); // 固定安全启动信息类。
        if (NT_SUCCESS(Environment->SecureBootStatus)) { // 成功后保存真实开关。
            Environment->SecureBoot = secureBoot.Enabled ? 1UL : 0UL; // 保留布尔值。
        }
    }
    if (NT_SUCCESS(Environment->CiStatus)) { // 全局 CI 采样确实可用。
        Environment->Flags |= KSWORD_BUGCHECK_ENV_CI; // 向报告发布有效位。
    }
    if (NT_SUCCESS(Environment->SecureBootStatus)) { // 安全启动采样确实可用。
        Environment->Flags |= KSWORD_BUGCHECK_ENV_SECUREBOOT; // 查询失败不显示关闭。
    }
}

// 所有会查询版本、策略或 CPU 的操作在安装准备阶段完成。
static VOID KswordEvidenceEnvironment(KSWORD_BUGCHECK_ENVIRONMENT* Environment)
{
    RTL_OSVERSIONINFOEXW version; // 产品类型需要 EX 版本结构。
    LARGE_INTEGER frequency; // 性能计数器频率。
    RtlZeroMemory(Environment, sizeof(*Environment)); // 清理旧环境身份。
    RtlZeroMemory(&version, sizeof(version)); // 系统查询失败仍有确定字段。
    version.dwOSVersionInfoSize = sizeof(version); // 请求完整版本信息。
    Environment->VersionStatus = RtlGetVersion((PRTL_OSVERSIONINFOW)&version); // 仅 PASSIVE_LEVEL 查询。
    if (NT_SUCCESS(Environment->VersionStatus)) { // 保存真实运行系统版本。
        Environment->Major = version.dwMajorVersion; // 主版本。
        Environment->Minor = version.dwMinorVersion; // 次版本。
        Environment->Build = version.dwBuildNumber; // 完整 build。
        Environment->ProductType = version.wProductType; // 工作站或服务器产品类型。
        Environment->Flags |= KSWORD_BUGCHECK_ENV_VERSION; // 版本有效位。
    }
    KswordEvidenceUbr(Environment); // 注册表只在此运行期阶段查询。
    Environment->ProcessorCount = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS); // 保存跨处理器组逻辑 CPU 数。
    Environment->Flags |= KSWORD_BUGCHECK_ENV_CPU; // CPU 采样是固定数值。
    (VOID)KeQueryPerformanceCounter(&frequency); // 正常运行期取得计数器频率。
    if (frequency.QuadPart > 0LL) { // 正值才构成有效频率。
        Environment->PerformanceFrequency = (ULONG64)frequency.QuadPart; // 保留完整计数单位。
        Environment->Flags |= KSWORD_BUGCHECK_ENV_PERF; // 性能频率有效位。
    }
    KswordEvidenceSecurity(Environment); // 复用现有 CI/Secure Boot 快照。
#if defined(_AMD64_) || defined(_M_AMD64) || defined(_X86_) || defined(_M_IX86)
    {
        int registers[4]; // CPUID 固定四寄存器输出。
        __cpuid(registers, 1); // 正常运行期检测 hypervisor 位。
        Environment->HypervisorPresent = (((ULONG)registers[2] & 0x80000000UL) != 0UL) ? 1UL : 0UL; // 不推断虚拟化产品。
        if (Environment->HypervisorPresent != 0UL) { // 只有存在位才查询 vendor leaf。
            __cpuid(registers, 0x40000000); // 标准 hypervisor 身份叶。
            RtlCopyMemory(Environment->HypervisorVendor, &registers[1], sizeof(int)); // 供应商字符串 EBX。
            RtlCopyMemory(Environment->HypervisorVendor + 4, &registers[2], sizeof(int)); // 供应商字符串 ECX。
            RtlCopyMemory(Environment->HypervisorVendor + 8, &registers[3], sizeof(int)); // 供应商字符串 EDX。
            Environment->HypervisorVendor[12] = '\0'; // 字符串固定 NUL 结尾。
        }
        Environment->Flags |= KSWORD_BUGCHECK_ENV_HYPERVISOR; // 记录成功采样，包括有效不存在。
    }
#endif
    (VOID)RtlStringCbCopyA(Environment->DriverBuild, sizeof(Environment->DriverBuild),
        "KSwordARK evidence/1 (PE+RSDS)"); // 确定性构建禁用日期宏；精确构建身份由 Driver 的 PE/RSDS 保留。
    Environment->Flags |= KSWORD_BUGCHECK_ENV_DRIVERBUILD; // 构建说明由当前编译器生成。
    Environment->SampleTime = KeQueryInterruptTime(); // 快照采样时刻供离线判断陈旧程度。
}

// 运行期持写锁者发布一项完整身份，崩溃期只检查奇偶序号。
static VOID KswordEvidencePublishImage(KSWORD_EVIDENCE_IMAGE_SLOT* Slot,
    const KSWORD_BUGCHECK_IMAGE_ID* Identity)
{
    (VOID)InterlockedIncrement(&Slot->Sequence); // 发布写入进行中。
    KeMemoryBarrier(); // 不允许字段写入越过奇数发布。
    RtlCopyMemory(&Slot->Identity, Identity, sizeof(*Identity)); // 固定非分页结构复制。
    KeMemoryBarrier(); // 完整字段先于稳定序号可见。
    (VOID)InterlockedIncrement(&Slot->Sequence); // 发布偶数稳定槽。
}

VOID KswordARKBugcheckEvidenceInitialize(PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING name; // 仅正常初始化解析稳定内核导出。
    if (DriverObject == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL) { // 初始化不允许进入崩溃路径。
        return; // 不执行 PAGE 服务或触碰未初始化锁。
    }
    RtlZeroMemory(&g_KswordEvidence, sizeof(g_KswordEvidence)); // 控制器已串行化安装，TrackingReady 尚未发布。
    KeInitializeSpinLock(&g_KswordEvidence.ImageLock); // 只供运行期写入。
    KeInitializeSpinLock(&g_KswordEvidence.ProcessLock); // 崩溃期不取得此锁。
    g_KswordEvidence.DriverBase = (ULONG_PTR)DriverObject->DriverStart; // 保存本驱动的稳定加载基址。
    RtlInitUnicodeString(&name, L"KeBugCheckEx"); // 内核身份由导出地址所属模块确定。
    g_KswordEvidence.KernelAddress = (ULONG_PTR)MmGetSystemRoutineAddress(&name); // 只解析数值，不在崩溃期使用该服务。
    KswordEvidenceEnvironment(&g_KswordEvidence.Environment); // 所有环境慢查询在此完成。
    g_KswordEvidence.Kernel.Identity.Status = STATUS_NOT_FOUND; // 内核基线枚举尚未完成。
    g_KswordEvidence.Driver.Identity.Status = STATUS_NOT_FOUND; // 本驱动 PE 尚未读取。
    KswordARKBugcheckContextReset(); // 同一次安装清理系统转储头拼接状态。
    KeMemoryBarrier(); // 初始化完成后才允许运行期通知更新缓存。
    InterlockedExchange(&g_KswordEvidence.Ready, 1L); // 发布固定缓存可用状态。
    KswordARKBugcheckEvidenceImage(g_KswordEvidence.DriverBase, DriverObject->DriverSize, NULL); // 本驱动身份提前固定。
    if ((g_KswordEvidence.Driver.Identity.Flags & KSWORD_BUGCHECK_ID_PE) != 0UL) { // 只有安全解析成功才附加真实 PE 身份。
        (VOID)RtlStringCbPrintfA(g_KswordEvidence.Environment.DriverBuild, sizeof(g_KswordEvidence.Environment.DriverBuild), // 安装时构造固定文本，不在崩溃时格式化。
            "KSwordARK evidence/1 PE=%08lX/%08lX", g_KswordEvidence.Driver.Identity.TimeDateStamp, g_KswordEvidence.Driver.Identity.Checksum); // 链接时间戳在确定性构建下属于身份值，不声称墙钟日期。
    } // 构建身份补充结束。
}

VOID KswordARKBugcheckEvidenceImage(ULONG_PTR Base, ULONG Size, PCUNICODE_STRING FullPath)
{
    KSWORD_BUGCHECK_IMAGE_ID identity; // 约三百字节的小型运行期解析副本。
    KIRQL oldIrql; // 写锁只覆盖固定缓存发布。
    ULONG index; // 映像槽下标。
    ULONG selected = MAXULONG; // 未命中槽时采用固定循环替换。
    if (Base == 0U || Size == 0UL || KeGetCurrentIrql() > APC_LEVEL ||
        InterlockedCompareExchange(&g_KswordEvidence.Ready, 0L, 0L) == 0L) { // 不在崩溃期解析映像。
        return; // 不等待锁、不读取候选地址。
    }
    KswordARKBugcheckIdentityRead(Base, Size, FullPath, &identity); // 安全读取与路径处理均在取得锁之前。
    KeAcquireSpinLock(&g_KswordEvidence.ImageLock, &oldIrql); // 运行期短暂串行化缓存写入。
    for (index = 0UL; index < KSWORD_ARK_BUGCHECK_MODULE_CACHE_COUNT; ++index) { // 固定 512 槽扫描。
        if (g_KswordEvidence.Images[index].Identity.Base == Base) { // 同一基址加载或刷新覆盖旧身份。
            selected = index; // 复用已存在槽。
            break; // 不增加重复缓存项。
        }
        if (selected == MAXULONG && g_KswordEvidence.Images[index].Identity.Base == 0U) { // 优先空闲槽。
            selected = index; // 暂存第一可用槽但继续查找已存在基址。
        }
    }
    if (selected == MAXULONG) { // 缓存容量耗尽。
        selected = g_KswordEvidence.NextImage++ % KSWORD_ARK_BUGCHECK_MODULE_CACHE_COUNT; // 固定容量循环覆盖。
        (VOID)InterlockedIncrement64(&g_KswordEvidence.ImageEvictions); // 记录覆盖边界。
    }
    KswordEvidencePublishImage(&g_KswordEvidence.Images[selected], &identity); // 发布缓存完整副本。
    if (Base == g_KswordEvidence.DriverBase) { // 本驱动身份独立保留。
        KswordEvidencePublishImage(&g_KswordEvidence.Driver, &identity); // 不受普通模块缓存替换影响。
    }
    if (g_KswordEvidence.KernelAddress >= Base && g_KswordEvidence.KernelAddress - Base < Size) { // 导出地址只用数值范围匹配。
        KswordEvidencePublishImage(&g_KswordEvidence.Kernel, &identity); // 当前实际内核映像身份。
    }
    KeReleaseSpinLock(&g_KswordEvidence.ImageLock, oldIrql); // 所有运行期写锁及时释放。
}

VOID KswordARKBugcheckEvidenceProcess(ULONG_PTR Object, ULONG_PTR Pid, BOOLEAN Exiting)
{
    KIRQL oldIrql; // 只供正常通知的写锁。
    ULONG index; // 固定进程缓存扫描下标。
    ULONG selected = MAXULONG; // 优先已存在对象或空槽。
    KSWORD_EVIDENCE_PROCESS_SLOT* slot; // 选中的固定 resident 进程槽。
    if (Object == 0U || KeGetCurrentIrql() > DISPATCH_LEVEL ||
        InterlockedCompareExchange(&g_KswordEvidence.Ready, 0L, 0L) == 0L) { // 不访问未知进程对象。
        return; // 对象数值之外没有任何解引用。
    }
    KeAcquireSpinLock(&g_KswordEvidence.ProcessLock, &oldIrql); // 运行期回调只发布小型固定字段。
    for (index = 0UL; index < KSWORD_ARK_BUGCHECK_PROCESS_CACHE_COUNT; ++index) { // 固定 1024 个槽。
        if (g_KswordEvidence.Processes[index].Object == Object) { // 地址复用时新 PID 会一同覆盖。
            selected = index; // 更新已存在对象槽。
            break; // 不重复创建条目。
        }
        if (selected == MAXULONG && g_KswordEvidence.Processes[index].Object == 0U) { // 优先空闲槽。
            selected = index; // 继续查找精确命中。
        }
    }
    if (selected == MAXULONG) { // 固定表满后采用有限循环槽。
        selected = g_KswordEvidence.NextProcess++ % KSWORD_ARK_BUGCHECK_PROCESS_CACHE_COUNT; // 不分配额外内存。
    }
    slot = &g_KswordEvidence.Processes[selected]; // 绑定当前 resident 槽。
    (VOID)InterlockedIncrement(&slot->Sequence); // 奇数表示写入中。
    KeMemoryBarrier(); // 字段更新不能先于奇数发布。
    slot->Object = Object; // 保存数值对象身份。
    slot->Pid = Pid; // 保存数值进程 ID。
    slot->Exiting = Exiting; // 保存创建或退出通知状态。
    slot->Time = KeQueryInterruptTime(); // 采样时间可判断缓存陈旧。
    KeMemoryBarrier(); // 所有字段先于偶数发布。
    (VOID)InterlockedIncrement(&slot->Sequence); // 完整进程槽发布。
    KeReleaseSpinLock(&g_KswordEvidence.ProcessLock, oldIrql); // 写入完毕立即释放运行期锁。
}

// 崩溃期只读取固定槽，遇到正在写入或代次变化立刻返回失败。
static BOOLEAN KswordEvidenceCopyImage(const KSWORD_EVIDENCE_IMAGE_SLOT* Slot,
    KSWORD_BUGCHECK_IMAGE_ID* Identity)
{
    LONG before = InterlockedCompareExchange((volatile LONG*)&Slot->Sequence, 0L, 0L); // 单次读取发布序号。
    if ((before & 1L) != 0L) { // 其他处理器正更新槽。
        return FALSE; // 绝不等待被冻结的写入者。
    }
    RtlCopyMemory(Identity, &Slot->Identity, sizeof(*Identity)); // 固定非分页源与目的地。
    KeMemoryBarrier(); // 完整复制后再检查同一代次。
    if (InterlockedCompareExchange((volatile LONG*)&Slot->Sequence, 0L, 0L) != before) { // 更新在复制期间发生。
        RtlZeroMemory(Identity, sizeof(*Identity)); // 不发布混合代次身份。
        Identity->Status = STATUS_DEVICE_BUSY; // 保留冲突原因。
        return FALSE; // 调用方标记快照冲突。
    }
    return TRUE; // 完整固定条目稳定。
}

// 查找已有对象数值的退出状态，不调用 PsGetProcess* 或读取 EPROCESS 字段。
static VOID KswordEvidenceProcessSnapshot(const KSWORD_ARK_BUGCHECK_DIAGNOSTICS* Diagnostics,
    KSWORD_BUGCHECK_EVIDENCE* Evidence)
{
    ULONG index; // 固定表中的有界扫描。
    for (index = 0UL; index < KSWORD_ARK_BUGCHECK_PROCESS_CACHE_COUNT; ++index) { // 单次扫描，不重试等待。
        const KSWORD_EVIDENCE_PROCESS_SLOT* slot = &g_KswordEvidence.Processes[index]; // 只读 resident 槽。
        LONG before = InterlockedCompareExchange((volatile LONG*)&slot->Sequence, 0L, 0L); // 奇偶发布检查。
        ULONG_PTR object; // 复制的数值对象。
        ULONG_PTR pid; // 复制的数值 PID。
        ULONG64 time; // 复制的更新时间。
        BOOLEAN exiting; // 复制的退出状态。
        if ((before & 1L) != 0L) { // 当前槽仍在更新。
            Evidence->Flags |= KSWORD_BUGCHECK_EVIDENCE_RACE; // 报告存在跳过槽。
            continue; // 不等待写锁。
        }
        object = slot->Object; // 固定字段读。
        pid = slot->Pid; // 固定字段读。
        time = slot->Time; // 固定字段读。
        exiting = slot->Exiting; // 固定字段读。
        KeMemoryBarrier(); // 校验复制前后属于同一代。
        if (InterlockedCompareExchange((volatile LONG*)&slot->Sequence, 0L, 0L) != before) { // 复制发生竞争。
            Evidence->Flags |= KSWORD_BUGCHECK_EVIDENCE_RACE; // 保留冲突边界。
            continue; // 不采用混合值。
        }
        if (object != 0U && object == Diagnostics->ProcessObject &&
            (Diagnostics->ProcessId == 0U || pid == Diagnostics->ProcessId)) { // 只匹配已有诊断缓存身份。
            Evidence->ProcessCacheFlags = KSWORD_BUGCHECK_PROCESS_HIT | KSWORD_BUGCHECK_PROCESS_STABLE; // 明确命中稳定缓存。
            if (exiting) { // 已观察到进程退出。
                Evidence->ProcessCacheFlags |= KSWORD_BUGCHECK_PROCESS_EXITING; // 不代表终止它的原因。
            }
            Evidence->ProcessSampleTime = time; // 精确记录缓存陈旧程度。
            return; // 已取得目标进程状态。
        }
    }
}

VOID KswordARKBugcheckEvidenceCapture(const KSWORD_ARK_BUGCHECK_DIAGNOSTICS* Diagnostics)
{
    LONG active; // 最近发布槽下标。
    LONG next; // 本次写入的非当前槽。
    KSWORD_BUGCHECK_EVIDENCE* evidence; // 大结构始终位于静态数据区。
    PROCESSOR_NUMBER processor; // 小型处理器组信息。
    ULONG index; // 固定候选映像表下标。
    if (Diagnostics == NULL || InterlockedCompareExchange(&g_KswordEvidence.Ready, 0L, 0L) == 0L ||
        InterlockedCompareExchange(&g_KswordEvidence.CaptureBusy, 1L, 0L) != 0L) { // 只尝试一次非阻塞工作区占用。
        return; // 崩溃期不等待锁或其他处理器。
    }
    active = InterlockedCompareExchange(&g_KswordEvidence.Published, 0L, 0L); // 双缓冲发布值只为零或一。
    next = active == 0L ? 1L : 0L; // 当前发布槽在本次采集中不被修改。
    evidence = &g_KswordEvidence.Snapshots[next]; // 本次使用静态大工作区。
    RtlZeroMemory(evidence, sizeof(*evidence)); // 不泄漏上次证据字段。
    evidence->Version = KSWORD_BUGCHECK_EVIDENCE_VERSION; // 固定扩展证据版本。
    evidence->Flags = KSWORD_BUGCHECK_EVIDENCE_VALID; // 已建立一次明确的固定快照。
    evidence->CaptureTime = KeQueryInterruptTime(); // 无等待的启动后时间。
    KeGetCurrentProcessorNumberEx(&processor); // 当前回调处理器组与组内编号。
    evidence->CpuGroup = processor.Group; // 只表示回调上下文。
    evidence->CpuNumber = processor.Number; // 不冒充故障现场处理器。
    evidence->ThreadId = (ULONG_PTR)PsGetCurrentThreadId(); // 当前回调线程数值 ID。
    RtlCopyMemory(&evidence->Environment, &g_KswordEvidence.Environment, sizeof(evidence->Environment)); // 初始化后不变的运行期快照。
    evidence->Kernel.Status = STATUS_DEVICE_BUSY; // 奇数槽失败时仍保留缺失原因。
    evidence->Driver.Status = STATUS_DEVICE_BUSY; // 不把读取冲突当作零身份。
    evidence->Candidate.Status = STATUS_NOT_FOUND; // 无候选或未缓存时明确缺失。
    if (!KswordEvidenceCopyImage(&g_KswordEvidence.Kernel, &evidence->Kernel)) { // 内核身份独立检查，不阻止本驱动副本采集。
        evidence->Flags |= KSWORD_BUGCHECK_EVIDENCE_RACE; // 报告正在更新的固定槽。
    }
    if (!KswordEvidenceCopyImage(&g_KswordEvidence.Driver, &evidence->Driver)) { // 本驱动身份独立检查，避免逻辑短路丢失证据。
        evidence->Flags |= KSWORD_BUGCHECK_EVIDENCE_RACE; // 报告正在更新的固定槽。
    }
    if (Diagnostics->CandidateModuleBase != 0U) { // 仅对已有安全归因候选读取缓存。
        for (index = 0UL; index < KSWORD_ARK_BUGCHECK_MODULE_CACHE_COUNT; ++index) { // 固定 512 个槽，不解析 PE。
            const KSWORD_EVIDENCE_IMAGE_SLOT* slot = &g_KswordEvidence.Images[index]; // 固定 resident 条目。
            LONG before = InterlockedCompareExchange((volatile LONG*)&slot->Sequence, 0L, 0L); // 先检查条目发布代次。
            if ((before & 1L) != 0L || slot->Identity.Base != Diagnostics->CandidateModuleBase) { // 非候选或写入中直接跳过。
                continue; // 绝不解引用候选映像地址。
            }
            if (KswordEvidenceCopyImage(slot, &evidence->Candidate) &&
                evidence->Candidate.Base == Diagnostics->CandidateModuleBase &&
                evidence->Candidate.ImageSize == Diagnostics->CandidateModuleSize) { // 防止同一基址复用不同映像范围。
                break; // 已取得稳定的候选身份。
            }
            RtlZeroMemory(&evidence->Candidate, sizeof(evidence->Candidate)); // 混合或过时身份不发布。
            evidence->Candidate.Status = STATUS_DEVICE_BUSY; // 记录冲突或代次不匹配。
            evidence->Flags |= KSWORD_BUGCHECK_EVIDENCE_RACE; // 报告归因缓存一致性边界。
        }
        if (evidence->Candidate.Base == 0U) { // 安全候选未命中身份缓存。
            evidence->Flags |= KSWORD_BUGCHECK_EVIDENCE_CACHE_PARTIAL; // 不声称所有映像均已采集。
        }
    }
    if (InterlockedCompareExchange64(&g_KswordEvidence.ImageEvictions, 0LL, 0LL) != 0LL) { // 固定身份表曾发生覆盖。
        evidence->Flags |= KSWORD_BUGCHECK_EVIDENCE_CACHE_PARTIAL; // 明确保留缓存容量边界。
    }
    KswordEvidenceProcessSnapshot(Diagnostics, evidence); // 不遍历未知进程对象，仅复制退出状态。
    KswordARKBugcheckTraceSnapshot(evidence); // 独立 trace 模块提供非等待固定事件副本。
    KswordARKBugcheckContextSnapshot(evidence); // 系统提供的寄存器和运行期栈来源保持分离。
    KeMemoryBarrier(); // 全部证据先于发布下标可见。
    InterlockedExchange(&g_KswordEvidence.Published, next); // 原子发布完整证据。
    InterlockedExchange(&g_KswordEvidence.CaptureBusy, 0L); // 所有采集成功路径都释放工作区。
}

const KSWORD_BUGCHECK_EVIDENCE* KswordARKBugcheckEvidenceSnapshot(VOID)
{
    LONG active = InterlockedCompareExchange(&g_KswordEvidence.Published, 0L, 0L); // 读取最近完成的固定副本。
    return &g_KswordEvidence.Snapshots[active == 0L ? 0 : 1]; // 调用者不得写入，地址始终常驻非分页。
}
