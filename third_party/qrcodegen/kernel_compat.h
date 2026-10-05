#pragma once

// 先引入声明，避免下方名字替换改变标准头文件里的函数原型。
#include <stdlib.h>

// 上游 abs 只接收 -4 至 4 的二维码坐标，不存在最小整数取负。
static __inline int
KswordQrKernelAbs(int Value) // 保存有界的有符号坐标。
{
    return Value < 0 ? -Value : Value; // 直接内联整数绝对值，避免用户 CRT 导入。
}

// 上游 labs 的掩码评分表达式绝对值小于 177*177*20，适合 LONG。
static __inline long
KswordQrKernelLabs(long Value) // 保存有界的二维码掩码评分。
{
    return Value < 0 ? -Value : Value; // 直接内联长整数绝对值，避免用户 CRT 导入。
}

#define abs KswordQrKernelAbs // 只在 qrcodegen.c 强制包含后替换上游绝对值调用。
#define labs KswordQrKernelLabs // 只替换掩码评分使用的长整数绝对值调用。
