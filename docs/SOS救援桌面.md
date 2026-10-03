# SOS 救援桌面

运行同一套新版 `Taskbar.exe` 和 `Ksword5.1.exe`，用真实键盘依次输入 `S O S Enter`。Taskbar 启动独立的无 Qt 引导进程，在原桌面先请求 Windows UAC。批准后由管理员监护进程创建私有 Win32 Desktop，并用同一高权限令牌启动新的 KSword 实例。取消 UAC、提权失败或无法核验提升令牌时结束本次启动，不创建低权限救援实例。若 Taskbar 已经提升，则直接进入高权限监护流程。已有主窗口保留在原桌面；主程序的普通防多开设置不拦截经过句柄校验的救援实例。

UAC 请求不在 SOS 的低级键盘 Hook 线程内等待。引导进程保持到高权限会话退出，使常驻 Taskbar 能继续阻止并发 SOS。桌面、就绪/返回事件及监护句柄全部在 UAC 批准之后创建，不向 `runas` 透传桌面句柄。

原桌面名称在请求 UAC 前读取。高权限监护进程按该名称打开原桌面，待它重新接收输入且 KSword 就绪后才切入救援桌面；不在 UAC 刚批准时重新把安全桌面当成原桌面。启动阶段和 Win32 错误记录到 exe 同目录 `logs/sos-rescue-<PID>.log`，普通引导进程与管理员监护进程各有一份，包含提权、桌面创建/绑定、输入 Hook、客户端启动/退出及切换状态。

救援窗口顶部有返回按钮，点击后恢复原桌面并关闭本次实例。真实键盘 `Ctrl+Alt+Shift+F10` 由监护进程处理，即使 KSword UI 线程正在阻塞也可请求返回。正常退出、子进程退出及监护进程意外退出各有恢复路径。切入前等待主事件循环和输入过滤器就绪，初始化失败不会退回在受干扰桌面启动 KSword。

## 桌面隔离与输入

- 桌面 DACL 不含允许 ACE，并以 OWNER RIGHTS 拒绝 ACE 关闭同账号所有者的隐式改 ACL 权限。普通进程不能按名称重新取得该桌面的窗口、Hook、读取安全描述符或修改 DACL 权限。
- 白名单只继承本次救援桌面、两个未命名事件和监护进程的 `SYNCHRONIZE` 句柄。客户端必须在创建任何 Qt/启动页窗口前显式 `SetThreadDesktop`，不能依赖桌面自动选择。
- 监护循环在该桌面安装 `WH_MOUSE_LL` 与 `WH_KEYBOARD_LL`，拒绝注入及低完整性注入标志。Hook 回调不启动进程、读写磁盘或进入 Qt；耗时工作在其外部执行。
- KSword 再用 `GetCurrentInputMessageSource` 检查鼠标/键盘消息的设备和硬件来源，拒绝来源未知、系统合成及消息伪造。还拒绝应用队列制造的鼠标点击/滚轮和键盘事件。真实键盘保留。
- `IMO_HARDWARE` 可能包含 UIAccess 注入，因此不能单独作为物理输入证明，仍需低级 Hook 的注入标志。R3 无法鉴别伪装成真实设备的虚拟 HID、驱动级注入或进程被篡改后的输入。
- 退出恢复只针对当前活动的救援桌面，不抢走锁屏、UAC 或用户切换到的其他桌面。自动管理员/缩放重启和权限接管在救援模式下受限，避免丢失私有句柄。退出救援实例不自动停止原实例正在使用的全局 R0 服务。

这是独立的、限制访问的 Win32 桌面，不是 Windows 的 Winlogon/UAC 安全桌面。高级权限或内核代码仍可突破用户态的保护边界。

## 现有窗口能否平移

Windows 的 `SetThreadDesktop` 对已拥有窗口或 Hook 的线程返回 `ERROR_BUSY`。KSword 的 Qt 主线程及其他 UI 线程已有窗口和平台辅助对象，现有 HWND 无法通过公开接口直接搬到另一个桌面。本实现创建新实例，没有迁移旧实例的未保存编辑或内存会话。

参考：[SetThreadDesktop](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setthreaddesktop)、[桌面访问权限](https://learn.microsoft.com/en-us/windows/win32/winstation/desktop-security-and-access-rights)、[输入来源](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ne-winuser-input_message_origin_id)。

UAC 入口使用 [`ShellExecuteEx` 的 `runas`](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/ns-shellapi-shellexecuteinfow)，并在新监护进程中再次查询真实 `TokenElevation`。内部“已尝试提权”标记只防止重复提示，不能代替令牌检查。

## 验证

`tools/Invoke-RescueDesktopTests.ps1` 使用已有 Taskbar Release 中间目录和 HostX64 编译器。测试创建不显示的私有桌面，通过正式启动函数创建测试子进程，验证句柄白名单、显式桌面绑定、同账号按名称访问拒绝、已有窗口迁移 `ERROR_BUSY` 以及真实窗口过程拒绝 SendMessage/PostMessage 输入。另用真实 Taskbar 验证未提升的内部入口拒绝伪造提权标记；测试宿主本身已经提升时跳过此探测，避免意外进入救援桌面。整个测试不请求 UAC、不切换输入桌面。

Release 编译和这些回归不能替代 UAC 批准/取消、救援实例管理员令牌、实机桌面切入、真实鼠标/键盘、中文输入法、跨屏、锁屏/UAC、监护进程退出和焦点骚扰下的操作验收。首次现场验证应先使用正常桌面及可控的自建干扰程序，不运行 SilkMain。

2026-10-03 修正 UAC 原桌面接续后，用户已确认救援窗口正常显示、管理员状态已启用。本次运行日志确认监护进程的提升令牌、客户端就绪、成功切入与返回原桌面，客户端及监护进程均正常退出。锁屏、焦点骚扰及各类真实/模拟输入的完整验收仍需分别记录。
