#pragma once

// DbgHelp 的进程级串行化锁。
//
// 微软明确写了 "All DbgHelp functions, such as this one, are single threaded."
// —— 这是**整个 DLL** 的约束，不是每个会话一把。所有 DbgHelp 用户必须共用此锁：
//
//   * ArkDriverClient/ArkRuntimeDynData.cpp —— 内核结构偏移的 PDB 解析
//   * ksword/process/injection_stack_walk.cpp —— 注入检查的跨进程栈展开
//   * ProcessDock/ThreadStackWindow.cpp —— 线程栈符号解析
//   * MinidumpDock/DumpSymbolResolver.cpp —— 离线转储符号解析
//   * MemoryDock/PoolTraceReader.cpp —— 离线 Pool 调用栈符号解析
//
// 两处各自持一把文件内的互斥量，等于没锁：它们可以同时进 DbgHelp。所以锁必须
// 落在这里，由两边共用。加锁范围要盖住 SymInitialize / SymCleanup / 选项设置和
// 中间的全部调用，不能只锁"那一次查询"。
//
// 只放一个函数局部静态量：不引入全局构造顺序问题，头文件即可，无需 .cpp。

#include <mutex>

namespace ks::dbghelp
{
    inline std::mutex& SerializationMutex()
    {
        static std::mutex mutex;
        return mutex;
    }
}
