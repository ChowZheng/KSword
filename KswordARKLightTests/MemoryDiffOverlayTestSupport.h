#pragma once

// MemoryDiffOverlay 测试套件的两个 .cpp 共用的支撑：类型别名、常量与基线夹具。
//
// 套件按职责拆成两个文件（单文件不得超过 800 行）：
//   MemoryDiffOverlayTests.cpp           入口 + 载入 / 暂存 / 拒绝 / 溢出 / 合并 / 空操作 / 丢弃
//   MemoryDiffOverlayTests.Behavior.cpp  重读语义 / 上次读取 / 变化种类 / 写入确认 / 视图 / 暂存上限
// 两者共用同一个 KswordTests::Suite，所以只在套件名 "MEMWB overlay" 下报一次汇总。
//
// 夹具约定：标准基线窗口为 [0x1000, 0x1010)，地址 0x1000 + i 处的字节 = i * 0x11
// （00 11 22 ... FF）。测试里暂存的值一律取 0x01~0x0F 一类不会碰巧等于基线的数，
// 免得"等于基线则消失"的规则误伤断言。

#include "TestSupport.h"

#include "../shared/evidence/memory_workbench/MemoryDiffOverlay.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace MemwbOverlayTests {

using ksword::memwb::AcceptWriteStatus;
using ksword::memwb::BaselineLoadStatus;
using ksword::memwb::ByteChangeKind;
using ksword::memwb::DiffBlock;
using ksword::memwb::MemoryDiffOverlay;
using ksword::memwb::StageStatus;

// 字节序列、补丁块序列、可选字节的简写。
using Bytes = std::vector<std::uint8_t>;
using Blocks = std::vector<DiffBlock>;
using Eff = std::optional<std::uint8_t>;

// 标准窗口起点；窗口为 [0x1000, 0x1010)。
inline constexpr std::uint64_t kBase = 0x1000ULL;
// uint64 地址空间最大值。
inline constexpr std::uint64_t kTop = 0xFFFFFFFFFFFFFFFFULL;

// 把一个字节常量包成可选字节，便于与 EffectiveByte / BaselineByte 的返回值比较。
// 先显式截成 uint8_t 再构造，免得库模板里再做一次隐式窄化而触发 /W4 警告。
inline Eff Val(int value) {
    return Eff(static_cast<std::uint8_t>(value));
}

// 标准基线：第 i 个字节 = i * 0x11。
inline Bytes StandardBaseline() {
    Bytes data;
    for (int index = 0; index < 16; ++index) {
        data.push_back(static_cast<std::uint8_t>(index * 0x11));
    }
    return data;
}

// 载入了标准基线（身份 target-A、基址 0x1000、16 字节全部读到）的叠加层。
inline MemoryDiffOverlay MakeOverlay() {
    MemoryDiffOverlay overlay;
    overlay.LoadBaseline("target-A", kBase, StandardBaseline(), Bytes(16, 1));
    return overlay;
}

// 第二个文件里的测试组：重读语义、上次读取、变化种类、写入确认、视图、暂存上限。
// 入参：suite 共用的断言容器。
void RunOverlayBehaviorGroups(KswordTests::Suite& suite);

} // namespace MemwbOverlayTests
