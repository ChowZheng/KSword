# 调试器共享后端与 x64dbg 适配

- CE 与 x64dbg 共用 `DebuggerBackend/`；R0 协议仍只定义在 `shared/driver/`，设备访问仍通过 `ArkDriverClient/`。
- `TitanEnginePlugin/` 的规范 ABI 是 x64dbg `f107330b6563da3c38d60a3ad6e629057b7cf5d0` 的 64 个导出，原生 TitanEngine 固定 `21ef77f31fd42d17f785802c36b2ca4ca44c43d7`。源头、检查器及声明见 `third_party/x64dbg_abi/`。
- 代理放在运行目录 `KSword/TitanEngine.dll`，原生依赖保留在上一级 `TitanEngine.dll`。两者均校验 AMD64/导出并固定进程生命周期；初始化不放在 DllMain。保留原生成功/失败 LastError。
- x64dbg 核心只增加 `DebugEngineKSword=4`，沿现有 checked loader 加载；补丁及基线在 `X96dbgIntegration/`。启动器使用真实 `-userdir` 和每会话 INI，不能发明 GUI 不接受的 `--engine` 参数。
- `X96dbgExecutablePlugin/` 的 ID 是 `x96dbg`，仅发行 x64dbg AMD64。KSword Tab 为配色/等宽控制日志页，调试器保持独立窗口。页面关闭不应终止调试器或目标。
- 默认原生模式不依赖驱动。HVM 控制使用 SESSION/REVISION 文件请求和实际状态确认；活动目标必须持有真实 Windows 调试事件且清空已有硬件绑定后，才允许切换模式。
- `DebuggerBackend` 原生会话 API 只借用前端会话，不能二次 attach，也不能抢 Wait/Continue。保留进程/线程句柄身份及代次，防止 PID/TID 重用和旧回调。
- HVM 硬件执行断点通过驱动精确线程/RIP 筛选和 guest #DB，交给原生 Windows `EXCEPTION_SINGLE_STEP` / Titan DebugLoop。历史 ring 事件不能伪装成当前停止，也不能由轮询线程调用前端断点回调。
- 代理仅改原生 DLL 内六个 IAT 槽：GetThreadContext、SetThreadContext、WaitForDebugEvent、ContinueDebugEvent、ReadProcessMemory、WriteProcessMemory。完整 GPR/SIMD/XSTATE 留给 Windows；内部断点读取见逻辑 INT3，公开内存读取见原字节。常规 EPT 保留 Windows DR6；CE ShadowPage 停止只在匹配的真实持有事件中叠加命中槽/RIP。DR7 先于地址的短序列必须暂存，未完成或失败时不能继续。
- Titan HVM 下通用硬件写/读写断点返回 ERROR_NOT_SUPPORTED，不能将 4 KiB EPT 数据页语义冒充字节范围。软件断点保留原生循环并支持隐藏 INT3，通用内存断点仍原生；显式 Watch 和完整 HVM 命令通过版本化 KSwordDebuggerCall。
- 常规 HVM 执行断点不可用时，公共后端自动尝试 EPTP 切换的 ShadowPage 隐藏 INT3（不要求 MTF）。原生协议操作 8/9 管理目标页 MDL、owner/target 进程引用、同页偏移合并及进程退出清理。不能接管外部准备/常驻；目标映射更换仍要重装。
- CE 标题是 KSword HVM / KSword R0 / KSword Connected；Connected 表示桥接已连接但驱动不可用，保留 CE 原函数表。VM 中 R0 上下文错误 31、挂起计数导出缺失会明确记录并用 Windows 接口，策略/身份拒绝不回退。进程内 Lua 缓冲区自读写不得送往目标驱动内存接口。
- 控制/确认文件读取必须允许 FILE_SHARE_READ/WRITE/DELETE，并在解析/等待前关闭句柄。否则第二次原子替换会因共享冲突失败。普通原生 StepInto/StepOver 不能套用硬件停止的延迟回调；只对真实匹配的自有硬件停止采用事件代次保护。
- 单步完成先在策略锁内退休私有回调记录，再释放锁调用前端回调。真实 x64dbg 暂停回调会等 Run；持锁调用会使另一线程的上下文查询死锁。实际命令层 HVM attach/bphws/mov/sti/run/bphwc/detach 已验证，DLL 回调 fixture 不能替代这个检查。
- 清理仅处理自动适配记录的私有 ID；外部已准备/常驻状态会阻止自动准备。原生出口、线程退出、模块卸载及内存释放需要清理。Decommit 按 Windows 实际页舍入处理，并检查地址溢出；模块通过 PID/AllocationBase 匹配。原始规则由调用者负责，不能混入自动持有的准备。
- 构建脚本 `tools/Build-X96dbgPayload.ps1` 必须使 CMake 探测进程也使用 AMD64 环境及 64 位 MSBuild；仅 `-T host=x64` 在本机可能仍探测 HostX86。MSBuild 始终传三项架构属性并读回 HostX64。
- `tools/export_x96dbg_source.py` 导出可构建源码及固定上游源码 ZIP，`tools/package_x96dbg_plugin.ps1` 校验 64 位 PE、64 导出与实际 bridge 标记，保留源代码/原始许可证并生成哈希清单。插件 ZIP 根直接为 plugin.json，解压到 plugin/x96dbg。
- 2026-09-30 后续在 KSword-HVM-Target（Win11 22621.4317、2 vCPU）加载匹配驱动，真实 GUI、CE R0/HVM 扫描、三态标题、CE 硬件/软件隐藏 INT3、Titan 完整上下文修改/单步/重复命中、ShadowPage owner/target 死亡清理均通过。最终全量原生回归 85/85 PASS、2 回放 SKIP；实际两个 Tab 连续开关与关闭保持独立调试器也通过。此 VM 缺 MTF，原始 MTF/EPT #DB 路由不支持；已验收自动 ShadowPage 执行回退。16 项 HVM 通路/布局检查不代表所有变更操作实测。准确产物哈希和边界见 `docs/ksword-debugger-vm-validation.md`。
