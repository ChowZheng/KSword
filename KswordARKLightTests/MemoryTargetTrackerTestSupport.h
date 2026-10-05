#pragma once

// MemoryTargetTracker 测试套件两个 .cpp 共用的声明。
//
// 套件按职责拆成两个文件（单文件不得超过 800 行），共用同一个 KswordTests::Suite，
// 只在套件名 "MEMWB target tracker" 下报一次汇总：
//   MemoryTargetTrackerTests.cpp        入口 + 手写场景（初始/附加分离/拒绝/范围/通道/钉住/回绕）
//   MemoryTargetTrackerTests.Walk.cpp   1 万步确定性随机游走 + 参考模型对拍 + 覆盖断言

#include "TestSupport.h"

// RunTrackerRandomWalk：跑随机游走测试，断言记到传入的 suite 上。
// 传入：suite 共用的断言容器。传出：无（失败记在 suite 里）。
void RunTrackerRandomWalk(KswordTests::Suite& suite);
