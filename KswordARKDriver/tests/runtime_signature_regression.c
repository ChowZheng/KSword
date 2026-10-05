// 中文说明：使用合成 x64 PE 和生产扫描器验证函数边界、跳板与安全读取，不访问驱动设备。
#include <ntifs.h>
#include <ntimage.h>
#include <stdio.h>
#include <string.h>
#include "runtime_signature_scan.h"

static UCHAR testImage[0x6000]; // 合成映像承载代码、数据节和异常目录。
static ULONG_PTR testBase = 0xFFFF800000100000ULL; // 高地址由模拟读取映射到本地数组。
static ULONG testFailures; // 汇总断言失败次数。
static KIRQL testIrql = PASSIVE_LEVEL; // 注入 IRQL 拒绝场景。
static BOOLEAN testPartialRead; // 注入短读取，不能误报完整复制成功。

// 中文说明：仅复制合成映像内的完整区间；输出 NTSTATUS 和真实复制长度。
static NTSTATUS TestCopy(VOID* Buffer, MM_COPY_ADDRESS Source, SIZE_T Size,
    ULONG Flags, SIZE_T* Copied)
{
    ULONG_PTR address = (ULONG_PTR)Source.VirtualAddress; // 待验证的虚拟地址。
    UNREFERENCED_PARAMETER(Flags); // 测试仅模拟虚拟内存复制。
    *Copied = 0U; // 失败路径保留零长度。
    if (address < testBase || address - testBase >= sizeof(testImage) ||
        Size > sizeof(testImage) - (SIZE_T)(address - testBase)) {
        return STATUS_PARTIAL_COPY; // 越界不可访问。
    }
    memcpy(Buffer, testImage + address - testBase, Size); // 只访问本地映像。
    *Copied = testPartialRead ? Size - 1U : Size; // 模拟成功但复制不足的 API 返回。
    return STATUS_SUCCESS; // 生产安全读取还须独立检查 Copied。
}

// 中文说明：五组公开锚点统一映射到合成入口，避免借助 PDB 或配置指定全局。
static PVOID TestExport(PVOID Base, PCCH Name)
{
    static const char* const names[] = { // 每一项对应本轮接入的实际导出锚点。
        "ObGetObjectType", "ExQueueWorkItem", "CiInitialize",
        "KeAddSystemServiceTable", "PsSetCreateProcessNotifyRoutine"
    };
    SIZE_T index; // 遍历受限锚点表。
    UNREFERENCED_PARAMETER(Base); // 地址来自本轮 testBase。
    for (index = 0U; index < RTL_NUMBER_OF(names); ++index) {
        if (strcmp(Name, names[index]) == 0) {
            return (PVOID)(testBase + 0x1100UL); // 只返回代码入口，不返回数据候选。
        }
    }
    return NULL; // 未知导出不能参与定位。
}

static KIRQL TestIrql(VOID) { return testIrql; } // 中文说明：生产 IRQL 门禁的测试 oracle。
#undef KeGetCurrentIrql
#define KeGetCurrentIrql TestIrql
#define MmCopyMemory TestCopy
#define RtlFindExportedRoutineByName TestExport
#include "../../output/runtime_signature_regression_source.c"

// 中文说明：输出断言结果，输入独立预期；不把预期数据地址交给扫描器。
static VOID Expect(const char* Name, BOOLEAN Passed)
{
    printf("%s %s\n", Passed ? "PASS" : "FAIL", Name); // 便于自动检查失败项。
    if (!Passed) {
        ++testFailures; // 保持最终非零退出。
    }
}

// 中文说明：在合成代码中写入 RIP 相对 LEA 或直接分支。
static VOID WriteRelative(ULONG Rva, UCHAR Opcode, ULONG Target)
{
    ULONG displacementOffset = Opcode == 0x8DU ? 3UL : 1UL; // LEA 的 ModRM 后跟 disp32。
    ULONG bytes = displacementOffset + sizeof(LONG); // 下一指令地址用于相对目标计算。
    LONG displacement = (LONG)Target - (LONG)(Rva + bytes); // 范围仅在合成 PE 内。
    if (Opcode == 0x8DU) {
        testImage[Rva] = 0x48U; // REX.W：64 位 LEA。
        testImage[Rva + 1UL] = Opcode; // LEA opcode。
        testImage[Rva + 2UL] = 0x05U; // RAX, [RIP + disp32]。
    }
    else {
        testImage[Rva] = Opcode; // E8/E9 直接分支。
    }
    memcpy(testImage + Rva + displacementOffset, &displacement, sizeof(displacement)); // 写本地位移。
}

// 中文说明：创建两个真实函数与无 unwind 的短跳板，邻接区故意放置诱饵引用。
static VOID ResetImage(VOID)
{
    IMAGE_DOS_HEADER* dos; // 合成 DOS 头。
    IMAGE_NT_HEADERS64* nt; // 合成 x64 NT 头。
    IMAGE_SECTION_HEADER* sections; // 三个互不重叠的节。
    IMAGE_RUNTIME_FUNCTION_ENTRY* functions; // PE 自带函数表。
    ULONG index; // 初始化节布局。
    memset(testImage, 0, sizeof(testImage)); // 隔离每个用例的映像修改。
    testIrql = PASSIVE_LEVEL; // 默认允许安全读取。
    testPartialRead = FALSE; // 默认要求完整复制。
    dos = (IMAGE_DOS_HEADER*)testImage; // 头位于本地映像起始处。
    dos->e_magic = IMAGE_DOS_SIGNATURE; // MZ。
    dos->e_lfanew = 0x80L; // NT 头固定在合成映像内。
    nt = (IMAGE_NT_HEADERS64*)(testImage + 0x80); // 安全的本地头。
    nt->Signature = IMAGE_NT_SIGNATURE; // PE。
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64; // 测试 x64 扫描路径。
    nt->FileHeader.NumberOfSections = 3U; // text/data/pdata。
    nt->FileHeader.SizeOfOptionalHeader = sizeof(nt->OptionalHeader); // 确定节表位置。
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC; // PE32+。
    nt->OptionalHeader.SizeOfImage = sizeof(testImage); // 已映射映像大小。
    nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES; // 启用异常目录。
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].VirtualAddress = 0x4000UL; // pdata。
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size = 2UL * sizeof(*functions); // 两项。
    sections = IMAGE_FIRST_SECTION(nt); // 生产代码也按该位置读取节表。
    for (index = 0UL; index < 3UL; ++index) {
        sections[index].VirtualAddress = index == 0UL ? 0x1000UL : 0x2000UL + index * 0x1000UL; // 独立范围。
        sections[index].Misc.VirtualSize = 0x1000UL; // 节均为 4 KiB。
        sections[index].Characteristics = IMAGE_SCN_MEM_READ | // 每节可读。
            (index == 0UL ? IMAGE_SCN_MEM_EXECUTE : IMAGE_SCN_MEM_WRITE); // 仅 text 可执行。
    }
    functions = (IMAGE_RUNTIME_FUNCTION_ENTRY*)(testImage + 0x4000); // 排序函数表。
    functions[0].BeginAddress = 0x1100UL; // 导出实现的起点。
    functions[0].EndAddress = 0x1120UL; // 邻接诱饵不属于该函数。
    functions[1].BeginAddress = 0x1300UL; // 深层实现的起点。
    functions[1].EndAddress = 0x1320UL; // 深层实现的边界。
    WriteRelative(0x1100UL, 0x8DU, 0x3030UL); // 导出函数内合法数据引用。
    WriteRelative(0x1108UL, 0xE8U, 0x1200UL); // 调用无 unwind 跳板。
    WriteRelative(0x1140UL, 0x8DU, 0x3050UL); // 相邻函数诱饵。
    testImage[0x1200] = 0xEBU; // 无 unwind 的 rel8 跳板。
    testImage[0x1201] = 0x1EU; // 跳到 0x1220。
    WriteRelative(0x1220UL, 0xE9U, 0x1300UL); // 第二级 rel32 跳板。
    WriteRelative(0x1300UL, 0x8DU, 0x3040UL); // 深层数据引用。
}

// 中文说明：检查结果中的虚拟地址，RVA 仅用于独立断言。
static BOOLEAN Contains(const KSW_RUNTIME_DATA_REFERENCE* References, ULONG Count, ULONG Rva)
{
    ULONG index; // 遍历实际输出的引用。
    for (index = 0UL; index < Count; ++index) {
        if (References[index].Address == testBase + Rva) {
            return TRUE; // 命中预期数据地址。
        }
    }
    return FALSE; // 未发现。
}

// 中文说明：驱动扫描接口执行在合成内存中，验证迁移后的共同边界和分支支持。
int main(VOID)
{
    static PCSTR const anchors[] = { "ObGetObjectType", "ExQueueWorkItem", "CiInitialize", // 实际导出名称。
        "KeAddSystemServiceTable", "PsSetCreateProcessNotifyRoutine" }; // 五组消费者。
    KSW_RUNTIME_IMAGE_VIEW view; // 生产 PE 节视图。
    KSW_RUNTIME_DATA_REFERENCE references[64]; // 小型受限结果缓冲区。
    IMAGE_NT_HEADERS64* nt; // 故障注入仅修改本地合成头。
    ULONG count; // 扫描器返回的实际数量。
    ULONG index; // 逐家族锚点验证。
    UCHAR probe; // 安全读取返回值验证。
    ResetImage(); // 创建默认夹具。
    Expect("synthetic PE accepted", KswordARKRuntimeInitializeImageView((PVOID)testBase, sizeof(testImage), &view));
    count = KswordARKRuntimeCollectAnchoredDataReferences(&view, anchors, 1UL, 4UL, 0x800UL, references, RTL_NUMBER_OF(references));
    Expect("legacy fixed window includes neighbor decoy", Contains(references, count, 0x3050UL));
    for (index = 0UL; index < RTL_NUMBER_OF(anchors); ++index) {
        count = KswordARKRuntimeCollectFunctionDataReferences(&view, &anchors[index], 1UL, 4UL, 0x800UL,
            references, RTL_NUMBER_OF(references)); // 无 PDB 的函数表扫描。
        Expect(anchors[index], Contains(references, count, 0x3030UL) && Contains(references, count, 0x3040UL) &&
            !Contains(references, count, 0x3050UL)); // 深层跳板可达，相邻诱饵不能污染结果。
    }
    count = KswordARKRuntimeCollectFunctionDataReferences(&view, anchors, 1UL, 0UL, 0x800UL, references, RTL_NUMBER_OF(references));
    Expect("depth zero does not follow thunk", !Contains(references, count, 0x3040UL));
    WriteRelative(0x111DUL, 0x8DU, 0x3060UL); // 完整指令跨越 0x1120 边界。
    count = KswordARKRuntimeCollectFunctionDataReferences(&view, anchors, 1UL, 4UL, 0x800UL, references, RTL_NUMBER_OF(references));
    Expect("cross-boundary RIP instruction rejected", !Contains(references, count, 0x3060UL));
    ResetImage(); // 清除前一个故障场景。
    memset(testImage + 0x1108, 0x90, 5U); // 去掉正常调用，让深层函数只能由跨界分支抵达。
    WriteRelative(0x111DUL, 0xE9U, 0x1300UL); // 分支跨越函数边界，目标有有效 unwind 条目。
    count = KswordARKRuntimeCollectFunctionDataReferences(&view, anchors, 1UL, 4UL, 0x800UL, references, RTL_NUMBER_OF(references));
    Expect("cross-boundary branch rejected", !Contains(references, count, 0x3040UL));
    ResetImage(); // 导出本身也可以是无 unwind 的跳板。
    ((IMAGE_RUNTIME_FUNCTION_ENTRY*)(testImage + 0x4000))[0].BeginAddress = 0x1120UL; // 移除导出地址的函数条目。
    ((IMAGE_RUNTIME_FUNCTION_ENTRY*)(testImage + 0x4000))[0].EndAddress = 0x1140UL; // 保持函数表条目有效。
    WriteRelative(0x1100UL, 0xE9U, 0x1300UL); // 入口处明确无条件跳转到有 unwind 实现。
    count = KswordARKRuntimeCollectFunctionDataReferences(&view, anchors, 1UL, 4UL, 0x800UL, references, RTL_NUMBER_OF(references));
    Expect("export thunk without unwind accepted", Contains(references, count, 0x3040UL) && !Contains(references, count, 0x3050UL));
    testImage[0x1100] = 0xE8U; // 无 unwind 的 call 不是完整跳板语义。
    count = KswordARKRuntimeCollectFunctionDataReferences(&view, anchors, 1UL, 4UL, 0x800UL, references, RTL_NUMBER_OF(references));
    Expect("unknown leaf call is not a thunk", count == 0UL);
    ResetImage(); // 有 unwind 的函数也可能短尾跳到独立实现。
    memset(testImage + 0x1108, 0x90, 5U); // 移除原来的正常调用。
    ((IMAGE_RUNTIME_FUNCTION_ENTRY*)(testImage + 0x4000))[1].BeginAddress = 0x1120UL; // 邻接实现拥有独立条目。
    ((IMAGE_RUNTIME_FUNCTION_ENTRY*)(testImage + 0x4000))[1].EndAddress = 0x1140UL; // 包含完整目标引用。
    testImage[0x1118] = 0xEBU; // 函数内部明确短尾跳。
    testImage[0x1119] = 0x06U; // 从 0x111A 跳到下一个函数 0x1120。
    WriteRelative(0x1120UL, 0x8DU, 0x3040UL); // 独立实现的数据引用。
    count = KswordARKRuntimeCollectFunctionDataReferences(&view, anchors, 1UL, 4UL, 0x800UL, references, RTL_NUMBER_OF(references));
    Expect("unwound function short tail jump", Contains(references, count, 0x3040UL) && !Contains(references, count, 0x3050UL));
    ResetImage(); // 修改跳板为两字节向后跳转。
    testImage[0x1201] = 0xDEU; // 从 0x1202 向后跳至 0x11E0。
    WriteRelative(0x11E0UL, 0xE9U, 0x1300UL); // 后向 rel8 之后仍为明确 rel32 跳板。
    count = KswordARKRuntimeCollectFunctionDataReferences(&view, anchors, 1UL, 4UL, 0x800UL, references, RTL_NUMBER_OF(references));
    Expect("negative rel8 sign extension", Contains(references, count, 0x3040UL));
    ResetImage(); // 验证调用链循环不失控。
    testImage[0x1201] = 0xFEU; // 跳板自循环。
    count = KswordARKRuntimeCollectFunctionDataReferences(&view, anchors, 1UL, 8UL, 0x800UL, references, RTL_NUMBER_OF(references));
    Expect("cyclic thunk remains bounded", Contains(references, count, 0x3030UL) && !Contains(references, count, 0x3040UL));
    ResetImage(); // 验证缺失和损坏函数表。
    nt = (IMAGE_NT_HEADERS64*)(testImage + 0x80); // 已知本地头地址。
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size = 0UL; // 没有函数证据。
    count = KswordARKRuntimeCollectFunctionDataReferences(&view, anchors, 1UL, 4UL, 0x800UL, references, RTL_NUMBER_OF(references));
    Expect("missing pdata rejected", count == 0UL);
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size = MAXULONG; // 映像外目录。
    count = KswordARKRuntimeCollectFunctionDataReferences(&view, anchors, 1UL, 4UL, 0x800UL, references, RTL_NUMBER_OF(references));
    Expect("out-of-image pdata rejected", count == 0UL);
    ResetImage(); // 执行相同扫描的第二个映像基址。
    testBase += 0x10000000ULL; // 改变虚拟装载基址而不修改代码位移。
    Expect("relocated PE accepted", KswordARKRuntimeInitializeImageView((PVOID)testBase, sizeof(testImage), &view));
    count = KswordARKRuntimeCollectFunctionDataReferences(&view, anchors, 1UL, 4UL, 0x800UL, references, RTL_NUMBER_OF(references));
    Expect("relocated data references", Contains(references, count, 0x3030UL) && Contains(references, count, 0x3040UL));
    testIrql = DISPATCH_LEVEL; // 候选读取不允许在高 IRQL 降级为直接访问。
    Expect("high IRQL read rejected", !KswordARKRuntimeReadMemory((PVOID)testBase, &probe, sizeof(probe)));
    testIrql = PASSIVE_LEVEL; // 短复制使用合法 IRQL。
    testPartialRead = TRUE; // API 成功状态不能代替完整复制。
    Expect("partial copy rejected", !KswordARKRuntimeReadMemory((PVOID)testBase, &probe, sizeof(probe)));
    printf("failures=%lu\n", testFailures); // 汇总生产接口回归结果。
    return testFailures != 0UL; // 非零代表至少一项失败。
}
