# KswordUacDesk 生命周期与日志

- 主程序 `main.cpp::tryLaunchUacDeskCompanion` 仅在非救援、已提升实例首次显示主窗口后启动同目录助手。initial 阶段识别实际父进程的 `Ksword5.1.exe` 名称，将 PID 与创建时间传给 admin/SYSTEM 阶段；独立启动没有主进程绑定。
- 2026-10-04：`UacDeskWindow::setParentWatch` 改为一次身份校验后，在一个后台线程中用 `WaitForMultipleObjects(INFINITE)` 等待原始父进程句柄或私有停止事件。取消事件排第一；销毁先置停止标志、唤醒并 join，再关闭句柄。不得恢复每 400ms 新建线程的轮询，也不得在 UI 线程阻塞等待。持有已校验的进程句柄后无需反复查询创建时间；PID 复用不会替换句柄对应的对象。
- 父进程结束会排队停止扫描定时器并请求 Qt 退出。主窗口隐藏后的异步停驱、主进程析构期间，进程尚未终止，助手仍等待。建立监视失败时 SYSTEM 阶段返回 11，不能继续无监视常驻。
- `ControlTraceW(..., Properties=nullptr, STOP)` 返回参数错误，不能停止会话。此前现场日志出现父进程退出、Qt 事件循环返回，但助手继续残留；LUA 线程可能阻塞在 `ProcessTrace`，析构的 `join` 因而卡住。
- LUA 停止和启用失败的清理现使用初始化的 `EVENT_TRACE_PROPERTIES`；LUA/ALPC 的消费句柄通过原子 exchange 由 worker/stopper 唯一关闭，停止时 `CloseTrace` 取消实时消费，包含停止与 `OpenTrace` 发布交错的情况。会话单实例互斥句柄通过 RAII 持有到窗口、监视器和线程析构完成。
- 日志按用户要求在所有构建配置禁用：`KSWORD_UAC_DIAGNOSTIC_LOG` 编译掉参数求值，删除硬编码目录/文件写入和 `OutputDebugStringW`，Qt 空消息处理器在 QApplication 构造前安装。ETW 数据采集本身保留；不要恢复高频原始事件诊断输出。
- 验证：HostX64 MSVC x64 Release 构建通过；链接真实 Release 对象的 Qt offscreen 回归覆盖日志参数不求值、活父进程销毁取消等待（20 次）、创建时间不匹配、独立模式、父进程正常退出/强制结束/建立监视前退出。构建后 exe 不含旧日志路径/文件名。当前宿主未提升，完整 SYSTEM/UIAccess 安全桌面与特权 ETW 会话退出尚未实机验收。
