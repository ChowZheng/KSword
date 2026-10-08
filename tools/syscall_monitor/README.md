# Syscall 检测回归

从仓库根目录执行：

```powershell
& tools/syscall_monitor/run-tests.ps1
& tools/syscall_monitor/run-etw-probe.ps1
```

第一个入口编译运行生产判据和关联 header 的离线 C++ 测试，覆盖静态编号、完整桩/兼容 wrapper、未知模块身份、残缺载荷，以及栈乱序、身份冲突、碰撞、容量、过期、退役键和重置。测试字节数组是惰性数据，不作为代码执行。

关联队列满时拒绝新事件并计入容量淘汰，保留已接纳事件的完整 250 ms 关联窗口。回归包含每秒 10 万条连续事件，确认满载仍能发布精确栈匹配，并检查被拒绝的同键事件仍触发冲突、提前栈被清理、各缓存保持有界。全局采集显示未能确定 PID 的已接纳原始进入事件，标记证据不足；按 PID 采集仍只发布身份匹配的事件。容量拒绝的事件仅计数，不进入 UI 队列。

第二个入口是独立 Windows ETW 验收探针，仅调用自己的普通 Windows 导出并创建/停止自己唯一的 system logger。它验证回调 ABI、CPU 索引布局，以及 QPC/StackWalk 精确关联；不修改权限或打开其他进程。需要只编译时传 `-BuildOnly`。

`SYSCALL_ETW_PROBE=PASS` 要求取得本探针 PID 的进入事件和精确关联的用户栈。权限拒绝、只编译、缺栈或捕获失败均输出 `CAPTURE_NOT_VERIFIED`，不能当作实机通过。探针不替代主程序 Qt/MSVC 构建、GUI 验收或完整系统矩阵。

主程序验收仍使用 `tools/Invoke-KSwordBuildCheck.ps1`。测试 g++ 不用于替换主程序的 HostX64 MSVC 工具链。

已知范围：原生 x64 桩形态；250 ms 有界精确关联。延迟共享用户栈、跨 CPU 分段、压缩 stack-key、WOW64、egg-hunter 与未匹配 wrapper 变种保留证据不足。静态桩编号不是执行时的 EAX，兼容形态不能归因到特定工具版本或恶意行为。
