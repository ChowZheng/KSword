#pragma once

// 用途：生产指令解码器只处理本地字节与地址，不依赖任何私有内核对象布局。
// 内核构建只使用 WDK 类型，避免引入用户态 vcruntime 的标准整数头。
#if defined(_KERNEL_MODE)
#include <ntddk.h>
typedef UCHAR KSW_ACCESSOR_BYTE; // 一字节无符号代码单元。
typedef ULONG KSW_ACCESSOR_U32; // 四字节无符号指令位移编码。
typedef LONG KSW_ACCESSOR_OFFSET; // 四字节有符号对象字段偏移。
typedef ULONG_PTR KSW_ACCESSOR_ADDRESS; // 与内核指针同宽的无符号代码地址。
typedef SIZE_T KSW_ACCESSOR_SIZE; // 与内核指针同宽的字节数与缓冲区位置。
#else
// 离线回归使用标准类型；仍编译相同的生产算法，且不定义全局类型副本。
#include <stddef.h>
#include <stdint.h>
typedef uint8_t KSW_ACCESSOR_BYTE; // 一字节无符号代码单元。
typedef uint32_t KSW_ACCESSOR_U32; // 四字节无符号指令位移编码。
typedef int32_t KSW_ACCESSOR_OFFSET; // 四字节有符号对象字段偏移。
typedef uintptr_t KSW_ACCESSOR_ADDRESS; // 与宿主指针同宽的无符号代码地址。
typedef size_t KSW_ACCESSOR_SIZE; // 与宿主指针同宽的字节数与缓冲区位置。
#endif

// 显式使用解码器自身类型计算最大值，不依赖任一 CRT 的 SIZE_MAX/UINTPTR_MAX。
#define KSW_ACCESSOR_SIZE_MAX ((KSW_ACCESSOR_SIZE)-1) // 无符号大小类型的最大值。
#define KSW_ACCESSOR_ADDRESS_MAX ((KSW_ACCESSOR_ADDRESS)-1) // 无符号地址类型的最大值。

#ifdef __cplusplus
extern "C" {
#endif

// 用途：标记写入 ABI 返回寄存器且紧接返回指令的访问器操作种类。
typedef enum _KSWORD_ACCESSOR_LOAD_KIND
{
    KswordAccessorLoadPointer, // 将一个指针宽度字段加载到 RAX。
    KswordAccessorLoadUlong, // 将 ULONG 字段加载到 EAX 并零扩展。
    KswordAccessorLoadUshort, // 将 USHORT 字段零扩展到 EAX。
    KswordAccessorLoadUchar, // 将 UCHAR 字段零扩展到 EAX。
    KswordAccessorAddress // 将对象内字段的地址计算到 RAX。
} KSWORD_ACCESSOR_LOAD_KIND;

// 用途：承载完整访问器指令明确编码的偏移和读取类型，不推测相邻字段布局。
typedef struct _KSWORD_ACCESSOR_DISPLACEMENT
{
    KSW_ACCESSOR_OFFSET Offset; // 字段偏移；负值表示不可用。
    KSWORD_ACCESSOR_LOAD_KIND LoadKind; // 指令确定的读取宽度或地址计算操作。
} KSWORD_ACCESSOR_DISPLACEMENT;

// 用途：请求一个完整代码字节；提供者须验证地址仍在原始 PE 的可执行区段。
// 返回：完整读取并通过地址验证时返回非零，否则返回零。
typedef int (*KSWORD_ACCESSOR_READ_CODE_FN)(
    void* Context, // 调用者提供的映像边界或离线夹具上下文。
    KSW_ACCESSOR_ADDRESS Address, // 本次要安全读取的一个代码字节地址。
    KSW_ACCESSOR_BYTE* ByteOut); // 接收完整读取的字节，失败时不得发布数据。

// 用途：只识别 RCX 作为对象基址的完整加载或地址计算，且后面必须立即返回。
// 返回：接受时返回非零；拒绝或截断时返回零并清空旧结果。
int
KswordARKDecodeAccessorDisplacement(
    const KSW_ACCESSOR_BYTE* Bytes, // 已完整读取到本地缓冲区的代码字节。
    KSW_ACCESSOR_SIZE ByteCount, // 缓冲区实际可读字节数，不允许越界补读。
    KSWORD_ACCESSOR_DISPLACEMENT* DisplacementOut); // 接收唯一完整形态的解码结果。

// 用途：至多跟随两次直接相对入口跳转，并在完整返回指令处立即停止读取。
// 返回：成功解码返回非零；读取失败、跳转越界、循环或预算耗尽返回零。
int
KswordARKResolveAccessorDisplacement(
    KSWORD_ACCESSOR_READ_CODE_FN ReadCode, // 验证映像边界并安全读取单字节的回调。
    void* Context, // 原样传递给安全读取回调的上下文。
    KSW_ACCESSOR_ADDRESS RoutineAddress, // 已由导出表确认的访问器起始地址。
    KSWORD_ACCESSOR_DISPLACEMENT* DisplacementOut); // 接收结果，失败时偏移保持不可用。

#ifdef __cplusplus
}
#endif
