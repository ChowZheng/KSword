# KSword 公共调试后端

本目录没有 CE SDK 依赖。内存访问、原生 R0 调试、HVM 选择、EPT 断点事务和
会话所有权集中在 `Backend`。CE 是首个适配器；后续调试器只需把自己的函数表、
事件和断点请求转换为这些公共方法，或调用导出的 C ABI。

## 分层

- `KswordDebuggerApi.h`：固定宽度、带版本的进程内 C ABI。
- `KswordDebuggerBackend.cpp`：会话、状态、能力布局、HVM 协议转发和日志。
- `KswordDebuggerMemory.cpp`：进程身份、R0/HVM 内存、分配与保护。
- `KswordDebuggerNative.cpp`：R0 线程上下文、挂起/恢复及公共 ShadowPage 执行断点回退。
- `KswordDebuggerFileProtocol.h`：共享控制文件读取；允许原子替换，读取后立即关闭句柄。
- `KswordDebuggerDebug.cpp`：Windows 调试事件适配、EPT 断点事务与资源退休。
- `CheatEnginePlugin/`：CE SDK hook，不能在这里实现新的驱动业务或 HVM 策略。
- `shared/driver/`：唯一 R0/R3 协议定义；内核业务在对应 feature 模块，注册表
  仅注册 handler。

其他调试器可将这些实现编入自己的适配模块，复用 `ArkDriverClient`，导出
`KSwordDebuggerCall`。当前 DLL 由 CE 适配工程生成，没有另建独立后端服务。
每个调试器进程拥有一个后端实例及持久设备会话。
公共后端的日志路径由 `KSWORD_DEBUGGER_LOG_FILE` 提供，适配器自行设置；
CE 启动器使它与 autorun 启动日志写入同一文件。公共代码不依赖 CE 环境变量。

## 保留原生调试循环的适配器

TitanEngine 等前端可以保留自己的 Windows 调试循环，使用下列 C++ 方法借用
公共后端。它们不调用 `DebugActiveProcess`、`DebugActiveProcessStop`、
`WaitForDebugEvent` 或 `ContinueDebugEvent`：

- `observeNativeSession(pid, &generation)`：保留真实进程对象句柄，建立原生会话。
  同一活动进程重复调用返回原代次；已有 CE 附加或不同进程返回 `ERROR_BUSY`。
- `observeNativeEvent(event, generation)`：记录前端已取得的事件，检查 PID 和代次。
- `validateNativeContinue(pid, tid, generation)`：继续前验证已记录事件及自动 EPT
  所需的全部 CPU 常驻状态。通过后由前端调用原有 Windows 继续接口。
- `retireNativeThread(tid, generation)`：停止自有常驻、只删除该线程的自动 EPT
  规则，再恢复常驻。其它线程和原始 API 规则不由该方法删除。
- `releaseNativeSession(generation)`：退出原生循环后退休本后端的规则和准备。
  没有自有准备时不触碰其它调用者的 HVM 常驻。

前端须把返回的非零代次保存在会话中，并传给异步事件、上下文及退休回调。
旧代次返回 `ERROR_INVALID_STATE`，不能结束新会话。进程句柄及每份自动断点
持有的线程对象句柄避免 PID/TID 重用；上下文叠加还复核线程创建时间。
`generation=0` 仅用于同步调用当前会话。原生 `EXIT_PROCESS` 事件仍由前端持有
时，状态轮询保留会话，待事件继续及原生循环收尾后显式释放。

原生上下文由 Windows 获取并保留完整 `CONTEXT` / XSTATE 分配。
`overlayNativeDebugContext` 仅叠加 DR0–DR3 与 DR7，保留 Windows 的 DR6 命中位、
GPR、SIMD 和扩展状态。`applyNativeDebugContext` 的 writer 回调只接收
`CONTEXT_DEBUG_REGISTERS` 切片；前端把这些 DR 字段放入完整 Windows 上下文副本，
再交给自己的原始 `SetThreadContext`。固定 1232 字节 R0 协议不承载 XSTATE。

writer 返回失败或常驻恢复失败时，后端恢复原 EPT 规则，并以 `rollback=TRUE`
回调恢复之前的物理调试寄存器；回调必须只写调试寄存器，不能重放 GPR/SIMD
写入。全部步骤在同一后端锁内进行。前端继续持有真实 Windows 停止事件，
不得在上下文事务结束之前继续目标。

## 执行断点自动回退

常规 HVM EPT 调试不可用时，公共后端尝试 EPTP 切换的 ShadowPage 隐藏 INT3。
该路径要求执行专用 EPT 与 INVEPT 支持，不要求 MTF。匹配的新驱动通过
`KswordArkDebuggerIoctl.h` 的原生操作 8/9 管理影子页：MDL 固定目标用户页，
保留 owner/target 进程身份，同页多个偏移合并，原始读取保留原字节。

CE 的执行 DR 请求转成私有隐藏 INT3，并仅在真实、匹配的持有停止事件中
叠加 DR6 槽位命中与准确 RIP。数据 DR 在此回退模式仍交给 Windows。
TitanEngine 保留原生软件断点表及调试循环；内部断点读看到逻辑 INT3，前端
Safe/Unsafe 读看到原字节。TF 单步时临时撤销命中偏移，下一真实单步事件后重装。
普通原生 StepInto/StepOver 不经过硬件停止事件的延迟回调路径。

自动准备不能接管其他调用者的准备/常驻。活动断点阻止关闭 HVM；移除最后一项
后可释放自己的资源。驱动进程退出回调处理 owner/target 崩溃，停止该影子会话
的常驻并释放视图、MDL 和进程引用。目标映射替换/自修改代码仍要求重新安装逻辑断点。
实时验收及能力限制见 `docs/ksword-debugger-vm-validation.md`。

## C ABI v1

`unsigned long __stdcall KSwordDebuggerCall(KSWORD_DEBUGGER_CALL*)` 接收 48 字节
调用结构。`version=1`、`size=48`、`reserved=0`；两个指针编码为 `uint64_t`。
调用者提供输入/输出地址和容量；返回 Win32 错误码，同时填写 `error` 与
`bytesReturned`。外部缓冲区在持锁前复制，支持请求/响应共用地址；无效指针
返回 `ERROR_NOACCESS`。输入最多 4 MiB、输出最多 8 MiB。

| 命令 | 输入 | 输出 |
| --- | --- | --- |
| 1 QUERY_BACKEND | 空 | 40 字节 KSWORD_DEBUGGER_BACKEND_STATUS |
| 2 USE_HVM | DWORD 0 或 1 | 相同状态；失败时保持实际选择 |
| 3 OPERATION_LAYOUT | DWORD 命令编号 | 24 字节 KSWORD_DEBUGGER_OPERATION_INFO，含输入/输出长度和协议版本 |
| 16–32 | 对应 shared/driver 请求 | 对应 shared/driver 响应 |

布局查询保证适配器不会硬编码数千字节响应长度。请求必须是协议要求的精确
大小；输出容量至少为对应响应大小。原始协议调用的成功返回只表示传输及
长度正确，调用者还必须检查该协议响应的 `status`、`lastStatus` 和能力字段。
不要将不支持或验证失败的响应当作成功。

## 当前全部 HVM 协议入口

| C ABI 编号 | CE Lua 名称 | 能力 |
| --- | --- | --- |
| 16 | status | HVM 状态和能力 |
| 17 | control | 准备、自检、常驻启动/停止、释放及现有控制操作 |
| 18 | memory | 私有窗口状态、虚拟/物理内存操作 |
| 19 | eptRule | EPT 规则、监视及既有规则操作 |
| 20 | events | HVM 事件流 |
| 21 | view | 多视图与页面映射 |
| 22 | crPolicy | CR 策略 |
| 23 | msrPolicy | MSR 策略 |
| 24 | domain | 域管理 |
| 25 | process | 进程与模块快照 |
| 26 | inject | 现有 HVM 注入协议 |
| 27 | platform | 平台诊断 |
| 28 | metrics | 指标 |
| 29 | nestedProbe | 嵌套能力探测 |
| 30 | nestedPage | 嵌套页面协议 |
| 31 | breakpoint | 新增、删除、撤销、查询 EPT 调试断点 |
| 32 | native | 公共 R0 调试查询、上下文、挂起/恢复、分配/保护/释放 |

这些入口完整转发现有协议，保留它们的功能与限制；不会凭 API 存在就声明
平台已经支持某项 HVM 操作。具体操作编号、字段、确认标记和结果语义见
`shared/driver/KswordArkHvm*.h`、`KswordArkDebuggerIoctl.h`。

CE autorun 建立全局 `KSword`，只需调用公共 API：

```lua
local state, err = KSword.status()
assert(state ~= nil, tostring(err))
local ok, err = KSword.useHvm(true)
assert(ok, tostring(err))

local packet = KSword.hvm.packet('status')
local response, transportError = packet.send()
packet.destroy()
assert(transportError == 0, tostring(transportError))
print('HVM response bytes: ' .. #response)
```

`packet.write32(offset, value)`、`write64`、`writeBytes` 按共享头的字节偏移填写
参数。工厂已设置版本和结构长度；边界越界会拒绝。`send()` 返回响应字节表
及传输错误，使用后必须 `destroy()`。例如 `control` 的具体控制命令、代际
与确认标记仍由调用者按共享头设置；禁止凭猜测填写危险控制字段。

## CE 自动接管与 EPT

默认 R0 模式接管进程内存、64 位线程上下文、挂起/恢复、分配和保护。
CE Windows 调试接口的事件等待、继续和附加由公共后端管理，Windows 负责
事件传递；远程线程创建也仍使用 Windows。VEH、DBVM 或 SDK 之外的直接
系统调用不经过这组 hook。

选择 HVM 后，读写必须返回 `usedDirectWindow`，私有窗口未就绪或失效时
直接失败。它不会自动变回普通 R0 读写。

常规 HVM 调试可用时，CE 通过调试寄存器设置的执行/写入/读写断点转为线程作用域的
EPT 规则。仅匹配用户态、目标线程 TEB 和启用的 DR7 槽；读回上下文保留
前端期待的 DR0–DR3。VM-exit 产生 DR6 命中位与 #DB，CE 收到单步异常。

- 执行断点精确匹配 RIP，停止于指令执行前；继续时 RF 跳过该次重试。
- 数据断点是 **4 KiB 页粒度**，MTF 在访问指令完成后交付 #DB。不是字节
  精确的硬件数据断点，同页其他访问也可能命中。
- 常规自动 EPT 断点要求 Intel VMX、MTF 和新的 HVM_DEBUG 协议，AMD 返回
  不支持；普通 HVM 协议仍按自身平台能力运行。
- 常规执行断点不可用时走前述 ShadowPage 回退；数据 DR 保留 Windows 行为。
- 一个线程最多映射四个 CE 调试寄存器槽；驱动总计 128 个调试槽。
- 页面以 MDL 固定，线程对象保持引用。目标映射被重建时应重新安装断点。
  线程/调试器退出通知立即撤销命中资格；固定页面资源在停止常驻后的规则
  退休/释放阶段回收。
- 不与同一页的普通 EPT 规则重叠，避免调试规则改变已有访问控制语义。

## 所有权与失败恢复

自动断点会建立本调试器拥有的 HVM 准备/常驻会话。已有其他调用者的准备或
常驻资源返回 `ERROR_BUSY`，不会强行重建。添加/替换/删除规则先停止全 CPU
常驻，变更完成后恢复并确认全部 CPU 常驻。中途失败会尝试恢复原断点及
原上下文，恢复结果写入日志；常驻不完整时拒绝继续调试目标。

有自动 EPT 断点时，原始生命周期、EPT/视图/CR/MSR/域策略及原始调试寄存器
写入返回忙，防止绕过前端事务。先移除自动断点再执行这些操作。普通原始
API 建立的规则由原始调用者负责退休。

HVM 开关关闭前必须移除 EPT 断点；关闭时释放本后端拥有的资源。状态轮询
也观察 CE 直接调用的 Windows 分离操作，退休遗留断点与自有常驻资源。
写入与调试修改走驱动安全策略，R0 写入按现有协议设置 UI_CONFIRMED/FORCE；HVM 自检/常驻启动
按控制协议携带所需确认标记。停用 CE 插件恢复仍由它拥有的
函数指针，清理失败时拒绝停用；代码映射保留至 CE 退出，以保护尚在 hook
中等待事件的线程。

## 验证

`tests/BackendTests.cpp` 使用生产公共后端和模拟驱动，验证 R0 读写分片/确认
标记、HVM 严格私有窗口与禁止回退、进程请求权限保留、线程/CPL/DR7/RF
匹配、断点替换失败回滚、常驻启动拒绝、所有权、无效 ABI 指针与后续调用。
它不加载驱动或运行 VMX。

同一模型测试还验证原生会话重复采用/冲突、事件所有权、旧代次拒绝、逐线程
EPT 退休、待继续退出事件的租约、外部常驻保护，以及完整 Windows 上下文
writer 的失败回滚与 GPR/SIMD/扩展状态保留。扩展状态由模型 writer 模拟；
这不代替原生适配器的 Windows `CopyContext` 实测或驱动加载验收。

```powershell
python DebuggerBackend/tests/validate_lua.py --lua-dll 'plugin/cheat-engine/payload/Cheat Engine/lua53-64.dll'
```

Lua 检查使用 x64 运行库和模拟 CE API，执行真实 autorun 工厂、所有协议
入口、HVM 开关确认、卸载检测与重新连接，并确认 CE 颜色和字体未被修改。
CLI 的 `r0 debugger-status` 是只读的原生 R0 能力探针。
上述模型/Lua 检查不证明实机行为；另在 `KSword-HVM-Target` 完成了实际 GUI、
驱动加载、HVM 内存及 ShadowPage 隐藏执行断点验收，见
`docs/ksword-debugger-vm-validation.md`。
