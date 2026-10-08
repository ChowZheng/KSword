# PFN 物理内存归因

入口：内存工具 → 系统内存审计 → **PFN 深度归因**。快速快照是独立视图；其“快照未解释余量”不等于逐页扫描后的未知页。取得有有效页且账目闭合的扫描结果后，物理分布总览默认切到“最新 PFN 分类账”，卡片、比例图和分类树统一使用该次扫描的 NT RAM 分母、状态和采集区间；可随时切回“快速快照”。Commit 仍单独标注快速快照来源与时间。

总览把未知用途与查询失败、未扫描分开显示，已知用途下按唯一在用页列出最大的后备对象子集。缺少进程或文件名称不会使已知用途重新成为未知。失败重扫保留上次有效分类账并明确提示；取消、部分覆盖、RAM 范围变化不会伪装成完成归因。点击快速快照图可启动逐页归因；点击 PFN 分类可进入对应物理页样本。进入 PFN 或 Hyper-V / 宿主内存页时暂停快速快照的自动采集，返回其它审计页后恢复。

快速快照各计数器分别采样。分类之和大于采样在用 RAM 时显示计数超出量，不能用被截为零的余量证明完整归因。硬件保留内存位于 Windows 可用 RAM 分母外，不能归入该分母内的在用余量。

## 分类账

扫描先获取 NT RAM 范围，排序并合并重叠区间。每批最多查询 4096 个 4 KiB PFN，每页只计一次。使用 Memory Manager 提供的身份记录，不读取硬编码 `_MMPFN` 偏移、不修改页表、不锁定待扫描的物理页。

可恢复的批量失败允许有界拆分（每批最多 64 次子查询）；仍无身份的页按原始 PFN 再复核一次。重试仅替换同一页的最终身份，不增加物理页总数。权限、ABI 不支持及资源不足等终止性错误立即停止，剩余范围保留为未扫描。原生未知用途代码和页状态另存原始矩阵，未来代码不会被猜测成已知分类。

进程来源在扫描前后分别枚举，可以解析仅在某一端点出现的来源，但同时保留观测端点。空键、重复冲突及前后 PID/名称冲突键不标注名称；名称相符也只是端点证据，不代表冻结的生命周期。压缩分类在归属复核后按已确认来源的在用页重分类，不改变页状态和物理总量。

本采集器的原生 ABI 和基页单位明确为 x64/4 KiB；PfnQueryClient 在任何提权、Native 查询或驱动访问前核对 native kernel 架构及系统页大小，不符合时保留 Unsupported。该结构门禁不生成 Windows build 的语义支持名单。

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

**解析 PFN 映射**是独立的可取消操作，需要新驱动。它用 `VirtualQueryEx` 遍历可访问进程的已提交区域及可读的保留 AWE 窗口，再以 `QueryWorkingSetEx` 查询驻留性，覆盖普通工作集接口会漏掉的 AWE 与大页。只需查询权限的进程仍可解析映射；文件名称使用另一个具有读取权限且创建时间匹配的句柄。只读 PTE walker 批量翻译 VA→PFN，保留映射前再次核对 PFN、页大小、驻留性与区域身份。请求携带进程创建时间，驱动前后核对 PID 身份；每批最多 256 项并有约 50 ms 工作预算。

采集先在所有可访问进程间完成普通工作集优先扫描，再用独立的虚拟页探测预算补查区域；大型普通预留区不能提前耗尽补查预算而阻止后续进程的普通驻留页发现。两阶段持有同一进程对象，已保留的 PID/VA 在补查时去重。普通工作集发现与完整区域遍历分别报告进程覆盖数。

映射可以一页对应多个 PID/VA。按 PFN 排序后的引用表保留这一关系，不把引用重新加进物理总账。大页、锁定页、多重映射页各自按 PFN 去重，仅作为子集统计。Windows ShareCount 有上限，不能据此声称拿到了全部引用。

文件路径会在路径观测前后复核原生 PFN 文件键，键不一致、路径截断及同键不同路径冲突时不回填名称。不同采样时刻不直接用 PFN 号推断文件归属；回填的路径仍属于带时间戳的映射观测，不是冻结的全系统状态。接受新 PFN 分类账后清除旧映射结果，避免可复用的文件键或 PFN 号关联到旧路径。独立映射路径只在自己的映射/对象表展示，不按同值 backing key 回填另一 epoch 的 PFN 分组。

映射扫描默认预算 90 秒，可设置 15–600 秒，最多探测约一千六百万个虚拟页并保留约两百万条引用。不可访问进程、区域/驻留查询失败、已变化映射、文件键冲突、取消和预算耗尽都会报告。受保护或预算内未访问的区域仍可能缺失，因此该统计是已观测下限，不是完整的大页/引用普查。

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


## 对照审计补齐（2026-10-07）

本阶段按 `KSword_Memory_Attribution_Audit_c9f36f7.md` 和同基线矩阵的 P01–P36、G01–G12、T01–T18 核对。原始审计是静态基线，不能作为当前电脑的采样结果。本阶段补齐可验证的归因链与覆盖报告；无法获得可靠来源的责任者继续保留为未知。

### 三种覆盖分别报告

用途账本仍以本次 NT RAM 范围内的唯一 PFN 计数。新增消费者覆盖矩阵，区分已解析、未解析、无需活动消费者三项；Free/Zeroed/Standby/Bad 不要求活动消费者。对象键已观测只是线索，不算消费者已解析。Private/Compression 只有保留的进程来源确实含非零 PID 才增加消费者已解析；分组超限的来源保守保留未解析。单独显示“已知用途、责任者未解析”在用字节，不能以用途未知为零证明责任归因完成。

映射和消费者采集各自有独立 epoch、开始/结束时间，关联的 PFN 分类账仅是上下文。后续观测的 Section、页表、栈、Pool 关系不从旧分类账扣除 owner-unresolved，也不把多个引用计入物理总量。单页检查重新查询来源和 PFN 身份，再决定压缩细分；原结果中的名称或压缩标签不能直接延用。

快速快照的系统 Commit 优先使用 `GetPerformanceInfo`，其失败时才使用有效完整的系统 `SystemMemoryUsageInformation`；有效性独立于数值零。`GlobalMemoryStatusEx` 的 pagefile 数值不再作为系统 Commit 回退。用户态驻留深扫保留 `MappedBackingUnknown`、路径状态及 COW 私有副本；pagefile-backed 必须有公开 `MemoryRegionInfo` 的明确证据，并复核区域身份。已安装减可用只显示聚合保留/不可用估计；SMBIOS 不可用时不能以 Windows 可用容量冒充已安装容量。

### 对象与消费者证据

可验证的关系保留原始字段、提供者状态及前后身份：

- Section/ControlArea：使用现有主映像或文件 ControlArea 查询，要求 PID、区域/映射见证及对象前后复核一致。视图发现不依赖应用仍持有 Section 句柄。Shareable 原生 backing 的 `ProtoPteAddress` 是不透明后备键，不能当作 Section 指针；匿名 Section 创建者仍缺少可靠提供者。
- 页表：基于已验证进程创建时间和 VA→PFN 见证，收集 CR3 与 PRESENT 页表层级的物理页地址，前后复查页表路径和原生 PageTable 用途。数据页的虚拟覆盖大小不是页表物理占用。
- 内核栈：只使用精确 PDB profile 的线程对象、CID 和栈界限；复核对象、CID、界限及原生 KernelStack 身份。当前缺少线程创建时间证据，明确保留该生命周期限制。
- Big Pool：受限查询 class 66 分配清单，复核 VA、大小和标签，再验证翻译得到的 PFN 及 Paged/Nonpaged 用途。标签属于分配见证，不自动确定驱动模块或历史分配调用点。

这些补查共享采集剩余预算，最多 64 个进程、64 个线程、256 个候选页，消费者阶段最长为剩余预算、15 秒和总预算四分之一的最小值；是已观测下限，不是全系统普查。完整 MDL 锁页责任、未映射 AWE 的分配者、缓存独有 file-key→路径、匿名 Section 创建者、系统 PTE/Session/元数据的完整对象归属仍缺少可验证来源，不能通过改名减小未知。

### GPU 与 Pool 历史入口

“GPU / 分配历史”页按需只读采集适配器和进程的 Shared/Dedicated GPU 指标，保留计数器状态与进程创建身份。Dedicated 在 UMA 等配置中不一定等于独显 VRAM。没有 PFN 对应证据时，不把 GPU 指标扣除 PFN 残差，也不把跨进程共享计数相加为独立物理占用。

用户可按需保存一次前瞻 Pool WPR 采集。生产状态机只管理唯一 `KSwordMemory_...` 实例，先验证 WPR 的实例参数和自定义 profile 支持，再使用内存循环缓冲；请求 128 KiB × 256 个 buffer（32 MiB），实际系统开销可能更高。成功停止会话与已验证生成新的非空 ETL 分别报告；命令超时、未知会话状态及清理失败不伪装成成功。命令仅取消/停止本次实例，不启用 PoolTag 注册表开关、不取消其他 WPR 会话。

ETL 可在 WPA 中检查采集开始后的 Pool 分配/释放调用栈。采集前已经存在的分配、覆盖外路径及事件丢失可能造成缺口；导出的元数据保留 `eventsLostKnown=false`，没有 ETL 解码结果不能宣称零丢失或完整历史责任。本阶段仅验证 profile schema 和状态机，未启动真实跟踪。

### 完整原始证据与离线复核

勾选“保留原始 PFN 证据”后，在扫描前选择 `.jsonl` 路径。默认不留逐页文件。`ksword.pfn.raw` v1 每条记录携带域/epoch，保留全部请求身份和最终唯一页分类：

- header：范围、Windows build/架构、固定 ABI、语义验证状态及保留限制。
- query / identities：每个 R3/R0 查询尝试、状态、UTC 和相对批次时间、原始 frame/backing 位。重试是尝试记录，不重复增加分类账。
- ledger_chunk：最终计账身份；原始 PFN、frame、backing 可重建全部类型×状态矩阵。
- ranges / owners：前后范围、来源查询状态及保留的最终来源。
- footer：分类、未知 native-use、消费者覆盖、压缩来源键、未读/未扫、完成/取消/范围变化、计时溢出和观察者内存开销。

单条记录上限 8 MiB、身份块上限 4096 页；顺序写入磁盘，不把全量 PFN 留在 RAM。磁盘短写/失败使证据不可用但不阻止分类采集，缺 footer 或缺口不得显示完整原始证据。工作集/private 开销分别记录前后采样有效性与高水位，它们是本进程观测指标而非严格增量归因。

导出 `ksword.pfn.evidence` v2 的 JSON 清单，复制本次原始 sidecar 并附 SHA256。复制前核对原始文件 header 的域/epoch、字节数；复制后复查源大小/时间，避免失败重扫覆盖同名文件后导出旧结果的错误组合。导出全部保留映射时，另写 `ksword.pfn.mappings` v1：16 个 backing 对象或 4096 个映射为一块，保留全部 PID/VA、前后原生身份、区域、路径/对象状态及消费者见证。对象记录保留前后 Section/ControlArea、匹配视图 PID/区间/类型、提供者状态和原生映射见证；消费者记录保留 CR3/页表项地址与 PRESENT 字段、线程 CID/栈界限/PDB 来源、两次 Big Pool 分配字段及地址翻译。只有实际执行有界对象查询的 backing 才分配原始 proof，避免为所有映射对象复制大型空证据。`translation.entries` 顺序为 PML4E/PDPTE/PDE/PTE；`stack.sources/offsets` 为 etCid/ktStackLimit/ktStackBase；`object.offsets` 为 EPROCESS.SectionObject、Section.ControlArea、ControlArea.ListHead、ControlArea.Lock，文件提供者前两项未使用为零。

映射文件中 `domain/epoch` 属于映射容器；`consumer_relations.observationDomain/observationEpoch` 是独立消费者采样，`observationContextOnly=true` 明确不能当成同一次 PFN 捕获。映射观察者工作集/private 前后值和少量阶段采样最大值单独记录，最大值不代表连续峰值。

界面 300 行限制不限制这份导出。映射 footer 的 `retainedExportComplete` 只表示已保留数据写完；`systemReferenceCoverageComplete=false` 明确不承诺全系统引用覆盖。

后台导出可取消，采用新 UUID sidecar 和原子提交清单；已完整提交的 sidecar 可能在取消后独立保留。PFN 分组、映射保留和文件导出限制分别报告，不以导出完整冒充采集完整。

离线复核示例（Python 3）：

```powershell
python -B tools/pfn_evidence_audit.py capture.raw.jsonl --pfn 0x12345 --output audit.json
python -B tools/pfn_evidence_audit.py first.raw.jsonl second.raw.jsonl third.raw.jsonl --require-proposed-criterion
python -B tools/test_pfn_evidence_audit.py
python -B tools/test_physical_page_mappings.py
python -B tools/test_physical_page_consumers.py
```

审计器流式校验记录版本、页序、范围、唯一页、查询与计账对应、原始矩阵、消费者分区、footer 及多遍文件一致性。成功只证明导出账本可回放；不证明 native-use 在该 Windows build 的语义正确。当前生产 `semanticsValidated=false`，因此不能通过提议的三份完整同域/build/架构快照、未知在用≤64 MiB 门槛。owner-unresolved 是另一项指标，不能隐藏在这个门槛中。

### 本阶段验证边界

独立严格 C++17 测试覆盖 PFN/owner/Commit 口径、批量恢复、GPU 提供者失败/真零/进程重用/边界、Pool 会话状态机；生产映射和消费者代码由确定性 API mocks 执行，覆盖 AWE/大页候选、预算/权限、Section 关系及页表/栈/Pool 身份变化。公开内存区域 API 的受控自有 Windows 测试涵盖 pagefile/datafile/image/COW 与系统 Commit，不替代 PFN 驱动测试。

当前主程序构建因本机缺少仓库要求的 Qt/x64 MSBuild 环境返回 `BUILD_RESULT=UNAVAILABLE`；未进行 GUI、加载新驱动、完整 PFN 实测、T02–T15 的受控驱动/硬件全流程或 T18 的三份支持 build 验收。历史章节里的其他日期构建结果不能作为本阶段已构建的证据。
