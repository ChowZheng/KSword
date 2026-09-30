# Cheat Engine KSword 插件

插件只启动 64 位 CE 7.6。CE 保留自己的主窗口、颜色、字体、菜单和布局，
仅在标题末尾显示 `[KSword R0]` 或 `[KSword HVM]`，确认公共后端已经加载。
关闭 KSword 日志 Tab 不会关闭 CE。

KSword 内嵌的是日志页，使用宿主主题 token 和 Consolas 等宽字体。页面包含
“使用 HVM”开关，实际状态由 CE 后端确认；切换失败会显示错误码并保持原状态。
启动、桥接、调试异常和生命周期日志从 CE 转发至该页面与 PluginHost。
这不是 CE 全部内部输出的捕获器。单行、显示长度和后端日志文件均有上限。

## 后端能力

- 进程打开、虚拟内存查询和读写由 KSword 管理；设备句柄在会话内复用。
- 线程上下文、挂起/恢复、远程分配和保护修改使用 KSword R0 调试协议。
- 选择 HVM 后，内存读写必须经过 HVM 私有窗口；窗口不可用时明确失败。
- CE 的 Windows 调试接口设置 DR0–DR3 时，公共后端把断点转换为 HVM EPT
  执行/写入/读写断点，再以 `EXCEPTION_SINGLE_STEP` 交给 CE。
- CE Lua 的 `KSword.hvm` 暴露当前全部 HVM 协议，包括生命周期、EPT 规则、
  事件、视图、CR/MSR 策略、域、进程、注入、平台、指标和嵌套探测/页面。

执行断点按指令地址匹配；数据断点目前是 **4 KiB 页粒度**，同页其他访问也可能
命中。自动 EPT 调试要求 Intel VMX、MTF 和匹配的新驱动；不是 CE 的 VEH/DBVM
调试器适配。现有驱动未支持的平台或操作仍会返回其能力/错误状态。
Windows 继续提供调试事件传递与远程线程创建。

公共后端、协议、资源所有权和 API 示例见仓库 `DebuggerBackend/README.md`；
插件包内对应 `DEBUGGER_BACKEND.md`。

## 使用

1. 加载本次构建的 `KswordARK.sys`，新协议不兼容旧驱动。
2. 从 KSword 插件 Tab 启动，或从进程菜单打开所选 PID。
3. 标题出现 KSword 标识后可使用后端；默认选择 R0。
4. 如需 HVM，在日志页勾选开关并等待确认。CE 调试器选择 Windows 调试接口。
5. 设置硬件执行/数据断点时自动建立专属 HVM 会话。已有其他调用者的 HVM
   准备/常驻会话不会被接管；冲突返回 `ERROR_BUSY`。
6. 移除 EPT 断点后可关闭 HVM；此时释放本后端拥有的常驻/准备资源。

`KswordCheatEngineLauncher.exe --ksword-plugin info` 可只读检查载荷，输出
`payload_ready` 和缺失文件路径。启动错误同时报告 Win32 错误码与说明。

## 构建与打包

启动器与桥接 DLL 均只构建 Release/x64，构建命令见 `CheatEnginePlugin/README.md`。
然后运行 `tools/package_cheat_engine_plugin.ps1`，生成 `plugin/cheat-engine/`。
脚本会替换其拥有的整个插件目录。包中保留 CE 原始用户态布局与许可证，
移除 CE 32 位入口、DBK/DBVM 内核载荷及旧的 CE 调色脚本。

本次验证包含 x64 构建、公共后端故障注入、Lua 定时器/协议模拟、载荷与导出
检查；没有执行 GUI 自动化、加载新驱动或启动实机 HVM。
