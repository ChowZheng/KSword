# 调试器共享后端与 x64dbg 适配

- CE 与 x64dbg 共用 `DebuggerBackend/`；R0 协议仍只定义在 `shared/driver/`，设备访问仍通过 `ArkDriverClient/`。
- `TitanEnginePlugin/` 的规范 ABI 是 x64dbg `f107330b6563da3c38d60a3ad6e629057b7cf5d0` 的 64 个导出，原生 TitanEngine 固定 `21ef77f31fd42d17f785802c36b2ca4ca44c43d7`。源头、检查器及声明见 `third_party/x64dbg_abi/`。
- 代理放在运行目录 `KSword/TitanEngine.dll`，原生依赖保留在上一级 `TitanEngine.dll`。两者均校验 AMD64/导出并固定进程生命周期；初始化不放在 DllMain。保留原生成功/失败 LastError。
- x64dbg 核心只增加 `DebugEngineKSword=4`，沿现有 checked loader 加载；补丁及基线在 `X96dbgIntegration/`。启动器使用真实 `-userdir` 和每会话 INI，不能发明 GUI 不接受的 `--engine` 参数。
- `X96dbgExecutablePlugin/` 的 ID 是 `x96dbg`，仅发行 x64dbg AMD64。KSword Tab 为配色/等宽控制日志页，调试器保持独立窗口。页面关闭不应终止调试器或目标。
- 默认原生模式不依赖驱动。HVM 控制使用 SESSION/REVISION 文件请求和实际状态确认；活动目标必须持有真实 Windows 调试事件且清空已有硬件绑定后，才允许切换模式。
- `DebuggerBackend` 原生会话 API 只借用前端会话，不能二次 attach，也不能抢 Wait/Continue。保留进程/线程句柄身份及代次，防止 PID/TID 重用和旧回调。
- HVM 硬件执行断点通过驱动精确线程/RIP 筛选和 guest #DB，交给原生 Windows `EXCEPTION_SINGLE_STEP` / Titan DebugLoop。历史 ring 事件不能伪装成当前停止，也不能由轮询线程调用前端断点回调。
- 代理仅改原生 DLL 内六个 IAT 槽：GetThreadContext、SetThreadContext、WaitForDebugEvent、ContinueDebugEvent、ReadProcessMemory、WriteProcessMemory。完整 GPR/SIMD/XSTATE 留给 Windows；内部断点读取见逻辑 INT3，公开内存读取见原字节。常规 EPT 保留 Windows DR6；CE ShadowPage 停止只在匹配的真实持有事件中叠加命中槽/RIP。DR7 先于地址的短序列必须暂存，未完成或失败时不能继续。
- Titan HVM 下通用硬件写/读写断点返回 ERROR_NOT_SUPPORTED，不能将 4 KiB EPT 数据页语义冒充字节范围。软件断点保留原生循环并支持隐藏 INT3，通用内存断点仍原生；显式 Watch 和完整 HVM 命令通过版本化 KSwordDebuggerCall。
- 常规 HVM 执行断点不可用时，公共后端自动尝试 EPTP 切换的 ShadowPage 隐藏 INT3（不要求 MTF）。原生协议操作 8/9 管理目标页 MDL、owner/target 进程引用、同页偏移合并及进程退出清理。不能接管外部准备/常驻；目标映射更换仍要重装。
- CE 标题是 KSword HVM / KSword R0 / KSword Connected；Connected 表示桥接已连接但驱动不可用，保留 CE 原函数表。VM 中 R0 上下文错误 31、挂起计数导出缺失会明确记录并用 Windows 接口，策略/身份拒绝不回退。进程内 Lua 缓冲区自读写不得送往目标驱动内存接口。
- 控制/确认文件读取必须允许 FILE_SHARE_READ/WRITE/DELETE，并在解析/等待前关闭句柄。否则第二次原子替换会因共享冲突失败。普通原生 StepInto/StepOver 不能套用硬件停止的延迟回调；只对真实匹配的自有硬件停止采用事件代次保护。
- 单步完成先在策略锁内退休私有回调记录，再释放锁调用前端回调。真实 x64dbg 暂停回调会等 Run；持锁调用会使另一线程的上下文查询死锁。实际命令层 HVM attach/bphws/mov/sti/run/bphwc/detach 已验证，DLL 回调 fixture 不能替代这个检查。
- 清理仅处理自动适配记录的私有 ID；外部已准备/常驻状态会阻止自动准备。原生出口、线程退出、模块卸载及内存释放需要清理。Decommit 按 Windows 实际页舍入处理，并检查地址溢出；模块通过 PID/AllocationBase 匹配。原始规则由调用者负责，不能混入自动持有的准备。
- 构建脚本 `tools/Build-X96dbgPayload.ps1` 必须使 CMake 探测进程也使用 AMD64 环境及 64 位 MSBuild；仅 `-T host=x64` 在本机可能仍探测 HostX86。MSBuild 始终传三项架构属性并读回 HostX64。
- `tools/export_x96dbg_source.py` 导出可构建源码及固定上游源码 ZIP，`tools/package_x96dbg_plugin.ps1` 校验 64 位 PE、64 导出与实际 bridge 标记，保留源代码/原始许可证并生成哈希清单。插件 ZIP 根直接为 plugin.json，解压到 plugin/x96dbg。
- 2026-09-30 后续在 KSword-HVM-Target（Win11 22621.4317、2 vCPU）加载匹配驱动，真实 GUI、CE R0/HVM 扫描、三态标题、CE 硬件/软件隐藏 INT3、Titan 完整上下文修改/单步/重复命中、ShadowPage owner/target 死亡清理均通过。最终全量原生回归 85/85 PASS、2 回放 SKIP；实际两个 Tab 连续开关与关闭保持独立调试器也通过。此 VM 缺 MTF，原始 MTF/EPT #DB 路由不支持；已验收自动 ShadowPage 执行回退。16 项 HVM 通路/布局检查不代表所有变更操作实测。准确产物哈希和边界见 `docs/ksword-debugger-vm-validation.md`。

## 2026-09-30 后续策略与 Shadow 代码补丁

- 公共 C ABI 命令 4/5/6/7 为 GET_OPTIONS / SET_OPTIONS / QUERY_POLICY / RESTORE_SHADOW_WRITES。选项 v1 为 48 字节（mode、shadowMemoryWrites、allowFallback、强制开启 logFallback、nativeContextFallback、nativeSuspendFallback、maxShadowPages=1..32），策略为 72 字节；路径 Native=0、EPT=1、Shadow=2。旧调用/默认行为保留；离线选项不初始化驱动，失败 ACK 返回实际选项，有自有绑定/补丁时不能更改策略。
- 常规 HVM 保留自动执行回退，隐蔽模式优先 Shadow，不允许真实代码/可见 INT3/真实执行 DR 回退。非可执行数据在 allowFallback=1 时可普通写入，隐蔽模式也允许该明确数据回退；每次记录原因及实际完成/失败、字节数。上下文/挂起还受对应自定义开关约束。
- CE Shadow 代码写入默认关闭，显式启用后修改可执行页执行视图，普通数据读取保持原字节。RX/RWX 不能判定代码意图，RWX 可能包含冻结数据；不能宣称通用数据冻结隐身。代码 Shadow 需要 HVM，失败不会普通写回原代码。单次同页最多 1232 字节；跨页、超限或代码/数据混合范围修改前拒绝。
- 代码补丁和隐藏 INT3 均接受 MEM_PRIVATE 或已 COW 私有化的 MEM_IMAGE / MEM_MAPPED；锁页后必须 working-set Valid=1、Shared=0，并复查当前 VA/PFN。COW 后 Windows 仍报告映像/映射类型，不能按类型误拒绝。仍共享的页面错误 50 明确提示 requires Windows COW-private backing，不自动向运行目标同字节写入来触发 COW。视图按物理页生效，不保证 PID/CR3 隔离；映射替换仍需重新安装。
- 驱动原生操作 10/11 写入/恢复 PatchMask，INT3 层优先，恢复内存补丁保留断点。旧补丁回放失败用操作 12 标记 owner/target 内容隔离；适配器 start/continue 和驱动直接 START 都拒绝，只有完整成功恢复清除，部分恢复不清除。完整恢复、进程退出、驱动卸载清理引用。
- START 先取得 Shadow 共享锁租约、验证所有 owner 视图/隔离，再取得 HVM 锁，统一出口先释放 HVM 再释放租约，防止检查到启动间视图被改。Shadow 变更及退出清理按 Shadow→HVM 顺序；Shadow 内不调用 START。租约的临界区进入/退出包括失败出口均平衡。
- 本轮模型 PASS（包括第二段旧字节回放失败的隔离/完整恢复）；驱动完整 WDK Build、x64 ApiValidator、CAT PASS；CE/Titan Release/x64 构建 PASS，均 0 警告/错误。安装布局 8 例及实际 VM 受控市场安装 PASS，主程序构建退出码 0、i18n 26697 条通过（`ksword-build-check-20260930-205354.raw.log`）；用户运行目录新版已部署并核对哈希，该目录市场操作仍待最终复测。本轮策略 offline、memory、normal-native、normal-hvm 实时 PASS：执行补丁 `0xff`、普通读原始、恢复执行 `0x101`，数据回退和无半写入拒绝通过。
- 本轮最终六阶段实时全部 PASS（offline、memory、normal-native、normal-hvm、stealth-hvm、stealth-no-hvm），证据 `.codex-build-logs/debugger-vm/policy-final-run.log` 的 `FINAL PASS policy regression`。首轮隐蔽模式问题已修复：原生 Titan 回调前暂时移除物理 INT3，状态必须按已安装的逻辑绑定报告，不能按当前物理字节或所选模式推断。恢复最后内存补丁后路径 Shadow=2、逻辑断点 1，恢复后的第二次真实隐藏 INT3 命中已通过；HVM 关闭的隐蔽模式明确拒绝可见 DR 回退。修复后构建见 `x96-policy-titan-route-build.log`，0 警告/错误。
- 最终策略复验前显式恢复 KswordARK 服务；服务不存在时 readiness 错误 2 不属于功能验收。之前 85/85 属于前一轮基线，本轮六阶段有独立实时证据；新插件在用户运行目录的实际部署和界面验证尚未包含在该后端测试内。保留首轮失败证据 `debugger-vm/policy-result-first.json`、`policy-stealth-first.log`，准确状态与范围见 `docs/ksword-debugger-vm-validation.md` 的后续策略章节。
- 后续 COW/上下文准备修正模型通过：prepared/resident 外部所有权在修改前拒绝，不 STOP/TEARDOWN/写上下文或假回滚；真实修改失败仍恢复。重复诊断按独立原因最多 64 键合并，首次和累计 10/100/1000 次输出；回退计数和实际错误 31 每次保留，日志不改调用方 LastError。证据 `.codex-build-logs/debugger-image-cow-model-results.log`。
- 新七阶段实际 VM 合计 490 PASS：offline 29、memory 130、image-cow 109、normal-native 55、normal-hvm 59、stealth-hvm 82、stealth-no-hvm 26。模块页先 Shared=1 拒绝 50；受控 Windows 同原字节 COW 后仍 MEM_IMAGE/Shared=0，Shadow 使目标执行 102，双方普通读及另一进程执行保持 101，恢复执行 101。证据 `.codex-build-logs/debugger-vm/policy-cow-ready-run.log` / `policy-cow-result.json`（passed=true）。驱动 65545A14...、CE 6A5B25F8...、Titan D85AA8C9...、目标 DC85BF56... 的完整中间身份见验收文档；该轮早于最后常规 HVM 仅数据 DR 上下文修正。
- 最后仅数据 DR 修正已通过 BackendTests：常规允许回退时原生 DR 设置、替换、清零、继续、线程/会话退休都不修改外部 prepared/resident，清零/退休解除策略占用；隐蔽或禁用回退明确拒绝可见 DR。证据 `.codex-build-logs/debugger-final-model-results.log`；实际 CE 验收另见后条。
- 最终 CE `1C569800FF2F472D5660B7840C5C47DA8C7A9A5483576EDDBEC95C93D9750600`、Titan `36CC7773A4EFBA33AB6AFDB4ECF3D0F703D69C2BF489EA2A37F9A15F2BEDD6C1`（驱动/目标不变）生产 VM 再跑七阶段 490 PASS，日志 `debugger-vm/policy-final-cow-data-run.log` FINAL PASS，已取回 `policy-final-cow-data-result.json` passed=true。真实原生代理兼容/依赖拒绝也通过 `debugger-final-proxy-results.log`，不打开驱动。后续独立生产 GUI 也通过，见后条。
- 实际 CE 数据观察断点独立通过 147 项（`debugger-vm/ce-data-live.log` FINAL PASS）：8 字节写断点真实 Windows #DB/DR6 数据槽命中，store 后 RIP/RAX/数据=0x101；清除后原始 Windows DR0–DR3=0、DR6=0、DR7=0，backend bindings=0/canChange=true；第二次 store counter=2 且回调 hits=1。外部 HVM generation81、prepared2、resident2/2 全程不变且无自有 EPT/Shadow；测试所有者最后 STOP/TEARDOWN 成功，generation83、prepared/resident0（`ce-data-hvm.log`）。该路径是可见原生数据 DR，不宣称隐藏数据断点。
- CE debug_getBreakpointList 可包含已禁用/markedfordeletion 的条目：getBreakpointAddresses 不过滤，RemoveBreakpoint 先禁用再由 idle cleanup 删除。即时列表非空不能证明仍活动；验收必须看原始 Windows DR、后端绑定/策略占用、再次真实写入无命中。上述实际 CE 测试确认此残留只是内部记账，未修改 CE 删除逻辑。
- 最终真实生产 GUI 315 PASS（`debugger-vm/gui-tabs-final.log` FINAL PASS）：用户目录 KSword 与两页 PluginHost 选项、六切换项/页预算、出站新 revision 和实际 ACK、错误选项不改当前/保存配置、独立 CE/x64dbg 顶层窗口、Consolas 与真实转发日志。真实新会话重新加载已保存自定义选项且 HVM off，最终无绑定/补丁页；`gui-tabs-owned-processes.json` Outcome=PASS、FreshSessionReload=true、OriginalPreferencesRestored=true，任务返回 0。
- GUI 首次失败属于 fixture 顺序：CE action1 只修改 options，不关闭已保存的 HVM 选择；必须先 action0 关闭 HVM 并 ACK，再 action1 设置选项。初始化/结束步骤已修，产品/DLL 身份未变。上述验收与之前 85/85 和早期 GUI 基线分别保留。
- 最终源码 ZIP 已重生成，CE 2.1.0 / x96dbg 1.1.0 根清单包校验通过并部署到用户运行目录；安装后的 338 / 125 个清单文件全部 SHA256 一致。用户 Main/CE/x64dbg 已重新打开，六个策略控件存在，实际加载的 CE/Titan DLL 与最终构建相同；CE 可见主窗为 `Cheat Engine (Admin) [KSword HVM]`。测试 HVM prepared/resident=0，7 个已完成的自有测试任务已移除。汇总证据 `dist/KSword-debugger-final-validation.json`；本轮新修改/插件包尚未提交或推送。
- Lazarus 的 `Get-Process.MainWindowTitle` 可能返回辅助应用窗 `Cheat Engine`，而可见 CE 主窗已有 KSword 三态标题。标题验收需按精确 CE PID 枚举可见顶层窗，不能把该辅助窗标题当作主窗标题更新失败。
