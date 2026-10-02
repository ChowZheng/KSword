# x64dbg #3983 / #3974 API 适用性评估

核对日期：2026-10-01。本文保留对通用上游接口的建议，并记录后续 PR #3974 适配。建议的通用接口名称尚未加入必需 ABI；KSword 已通过可选私有命令实现引擎与断点机制查询。

## 结论

这些接口可以使用，KSword 当前已以这套接口制作 TitanEngine 代理。修复 GUI 断点安装失败后的错误记账，不需要新增 API；现有布尔返回值和 LastError 已能报告失败。

内存查询、分配、保护、释放与线程挂起/恢复已接入共享后端；实时会话仍保留原生 Windows 事件通道。转储与 TTD 使用 PR 配套 DbgEng 引擎。前端新增 KSword 专属菜单及实际安装机制查询；通用内存视图与独立断点 ID 仍是上游扩展建议。

## 上游与本地身份

- [Issue #3983](https://github.com/x64dbg/x64dbg/issues/3983) 讨论可选外部调试引擎。维护者的[指定评论](https://github.com/x64dbg/x64dbg/issues/3983#issuecomment-5930735895) 要求评估 [PR #3974](https://github.com/x64dbg/x64dbg/pull/3974) 的 API 形态；KSword 可以继续提供这套引擎接口的实现。
- 维护者在[后续评论](https://github.com/x64dbg/x64dbg/issues/3983#issuecomment-5932140104) 明确说明，内存映射等操作已经过 TitanEngine、引擎可以报告能力，目标是运行中动态注册更多引擎，动态注册尚未完成。Issue 后续提出了显示真实断点行为的菜单；建议让菜单使用引擎能力 / 断点结果，而非仅按 KSword 模式改名。
- 当前固定 x64dbg 基线：`de6fc34df7cb01edea28f40c8b5c49d37bc1c0fb`；原生 TitanEngine：`21ef77f31fd42d17f785802c36b2ca4ca44c43d7`。来源见 `X96dbgIntegration/PINNED_BASELINES.json`。
- 本次核对的 PR 最新 head 为 `de6fc34df7cb01edea28f40c8b5c49d37bc1c0fb`，仍 open / 未合并。最新提交调整 CI/测试，`TitanEngine.h` 与 `.def` 内容和固定基线一致。
- 从 GitHub API 读取最新头文件/导出表，与 `third_party/x64dbg_abi/` 归一化换行后逐字比较，二者均一致，规范导出仍为 64 个。PR 仍可能继续变化，不能把此结果当成之后提交的兼容保证。

## 已有接口可以承担哪些功能

| 功能 | 现有入口 | KSword 当前行为 / 接入方向 |
| --- | --- | --- |
| 内存读写 | `MemoryReadSafe`、`MemoryReadUnsafe`、`MemoryWriteSafe` | 公开读取保留原字节视图；策略写入可进入共享后端 / Shadow 执行补丁。现有入口可继续替换实现。 |
| 内存映射与分配 | `MemoryQuerySafe`、`MemoryAllocSafe`、`MemoryFreeSafe`、`MemoryProtectSafe` | HVM 实时模式接入共享后端；成功释放清理相应自有绑定。回放保持 provider 的权限与合成句柄语义。 |
| 进程、线程与句柄 | `TitanOpenProcess/Thread`、`TitanCloseHandle`、挂起/恢复、优先级、线程时间等 | 大多仍转发原生。PR 将前端直接访问 Windows 的部分操作移到引擎边界，方便其他后端实现。 |
| 暂停 | `RequestPause(MaximumPolicy, callback)` | 已有非侵入 / 常规 / 强制的策略上限；当前转发原生，策略切换仍要求真实持有停止事件。 |
| 软件、硬件、内存断点 | `SetBPX`、`SetHardwareBreakPoint`、`SetMemoryBPXEx` 及对应删除接口 | 足以安装、拒绝、回退并清理当前断点。硬件执行可映射至 EPT / Shadow INT3；原生数据 DR、PAGE_GUARD 与页级 EPT Watch 的语义不同。 |
| 上下文和单步 | `Get/Set*Context*`、`StepInto`、`StepOver` | 完整寄存器 / SIMD 状态沿原生，局部 DR 与隐藏断点按后端策略适配；无需为常规单步另加接口。 |
| 会话与能力 | `GetSessionInfo(TITAN_SESSION_INFO*)` | 返回当前原生 / DbgEng provider 的 live / dump / TTD 等会话及真实能力位。 |
| 调试事件 | `DebugLoop`、`SetCustomHandler`、`GetDebugData`、attach / detach | 保留真实 Windows 调试事件通道。现有事件历史 ring 不能替代当前真实停止事件。 |
| 回放 | `InitReplayW`、`Replay*` | 转储与 TTD 委托固定 DbgEng；不将 HVM 事件历史宣告为 TTD。转储已实测，TTD 导航缺有效录制文件。 |

本地入口：`TitanEnginePlugin/Provider.cpp`、`Forwarders.cpp`、`Proxy.cpp`、`NativeSeam.cpp`。HVM 公开内存读使用私有窗口原始视图；原生模式与完整 GPR/SIMD 上下文仍有 Windows 路径，不能把“已实现 64 个导出”解释成“全部操作已由 R0/HVM 执行”。

## 本次实现的可选扩展

`KSwordDebuggerCall` 命令 8 查询当前引擎、会话与策略；命令 9 查询已安装断点的实际机制、覆盖范围、槽位与逐绑定回退原因。查询读取实际记录，不以选择的模式推断成功；Shadow 命中期间区分逻辑保留与字节临时撤销。前端仅在实际加载 KSword 时显示菜单、断点 Type 和详情。当前 64 个必需导出保持兼容，R0 协议不变；以下通用独立 ID、能力通知和显式视图接口仍是建议。

构建、转储 provider 和虚拟机 UI 的证据见 [PR 验收记录](ksword-x64dbg-pr3974-validation.md)。

## 建议的通用扩展

### 1. 断点能力、创建结果与状态查询（优先）

建议可选 `QueryBreakpointCapabilities`、`CreateBreakpointEx` / `QueryBreakpointInfo`，使用版本、结构大小、独立逻辑 ID，返回：

- 请求类型和实际机制：原生 DR、原页 INT3、Shadow INT3、EPT、PAGE_GUARD。
- 地址覆盖、长度、页粒度、对齐、线程 / 目标作用范围和可用数量。
- 是否允许回退、实际回退原因、成功 / 拒绝及错误码。
- 逻辑已启用与物理暂时撤销分别报告：真实停止期间暂时撤销 INT3，不能显示成断点已删除。

现有硬件接口和前端记账采用 DR0–DR3 槽位；驱动可以持有更多 EPT 规则，但不能将规则数量冒充更多真实 DR 寄存器。超过四槽需要前端支持独立逻辑 ID。

`UE_SESSION_CAP_LOGICAL_CODE_BREAKPOINT` / `DATA_BREAKPOINT` 无法表达上述细节。当前 x64dbg 在会话开始缓存 `gSessionInfo`，`dbghassessioncapability` 读取缓存；只改变 DLL 返回值仍不足以更新已运行的 GUI。应配套能力 generation / 变更通知或明确的刷新入口。

### 2. 内存视图读写（用于验证 Shadow）

建议可选 `MemoryReadEx` / `MemoryWriteEx` 或统一扩展表，显式指定 Original、Execution、DebuggerLogical 视图；写入同时区分代码补丁与普通数据意图。

Original 保持普通数据读取语义；Execution 才能查询当前执行映射中的 `CC` / 补丁；DebuggerLogical 满足原生断点记账。执行视图不可查询时应报告不可用，不能按模式或 GUI 名称合成一个 `CC`。

Shadow 执行代码补丁并不自动修改普通数据读取，因此这种扩展也可以避免将 CE 数值冻结误称为 Shadow 隐藏数据修改。

本次 CE GUI 测试还发现一个实际例子：[CE SetValue 源码](https://github.com/cheat-engine/cheat-engine/blob/master/Cheat%20Engine/MemoryRecordUnit.pas) 会临时将普通数据页改为 RWX，再写入并恢复属性。仅在写入时查询 Protect 会误判为代码。当前修正在 CE 适配器内验证同线程的原始属性事务，并用内部 C++ 数据意图传入共享后端；未新增 DLL 必需导出。对本来就是 RWX 的混合页面，通用显式意图接口仍是正确方向。

### 3. 版本化引擎扩展与配置

建议 `GetEngineExtension(version, size, extensionId)` 或可选导出，供前端发现配置 / 诊断接口。HVM 开关、Shadow 策略、允许回退、页预算属于此层。

KSword 已有私有 `KSwordDebuggerCall` 的 GET/SET_OPTIONS、QUERY_POLICY 和 HVM 命令，可以继续服务 CE 与其他前端。若推动上游，应采用通用能力描述，不让所有引擎必须实现 KSword 专用函数。

保持现有 64 个必需导出兼容；扩展按版本发现，缺失时使用现有行为。`SetEngineVariable` 当前是固定枚举和 bool，不适合承担所有结构化配置。

## 为何这套 API 尚不能消除原生适配

PR 抽象了许多前端 Windows 调用，但我们目前仍使用原生 Titan 的 DebugLoop。原生 DLL 内部的 Wait/ContinueDebugEvent、Get/SetThreadContext、Read/WriteProcessMemory 六个调用点仍由 `NativeSeam` 适配，新增高层导出不会自动替换这些内部操作。

如继续保留原生循环，可讨论可选的内部 provider 回调，以减少 IAT 适配；如独立实现整个引擎，也能沿已有 DebugLoop 接口工作。两种路线均不要求前端新增一个强制的 KSword 调试循环 API。

当前隐蔽模式隐藏执行断点的代码修改，Windows debug port / 事件通道仍存在。PR 的抽象和可选接口都不能单独证明目标无法发现调试器。

## GUI 故障与 API 的关系

本次源代码修复位于 `X96dbgIntegration/patches/x64dbg-ksword-engine.patch`：硬件 / 内存安装失败删除刚创建的前端记录；启用全部硬件断点失败恢复 Disabled 和原槽位信息。Titan 的冗余恢复字节不再创建持久 Shadow 写入页。

这些修复已使用真实 GUI 操作验证，见 [VM 验收记录](ksword-debugger-vm-validation.md)。扩展 API 负责表达机制和能力，不能替代前端处理当前已有的安装失败返回值。
