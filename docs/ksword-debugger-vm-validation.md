# CE / x96dbg 虚拟机验收（2026-09-30）

## 环境与范围

实际执行位置为 `KSword-HVM-Target`：Windows 11 Pro 22621.4317、2 vCPU、
8 GiB、Hyper-V 嵌套虚拟化，控制台 Session 1。来宾启用测试签名，内部
Hyper-V/VBS 关闭。测试仅加载来宾驱动，宿主未加载驱动或启动 KSword HVM。

来宾测试目录为 `C:\ksword\debugger-tests\20260930`，使用独立 CE/x64dbg
载荷、会话和受控 `DebugTarget.exe`。没有附加无关应用。主程序、CE 和
x64dbg 的真实 GUI 均实际启动；原生全量回归通过 x64dbg headless 调试循环运行。
构建统一使用 AMD64 MSBuild/HostX64，并传入三项架构属性。

本机嵌套 VMX 能力不含 MTF（`0x01000000`），常规 HVM_DEBUG 执行断点
协议报告不可用。验证的自动回退是 **EPTP 切换的 ShadowPage 隐藏 INT3**：
执行视图包含 INT3，原页保留原指令，不要求 MTF；不是绕过能力检查启用常规路径。

## 最终测试产物身份

以下产物已逐文件传输并核对 SHA256，最终 GUI/原生回归使用同一新版代理和启动器。

| 产物 | SHA256 |
| --- | --- |
| `KswordARK.sys` | `2C0CA1A4A1E9F822DC7795F9C31D6B6737E3F02FE476D93B9A06B7DDCB0C4146` |
| CE `bridge/x64/KswordCheatEnginePlugin.dll` | `77531D845C71F25674CC6C018B882B20CA322E9AD09A6A43B065958EF0BC04E3` |
| `KSword/TitanEngine.dll` | `F414DC06512855F65DE50E5AEFB88182502E40B952BC1CCB9705920539D1AB83` |
| `KswordCheatEngineLauncher.exe` | `D733535D3A8FC04C5F3D9E61C07DDDFCD52444D5AB325C11C511109EB6445695` |
| `KswordX96dbgLauncher.exe` | `D0F9E62EC000D3CE59C5221F751A3D7FC7C5EF2A1FE568363B819D01FA5B49F3` |
| `DebugTarget.exe` | `1217AC4437C0295831427EA96A2D8C40D13B478BCDF0CFC51FE99541DD888FE9` |

驱动完整 WDK 构建、x64 ApiValidator（`Driver is 'Universal'.`）及 CAT
Signability 均通过；来宾加载的是对应测试签名驱动。主程序使用本轮已有的
Release 产物及实际 Qt/ADS 依赖，主程序源码没有为测试而修改。

## 实测矩阵

| 路径 | 结果 | 实际覆盖 |
| --- | --- | --- |
| CE R0 内存 | PASS | 打开目标、读写、分配、修改保护、释放；Qword 首次扫描及值变化后的再次扫描 |
| CE HVM 内存 | PASS | 严格私有窗口读写、首次扫描和再次扫描；显式切回 R0 |
| CE 原生硬件执行断点 | PASS | Windows 接口、准确 RIP、寄存器修改、单步一条指令、再次命中、移除、分离及目标退出 |
| CE HVM 硬件执行断点回退 | PASS | 不修改 CE 的硬件断点请求，自动 ShadowPage；读取仍为原字节；准确 RIP、修改 RAX、单步、两次命中及退休 |
| CE HVM 软件 INT3 | PASS | 显式 `bpmInt3`，自动隐藏执行视图；读取原字节、单步、两次命中、移除及清理 |
| CE 标题三态 | PASS | 驱动不可用 `KSword Connected`，默认 `KSword R0`，选择后 `KSword HVM`；保持 CE 的独立窗口与原界面 |
| CE 驱动不可用 | PASS | 不安装 15 个 hook，保留原生读写；桥接状态可查询，HVM 未被虚假启用 |
| Titan 原生实时循环 | PASS | 真实 attach/DebugLoop、硬件执行、准确线程/RIP、GPR/flags/x87/SSE/AVX、RAX 编辑、精确单步、两次命中和 detach |
| Titan HVM 自动回退 | PASS | 替换槽地址/回调、同页两断点合并及删除一个、隐藏字节、真实 Windows 停止、完整上下文、精确单步、重复命中、删除/分离/释放 |
| x64dbg 实际命令层 HVM | PASS | 前端 `attach` / `bphws` / `mov rax` / `sti` / `run` / `bphwc` / `detach`；前端表达式读原字节、准确 RIP/修改后的 RAX、两次命中及分离后目标存活 |
| Titan 模式保护 | PASS | 活动执行断点时禁止关闭 HVM；移除后关闭并释放自有常驻；通用 HVM 数据硬件断点明确返回错误 50 |
| x64dbg 完整原生回归 | **85/85 PASS，2 SKIP** | 全部 87 项发现的上游测试；软/硬件/内存断点、跨页/同页删除、多次命中、线程、脚本、跟踪、命令解析、生命周期等 |
| ShadowPage 崩溃清理 | PASS | owner 与 target 分别终止后，视图归零、两颗 vCPU 常驻归零，释放固定页资源；后续可再建会话 |
| 实际 PluginHost Tabs | PASS | CE/x96dbg 独立顶层 GUI、日志页嵌入、真实日志转发、Consolas、每个开关连续 `1→0→1→0` 并收到实际确认 |
| 日志页关闭 | PASS | 关闭两个实际日志 surface 后，两个调试器仍存活；随后才清理本轮客户端 |
| 公共 HVM 协议 | 16 个传输/布局检查通过 | 命令 16–28、30–32，响应长度与协议版本正确；没有把内部拒绝/未准备状态当成功操作 |

回归跳过的是 `replay_minidump` / `replay_ttd`，原因均为
`unsupported_engine_KSword`。普通硬件执行/写/读写及原生软件/内存断点都
包含在 85 项通过的原生测试中。命令解析测试单项通过 822 个断言。

Titan HVM 回退实时 fixture 通过 35 条断言，命中 2 次；原生实时 fixture
通过 26 条断言，命中 2 次。步骤使用私有代码页：3 字节 `mov` 后的
3 字节 `inc`，断点停在 `inc` 前，RAX 从 `0x100` 修改成 `0x1234`；
StepInto 后 RIP 前进 3 字节、RAX 为 `0x1235`，继续后目标存储该值。
再次命中后移除断点，目标正常完成并可退出。

## 测试中修复的问题

1. CE 的进程内 Lua/ABI 缓冲区误走目标驱动内存策略，造成错误 1306；
   本地自读写改用 Windows 原接口，目标读写仍走公共后端。
2. R0 写入缺少驱动现有协议要求的 FORCE 确认；与 UI_CONFIRMED 一并补齐。
3. 来宾 R0 上下文操作返回 `0xC0000001` / Win32 31，挂起计数导出缺失。
   后端明确记录后使用 Windows 上下文/挂起接口，保留计数及完整上下文。
   这不表示 R0 上下文实现本身通过；权限/身份失败不触发回退。
4. CE 隐藏执行 DR 的停止事件没有正确暴露命中 DR6；仅对实际持有且匹配
   的 ShadowPage 停止叠加槽位命中及 RIP，避免把历史事件当成停止。
5. Titan 硬件停止中的 StepInto 回调过早完成；通过真实事件代次等待下一
   单步。该延迟只能用于自有硬件停止，扩大到普通原生步进曾使 4 项回归
   超时；收紧后，4 项定点回归及最终全量 85 项均通过。
6. x96dbg 控制文件读取句柄跨越轮询等待，阻止第二次原子替换；公共 reader
   使用读/写/删除共享并立即关闭，两个 Tab 的连续切换实测通过。
7. Titan 单步完成时仍持有策略锁调用前端暂停回调；真实 x64dbg 回调等待 Run，
   而命令层上下文查询需要同一锁，造成卡住。完成私有记录后先释放策略锁，
   再调用前端回调；实际命令层全流程随后通过。

## 证据和复跑入口

生产测试源码位于 `tools/debugger_vm_test/`：

- `ce_live.lua`：真实 CE、扫描和重复执行断点；`KSWORD_TEST_USE_HVM=1`
  选择 HVM；`KSWORD_TEST_BREAKPOINT_METHOD=int3` 测软件路径，默认硬件请求。
- `ce_connected.lua`：桥接连接但驱动不可用的原生能力及标题。
- `titan_live.py ROOT native|hvm`：真实 TitanEngine 调试循环和上下文。
- `x64dbg_hvm_live.py ROOT`：真实 x64dbg 命令处理、HVM 自动回退及暂停时上下文查询。
- `shadow_live.py` / `shadow_cleanup.py`：底层执行视图和进程死亡清理。
- `gui_tabs.ps1` / `gui_close.ps1`：在来宾交互会话验证真实日志页和独立窗口。
- `protocol_probe.py`：公共布局/协议查询和 R0 上下文诊断。

主机实际证据存于 `.codex-build-logs/debugger-vm/`：
`native-suite-pass.log`、`titan-native-pass.log/json`、`titan-hvm-pass.log/json`、
`ce-native-scan-pass.log`、`ce-shadow-scan-pass.log`、`ce-shadow-software-pass.log`、
`ce-connected-pass.log`、`gui-tabs-pass.log`、`gui-close-pass.log`、
`shadow-cleanup-pass.log`、`protocol-probe-pass.log`，以及对应实际 GUI 截图。
实际命令层另有 `frontend-hvm-pass.log` / `frontend-hvm-commands-pass.log`。
来宾保留完整 `native-results/` 断言/目标产物。

此外通过公共后端模型、真实 Lua 启动脚本测试、64 个规范 ABI 声明/导出
检查，以及真实原生 DLL fixture（LastError、内存、上下文大小、文件会话
确认、缺失导出拒绝）。这些辅助测试与实时证据分别记录，不互相替代。

## 能力边界

- **原始 MTF/EPT 调试路由在此 VM 不支持**；通过的是无 MTF 的隐藏执行断点
  回退，不能写成原始路由通过。
- Titan HVM 通用写/读写硬件断点明确不支持；原生模式对应测试通过。
  显式 Watch 是页粒度诊断，不能冒充字节精确断点。CE 回退模式的数据 DR
  保留 Windows 行为，此次 CE 实时 fixture 只验收执行断点。
- 公共 HVM 接口全部接入并进行了 16 项传输/布局检查；注入、CR/MSR/域策略、
  嵌套页面等每个变更操作没有在本次调试器验收中逐一执行。命令 29 的嵌套
  探测未执行，不能把查询通路通过描述为所有变更功能通过。
- 目标映射替换/自修改代码要求重新安装逻辑断点；本轮实际目标使用私有代码页。
  这不是任意目标、多前端共享常驻或通用不可检测性的证明。
- 不包含 AMD 实机、x86/WOW64 HVM、CE VEH/DBVM、回放/逆向执行的实测。
- 关闭 KSword 主程序会执行其现有驱动关闭流程；“日志页关闭保持调试器”
  的验收专指日志页自身。测试后仅清理自己的客户端/任务，VM 保持运行。

## 后续策略与 Shadow 代码补丁（2026-09-30）

本节记录上述验收之后新增的策略功能及其当前证据。前面的产物哈希、
85/85 原生回归和 GUI 结果属于前一轮，不能作为本节修改后产物的验收身份。

### 已实现的接口与约束

- 公共 C ABI 增加命令 4–7：读取选项、设置选项、查询实际策略、恢复
  Shadow 内存补丁。版本 1 选项为 48 字节，策略状态为 72 字节；旧调用
  布局和默认行为保留。选项包含常规/隐蔽模式、Shadow 代码写入、明确
  回退、原生上下文/挂起回退及最多 1–32 个 Shadow 页。关键回退日志
  必须启用；离线查询/设置不初始化驱动，失败返回实际保留的选项。
- 常规 HVM 保留现有执行断点自动回退；隐蔽模式优先 Shadow 执行视图，
  不以真实代码写入、可见 INT3 或真实执行 DR 作为失败回退。非可执行
  数据页在允许回退时可使用有效的普通数据写入，常规/隐蔽模式均明确
  记录原因和完成结果、实际字节数；禁用回退时明确拒绝。
- CE 的 Shadow 代码写入默认关闭，可显式启用。Shadow 修改的是
  **可执行页的执行视图**，普通数据读取仍为原字节；RX/RWX 保护无法
  证明写入意图是代码，RWX 页也可能承载冻结数据。因此不能把该选项
  说成通用数据写入/冻结隐身，每次接受补丁均记录执行视图与数据读取
  的区别。代码 Shadow 写入需要 HVM；失败不转为原代码写入。
- 代码补丁与隐藏 INT3 均接受 `MEM_PRIVATE` 或已 COW 私有化的
  `MEM_IMAGE` / `MEM_MAPPED`；锁页后要求 working-set Valid=1、Shared=0，
  并复查虚拟地址对应 PFN。Windows 在 COW 后仍报告原映射类型，类型
  本身不能证明共享或私有。仍共享的页面拒绝，错误 50 日志明确要求
  Windows COW-private backing；不自动向运行中的目标写回原字节来
  触发 COW，以免覆盖并发修改。执行视图按物理页生效，
  不提供 PID/CR3 隔离保证。单次代码写入限制为同页且最多 1232 字节；
  超限、跨页和代码/数据混合范围在修改前拒绝。
- 同页内存补丁与 INT3 分层合并，INT3 优先；恢复内存补丁保留断点。
  原生协议操作 10/11 写入/恢复，操作 12 标记内容恢复隔离。更新失败
  回滚旧字节/掩码；回滚无法完成时，适配器和驱动直接 START 均拒绝
  恢复常驻，只有完整成功恢复才能清除隔离。
- 驱动 START 先取得 Shadow 共享锁租约并校验所有 owner 的视图/隔离，
  再取得 HVM 生命周期锁，统一出口按相反顺序释放；校验与启动之间
  不允许 Shadow 更新。完整恢复、owner/target 退出和卸载清理相应
  引用；部分恢复不清除隔离。本轮静态审查未发现该租约/清理路径的
  新阻塞问题，不替代运行时错误注入验证。
- Shadow 上下文准备失败发生在修改前时直接返回真实错误，不执行
  STOP、TEARDOWN、上下文写入或假回滚，不接管外部 prepared/resident。
  真正发生修改后的失败继续执行恢复。重复错误日志按独立原因合并，
  首次及累计 10/100/1000 次输出；每次回退仍计数并保留实际错误 31
  等状态，日志操作保留调用方 `LastError`。
- 常规 HVM 缺少精确数据断点能力、且允许回退时，仅数据 DR 的上下文
  使用明确记录的 Windows 原生寄存器路径。设置、替换、清零、继续和
  退休均不准备或暂停外部 HVM；清零后解除策略占用。隐蔽模式或禁用
  回退时不写可见数据 DR。该生命周期已在模型及真实 CE 数据观察
  断点验证；这仍是可见原生数据 DR，不能称为隐藏数据断点。

### 当前验证状态

| 检查 | 结果与范围 |
| --- | --- |
| 公共后端模型 | PASS；含 COW 接受/共享或过期 PFN 拒绝、准备失败无修改/假回滚、真实失败恢复、错误 31 计数/日志合并、仅数据 DR 设置/替换/清零/继续/退休及外部 HVM 保留 |
| 驱动构建 | 完整 WDK x64 Build PASS，0 警告/错误；x64 ApiValidator Universal 和 CAT Signability 通过 |
| CE/Titan 策略适配构建 | Release/x64 PASS，0 警告/错误；Titan 暂停时逻辑路径修复的产物已通过本轮策略实时复验 |
| 安装布局 | 8 个模型用例 PASS；实际 VM 受控插件市场安装 x96dbg PASS |
| 主程序 | 本轮构建退出码 0，i18n 审计 26697 条源字符串 PASS；用户运行目录已部署新版并核对哈希，该目录的市场操作待最终复测 |
| 策略实时测试 offline | 29 项 PASS；离线选项及实际 ACK |
| 策略实时测试 memory | 130 项 PASS；执行补丁 `0xff`、普通读原字节、恢复执行 `0x101`，数据回退与无半写入拒绝 |
| 策略实时测试 image-cow | 109 项 PASS；真实模块代码 Shared=1 拒绝 50，显式 COW 后仍 MEM_IMAGE 且 Shared=0，补丁仅目标执行变为 102，两进程普通读取/另一进程执行保持原始 101，恢复为 101 |
| 策略实时测试 normal-native | 55 项 PASS |
| 策略实时测试 normal-hvm | 59 项 PASS |
| 策略实时测试 stealth-hvm | 82 项 PASS；恢复内存补丁后路径保持 Shadow、逻辑断点数为 1，第二次真实隐藏 INT3 命中通过 |
| 策略实时测试 stealth-no-hvm | 26 项 PASS；明确拒绝可见硬件 DR 回退，无已安装绑定且原生会话正常退出 |
| 最终生产代理原生兼容 | PASS；真实 native64 ABI、内存/上下文/LastError、控制协议和依赖拒绝，不打开驱动 |
| 实际 CE 数据观察断点 | 147 项 PASS；真实写断点 #DB、物理 DR 清除、第二次写入不再命中、外部 HVM 全程保留 |
| 最新生产插件 GUI | 315 项 PASS；真实双 Tab 控件/实际 ACK、错误选项回退、新会话加载保存选项、独立窗口、Consolas 日志及原偏好恢复 |

首轮策略总结果为失败。该隐蔽模式问题来自原生
Titan 循环在前端回调前临时移除物理 INT3，而状态只观察已安装视图；
适配器现按已持有逻辑绑定补齐路径报告。此前六阶段实时复验全部通过，
日志包含两次 `activePath=2`、`shadowWritePages=0`、`activeBreakpoints=1`
的暂停中恢复状态，以及恢复后的第二次真实命中，确认重新安装有效。
复验前显式恢复了 KswordARK 服务；本结果以最终成功轮次为准，不把
服务不存在时的 readiness 错误 2 算作策略功能验证。原始 MTF/EPT
路由不支持及其他前述能力边界继续适用。此后纳入 COW 模块代码与
上下文准备修正的七阶段实时测试合计 **490 项 PASS**，结构化结果
`policy-cow-result.json` 为 `passed=true`，运行日志末尾为 FINAL PASS。
该轮结果早于随后常规 HVM 仅数据 DR 上下文小修正。该修正随后完成
并通过最终模型；使用新 CE/Titan 生产产物再次运行七阶段，日志含
490 条 PASS 和 `FINAL PASS policy regression`，已取回的结构化报告
`policy-final-cow-data-result.json` 为 `passed=true`，各阶段计数与上表一致。
新插件在用户运行目录的最终 GUI 随后通过独立验收，详见下节。

真实 CE 随后完成独立数据观察断点验收：8 字节写断点通过 Windows
`EXCEPTION_SINGLE_STEP` 命中，DR6 为数据槽命中而非 TF 单步，停止时
RIP 位于数据 store 后、RAX 和实际数据均为 `0x101`。清除后另行暂停
直接读取原始 Windows 上下文，DR0–DR3=0、DR6=0、DR7=0；后端绑定为
0、`canChangeOptions=true`。第二次真实 store 的完成计数为 2，但断点
回调仍只有 1 次。整个附加/设置/清除/继续/分离期间，外部 HVM
generation=81、prepared=2、resident=2/2 不变，调试器没有持有任何
EPT 规则或 Shadow 视图。测试所有者随后 STOP/TEARDOWN 均返回 0，
prepared/resident=0、generation=83。

此过程中 CE 的 `debug_getBreakpointList` 暂留一个已删除断点地址；
其 `getBreakpointAddresses` 枚举全部内部记录，`RemoveBreakpoint`
先禁用并标记删除，由空闲清理稍后释放。因此不能以立即非空的 Lua
列表判定物理断点仍然活动；上述原始 DR、后端状态及第二次写入无命中
共同确认已清除。此检查未修改 CE 的内部记账行为。

### COW 与上下文准备修正的中间产物

以下 SHA256 对应先完成的 COW 七阶段成功轮次；最终上下文修正产物
身份另列于下，前一轮基线身份保持不变。

| 产物 | SHA256 |
| --- | --- |
| `KswordARK.sys` | `65545A14F9F62DA21C0595EF7FB6274589254436A26AA7B0C033E3C9A3BAA250` |
| CE bridge | `6A5B25F8352564D5ED8EAEEDEE69DB14F75C5685945BC8361BB273FBEDC26E05` |
| Titan proxy | `D85AA8C9C6272B1783E82C23DA19FAF1E7D4866AC39B478C1CBDA31A364ECFFA` |
| `DebugTarget.exe` | `DC85BF561022A1234A45E0D128A793A25F02E3F2707691BC825DBD63A75D4B51` |

### 最终数据上下文修正产物

以下产物用于七阶段生产 VM 重跑及后续生产 GUI 验收；驱动与目标身份
沿用上表。GUI 测试的初始化顺序修正没有更改产品代码或这些 DLL 身份。

| 产物 | SHA256 |
| --- | --- |
| CE bridge | `1C569800FF2F472D5660B7840C5C47DA8C7A9A5483576EDDBEC95C93D9750600` |
| Titan proxy | `36CC7773A4EFBA33AB6AFDB4ECF3D0F703D69C2BF489EA2A37F9A15F2BEDD6C1` |

本轮证据：`.codex-build-logs/debugger-policy-model-build.log`、
`debugger-policy-model-results.log`、`debugger-policy-driver-build.log`、
`ce-final-shadow-policy-build.log`、`titan-final-shadow-policy-build.log`；
安装模型见 `.codex-build-logs/plugin-install-layout/run.log`，实际安装见
`.codex-build-logs/debugger-vm/plugin-install-vm-pass.json`。策略首轮完整
结果为 `debugger-vm/policy-result-first.json` / `policy-stealth-first.log`；
最终证据为 `.codex-build-logs/debugger-vm/policy-final-run.log`，末尾为
`FINAL PASS policy regression`；修复后的 Titan 构建日志为
`.codex-build-logs/x96-policy-titan-route-build.log`。复跑入口为
`tools/debugger_vm_test/policy_live.py`。主程序构建/i18n 日志为
`.codex-build-logs/ksword-build-check-20260930-205354.raw.log`；受控安装
成功不能替代用户运行目录更新之后的最终市场操作复测。

七阶段新证据为 `.codex-build-logs/debugger-vm/policy-cow-ready-run.log`
及 `policy-cow-result.json`；模型结果为
`.codex-build-logs/debugger-image-cow-model-results.log`。对应构建日志为
`debugger-image-cow-model.log`、`debugger-image-cow-ce.log`、
`debugger-image-cow-titan.log`、`debugger-image-cow-driver.log` 和
`debugger-image-cow-target-final.log`，均位于 `.codex-build-logs/`。

最后数据上下文修正后的实时重跑为
`.codex-build-logs/debugger-vm/policy-final-cow-data-run.log`（490 PASS、
FINAL PASS）及 `policy-final-cow-data-result.json`（passed=true）；模型/构建见 `debugger-final-model-results.log`、
`debugger-final-model.log`、`debugger-final-ce.log`、`debugger-final-titan.log`。
驱动关闭的真实原生代理兼容证据为 `debugger-final-proxy-results.log`；
这些辅助日志均位于 `.codex-build-logs/`。

真实 CE 数据断点证据为 `.codex-build-logs/debugger-vm/ce-data-live.log`
（147 条 PASS、FINAL PASS），外部 HVM 生命周期见同目录
`ce-data-hvm.log`；复跑入口为 `tools/debugger_vm_test/ce_data_live.lua`。

### 最终生产 GUI 验收

`.codex-build-logs/debugger-vm/gui-tabs-final.log` 包含 **315 条 PASS**
及 FINAL PASS。测试直接使用用户运行目录中的真实 KSword、两个
PluginHost 日志页和独立 CE/x64dbg 顶层窗口，确认实际 Qt Tab 被选中、
控件可见可用、日志转发及 Consolas 字体。

两页的六个切换项和页预算均验证实际请求/ACK；出站 revision 严格
大于前一确认值，隐蔽模式强制有效 Shadow 并禁用对应切换控件。
无效页预算和原始错误选项包均保留最后成功选项及已保存配置。另行
启动真实新 CE/x64dbg 会话，确认保存的自定义选项重新加载且 HVM
仍关闭。最终绑定数和 Shadow 补丁页数均为零，测试恢复原偏好文件。

结构化 `gui-tabs-owned-processes.json` 为 Outcome=PASS、
FreshSessionReload=true、OriginalPreferencesRestored=true；VM 任务
TaskLastResult=0。首次 GUI 失败是 fixture 在重置保存的 HVM 选择前
假设其已关闭，而且 CE action 1 仅修改 options；修正为先 action 0
关闭 HVM 并取得实际 ACK，再 action 1 设置常规选项。本次修正仅
调整测试初始化/结束顺序，不涉及产品代码或 DLL 更新。

此前基线 GUI 结果继续保留，本节是新增选项及最终产物的独立验证。
GUI 通过不替代之后重新生成源码 ZIP/插件包及最终重新部署、重开
用户窗口的产物核对；后者由主任务完成并单独记录。

## 2026-10-01 GUI 断点修复与 x64dbg UI 验收

x64dbg 的 `cbDebugSetHardwareBreakpoint` 在 `BpNew` 后调用引擎；原始
实现没有在引擎失败时撤销记录。GUI 会保留已启用的无效条目，再次
设置直接返回“already set”，不会重新安装。内存断点也存在同类问题。
补丁现在在硬件 / 内存安装失败时删除本次记录、刷新 GUI 并保留引擎
错误码；启用全部硬件断点失败时恢复该项 Disabled 和原始槽位信息。
软件断点原本已有安装失败回滚。

实际 UI 单步又发现 Titan 对已暂时撤销的隐藏断点重复恢复原字节，
会误创建持久 Shadow 写入页，删除断点后仍阻止策略切换。现在仅对
原生内部逻辑写、当前自有隐藏绑定、单字节非 CC 且已匹配的恢复操作
报告成功，避免创建代码补丁；用户主动写入仍遵循原有策略。

共享后端增加实际 Shadow INT3 安装/移除日志，包含 PID、VA、原物理页
和 view ID；Titan 隐藏执行断点回调记录真实事件的 PID/TID/VA/异常码。
临时移除物理 INT3 与保留原生引擎逻辑断点分别表述。

源码补丁对固定上游 index 的正向检查、工作副本的反向检查均通过；
构建脚本使用 `--ignore-whitespace` 兼容上游 `-text` C++ 文件的 CRLF
与发行补丁的 LF。Release/x64 构建及虚拟机传输哈希核对通过：

| 文件 | SHA256 |
| --- | --- |
| x64dbg.dll | `98627DE595ED4CBA595BB88777389C6B8EA3B5FDD4257E1C539F0C9B1A510700` |
| TitanEngine.dll | `DDD39FC65E008AEC2052484182C2FA0FD7B4C51EF6F7DC060F1030439CF1A8E0` |
| KswordCheatEnginePlugin.dll | `BB7D0A72BAEE258284DB371C986FAC691A698D1A324E7D9240C6813C317B83B6` |
| GUI 演示目标 | `5000F229ECBC2E141EC76F14D0C48BEE1B571E08BB05B45D2D0D2A00145EF16D` |

以上是开发验收候选，已部署到 KSword-HVM-Target 并同步已安装载荷的
对应哈希记录，尚未发布。驱动仍是前节已验证的 `65545A14...`。

`tools/breakpoint_demo/breakpoint_demo_gui.c` 提供 Increment 按钮、PID、
执行/数据地址、原字节及计数显示；独立代码页先通过 Windows COW
私有化。GUI 测试程序已放到虚拟机桌面的 BreakpointDemo-Shadow。

最初普通权限 computer-use 无法操作管理员 VMConnect。用户随后指定
PowerShell 直接控制光标，因此通过 PS Direct 部署当前登录桌面的提升
权限交互 worker，使用真实 SetCursorPos / 鼠标 / 键盘输入。断点安装、
命中、单步、禁用 / 启用 / 删除与目标触发均走实际界面；未用 Lua、
headless 或后端调用替代验收动作。只读日志和加载模块哈希用于补充证据。

### x64dbg 实测结果

目标 PID=10464，独立私有 COW 代码页入口 `00007FF7472B1000`，4 字节
计数地址 `00007FF7472C5D60`。写入指令在入口 +8，ret 在入口 +E。

| UI 场景 | 观测结果 | 截图序号 |
| --- | --- | --- |
| 隐蔽硬件执行：安装 / 实际命中 / 单步 / 重复命中 | 停在入口；F7 到 +6、F8 到 +8；再次按钮触发再次停止 | 102–111 |
| 隐蔽硬件执行：禁用 / 启用 / 删除 | 禁用后计数推进，无命中；启用后再次停止；删除后计数推进 | 40–55，首次候选轮次 |
| 共享映像页拒绝及再次尝试 | 错误 50 后表为空；再次尝试到达引擎，没有 already set 短路 | 64–69，首次候选轮次 |
| 常规 HVM 硬件执行 | 缺原始 EPT 事件能力时明确回退 Shadow，真实停在入口 | 125–133 |
| 常规 HVM 软件 F2 断点 | 隐藏 INT3 实际命中；F7 到 +6；删除成功 | 136–144 |
| HVM 关闭的原生硬件执行 | 真实命中；删除成功 | 154–161 |
| HVM 硬件数据写断点 | 明确拒绝 50；不留下已启用记录，不静默安装可见 DR | 173–174 |
| 原生 DWORD 硬件写断点 | 在 store 后 +E 停止，两次命中；删除成功 | 185–193 |
| 隐蔽内存断点失败回滚 | 失败后菜单仍显示可添加项，两次请求均到达引擎，表为空 | 268–273，最终 x64dbg.dll |
| 常规 HVM 内存写断点 | 明确 PAGE_GUARD 回退；两次停在 store +8；待写值 12 / 13，内存 11 / 12；删除后计数 14 且无命中 | 297–313，最终 x64dbg.dll |
| 启用全部硬件断点的失败回滚 | 先禁用原生数据 DR，切至隐蔽 HVM 后 Enable all 被拒绝；条目保持 Disabled，后端活动绑定 0 | 323–334，最终 x64dbg.dll |
| 最终候选隐藏执行断点再验证 | 安装成功，计数 14 时真实停在入口；F7 到 +6；删除 / 继续后计数 15，再触发计数 16 且无命中 | 342–357，最终 x64dbg.dll |
| Titan 冗余恢复清理 | 删除后断点 0、Shadow 写入页 0；首次修复后可直接切换模式 | 107–117、349–359 |

序号对应 `.codex-build-logs/debugger-vm/ui-evidence/ui-NNNN.png/json`。
首次候选使用 Titan `FCE2E242...`，后续使用最终 Titan `DDD39FC6...`；
序号 1–232 使用 x64dbg `B5604C30...`，之后换成最终 `98627DE5...`。
表中标出最终候选上的新增失败回滚验证，早期覆盖不冒充最终候选全量重跑。

最终加载模块和磁盘文件均核对最终 SHA256，见
`.codex-build-logs/debugger-vm/ui-final-runtime.json`。真实转发日志取回为
`ui-final-backend.log`，控制 ACK 为 `ui-final-backend.log.state`；操作记录
为 `ui-actions.jsonl`，均在同目录。证据目录为本地忽略项，发行源码不附带
这些运行截图；本表保留可复核的关键值和证据身份。

最后 detach 保持目标与独立 x64dbg 窗口运行；KSword Tab 显示活动断点 0、
Shadow 写入页 0。交互 worker 正常退出，专用任务
`KSword-Desktop-UI-20261001` 已删除，TaskLastResult=0。

### 本轮边界

- 这台嵌套 Intel VM 不提供原始 MTF 通路，实测的是 EPTP Shadow 隐藏 INT3 执行回退。原始 MTF / EPT #DB 不能宣称通过。
- PAGE_GUARD 和原生数据 DR 是可见机制，日志已区分；页级 EPT Watch 不等同于字节级 DR 数据断点。
- 隐藏代码读取原字节是预期行为；日志中的执行字节 CC 和真实命中支持当前自有执行绑定，不代表 DDMA / 普通物理读取能看见影子页。
- Windows 调试事件 / debug port 仍在使用，隐蔽模式不提供 debug port 隐藏。
- 以上是 x64dbg 的独立验收结果；按用户指定顺序完成后继续了 CE，结果见下节。

最新 API 兼容性及可选扩展建议见 [x64dbg API 评估](ksword-x64dbg-api-review.md)。

## 2026-10-01 CE UI 验收与临时 RWX 数据修正

继续使用同一 GUI 演示目标 PID=10464、相同执行 / 计数地址，所有验收
动作均为真实鼠标 / 键盘输入。CE 独立窗口和 KSword 日志控制页共同
确认选择、请求 / ACK、实际目标效果与清理状态。

### UI 发现与修正

首次候选 `BB7D0A72...` 中，CE GUI 将计数写为 25，演示按钮却仍从 17
增至 18；冻结后也继续增长。日志显示普通数值被当成执行代码写入
Shadow。原因是 CE SetValue / freeze 会临时调用 VirtualProtectEx 将
普通数据页设为 RWX，再 WriteProcessMemory，最后恢复原属性；后端
只看写入时的保护属性，因而误判。

CE 适配器现在记录同线程、仅供下一次写入的保护事务，验证原始提交
区域、非执行属性、范围、实际保护结果以及当前映射的 allocation / type。
事务保留 CE 原进程句柄的 DuplicateHandle；目标已退出、身份无法确认
或映射不匹配时拒绝写入。通过验证后传入内部 C++ 数据意图，允许
显式回退时执行真实数据写入并记录原因 / 结果；禁止回退时明确拒绝，
不会误建执行 Shadow 页。实际 VirtualProtectEx 及属性恢复仍执行。

该修正没有新增 DLL 必需导出，也没有将原本已经 RX / RWX 的混合
代码 / 数据页自动归类为数据；此类页面需要前端显式表达写入意图。

### CE 实测结果

完整下表使用首个修正候选 `BCBDF20E...`，CE PID=11716；部署最终
候选 `5F44E66B...` 后另作针对新增句柄绑定与读取诊断的复测。

| UI 场景 | 观测结果 | 截图序号 |
| --- | --- | --- |
| HVM 读取与精确扫描 | 实际值 28，限定计数范围扫描得到唯一正确结果 | 473–477 |
| GUI 数值修改 | 写入 25 后演示按钮得到 26，实际数据已改变 | 457–461 |
| 数值冻结 / 解冻 | 冻结值 26；两次按钮均显示 27，CE 定时恢复为 26；解冻后按钮依次得到 27、28 | 463–471 |
| Shadow 代码补丁 | 把 +6 的 inc 改成 dec；普通反汇编仍显示 inc，按钮使计数 28→27，目标原始字节仍为 8B 05 5A 4D | 478–488 |
| 恢复 Shadow 写入 | Restore Shadow 后按钮使计数恢复递增至 28，补丁页 0 | 490–492 |
| 隐蔽执行断点 | F5 安装 Shadow INT3；两次按钮均真实停在入口，目标读取仍为原字节 | 500–512 |
| 单步及删除后清理 | F7 到入口 +6；F5 删除，继续 / 再触发后计数推进至 31，断点 0、Shadow 写入页 0 | 508–518 |
| Find what writes 数据观察 | 常规 HVM 明确回退原生数据 DR；两次记录 store +8，计数 1 / 2；寄存器结果 RIP=+E、RAX=32 | 526–535 |
| 停止数据观察 | Stop 移除绑定；再次按钮计数推进至 34，观察记录仍为 2 | 537–541 |
| 禁止数据回退 | 关闭 Allow fallback、Apply；GUI 写入 100 被拒绝，按钮从 34 到 35，未修改为 100，Shadow 页 0 | 543–551 |
| 最终 DLL 数值修改 | 写入 40 后按钮得到 41；日志为实际 HVM 普通数据写入 8/8 字节 | 581–586 |
| 最终 DLL 冻结 / 解冻 | 冻结值 41，两次按钮均显示 42，CE 仍读 41；解冻后计数依次 42、43 | 587–595 |
| 最终 DLL 严格读取失败诊断 | 初始计数读取 ??，日志 status=5、NTSTATUS=0xC0000225、directWindow=0、error=299；目标按钮实际访问该页后，HVM 读取恢复为 36 | 577–580 |
| 恢复原偏好 | HVM off、Stealth on、Shadow / fallback / context / suspend on、预算 32；实际 ACK、标题 KSword R0，断点 / Shadow 页均 0 | 596–600 |

严格 HVM 私有窗口不会通过普通内存读取来隐式触发缺页。最后的现象
符合目标页初始未解析的边界；本次未额外读取页表证明具体缺页原因。
新日志明确保留协议状态与原始 NTSTATUS，避免把 ?? 误称为正常读取。
本次没有引入 HVM 读取的普通内存回退。

### 产物与证据

| 候选 / 验证阶段 | CE DLL SHA256 |
| --- | --- |
| 首次发现临时 RWX 问题 | `BB7D0A72BAEE258284DB371C986FAC691A698D1A324E7D9240C6813C317B83B6` |
| 修正后的完整 CE UI 场景 | `BCBDF20E2E8A0F21FF07388CF799F1CBAF88E803407D4A761609714C67D1F8D0` |
| 最终句柄绑定 / 诊断及针对性 UI 复测 | `5F44E66BF6DD33259392F0B0057FCAA84B8F6609BC444D655216F292AE44DD90` |

最终 DLL Release/x64 构建 0 错误、0 警告，已部署并更新安装目录哈希
记录；CE PID=1248 实际加载该文件，422912 字节。驱动服务 Running，
驱动身份和本节之前的 x64dbg / Titan 身份未变。最终 CE 内含新增 HVM
失败诊断，当前 Titan `DDD39FC6...` 尚未重新构建包含这一条新日志。
模型的临时 RWX 数据回归通过；模型仅补充 GUI 证据。

本地忽略目录 `.codex-build-logs/debugger-vm/` 中：

- `ce-ui-first-rwx-failure.log/.state`：首次实际故障。
- `ce-ui-main-tested.log/.state/.json`：完整修正后场景及当时加载身份。
- `ce-ui-final-tested.log/.state/.json`：最终 DLL 日志、实际加载身份和 ACK。
- `ce-ui-final-build.log`、`ce-ui-final-model-results.log`：最终构建 / 模型结果。
- `ui-evidence/ui-NNNN.png/json`：上表真实 UI 截图 / 输入结果。

最后无冻结、无调试绑定、无 Shadow 页，演示计数 43；CE 保持独立
R0 窗口，x64dbg 已 detach 并保持 Ready。输入 worker 在序号 601 正常
退出，专用任务已删除，TaskLastResult=0。worker 的请求 / 响应原子
替换曾遇到共享冲突，已对测试工具增加有界重试；不属于产品失败。

本轮通过的是表内扫描、读写 / 冻结、执行补丁、断点命中 / 单步 / 删除、
数据观察与拒绝场景。未覆盖 CE 的 VEH / DBVM、所有线程 / 内存 API、
128 个 EPT 槽位或 TTD；不能将其表述为所有功能均已通过。
候选源码 / DLL 尚未提交或发布，插件 ZIP 和源码 ZIP 尚未重新生成。
