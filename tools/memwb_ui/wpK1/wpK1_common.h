#pragma once

// ============================================================
// wpK1_common.h
// 作用：WP-K1（MemoryDock 宿主侧生产实现）纯函数夹具的公共设施——轻量断言计数。
// 范围：
//   1) 三个"纯函数文件"（不含 Windows.h / Qt / Framework.h，不需要 QApplication）：
//        Ksword5.1/Ksword5.1/MemoryDock/WorkbenchServicesMapping.*   模块映射/指针读取/徽章/汇编判据/路径
//        Ksword5.1/Ksword5.1/MemoryDock/WorkbenchServicesAudit.*     审计分级/格式化/链路登记表
//        Ksword5.1/Ksword5.1/MemoryDock/WorkbenchServicesProbe.*     HVM 探测状态机/句柄缓存/候选刷新判据
//   2) 生产审计接收器 MemoryDock.WorkbenchServices.Audit.cpp：它只依赖项目日志框架
//      （ksword/log/log.cpp，只用 Windows + 标准库），所以可以链接真实日志实现、在夹具里
//      从全局日志仓库读回去断言（wpK1_tests.AuditSink.cpp）。
//   其余真正"问系统"的宿主文件（MemoryDock.WorkbenchServices.cpp / .Impl / .Gate / .Disasm）依赖
//   真实 Qt、驱动客户端与既有 UI 组件，只能做语法检查（build-wpK1-check.cmd），不在本夹具内。
// 汇总行：wpK1_main.cpp 最后输出恰好一行 "wpK1_tests: N checks, M failures"。
// ============================================================

#include <string>

namespace wpK1_test
{
    // g_checks / g_failures：断言总数与失败数（单线程测试体里累加；并发测试体内只在汇合
    // 之后由主线程断言，不在工作线程里调用 Report）。
    extern int g_checks;
    extern int g_failures;

    // Report：记录一次断言。
    // 传入：ok 是否通过；expression 表达式文本；file/line 位置；note 额外说明（可为空）。
    void Report(bool ok, const char* expression, const char* file, int line, const std::string& note);

#define WPK1_CHECK(expression) \
    ::wpK1_test::Report(static_cast<bool>(expression), #expression, __FILE__, __LINE__, std::string())
#define WPK1_CHECK_NOTE(expression, note) \
    ::wpK1_test::Report(static_cast<bool>(expression), #expression, __FILE__, __LINE__, (note))

    // 各测试分组的入口（定义在 wpK1_tests.*.cpp）。
    void RunMappingModuleTests();
    void RunMappingPointerTests();
    void RunMappingMiscTests();
    void RunAuditClassifyTests();
    void RunAuditFormatTests();
    void RunAuditRegistryTests();
    void RunAuditSinkTests();
    void RunProbeTrackerTests();
    void RunProbeDecisionTests();
}
