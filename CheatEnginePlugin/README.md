# KSword Cheat Engine 调试后端适配器

这是 CE SDK v6 的 x64 原生插件。`KswordCeBridge.cpp` 只处理 CE 函数表与调用
转换，内存、调试、EPT 和会话策略在 `DebuggerBackend/`，其他调试器可复用。

## 接管的接口

| CE / Windows 接口 | 后端 |
| --- | --- |
| OpenProcess | 保留请求权限；失败时查询/同步句柄或带对象身份校验的 PID 代理 |
| VirtualQueryEx | KSword 虚拟内存查询 |
| ReadProcessMemory / WriteProcessMemory | R0 分片读写；HVM 模式为严格私有窗口读写 |
| GetThreadContext / SetThreadContext | KSword R0 上下文；HVM 调试寄存器映射为 EPT 断点 |
| SuspendThread / ResumeThread | KSword R0，保留原挂起计数语义 |
| VirtualAllocEx / VirtualProtectEx | KSword R0 调试协议 |
| OpenThread | 取得查询身份所需句柄，上下文和挂起操作交由 R0 |
| DebugActiveProcess / WaitForDebugEvent / ContinueDebugEvent | 公共后端管理会话、EPT 生命周期和继续执行，Windows 传递事件 |
| CreateRemoteThread | 公共后端解析进程身份，Windows 创建线程 |

共有 15 个 SDK hook。写入和调试变更仍受驱动安全策略约束；内存写入不添加
FORCE。HVM 自检/常驻启动按现有控制协议携带所需确认标记。
64 位线程上下文是本版原生调试协议的范围；WOW64 上下文不做隐式转换。
CE VEH/DBVM 不经过此 Windows 调试接口，不能宣称也被这些 hook 接管。

HVM 开关、全部 HVM API、断点精度和资源所有权说明见
`DebuggerBackend/README.md`。CE 本体只增加标题标识，主题与等宽字体应用在
KSword 的日志页。

## 构建

```powershell
$msbuild='C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe'
$hostToolArgs=@('/p:PreferredToolArchitecture=x64', '/p:PROCESSOR_ARCHITECTURE=AMD64', '/p:PROCESSOR_ARCHITEW6432=AMD64')
& $msbuild 'CheatEnginePlugin\KswordCheatEnginePlugin.vcxproj' `
    /t:Build /p:Configuration=Release /p:Platform=x64 @hostToolArgs /m:1 /v:minimal
& $msbuild 'CheatEngineExecutablePlugin\KswordCheatEngineLauncher.vcxproj' `
    /t:Build /p:Configuration=Release /p:Platform=x64 @hostToolArgs /m:1 /v:minimal
```

两个架构变量是必要参数：MSVC 导入规则可以覆盖单独的工具架构偏好。
不提供 Win32 配置或 DLL。输出分别为这两个工程的 `x64/Release/`。

## 加载与停用

KSword 启动器通过 autorun 自动加载 `KSword Debugger Backend 2.0`；也可以在
64 位 CE 中手动启用 DLL，但自动日志、开关和标题标识由 autorun 提供。
初始化无法连接 KSword 驱动时拒绝安装 hook。

停用恢复仍由本插件拥有的 SDK 指针、移除 EPT 规则、释放自有 HVM 与设备会话。
清理失败时拒绝停用。DLL 代码映射保留至 CE 退出，避免等待调试事件的线程
返回到已卸载代码；逻辑停用后公共 API 返回设备未连接。

## ABI 来源

SDK 函数表依据 CE 官方仓库的 `Cheat Engine/plugin/cepluginsdk.h`。
本项目只声明使用到 `OpenThread` 的 ABI 前缀，未复制 Lua SDK 或示例实现。
原始许可证与通知保留在发行载荷中。
