# x86 API Monitor

独立的 `APIMonitor_x86.vcxproj` 生成 32 位 Agent，复用 `APIMonitor_x64` 中的会话、采集、异步、覆盖协议与专用处理器。两套 Agent 共用唯一的 `APIMonitor_x64/api_monitor_definitions.json`：641 项定义、417 个生成包装和224个专用处理器，ID 保持一致。不要复制一份 JSON 或维护另一套 HookTargets。

## 构建与部署

MSBuild 和编译器宿主必须是 x64；编译目标为 Win32。在 HostX64/x86 的 MSVC 开发环境中执行，`$msbuild` 指向 `Bin/amd64/MSBuild.exe`：

```powershell
& $msbuild APIMonitor_x86/APIMonitor_x86.vcxproj /t:Build /p:Configuration=Release /p:Platform=Win32 /p:PreferredToolArchitecture=x64 /p:PROCESSOR_ARCHITECTURE=AMD64 /p:PROCESSOR_ARCHITEW6432=AMD64 /m:1 /v:minimal
```

构建前使用 Python 标准库生成器，`--architecture x86` 输出至 `APIMonitor_x86/obj/Release/generated`。`sizeof` 按目标平台计算参数栈字节数及返回宽度，不能使用 x64 生成目录。成功后发布到统一的 `Ksword5.1/x64/Release`：

- `APIMonitor_x86.dll` 和对应 PDB。
- `profiles/api_monitor_definitions.json` 原样副本；两套 Agent 内嵌同一 SHA256。

主程序通过非链接项目依赖同时构建两个 Agent。发行时携带主程序、两个 DLL 和 JSON，不再构建或部署独立注入助手。32 位 Agent 采用静态 CRT，避免32位运行库覆盖主程序目录中的64位运行库。

API 监控及剪贴板保护在安装时查询目标进程架构，自动选择同目录的对应 DLL；自定义文件名保留，但必须通过 PE 位数校验。64位主程序直接完成两种位数的注入；32位加载器通过目标 PE 导出表解析，不套用本地64位 RVA。子进程由 Agent 通过会话专用管道请求主程序注入，校验会话令牌、管道真实客户端、进程创建时间、父子关系和子配置。自动子进程跟踪也支持32→64及64→32，子配置记录正确 DLL 并继承根停止路径。主程序退出或会话停止后，新子进程注入明确失败；不回退到启动助手。ARM/ARM64、位数错误、父进程身份不匹配或权限不足会报告失败。

## 引擎与 ABI

`hook/HookEngineX86.cpp` 编译共享引擎的 x86 分支，使用本目录的保守指令解码器。支持32位绝对寻址、CALL/JMP rel32、短/近分支及覆盖区内部目标修正，保留 ENDBR32；使用5字节跳转及32位地址回绕。线程检查使用 EIP，共享补丁、冲突、失败重试和退役 trampoline 策略与 x64 一致。未知指令、地址宽度覆盖、跳入指令中间和不可读入口明确拒绝，Raw 也使用相同引擎。

Raw 桩保存寄存器、标志及 x87/MMX/XMM 状态，再尾跳原函数，保持 cdecl/stdcall/fastcall 原调用约定。现有异步回调和七种 Winsock 扩展使用独立上下文桩，复制原32位参数、按 stdcall 清栈；停止后仍执行已提交的应用回调，旧操作不能进入新会话。

Fake 配置兼容旧六字段规则，第七字段是可选的 `x86StackBytes`：

```text
module|api|returnType|returnValue|lastErrorKind|lastErrorValue|x86StackBytes
```

已知签名自动计算清栈字节数；显式覆盖必须与签名一致。未知导出必须提供0至65532的4的倍数，stdcall 填参数栈字节数，cdecl 填0；未提供或不兼容的 ABI 显示为不支持。未知 Fake 不支持 fastcall、浮点及结构返回，也不生成输出参数。界面有对应输入和校验。标量及64位整数返回使用 EAX/EDX:EAX；指针与 HANDLE 按目标宽度处理。

## 验证与范围

在 HostX64/x86 的 MSVC 开发环境运行：

```powershell
python tools/api_monitor/run_regressions.py --architecture x86
```

29个脚本覆盖生产引擎的可执行机器码、Raw/Fake ABI、元数据、协议、会话、真实文件/IOCP/APC/取消/TCP/UDP、七种扩展、双向跨位数注入及子进程停止。x64 环境的33个脚本另含 x64 解码与五项 offscreen Qt 回归；UI 规则及 UTF-16 会话配置直接交给生产 Agent 解析器验证；会话服务回归覆盖错误身份、重复注入、旧会话和未完整发送的客户端。新增回归覆盖全部分类与默认 Raw、TLS 清理后的旁路、加载器标记参数/不可读页，以及默认栈只有256 KiB的目标进程。

全选监控的安全约束：Raw 排除策略同时检查导出实际归属与跳转桩目标，不能通过 `gdi32!EngAcquireSemaphore` 等别名钩入 ntdll；线程 TLS 尚未初始化或已清理时直接调用原函数；`LdrLoadDll` 的低地址搜索参数作为标记值记录，不当字符串读取；256条事件发送批次使用受管道锁保护的静态缓冲，不占用宿主的小线程栈。2026-10-02 使用新启动的 x86 JC、SysWOW64 记事本和 cmd 做了全选分类、默认 Raw、自动子进程安装及停止验证，三者通过；短时实测不替代长期压力矩阵。

覆盖边界与 x64 相同：不保证任意 COM 方法、直接系统调用、启动前调用、未识别原生完成路径及未发现的扩展地址。缺失导出或不支持入口按实际失败显示。退役代码保留到目标进程退出，多会话仍会增加内存。跨用户临时目录、受保护进程、多系统版本、长期压力和严格 CFG/CET 尚未做完整矩阵验证。更新已驻留的 Agent 时需要重启目标进程。
