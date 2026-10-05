#pragma once

// memwb_ui_tests.IoMapping.h
// 作用：声明 RunIoMappingTests，供 memwb_ui_tests.cpp 的 main() 调用。
// 单独起一个头文件（而不是加进 memwb_ui_common.h），是因为这套测试只覆盖
// M-1 拆出来的三个纯映射函数（WorkbenchIoMapping.h），不依赖 memwb_ui_common.h
// 里那一整套画布夹具/鼠标键盘辅助，没必要把声明混进那个文件。

namespace memwb_test
{
    // RunIoMappingTests：覆盖 ksword::memwb_ports_detail 的三个纯映射函数
    // （MapFacadeReadOutcome / MapFacadeWriteOutcome / MapStandardDriverVirtualRead）
    // 的每一个分支。调用方法：在 main() 里直接调用，无参数、无返回值，断言
    // 结果记进 memwb_test::g_checks / g_failures（与本夹具其余套件共用同一套
    // 计数器）。
    void RunIoMappingTests();
}
