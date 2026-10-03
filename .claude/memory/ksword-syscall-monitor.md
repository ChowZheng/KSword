# Syscall 监控与 SysWhispers 兼容形态

## 实现边界

- `MonitorDock/DirectKernelCallMonitorWidget.{h,cpp}`：独立 system logger，唯一 session GUID/name，只停止持有的会话；`TraceStackTracingInfo` 请求 PerfInfo opcode 51 栈。
- `shared/evidence/SyscallEvidence.h`：Qt/Win32-free、严格进入/退出/栈布局及 x64 桩判据；`SyscallCorrelation.h`：原始 QPC + CPU 精确关联，核对 StackWalk **载荷** PID/TID，可补未知事件头身份。
- 不再用字段名模糊匹配或 payload DWORD0 猜服务号。进入事件只有 `SysCallAddress` 内核服务入口；退出只有 NTSTATUS，成功状态 0 不是服务号 0。
- QPC 会话必须使用 `PROCESS_TRACE_MODE_RAW_TIMESTAMP`；否则头部被转换成 FILETIME、栈载荷仍为 QPC，关联全失败。FILETIME 仅用于显示及进程创建时间边界。
- 关联 250 ms 窗口、2048 事件、512 提前栈；乱序/重复/冲突/迟到与退役键均有离线测试。缺栈和容量淘汰不能读成常规调用。严格关联不覆盖全部延迟共享用户栈、CPU 迁移、压缩 stack-key。

## 易误判之处

- `ntdll.dll`/`win32u.dll` **名称或 PEB/Toolhelp 路径不是信任依据**；要求 MEM_IMAGE，实际映像映射路径匹配本机系统目录 DLL。身份未知不能等同已知非系统模块。
- NativeStub 仍不证明调用者安全。系统桩后的私有/映射可执行调用者可来自 JIT/运行时；没有兼容解析桩时展示动态代码来源，不能硬归因为间接规避。
- 静态服务号来自完整桩的 `mov eax,imm32`，不是执行时的 EAX。间接跳入任意系统桩会借用别的静态号，此时保持未知。
- SysWhispers 兼容形态要求完整参数保存/恢复、shadow space、hash/resolver 和 syscall/间接跳转骨架；单个 `0f05`、字符串或 RWX 不构成家族归因。SysWhispers2 也有间接模式，不能按直接/间接分版本。
- 尾跳转 wrapper 不留栈帧，可从 caller 的 `call rel32` 回溯目标；**只从目标入口验证完整形态**，不能扫描目标后的邻近函数。导出调用号映射也仅验证导出入口，避免 detour 残留桩污染。
- 远程采样只读、不挂起、不改页保护；按进程实例 + 地址缓存 250 ms，采样时复核句柄创建时间及 WOW64。结果是事件后的样本，不能证明那些字节执行过。

## 验证

- `tools/syscall_monitor/run-tests.ps1` 编译执行两个生产 header 的独立 C++ 回归，包含截断/未知布局/错误归因、乱序/碰撞/容量/会话重置；这些是离线测试，不代替 Qt/MSVC 构建或实机 ETW 验收。
- `tools/syscall_monitor/run-etw-probe.ps1` 是只触发自身标准 Windows 调用的独立实机探针；`CAPTURE_NOT_VERIFIED`（例如 StartTrace 访问拒绝）不是通过。停止失败保留自己的 session handle，消费者结束、析构或下次开始再试，不能丢掉所有权或停止别人的会话。
- 主程序仍使用 `tools/Invoke-KSwordBuildCheck.ps1` 的 HostX64 MSVC 门禁。机器缺 Qt/MSBuild 时报告构建未验证，不安装或替换工具链绕过。
- UI 新文本定点同步双语语言包并运行 i18n audit。

资料：[SysCallEnter](https://learn.microsoft.com/en-us/windows/win32/etw/syscallenter)、[SysCallExit](https://learn.microsoft.com/en-us/windows/win32/etw/syscallexit)、[StackWalk_Event](https://learn.microsoft.com/en-us/windows/win32/etw/stackwalk-event)、[SysWhispers2](https://github.com/jthuraisamy/SysWhispers2)、[SysWhispers3](https://github.com/klezVirus/SysWhispers3)。
