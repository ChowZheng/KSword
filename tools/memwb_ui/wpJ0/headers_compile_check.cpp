// ============================================================
// headers_compile_check.cpp
// 作用：
// - 只做"能否编译"的最低限度验证（见 docs/内存工作台Phase3装配接口.md）：把六个
//   新冻结的装配层头文件（WorkbenchShared/WorkbenchPageProvider/
//   WorkbenchBaselineFeeder/WorkbenchWriteController(.Undo)/WorkbenchHexPane/
//   MemoryWorkbenchView）及其直接依赖的既有头文件全部 #include 一遍，验证语法、
//   命名空间、依赖路径与声明本身自洽；不构造任何对象、不链接、不运行。
// - 本文件不是产品代码，不登记进任何 .vcxproj；只给
//   tools\memwb_ui\wpJ0\build-wpJ0-check.cmd 的 /W4 /WX 单次编译使用。
// ============================================================

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchShared.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchPageProvider.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchBaselineFeeder.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchWriteController.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchWriteController.Undo.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchHexPane.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/MemoryWorkbenchView.h"

// main：空实现。本文件只验证"能否编译"，不构造任何装配层对象（它们的构造需要
// 真实或假的工厂/服务实现，属于后续实现工作包的职责，不属于接口冻结阶段）。
int main()
{
    return 0;
}
