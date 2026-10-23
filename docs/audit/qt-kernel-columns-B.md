# Qt 内核页表格列名（B 组：audit / hal / wdf / slat / timers / workers / cid / ipc）

## audit.inline  Inline Hook
source: KernelDock.KernelHooks.cpp:1803（列文本由 inlineHookColumnHeader 生成，KernelDock.KernelHooks.cpp:1020）
columns: 模块 | 函数 | 函数地址 | 类型 | 目标地址 | 目标模块 | 状态 | 内存字节 | 磁盘字节 | 差异状态

## audit.iat  IAT/EAT
source: KernelDock.KernelHooks.cpp:1912（列文本由 iatEatColumnHeader 生成，KernelDock.KernelHooks.cpp:1049）
columns: 类别 | 模块 | 导入模块 | 函数/序号 | Thunk/EAT项 | 当前目标 | 期望目标 | 目标模块 | 状态

## audit.callbacks.list  回调列表
source: KernelDock.CallbackEnum.cpp:2162（列文本由 callbackEnumColumnHeaderText 生成，KernelDock.CallbackEnum.cpp:1852）
columns: 类别 | 注册类型 | 来源 | 可信状态 | 状态 | 可移除 | 名称 | 回调/对象地址 | 模块 | 公司 | 文件版本 | 文件描述 | Altitude

## audit.callbacks.tree  Minifilter 回调树
source: KernelDock.CallbackEnum.cpp:2211
columns: <TREE>
tree_headers: Filter / 操作 | 类型 | Pre/Post 回调 | 驱动 | 驱动路径 | 公司 | 文件版本 | 描述 | Altitude | 来源/可信状态

## audit.intercept  驱动回调（规则表，registry/process/thread/image/object/minifilter 六类共用同一构造）
source: KernelDock.CallbackIntercept.cpp:4082（createRuleTableTab，KernelDock.CallbackIntercept.cpp:4078）
columns:[主表] 启用 | RuleID | GroupID | 规则名称 | 操作类型 | 匹配模式 | 动作 | 超时毫秒 | 超时决策 | 优先级
columns:[明细表] groupId | 组名称 | 启用 | 优先级 | 备注
source: KernelDock.CallbackIntercept.cpp:1950（规则组表 m_groupTable）

## audit.intercept.registry  注册表
source: KernelDock.CallbackIntercept.cpp:4082（createRuleTableTab 调用点 KernelDock.CallbackIntercept.cpp:2003）
columns: 启用 | RuleID | GroupID | 规则名称 | 操作类型 | 匹配模式 | 动作 | 超时毫秒 | 超时决策 | 优先级

## audit.intercept.process  进程创建
source: KernelDock.CallbackIntercept.cpp:4082（createRuleTableTab 调用点 KernelDock.CallbackIntercept.cpp:2004）
columns: 启用 | RuleID | GroupID | 规则名称 | 操作类型 | 匹配模式 | 动作 | 超时毫秒 | 超时决策 | 优先级

## audit.intercept.thread  线程创建
source: KernelDock.CallbackIntercept.cpp:4082（createRuleTableTab 调用点 KernelDock.CallbackIntercept.cpp:2005）
columns: 启用 | RuleID | GroupID | 规则名称 | 操作类型 | 匹配模式 | 动作 | 超时毫秒 | 超时决策 | 优先级

## audit.intercept.image  镜像加载
source: KernelDock.CallbackIntercept.cpp:4082（createRuleTableTab 调用点 KernelDock.CallbackIntercept.cpp:2006）
columns: 启用 | RuleID | GroupID | 规则名称 | 操作类型 | 匹配模式 | 动作 | 超时毫秒 | 超时决策 | 优先级

## audit.intercept.object  对象管理器
source: KernelDock.CallbackIntercept.cpp:4082（createRuleTableTab 调用点 KernelDock.CallbackIntercept.cpp:2007）
columns: 启用 | RuleID | GroupID | 规则名称 | 操作类型 | 匹配模式 | 动作 | 超时毫秒 | 超时决策 | 优先级

## audit.intercept.minifilter  文件系统微过滤器
source: KernelDock.CallbackIntercept.cpp:4082（createRuleTableTab 调用点 KernelDock.CallbackIntercept.cpp:2008）
columns: 启用 | RuleID | GroupID | 规则名称 | 操作类型 | 匹配模式 | 动作 | 超时毫秒 | 超时决策 | 优先级

## audit.intercept.bypass  Minifilter PID 放行
source: KernelDock.CallbackIntercept.cpp:2548
columns: PID | 进程

## audit.intercept.protect  进程保护
source: KernelDock.CallbackIntercept.cpp:3087（保护规则表）/ KernelDock.CallbackIntercept.cpp:3172（受信发起方表）
columns:[主表] 启用 | 匹配方式 | 受保护目标 | 拦截的权限 | 含线程 | 内核保护 | 守护 | 规则名 | 削权次数 | 施加次数
columns:[明细表] 匹配方式 | 信任的发起方

## audit.intercept.application  应用日志
source: KernelDock.CallbackIntercept.cpp:2014（m_appLogEditor = new CodeTextEdit）
columns: <NO TABLE>
说明: 该页为 QPlainTextEdit/CodeTextEdit 纯文本编辑器，无表格、无表头。搜过 setHorizontalHeaderLabels / setHeaderLabels / setColumnCount，均无命中。

## audit.intercept.events  事件日志
source: KernelDock.CallbackIntercept.cpp:2028（m_eventLogEditor = new CodeTextEdit，最多 4096 块）
columns: <NO TABLE>
说明: 同为纯文本编辑器（另有“完整事件日志”对话框 QPlainTextEdit），无表格、无表头。

## audit.unified  统一审计
source: KernelPatchHookAuditTab.cpp:208
columns: 类别 | 模块 | 目标 | 当前地址 | 期望地址 | 风险 | 当前字节 | 基线字节 | 详情

## audit.shadow  Shadow 比较
source: KernelShadowComparisonTab.cpp:205（ColCount 同文件 :204）
columns: 序号 | 函数名 | 当前地址 | 表值 | 所属模块 | 状态

## hal.dispatch  HalDispatchTable
source: KernelPlatformAuditTab.cpp:571（初始化）/ KernelPlatformAuditTab.cpp:480（重译表头，同一组列）
columns: 函数 / 回调 | 索引 | 当前地址 | 原始地址 | Hook | 模块 | 厂商 | 表地址 | 签名策略 | 置信度 | 状态 | 备注

## hal.private  HalPrivateDispatchTable
source: KernelPlatformAuditTab.cpp:571（初始化）/ KernelPlatformAuditTab.cpp:480（重译表头，同一组列）
columns: 函数 / 回调 | 索引 | 当前地址 | 原始地址 | Hook | 模块 | 厂商 | 表地址 | 签名策略 | 置信度 | 状态 | 备注

## hal.acpi  HalAcpiDispatchTable
source: KernelPlatformAuditTab.cpp:571（初始化）/ KernelPlatformAuditTab.cpp:480（重译表头，同一组列）
columns: 函数 / 回调 | 索引 | 当前地址 | 原始地址 | Hook | 模块 | 厂商 | 表地址 | 签名策略 | 置信度 | 状态 | 备注

## hal.components  HalSubComponents
source: KernelPlatformAuditTab.cpp:571（初始化）/ KernelPlatformAuditTab.cpp:480（重译表头，同一组列）
columns: 函数 / 回调 | 索引 | 当前地址 | 原始地址 | Hook | 模块 | 厂商 | 表地址 | 签名策略 | 置信度 | 状态 | 备注

## wdf.functions  WDF 函数
source: KernelPlatformAuditTab.cpp:571（初始化）/ KernelPlatformAuditTab.cpp:480（重译表头，同一组列）
columns: 函数 / 回调 | 索引 | 当前地址 | 原始地址 | Hook | 模块 | 厂商 | 表地址 | 签名策略 | 置信度 | 状态 | 备注

## wdf.callbacks  WDF 回调
source: KernelPlatformAuditTab.cpp:571（初始化）/ KernelPlatformAuditTab.cpp:480（重译表头，同一组列）
columns: 函数 / 回调 | 索引 | 当前地址 | 原始地址 | Hook | 模块 | 厂商 | 表地址 | 签名策略 | 置信度 | 状态 | 备注

## slat.probes  EPT/NPT 交叉视图
source: KernelSlatIommuAuditTab.cpp:169
columns: 探针 | 虚拟地址 | 物理地址 | 字节 | 虚拟哈希 | 物理哈希 | 交叉视图 | NTSTATUS

## slat.iommu  IOMMU / DMAR / IVRS
source: KernelSlatIommuAuditTab.cpp:200
columns: 类型 | 段 / 设备 | 基址 | 上界 | 证据标志 | 能力 / 扩展能力 | 状态 / 根表 | NTSTATUS

## timers  定时器/DPC
source: KernelDock.KernelHooks.cpp:1998（表头循环，按 TimerDpcColumn 枚举顺序，枚举定义 KernelDock.KernelHooks.cpp:835；列文本 timerDpcColumnHeader KernelDock.KernelHooks.cpp:1076）
columns: CPU | Bucket | Timer | DueTime | Period | 类型 | DPC | 例程 | 上下文 | 模块 | 状态

## workers  内核工作队列线程
source: KernelThreadAuditTab.cpp:302（表头在 applyTranslatedText 中设置，列枚举 Column 见 KernelThreadAuditTab.cpp:230）
columns: 线程ID | EThread | 类别 | 队列类型 | Node/Priority | EX_WORK_QUEUE | 线程状态 | 等待原因 | 例程入口 | 参数 | 入口模块 | 模块基址 | 模块路径 | R0状态 | 管理保护

## cid  CID表
source: KernelDockCidTab.cpp:218
columns: 类型 | CID | PID | TID | 图像 | 对象地址 | 起始地址 | SourceMask | AnomalyFlags | CID Flags | Confidence | DetailStatus | Denoise | 状态

## ipc.pipe  NamedPipe
source: KernelNamedPipeTab.cpp:211（ks::ui::FlatTableModel ColumnSpec 表头，源码为英文硬编码字面量）
columns: Pipe Name | NT Path | Attributes | LastWriteTime | Status | Source
说明: 该页表头走 FlatTableModel::headerData → ks::i18n::displayText（UI/FlatTableModel.h:335），但 displayText→sourceText 在 zh-CN（产品源语言基线，LanguageManager.cpp:82/1977）直接 return sourceText，故中文界面即原样显示这六个英文词；zh-CN.json 里 "Pipe Name"/"NT Path"/"Status" 的中文映射不参与该路径。无 retranslate 改写表头。

## ipc.alpc  ALPC
source: KernelDockIpcTab.cpp:393（ALPC 端口表）/ KernelDockIpcTab.cpp:369（IPC 摘要表 m_ipcSummaryTable）
columns:[主表] 角色 | Relation | Owner PID | Flags | State | Seq | Object | Context | Port Name | Status
columns:[明细表] 类别 | 状态 | Count | 来源 | 详情

## ipc.communication  通信对象
source: KernelCommunicationEndpointTab.cpp:119
columns: 来源目录 | 名称 | 类型 | 完整路径 | 状态
