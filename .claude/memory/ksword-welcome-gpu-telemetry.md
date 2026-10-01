# 欢迎页采样与 GPU 遥测蓝屏

## 现场证据与归因边界

- `092826-16203-01.dmp` 是 x64 Kernel Generated Triage Dump。匹配微软符号后确认 `0x3B SYSTEM_SERVICE_EXCEPTION`、`0xC0000005`，进程为 `Ksword5.1.exe`。
- 故障在 `dxgkrnl.sys 10.0.17763.9240!DXGADAPTER::GetNodePerfData+0x8c`。指令 `sub rcx, [rax+r12*8]` 在 RAX/R12 均为零时读取地址 0；RAX 来自节点上下文 `+0x78` 的内部数组指针。
- 内核调用链为 `NtGdiDdDDIQueryAdapterInfo -> DxgkQueryAdapterInfo -> GetNodePerfData`。活动模块列表没有 KswordARK，现有栈也没有 KswordARK 驱动帧。不能因为现场是蓝屏就只追查项目 R0 功能。
- 小转储缺少完整用户栈与适配器对象，不能区分周期 GPU 采样和静态显卡摘要两个调用者，也不能确定内部数组未初始化的原因；现场 WDDM 版本未知，旧 WDDM 是兼容性缺口而不是已经证实的根因。

## 欢迎页启动链

`MainWindow` 为欢迎页创建 `HardwareDock`，`WelcomeDock::setHardwareDock` 绑定 `performanceSnapshotChanged/staticOverviewChanged` 后启动采样。用户没有打开硬件页，也可能发起 GPU 查询。

旧 `startPerformanceSampling(false)` 只关闭 R0 健康查询，仍启动每秒 GPU 遥测、静态 GPU 摘要、CPU 温度/电压探测。因此仅强调“首页没有 R0”不能隔离此故障。

## 当前约束

- `HardwareDock::SamplingScope::WelcomeOverview`：保留利用率、DXGI 容量、PDH 计数器和欢迎页静态概览；GPU 频率/节点性能、额外显卡/内存详情与 CPU 传感器探测均不启动。
- 欢迎页静态 CIM 仅查询 BaseBoard、BIOS、Processor、VideoController、NetworkAdapter、DesktopMonitor 六类；磁盘完整设备清单、声卡、PnPEntity 摄像头筛查、打印机与 USB 控制器映射留到硬件详情范围。
- `SamplingScope::HardwareDetails` 由硬件页 `showEvent` 升级。同一个采样器只升级范围；异步任务捕获投递时的范围。概览任务执行期间升级时，回投后立即补采详情，避免丢请求或用缺省摘要覆盖已采集字段。
- `queryGpuAdapterTelemetrySnapshot` 在打开适配器前拒绝 build <=17763 或无法解析的系统版本。这是针对已确认故障家族的保守规避，不表示后续 build 的所有驱动已通过实机验证。
- 打开适配器后先用 `KMTQAITYPE_DRIVERVERSION` 读取显示驱动模型版本；查询失败或低于 `KMT_DRIVERVERSION_WDDM_2_4` 时关闭句柄并返回。不得用 NODEPERFDATA/ADAPTERPERFDATA 试调用来探测支持性：内核崩溃不能由用户态 SEH、超时或独立子进程隔离。
- 节点统计失败或节点数为零时不猜测节点 0；节点预算仍为 64。静态摘要与周期 GPU 遥测共享互斥锁，串行调用可选遥测。
- 降级后 GPU 时钟继续显示已有 `N/A`，不把缺少数据显示成真实 0 MHz。DXGI 容量与 PDH 利用率仍按各自可用性采集。

## 后续现场验证

- 2026-10-01：修改涉及源文件通过 MSVC Release/x64 `ClCompile`；i18n 与主题 token 审计通过。该检查没有重新链接运行中的主程序，不代表已生成新的发行 exe。
- 临时测试从生产源码抽取 GPU helper，用模拟 D3DKMT 后端验证 11 个版本/失败/节点数量场景，均通过：17763（包括模拟 WDDM 3.2）及更早/未知系统不打开适配器；旧 WDDM 或版本查询失败不发起性能查询；节点统计失败/零节点不猜测节点 0；超量节点限制为 64。
- 从生产源码抽取概览 PowerShell 脚本，替换 CIM 后端后验证首页仅 6 类查询、详情 11 类查询，两个模式语法及查询范围检查通过。模拟测试没有调用现场显卡或真实 CIM 提供程序。

- 在原现场系统/显卡驱动组合中分别启动停留欢迎页、打开硬件页、持续性能监视，验证不再进入 17763 的节点性能查询。
- 本机编译或模拟后端检查只能证明代码路径与版本门槛，不能代替原机器的蓝屏回归。

- 2026-10-01：用户反馈修改后确实不再蓝屏。反馈尚未说明是否打开硬件 Dock，以及复测机器的 OS/显卡驱动是否与 dump 一致；记录为规避有效的用户反馈，不扩大为所有硬件详情路径已通过实机回归。
- 同日再次用 CDB 恢复 dump 异常上下文，确认 `GetNodePerfData+0x8c`、RAX/R12 为零和查询调用栈。可证实直接故障点是节点性能查询处理中的空指针读取；缺少用户栈仍不能区分周期采样与静态摘要，也不能证实图形内核内部数组为空的底层原因。
- 硬件详情在 build >17763 且 WDDM >=2.4 时仍保留 NODEPERFDATA 查询；范围升级后返回欢迎页不会降级。原 17763 现场系列的统一 helper 门槛同时覆盖静态摘要与周期采样，因此打开硬件页也不会重新启用此查询；较新系统上的版本门槛不构成无蓝屏保证。
