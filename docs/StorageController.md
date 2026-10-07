# KSword 存储控制器

入口位于文件 Tab 侧边栏第二项“存储控制器”。AHCI、NVMe 和 IDE 后端已合并到主工程，最终二进制为 `KswordARK.sys`，服务为 `KswordARK`。不再构建或发布 `KswordARKController.sys`。

## 两种启动配置

- 普通配置：服务 `Parameters` 下 `StorageControllerPnP` 缺失或为 DWORD `0`，保留原来的非 PnP 控制驱动和 SCM 启停流程。未绑定硬件时，控制器入口显示不可用，不扫描或映射未分配的 PCI 资源。
- 控制器配置：可选 `KswordARKStorageController.inf` 在手动绑定时设置 DWORD `StorageControllerPnP=1`。同一个主驱动创建控制器 PnP FDO，并在首个控制器建立后启动常规 ARK 控制端点。最后一个控制器移除后停止新请求、排空运行时，再删除控制端点；本次驱动生命周期进入退出状态。

PnP 配置通过设备管理器恢复系统驱动或移除绑定完成退出。普通“停止驱动”、SCM 删除服务和程序启动时重写驱动文件路径在此配置下被拒绝。常驻 HVM 生命周期保护在 PnP 配置下不可用；恢复普通配置并重新加载后才能使用该工作流。

## 手动绑定与恢复

仅使用专用、非启动且没有挂载卷的控制器。它不能承载 Windows、分页、崩溃转储或休眠数据。可选 INF 属于存储设备 setup class，原来的主驱动 INF 保持原有用途；两者使用同一服务和二进制。

1. 关闭现有控制器会话。若旧版本的 `KswordARKController` 已绑定设备，先在设备管理器恢复对应系统驱动。
2. 在普通配置下停止主驱动，关闭所有持有它的程序，再手动选择可选 INF。程序不会安装或自动重绑设备。
3. PnP 建立控制器后，在文件侧边栏打开“存储控制器”并刷新状态。客户端只接受服务属性为 `KswordARK` 的设备接口，跳过旧独立服务。
4. 退出控制器配置时，先释放会话并关闭驱动客户端，再在设备管理器恢复系统绑定。确认所有控制器绑定已移除且驱动已退出后，显式将 `StorageControllerPnP` 设为 DWORD `0`，再使用普通加载流程。程序不会自动清除该配置。

原始写入保留独占会话、代次、确认令牌、原数据哈希比较、flush、复读验证和条件回滚。介质写入与电源故障不构成硬件原子事务。只有 NVMe 提供可验证的受控复位，AHCI/IDE 不报告未经验证的恢复成功。

## 实现与验证

- 硬件源码：`KswordARKDriver/src/features/storage_controller/`。
- 共享协议：`shared/driver/KswordArkStorageControllerIoctl.h`。
- 四个 IOCTL 统一登记到主驱动中央注册表。控制器 FDO 仅接受控制器条目，普通控制设备拒绝这些条目；独占句柄、资源和会话仍属于具体 FDO。
- 客户端：`Ksword5.1/Ksword5.1/ArkDriverClient/ArkStorageControllerClient.*`。页面设备 I/O 在后台串行执行。
- 分配使用主驱动的兼容池分配器，合并不会给普通配置新增 `ExAllocatePool2` 静态导入。

离线回放使用真实生产事务、硬件后端、中央分发和服务管理代码，替换 KMDF、SCM、注册表、MMIO、DMA 与时间来源。CI 验证主驱动 Release 构建、两个 INF、目录文件及统一产物。编译与离线回放不能代替设备绑定、真实 DMA、PnP 移除和实机写入验收。

WDF 约束参考：[控制设备生命周期](https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/using-control-device-objects)、[驱动初始化标志](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfdriver/ne-wdfdriver-_wdf_driver_init_flags)、[PnP 卸载规则](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-zwunloaddriver)。
