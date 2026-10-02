#pragma once

#include "ark/ark_driver.h"

// 在已校验创建时间的同一 EPROCESS 上执行可选 PDB 私有终止例程。
NTSTATUS KswordARKDriverTerminateProcessPsp(_In_ PEPROCESS Process, _In_ NTSTATUS ExitStatus);

// 在精确对象所属线程上排入一个可追踪的 Normal Kernel APC。
NTSTATUS KswordARKDriverTerminateProcessViaApc(_In_ PEPROCESS Process, _In_ NTSTATUS ExitStatus);
