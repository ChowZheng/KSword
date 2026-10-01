# Cheat Engine KSword 插件

插件只启动 64 位 CE 7.6。CE 保留自己的主窗口、颜色、字体、菜单和布局，
仅在标题末尾显示 `[KSword Connected]`、`[KSword R0]` 或 `[KSword HVM]`。
它们分别表示桥接已连接但驱动不可用、已选择 R0、已选择 HVM；HVM 标识本身
不代表已经常驻或安装断点。驱动不可用时保留 CE 的原生功能。
关闭 KSword 日志 Tab 不会关闭 CE。

KSword 内嵌的是日志页，使用宿主主题 token 和 Consolas 等宽字体。页面包含
“使用 HVM”开关与内存/调试选项，实际状态由 CE 后端确认；切换失败会显示错误码并保持原状态。
启动、桥接、调试异常和生命周期日志从 CE 转发至该页面与 PluginHost。
这不是 CE 全部内部输出的捕获器。单行、显示长度和后端日志文件均有上限。

## 后端能力

- 进程打开、虚拟内存查询和读写由 KSword 管理；设备句柄在会话内复用。
- 线程上下文、挂起/恢复、远程分配和保护修改使用 KSword R0 调试协议。
  当前 VM 的 R0 上下文查询失败及挂起计数导出缺失会明确记录，并使用 Windows
  上下文/挂起接口；访问拒绝和身份校验失败不触发回退。
- 选择 HVM 后，普通内存读写经过 HVM 私有窗口；Shadow 代码修改由执行视图
  协议处理。窗口或 Shadow 路径不可用时明确失败并记录。
- CE 的 Windows 调试接口设置 DR0–DR3 时，公共后端把断点转换为 HVM EPT
  执行/写入/读写断点，再以 `EXCEPTION_SINGLE_STEP` 交给 CE。
- CE Lua 的 `KSword.hvm` 暴露当前全部 HVM 协议，包括生命周期、EPT 规则、
  事件、视图、CR/MSR 策略、域、进程、注入、平台、指标和嵌套探测/页面。

常规 EPT 调试要求 Intel VMX、MTF 和匹配的新驱动。执行断点按指令地址匹配；
该路径的数据断点目前是 **4 KiB 页粒度**，同页其他访问也可能命中。
常规执行断点协议不可用时，自动回退到 EPTP 切换的 ShadowPage 隐藏 INT3，
不要求 MTF。原页保留原指令，执行视图包含 INT3；真实 Windows 停止事件、
单步及重装仍由调试循环处理。该回退路径的 CE 数据 DR 保留 Windows 行为。
它不是 CE 的 VEH/DBVM 调试器适配。现有驱动未支持的平台或操作仍会返回其能力/错误状态。
Windows 继续提供调试事件传递与远程线程创建。

## 内存修改与冻结配置

CE 的内存编辑、Lua 目标写入和地址列表冻结都使用同一个 `WriteProcessMemory`
策略。默认关闭影子执行页代码修改，保留普通写入和数据冻结；允许显式回退、
全部回退日志、Windows 上下文/挂起回退开启，常规断点模式，Shadow 页上限为 32。
HVM 默认关闭。用户可自行开启影子执行页代码修改；使用前需先开启 HVM。
RX/RWX 保护只能证明页面允许执行，不能证明每次 CE 写入都是代码补丁。
这类页上的数字冻结需要普通数据写入；开启 Shadow 后，执行视图修改不会改变
普通数据读取，不能用它冻结数字数据。

| 日志页选项 | 实际行为 |
| --- | --- |
| 影子执行页代码修改 | 默认关闭。HVM 下只改可执行页的执行视图，原页及公开读取保留原字节；不适用于 RX/RWX 页上的数字冻结 |
| 允许显式回退（含数据写入） | 纯非执行数据范围可普通写入/冻结，每次实际回退结果均写日志；关闭后拒绝该回退 |
| 隐蔽断点模式 | 优先隐藏 INT3/Shadow 路由，强制代码写入走 Shadow；不隐藏 Windows 调试端口 |
| Windows 上下文回退 | 驱动上下文不支持时允许记录后调用 Windows，受总回退开关控制；身份/策略拒绝仍不能回退 |
| Windows 挂起/恢复回退 | 驱动挂起计数接口不支持时允许记录后调用 Windows，仍受总回退开关控制 |
| Shadow 页上限 | 1–32 页，后端实际资源限制，超过上限明确失败 |
| 恢复 Shadow 写入 | 恢复本适配器的执行视图修改，保留隐藏断点；先取消 CE 冻结，否则下次冻结可再次创建修改 |

Shadow 代码修改失败不会转为原代码页写入。跨代码/数据的混合范围拒绝。
当前每次代码修改必须位于一个 4 KiB 页内且不超过 1232 字节；跨页/过长请求
在修改之前拒绝，不留下前半段修改。共享 image/mapped 页不支持此物理页视图写入。
冻结普通数据要让目标的数据读取看到新值，因此普通数据回退是可见修改。
关闭 Shadow 代码写入只在常规模式有效；隐蔽模式强制 Shadow。

勾选选项后点击“应用选项”，等待后端确认。后端持有断点或 Shadow 修改页时
拒绝更改策略；先移除断点、停止冻结并恢复 Shadow 写入。HVM 开关单独确认。
页面显示确认后的路由、Shadow 写入页数、回退次数和最近回退原因。
请求失败保留实际配置，5 秒未收到确认时允许重试。设置只在确认后保存到
`%LOCALAPPDATA%\KSword\ce-backend-options.txt`，下次启动按保存配置请求后端；
保存失败单独显示并记录。驱动不可用时保留 CE 原生表并记录该路由，已保存的
Shadow 选项不表示此时正在使用 Shadow。

Lua 可查询/修改运行时策略和恢复指定范围：

```lua
local actual, errorCode = KSword.setOptions({shadowMemoryWrites = true,
    allowFallback = false, mode = 0, maxShadowPages = 8})
-- 失败时 actual 仍是后端当前配置；检查 errorCode 后再使用。
local policy = KSword.policy()
KSword.restoreShadowWrites() -- 0/0：恢复全部自有内存修改，保留断点
-- KSword.restoreShadowWrites(address, byteCount) -- 指定非零范围
```

`KSword.options()` 返回确认配置；`KSword.policy()` 另返回活动路径（0 原生、
1 EPT、2 Shadow）、页数、断点数、是否可修改和回退统计。Lua 直接设置是运行时
设置；日志页确认的设置才写入持久化文件。回退日志不可关闭。

无 GUI 的 Lua ABI/确认/持久化验证：

```powershell
py -3.13 CheatEngineExecutablePlugin\tests\validate_lua_policy.py `
  --lua-dll 'plugin\cheat-engine\payload\Cheat Engine\lua53-64.dll'
```

该测试模拟 CE 调用与后端响应，不替代驱动执行视图或真实冻结验证。

公共后端、协议、资源所有权和 API 示例见仓库 `DebuggerBackend/README.md`；
插件包内对应 `DEBUGGER_BACKEND.md`。

## 使用

1. 加载本次构建的 `KswordARK.sys`，新协议不兼容旧驱动。
2. 从 KSword 插件 Tab 启动，或从进程菜单打开所选 PID。
3. 标题出现 KSword 标识后可使用后端；默认选择 R0。
4. 如需 HVM，在日志页勾选开关并等待确认。CE 调试器选择 Windows 调试接口。
5. 设置 HVM 执行断点时自动选择常规 EPT 或 ShadowPage 回退并建立专属会话。
   已有其他调用者的 HVM
   准备/常驻会话不会被接管；冲突返回 `ERROR_BUSY`。
6. 移除 EPT 断点后可关闭 HVM；此时释放本后端拥有的常驻/准备资源。

`KswordCheatEngineLauncher.exe --ksword-plugin info` 可只读检查载荷，输出
`payload_ready` 和缺失文件路径。启动错误同时报告 Win32 错误码与说明。

## 构建与打包

启动器与桥接 DLL 均只构建 Release/x64，构建命令见 `CheatEnginePlugin/README.md`。
先运行 `python tools/export_cheat_engine_source.py`，保存本轮对应源码归档；
再运行 `tools/package_cheat_engine_plugin.ps1`，生成 `plugin/cheat-engine/`，
其中包含 `KSword-cheat-engine-source.zip`。
脚本会替换其拥有的整个插件目录。包中保留 CE 原始用户态布局与许可证，
移除 CE 32 位入口、DBK/DBVM 内核载荷及旧的 CE 调色脚本。

2026-09-30 在 `KSword-HVM-Target` 完成真实 CE GUI、三种标题状态、R0/HVM
内存读写与扫描、自动隐藏 INT3 回退、寄存器修改、单步、重复命中及日志 Tab
验收。详细结果及未支持的常规 MTF 路径见 `docs/ksword-debugger-vm-validation.md`。
