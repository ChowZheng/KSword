# Qt 硬件 Dock 挂载结构

## 顶层页（按 addTab 顺序）
1. 性能监控  id=hardware.tab.utilization  源=HardwareDock.cpp:5154
2. 硬件概览  id=hardware.tab.overview  源=HardwareDock.cpp:5155
3. 处理器  id=hardware.tab.cpu  源=HardwareDock.cpp:5156
4. 电源  id=hardware.tab.power  源=HardwareDock.cpp:5157
5. 显卡  id=hardware.tab.gpu  源=HardwareDock.cpp:5158
6. 内存  id=hardware.tab.memory  源=HardwareDock.cpp:5159
7. 磁盘活动  id=hardware.tab.disk_activity  源=HardwareDock.cpp:5160
8. 设备管理  id=hardware.tab.device_manager  源=HardwareDock.cpp:5161
9. 其他设备  id=hardware.tab.other_devices  源=HardwareDock.cpp:5162
10. 硬件标识设置  id=hardware.tab.hwid  源=HardwareDock.cpp:5163
11. 设备驱动链  id=hardware.tab.device_stack  源=HardwareDock.cpp:5164
12. 键盘与鼠标  id=hardware.tab.keyboard_mouse  源=HardwareDock.cpp:5165
13. i8042prt 审计  id=hardware.tab.i8042_audit  源=HardwareDock.cpp:5166
14. USB 设备  id=hardware.tab.usb  源=HardwareDock.cpp:5167
15. 系统总线  id=hardware.tab.system_bus  源=HardwareDock.cpp:5168
16. 底层硬件检查  id=hardware.tab.r0_evidence  源=HardwareDock.cpp:5169

## 二级页
父=hardware.tab.utilization
（实现：左侧 QListWidget 设备卡片 + QStackedWidget 详情页；整个硬件 Dock 只有一个 QTabWidget 即顶层 m_sideTabWidget，HardwareDock.cpp:5512）
- CPU  id=hardware.utilization.cpu  源=HardwareDock.cpp:5179
  columns: <NO TABLE>
- 内存  id=hardware.utilization.memory  源=HardwareDock.cpp:5181
  columns: <NO TABLE>
- 磁盘 N（每块磁盘一条）  id=hardware.utilization.disk:<instanceName 小写 UTF-8 的 hex>  源=HardwareDock.cpp:5184, 5260
  columns: <NO TABLE>
- 以太网 N（每块物理网卡一条）  id=hardware.utilization.network:<LUID 十六进制>  源=HardwareDock.cpp:5186, 5270
  columns: <NO TABLE>
- 虚拟网络  id=hardware.utilization.virtual_network  源=HardwareDock.cpp:5279
  columns: <NO TABLE>
- GPU N（每块显卡一条）  id=hardware.utilization.gpu:<adapterKey 十六进制>  源=HardwareDock.cpp:5190, 5282
  columns: <NO TABLE>

## 无二级页的页面
- 硬件概览  id=hardware.tab.overview
  columns: <NO TABLE>（StructuredFieldView Presentation::Tree，表头隐藏的两列，名称/值；HardwareDock.cpp:5603）
- 处理器  id=hardware.tab.cpu
  columns: 逻辑处理器 | 利用率(%) | 当前频率(MHz) | 最大频率(MHz) | 限频(MHz) | 温度 | 电压
  源=HardwareDock.cpp:8113-8122（7 列）
- 电源  id=hardware.tab.power
  columns: 项目 | 当前值
  源=HardwarePowerPage.cpp:468-471（2 列，表头用 hardware.power.table.item / hardware.power.table.value，中文兜底为“项目/当前值”）
- 显卡  id=hardware.tab.gpu
  columns: <NO TABLE>（StructuredFieldView Presentation::Tree，名称/值；HardwareDock.cpp:8193）
- 内存  id=hardware.tab.memory
  columns: <NO TABLE>（StructuredFieldView Presentation::Tree，名称/值；HardwareDock.cpp:8212）
- 磁盘活动  id=hardware.tab.disk_activity
  columns:[进程表] 选择 | 进程 | PID | 读字节/s | 写字节/s | 总字节/s | 响应时间 | 读次数/s | 写次数/s | 路径
  columns:[活动表] 进程 | PID | 文件 | 读(字节/秒) | 写(字节/秒) | 总数(字节/秒) | I/O 优先级 | 响应时间(ms)
  columns:[存储表] 驱动器 | 卷标 | 文件系统 | 活动时间 | 可用空间 | 总空间 | 读(字节/秒) | 写(字节/秒) | 响应时间(ms) | 队列长度
  源=DiskMonitorPage.cpp:1245-1256 进程表（10 列，setHorizontalHeaderLabels 后于 1264 moveSection 把“进程”移到可视第 2 位）、DiskMonitorPage.cpp:1280-1289 活动表（8 列，1299 moveSection 把“进程”移到可视第 1 位）、DiskMonitorStoragePanel.cpp:348-359 存储表（10 列）
- 设备管理  id=hardware.tab.device_manager
  columns: <TREE>
  tree_headers: Name | Manufacturer | Service | Class | Enumerator | Installed
  源=HardwareDeviceManagerPage.cpp:958-967（QTreeWidget，6 列）
- 其他设备  id=hardware.tab.other_devices
  columns: <NO TABLE>（StructuredFieldView，HardwareOtherDevicesPage.cpp:117）
- 硬件标识设置  id=hardware.tab.hwid
  columns: 目标 | 驱动对象 | Active | LastStatus | DriverObject | OriginalDispatch | CurrentDispatch
  源=HardwareHwidDispatchPage.cpp:292-301（7 列）
- 设备驱动链  id=hardware.tab.device_stack
  columns: Profile | 行类型 | 角色 | 状态 | 风险 | 置信度 | 链路深度 | 附加深度 | 驱动 | 服务 | ImagePath | 设备 | DriverObject | DeviceObject/AttachedDevice | Attached/NextAttached | NextDevice/Next | OwnerDriver | DeviceType | DeviceFlags | StackSize | Alignment | FieldFlags | LastStatus | IntegrityStatus | IntegrityRows | Modules | IntegrityFlags | 备注
  源=HardwareDock.cpp:193-225 hardwareDeviceAuditTableHeaders()（28 列，createDeviceAuditTable 于 625-627 应用）
- 键盘与鼠标  id=hardware.tab.keyboard_mouse
  columns: Profile | 行类型 | 角色 | 状态 | 风险 | 置信度 | 链路深度 | 附加深度 | 驱动 | 服务 | ImagePath | 设备 | DriverObject | DeviceObject/AttachedDevice | Attached/NextAttached | NextDevice/Next | OwnerDriver | DeviceType | DeviceFlags | StackSize | Alignment | FieldFlags | LastStatus | IntegrityStatus | IntegrityRows | Modules | IntegrityFlags | 备注
  源=HardwareDock.cpp:8314 复用 createDeviceAuditPage（同一 28 列表头）
- i8042prt 审计  id=hardware.tab.i8042_audit
  columns: 设备类型 | PnP / Hardware ID | 端点 | 地址 | Context | ClassDeviceObject | 归属模块 | 执行节 | 同设备栈 | 结论 | 状态 | 证据
  源=HardwareI8042AuditPage.cpp:249-262（12 列，hardware.i8042.header.* 的 i8042Text 中文兜底）
- USB 设备  id=hardware.tab.usb
  columns: Profile | 行类型 | 角色 | 状态 | 风险 | 置信度 | 链路深度 | 附加深度 | 驱动 | 服务 | ImagePath | 设备 | DriverObject | DeviceObject/AttachedDevice | Attached/NextAttached | NextDevice/Next | OwnerDriver | DeviceType | DeviceFlags | StackSize | Alignment | FieldFlags | LastStatus | IntegrityStatus | IntegrityRows | Modules | IntegrityFlags | 备注
  源=HardwareDock.cpp:8351 复用 createDeviceAuditPage
- 系统总线  id=hardware.tab.system_bus
  columns: <NO TABLE>（createReadOnlyFieldPage → StructuredFieldView Presentation::Tree，HardwareDock.cpp:8367）
- 底层硬件检查  id=hardware.tab.r0_evidence
  columns: 类别 | CPU/Vector | 来源 | 状态 | 对象/寄存器 | 目标/入口 | Owner模块 | 风险 | 置信度 | CR0 | CR4 | EFER | MSR_LSTAR | SYSENTER_EIP | IDTR base | IDTR limit | GDTR base | GDTR limit | Selector | Attr | FieldMask | StatusFlags | RiskScore | 备注
  源=HardwareR0EvidencePage.cpp:1185-1210（QAbstractTableModel::headers，24 列，模型 headerData 走 ks::i18n::displayText）
