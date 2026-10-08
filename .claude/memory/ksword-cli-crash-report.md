# CLI 崩溃报告与 VMware 验收（2026-10-08）

- `log` 在 `_O_U8TEXT` 下用 `std::cout` 写字节：偶数字节被当作 UTF-16 产生乱码，奇数字节触发 CRT invalid-parameter fastfail `0xC0000409`。不是栈缓冲溢出。统一先解码 UTF-8（旧 ANSI 帧回退系统代码页），再用 `std::wcout`。不要在宽字符 CRT 模式下恢复任何窄字符 stdout/stderr 写入。
- `tools/test_ksword_cli.py` 直接包含生产 CLI，并用 Windows 传输夹具验证真实 CRT 重定向、日志边界、参数诊断、退出码、别名、帮助和失败顺序；扩展结果打印/退出码函数也来自生产源文件。需在 HostX64 VS 环境运行。
- 旧报告在 26200 系统上使用 2026-10-01 驱动，所有只读请求连能力查询都返回 50，与已于 2026-10-03 删除的 OS build 上限分发门禁一致。IOCTL 名称打印正确不证明驱动未注册。查 `preflight query`、`r0 ioctl-registry` 与实际服务映像路径；不得因旧报告重新关闭现有处理器或绕过逐功能布局校验。
- 内置帮助的当前顶层 `driver` 子树已完整；用回归测试保证与 family help 一致。缺必填项和未知选项分开诊断，命令参数错误带语法/参数元数据；允许的选项按具体命令 help 语法检查，不能依赖以前被静默忽略的拼错参数（例如 `r0 object-types --limit`，应使用 `--max-entries`）。
- 退出码保留既有语义：0 成功、1 参数、2 打开设备失败、3 I/O 失败、4 响应格式、5 不支持/证据不可用；扩展和固定/变长响应的同类传输失败保持一致。失败且默认 NTSTATUS=0 时显示 `n/a`，实际返回的非零状态保留。
- 缺驱动现场验收发现 Callback Monitor 和内核枚举封装会覆盖 `IoResult.message`，因此不能依赖 `CreateFileW` 字符串判断阶段。`DriverClient::deviceIoControl` 在打开失败时设置 `IoResult.deviceOpenFailed`，后续复制保留；CLI 用该标志判断退出码和启动提示，不能将真正 IOCTL 内部的文件不存在误归为驱动没加载。
- `tools/ksword_cli_vm.py` 通过安装的 VMware VIX x64 DLL 进程内登录、传文件和运行程序，不在命令行传密码。`tools/Invoke-KSwordCliReportCases.ps1` 在 VMware 来宾执行报告涉及的只读命令、保留 stdout/stderr、退出码、超时、OS/服务/文件 SHA256。`-EnsureDriverLoaded` 只加载 Release 中已有驱动并保留运行供后续复用，不替换驱动或修改签名策略。
- 本次 Win10 20H2 来宾原 CLI 真实复现日志乱码及 1/2/100 帧 fastfail；仅替换 CLI 的候选版本全部正常。当前同目录原 SYS 下进程/SSDT/回调/能力等查询可返回，PiDDB/卸载驱动全局证据仍可如实 unavailable。服务最初未安装，验收创建正常 SCM 测试服务，保留用于复用。
- 来宾 Administrator 无密码，VIX 返回 3033 时由用户设置 `LimitBlankPasswordUse=0`；用户明确要求保持此测试环境设置，不再恢复。来宾通道不需要网络 WinRM/SSH。
- 用户原有七个未提交 UI/语言包/记忆改动应保留；CLI 提交不得顺带提交它们。
