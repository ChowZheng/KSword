#pragma once

// 仅四个二维码驱动编译项使用此目录；普通用户态回放继续包含工具链原生 stdint.h。
// WDK km/crt 缺少 stdint.h，VC 对应头会引入与 WDK crtdefs.h 冲突的 vcruntime.h。
#include <stddef.h> // size_t 由已选定的 WDK 内核 CRT 提供。
#include <limits.h> // SIZE_MAX 和基础整数边界优先保留 WDK 原始定义。

typedef signed char int8_t; // MSVC 八位有符号整数。
typedef unsigned char uint8_t; // QR 矩阵和临时数组使用的八位字节。
typedef short int16_t; // MSVC 十六位有符号整数。
typedef unsigned short uint16_t; // MSVC 十六位无符号整数。
typedef int int32_t; // MSVC 三十二位有符号整数。
typedef unsigned int uint32_t; // MSVC 三十二位无符号整数。
typedef long long int64_t; // MSVC 六十四位有符号整数。
typedef unsigned long long uint64_t; // MSVC 六十四位无符号整数。

typedef int8_t int_least8_t; // 至少八位的有符号整数。
typedef uint8_t uint_least8_t; // 至少八位的无符号整数。
typedef int16_t int_least16_t; // 至少十六位的有符号整数。
typedef uint16_t uint_least16_t; // 至少十六位的无符号整数。
typedef int32_t int_least32_t; // 至少三十二位的有符号整数。
typedef uint32_t uint_least32_t; // 至少三十二位的无符号整数。
typedef int64_t int_least64_t; // 至少六十四位的有符号整数。
typedef uint64_t uint_least64_t; // 至少六十四位的无符号整数。
typedef int8_t int_fast8_t; // 与 MSVC stdint.h 相同的最快八位整数。
typedef uint8_t uint_fast8_t; // 与 MSVC 相同的最快无符号八位整数。
typedef int32_t int_fast16_t; // 至少十六位且使用本机三十二位运算。
typedef uint32_t uint_fast16_t; // 至少十六位的快速无符号整数。
typedef int32_t int_fast32_t; // 与 MSVC 相同的快速三十二位整数。
typedef uint32_t uint_fast32_t; // 快速无符号三十二位整数。
typedef int64_t int_fast64_t; // 快速六十四位有符号整数。
typedef uint64_t uint_fast64_t; // 快速六十四位无符号整数。
typedef int64_t intmax_t; // 标准整数族中可容纳最大有符号值的类型。
typedef uint64_t uintmax_t; // 标准整数族中可容纳最大无符号值的类型。

#ifndef _INTPTR_T_DEFINED
#define _INTPTR_T_DEFINED // 与 WDK CRT 的指针整数 guard 保持一致。
#ifdef _WIN64
typedef int64_t intptr_t; // 六十四位驱动中的有符号指针整数。
#else
typedef int32_t intptr_t; // 保留三十二位编译的标准宽度。
#endif
#endif
#ifndef _UINTPTR_T_DEFINED
#define _UINTPTR_T_DEFINED // 与 WDK CRT 的无符号指针整数 guard 保持一致。
#ifdef _WIN64
typedef uint64_t uintptr_t; // 六十四位驱动中的无符号指针整数。
#else
typedef uint32_t uintptr_t; // 保留三十二位编译的标准宽度。
#endif
#endif

// 常量字面量与 WDK ntintsafe.h 完全一致，已定义者保留，避免任何宏重定义告警。
#ifndef INT8_MIN
#define INT8_MIN (-127i8 - 1) // 八位有符号最小值。
#endif
#ifndef INT8_MAX
#define INT8_MAX 127i8 // 八位有符号最大值。
#endif
#ifndef UINT8_MAX
#define UINT8_MAX 0xffui8 // 八位无符号最大值。
#endif
#ifndef INT16_MIN
#define INT16_MIN (-32767i16 - 1) // 十六位有符号最小值。
#endif
#ifndef INT16_MAX
#define INT16_MAX 32767i16 // Nayuki 位缓冲边界使用的十六位有符号最大值。
#endif
#ifndef UINT16_MAX
#define UINT16_MAX 0xffffui16 // 十六位无符号最大值。
#endif
#ifndef INT32_MIN
#define INT32_MIN (-2147483647i32 - 1) // 三十二位有符号最小值。
#endif
#ifndef INT32_MAX
#define INT32_MAX 2147483647i32 // 三十二位有符号最大值。
#endif
#ifndef UINT32_MAX
#define UINT32_MAX 0xffffffffui32 // 三十二位无符号最大值。
#endif
#ifndef INT64_MIN
#define INT64_MIN (-9223372036854775807i64 - 1) // 六十四位有符号最小值。
#endif
#ifndef INT64_MAX
#define INT64_MAX 9223372036854775807i64 // 六十四位有符号最大值。
#endif
#ifndef UINT64_MAX
#define UINT64_MAX 0xffffffffffffffffui64 // 六十四位无符号最大值。
#endif
#ifndef SIZE_MAX
#define SIZE_MAX ((size_t)-1) // WDK limits.h 已提供时不重定义；缺失时使用正确 size_t 宽度。
#endif

#define INT8_C(Value) Value // 常量整数提升到 int，与标准表达式语义一致。
#define UINT8_C(Value) Value // 八位无符号常量同样适用整数提升。
#define INT16_C(Value) Value // 十六位常量按标准提升到 int。
#define UINT16_C(Value) Value // 十六位无符号常量按标准提升到 int。
#define INT32_C(Value) Value // 三十二位有符号常量无需额外后缀。
#define UINT32_C(Value) Value##U // 三十二位无符号常量使用 U 后缀。
#define INT64_C(Value) Value##LL // 六十四位有符号常量使用标准 LL 后缀。
#define UINT64_C(Value) Value##ULL // 六十四位无符号常量使用标准 ULL 后缀。
#define INTMAX_C(Value) Value##LL // 最大宽度有符号整数常量。
#define UINTMAX_C(Value) Value##ULL // 最大宽度无符号整数常量。
