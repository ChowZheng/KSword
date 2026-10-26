# Qt 内核页列名真值（A 组：object / io / atom / misc）

## object.overview  总览
source: KernelDock.cpp:1181
columns: <TREE>
tree_headers: 名称 | 类型 | 路径/说明 | 状态 | 符号链接目标
columns:[属性表] 属性项 | 值
source:[属性表] KernelDock.cpp:1207

## object.directory  目录递归
source: KernelObjectDirectoryDeepTab.cpp:357
columns: <TREE>
tree_headers: 名称 | 类型 | 完整路径 | 深度 | 状态

## object.named  BaseNamedObjects
source: KernelBaseNamedObjectsTab.cpp:231
columns: scope | directoryPath | objectName | objectType | fullPath | symbolicTarget | statusText
note: 控件为 ks::ui::VisibleTableWidget（QTableWidget 派生，UI/VisibleTableWidget.h:428），表头走 setHorizontalHeaderLabels 直接写入 QTableWidgetItem，不经任何 i18n，字面量即最终显示值。

## object.links  符号链接
source: KernelSymbolicLinkTab.cpp:153
columns: sourceDirectory | linkName | fullPath | targetPath | dosCandidate | statusText
note: 控件为 ks::ui::FlatTableModel（UI/FlatTableModel.h:335 `headerData` 调 ks::i18n::displayText）。displayText 在 LanguageManager.cpp:2052 把 translate 绑成 sourceText，sourceText → cachedSourceText(...,true) 在 :1977 命中 isHistoricalChineseLanguage 提前返回原串；当前语言 zh-CN 即 kProductFallbackLanguageId（:82），故不做任何词典反查，显示值 = 字面量。

## object.drivers  设备与驱动
source: KernelDeviceDriverObjectsTab.cpp:230
columns: 目录路径 | 对象名称 | 对象类型 | 完整路径 | 目标路径 | 状态 | 能力提示

## object.iotimer  IoTimer
source: KernelIoTimerTab.cpp:262
columns: IoTimer 地址 | DeviceObject | DriverObject | 驱动名 | 设备名 | 对象路径 | 查询状态

## object.ioctl  IOCTL 派遣表
source: KernelIoctlAuditTab.cpp:410
columns:[驱动概览] DriverObject | 对象地址 | DriverStart | 镜像大小 | Flags | 设备数 | MajorFunction 数 | 查询状态 | NTSTATUS
columns:[设备对象] DriverObject | 关系 | DeviceObject | 设备名称 | 设备类型 | Flags | Characteristics | StackSize | RootDevice | NextDevice | AttachedDevice | 归属 DriverObject | NameStatus
columns:[MajorFunction] DriverObject | 对象地址 | MajorFunction | 编号 | 派遣地址 | 模块基址 | 归属模块 | Flags | 状态
columns:[KswordARK IOCTL 注册表] 控制码 | Function | Method/Access | 能力门槛 | Handler | 名称 | Flags
source:[设备对象] KernelIoctlAuditTab.cpp:420
source:[MajorFunction] KernelIoctlAuditTab.cpp:434
source:[KswordARK IOCTL 注册表] KernelIoctlAuditTab.cpp:444

## object.procedures  ObjectType 方法指针
source: KernelObjectTypeProcedureTab.cpp:150
columns: 类型索引 | 类型名 | 方法 | 槽位地址 | 目标地址 | 归属模块 | 所在节 | 风险 | 状态

## object.types  对象类型
source: KernelObjectTypeMatrixTab.cpp:202
columns: 类型编号 | 类型名 | R0 类型地址 | R0 交叉验证 | 对象数 | 句柄数 | 访问掩码 | 枚举策略

## io.ssdt  SSDT
source: KernelDock.Ssdt.cpp:212
columns: 索引 | 服务名 | Zw导出地址 | 服务例程 | 槽位地址 | 模块
note: 控件为 KernelResultTable（KernelResultModel.h:85，QTableView + KernelResultModel），表头经 KernelResultModel.cpp:51 `ks::i18n::displayText`；zh-CN 下 displayText 走 sourceText 提前返回原串（LanguageManager.cpp:1977），显示值 = 字面量。

## io.shadow  ShadowSSDT
source: KernelDock.KernelHooks.cpp:1697
columns: 索引 | 服务名 | Stub地址 | 服务例程 | 槽位地址 | 模块
note: 同上，KernelResultTable；列名文本经 shadowSsdtColumnHeader 逐列给出，zh-CN 下 displayText 回原串，显示值 = 字面量。

## io.idt  IDT
source: KernelDescriptorTableTab.cpp:187
columns: 表 | CPU | 向量/选择子 | 表基址 | 表 Limit | 表项地址 | 大小 | Handler/段基址 | SEL | 类型 | DPL | Present | IST | Granularity | 归属模块 | 完整性 | 启动期基线 | 可信映像基线 | 原始值

## io.gdt  GDT
source: KernelDescriptorTableTab.cpp:187
columns: 表 | CPU | 向量/选择子 | 表基址 | 表 Limit | 表项地址 | 大小 | Handler/段基址 | SEL | 类型 | DPL | Present | IST | Granularity | 归属模块 | 完整性 | 启动期基线 | 可信映像基线 | 原始值

## io.ioctls  IOCTLS
source: KernelIoctlDecoderTab.cpp:1
columns: <NO TABLE>
note: 该页为纯 R3 CTL_CODE 解码器（KernelIoctlDecoderTab），只含 QLineEdit/QGroupBox/QFormLayout 与位段绘制控件 KernelIoctlBitLayoutWidget，全文件无 setHorizontalHeaderLabels / setHeaderLabels / setColumnCount / QTableWidget / QTreeWidget。

## io.startio  DriverStartIo
source: KernelDriverStartIoTab.cpp:120
columns: DriverObject | 对象地址 | 状态 | StartIo 地址 | 模块基址 | 归属模块 | 镜像路径 | NTSTATUS

## atom  原子表遍历
source: KernelDock.cpp:1303
columns: Atom值 | 十六进制 | 名称 | 来源 | 状态
note: 控件为 KernelResultTable，表头经 KernelResultModel.cpp:51 displayText；zh-CN 下 displayText 回原串，显示值 = 字面量。

## misc.ntquery  历史NtQuery
source: KernelDock.cpp:1389
columns: 类别 | 函数 | 查询项 | 状态 | 摘要

## misc.integrity  代码完整性
source: KernelTextIntegrityTab.cpp:205
columns:[模块表] 模块 | 基址 | 访问后端 | 可执行节 | 已比对字节 | 不可读字节 | 差异字节 | 动态重定位位点 | 无法解释 | 磁盘信任 | 结论
columns:[差异区间表] 模块 | 节 | RVA | 内核地址 | 长度 | 归类 | 净映像字节 | 内存字节
source:[差异区间表] KernelTextIntegrityTab.cpp:250

## misc.vbs  VBS/HVCI
source: KernelVbsPostureTab.cpp:129
columns: 检查项 | 观测值 | 判定 | 证据来源

## misc.hvci  HVCI 绕过
source: MiscDock/HvciBypass/HvciBypassPage.cpp:1
columns: <NO TABLE>
note: 页面由 MiscDock/HvciBypass/HvciBypassPage.cpp 实现，全文件无 QTableWidget / QTreeWidget / setHorizontalHeaderLabels / setHeaderLabels / setColumnCount，只有参数区控件与 CodeEditorWidget 日志。

## misc.rxpf  RXPF 模拟
source: MiscDock/HvciBypass/RxpfPage.cpp:1
columns: <NO TABLE>
note: 页面由 MiscDock/HvciBypass/RxpfPage.cpp 实现，使用 ParameterWorkspace 两栏结构（参数区 + 按钮 + CodeEditorWidget 轨迹/日志），全文件无 QTableWidget / QTreeWidget / 表头设置调用。
