# CE / x96dbg 虚拟机验收（2026-09-30）

## 环境与范围

实际执行位置为 `KSword-HVM-Target`：Windows 11 Pro 22621.4317、2 vCPU、
8 GiB、Hyper-V 嵌套虚拟化，控制台 Session 1。来宾启用测试签名，内部
Hyper-V/VBS 关闭。测试仅加载来宾驱动，宿主未加载驱动或启动 KSword HVM。

来宾测试目录为 `C:\ksword\debugger-tests\20260930`，使用独立 CE/x64dbg
载荷、会话和受控 `DebugTarget.exe`。没有附加无关应用。主程序、CE 和
x64dbg 的真实 GUI 均实际启动；原生全量回归通过 x64dbg headless 调试循环运行。
构建统一使用 AMD64 MSBuild/HostX64，并传入三项架构属性。

本机嵌套 VMX 能力不含 MTF（`0x01000000`），常规 HVM_DEBUG 执行断点
协议报告不可用。验证的自动回退是 **EPTP 切换的 ShadowPage 隐藏 INT3**：
执行视图包含 INT3，原页保留原指令，不要求 MTF；不是绕过能力检查启用常规路径。

## 最终测试产物身份

以下产物已逐文件传输并核对 SHA256，最终 GUI/原生回归使用同一新版代理和启动器。

| 产物 | SHA256 |
| --- | --- |
| `KswordARK.sys` | `2C0CA1A4A1E9F822DC7795F9C31D6B6737E3F02FE476D93B9A06B7DDCB0C4146` |
| CE `bridge/x64/KswordCheatEnginePlugin.dll` | `77531D845C71F25674CC6C018B882B20CA322E9AD09A6A43B065958EF0BC04E3` |
| `KSword/TitanEngine.dll` | `F414DC06512855F65DE50E5AEFB88182502E40B952BC1CCB9705920539D1AB83` |
| `KswordCheatEngineLauncher.exe` | `D733535D3A8FC04C5F3D9E61C07DDDFCD52444D5AB325C11C511109EB6445695` |
| `KswordX96dbgLauncher.exe` | `D0F9E62EC000D3CE59C5221F751A3D7FC7C5EF2A1FE568363B819D01FA5B49F3` |
| `DebugTarget.exe` | `1217AC4437C0295831427EA96A2D8C40D13B478BCDF0CFC51FE99541DD888FE9` |

驱动完整 WDK 构建、x64 ApiValidator（`Driver is 'Universal'.`）及 CAT
Signability 均通过；来宾加载的是对应测试签名驱动。主程序使用本轮已有的
Release 产物及实际 Qt/ADS 依赖，主程序源码没有为测试而修改。

## 实测矩阵

| 路径 | 结果 | 实际覆盖 |
| --- | --- | --- |
| CE R0 内存 | PASS | 打开目标、读写、分配、修改保护、释放；Qword 首次扫描及值变化后的再次扫描 |
| CE HVM 内存 | PASS | 严格私有窗口读写、首次扫描和再次扫描；显式切回 R0 |
| CE 原生硬件执行断点 | PASS | Windows 接口、准确 RIP、寄存器修改、单步一条指令、再次命中、移除、分离及目标退出 |
| CE HVM 硬件执行断点回退 | PASS | 不修改 CE 的硬件断点请求，自动 ShadowPage；读取仍为原字节；准确 RIP、修改 RAX、单步、两次命中及退休 |
| CE HVM 软件 INT3 | PASS | 显式 `bpmInt3`，自动隐藏执行视图；读取原字节、单步、两次命中、移除及清理 |
| CE 标题三态 | PASS | 驱动不可用 `KSword Connected`，默认 `KSword R0`，选择后 `KSword HVM`；保持 CE 的独立窗口与原界面 |
| CE 驱动不可用 | PASS | 不安装 15 个 hook，保留原生读写；桥接状态可查询，HVM 未被虚假启用 |
| Titan 原生实时循环 | PASS | 真实 attach/DebugLoop、硬件执行、准确线程/RIP、GPR/flags/x87/SSE/AVX、RAX 编辑、精确单步、两次命中和 detach |
| Titan HVM 自动回退 | PASS | 替换槽地址/回调、同页两断点合并及删除一个、隐藏字节、真实 Windows 停止、完整上下文、精确单步、重复命中、删除/分离/释放 |
| x64dbg 实际命令层 HVM | PASS | 前端 `attach` / `bphws` / `mov rax` / `sti` / `run` / `bphwc` / `detach`；前端表达式读原字节、准确 RIP/修改后的 RAX、两次命中及分离后目标存活 |
| Titan 模式保护 | PASS | 活动执行断点时禁止关闭 HVM；移除后关闭并释放自有常驻；通用 HVM 数据硬件断点明确返回错误 50 |
| x64dbg 完整原生回归 | **85/85 PASS，2 SKIP** | 全部 87 项发现的上游测试；软/硬件/内存断点、跨页/同页删除、多次命中、线程、脚本、跟踪、命令解析、生命周期等 |
| ShadowPage 崩溃清理 | PASS | owner 与 target 分别终止后，视图归零、两颗 vCPU 常驻归零，释放固定页资源；后续可再建会话 |
| 实际 PluginHost Tabs | PASS | CE/x96dbg 独立顶层 GUI、日志页嵌入、真实日志转发、Consolas、每个开关连续 `1→0→1→0` 并收到实际确认 |
| 日志页关闭 | PASS | 关闭两个实际日志 surface 后，两个调试器仍存活；随后才清理本轮客户端 |
| 公共 HVM 协议 | 16 个传输/布局检查通过 | 命令 16–28、30–32，响应长度与协议版本正确；没有把内部拒绝/未准备状态当成功操作 |

回归跳过的是 `replay_minidump` / `replay_ttd`，原因均为
`unsupported_engine_KSword`。普通硬件执行/写/读写及原生软件/内存断点都
包含在 85 项通过的原生测试中。命令解析测试单项通过 822 个断言。

Titan HVM 回退实时 fixture 通过 35 条断言，命中 2 次；原生实时 fixture
通过 26 条断言，命中 2 次。步骤使用私有代码页：3 字节 `mov` 后的
3 字节 `inc`，断点停在 `inc` 前，RAX 从 `0x100` 修改成 `0x1234`；
StepInto 后 RIP 前进 3 字节、RAX 为 `0x1235`，继续后目标存储该值。
再次命中后移除断点，目标正常完成并可退出。

## 测试中修复的问题

1. CE 的进程内 Lua/ABI 缓冲区误走目标驱动内存策略，造成错误 1306；
   本地自读写改用 Windows 原接口，目标读写仍走公共后端。
2. R0 写入缺少驱动现有协议要求的 FORCE 确认；与 UI_CONFIRMED 一并补齐。
3. 来宾 R0 上下文操作返回 `0xC0000001` / Win32 31，挂起计数导出缺失。
   后端明确记录后使用 Windows 上下文/挂起接口，保留计数及完整上下文。
   这不表示 R0 上下文实现本身通过；权限/身份失败不触发回退。
4. CE 隐藏执行 DR 的停止事件没有正确暴露命中 DR6；仅对实际持有且匹配
   的 ShadowPage 停止叠加槽位命中及 RIP，避免把历史事件当成停止。
5. Titan 硬件停止中的 StepInto 回调过早完成；通过真实事件代次等待下一
   单步。该延迟只能用于自有硬件停止，扩大到普通原生步进曾使 4 项回归
   超时；收紧后，4 项定点回归及最终全量 85 项均通过。
6. x96dbg 控制文件读取句柄跨越轮询等待，阻止第二次原子替换；公共 reader
   使用读/写/删除共享并立即关闭，两个 Tab 的连续切换实测通过。
7. Titan 单步完成时仍持有策略锁调用前端暂停回调；真实 x64dbg 回调等待 Run，
   而命令层上下文查询需要同一锁，造成卡住。完成私有记录后先释放策略锁，
   再调用前端回调；实际命令层全流程随后通过。

## 证据和复跑入口

生产测试源码位于 `tools/debugger_vm_test/`：

- `ce_live.lua`：真实 CE、扫描和重复执行断点；`KSWORD_TEST_USE_HVM=1`
  选择 HVM；`KSWORD_TEST_BREAKPOINT_METHOD=int3` 测软件路径，默认硬件请求。
- `ce_connected.lua`：桥接连接但驱动不可用的原生能力及标题。
- `titan_live.py ROOT native|hvm`：真实 TitanEngine 调试循环和上下文。
- `x64dbg_hvm_live.py ROOT`：真实 x64dbg 命令处理、HVM 自动回退及暂停时上下文查询。
- `shadow_live.py` / `shadow_cleanup.py`：底层执行视图和进程死亡清理。
- `gui_tabs.ps1` / `gui_close.ps1`：在来宾交互会话验证真实日志页和独立窗口。
- `protocol_probe.py`：公共布局/协议查询和 R0 上下文诊断。

主机实际证据存于 `.codex-build-logs/debugger-vm/`：
`native-suite-pass.log`、`titan-native-pass.log/json`、`titan-hvm-pass.log/json`、
`ce-native-scan-pass.log`、`ce-shadow-scan-pass.log`、`ce-shadow-software-pass.log`、
`ce-connected-pass.log`、`gui-tabs-pass.log`、`gui-close-pass.log`、
`shadow-cleanup-pass.log`、`protocol-probe-pass.log`，以及对应实际 GUI 截图。
实际命令层另有 `frontend-hvm-pass.log` / `frontend-hvm-commands-pass.log`。
来宾保留完整 `native-results/` 断言/目标产物。

此外通过公共后端模型、真实 Lua 启动脚本测试、64 个规范 ABI 声明/导出
检查，以及真实原生 DLL fixture（LastError、内存、上下文大小、文件会话
确认、缺失导出拒绝）。这些辅助测试与实时证据分别记录，不互相替代。

## 能力边界

- **原始 MTF/EPT 调试路由在此 VM 不支持**；通过的是无 MTF 的隐藏执行断点
  回退，不能写成原始路由通过。
- Titan HVM 通用写/读写硬件断点明确不支持；原生模式对应测试通过。
  显式 Watch 是页粒度诊断，不能冒充字节精确断点。CE 回退模式的数据 DR
  保留 Windows 行为，此次 CE 实时 fixture 只验收执行断点。
- 公共 HVM 接口全部接入并进行了 16 项传输/布局检查；注入、CR/MSR/域策略、
  嵌套页面等每个变更操作没有在本次调试器验收中逐一执行。命令 29 的嵌套
  探测未执行，不能把查询通路通过描述为所有变更功能通过。
- 目标映射替换/自修改代码要求重新安装逻辑断点；本轮实际目标使用私有代码页。
  这不是任意目标、多前端共享常驻或通用不可检测性的证明。
- 不包含 AMD 实机、x86/WOW64 HVM、CE VEH/DBVM、回放/逆向执行的实测。
- 关闭 KSword 主程序会执行其现有驱动关闭流程；“日志页关闭保持调试器”
  的验收专指日志页自身。测试后仅清理自己的客户端/任务，VM 保持运行。
