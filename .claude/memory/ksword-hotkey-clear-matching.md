# 快捷键清除与跨版本 Win32k 规则匹配

- 2026-10-25本机版：窗口→热键/钩子的程序快捷键、注册热键支持多选清除；杂项末尾追加快捷键管理导航代理，实际只保留原列表/扫描实例。进程详情单项删除共用 `ProcessDock/HotkeyClearController`，编辑路径保留。
- 行身份必须保留typed原始记录、PID创建时间、模型token和刷新代次；不得从格式化列或可视行号构造变更。确认框返回后需QPointer及PID/time/ticket/快照/选区复核，避免重入换进程。
- 窗口激活热键、快捷方式文件配置、系统注册热键是不同来源。PE Accelerator和菜单文字只读。`.lnk` Save后通知Shell并重开核对文件ID/目标路径/Hotkey；verified只指文件配置，不保证Explorer内存注册已撤销。未确认回执不可报成功或确定未修改。
- `DetailActionResult::outcomeUncertain`仅用于未确认副作用，跳过共用权限失败提示后由热键回调显示警告；不改变其它动作默认行为，不添加诊断事件日志。
- 当前热键可变布局由 `KswordARKDriver/src/features/keyboard/keyboard_rule_match.c` 调用同一套纯 C register/insert/remove/loop/slot/owner/flow 规则，从当前实际 FULL/BASE 映像的导入、导出和指令证明。`ScanRva` 是 owner/window/ID 筛选函数，`RemoveRva` 是经单槽生命周期证明的一参链槽 helper；绝不可将四参扫描器或两参内联包装器当一参 helper 调用。缓存由注册与卸载同源读写交叉证明，运行时缓存指纹覆盖代码、PE 元数据和导入/IAT，变化后拒绝旧证明。
- 逐配对生成 `keyboard_profiles.generated.h` 的方案已退役并删除生成头；旧Python入口现调用同一生产C引擎，不再生成授权表。规则不按版本、SHA、PDB GUID 或样本 RVA 查白名单；未来文件在自身实际字节上通过相同语义规则及运行时门禁，不保证所有未知构型可用。
- 六组已验 AMD64 配对：22621.900、22621.6133、22621.7079、26100.1、26100.5074、本机26100.9022。首组节点64字节，其余72字节；22621三组用 `win32ksgd.sys!SGDGetUserSessionState` 和窗口对象表示，26100三组用 `win32k.sys!W32GetUserSessionState` 和窗口句柄表示。两组早期26100把扫描内联于两参包装器，其余为独立四参扫描；owner 的数据导入、线程getter及透明包装器必须按实际同源路径证明。表/缓存偏移差在六样本均为0x418，但该差值本身不是规则。
- 本机EnterCrit实际消费ECX/EDX，Full的NtUserUnregisterHotKey先将两参置零；不得声明无参。该精确文件的 ScanRva=0x1C8108、单槽RemoveRva=0x285BA0、表偏移0x3290、缓存0x36A8；这些只是取证点，不构成版本表。单槽 helper 在普通路径摘链释放，含子项路径可能改占位并返回零；普通项/空子链/完整快照/同址唯一、会话及执行前后读回门禁必须保留。
- 键盘 R0/R3 协议现为v4：枚举携带实际SessionId、变更请求携带expectedSessionId；SessionId=0是合法System会话值，须与namespace指针独立核对。只读枚举保留InCallerContext/USER；PID、线程、session筛选及读取失败/环/预算的partial状态不应丢失，旧协议变更请求不得进入新驱动写路径。
- 最终生产规则对六组真实PE配对6/6匹配，离线规则22/22、清除身份40/40通过，源码/样本/探针哈希与指令证据见 `docs/hotkey_matching/runtime-rule-validation-20261025.json`。主程序Release/x64与标准WDK驱动构建通过，ApiValidator通过，语言包/223项IOCTL计划门禁通过；驱动本轮NotSigned，未加载，GUI与真实清除由用户实机验收。DELETE/EDIT共用完整全桶唯一性门禁，拒绝重复挂链；EDIT未确认回执不可沿用旧hash。详细证据见 `docs/本机快捷键清除与自动匹配-20261025.md`、`docs/快捷键跨版本规则匹配-20261025.md`。
