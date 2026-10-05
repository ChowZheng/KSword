/* 回调全局特征回退离线回归：直接编译生产 helper，只模拟操作系统和安全读取，不读取内核、不注册回调、不加载驱动。 */
#include <ntddk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../KswordARKDriver/src/features/callback/callback_global_fallback.h"

/* 将虚构的系统地址映射到测试缓冲区，确保测试无法访问真实系统地址。 */
typedef struct _REPLAY_REGION
{
    ULONG_PTR Address; // 此测试区域的虚构系统地址。
    SIZE_T Size; // 测试区域边界，短读必须拒绝。
    VOID* Bytes; // 实际存储仅在本地用户态测试缓冲区。
} REPLAY_REGION;
static REPLAY_REGION gRegions[8]; // 虚构内存映射表，容量覆盖所有固定夹具。
static ULONG gRegionCount; // 当前虚构可读区域数量。
static ULONG gReferenceCount; // 模拟扫描器返回的候选数量。
static ULONG gChecks; // 已执行的断言数量。
static ULONG gFailures; // 断言失败数量。
static ULONG gAllocations; // 当前未释放的池分配模拟数量。
static KSW_RUNTIME_DATA_REFERENCE gReferences[256]; // 与生产入口候选容量相同的扫描结果夹具。
static KIRQL gIrql; // 当前模拟 IRQL。
static ULONG_PTR gFailRead; // 注入安全读取失败的地址。
static ULONG_PTR gChangeRead; // 注入第二次读取变化的地址。
static ULONG_PTR gNonExecutable; // 模拟落入模块数据节而非可执行节的函数地址。
static ULONG_PTR gReadOnly; // 模拟只读节候选，不能作为全局容器。
static ULONG gChangeReads; // 目标地址已被读取的次数。
static SIZE_T gChangeByte; // 第二次读取时变化的字节，默认选择最后一个字节。
static BOOLEAN gRegistryLayout; // 当前自 Cookie 布局校准是否成功。
static BOOLEAN gAllocationFail; // 模拟兼容池分配器资源不足。
static PCSTR gFirstAnchor; // 捕获家族首个公开导出名称，防止三个数组用错入口。
static ULONG_PTR gArrays[2][64]; // 两个独立通知数组，用于验证多候选歧义。
static ULONG_PTR gBlocks[4][3]; // 通知块 Rundown/Function/Context 前缀。
static ULONG_PTR gHead[2]; // Registry 双向链头。
static ULONG_PTR gNodes[2][6]; // 两个完整 Registry 节点前缀。
#define REPLAY_BASE ((ULONG_PTR)0xFFFF800000000000ULL)
#define ARRAY_A (REPLAY_BASE + 0x1000U)
#define ARRAY_B (REPLAY_BASE + 0x2000U)
#define BLOCK_A (REPLAY_BASE + 0x3000U)
#define BLOCK_B (REPLAY_BASE + 0x3100U)
#define HEAD (REPLAY_BASE + 0x4000U)
#define NODE_A (REPLAY_BASE + 0x5000U)
#define NODE_B (REPLAY_BASE + 0x5100U)
#define CALLBACK_A (REPLAY_BASE + 0x8000U)
#define CALLBACK_B (REPLAY_BASE + 0x9000U)

// 统一统计断言；失败显示对应场景，最终进程退出码反映所有失败。
static VOID Check(BOOLEAN Condition, PCSTR Name)
{
    ++gChecks;
    if (!Condition) {
        ++gFailures;
        printf("FAIL: %s\n", Name);
    }
}
// 登记一个有界虚构内存区，模拟读取器仅能复制这些本地缓冲区。
static VOID Region(ULONG_PTR Address, VOID* Bytes, SIZE_T Size)
{
    gRegions[gRegionCount].Address = Address;
    gRegions[gRegionCount].Bytes = Bytes;
    gRegions[gRegionCount].Size = Size;
    ++gRegionCount;
}
// 重置为一个完整、有效、含精确自回调的通知数组；每个场景只改变需要验证的条件。
static VOID Reset(VOID)
{
    RtlZeroMemory(gArrays, sizeof(gArrays));
    RtlZeroMemory(gBlocks, sizeof(gBlocks));
    RtlZeroMemory(gHead, sizeof(gHead));
    RtlZeroMemory(gNodes, sizeof(gNodes));
    RtlZeroMemory(gReferences, sizeof(gReferences));
    gRegionCount = 0;
    Region(ARRAY_A, gArrays[0], sizeof(gArrays[0]));
    Region(ARRAY_B, gArrays[1], sizeof(gArrays[1]));
    Region(BLOCK_A, gBlocks[0], sizeof(gBlocks[0]));
    Region(BLOCK_B, gBlocks[1], sizeof(gBlocks[1]));
    Region(HEAD, gHead, sizeof(gHead));
    Region(NODE_A, gNodes[0], sizeof(gNodes[0]));
    Region(NODE_B, gNodes[1], sizeof(gNodes[1]));
    gReferenceCount = 1;
    gReferences[0].Address = ARRAY_A;
    gArrays[0][0] = BLOCK_A | 3U;
    gBlocks[0][1] = CALLBACK_A;
    gBlocks[0][2] = 2U;
    gIrql = PASSIVE_LEVEL;
    gFailRead = 0;
    gChangeRead = 0;
    gChangeReads = 0;
    gChangeByte = MAXULONG_PTR;
    gNonExecutable = 0;
    gReadOnly = 0;
    gRegistryLayout = TRUE;
    gAllocationFail = FALSE;
    gFirstAnchor = NULL;
}
// 替代统一安全读取器；按区域边界复制本地数据，并可注入失败或第二次快照变化。
static BOOLEAN ReplayRead(const VOID* Address, VOID* Output, SIZE_T Size)
{
    ULONG index; // 当前有界虚构区域下标。
    ULONG_PTR address = (ULONG_PTR)Address; // 只用于映射查找，不直接解引用此虚构系统地址。
    if (address == gFailRead) {
        return FALSE;
    }
    for (index = 0; index < gRegionCount; ++index) {
        REPLAY_REGION* region = &gRegions[index]; // 当前本地虚构内存区域。
        if (address >= region->Address && address - region->Address <= region->Size &&
            Size <= region->Size - (address - region->Address)) {
            memcpy(Output, (const UCHAR*)region->Bytes + (address - region->Address), Size);
            if (address == gChangeRead && ++gChangeReads == 2U) {
                ((UCHAR*)Output)[gChangeByte < Size ? gChangeByte : Size - 1U] ^= 1U;
            }
            return TRUE;
        }
    }
    return FALSE;
}
// 替代共享扫描器，检查三锚点、两级调用图和单函数预算，然后提供指定候选集。
static ULONG ReplayCollect(const KSW_RUNTIME_IMAGE_VIEW* View, PCSTR const* Anchors,
    ULONG AnchorCount, ULONG Depth, ULONG ScanBytes, KSW_RUNTIME_DATA_REFERENCE* References, ULONG Capacity)
{
    UNREFERENCED_PARAMETER(View);
    Check(AnchorCount == 3UL && Depth == 2UL && ScanBytes == 0x800UL, "bounded family scanner contract");
    gFirstAnchor = Anchors[0];
    if (gReferenceCount <= Capacity) {
        memcpy(References, gReferences, sizeof(*References) * gReferenceCount);
    }
    return gReferenceCount;
}
// 仅承认完整数组或链头边界，模拟只读节拒绝与 writable 数据节验证。
static BOOLEAN ReplayWritable(const KSW_RUNTIME_IMAGE_VIEW* View, ULONG_PTR Address, SIZE_T Size)
{
    UNREFERENCED_PARAMETER(View);
    if (Address == gReadOnly) {
        return FALSE;
    }
    return ((Address == ARRAY_A || Address == ARRAY_B) && Size == sizeof(gArrays[0])) ||
        (Address == HEAD && Size == sizeof(gHead));
}
// 模拟兼容非分页池分配，并统计资源用于检测所有返回路径是否释放。
static PVOID ReplayAllocate(SIZE_T Size, ULONG Tag)
{
    UNREFERENCED_PARAMETER(Tag);
    if (gAllocationFail) {
        return NULL;
    }
    ++gAllocations;
    return malloc(Size);
}
// 释放本地模拟分配并减少未释放计数。
static VOID ReplayFree(PVOID Address, ULONG Tag)
{
    UNREFERENCED_PARAMETER(Tag);
    --gAllocations;
    free(Address);
}
// 返回场景设定的 IRQL，验证高 IRQL 下不会进入候选读取。
static KIRQL ReplayIrql(VOID)
{
    return gIrql;
}
// 仅两个明确的代码地址可以通过，数据节反例可单独注入。
static BOOLEAN ReplayExecutable(PVOID Context, ULONG_PTR Address)
{
    UNREFERENCED_PARAMETER(Context);
    return (Address == CALLBACK_A || Address == CALLBACK_B) && Address != gNonExecutable;
}
// 模拟现有自注册 Cookie 校准接口；未校准时不能解释 Registry 固定前缀。
static BOOLEAN ReplayRegistryLayout(PVOID Context, ULONG_PTR Address)
{
    UNREFERENCED_PARAMETER(Context);
    return gRegistryLayout && Address == HEAD;
}

/* 将生产入口所需的 OS 服务替换为本文件模拟；下面直接 include 实际生产 .c，不复制判据。 */
#undef MmSystemRangeStart
#define MmSystemRangeStart ((PVOID)REPLAY_BASE)
#define KswordARKRuntimeReadMemory ReplayRead
#define KswordARKRuntimeCollectFunctionDataReferences ReplayCollect
#define KswordARKRuntimeAddressIsWritableData ReplayWritable
#define KswordARKAllocateNonPagedPool ReplayAllocate
#define ExFreePoolWithTag ReplayFree
#undef KeGetCurrentIrql
#define KeGetCurrentIrql ReplayIrql
#include "../KswordARKDriver/src/features/callback/callback_global_fallback.c"

// 执行实际生产入口，联合检查状态、输出地址及资源释放，失败地址必须清零。
static VOID Expect(KSW_CALLBACK_GLOBAL_FAMILY Family, ULONG_PTR Known, NTSTATUS Expected,
    ULONG_PTR ExpectedAddress, PCSTR Name)
{
    KSW_RUNTIME_IMAGE_VIEW view; // 扫描器被模拟，视图仅作为生产函数的参数契约。
    ULONG_PTR address = 123U; // 用非零哨兵验证所有失败输出确实清零。
    NTSTATUS status; // 记录生产入口实际返回状态。
    RtlZeroMemory(&view, sizeof(view));
    status = KswordArkCallbackGlobalFallbackResolve(&view, Family, Known, ReplayExecutable,
        ReplayRegistryLayout, NULL, &address);
    Check(status == Expected && address == ExpectedAddress && gAllocations == 0U, Name);
}
// 构造两个节点的完整 reciprocal Registry 环形链，提供 Cookie/Context/Function 三元组。
static VOID Registry(VOID)
{
    Reset();
    gReferences[0].Address = HEAD;
    gHead[0] = NODE_A;
    gHead[1] = NODE_B;
    gNodes[0][0] = NODE_B;
    gNodes[0][1] = HEAD;
    gNodes[0][3] = 10U;
    gNodes[0][4] = 11U;
    gNodes[0][5] = CALLBACK_A;
    gNodes[1][0] = HEAD;
    gNodes[1][1] = NODE_A;
    gNodes[1][3] = 20U;
    gNodes[1][4] = 21U;
    gNodes[1][5] = CALLBACK_B;
}
// 分别验证家族身份、完整数组、并发变化、失败边界及 Registry 完整链校准。
int main(VOID)
{

    // 家族入口与自回调身份：Process/Thread/Image 使用各自精确函数地址。
    Reset();
    Expect(KswCallbackGlobalProcess, CALLBACK_A, STATUS_SUCCESS, ARRAY_A, "unique process array");
    Check(strcmp(gFirstAnchor, "PsSetCreateProcessNotifyRoutine") == 0, "process family anchors");

    Reset();
    Expect(KswCallbackGlobalThread, CALLBACK_A, STATUS_SUCCESS, ARRAY_A, "unique thread array");
    Check(strcmp(gFirstAnchor, "PsRemoveCreateThreadNotifyRoutine") == 0, "thread family anchors");

    Reset();
    Expect(KswCallbackGlobalImage, CALLBACK_A, STATUS_SUCCESS, ARRAY_A, "unique image array");
    Check(strcmp(gFirstAnchor, "PsRemoveLoadImageNotifyRoutine") == 0, "image family anchors");

    Reset();
    Expect(KswCallbackGlobalProcess, CALLBACK_B, STATUS_NOT_FOUND, 0U, "different self callback rejects family");

    Reset();
    Expect(KswCallbackGlobalImage, 0U, STATUS_SUCCESS, ARRAY_A, "structural evidence without self registration");

    // 全局候选必须 writable、非空、完整可读且唯一；重复引用本身不构成歧义。
    Reset();
    gReadOnly = ARRAY_A;
    Expect(KswCallbackGlobalProcess, CALLBACK_A, STATUS_NOT_FOUND, 0U, "read-only global rejected");

    Reset();
    gReferenceCount = 0;
    Expect(KswCallbackGlobalProcess, CALLBACK_A, STATUS_NOT_FOUND, 0U, "no references unavailable");

    Reset();
    gArrays[0][0] = 0;
    Expect(KswCallbackGlobalProcess, 0U, STATUS_NOT_FOUND, 0U, "empty writable data rejected");

    Reset();
    gReferenceCount = 2;
    gReferences[1].Address = ARRAY_B;
    gArrays[1][0] = BLOCK_A | 3U;
    Expect(KswCallbackGlobalProcess, CALLBACK_A, STATUS_OBJECT_NAME_COLLISION, 0U, "two valid containers ambiguous");

    Reset();
    gReferenceCount = 2;
    gReferences[1].Address = ARRAY_A;
    Expect(KswCallbackGlobalProcess, CALLBACK_A, STATUS_SUCCESS, ARRAY_A, "duplicate reference does not imply ambiguity");

    Reset();
    gArrays[0][63] = 7U;
    Expect(KswCallbackGlobalProcess, CALLBACK_A, STATUS_NOT_FOUND, 0U, "bad last slot rejects whole array");

    Reset();
    gArrays[0][63] = BLOCK_B | 2U;
    gBlocks[1][1] = CALLBACK_B;
    gNonExecutable = CALLBACK_B;
    Expect(KswCallbackGlobalProcess, CALLBACK_A, STATUS_NOT_FOUND, 0U, "non-executable callback rejects whole array");

    Reset();
    gFailRead = BLOCK_A;
    Expect(KswCallbackGlobalProcess, CALLBACK_A, STATUS_NOT_FOUND, 0U, "unreadable routine block");

    Reset();
    gFailRead = ARRAY_A;
    Expect(KswCallbackGlobalProcess, CALLBACK_A, STATUS_NOT_FOUND, 0U, "incomplete array unavailable");

    // 并发快照：仅引用计数位变化仍可用，块指针、函数或上下文变化必须拒绝。
    Reset();
    gChangeRead = ARRAY_A;
    gChangeByte = 0U;
    Expect(KswCallbackGlobalProcess, CALLBACK_A, STATUS_SUCCESS, ARRAY_A, "fast-ref count change preserves block identity");

    Reset();
    gChangeRead = ARRAY_A;
    Expect(KswCallbackGlobalProcess, CALLBACK_A, STATUS_NOT_FOUND, 0U, "array block pointer changed during validation");

    Reset();
    gChangeRead = ARRAY_A;
    gChangeByte = 63U * sizeof(ULONG_PTR);
    Expect(KswCallbackGlobalProcess, CALLBACK_A, STATUS_NOT_FOUND, 0U, "empty slot changed into count-only invalid value");

    Reset();
    gChangeRead = BLOCK_A;
    gChangeByte = sizeof(ULONG_PTR);
    Expect(KswCallbackGlobalProcess, CALLBACK_A, STATUS_NOT_FOUND, 0U, "routine block function changed");

    Reset();
    gChangeRead = BLOCK_A;
    Expect(KswCallbackGlobalProcess, CALLBACK_A, STATUS_NOT_FOUND, 0U, "routine block context changed");

    Reset();
    gArrays[0][1] = BLOCK_A | 3U;
    Expect(KswCallbackGlobalProcess, CALLBACK_A, STATUS_NOT_FOUND, 0U, "duplicate self callback rejected");

    // 扫描截断、高 IRQL、资源不足和未知家族均必须零地址失败。
    Reset();
    gReferenceCount = 256;
    Expect(KswCallbackGlobalProcess, CALLBACK_A, STATUS_BUFFER_OVERFLOW, 0U, "candidate truncation unavailable");

    Reset();
    gIrql = DISPATCH_LEVEL;
    Expect(KswCallbackGlobalProcess, CALLBACK_A, STATUS_INVALID_DEVICE_STATE, 0U, "high IRQL rejected before read");

    Reset();
    gAllocationFail = TRUE;
    Expect(KswCallbackGlobalProcess, CALLBACK_A, STATUS_INSUFFICIENT_RESOURCES, 0U, "allocation failure unavailable");

    Reset();
    Expect((KSW_CALLBACK_GLOBAL_FAMILY)99, CALLBACK_A, STATUS_INVALID_PARAMETER, 0U, "unknown family rejected");

    // Registry 必须校准当前布局、逐跳互指、闭合并在两次完整遍历间身份一致。
    Registry();
    Expect(KswCallbackGlobalRegistry, CALLBACK_A, STATUS_SUCCESS, HEAD, "complete calibrated reciprocal registry chain");
    Check(strcmp(gFirstAnchor, "CmUnRegisterCallback") == 0, "registry family anchors");

    Registry();
    gRegistryLayout = FALSE;
    Expect(KswCallbackGlobalRegistry, CALLBACK_A, STATUS_NOT_FOUND, 0U, "uncalibrated registry layout unavailable");

    Registry();
    gNodes[1][1] = HEAD;
    Expect(KswCallbackGlobalRegistry, CALLBACK_A, STATUS_NOT_FOUND, 0U, "registry forward-backward corruption");

    Registry();
    gHead[1] = NODE_A;
    Expect(KswCallbackGlobalRegistry, CALLBACK_A, STATUS_NOT_FOUND, 0U, "registry head tail mismatch");

    Registry();
    gNodes[1][0] = NODE_A;
    Expect(KswCallbackGlobalRegistry, CALLBACK_A, STATUS_NOT_FOUND, 0U, "registry cycle does not terminate at head");

    Registry();
    gNodes[1][3] = 0U;
    Expect(KswCallbackGlobalRegistry, CALLBACK_A, STATUS_NOT_FOUND, 0U, "zero registry cookie rejected");

    Registry();
    gNonExecutable = CALLBACK_B;
    Expect(KswCallbackGlobalRegistry, CALLBACK_A, STATUS_NOT_FOUND, 0U, "registry data pointer is not callback");

    Registry();
    gFailRead = NODE_B;
    Expect(KswCallbackGlobalRegistry, CALLBACK_A, STATUS_NOT_FOUND, 0U, "partial registry chain unavailable");

    Registry();
    gChangeRead = NODE_A;
    Expect(KswCallbackGlobalRegistry, CALLBACK_A, STATUS_NOT_FOUND, 0U, "registry identity changed between complete passes");

    Registry();
    gHead[0] = HEAD;
    gHead[1] = HEAD;
    Expect(KswCallbackGlobalRegistry, CALLBACK_A, STATUS_NOT_FOUND, 0U, "empty registry chain cannot calibrate");
    printf("callback_global_fallback: %lu checks, %lu failures\n", gChecks, gFailures);
    return gFailures != 0U;
}
