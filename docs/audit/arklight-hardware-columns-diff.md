# ARKLight 硬件页列 vs Qt 真值

Qt 真值来源：`docs/audit/qt-hardware-map.md`
ARKLight 来源：`KswordARKLight/Features/`（Hardware/、HardwareStats/，以及 Hardware/ 内嵌的 Kernel 单功能页）

页签装配顺序见 `KswordARKLight/Features/Hardware/HardwareFeature.cpp:61-117`（`CreateHardwareFeaturePage`，共 12 个页签）。

---

## 页签覆盖总览

Qt 顶层页16 个，ARKLight 顶层页 12 个。

| Qt 顶层页 | ARKLight 页签 | 备注 |
| --- | --- | --- |
| 性能监控 | 性能监控 | 有页 |
| 硬件概览 | — | **ARKLight 无此页** |
| 处理器 | 处理器 | 有页（内嵌 Kernel 页） |
| 电源 | — | **ARKLight 无此页** |
| 显卡 | — | **ARKLight 无此页** |
| 内存 | 内存 | 有页（内嵌 Kernel 页） |
| 磁盘活动 | 磁盘活动 | 有页 |
| 设备管理 | 设备管理 | 有页 |
| 其他设备 | — | **ARKLight 无此页** |
| 硬件标识设置 | 硬件标识设置 | 有页 |
| 设备驱动链 | — | **ARKLight 无此页** |
| 键盘与鼠标 | — | **ARKLight 无此页** |
| i8042prt 审计 | i8042prt 审计 | 有页（内嵌 Kernel 页） |
| USB 设备 | USB 设备 | 有页 |
| 系统总线 | 系统总线 | 有页 |
| 底层硬件检查 | 底层硬件检查 | 有页（内嵌 Kernel 页） |

ARKLight 独有页（Qt 无对应页）：**物理磁盘扇区**（`kRawDiskTab`）、**系统计时状态**（`kSystemTimeTab`）。

Qt 性能监控下的 6 个二级页（CPU / 内存 / 磁盘N / 以太网N / 虚拟网络 / GPU N）在 ARKLight 里**全部没有对应实现**，性能监控是单层页。

---

## 性能监控

ARKLight: 源 `HardwareStats/PerformanceView.cpp:367-372`（`state.metricList.addColumns`）
ARKLight columns: `分组` | `指标` | `当前值` | `计数器路径`（4 列）
Qt columns: `<NO TABLE>`（顶层页无表；6 个二级页也全是 `<NO TABLE>`，Qt 用左侧 QListWidget 设备卡片 + QStackedWidget 详情页）
差异: **不是列名差异，是结构差异。** Qt 这一页完全没有表格控件，ARKLight 有一张 4 列指标表。同时 Qt 的 6 个二级页（逐 CPU / 逐磁盘 / 逐网卡 / 虚拟网络 / 逐 GPU）在 ARKLight 无对应物——ARKLight 把它们压平成一张按`分组`聚合的指标表。
附带: Qt 二级页 CPU 有利用率语义，ARKLight 的 `指标`+`当前值` 两列承载部分利用率信息，但没有逐逻辑处理器粒度。

---

## 硬件概览

ARKLight: 无此页
ARKLight columns: `<NO TABLE>`（页签不存在）
Qt columns: `<NO TABLE>`（StructuredFieldView Presentation::Tree，名称/值，表头隐藏）
差异: 两侧都无表格列，Qt 是结构化字段树，ARKLight 整页缺失。

---

## 处理器

ARKLight: 源 `Hardware/HardwareFeature.cpp:68-71` → `Kernel::KernelFeatureId::CpuHardwareSnapshot`，列由 `KernelPage` 动态投影
ARKLight columns: `<DYNAMIC>`
决定列的地方: `Kernel/KernelProjectionValues.inl:1931-1938`（`snapshot.columns` = `"#"` + `CanonicalColumnNames(CpuHardwareSnapshot)` + 结果行里出现的**全部 raw 字段 key** + `"Detail"`），raw 字段 key 来自 `Kernel/KernelFacade.cpp:3776-3807`（`QueryCpuHardwareSnapshot` 的 `Row({...})`）。
固定部分（`Kernel/KernelPageLayout.cpp:262-263`）只有 4 列：`项目` | `值` | `摘要` | `状态`，实际运行还会前置一列 `L"#"`（`KernelProjectionValues.inl:1931`）。
Qt columns: 逻辑处理器 | 利用率(%) | 当前频率(MHz) | 最大频率(MHz) | 限频(MHz) | 温度 | 电压（7 列）
差异:
- 缺 Qt 列（7/7 全缺）：`逻辑处理器`、`利用率(%)`、`当前频率(MHz)`、`最大频率(MHz)`、`限频(MHz)`、`温度`、`电压`。Qt 是逐逻辑处理器一行的频率/温度表，ARKLight 是单行 CPUID 快照表，语义完全不同。
- 多 ARKLight 列：`#`、`项目`、`值`、`摘要`、`状态`，以及 `QueryCpuHardwareSnapshot` 返回的全部诊断字段（`Unsupported`、`Version`、`FieldFlags`、`Vendor`、`Brand`、`Logical`、`Active`、`Package`、`Family`、`Model`、`Stepping`、`ProcessorType`、`BrandIndex`、`CLFlushLine`、`InitialApicId`、`MaxBasicLeaf`、`MaxExtendedLeaf`、`Leaves`、`FeatureMask`、`Features`、`Leaf1ECX`、`Leaf1EDX`、`Leaf7EBX`、`Leaf7ECX`、`Leaf7EDX`、`Leaf80000001ECX`、`Leaf80000001EDX`、`LastStatus`）与 `Detail`。
- 顺序：不适用（列集无交集）。

---

## 电源

ARKLight: 无此页
ARKLight columns: `<NO TABLE>`（页签不存在）
Qt columns: 项目 | 当前值（2 列）
差异: 整页缺失。

---

## 显卡

ARKLight: 无此页
ARKLight columns: `<NO TABLE>`（页签不存在）
Qt columns: `<NO TABLE>`（StructuredFieldView）
差异: 两侧都无列定义，ARKLight 整页缺失。

---

## 内存

ARKLight: 源 `Hardware/HardwareFeature.cpp:73-76` → `Kernel::KernelFeatureId::PhysicalMemoryLayout`
ARKLight columns: `<DYNAMIC>`
决定列的地方: 同处理器，`KernelProjectionValues.inl:1931-1938`。
固定部分（`KernelPageLayout.cpp:264-265`）5 列：`范围` | `总物理内存` | `最高物理地址` | `最大连续Range` | `状态`，前置 `L"#"`。
Qt columns: `<NO TABLE>`（StructuredFieldView 名称/值）
差异: Qt 无表，ARKLight 有 6+列表。结构差异，非列名差异。

---

## 磁盘活动

ARKLight: 源 `HardwareStats/DiskActivityView.cpp:357-368`（`state.diskList.addColumns`）
ARKLight columns: `物理磁盘` | `读取吞吐` | `写入吞吐` | `读 IOPS` | `写 IOPS` | `当前队列` | `平均队列` | `忙碌比例` | `读延迟` | `写延迟`（10 列）
Qt columns:
- `[进程表]` 选择 | 进程 | PID | 读字节/s | 写字节/s | 总字节/s | 响应时间 | 读次数/s | 写次数/s | 路径（10 列）
- `[活动表]` 进程 | PID | 文件 | 读(字节/秒) | 写(字节/秒) | 总数(字节/秒) | I/O 优先级 | 响应时间(ms)（8 列）
- `[存储表]` 驱动器 | 卷标 | 文件系统 | 活动时间 | 可用空间 | 总空间 | 读(字节/秒) | 写(字节/秒) | 响应时间(ms) | 队列长度（10 列）
差异:
- **缺整张进程表**（10 列全缺）：`选择`、`进程`、`PID`、`读字节/s`、`写字节/s`、`总字节/s`、`响应时间`、`读次数/s`、`写次数/s`、`路径`。ARKLight 没有进程级 I/O 归属这一维度。
- **缺整张活动表**（8 列全缺）：`进程`、`PID`、`文件`、`读(字节/秒)`、`写(字节/秒)`、`总数(字节/秒)`、`I/O 优先级`、`响应时间(ms)`。
- **缺整张存储表**（10 列全缺）：`驱动器`、`卷标`、`文件系统`、`活动时间`、`可用空间`、`总空间`、`读(字节/秒)`、`写(字节/秒)`、`响应时间(ms)`、`队列长度`。
- 多 ARKLight 列（7）：`读 IOPS`、`写 IOPS`、`当前队列`、`平均队列`、`忙碌比例`、`读延迟`、`写延迟`（Qt 三张表都没有 IOPS / 忙碌比例 / 独立延迟列）。概念相近但列名不匹配：Qt `读次数/s`↔ARKLight `读 IOPS`，Qt `队列长度`（存储表）↔ARKLight `当前队列`/`平均队列`。
- 顺序：不适用（列集几乎无交集）。
- 补充: ARKLight 这一页只有一张 ListView，没有 Qt 那样的三表分栏布局。

---

## 设备管理

ARKLight: 源 `Hardware/HardwareView.cpp:727-741`
ARKLight columns: `<NO TABLE>`（主控件）+ `属性` | `值`（详情表）
控件构成:
- 左侧 `WC_TREEVIEWW`（`HardwareView.cpp:727-729`）——Win32原生树，**单列无表头**，Qt 那6 个树列在这里一个都没有。节点数据放在 `TVINSERTSTRUCTW::item.lParam`（`HardwareView.cpp:258-266`）。
- 右侧 `WC_LISTVIEWW` report 模式（`HardwareView.cpp:730-732`），列由 `AddListColumn` 建立（`HardwareView.cpp:97-104`），实际标题在 `HardwareView.cpp:740-741`：`属性`（宽 180）| `值`（宽 560）。
Qt columns: `<TREE>`，tree_headers = `Name` | `Manufacturer` | `Service` | `Class` | `Enumerator` | `Installed`（6 列）
差异:
- 缺 Qt 树列（6/6 全缺）：`Name`、`Manufacturer`、`Service`、`Class`、`Enumerator`、`Installed`。Qt 是 QTreeWidget 6 列表格树，ARKLight 是无表头的 TVS 树——所有字段信息只能在选中节点后从右侧 `属性`/`值` 明细里逐条读。
- 多 ARKLight 列：`属性`、`值`。这两列 Qt 的设备管理页没有（Qt 直接把6 个字段平铺成树列）。
- 顺序：不适用（列集无交集）。
- 明细行键（`HardwareView.cpp:298-308`）能看到 ARKLight 用 `选择` / `设备` / `审计模式` / `R0 设备栈协议` / `R0 输入栈协议` / `R0 USB 拓扑协议` / `输入隐私` / `审计分类` 以及后续设备字段充当 `属性` 行的标签，这与 Qt 的 6 个树列不是一一对应关系。

---

## 硬件标识设置

ARKLight: 源 `Hardware/HardwareHwidDispatchView.cpp:535-547`（`CreateReadOnlyAuditTable`，列定义在 540-546 行）
ARKLight columns: `目标` | `驱动对象` | `Active` | `LastStatus` | `DriverObject` | `OriginalDispatch` | `CurrentDispatch`（7 列）
Qt columns: 目标 | 驱动对象 | Active | LastStatus | DriverObject | OriginalDispatch | CurrentDispatch（7 列）
差异: **完全一致**，列名逐字相同、顺序相同、数量相同。仅列宽不同（ARKLight 150/190/70/110/150/150/150）。

---

## 设备驱动链

ARKLight: 无此页
ARKLight columns: `<NO TABLE>`（页签不存在）
Qt columns: 28 列（Profile | 行类型 | 角色 | 状态 | 风险 | 置信度 | 链路深度 | 附加深度 | 驱动 | 服务 | ImagePath | 设备 | DriverObject | DeviceObject/AttachedDevice | Attached/NextAttached | NextDevice/Next | OwnerDriver | DeviceType | DeviceFlags | StackSize | Alignment | FieldFlags | LastStatus | IntegrityStatus | IntegrityRows | Modules | IntegrityFlags | 备注）
差异: 整页缺失，28 列全缺。

---

## 键盘与鼠标

ARKLight: 无此页
ARKLight columns: `<NO TABLE>`（页签不存在）
Qt columns: 28 列（与「设备驱动链」同一表头）
差异: 整页缺失，28 列全缺。ARKLight 硬件区没有任何页面复用这28 列表头。

---

## i8042prt 审计

ARKLight: 源 `Hardware/HardwareFeature.cpp:92-95` → `Kernel::KernelFeatureId::I8042Audit`
ARKLight columns: `<DYNAMIC>`
决定列的地方: `I8042Audit` **不在** `CanonicalColumnNames`（`KernelPageLayout.cpp:205-301`，走 `default: return {}`），**也不在** `IsR0EvidenceFeature`（`KernelPage.cpp:224-240`）与 `KernelUsesValueProjection`（`KernelPage.cpp:1448-1470`）。因此落到通用路径 `KernelPage.cpp:5420` `RebuildResultListFromCache()` → `AddDynamicResultColumns`（`KernelPage.cpp:5470-5500`），列 = 结果行实际出现的 key 顺序 + 末尾补 `Detail`。
实际 key 来自 `Kernel/KernelFacade.cpp:4370-4415`（`QueryI8042Audit`）：
- 汇总行：`Status`、`Unsupported`、`Version`、`QueryStatus`、`ResponseFlags`、`DescriptorId`、`ImageBase`、`ImageSize`、`ImageTimeDateStamp`、`PdbAge`、`Total`、`Returned`、`LastStatus`
- 端点行：`PnpId`、`DeviceKind`、`EndpointKind`、`Verdict`、`Callback`、`OwnerModule`、`DeviceObject`、`Status`
- 末尾 `Detail`（`KernelPage.cpp:5496-5498`）
Qt columns: 设备类型 | PnP / Hardware ID | 端点 | 地址 | Context | ClassDeviceObject | 归属模块 | 执行节 | 同设备栈 | 结论 | 状态 | 证据（12 列）
差异:
- 缺 Qt 列（12/12 全缺）：`设备类型`、`PnP / Hardware ID`、`端点`、`地址`、`Context`、`ClassDeviceObject`、`归属模块`、`执行节`、`同设备栈`、`结论`、`状态`、`证据`。
  - 部分信息在 ARKLight 里被塞进了 `Detail` 多行文本（`KernelFacade.cpp:4390-4404` 逐行拼 `PnpId:` / `DeviceKind:` / `EndpointKind:` / `Verdict:` / `DeviceObject:` / `ClassDeviceObject:` / `Callback:` / `Context:` / `ModuleBase:` / `ModuleSize:` / `OwnerModule:` / `DetailCode:` / `LastStatus:`），但不是列，无法排序/导出成列。
  - `同设备栈`（递归栈深度）在 ARKLight 完全没有对应数据源。
- 多 ARKLight 列：上述全部 raw key + `Detail`。
- 顺序：不适用（列集无交集）。

---

## USB 设备

ARKLight: 源 `HardwareStats/UsbTopologyView.cpp:510-522`（主表 `state.nodeList.addColumns`）+ `531-532`（详情表）
ARKLight columns:
- 主表（11 列）：`设备` | `类型` | `VID` | `PID` | `序列号` | `端口` | `驱动服务` | `设备类` | `状态` | `位置信息` | `实例 ID`
- 详情表（2 列）：`属性` | `值`
Qt columns: 28 列驱动审计表头（Profile | 行类型 | 角色 | 状态 | 风险 | 置信度 | 链路深度 | 附加深度 | 驱动 | 服务 | ImagePath | 设备 | DriverObject | DeviceObject/AttachedDevice | Attached/NextAttached | NextDevice/Next | OwnerDriver | DeviceType | DeviceFlags | StackSize | Alignment | FieldFlags | LastStatus | IntegrityStatus | IntegrityRows | Modules | IntegrityFlags | 备注）
差异:
- 缺 Qt 列（28 列中26 列缺）：`Profile`、`行类型`、`角色`、`风险`、`置信度`、`链路深度`、`附加深度`、`驱动`、`服务`、`ImagePath`、`设备`、`DriverObject`、`DeviceObject/AttachedDevice`、`Attached/NextAttached`、`NextDevice/Next`、`OwnerDriver`、`DeviceType`、`DeviceFlags`、`StackSize`、`Alignment`、`FieldFlags`、`LastStatus`、`IntegrityStatus`、`IntegrityRows`、`Modules`、`IntegrityFlags`、`备注`。
  - 唯一名字对得上的是 `状态`（Qt 第 4 位，ARKLight 第 9 位），但语义不同：Qt 是驱动行状态，ARKLight 是 USB 设备状态。
- 多 ARKLight 列（12）：`设备`、`类型`、`VID`、`PID`、`序列号`、`端口`、`驱动服务`、`设备类`、`状态`、`位置信息`、`实例 ID`、`属性`、`值`。
- 顺序差异：`状态` 在 Qt 是第 4 位，在 ARKLight 是第 9 位。
- 根本差异：Qt 这一页复用驱动审计表（设备栈/派发/完整性视角），ARKLight 是 USB 拓扑枚举视角（VID/PID/序列号/端口/位置），两者是**不同的数据模型**，不是列裁剪问题。

---

## 系统总线

ARKLight: 源 `HardwareStats/BusDeviceView.cpp:510-523`（主表 `state.busList.addColumns`）+ `532-533`（详情表）
ARKLight columns:
- 主表（12 列）：`设备` | `枚举器` | `总线类型` | `传统总线类型` | `总线号` | `地址` | `槽位` | `位置信息` | `已分配资源` | `驱动服务` | `状态` | `实例 ID`
- 详情表（2 列）：`属性` | `值`
Qt columns: `<NO TABLE>`（`createReadOnlyFieldPage` → StructuredFieldView Presentation::Tree，名称/值）
差异: Qt 无表，ARKLight 有 12+2 列表。结构差异，非列名差异。

---

## 底层硬件检查

ARKLight: 源 `Hardware/HardwareFeature.cpp:103-106` → `Kernel::KernelFeatureId::KernelCpuIntegrity`，投影在 `KernelProjectionValues.inl:906-919`（`RebuildIntegrityEvidenceListFromCache`）
ARKLight columns:
- **可见列（9）**：`#` | `类别` | `CPU/Vector` | `对象/寄存器` | `目标/入口` | `Owner模块` | `风险` | `置信度` | `Detail`
  - 组成：`L"#"`（`KernelProjectionValues.inl:907`）+ `CanonicalColumnNames(KernelCpuIntegrity)` 8 列（`KernelPageLayout.cpp:260-261`）
- **隐藏列（23，宽 0）**：`Class`、`ClassText`、`Risk`、`RiskText`、`Source`、`SourceMask`、`SourceText`、`Confidence`、`Group`、`CPU`、`Vector`、`CpuVector`、`Object`、`ObjectAddress`、`Target`、`TargetAddress`、`OwnerBase`、`OwnerModuleBase`、`OwnerSize`、`OwnerModuleSize`、`OwnerModuleSizeText`、`OwnerModule`、`LastStatus`
  - 来源 `KernelProjectionValues.inl:911-919`；宽度置 0 由 `KernelProjectionValues.inl:1013`（`IsIntegrityHiddenColumn`）
Qt columns: 类别 | CPU/Vector | 来源 | 状态 | 对象/寄存器 | 目标/入口 | Owner模块 | 风险 | 置信度 | CR0 | CR4 | EFER | MSR_LSTAR | SYSENTER_EIP | IDTR base | IDTR limit | GDTR base | GDTR limit | Selector | Attr | FieldMask | StatusFlags | RiskScore | 备注（24 列）
差异:
- 缺 Qt 列（17）：`来源`、`状态`、`CR0`、`CR4`、`EFER`、`MSR_LSTAR`、`SYSENTER_EIP`、`IDTR base`、`IDTR limit`、`GDTR base`、`GDTR limit`、`Selector`、`Attr`、`FieldMask`、`StatusFlags`、`RiskScore`、`备注`。
  - `来源` / `状态` 在 ARKLight 里有对应 raw 字段（`Source`/`SourceText`、`LastStatus`），但被归入隐藏列（宽 0），**UI 上不可见**。
  - `备注` 在 Qt 是最后一列；ARKLight 有一个语义相近但**列名不同**的 `Detail`（第 9 位）。
  - 全部 8 个控制寄存器列（`CR0`/`CR4`/`EFER`/`MSR_LSTAR`/`SYSENTER_EIP`/`IDTR base`/`IDTR limit`/`GDTR base`/`GDTR limit`）在 ARKLight 完全不存在——这是 Qt 这一页的核心内容（描述符表与 LSTAR 完整性），ARKLight 没有任何对应列或数据源。
- 多 ARKLight 列：`#`（行号helper，Qt 无）、`Detail`（Qt 叫 `备注`）。
- 顺序差异（可见列）：
  - `对象/寄存器`：Qt 第 **5** 位，ARKLight 第 **4** 位
  - `目标/入口`：Qt 第 **6** 位，ARKLight 第 **5** 位
  - `Owner模块`：Qt 第 **7** 位，ARKLight 第 **6** 位
  - `风险`：Qt 第 **8** 位，ARKLight 第 **7** 位
  - `置信度`：Qt 第 **9** 位，ARKLight 第 **8** 位
  - 原因：Qt 在 `CPU/Vector` 与 `对象/寄存器` 之间插了 `来源`、`状态` 两列，ARKLight 没有这两列（它们被隐藏），所以后5 列整体前移 2 位。

---

## ARKLight 独有页（Qt 无对应真值）

以下两页在 Qt 顶层 16 页里没有对应项，无法做列比对。

### 物理磁盘扇区
ARKLight: 源 `Hardware/HardwareFeature.cpp:81-84` → `Kernel::KernelFeatureId::RawDiskSectors`
ARKLight columns: `<DYNAMIC>`（`RawDiskSectors` 不在 `CanonicalColumnNames`，落`RebuildResultListFromCache` 动态路径，列由 `KernelFacade.cpp:QueryRawDiskSectors` 的行 key 决定）

### 系统计时状态
ARKLight: 源 `Hardware/HardwareFeature.cpp:108-111` → `Kernel::KernelFeatureId::SystemTimeState`
ARKLight columns: `<DYNAMIC>`（同上，动态路径）

---

## 无法确认

1. **Qt 缺失 6 页的列无法核对**：硬件概览、电源、显卡、其他设备、设备驱动链、键盘与鼠标 在 ARKLight 里根本没有页签，对应 View 文件不存在，无法提取列定义。其中「电源」「设备驱动链」「键盘与鼠标」Qt 侧有明确列（2 列 / 28 列 / 28 列），属于确定的整页缺失；「硬件概览」「显卡」「其他设备」Qt 侧本身也是 `<NO TABLE>`，只是页面缺失。
2. **Qt 性能监控 6 个二级页无法核对**：ARKLight 无对应实现（无子页结构），无法提取 CPU / 内存 / 磁盘N / 以太网N / 虚拟网络 / GPU N 各页的列定义。Qt 侧这些二级页本身标记为 `<NO TABLE>`，但其子控件构成（QListWidget 卡片 + StackedWidget 详情）的字段集在 Qt 真值文档里没有记录。
3. **内嵌 Kernel 页的「运行时实际可见列」无法完全静态确定**：处理器 / 内存 / i8042prt 审计 / 底层硬件检查 四页的列由 `CanonicalColumnNames()` 固定前缀 + 结果行 raw key 动态拼接而成。raw key 取决于驱动实际返回的字段集合，静态只能读到 `KernelFacade.cpp` 里构造 `Row({...})` 时列出的那些 key；驱动侧若新增字段，UI 会自动多出列。
   - 已完整抄录：i8042prt 审计（`KernelFacade.cpp:4370-4415`）、内存（`KernelFacade.cpp:4710-4734`，止于 `LastStatus`）、处理器（`KernelFacade.cpp:3776-3808`，止于 `LastStatus`）。
   - 未能完全确定：驱动运行时实际返回的字段集合可能与 facade 构造的 key 不完全一致（facade 只映射它关心的字段），因此运行时列数可能少于本报告列出的 key 数，也可能多出 driver 侧新字段。
4. **「物理磁盘扇区」「系统计时状态」两页的动态列未展开**：`KernelFacade.cpp` 的 `QueryRawDiskSectors` / `QuerySystemTimeState` 行 key 列表本次未逐行抄录，需要时再补。
5. **Qt 侧列顺序的真值粒度**：`qt-hardware-map.md` 对「磁盘活动-进程表」记录了 `moveSection` 造成的可视顺序调整（进程移到可视第 2 位）、「活动表」把「进程」移到可视第 1 位。本报告的顺序比对以文档记录的 **setHorizontalHeaderLabels 逻辑顺序** 为准，未展开 Qt 的运行时可视重排。