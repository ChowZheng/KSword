#include "ark/ark_driver.h"
#include "keyboard_internal.h"
#include "keyboard_rule_match.h"
#include "../kernel/hook_scan_support.h"

#define KSW_RULE_POOL_TAG 'rHkK' /* 规则工作区仅在PASSIVE_LEVEL短期存在。 */
#define KSW_RULE_CACHE_RANGES 4096UL /* 函数冷块与PE元数据共享明确预算。 */

/* 生产PE适配器只保存当前模块的实际映射，不包含版本允许名单。 */
// 映像视图只保存当前加载模块的基址和大小，指纹项保存模块、范围与散列。
typedef struct KSW_RULE_KERNEL_IMAGE { PVOID Base; ULONG Size; } KSW_RULE_KERNEL_IMAGE;
typedef struct KSW_RULE_FINGERPRINT { ULONG Module, Rva, Bytes; ULONG64 Hash; } KSW_RULE_FINGERPRINT;
typedef struct KSW_RULE_RUNTIME_CACHE {
    // 缓存有效位、映像身份和规则版本共同决定热路径是否可复用。
    BOOLEAN Valid;
    ULONG FullStamp, BaseStamp, RuleVersion, Count, EvidenceCount;
    // 缓存中的schema只含可复制标量，代码范围与指令证据单独保存。
    KSW_KEYBOARD_HOTKEY_RUNTIME Runtime;
    KSW_RULE_FINGERPRINT Ranges[KSW_RULE_CACHE_RANGES];
    KSW_HOTKEY_RULE_EVIDENCE Evidence[KSW_HOTKEY_RULE_MAX_EVIDENCE];
} KSW_RULE_RUNTIME_CACHE;
typedef struct KSW_RULE_RUNTIME_WORK {
    // 冷路径工作区容纳完整匹配结果、待发布缓存和稳定性护栏。
    KSW_HOTKEY_RULE_RESULT Result;
    KSW_RULE_RUNTIME_CACHE Cache;
    KSW_RULE_RUNTIME_CACHE Guard; // 仅miss时使用的完整代码/元数据前后稳定性快照，不复制到warm缓存。
    KSW_RULE_FUNCTION Function;
    IMAGE_NT_HEADERS CheckFullHeaders, CheckBaseHeaders; // 冷匹配基线与实际解析使用同一份头部布局。
} KSW_RULE_RUNTIME_WORK;

static EX_PUSH_LOCK g_KswRuleCacheLock; /* 零初始化push lock只保护此元数据缓存。 */
static KSW_RULE_RUNTIME_CACHE g_KswRuleCache; /* 仅持有定长值，不保存可泄漏的Result指针。 */
static ULONG g_KswRuleGeneration; /* 在排他锁内递增，旧scalar副本不能借用新证明。 */
static PVOID volatile g_KswRuleAllocate2; /* 内核导出解析结果，1表示旧系统无此入口。 */

typedef PVOID(NTAPI* KSW_RUNTIME_SESSION_GETTER_FN)(VOID); // W32与SGD实际导出的共同零参数ABI。

NTSYSAPI PVOID NTAPI RtlFindExportedRoutineByName(_In_ PVOID ImageBase, _In_z_ PCSTR RoutineName);

/* SID和实际USER namespace分别比较，不以可碰撞散列代替身份。 */
NTSTATUS KswordARKKeyboardGetSessionId(_Out_ ULONG* SessionIdOut)
{
    // 会话ID由系统导出针对当前附加进程查询。
    typedef ULONG(NTAPI* GET_SESSION_ID)(PEPROCESS);
    UNICODE_STRING name; GET_SESSION_ID getSessionId;
    // 输出指针和被动级别是查询系统会话的前置条件。
    if (!SessionIdOut || KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_STATE;
    // 先设无效哨兵值，再按名称解析实际系统入口。
    *SessionIdOut = MAXULONG; RtlInitUnicodeString(&name, L"PsGetProcessSessionId");
    getSessionId = (GET_SESSION_ID)MmGetSystemRoutineAddress(&name);
    // 缺少系统入口时不能凭当前线程或默认会话猜测SID。
    if (!getSessionId) return STATUS_PROCEDURE_NOT_FOUND;
    *SessionIdOut = getSessionId(PsGetCurrentProcess()); // 附加进程时返回本次实际选中的SID。
    // 哨兵值仍未被覆盖时返回真实状态失败。
    return *SessionIdOut == MAXULONG ? STATUS_INVALID_DEVICE_STATE : STATUS_SUCCESS;
}

/* 将同源规则schema绑定到本次SID与实际USER namespace，不共享会话可变状态。 *//* 附加进程期间只做有界读取与真实getter绑定，不运行CFG或分配大型证明图。 */
NTSTATUS KswordARKKeyboardBindHotkeyRuntimeContext(_Inout_ KSW_KEYBOARD_HOTKEY_RUNTIME* Runtime)
{
    // 绑定前要求已证明的schema、getter地址和被动级别。
    ULONG_PTR importedGetter = 0U; NTSTATUS status;
    if (!Runtime || !Runtime->RuleProven || !Runtime->SessionGetterAddress || KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_STATE;
    // 清除上次会话派生字段，避免旧namespace残留。
    Runtime->SessionGlobals = 0U; Runtime->UserSessionState = 0U; Runtime->CacheAddress = 0U;
    // 本次SID与实际getter返回的namespace分别取得。
    status = KswordARKKeyboardGetSessionId(&Runtime->SessionId);
    if (!NT_SUCCESS(status)) return status;
    // 重新读取Full实际IAT槽，确认它仍指向已证明的getter。
    if (!Runtime->SessionGetterSlotRva || Runtime->SessionGetterSlotRva > Runtime->Win32kfullSize ||
        sizeof(importedGetter) > Runtime->Win32kfullSize - Runtime->SessionGetterSlotRva ||
        !KswordARKHookReadMemorySafe((const UCHAR*)Runtime->Win32kfullBase + Runtime->SessionGetterSlotRva, &importedGetter, sizeof(importedGetter)) ||
        importedGetter != Runtime->SessionGetterAddress) return STATUS_REVISION_MISMATCH;
    // 受保护调用当前USER getter，异常原样转成失败状态。
    __try { Runtime->UserSessionState = (ULONG_PTR)((KSW_RUNTIME_SESSION_GETTER_FN)Runtime->SessionGetterAddress)(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
    // 会话地址必须位于系统范围，并容纳表及缓存偏移。
    if (Runtime->UserSessionState < (ULONG_PTR)MmSystemRangeStart ||
        Runtime->UserSessionState > MAXULONG_PTR - Runtime->TableOffset || Runtime->UserSessionState > MAXULONG_PTR - Runtime->CacheOffset) return STATUS_INVALID_DEVICE_STATE;
    // 本次真实namespace和SID共同组成快照身份。
    Runtime->SessionGlobals = Runtime->UserSessionState; // 精确namespace身份与单独SID一起进入快照。
    // 缓存可由映像RVA或会话内偏移给出，依规则schema选择。
    Runtime->CacheAddress = Runtime->CacheRva ? (ULONG_PTR)Runtime->Win32kfullBase + Runtime->CacheRva : Runtime->UserSessionState + Runtime->CacheOffset;
    // 地址或整张表范围溢出时拒绝绑定。
    if (!Runtime->CacheAddress || Runtime->CacheAddress > MAXULONG_PTR - sizeof(PVOID) ||
        (!Runtime->TableBase && Runtime->UserSessionState + Runtime->TableOffset > MAXULONG_PTR - Runtime->BucketCount * sizeof(PVOID))) return STATUS_INTEGER_OVERFLOW;
    return STATUS_SUCCESS;
}


/* 有界RVA读取沿用项目既有安全复制器，不执行或修改待分析代码。 */
static int KswRuleKernelRead(void* Context, uint32_t Rva, void* Output, size_t Bytes)
{
    // 读取回调只接受映像内且指针加法不溢出的完整区间。
    KSW_RULE_KERNEL_IMAGE* image = (KSW_RULE_KERNEL_IMAGE*)Context;
    if (!image || !Output || Rva > image->Size || Bytes > image->Size - Rva ||
        (ULONG_PTR)image->Base > MAXULONG_PTR - Rva) return 0;
    return KswordARKHookReadMemorySafe((const UCHAR*)image->Base + Rva, Output, Bytes) ? 1 : 0;
}

/* 大型Result、指令数组与证明图均分配于分页池，不占用嵌套内核栈。 */
static void* KswRuleKernelAllocate(void* Context, size_t Bytes)
{
    // 内核池分配器运行时解析，避免旧系统缺少新导入。
    typedef PVOID(NTAPI* ALLOCATE2)(ULONGLONG, SIZE_T, ULONG);
    PVOID routine = InterlockedCompareExchangePointer(&g_KswRuleAllocate2, NULL, NULL);
    // 宿主上下文不参与分配；本路径仅能在被动级别申请非零空间。
    UNREFERENCED_PARAMETER(Context);
    if (!Bytes || KeGetCurrentIrql() != PASSIVE_LEVEL) return NULL;
    // 首次使用才解析ExAllocatePool2，并缓存存在或缺失的结果。
    if (!routine) {
        UNICODE_STRING name; /* 旧系统不能产生静态ExAllocatePool2导入。 */
        RtlInitUnicodeString(&name, L"ExAllocatePool2");
        routine = MmGetSystemRoutineAddress(&name);
        if (!routine) routine = (PVOID)(ULONG_PTR)1;
        (VOID)InterlockedCompareExchangePointer(&g_KswRuleAllocate2, routine, NULL);
    }
    // 可用新入口时申请分页池；否则局部回退旧分页池入口。
    if (routine != (PVOID)(ULONG_PTR)1) return ((ALLOCATE2)routine)(POOL_FLAG_PAGED, Bytes, KSW_RULE_POOL_TAG);
#pragma warning(suppress: 4996) /* 仅此旧系统兼容分支使用仍可用的分页池旧入口。 */
    return ExAllocatePoolWithTag(PagedPool, Bytes, KSW_RULE_POOL_TAG);
}

/* 每个成功分配由同一宿主显式释放，不缓存分页池对象地址。 */
static void KswRuleKernelFree(void* Context, void* Pointer)
{
    // 释放使用与申请相同的池标签，空指针无需动作。
    UNREFERENCED_PARAMETER(Context);
    if (Pointer) ExFreePoolWithTag(Pointer, KSW_RULE_POOL_TAG);
}

/* 用真实模块建立同源规则引擎的只读适配器。 */
static VOID KswRuleKernelView(const KSW_HOOK_SYSTEM_MODULE_ENTRY* Module,
    KSW_RULE_KERNEL_IMAGE* Context, KSW_RULE_IMAGE* Image)
{
    // 将模块实际基址、大小和安全读取/分配回调交给共用规则引擎。
    Context->Base = Module->ImageBase; Context->Size = Module->ImageSize;
    Image->Context = Context; Image->SizeOfImage = Module->ImageSize;
    Image->Read = KswRuleKernelRead; Image->Alloc = KswRuleKernelAllocate; Image->Free = KswRuleKernelFree;
}

/* 时间戳只用于识别缓存映像实例，绝不决定系统版本是否获得支持。 */
static BOOLEAN KswRuleHeaderStamp(const KSW_HOOK_SYSTEM_MODULE_ENTRY* Module, ULONG* Stamp,
    IMAGE_NT_HEADERS* Headers)
{
    // PE头须与实际加载模块大小一致才能作为缓存身份元数据。
    if (!Module || !Module->ImageBase || !KswordARKHookReadImageNtHeaders(Module, Headers) ||
        Headers->OptionalHeader.SizeOfImage != Module->ImageSize) return FALSE;
    // 时间戳只参与失效检测，不单独授权任何结构。
    *Stamp = Headers->FileHeader.TimeDateStamp;
    return TRUE;
}

/* 节点内字段逐项验证完整宽度；偏移零本身合法。 */
static BOOLEAN KswRuleFieldFits(ULONG Size, ULONG Offset, ULONG Width)
{
    // 字段必须有正宽度，并完整落在对象范围内。
    return Width != 0UL && Offset <= Size && Width <= Size - Offset;
}

/* RIP全局模式的可写对象必须落在实际可写数据节，不能把指令或只读常量当缓存。 */
static BOOLEAN KswRuleWritableRva(const KSW_HOOK_SYSTEM_MODULE_ENTRY* Module, ULONG Rva, ULONG Bytes)
{
    // 节表索引按已验证的DOS/NT头计算，拒绝越界和过多节。
    IMAGE_DOS_HEADER dos; IMAGE_NT_HEADERS nt; ULONG sectionTable = 0UL, i;
    if (Rva > Module->ImageSize || Bytes > Module->ImageSize - Rva ||
        !KswordARKHookReadImageBytes(Module, 0UL, &dos, sizeof(dos)) || dos.e_lfanew <= 0 ||
        !KswordARKHookReadImageNtHeaders(Module, &nt) || nt.FileHeader.NumberOfSections > 96U ||
        !KswordARKHookAddRvaOffset((ULONG)dos.e_lfanew, 1UL,
            (ULONG)FIELD_OFFSET(IMAGE_NT_HEADERS, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader, &sectionTable)) return FALSE;
    // 逐节检查真实写权限，排除可执行节冒充缓存数据。
    for (i = 0UL; i < nt.FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER section; ULONG sectionRva = 0UL, size;
        if (!KswordARKHookAddRvaOffset(sectionTable, i, (ULONG)sizeof(section), &sectionRva) ||
            !KswordARKHookReadImageBytes(Module, sectionRva, &section, sizeof(section))) return FALSE;
        // 字段范围采用虚拟与原始大小较大值，仍限制在对应节内。
        size = section.Misc.VirtualSize > section.SizeOfRawData ? section.Misc.VirtualSize : section.SizeOfRawData;
        if ((section.Characteristics & IMAGE_SCN_MEM_WRITE) != 0UL && (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0UL &&
            Rva >= section.VirtualAddress && Rva - section.VirtualAddress <= size && Bytes <= size - (Rva - section.VirtualAddress)) return TRUE;
    }
    return FALSE;
}

/* 将完整证明转为可复制scalar；协议无法完整承载的字段宽度保持不支持。 */
static BOOLEAN KswRuleCopySchema(const KSW_HOTKEY_RULE_RESULT* Result,
    const KSW_HOOK_SYSTEM_MODULE_ENTRY* Full, const KSW_HOOK_SYSTEM_MODULE_ENTRY* Base,
    ULONG Stamp, KSW_KEYBOARD_HOTKEY_RUNTIME* Runtime)
{
    // 待复制字段偏移与宽度采用同一顺序逐项核验。
    const ULONG offsets[10] = {Result->OwnerOffset, Result->CallbackOffset, Result->WindowOffset,
        Result->DestinationOffset, Result->ModifiersOffset, Result->FlagsOffset, Result->VkOffset,
        Result->IdOffset, Result->NextOffset, Result->ChildOffset};
    const ULONG widths[10] = {8UL, 8UL, 8UL, Result->DestinationWidth, Result->ModifiersWidth, Result->FlagsWidth,
        Result->VkWidth, Result->IdWidth, 8UL, 16UL};
    ULONG i, j; /* 指针、数值与子链头不得互相覆盖。 */
    // schema先通过规则版本、节点大小、窗口表示和桶数基本门禁。
    if (Result->RuleVersion != KSW_HOTKEY_RULE_VERSION || Result->Error != KSW_HK_RULE_OK ||
        Result->ObjectSize == 0 || Result->ObjectSize > KSW_HOTKEY_RULE_MAX_NODE ||
        Result->WindowRepresentation > 1U ||
        (Result->WindowRepresentation == 1U && (Result->AssignmentRva == 0U || Result->AssignmentRva >= Base->ImageSize)) ||
        Result->BucketCount == 0 || Result->BucketCount > 256U ||
        (Result->BucketCount & (Result->BucketCount - 1U)) != 0U) return FALSE;
    // 可选目标字段允许零宽度，其余字段必须完整对齐且互不重叠。
    for (i = 0; i < RTL_NUMBER_OF(offsets); ++i) {
        if (i == 3UL && widths[i] == 0UL) continue; // 经证明不存在的字段不能借用Owner偏移零。
        if (i == 3UL && widths[i] != 8UL) return FALSE;
        if (!KswRuleFieldFits(Result->ObjectSize, offsets[i], widths[i])) return FALSE;
        if (i >= 4UL && i <= 7UL && widths[i] != 1UL && widths[i] != 2UL && widths[i] != 4UL) return FALSE;
        if ((offsets[i] & ((widths[i] > 8UL ? 8UL : widths[i]) - 1UL)) != 0UL) return FALSE;
        for (j = 0; j < i; ++j) if (widths[j] != 0UL && offsets[i] < offsets[j] + widths[j] && offsets[j] < offsets[i] + widths[i]) return FALSE;
    }
    // 执行入口、会话getter槽及全局表缓存RVA必须位于相应真实映像。
    if (Result->RemoveRva == 0U || Result->ScanRva == 0U || Result->RemoveRva == Result->ScanRva ||
        Result->RemoveRva >= Full->ImageSize || Result->ScanRva >= Full->ImageSize || Result->EnterCritRva >= Base->ImageSize ||
        Result->LeaveCritRva >= Base->ImageSize || Result->ValidateHwndRva >= Base->ImageSize ||
        Result->SessionGetterSlotRva == 0U || Result->SessionGetterProvider > 1U ||
        Result->SessionGetterSlotRva > Full->ImageSize || sizeof(PVOID) > Full->ImageSize - Result->SessionGetterSlotRva ||
        (Result->TableRva != 0U && (Result->TableRva > Full->ImageSize || Result->BucketCount * 8U > Full->ImageSize - Result->TableRva)) ||
        (Result->CacheRva != 0U && (Result->CacheRva > Full->ImageSize || 8U > Full->ImageSize - Result->CacheRva))) return FALSE;
    // 全局表和缓存还需属于真实可写的非代码节。
    if ((Result->TableRva != 0U && !KswRuleWritableRva(Full, Result->TableRva, Result->BucketCount * 8U)) ||
        (Result->CacheRva != 0U && !KswRuleWritableRva(Full, Result->CacheRva, 8UL))) return FALSE;
    // 清空结果后仅复制已证实的静态schema；会话字段稍后绑定。
    RtlZeroMemory(Runtime, sizeof(*Runtime)); /* SessionGlobals在每次原调用者请求中另外取得。 */
    // 两个加载模块的地址、长度及时间戳共同记录为实例身份。
    Runtime->Win32kfullBase = Full->ImageBase; Runtime->Win32kfullSize = Full->ImageSize;
    Runtime->Win32kbaseBase = Base->ImageBase; Runtime->Win32kbaseSize = Base->ImageSize;
    Runtime->Win32kfullTimeDateStamp = Stamp;
    // 表来源为会话内偏移或映像绝对RVA，两者不混用。
    Runtime->TableOffset = Result->TableOffset;
    Runtime->TableBase = Result->TableRva ? (ULONG_PTR)Full->ImageBase + Result->TableRva : 0U;
    Runtime->CacheOffset = Result->CacheOffset; Runtime->CacheRva = Result->CacheRva;
    // 节点长度、桶数和所有字段偏移沿用匹配结果。
    Runtime->ObjectSize = Result->ObjectSize; Runtime->BucketCount = Result->BucketCount;
    Runtime->OwnerOffset = Result->OwnerOffset; Runtime->CallbackOffset = Result->CallbackOffset;
    Runtime->WindowOffset = Result->WindowOffset; Runtime->DestinationOffset = Result->DestinationOffset;
    Runtime->WindowRepresentation = Result->WindowRepresentation;
    Runtime->DestinationWidth = Result->DestinationWidth;
    Runtime->ModifiersOffset = Result->ModifiersOffset; Runtime->FlagsOffset = Result->FlagsOffset;
    Runtime->VkOffset = Result->VkOffset; Runtime->IdOffset = Result->IdOffset;
    Runtime->NextOffset = Result->NextOffset; Runtime->ChildOffset = Result->ChildOffset;
    // 数值字段的实际宽度逐一保留，供运行时精确读取。
    Runtime->ModifiersWidth = Result->ModifiersWidth; Runtime->FlagsWidth = Result->FlagsWidth;
    Runtime->VkWidth = Result->VkWidth; Runtime->IdWidth = Result->IdWidth;
    Runtime->WindowObjectHandleOffset = Result->WindowObjectHandleOffset;
    // 删除函数与临界区、窗口验证器RVA均来自本次证明。
    Runtime->RemoveRva = Result->RemoveRva; Runtime->EnterCritRva = Result->EnterCritRva;
    Runtime->LeaveCritRva = Result->LeaveCritRva; Runtime->ValidateHwndRva = Result->ValidateHwndRva;
    // getter的槽位与提供者类别进入最终schema。
    Runtime->SessionGetterSlotRva = Result->SessionGetterSlotRva;
    Runtime->SessionGetterProvider = Result->SessionGetterProvider;
    // 全部复制完毕才标记规则已证明。
    Runtime->RuleProven = 1UL;
    return TRUE;
}

/* 将重叠或相邻片段合并，避免每次查询重复读取同一段证明代码。 */
static BOOLEAN KswRuleCacheAddRange(KSW_RULE_RUNTIME_CACHE* Cache, ULONG Module,
    ULONG Rva, ULONG Bytes, ULONG ImageSize)
{
    // 范围合并从第一个缓存项开始，容量受独立上限限制。
    ULONG i = 0UL; /* 片段数组有独立容量预算。 */
    // 零长度或越过映像的指纹范围不能登记。
    if (!Bytes || Rva > ImageSize || Bytes > ImageSize - Rva) return FALSE;
    // 新范围与同模块已有范围重叠或近邻时合并后重新扫描。
    while (i < Cache->Count) {
        KSW_RULE_FINGERPRINT* range = &Cache->Ranges[i];
        if (range->Module == Module && (ULONG64)Rva <= (ULONG64)range->Rva + range->Bytes + 3ULL &&
            (ULONG64)range->Rva <= (ULONG64)Rva + Bytes + 3ULL) { // 同时合并名称表的至多三字节对齐空隙。
            // 取并集边界并移除旧项，避免缓存同一字节的重复片段。
            ULONG begin = Rva < range->Rva ? Rva : range->Rva;
            ULONG end = Rva + Bytes > range->Rva + range->Bytes ? Rva + Bytes : range->Rva + range->Bytes;
            Rva = begin; Bytes = end - begin; Cache->Ranges[i] = Cache->Ranges[--Cache->Count]; i = 0UL;
        } else ++i;
    }
    // 合并后仍超出范围数组预算则失败。
    if (Cache->Count >= KSW_RULE_CACHE_RANGES) return FALSE;
    // 新增范围先置零散列，待稳定性检查时赋值。
    Cache->Ranges[Cache->Count].Module = Module; Cache->Ranges[Cache->Count].Rva = Rva;
    Cache->Ranges[Cache->Count].Bytes = Bytes; Cache->Ranges[Cache->Count].Hash = 0ULL; ++Cache->Count;
    return TRUE;
}

/* 名称字符串必须完整有界，目录之外的import名称同样属于匹配依据。 */
static BOOLEAN KswRuleCacheString(KSW_RULE_RUNTIME_CACHE* Cache, ULONG Module,
    const KSW_RULE_IMAGE* Image, ULONG Rva, ULONG Prefix)
{
    // 导入名称读取最多512字节，必须包含终止零。
    ULONG count; UCHAR value;
    // 前缀与起点必须完整位于映像内。
    if (Rva > Image->SizeOfImage || Prefix > Image->SizeOfImage - Rva) return FALSE;
    // 逐字节读取直到终止符，再把完整字符串纳入指纹范围。
    for (count = 0UL; count < 512UL; ++count) {
        if (count >= Image->SizeOfImage - Rva - Prefix || !KswRuleRead(Image, Rva + Prefix + count, &value, 1U)) return FALSE;
        if (!value) return KswRuleCacheAddRange(Cache, Module, Rva, Prefix + count + 1UL, Image->SizeOfImage);
    }
    return FALSE;
}

/* 导入lookup表和符号名称常在IMPORT目录描述块之外，不能只指纹20字节描述符。 */
static BOOLEAN KswRuleCacheImports(KSW_RULE_RUNTIME_CACHE* Cache, ULONG Module,
    const KSW_RULE_IMAGE* Image, const IMAGE_NT_HEADERS* Headers)
{
    // 只对实际存在的导入目录执行完整INT/IAT登记。
    IMAGE_DATA_DIRECTORY directory; ULONG index;
    if (!KswordARKHookGetDataDirectory(Headers, IMAGE_DIRECTORY_ENTRY_IMPORT, &directory) || directory.Size == 0UL) return TRUE;
    // 导入描述符与每个符号槽都有独立预算。
    for (index = 0UL; index < directory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR) && index < 65536UL; ++index) {
        IMAGE_IMPORT_DESCRIPTOR descriptor; ULONG rva, thunkIndex, lookup, thunkBytes;
        // 描述符读取失败或RVA溢出不能继续部分登记。
        if (!KswordARKHookAddRvaOffset(directory.VirtualAddress, index, (ULONG)sizeof(descriptor), &rva) ||
            !KswRuleRead(Image, rva, &descriptor, sizeof(descriptor))) return FALSE;
        // 完整零描述符表示导入目录正常结束。
        if (!descriptor.Name && !descriptor.FirstThunk && !descriptor.OriginalFirstThunk) return TRUE;
        // DLL名称也是规则语义锚点，须参与缓存指纹。
        if (!KswRuleCacheString(Cache, Module, Image, descriptor.Name, 0UL)) return FALSE;
        // 必须存在原始名称查找表，不能从已绑定IAT反推名称。
        lookup = descriptor.OriginalFirstThunk;
        if (!lookup) return FALSE; // 不能把已绑定IAT地址猜成导入名称。
        // 遍历当前描述符的所有真实thunk直到终止项。
        for (thunkIndex = 0UL; thunkIndex < 65536UL; ++thunkIndex) {
            ULONG64 thunk = 0ULL;
            if (!KswordARKHookAddRvaOffset(lookup, thunkIndex, (ULONG)sizeof(thunk), &rva) ||
                !KswRuleRead(Image, rva, &thunk, sizeof(thunk))) return FALSE;
            if (!thunk) break;
            // 非ordinal导入的Hint/Name字符串也纳入缓存。
            if ((thunk & IMAGE_ORDINAL_FLAG64) == 0ULL &&
                (thunk > MAXULONG || !KswRuleCacheString(Cache, Module, Image, (ULONG)thunk, sizeof(USHORT)))) return FALSE;
        }
        // 缺失终止thunk说明元数据损坏或超预算。
        if (thunkIndex == 65536UL) return FALSE;
        // 原始INT与当前IAT完整槽区间分别计算并核验。
        thunkBytes = (thunkIndex + 1UL) * (ULONG)sizeof(ULONG64); // lookup实际长度包含终止槽，乘法由65536槽预算限制。
        if (lookup > Image->SizeOfImage || thunkBytes > Image->SizeOfImage - lookup ||
            descriptor.FirstThunk == 0UL || descriptor.FirstThunk > Image->SizeOfImage ||
            thunkBytes > Image->SizeOfImage - descriptor.FirstThunk) return FALSE; // 两个完整槽区间分别验证，不能只信IAT目录。
        // 同时缓存名字表和实际函数指针，捕获后续重定向。
        if (!KswRuleCacheAddRange(Cache, Module, lookup, thunkBytes, Image->SizeOfImage) ||
            !KswRuleCacheAddRange(Cache, Module, descriptor.FirstThunk, thunkBytes, Image->SizeOfImage)) return FALSE; // 当前实际函数指针同样参与cold/warm指纹。
    }
    // 缺少描述符终止项时不发布部分导入指纹。
    return FALSE;
}

/* 普通代码导出使用有界PE解析；gpti数据导出只取地址，不执行或解引用其值。 */
static BOOLEAN KswRuleImportTarget(const KSW_HOOK_SYSTEM_MODULE_ENTRY* Module,
    const KSW_RULE_IMAGE* Image, PCSTR Name, BOOLEAN DataExport, ULONG_PTR* AddressOut)
{
    // 普通函数导出与数据导出采用不同的真实性检查。
    uint32_t rva = 0U; // 同源PE接口固定使用uint32_t输出，不能以ULONG指针代传。
    // 代码入口可由共用PE解析器唯一解析并加实际模块基址。
    if (!DataExport) {
        if (!KswRuleFindExport(Image, Name, &rva)) return FALSE;
        *AddressOut = (ULONG_PTR)Module->ImageBase + rva; return TRUE;
    }
    // 数据导出使用受保护的内核导出查询，避免异常穿透。
    __try {
        IMAGE_NT_HEADERS headers; IMAGE_DATA_DIRECTORY directory;
        ULONG_PTR address = (ULONG_PTR)RtlFindExportedRoutineByName(Module->ImageBase, Name);
        // 数据地址必须完整落在提供者映像内。
        if (address < (ULONG_PTR)Module->ImageBase || address - (ULONG_PTR)Module->ImageBase > Module->ImageSize ||
            sizeof(PVOID) > Module->ImageSize - (address - (ULONG_PTR)Module->ImageBase)) return FALSE;
        rva = (uint32_t)(address - (ULONG_PTR)Module->ImageBase); // 上面的映像范围已限制为32位RVA。
        // 数据不能是forwarder字符串或可执行函数入口。
        if (!KswordARKHookReadImageNtHeaders(Module, &headers) ||
            !KswordARKHookGetDataDirectory(&headers, IMAGE_DIRECTORY_ENTRY_EXPORT, &directory) ||
            KswordARKHookIsRvaInsideDirectory((ULONG)rva, &directory) || KswRuleIsCode(Image, rva, 1U)) return FALSE; // Win32 helper只做按值转换，forwarder和函数入口都不是数据变量。
        *AddressOut = address; return TRUE;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return FALSE; }
}

/* 按实际INT名称找到槽后，还必须逐槽确认当前绑定等于同名真实导出。 */
static BOOLEAN KswRuleBindImport(const KSW_RULE_IMAGE* From, PCSTR Dll, PCSTR Name,
    const KSW_HOOK_SYSTEM_MODULE_ENTRY* ToModule, const KSW_RULE_IMAGE* To,
    ULONG ExpectedRva, BOOLEAN DataExport, BOOLEAN Required)
{
    // 先根据INT语义名字找槽，再读回当前IAT指针。
    uint32_t slot = 0U; ULONG_PTR actual = 0U, expected = 0U; // IAT查询输出与同源接口的uint32_t类型一致。
    // 可选导入缺失允许继续，必需导入缺失立即拒绝。
    if (!KswRuleFindImportSlot(From, Dll, Name, &slot)) return !Required;
    // 提供者导出、指定RVA及IAT实值必须三方相等。
    if (!KswRuleImportTarget(ToModule, To, Name, DataExport, &expected) ||
        (ExpectedRva != 0UL && expected != (ULONG_PTR)ToModule->ImageBase + ExpectedRva) ||
        !KswRuleRead(From, slot, &actual, sizeof(actual))) return FALSE;
    return actual == expected; // 仅在映像范围内或INT名字正确都不足以授权被重定向的IAT。
}

/* Owner来源是规则输出的精确IAT槽，反查该槽实际DLL/符号，不猜某个固定版本的getter。 */
static BOOLEAN KswRuleOwnerImport(const KSW_HOOK_SYSTEM_MODULE_ENTRY* Full,
    const IMAGE_NT_HEADERS* Headers, ULONG Slot, CHAR Dll[128], CHAR Name[256])
{
    // 反查owner导入槽时必须在真实导入目录里找到唯一名称。
    IMAGE_DATA_DIRECTORY directory; ULONG index, matches = 0UL;
    if (!KswordARKHookGetDataDirectory(Headers, IMAGE_DIRECTORY_ENTRY_IMPORT, &directory)) return FALSE;
    // 逐个描述符定位FirstThunk对应的原始INT项。
    for (index = 0UL; index < directory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR) && index < 65536UL; ++index) {
        IMAGE_IMPORT_DESCRIPTOR descriptor; ULONG rva, thunkIndex;
        if (!KswordARKHookAddRvaOffset(directory.VirtualAddress, index, (ULONG)sizeof(descriptor), &rva) ||
            !KswordARKHookReadImageBytes(Full, rva, &descriptor, sizeof(descriptor))) return FALSE;
        // 只有一个匹配项且存在终止描述符才可发布名称。
        if (!descriptor.Name && !descriptor.FirstThunk && !descriptor.OriginalFirstThunk) return matches == 1UL;
        if (!descriptor.OriginalFirstThunk || !descriptor.FirstThunk) return FALSE;
        // 逐槽建立实际IAT位置，不解析格式化字符串。
        for (thunkIndex = 0UL; thunkIndex < 65536UL; ++thunkIndex) {
            ULONG64 thunk = 0ULL; ULONG actualSlot = 0UL;
            if (!KswordARKHookAddRvaOffset(descriptor.OriginalFirstThunk, thunkIndex, (ULONG)sizeof(thunk), &rva) ||
                !KswordARKHookReadImageBytes(Full, rva, &thunk, sizeof(thunk))) return FALSE;
            if (!thunk) break; // 只能在此描述符的实际INT长度内比较FirstThunk槽。
            if (!KswordARKHookAddRvaOffset(descriptor.FirstThunk, thunkIndex, (ULONG)sizeof(thunk), &actualSlot)) return FALSE;
            if (actualSlot != Slot) continue;
            if ((thunk & IMAGE_ORDINAL_FLAG64) != 0ULL || thunk > MAXULONG - sizeof(USHORT)) return FALSE; // ordinal不能冒充已命名Owner来源。
            if (!KswordARKHookCopyImageAnsi(Full, descriptor.Name, Dll, 128U) ||
                !KswordARKHookCopyImageAnsi(Full, (ULONG)thunk + (ULONG)sizeof(USHORT), Name, 256U) || ++matches != 1UL) return FALSE;
        }
        if (thunkIndex == 65536UL) return FALSE;
    }
    return FALSE; // 缺少描述符终止项时不发布半解析结果。
}

/* 冷态必须绑定规则依赖的语义锚点；指纹本身只能检测后续变化，不能信任初始错绑。 */
static BOOLEAN KswRuleBindAnchors(const KSW_HOOK_SYSTEM_MODULE_ENTRY* Full,
    const KSW_HOOK_SYSTEM_MODULE_ENTRY* Base, const KSW_HOOK_SYSTEM_MODULE_INFORMATION* Modules,
    const KSW_RULE_IMAGE* FullImage, const KSW_RULE_IMAGE* BaseImage,
    const IMAGE_NT_HEADERS* Headers, const KSW_HOTKEY_RULE_RESULT* Result)
{
    // 语义锚点同时绑定Full、Base和系统内核的真实模块。
    const KSW_HOOK_SYSTEM_MODULE_ENTRY* kernel;
    KSW_RULE_KERNEL_IMAGE kernelContext; KSW_RULE_IMAGE kernelImage; uint32_t slot = 0U; // 不用指针cast掩盖ULONG与uint32_t的基础类型差异。
    CHAR dll[128], name[256]; // Owner只读取其实际导入身份，不枚举或执行额外模块代码。
    // 系统入口所在模块由实际加载模块表定位。
    kernel = KswordARKHookFindModuleForAddress(Modules, (ULONG_PTR)MmGetSystemRoutineAddress);
    if (!kernel) return FALSE;
    KswRuleKernelView(kernel, &kernelContext, &kernelImage);
    // 临界区、窗口验证和分配释放入口须与Base导出绑定。
    if (!KswRuleBindImport(FullImage, "win32kbase.sys", "EnterCrit", Base, BaseImage, Result->EnterCritRva, FALSE, TRUE) ||
        !KswRuleBindImport(FullImage, "win32kbase.sys", "ValidateHwnd", Base, BaseImage, Result->ValidateHwndRva, FALSE, TRUE) ||
        !KswRuleBindImport(FullImage, "win32kbase.sys", "Win32AllocPoolZInit", Base, BaseImage, 0UL, FALSE, TRUE) ||
        !KswRuleBindImport(FullImage, "win32kbase.sys", "Win32FreePool", Base, BaseImage, 0UL, FALSE, TRUE)) return FALSE;
    // LeaveCrit的两种导出名称按当前实际导入选择。
    if (KswRuleFindImportSlot(FullImage, "win32kbase.sys", "UserSessionSwitchLeaveCrit", &slot)) {
        if (!KswRuleBindImport(FullImage, "win32kbase.sys", "UserSessionSwitchLeaveCrit", Base, BaseImage, Result->LeaveCritRva, FALSE, TRUE)) return FALSE;
    } else if (!KswRuleBindImport(FullImage, "win32kbase.sys", "LeaveCrit", Base, BaseImage, Result->LeaveCritRva, FALSE, TRUE)) return FALSE;
    // owner数据、赋值锁与BugCheck都需要真实IAT绑定。
    if (!KswRuleBindImport(FullImage, "win32kbase.sys", "gptiCurrent", Base, BaseImage, 0UL, TRUE, FALSE) ||
        (Result->AssignmentRva != 0U && !KswRuleBindImport(FullImage, "win32kbase.sys", "HMAssignmentLock", Base, BaseImage, Result->AssignmentRva, FALSE, TRUE)) ||
        !KswRuleBindImport(FullImage, "ntoskrnl.exe", "KeBugCheckEx", kernel, &kernelImage, 0UL, FALSE, FALSE) ||
        !KswRuleBindImport(BaseImage, "ntoskrnl.exe", "KeBugCheckEx", kernel, &kernelImage, 0UL, FALSE, FALSE)) return FALSE; // no-return证明同样依赖真实的bugcheck入口。
    // 数据型owner来源必须恰好是gptiCurrent导入槽。
    if (Result->OwnerGetterIndirect == 2U) return KswRuleFindImportSlot(FullImage,
        "win32kbase.sys", "gptiCurrent", &slot) && slot == Result->OwnerGetterRva;
    // 函数型owner来源先反查实际导入，再限制提供者模块。
    if (Result->OwnerGetterIndirect != 1U || !KswRuleOwnerImport(Full, Headers, Result->OwnerGetterRva, dll, name)) return FALSE;
    if (KswordARKHookBoundedAnsiEqualsInsensitive((const UCHAR*)dll, sizeof(dll), "win32kbase.sys"))
        return KswRuleBindImport(FullImage, dll, name, Base, BaseImage, 0UL, FALSE, TRUE);
    if (KswordARKHookBoundedAnsiEqualsInsensitive((const UCHAR*)dll, sizeof(dll), "ntoskrnl.exe"))
        return KswRuleBindImport(FullImage, dll, name, kernel, &kernelImage, 0UL, FALSE, TRUE);
    return FALSE; // 未解析的提供者或forwarder保持明确不支持，不能按任意映像内地址接受。
}

/* 缓存指纹使用完整已分析函数片段，包含被排除候选的negative证明。 */
static BOOLEAN KswRuleCacheFunction(KSW_RULE_RUNTIME_WORK* Work, const KSW_RULE_IMAGE* Image,
    ULONG Module, ULONG Entry)
{
    // 待缓存的函数须由相同IR完整加载。
    ULONG i; /* 临时指令数组由同源引擎通过分页池分配。 */
    if (!KswRuleLoadFunction(Image, Entry, &Work->Function)) return FALSE;
    // 逐片段确认冷态前置护栏已经覆盖全部代码。
    for (i = 0; i < Work->Function.RangeCount; ++i) {
        ULONG guardIndex; BOOLEAN covered = FALSE;
        for (guardIndex = 0UL; guardIndex < Work->Guard.Count; ++guardIndex) {
            const KSW_RULE_FINGERPRINT* guard = &Work->Guard.Ranges[guardIndex];
            if (guard->Module == Module && guard->Rva <= Work->Function.Ranges[i].Begin &&
                guard->Bytes >= Work->Function.Ranges[i].End - guard->Rva) { covered = TRUE; break; }
        }
        // 有未覆盖函数片段时拒绝缓存该次证明。
        if (!covered) { KswRuleFreeFunction(Image, &Work->Function); return FALSE; } // 不能缓存未参加证明前稳定性检查的代码。
        // 将完整已分析片段登记到热路径指纹。
        if (!KswRuleCacheAddRange(&Work->Cache, Module, Work->Function.Ranges[i].Begin,
                Work->Function.Ranges[i].End - Work->Function.Ranges[i].Begin, Image->SizeOfImage)) {
            KswRuleFreeFunction(Image, &Work->Function); return FALSE;
        }
    }
    // 每个函数都通过原映像宿主释放临时IR。
    KswRuleFreeFunction(Image, &Work->Function);
    return TRUE;
}

/* PE头与导出、导入、异常、IAT目录也参与失效，不将其内容当版本授权。 */
static BOOLEAN KswRuleCacheMetadata(KSW_RULE_RUNTIME_CACHE* Cache, ULONG Module,
    const KSW_RULE_IMAGE* Image, const IMAGE_NT_HEADERS* Headers)
{
    // 元数据指纹涵盖导出、导入、异常和IAT目录。
    const ULONG indices[4] = {IMAGE_DIRECTORY_ENTRY_EXPORT, IMAGE_DIRECTORY_ENTRY_IMPORT,
        IMAGE_DIRECTORY_ENTRY_EXCEPTION, IMAGE_DIRECTORY_ENTRY_IAT};
    ULONG i, ntBytes; IMAGE_DOS_HEADER dos; /* 只登记实际存在的目录，并覆盖真实NT/节表位置。 */
    // 映像头、实际NT头和节表必须全部入缓存范围。
    if (!KswRuleCacheAddRange(Cache, Module, 0UL, Headers->OptionalHeader.SizeOfHeaders, Image->SizeOfImage)) return FALSE;
    if (!KswRuleRead(Image, 0U, &dos, sizeof(dos)) || dos.e_lfanew <= 0 || Headers->FileHeader.NumberOfSections > 96U) return FALSE;
    ntBytes = (ULONG)FIELD_OFFSET(IMAGE_NT_HEADERS, OptionalHeader) + Headers->FileHeader.SizeOfOptionalHeader +
        Headers->FileHeader.NumberOfSections * (ULONG)sizeof(IMAGE_SECTION_HEADER);
    if (!KswRuleCacheAddRange(Cache, Module, (ULONG)dos.e_lfanew, ntBytes, Image->SizeOfImage)) return FALSE;
    // 仅登记实际存在且范围完整的数据目录。
    for (i = 0; i < RTL_NUMBER_OF(indices); ++i) {
        IMAGE_DATA_DIRECTORY directory;
        if (KswordARKHookGetDataDirectory(Headers, indices[i], &directory) && directory.Size != 0UL &&
            !KswRuleCacheAddRange(Cache, Module, directory.VirtualAddress, directory.Size, Image->SizeOfImage)) return FALSE;
    }
    // 目录外的导入名称与槽位由专门函数继续登记。
    return KswRuleCacheImports(Cache, Module, Image, Headers);
}

/* 只在冷匹配前后核对全部驻留可执行节；INIT等可丢弃节不参与当前运行证明。 */
static BOOLEAN KswRuleCacheExecutable(KSW_RULE_RUNTIME_CACHE* Guard, ULONG Module,
    const KSW_HOOK_SYSTEM_MODULE_ENTRY* Entry, const IMAGE_NT_HEADERS* Headers)
{
    // 冷态护栏遍历当前映像的驻留可执行节。
    IMAGE_DOS_HEADER dos; ULONG sectionTable = 0UL, i;
    if (!KswordARKHookReadImageBytes(Entry, 0UL, &dos, sizeof(dos)) || dos.e_lfanew <= 0 ||
        Headers->FileHeader.NumberOfSections > 96U || !KswordARKHookAddRvaOffset((ULONG)dos.e_lfanew, 1UL,
            (ULONG)FIELD_OFFSET(IMAGE_NT_HEADERS, OptionalHeader) + Headers->FileHeader.SizeOfOptionalHeader, &sectionTable)) return FALSE;
    // 逐节跳过非代码或可丢弃区域。
    for (i = 0UL; i < Headers->FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER section; ULONG sectionRva = 0UL, bytes;
        if (!KswordARKHookAddRvaOffset(sectionTable, i, (ULONG)sizeof(section), &sectionRva) ||
            !KswordARKHookReadImageBytes(Entry, sectionRva, &section, sizeof(section))) return FALSE;
        if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0UL ||
            (section.Characteristics & IMAGE_SCN_MEM_DISCARDABLE) != 0UL) continue;
        // 节大小由虚拟/原始范围决定并纳入前后稳定性检查。
        bytes = section.Misc.VirtualSize > section.SizeOfRawData ? section.Misc.VirtualSize : section.SizeOfRawData;
        if (bytes != 0UL && !KswRuleCacheAddRange(Guard, Module, section.VirtualAddress, bytes, Entry->ImageSize)) return FALSE;
    }
    return TRUE;
}

/* 指纹只读取原始字节，不重新解码CFG；完整失败时不发布旧证明。 */
static BOOLEAN KswRuleHashRange(PVOID Base, ULONG Size, ULONG Rva, ULONG Bytes, ULONG64* Hash)
{
    // FNV指纹按小缓冲分片处理，限制栈占用。
    UCHAR buffer[256]; /* 大范围也只使用小型有界栈缓冲。 */
    ULONG offset = 0UL; ULONG64 hash = 14695981039346656037ULL;
    // 起点、长度或基址指针溢出则拒绝读取。
    if (Rva > Size || Bytes > Size - Rva || (ULONG_PTR)Base > MAXULONG_PTR - Size) return FALSE;
    // 循环读取每个片段并逐字节更新64位散列。
    while (offset < Bytes) {
        ULONG count = Bytes - offset, i;
        if (count > sizeof(buffer)) count = (ULONG)sizeof(buffer);
        if (!KswordARKHookReadMemorySafe((const UCHAR*)Base + Rva + offset, buffer, count)) return FALSE;
        for (i = 0; i < count; ++i) { hash ^= buffer[i]; hash *= 1099511628211ULL; }
        offset += count;
    }
    // 完整范围读取成功后才输出最终散列。
    *Hash = hash;
    return TRUE;
}

/* 对同一缓存映像逐段核对；调用者持有缓存锁，返回后只取scalar副本。 */
static BOOLEAN KswRuleCacheCheck(KSW_RULE_RUNTIME_CACHE* Cache, BOOLEAN Initialize)
{
    // 缓存核对同时遍历范围指纹和原始指令证据。
    ULONG i; ULONG64 total = 0ULL;
    // 累计读取量超过32MiB时拒绝过大的冷/热缓存。
    for (i = 0; i < Cache->Count; ++i) {
        KSW_RULE_FINGERPRINT* range = &Cache->Ranges[i]; ULONG64 hash = 0ULL;
        PVOID base = range->Module ? Cache->Runtime.Win32kbaseBase : Cache->Runtime.Win32kfullBase;
        ULONG size = range->Module ? Cache->Runtime.Win32kbaseSize : Cache->Runtime.Win32kfullSize;
        total += range->Bytes; if (total > 32ULL * 1024ULL * 1024ULL) return FALSE;
        // 任何字节读取失败都使当前证明失效。
        if (!KswRuleHashRange(base, size, range->Rva, range->Bytes, &hash)) return FALSE;
        // 初始化时记录散列，复查时要求散列逐段一致。
        if (Initialize) range->Hash = hash; else if (range->Hash != hash) return FALSE;
    }
    // 每条指令证据重新读回并与初次捕获字节精确比较。
    for (i = 0; i < Cache->EvidenceCount; ++i) {
        const KSW_HOTKEY_RULE_EVIDENCE* evidence = &Cache->Evidence[i]; UCHAR bytes[15];
        if (!evidence->Length || evidence->Length > sizeof(bytes) || evidence->Rva > Cache->Runtime.Win32kfullSize ||
            evidence->Length > Cache->Runtime.Win32kfullSize - evidence->Rva ||
            !KswordARKHookReadMemorySafe((const UCHAR*)Cache->Runtime.Win32kfullBase + evidence->Rva, bytes, evidence->Length) ||
            RtlCompareMemory(bytes, evidence->Bytes, evidence->Length) != evidence->Length) return FALSE;
    }
    // 零范围缓存不能被视为有效证明。
    return Cache->Count != 0UL;
}

/* 首次或代码变化时运行通用规则；成功缓存不使其它版本自动获得支持。 */
NTSTATUS KswordARKKeyboardResolveRuleSchema(const KSW_HOOK_SYSTEM_MODULE_ENTRY* Full,
    const KSW_HOOK_SYSTEM_MODULE_ENTRY* Base, const KSW_HOOK_SYSTEM_MODULE_INFORMATION* Modules,
    KSW_KEYBOARD_HOTKEY_RUNTIME* RuntimeOut)
{
    // 解析过程的完整工作区一次分配，避免大型内核栈对象。
    KSW_RULE_RUNTIME_WORK* work = NULL; KSW_RULE_KERNEL_IMAGE contexts[2]; KSW_RULE_IMAGE images[2];
    IMAGE_NT_HEADERS fullHeaders, baseHeaders; ULONG fullStamp = 0UL, baseStamp = 0UL, checkStamp = 0UL, i;
    NTSTATUS status = STATUS_NOT_SUPPORTED;
    // 输出、模块列表和调用级别先过前置门禁。
    if (!RuntimeOut || !Modules || KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_PARAMETER;
    // 失败前先清空调用方结果，禁止残留旧schema。
    RtlZeroMemory(RuntimeOut, sizeof(*RuntimeOut));
    // Full与Base的实际PE头都须与模块表匹配。
    if (!KswRuleHeaderStamp(Full, &fullStamp, &fullHeaders) || !KswRuleHeaderStamp(Base, &baseStamp, &baseHeaders)) return STATUS_INVALID_IMAGE_FORMAT;
    // 缓存元数据在排他锁下更新，临界区保护锁取得过程。
    KeEnterCriticalRegion(); ExAcquirePushLockExclusive(&g_KswRuleCacheLock);
    // 只有映像实例、规则版本与全部指纹一致才能走热路径。
    if (g_KswRuleCache.Valid && g_KswRuleCache.RuleVersion == KSW_HOTKEY_RULE_VERSION &&
        g_KswRuleCache.Runtime.Win32kfullBase == Full->ImageBase && g_KswRuleCache.Runtime.Win32kfullSize == Full->ImageSize &&
        g_KswRuleCache.Runtime.Win32kbaseBase == Base->ImageBase && g_KswRuleCache.Runtime.Win32kbaseSize == Base->ImageSize &&
        g_KswRuleCache.FullStamp == fullStamp && g_KswRuleCache.BaseStamp == baseStamp && KswRuleCacheCheck(&g_KswRuleCache, FALSE)) {
        // 热路径仅返回已缓存的标量schema。
        *RuntimeOut = g_KswRuleCache.Runtime; status = STATUS_SUCCESS; goto done;
    }
    g_KswRuleCache.Valid = FALSE; /* 旧证明从此不可继续写入。 */
    // 缓存失效后分配新的冷态工作区。
    work = (KSW_RULE_RUNTIME_WORK*)KswRuleKernelAllocate(NULL, sizeof(*work));
    if (!work) { status = STATUS_INSUFFICIENT_RESOURCES; goto done; }
    // 建立两个实际映像的只读回调视图。
    RtlZeroMemory(work, sizeof(*work)); KswRuleKernelView(Full, &contexts[0], &images[0]); KswRuleKernelView(Base, &contexts[1], &images[1]);
    // 护栏持有Full/Base真实基址与大小供前后对比。
    work->Guard.Runtime.Win32kfullBase = Full->ImageBase; work->Guard.Runtime.Win32kfullSize = Full->ImageSize;
    work->Guard.Runtime.Win32kbaseBase = Base->ImageBase; work->Guard.Runtime.Win32kbaseSize = Base->ImageSize;
    // 证明前先读取全部驻留代码与PE元数据的稳定基线。
    if (!KswRuleCacheExecutable(&work->Guard, 0UL, Full, &fullHeaders) ||
        !KswRuleCacheExecutable(&work->Guard, 1UL, Base, &baseHeaders) ||
        !KswRuleCacheMetadata(&work->Guard, 0UL, &images[0], &fullHeaders) ||
        !KswRuleCacheMetadata(&work->Guard, 1UL, &images[1], &baseHeaders) || !KswRuleCacheCheck(&work->Guard, TRUE)) {
        status = STATUS_PARTIAL_COPY; goto done; // 证明开始前没有完整稳定性基线就不运行授权规则。
    }
    // 基线选定后PE头发生变化即拒绝继续分析。
    if (!KswRuleHeaderStamp(Full, &checkStamp, &work->CheckFullHeaders) ||
        RtlCompareMemory(&fullHeaders, &work->CheckFullHeaders, sizeof(fullHeaders)) != sizeof(fullHeaders) ||
        !KswRuleHeaderStamp(Base, &checkStamp, &work->CheckBaseHeaders) ||
        RtlCompareMemory(&baseHeaders, &work->CheckBaseHeaders, sizeof(baseHeaders)) != sizeof(baseHeaders)) {
        status = STATUS_REVISION_MISMATCH; goto done; // 选取前置指纹范围时头部已变化则整次重试。
    }
    // 调用通用C规则引擎；资源不足与结构不匹配分开回执。
    if (!KswHotkeyMatchRules(&images[0], &images[1], &work->Result)) {
        if (work->Result.Error == KSW_HK_RULE_RESOURCE) status = STATUS_INSUFFICIENT_RESOURCES;
        goto done; // 不把真实资源失败伪装成正常无结果。
    }
    // 证明输出还需绑定实际IAT并通过运行时schema限制。
    if (!KswRuleBindAnchors(Full, Base, Modules, &images[0], &images[1], &fullHeaders, &work->Result) ||
        !KswRuleCopySchema(&work->Result, Full, Base, fullStamp, &work->Cache.Runtime)) goto done; // 错绑IAT不允许进入scalar或成功缓存。
    // 分析函数与证据项必须非空且不越过定长缓存数组。
    if (work->Result.AnalyzedFunctionCount == 0U || work->Result.AnalyzedFunctionCount > RTL_NUMBER_OF(work->Result.AnalyzedFunctions) ||
        work->Result.EvidenceCount > RTL_NUMBER_OF(work->Cache.Evidence)) goto done;
    // Full中参与正负候选判断的每个函数都纳入缓存。
    for (i = 0; i < work->Result.AnalyzedFunctionCount; ++i)
        if (!KswRuleCacheFunction(work, &images[0], 0UL, work->Result.AnalyzedFunctions[i])) goto done;
    // Base中实际使用的临界区、窗口与赋值锁函数同样纳入指纹。
    if (!KswRuleCacheFunction(work, &images[1], 1UL, work->Result.EnterCritRva) ||
        !KswRuleCacheFunction(work, &images[1], 1UL, work->Result.LeaveCritRva) ||
        !KswRuleCacheFunction(work, &images[1], 1UL, work->Result.ValidateHwndRva) ||
        (work->Result.AssignmentRva != 0U && !KswRuleCacheFunction(work, &images[1], 1UL, work->Result.AssignmentRva)) ||
        !KswRuleCacheMetadata(&work->Cache, 0UL, &images[0], &fullHeaders) ||
        !KswRuleCacheMetadata(&work->Cache, 1UL, &images[1], &baseHeaders)) goto done;
    // 把匹配时捕获的指令字节保存到缓存证据表。
    work->Cache.EvidenceCount = work->Result.EvidenceCount;
    RtlCopyMemory(work->Cache.Evidence, work->Result.Evidence, sizeof(work->Cache.Evidence[0]) * work->Cache.EvidenceCount);
    // 最终指纹同时匹配新schema和证明前护栏才可发布。
    if (!KswRuleCacheCheck(&work->Cache, TRUE) || !KswRuleCacheCheck(&work->Guard, FALSE)) {
        status = STATUS_REVISION_MISMATCH; goto done; // Core前后任一代码/目录变化都拒绝本次证明。
    }
    // 记录本次映像和规则版本，供以后识别同一缓存实例。
    work->Cache.FullStamp = fullStamp; work->Cache.BaseStamp = baseStamp; work->Cache.RuleVersion = KSW_HOTKEY_RULE_VERSION;
    // 代次绕过零值，避免零被误作未初始化身份。
    if (++g_KswRuleGeneration == 0UL) ++g_KswRuleGeneration;
    // 只有全部校验通过才设置有效位并发布新代次。
    work->Cache.Runtime.RuleGeneration = g_KswRuleGeneration; work->Cache.Valid = TRUE;
    // 在锁内复制完整缓存，再向调用方返回标量结果。
    RtlCopyMemory(&g_KswRuleCache, &work->Cache, sizeof(g_KswRuleCache));
    *RuntimeOut = g_KswRuleCache.Runtime; status = STATUS_SUCCESS;
done:
    // 无论成功失败都释放冷态工作区并退出缓存锁。
    if (work) KswRuleKernelFree(NULL, work);
    ExReleasePushLockExclusive(&g_KswRuleCacheLock); KeLeaveCriticalRegion();
    return status;
}

/* 写入前只复核缓存代码及会话IAT，不在USER临界区重新建立证明图。 */
NTSTATUS KswordARKKeyboardValidateRuleRuntime(const KSW_KEYBOARD_HOTKEY_RUNTIME* Runtime)
{
    // 写前核验默认返回版本不匹配，只有完整复查才改为成功。
    NTSTATUS status = STATUS_REVISION_MISMATCH; ULONG_PTR getter = 0U; BOOLEAN currentGeneration = FALSE;
    // 旧schema或非被动级别不能进入写前复核。
    if (!Runtime || !Runtime->RuleProven || KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_STATE;
    // 持排他锁防止复查期间缓存代次被替换。
    KeEnterCriticalRegion(); ExAcquirePushLockExclusive(&g_KswRuleCacheLock);
    // 调用方代次、模块基址和大小须对应当前缓存。
    if (g_KswRuleCache.Valid && Runtime->RuleGeneration == g_KswRuleCache.Runtime.RuleGeneration &&
        Runtime->Win32kfullBase == g_KswRuleCache.Runtime.Win32kfullBase && Runtime->Win32kbaseBase == g_KswRuleCache.Runtime.Win32kbaseBase &&
        Runtime->Win32kfullSize == g_KswRuleCache.Runtime.Win32kfullSize && Runtime->Win32kbaseSize == g_KswRuleCache.Runtime.Win32kbaseSize) {
        currentGeneration = TRUE; // 旧代次调用只能拒绝自己，不能使其它线程刚生成的新证明失效。
        // 先复查全部代码/元数据指纹，再复查会话getter的实际IAT指针。
        if (KswRuleCacheCheck(&g_KswRuleCache, FALSE)) {
            if (Runtime->SessionGetterAddress != 0U && Runtime->SessionGetterSlotRva <= Runtime->Win32kfullSize &&
                sizeof(getter) <= Runtime->Win32kfullSize - Runtime->SessionGetterSlotRva &&
                KswordARKHookReadMemorySafe((const UCHAR*)Runtime->Win32kfullBase + Runtime->SessionGetterSlotRva, &getter, sizeof(getter)) &&
                getter == Runtime->SessionGetterAddress) status = STATUS_SUCCESS;
        }
    }
    // 当前代次失配后撤销缓存有效位；旧代次失败不得误伤新缓存。
    if (currentGeneration && !NT_SUCCESS(status)) g_KswRuleCache.Valid = FALSE;
    // 释放缓存锁并返回精确复查状态。
    ExReleasePushLockExclusive(&g_KswRuleCacheLock); KeLeaveCriticalRegion();
    return status;
}
