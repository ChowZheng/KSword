# API Monitor 生命周期与事件协议

## 分层和会话

- `APIMonitor_x64/hook/HookEngine.cpp` 负责入口补丁与 trampoline；HookTargets 负责强类型包装、Raw 入口和 Fake Success。MonitorAgent 管理配置与安装，MonitorPipe 负责有界队列和发送。主程序的 MonitorDock/WinAPIDock 与 MiscDock/ClipboardGuard 共用 Agent。
- 同一个 PID 只允许一个监控会话。`shared/WinApiMonitorProtocol.h` 的 SessionLease 用命名内核对象协调主程序实例、API Monitor 与剪贴板保护，必须在写配置之前获取。
- UI 的后台回调带 sessionGeneration；开始、停止都会换代，旧回调不能修改新会话状态。
- 管道为字节流，读端必须累计一个完整包；不要把一次 ReadFile 当成包边界。共享 readEventPacket 轮询可用字节并检查停止信号。

## Hook 安全约束

- 当前解码器是保守子集，无法处理的指令、相对寻址、地址宽度覆盖和过早 RET/INT3 应拒绝安装，不能猜长度后覆盖下一函数。
- 安装和恢复必须检查补丁区线程 RIP；挂起线程前准备容器，恢复线程之后才分配错误字符串，避免暂停持有堆锁的线程后死锁。
- 恢复失败必须保留 installed 和原始指针，允许重试。
- 入口恢复不证明 detour 或 trampoline 已无人执行。已公布的 trampoline、Raw/Fake stub 及绑定上下文保留到进程退出；目标模块与 Agent 固定驻留，配置快照不可变。重复会话会增加内存占用，这是当前保守回收策略的代价，后续替换引擎需单独设计安全回收。

## 子进程与就绪状态

- 自动子进程支持 CreateProcess A/W、CreateProcessAsUser A/W、CreateProcessWithTokenW、CreateProcessWithLogonW；关闭进程分类时自动注入入口仍可安装。根及子管道继续解析 AutoInjectChild，以发现后代。
- 子配置以 UTF-16 临时文件原子替换，继承剪贴板策略、Raw/Fake 配置与根停止路径；停止根会话可通知尚未被 UI 发现的后代。
- 管道连接不代表 Hook 已安装；UI 分别显示安装中、部分生效、安装失败、监控中与已移除。

## 有界记录与协议

- Agent 环形队列 4096 个包，其中 64 个容量预留内部通知。队列拒收及取出后无法发送的包计入累计 EventsDropped 通知，通知由发送线程直接发送，不进入满队列。内部通知洪峰仍可能溢出，进程突然退出前的最后统计也不能保证送达。
- UI 保留 24000 个待显示事件与 12000 行表格，分别记录 UI 丢失、移出表格和待显示数量。Agent 丢失按 PID 汇总。TSV 只导出当前可见行，包含覆盖范围与计数说明；它不是完整调用轨迹。
- 筛选文本缓存于时间列 UserRole；已有行不可变，仅筛选条件变化时全表扫描。新行单独筛选，淘汰旧行时同步可见计数。
- 协议版本 `0x20261002`，Windows 包大小 872 字节。新增 resultKind：StatusCode 或 EntryOnly。Raw fallback 只记录入口，结果显示“未采集”，不能因为 resultCode=0 标成 OK，也不能标成错误。剪贴板分类码为 7。
- 升级必须同时部署新主程序与 APIMonitor_x64.dll；旧协议组合不兼容。已加载旧 Agent 的目标进程需要重启后再监控。剪贴板保护的读端也使用共享包布局与版本检查。

## 回归验证与边界

在 x64 MSVC 开发环境中运行 `python tools/api_monitor/test_*.py` 的各脚本（通配符不能作为 Python 单脚本参数）：decoder、lifecycle、session_lease、pipe_packets、child_config、event_loss、ui_status、ui_filter。

这些测试编译生产代码或生产函数，覆盖指令边界、恢复失败、在途 detour、跨线程租约、字节管道分片、子配置继承、溢出计数与 Qt 状态/增量筛选。UI 测试使用 offscreen Qt Widgets，不替代实际目标程序的注入、长期压力、多系统版本、CFG/CET 或受保护进程验证。主程序构建使用 `tools/Invoke-KSwordBuildCheck.ps1`；Agent 使用 x64 MSBuild。修改用户可见字符串时定点更新双语包，并通过 i18n 审计。
