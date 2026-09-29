# PFN 物理内存归因

入口：内存工具 → 系统内存审计 → **PFN 深度归因**。快速快照是独立视图；其“快照未解释余量”不等于逐页扫描后的未知页。进入 PFN 或 Hyper-V / 宿主内存页时暂停快速快照的自动采集，返回其它审计页后恢复。

## 分类账

扫描先获取 NT RAM 范围，排序并合并重叠区间。每批最多查询 4096 个 4 KiB PFN，每页只计一次。使用 Memory Manager 提供的身份记录，不读取硬编码 `_MMPFN` 偏移、不修改页表、不锁定待扫描的物理页。

主分类包括进程私有、映像、映射文件/缓存、可共享节、页表、分页池、非分页池、系统 PTE、会话私有、文件系统元数据、AWE、驱动锁定、内核栈、压缩进程私有及未知用途。清零和空闲页没有所有者。待机页的后备对象可供解释，但它们属于 Available，不再次计入占用。

分区恒等式：

```
NT RAM 范围
  = Available（Zeroed + Free + Standby）
  + 各主分类的占用（Active + Modified + ModifiedNoWrite + Transition）
  + Bad
  + 查询失败
  + 未扫描
```

“真正未知”只表示成功查询、但用途代码尚未识别的占用页。它不是查询失败、缺少文件名或未扫描页的替代名称。即使未知为零，也不代表已知道每个文件名、内核分配器或安全世界内部用途。

原生查询受权限限制时，客户端可以回退到新驱动的 `QUERY_PFN_BATCH`。旧驱动、权限不足、ABI 不支持、连续查询失败和 RAM 热增减均显示具体状态及覆盖缺口。驱动保留现有 OS build 支持门禁。

## 可视化与明细

- 比例条显示整个 NT RAM 账目；下面的横向排行便于比较大项。点击分类查看 PFN 样本。
- 状态矩阵同时显示分类和 Active/Standby/Modified 等页面状态，数值可排序。
- 归属排行按去重后的物理字节排序；双击对象检查首个 PFN。界面最多显示 300 个匹配对象，导出保留全部已收集的分组。
- 每类最多保留 256 个 PFN 样本，但统计扫描覆盖整个范围；分组最多保留 65536 个，超出后仍计入总账并报告分组缺口。
- PFN 输入是页帧号；物理字节地址为 `PFN * 4096`。单页检查是新采样，会显示自己的时间。

## 进程映射与重叠属性

**解析 PFN 映射**是独立的可取消操作，需要新驱动。它枚举可访问进程的工作集，经只读 PTE walker 批量翻译 VA→PFN，再结合 `VirtualQueryEx`、`GetMappedFileNameW`、`QueryWorkingSetEx` 获取文件、页大小、锁定和共享属性。请求携带进程创建时间，驱动前后核对 PID 身份；每批最多 256 项并有约 50 ms 工作预算。

映射可以一页对应多个 PID/VA。按 PFN 排序后的引用表保留这一关系，不把引用重新加进物理总账。大页、锁定页、多重映射页各自按 PFN 去重，仅作为子集统计。Windows ShareCount 有上限，不能据此声称拿到了全部引用。

文件路径会通过有界的原生 PFN 身份批量复核，按文件标识回填到物理占用排行与导出分组。不同采样时刻不直接用 PFN 号推断文件归属；回填的路径仍属于带时间戳的映射观测，不是冻结的全系统状态。

映射扫描默认预算 90 秒，可设置 15–600 秒，最多保留约两百万条引用。不可访问进程、查询失败、取消和预算耗尽都会报告。普通工作集接口可能漏掉 AWE、大页及受保护映射，因此该统计是已观测下限，不是完整的大页/引用普查。

## 证据边界

- 分批扫描不是原子快照。记录开始/结束时间、前后物理总量和 Available；二者变化不能直接当作泄漏。
- 原始 PFN 用途可确认后备类型，但文件路径依赖另一次映射观测；没有路径的文件键仍属于已知用途。
- 压缩分类只对匹配 `MemCompression` 私有来源的 PFN 生效，不把整个 System 工作集或压缩逻辑大小当作物理存储。
- Hypervisor 存在和 Secure Kernel 运行状态可以查询；它们的具体物理占用和 VTL1 内部语义不能由该接口可靠获得。因此不把残差、地址空洞或 Driver Locked 自动改名为 Hyper-V/VBS。
- 已安装 RAM 减 Windows 可用物理总量只是保留内存总量估计。PFN 范围与 Windows 可用总量的差异另行报告，不伪造每个 firmware 页的归属。
- PFN 数据库、MDL 总数、系统页表提交量等原有快照计数继续作为诊断证据，不能与逐页主分类再次相加。

## Hyper-V 分区与宿主证据

入口：内存工具 → 系统内存审计 → **Hyper-V / 宿主内存** → **采集 Hyper-V 证据**。此入口独立于 PFN 扫描，不要求先完成 PFN 扫描，也不加载驱动。提供程序自身的权限要求仍适用。

页面包含分区占用比例图、可点击的分区排行、宿主计数器、进程视图、数据源覆盖情况和快照对照。重复采集可比较同一分区身份的 VID 变化量；GUID 或已知运行时 ID 变化时不延续旧实例的变化量。选择分区可查看 GUID、运行时 ID、HCS Owner、计数器路径、采集时间及原始 HCS 内存属性。

| 数据源 | 用途 | 计数边界 |
| --- | --- | --- |
| VID `Physical Pages Allocated` | 按分区统计分配的物理页，4 KiB/页 | 图表仅汇总非 `_Total` 实例；远端 NUMA 页是子集 |
| HCS 枚举及 Memory 属性 | 发现 WSL、容器实用虚拟机及 Owner、RuntimeId、HostingSystemId | 节点 `MemoryUsageInPages` 可核对 VID，不能与其相加；容器工作集不另加进 VM 分配 |
| Hyper-V WMI | 常规 VM 名称、状态、工作进程 PID、内存容量 | 没出现在清单中不代表没有实用虚拟机；容量不等于额外宿主占用 |
| Dynamic Memory VM | 物理分配与来宾可见容量 | 与 VID 重叠；实用虚拟机可能没有实例 |
| Hypervisor Total / Deposited | Hypervisor 总页数，以及根分区、子分区存入的页 | Total 已含存入页，不再叠加各分项 |
| GPA / Virtual TLB / Balancer | 地址空间、地址翻译及可用容量的辅助证据 | 不作为额外物理分配累加 |
| 原生进程快照 | `vmmem*`、`vmwp`、`vmcompute`、`wslservice` 的工作集和私有提交 | 与 VM 分配并列对照，不按进程名称推断唯一物理页归属 |
| Device Guard WMI | VBS 状态及运行的安全服务 | 没有 VTL1 / Secure Kernel 的精确字节统计 |

身份关联优先使用精确 GUID 和 HCS RuntimeId 别名；只有名称唯一且完全匹配时才使用实例名。无法关联的 VID 分配仍显示其字节量，但标为归属未确定。GUID 冲突或重复 VID 别名会使合计和图表不可用，原始计数器仍可检查。`_Total` 与实例合计单独比较，不把差值归给某个虚拟机。

HCS 和完整的常规 WMI 清单交叉比较，可以标出“仅 HCS 可见”的计算系统。WSL Owner 的 VID 分配与 `vmmemWSL` 工作集另有直观对照；它不能识别共享 WSL 虚拟机内部的具体发行版、进程或缓存用途，也不证明内存泄漏。

采集在后台进行，可取消。预算为 60 秒，单次 HCS 等待上限为 8 秒；正在执行的 WMI 连接等系统调用不能被本线程强行中断，因此预算不是强制终止期限。最多保留 4096 个计数器和每类 512 个清单对象。取消、超时、访问拒绝、计数器缺失分别保留覆盖状态，空值不转换为零。英文 PDH 路径先解析成本机语言路径再展开实例，兼容中文 Windows。

快速快照和最近 PFN 结果在开始采集时被固定为对照上下文，保留各自时间。Hyper-V 证据不从 PFN Unknown 或 Driver Locked 中扣除数值，不把 VBS 开启自动等同于残差归属。JSON 导出包含当前/前次采样、原始计数、状态码、HCS 属性和上下文，64 位字节计数使用十进制字符串避免精度丢失。HCS AssignedMemory / ReservedMemory 保留原始值，不假设其未注明的单位。

此功能只读取计数器和查询属性，不停止虚拟机、不调用内存回收、不修改虚拟化或安全配置。

无 GUI 回归及实机证据入口：`tools/tests/hyperv_memory_probe.cpp`，复用生产采集代码。`--self-test` 覆盖 GUID/别名合并、同名歧义、重复计数保护、GUID 冲突、容器/总计不叠加、缺失值与真零值；`--output <json>` 采集当前主机证据。使用 x64 MSVC、仓库 QtCore、C++17、`/Zc:__cplusplus /permissive- /EHsc /MD /utf-8 /W4 /WX`，与 `HyperVMemoryEvidence.cpp`、`HyperVMemoryProviders.cpp` 一起编译；依赖库由源码的 pragma 和 Qt6Core.lib 提供。

接口参考：[HCS 属性 schema](https://learn.microsoft.com/en-us/virtualization/api/hcs/schemareference)、[HCS 枚举](https://learn.microsoft.com/en-us/virtualization/api/hcs/reference/hcsenumeratecomputesystems)、[HCS 属性查询](https://learn.microsoft.com/en-us/virtualization/api/hcs/reference/hcsgetcomputesystemproperties)、[本地化 PDH 计数器](https://learn.microsoft.com/en-us/windows/win32/api/pdh/nf-pdh-pdhaddenglishcounterw)、[VID 与远端 NUMA 页](https://learn.microsoft.com/en-us/windows-server/virtualization/hyper-v/manage/configure-non-uniform-memory-access)。

## 手工验收

1. 用管理员身份启动新主程序，点击 PFN 深度归因。检查有效覆盖率、失败/未扫描和账目校验；查询失败时不得显示已完成归因。
2. 在同一时段与 RAMMap 的 Use Counts 对照类型和状态，允许采样时间及类别边界差异。不要把 RAMMap 总引用数与本页唯一 PFN 数混比。
3. 点击大项、双击后备对象，检查 PFN 状态和物理地址。运行映射解析后查看同页多个 PID；确认物理总账不因引用增加而增长。
4. 扫描途中取消，验证剩余部分显示未扫描；在预算很短时运行映射解析，确认显示预算耗尽及覆盖范围。
5. 切换中英文与主题，调整窗口宽度，检查图表和数字排序。导出 JSON 检查范围、状态矩阵、分组、状态码和采集时间。
6. 在 Hyper-V / 宿主内存页采集，检查 HCS Owner、VID 与 HCS 节点内存是否对应，以及缺失实例是否显示不可用。点击分区图查看 GUID 和原始证据。
7. 在自然负载变化后重复采集，核对同一 GUID 的变化量；VM 重建或身份变化不得继承旧变化量。确认 `_Total`、容器工作集、GPA 和 Hypervisor 存入页没有重复累加到图表或 PFN 账目。

独立无 GUI 算法回归：`tools/tests/pfn_accounting_tests.cpp` 覆盖重叠范围、无效范围、未知用途、可用/坏页边界、取消后的未扫描分区和属性不重复计数。

## PFN 阶段验证（2026-09-28）

- 主程序最终 Release 构建：`BUILD_RESULT=SUCCESS`、`EXIT_CODE=0`；标准产物 `Ksword5.1/x64/Release/Ksword5.1.exe` 为 19,873,792 字节。日志：`.codex-build-logs/ksword-build-check-20260928-114631.raw.log`。
- 驱动 Release 构建通过；新增模块以 `/W4 /WX` 编译，x64 ApiValidator 输出 `Driver is 'Universal'.`，INF/CAT 校验通过。产物尚未签名，本次没有加载或替换运行中的驱动。日志：`.codex-build-logs/pfn-driver-final-build.log`。
- 逐页记账算法测试通过；中英文语言包审计、主题审计、IOCTL 功能计划覆盖检查通过。功能计划检查只验证注册与覆盖声明，不代表实际执行了新增 IOCTL。
- 当前非管理员会话的原生范围查询返回 `0xC0000022`（访问被拒绝），没有取得本机 PFN 实测结果，不能宣称本机 Unknown 已达到 MB 级。证据：`.codex-build-logs/pfn-native-probe.json`。
- 未执行 GUI 测试；真实页面交互、原生及 R0 扫描结果仍需按上面的手工验收步骤检查。

构建环境排查：如果构建宿主清空了 `PROCESSOR_ARCHITECTURE` 和 `PROCESSOR_ARCHITEW6432`，MSBuild 的工具集属性可能把工程声明的 `PreferredToolArchitecture=x64` 降为 x86，导致 LTCG 阶段出现 C1002。先核验真实 OS 架构；对于已确认的 x64 Windows，可仅在该次构建进程环境中补入 `PROCESSOR_ARCHITEW6432=AMD64`，继续使用仓库约定的 MSBuild 与 `ksword-build-check`，不更换 MSVC 版本或产物名称。应以最终构建退出码判断成功，不能把链接器自动重试后产生了 exe 当成整体成功。

## Hyper-V 阶段实测（2026-09-28）

- 集成主程序 Release 构建通过：`BUILD_RESULT=SUCCESS`、`EXIT_CODE=0`。标准 exe 为 20,081,152 字节，SHA256 `29BD80A6FAE69E6E5C5A165497408B1511F004AF59428953E381A8FD88B7CDA1`；日志：`.codex-build-logs/ksword-build-check-20260928-130841.raw.log`。本阶段没有修改驱动。
- 生产采集代码通过独立无 GUI 探针以 `/W4 /WX` 编译；身份关联回归通过。主程序语言包审计通过（26657 条源文本），主题源码审计通过（752 个源文件）。
- 17:06:31–17:06:32 UTC 的本机只读采样发现一个 HCS Owner 为 WSL 的运行中虚拟机，完整的普通 Hyper-V WMI 清单没有该实例。
- VID 和 HCS 虚拟节点分别报告 **4,151,296 页 = 17,003,708,416 字节 = 15.84 GiB**，二者一致；VID `_Total` 也相符。同一采集区间的 `vmmemWSL` 工作集为 **1,405,095,936 字节，约 1.31 GiB**。这是 VM 分配与进程工作集的口径差异，不是已经定位了来宾内部泄漏。
- Hypervisor 总页数计数为 **335,654,912 字节，约 320 MiB**，该样本恰好等于根分区与子分区存入页之和，不重复累加。VBS 状态为运行中，没有据此估算安全内核的占用字节。
- 21 项计数器中 19 项有效；WSL 的 Dynamic Memory VM 两项没有实例，正确保留为不可用。WMI、HCS 属性和原生进程快照均读取成功。
- 原始证据：`.codex-build-logs/hyperv-host-evidence.json`，SHA256 `76088C418DF5943DA325D3C580A8573DFD31C09ED8A91822434C50363B1A9FF1`。此文件是当时采样，不表示后续时刻的 VM ID 或用量仍相同。
- 没有停止 WSL/虚拟机、修改系统配置或执行 GUI 测试；新页面交互仍按手工验收检查。

参考：[PHNT 原生内存接口](https://github.com/winsiderss/phnt/blob/master/ntmmapi.h)、[PHNT Superfetch ABI](https://github.com/winsiderss/phnt/blob/master/ntpfapi.h)、[Microsoft RAMMap](https://learn.microsoft.com/en-us/sysinternals/downloads/rammap)。裁剪后的原生 ABI 声明位于 `third_party/systeminformer_dyn/PfnNative.h`，保留该目录的 MIT 许可证。
