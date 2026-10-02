# API Monitor 定义、x64 引擎、覆盖与异步采集

## 分层和会话

- `APIMonitor_x64/hook/HookEngine.cpp` 负责入口补丁与 trampoline；HookTargets 负责强类型包装、Raw 入口和 Fake Success。MonitorAgent 管理配置与安装，MonitorPipe 负责有界队列和发送。主程序的 MonitorDock/WinAPIDock 与 MiscDock/ClipboardGuard 共用 Agent。
- 同一个 PID 只允许一个监控会话。`shared/WinApiMonitorProtocol.h` 的 SessionLease 用命名内核对象协调主程序实例、API Monitor 与剪贴板保护，必须在写配置之前获取。
- UI 的后台回调带 sessionGeneration；开始、停止都会换代，旧回调不能修改新会话状态。
- 管道为字节流，读端必须累计一个完整包；不要把一次 ReadFile 当成包边界。共享 readEventPacket 轮询可用字节并检查停止信号。

## Hook 安全约束

- 当前解码器是保守子集，指令描述与两遍布局支持已识别 RIP 相对寻址、CALL/JMP rel32、短跳转、短/近条件跳转和覆盖区内部目标重定位；跳入指令中间拒绝。识别并保留 ENDBR64。未知指令、地址宽度覆盖、超范围位移、不可读/不可验证入口、跳转桩循环和过早 RET/INT3 明确拒绝并报告地址/偏移/原因，不猜长度。
- 优先邻近 relay + 5 字节入口补丁，邻近分配失败后仅安全条件允许才用14字节；代码 RW 写入后转 RX。全局补丁区间登记防重叠，同址同 detour 的兼容绑定共享，不同处理器拒绝覆盖。Raw 与 Strong 使用同一引擎，Strong 表中存在但安装失败不能算已覆盖。
- 安装和恢复必须检查补丁区线程 RIP；挂起线程前准备容器，恢复线程之后才分配错误字符串，避免暂停持有堆锁的线程后死锁。
- 线程枚举使用 NtGetNextThread 检查当前进程线程，避免每个 API 全系统 Toolhelp 扫描。PE 导出遍历用 `hook/ExportCatalog.h` 的固定模块、安全读、范围和数量限制，拒绝畸形/不可读数据。
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
- 协议版本 `0x20261003`，Windows 包大小1000字节。包包含 EventKind、API ID、操作 ID、会话身份、快照修订/状态/种类/地址及定义 SHA256。resultKind 为 StatusCode 或 EntryOnly，Raw 结果显示“未采集”，不能因 resultCode=0 标成 OK 或错误。剪贴板分类码为7。
- 升级必须同时部署新主程序与 APIMonitor_x64.dll；旧协议组合不兼容。已加载旧 Agent 的目标进程需要重启后再监控。剪贴板保护的读端也使用共享包布局与版本检查。

## 回归验证与边界

在 x64 MSVC 开发环境运行 `python tools/api_monitor/run_regressions.py` 顺序执行22个脚本。原始614项冻结身份、确定性生成/非法定义、元数据/返回契约、可执行机器码重定位、共享冲突/卸载失败/退役 trampoline、租约、管道分片、覆盖快照/中断/会话切换、晚加载、丢失统计、Qt状态/筛选和真实自有 x64 文件/IOCP/APC/取消/TCP/UDP/七个扩展路径均已通过。

这些测试编译生产代码或生产函数，覆盖指令边界、恢复失败、在途 detour、跨线程租约、字节管道分片、子配置继承、溢出计数与 Qt 状态/增量筛选。UI 测试使用 offscreen Qt Widgets，不替代实际目标程序的注入、长期压力、多系统版本、CFG/CET 或受保护进程验证。主程序构建使用 `tools/Invoke-KSwordBuildCheck.ps1`；Agent 使用 x64 MSBuild。修改用户可见字符串时定点更新双语包，并通过 i18n 审计。

## JSON 定义与构建（2026-10-02）

- `APIMonitor_x64/api_monitor_definitions.json` 是唯一强类型定义源，641项：原有614项 ID 不变，615–623普通补缺，624–634异步观察，635–641 Winsock扩展。417个普通生成包装、224个具名专用处理器。项目说明见 `APIMonitor_x64/README.md`。
- `tools/api_monitor/generate_definitions.py` 仅用 Python 标准库，编译前校验/生成至 `$(IntDir)generated`，只更新变化内容。绑定表、原函数指针声明、普通包装、扩展发现元数据由 JSON 生成，专用正文保留 C++ 并在 `hook/ApiHandlers.json` 注册。禁止 JSON 函数正文和任意表达式。
- 定义包括 ABI/方向/编码/采集方式/长度/成功条件/错误来源；复杂行为以具名 handler 解释。重复身份/符号、未知类型/处理器、错误长度引用/间接类型/引用环都会失败。
- Release 成功后原样复制至 `Ksword5.1/x64/Release/profiles/api_monitor_definitions.json`，不压缩。主程序有 Agent 非链接依赖；发布副本仅查看/核对，不运行时加载。源定义改后必须重建。
- 生成的定义 SHA256 出现在 Agent 事件/覆盖快照，UI 比较发布 JSON 并提示缺失/不匹配。已验证源与发布副本字节一致，SHA256=`9a371177e144fce6f5939ec7c648e77c95d89acb4e490cd282e8f1c97d6e153c`。
- 基于 SDK 10.0.26100.0 与可选 phnt 原生声明只读核对2592个带方向注解的参数，差异0，定义有217项简单长度关系。`tools/api_monitor/audit_definition_metadata.py --headers ...` 可重复审计；原生声明不作为构建依赖，没有第三方 Hook/解码实现接入。
- Agent x64 Release 增量/干净 Rebuild 均通过。主程序最后检查日志 `.codex-build-logs/ksword-build-check-20261002-134416.raw.log`：BUILD_RESULT=SUCCESS、EXIT_CODE=0、exe=20259840字节、I18N_AUDIT_PASSED=True。每阶段回归和 Release 编译后分别提交。

## 完整覆盖快照

- EventKind：ApiCall=0，IoSubmit/IoWait/IoComplete/IoCancel=1/2/3/4，CoverageBegin/Item/End=16/17/18。Strong/Raw/Fake/Dynamic=0/1/2/3，运行时 Raw/Fake ID 从 `0x80000000` 起。
- 状态：Installed=0、SharedEntry=1、CategoryDisabled=2、WaitingModule=3、ExportMissing=4、RuleExcluded=5、RetryableFailure=6、Unsupported=7、Removed=8。普通、剪贴板 Win32u Fake、已发现扩展均报告实际结果。新会话分类关闭不得继承旧会话 Removed。
- `MonitorCoverage` 产生不可变完整快照，内容不变不升修订；发送线程直接发送，不占普通队列。`shared/ApiMonitorCoverage.h` 验证顺序/完整性、拒绝旧会话/旧修订。断线/中断保留上次完整快照并标记过期。
- UI API事件/API覆盖按 PID 筛选；异步事件标注提交/等待/完成/取消与操作 ID。根/子读端各自维护覆盖接收状态，ClipboardGuard 跳过控制/异步包。会话身份与 UI generation 联合拒绝旧更新。停止使用有界 Drain 发送最新快照/事件后关闭管道。

## 异步 I/O 与 Winsock 扩展

- `core/MonitorAsyncIo` 容量8192，按会话/资源身份/OVERLAPPED 关联单调唯一操作 ID；原调用前登记，提交先于竞态完成，重复完成去重。同步完成与 IOCP 消费分别记录预期，完成键参与关联。
- ReadFile/WriteFile/DeviceIoControl、WSASend/Recv/SendTo/RecvFrom、Ex/结果查询/IOCP/取消入口参与关联；取消请求不表示完成，CancelIo 只针对发起线程。处理 FILE_SKIP_COMPLETION_PORT_ON_SUCCESS 与 hEvent 低位。
- CloseHandle/closesocket 退役身份，句柄复用产生新身份；未完成 OVERLAPPED 重用标记歧义，结果查询/IOCP 不强行关联。满容量报告 IoUntracked 并保留原调用/回调。
- `hook/ContextThunk.h` 保持 x64 寄存器/栈参数并登记 unwind 元数据。停止后应用回调仍执行，保留 error/bytes/OVERLAPPED/flags，旧操作不向新会话发事件。UDP 来源地址完成时有界读取，待完成提交不读无效输出。
- WSAIoctl 的 SIO_GET_EXTENSION_FUNCTION_POINTER 发现 AcceptEx、ConnectEx、WSARecvMsg、WSASendMsg、TransmitFile、TransmitPackets、GetAcceptExSockaddrs，已知异步完成也触发发现。
- 每个扩展地址保留独立原函数/上下文，重复幂等，不替换应用指针；覆盖按实际地址报告冲突/不支持。File 分类关闭但 Network 开启时仍安装必要 IOCP 观察入口，抑制 File 事件并保留 Network 完成关联。

## 需要保留的边界

- 任意 COM 方法、直接系统调用、启动前调用、无法识别的原生异步完成路径、本会话没有通过 WSAIoctl 发现的扩展地址不保证覆盖。
- 启动前 IOCP 关联无法补推；同键同 OVERLAPPED 的手动投递无法完全区分真实内核完成。不同键手动通知不会错误完成操作，已通过回归。
- 系统缺失导出、保守解码器拒绝入口按不可用/失败报告，Raw 不绕过同一入口失败。
- 保留到进程退出的退役代码/回调上下文随重复会话/回调增长。8192限制跟踪槽位，不限制所有退役内存；当前没有安全回收算法。
