# HVM 内存监视 / 首次访问归因

对应 [issue #195](https://github.com/KSwordDEV/KSword/issues/195) 的 P0。

## 它解决的问题

快照式检测能告诉用户「SSDT / DriverObject / 回调 / 内核代码现在不对劲」：

```text
Before: nt!Foo
After : suspicious.sys+0x1234
```

但答不出**是谁改的、从哪条指令改的、第一次改发生在什么时候**——因为改动那一刻
已经过去了，而快照只保留了前后两个端点。

内存监视把问题换个方向问：盯住目标，等下一次访问，把那一刻的现场记下来。

```text
Ksword 发现某个 DriverObject dispatch 当前正常
        ↓  对 MajorFunction[IRP_MJ_DEVICE_CONTROL] 建立写监视
        ↓  继续正常使用系统
        ↓  某驱动第一次修改所在页
        ↓  记下 时间 / CPU / GPA / GLA / RIP / RSP / CR3 / 写入者模块
        ↓  原写入正常继续，HVM 仍保持常驻
        ↓  查看写入者反汇编
```

## 为什么要新加一种处置

EPT 上已经有三种处置，没有一个能承担这件事：

| 处置 | 命中后 | 为什么不能用来做归因 |
| --- | --- | --- |
| 严格 tripwire | 记录并**退出虚拟化** | 抓到一次访问的代价是整台机器的 VMM 没了 |
| `ALLOW_ONCE` | 放行一条指令，用 monitor-trap 把权限收回来 | 要 MTF，而嵌套 Hyper-V 实测不给；多核共享层次下放宽窗口全机可见 |
| `ENFORCE` | 持久拒绝，注 #PF | 已判 `UNIMPLEMENTED`：guest 看不见 EPT，缺页处理器什么都不修就返回，实测无限活锁 |

`WATCH_ONCE` 正好是**「ALLOW_ONCE 去掉再收回那一步」**：

```text
EPT violation
    ↓
原子 ARMED → TRIGGERED（只有一个 CPU 赢）
    ↓
把这一页的权限永久恢复（这条规则从此不再拒绝）
    ↓
INVEPT
    ↓
RIP 不推进，VMRESUME
    ↓
原指令重执行并正常完成；常驻继续
```

因为没有「再收回来」，它**不需要 MTF**，也就不需要 ALLOW_ONCE 那道「单核或私有
层次」的门：权限是朝放开方向单向变化的，别的处理器提前看到放开的权限，结果只是
它们那次访问也正常完成——而这条 watch 本来就已经决定不再拦了。

这也是为什么它**不是安全边界**：它不阻止访问。

## 多核首次命中

`InterlockedCompareExchange(ARMED → TRIGGERED)` 决出唯一的 first-hit owner。
关键在于输的那一方**也要修复自己那一份视图**：

```text
CPU A: violation → CAS 赢 → 清 DeniedAccess → 写叶 → INVEPT → 记证据 → RESUME
CPU B: violation → CAS 输 → 写叶（同一个值）→ INVEPT → 不记证据 → RESUME
```

只有赢家修的话，输家会在赢家的写传播到自己之前一直重复违规——一个没有错误码、
没有日志、也不会自己停下来的活锁。判据钉在
`KswordArkHvmWatchPlanHit` 的全局不变式 `Accepted == MustRepair` 上。

恢复值用「**排除这条 watch 之后重算**」而不是「读已被清零的 `DeniedAccess`」：
输家可能在赢家的清零传播到自己之前到达，按 identity 排除让每个 CPU 算出同一个
终值，与到达顺序无关。

## 必须如实展示的三件事

这三件都属于「读起来正确、理解起来会错」，所以协议和界面都保留两套数字：

**监视单位是 4 KiB 物理页，不是用户选的那几个字节。** EPT 权限就是页粒度。
用户从 `DriverObject->MajorFunction[14]` 这样一个 8 字节字段建监视，装到硬件上的
仍然是那一页。所以 `requestedAddress/Length` 与 `physicalPage`（恒 4096 字节）
并排显示，绝不能描述成「8 字节硬件断点」。命中时若 CPU 给了有效 GLA，再另外
判断它落没落在请求的那一段里——那属于归因细化，**不是过滤**：无论落没落在范围
内，命中都要报出来。

**请求的访问类型与实际生效的可能不同。** EPT 不允许 `W=1` 而 `R=0`，所以「只监视
读」在硬件上一定连写也监视了；处理器不支持仅执行叶项时还会连带包含执行。
`requestedAccess` 与 `effectiveAccess` 两栏都留着。安装对话框在勾「读」的当场就
说明这一点——用户是在那一刻决定接不接受的，不是等装完在表里发现。

**「命中了但证据丢了」与「从未命中」必须分开。** 事件环在 VMX root 从不等待，
会丢包。两种情况在事件列表里长得一模一样而结论正好相反。所以 watch 自己保留
`hitCount` / `lastHitSequence` / `lastHitStatus`，命中时先记为 `EVENT_LOST`，
事件发布成功才改写成 `PUBLISHED`。

## 生命周期

```text
CREATED → ARMED → TRIGGERED → DISARMED
                      ↓
                 （常驻停止）
ARMED ─────────────→ INVALIDATED
```

常驻停止时把所有 `ARMED` watch 转成 `INVALIDATED` 并恢复权限：watch 是「某段时间
里有人在看」的断言，跨过一次没人看的空档还报「未命中」是编造的观测结果。已
`DISARMED` 的不动——那是真实发生过的观测，与residency 之后做什么无关。

重新武装要显式 `REARM`，它保留 watchId 与累计命中次数（「这个目标一共被动过
几次」才是要回答的问题）。

## 安装期就拒绝的几种情况

宁可在安装时说清楚，也不要装上一条看似成功、某一刻才暴露问题的监视：

| 状态码 | 条件 | 为什么在安装期拒绝 |
| --- | --- | --- |
| `NOT_RESIDENT` | 常驻没在跑 | EPT 权限只在有处理器加载了这套 EPT 指针时才产生退出。装上的监视永远不会响 |
| `LEAF_CONFLICT` | 这一页已被分离视图 / 规则 / 另一条 watch 占着 | 两套机制对同一叶项的期望值不同，谁后写谁赢，而赢的一方会在对方毫不知情的情况下把对方的功能改掉 |
| `INVALID_REQUEST` | `pageCount != 1`，或同时请求 `ENFORCE` / `ALLOW_ONCE` | 命中路径在 VMX root 恢复权限，那份工作必须由常数决定；多页 watch 只是几条独立 watch 共用一个命中计数，答不出「被动的是哪一页」 |

## 虚拟地址不跟踪重映射

用内核虚拟地址建监视时，地址**在安装那一刻**翻译一次并就此绑定到那个物理页。
之后 guest 页表把同一个 VA 指到别处，这条 watch 仍然监视原来那一页。

这不是遗漏：跟踪重映射要监视 guest 页表本身，那是另一个数量级的机制。这里做的
是把绑定的时刻和结果如实记下来，并在详情页做一次核对——当前 VA 已经指到别的页
时明说「这条监视仍然盯着武装时那一页，不再对应该虚拟地址」，而不是继续显示成
「正在监视该 VA」。

## 归因分层

```text
VMX root      记录最小可信现场：RIP / RSP / CR3 / GPA / GLA / qualification / CPU / 时间
    ↓
event ring
    ↓
R3            RIP → 已加载内核模块范围 → module.sys + RVA
```

VM-exit 热路径里**不**解析 `PEPROCESS` / `ETHREAD` / 模块路径 / PDB 符号 / 调用栈。
那些要真实上下文和可分页数据，在 VMX root 里做是拿整台机器冒险，而晚几毫秒做
完全不影响结论。HVM 负责提供事实，Windows-aware 层负责解释事实。

RIP 归不到任何已加载模块时显示「未知可执行区域」并给出反汇编入口——那本身就是
一条可疑读数，不是一次失败。反汇编从命中 RIP 往前退 `0x40` 开始：RIP 指的是
**尚未完成**的那条指令（EPT violation 发生在指令退休之前），只从它开始看不到
前面几条在算什么地址，而那往往才是要找的东西。

CR3 原值上报，**不**反解 PID：KVA shadow、系统地址空间、内核工作线程、CR3 复用
都会让 `CR3 → 进程` 这一步不成立。

## 写入值的语义

第一版不承诺精确的 `Old Value` / `New Value`。EPT violation 发生在导致访问的指令
**真正完成之前**，所以命中时拿到的是「写之前」的现场；在没有可靠的单指令完成
观测机制之前，无法在同一次 VM-exit 里知道该指令最终写进去的完整结果。

## 入口

- **界面**：虚拟化 → **内存监视** 子页。添加 / 重新武装 / 移除 / 刷新 /
  查看写入者反汇编 / 复制证据。
- **CLI**（`tools/hvm_ctl`，探针工具，不随主程序发布）：

```powershell
.\tools\hvm_ctl\hvm_ctl.exe --json watch-add-va FFFFF80112345678 8 2
.\tools\hvm_ctl\hvm_ctl.exe --json watch-add-pa 183A45000 0 4
.\tools\hvm_ctl\hvm_ctl.exe --json watch-list
.\tools\hvm_ctl\hvm_ctl.exe --json watch-rearm 17
.\tools\hvm_ctl\hvm_ctl.exe --json watch-remove 17
.\tools\hvm_ctl\hvm_ctl.exe --json watch-clear
```

`watch-clear` 的名字比它的副作用小：协议里没有"只清监视"这个操作，监视与普通 EPT
规则住在同一张表里，所以 CLEAR 会把分离视图规则一起清掉。命令说明里原样写着这一点。

访问掩码：`1`=读 `2`=写 `4`=执行，可相加。JSON 输出含 `watchId`、`state`、
`requestedAccess` / `effectiveAccess`、`physicalPage` 与 `effectiveBytes=4096`、
`hitCount`、`lastHitSequence`、`lastHitStatus`（`published` / `event-lost` /
`none`），够靶机自动化直接判定。

## 端到端自检

```powershell
.\tools\hvm_ctl\hvm_ctl.exe --json watch-selftest
```

在**同一个进程**里跑完 issue 第二十二节第 1 项（WRITE First-touch）的十三条
检查：分配并锁住一页 → 装一条写监视 → 写它 → 逐项核对命中现场 → 再写一次 →
撤掉监视。

同一条流程换一个访问类型就是第 2、3 项，所以三者共用一份实现而不是抄三遍：

```powershell
.\tools\hvm_ctl\hvm_ctl.exe --json watch-selftest-read   # 第 2 项
.\tools\hvm_ctl\hvm_ctl.exe --json watch-selftest-exec   # 第 3 项
```

抄三遍的结果是三份会各自演化，而它们本该逐条对齐——尤其是"命中不阻止访问"与
"命中不结束常驻"这两条分界判据，三种访问类型下必须完全一样。JSON 里带 `access`
字段，否则三份结果并排贴出来完全一样，等于没有证据。

其余五条各自对应一个验收项，判定语义与退出码相同：

```powershell
.\tools\hvm_ctl\hvm_ctl.exe --json watch-selftest-smp       # 第 5 项：多核同时命中
.\tools\hvm_ctl\hvm_ctl.exe --json watch-selftest-conflict  # 第 6 项：视图冲突（会先 teardown）
.\tools\hvm_ctl\hvm_ctl.exe --json watch-selftest-remap     # 第 7 项：VA 重映射
.\tools\hvm_ctl\hvm_ctl.exe --json watch-selftest-restart   # 第 8 项：常驻重启
.\tools\hvm_ctl\hvm_ctl.exe --json watch-selftest-evidence  # 第 9 项：事件丢失
.\tools\hvm_ctl\hvm_ctl.exe --json watch-selftest-process   # P1：CR3 → 进程归因
```

同进程是 RIP 判据的前提：要证明"记下来的 RIP 就是那条写指令"，就得有一个已知的
写指令地址可比；跨进程只能比到模块粒度，而模块粒度答不出"是不是记错了一条指令"。
它也因此不需要另写一个测试驱动。

判定是四态而不是布尔：

| 退出码 | 含义 |
| --- | --- |
| `0` | PASS，全部检查通过 |
| `2` | FAIL，有逻辑失败 |
| `3` | 有"这台机器上问不出来"的项但没有失败（常驻没跑、页拆不开、叶被占、处理器没报告 GLA） |

把"问不出来"混进 FAIL，会让一台架构上就提供不了该信息的机器永远绿不了；混进
PASS，则等于凭空承认了一个没观测到的事实。两条检查因此天生带 BLOCKED 分支：
**有效客户线性地址**（处理器可以不报告）和**装不上监视**（能力/占用类 vs 真缺陷）。

其中两条是这个功能与别的处置的分界，单独点名：

- **被监视的写最终真的完成了**（`page[0] == 0xB2`）——命中不阻止访问，这是它与
  `ENFORCE` 的分界；
- **命中没有让任何处理器退出虚拟化**（`residentAfter == residentBefore`）——这是
  它与严格 tripwire 的**根本**区别，也是 issue 里那句"命中 Watch ≠ HVM 退场"。

## 验证状态

**已在编译机上证明**：

- `KswordARKLightTests` 的 `HVM watch` 套件 157/157 通过。覆盖权限归一化（逐组合
  核对超集性、幂等、架构约束）、叶项算术（掩码外的位逐位不变、拿掉再恢复回原值）、
  首触决议（六个状态加协议外数值穷举，钉死 `Accepted == MustRepair`）、范围判定
  （半开区间两侧、无效 GLA、零长度、回绕、顶到地址空间末尾）。
  这套测试当场抓到实现里一个真错：回绕守卫写成 `base > ~0 - length` 会把恰好顶到
  地址空间末尾的合法区间也拒掉。
- 驱动 `/t:Rebuild` 干净重建，零错误零警告；GUI、`hvm_ctl`、i18n 审计、目录门禁、
  参数回归全部通过。

**已在 2-vCPU 嵌套 Hyper-V 靶机上实测**：issue 第二十二节 **1–9 项全部 PASS**，
外加 P1 的 CR3 → 进程归因。逐条读数见
`docs/next/logs/hvm-memory-watch-nested-20260919.md`。每一项都做成了 `hvm_ctl`
的一条命令，可重复跑：

| # | 验收项 | 命令 | 判定 |
| --- | --- | --- | --- |
| 1 | WRITE First-touch | `watch-selftest` | PASS 13/13 |
| 2 | READ First-touch | `watch-selftest-read` | PASS 13/13 |
| 3 | EXECUTE First-touch | `watch-selftest-exec` | PASS 13/13 |
| 4 | Nested Hyper-V（P0 关键） | 以上全部跑在该靶机上 | PASS |
| 5 | SMP 同时命中 | `watch-selftest-smp` | PASS 7/7 |
| 6 | View 冲突 | `watch-selftest-conflict` | PASS 6/6 |
| 7 | VA 映射变化 | `watch-selftest-remap` | PASS 5/5 |
| 8 | HVM restart | `watch-selftest-restart` | PASS 5/5 |
| 9 | Event loss | `watch-selftest-evidence` | PASS 6/6 |
| P1 | CR3 → 进程归因 | `watch-selftest-process` | PASS 4/4 |

靶机 `featureNames` 里没有 `MONITOR_TRAP_FLAG`——这是"本机不给 MTF"的直接读数，
也是 `WATCH_ONCE` 这套语义存在的前提。

**明确不在本轮范围**（issue 第二十节）：eVMCS、VPID、Nested VMX、VMFUNC 新功能、
性能调优、完整 x86 指令模拟器、无限制 Continuous Watch、精确 post-instruction
`New Value`、全量内核栈回溯、强制阻止目标访问、把 watch 做成安全边界。

**P1 已做**：Memory / Kernel Disassembly / SSDT / SSSDT / DriverObject（整对象
与 **MajorFunction 单项槽位**）/ FastIo（槽位与例程）/ Callback 的右键接入
（`ks::ui::openHvmWatch(target)` 统一入口）、导出表符号解析、
CR3 → 进程的 best-effort 归因（四态：Resolved / NotFound / Failed / Unavailable）、
证据详情与复制/导出、"查看目标内存"与"查看模块"。

## 2026-09-27 收尾轮

对着 issue 二十四节逐条复核了一遍**代码**（不是复核上一轮的自述），补掉下面这些。
分成"说错了"和"没做"两类，因为前一类比后一类危险：它们都不报错。

### 说错了的（读起来正确、实际不成立）

| 位置 | 原来怎么说 | 实际 |
| --- | --- | --- |
| `armedGeneration` 协议注释 | "代次对不上就说明跨过了没人看的空档" | `Generation` 每次 HVM 控制 IOCTL 都加一，装监视后必然要 `START_RESIDENT`（再加一代），所以**正常观察中的监视代次恒定对不上**；而真正的 fail-closed 故障不走控制路径、代次反而一致。两个方向都会错。权威判据是 `ARMED → INVALIDATED` 这个状态转换本身 |
| `WATCH_ROW` 字段注释 | "实际监视的物理页与页内偏移" | 紧跟的第二个字段是 `pageCount`（watch 恒为 1），照注释去读会得到一个恒等于 1 的"偏移"。页内偏移由 R3 用 `requestedAddress & 0xFFF` 算 |
| DriverObject 页注释 | "MajorFunction 槽位与 DriverObject 必在同一页，所以监视整个对象等于监视了每一项" | `DRIVER_OBJECT` 是池分配、16 字节对齐，x64 上 `sizeof` 0x150 而数组在偏移 0x70；基址页内偏移超过 0xE90 时**尾部若干项落到下一页**，而一条监视只覆盖一页 |
| `openTargetMemory` 注释 | "读不到再退回物理页" | 代码是一次三元选择，虚拟读失败就直接返回。恰好在最需要看当前内容的场景（VA 已不指向那一页）里一个字都读不出来 |
| `InvalidateWatchesLocked` 的调用点注释 | "放在 rendezvous 之前，因为权限必须趁处理器还能被失效时恢复" | 该函数从来没有 INVEPT 过 |
| `docs/CLI使用文档.md` | "主程序完整操作与 `hvm_ctl` 共用命令目录" | 那条通路（子页、`--ksword-hvm-command`、`HvmCommandPanel`）已经整条删除 |

### 没做的

- **命中时间从来没有显示过**。驱动采集了（`KeQueryPerformanceCounter`）、协议带了、
  R3 结构解析了，然后就断了：表格、详情、复制、导出、事件流五处全无。现在详情里
  显示，并且**标明它是换算来的**——那是性能计数不是墙钟，换算依赖计数源没被重置。
- **`clearWatches` 只活在一条注释里**，从未声明也从未实现；UI 没有"清空"、CLI 没有
  `watch-clear`。两处都补上了，并且都如实说明副作用：协议没有"只清监视"这个操作，
  CLEAR 清的是整张 EPT 规则表。
- **面板从不读事件环**。watch 行里没有 `qualification`，那是"这次访问究竟是读、是写
  还是取指，页当时还剩哪些权限"的原始读数，只存在于事件行里。新增"查看命中事件"，
  结果四态：`Found` / `Evicted`（命中过但证据被挤出环）/ `NeverHit` / `Unavailable`
  （这次查询没跑起来）。后三态在界面上极易长成同一句"无事件"，而结论完全不同。
- **模块归因把失败说成结论**。三条早返回（解析不到 `NtQuerySystemInformation`、
  问不出长度、两次查询都失败）与"比过一遍、确实不在任何模块里"返回同一个值，
  界面渲染成肯定句"未知可执行区域"。改成四态；失败时不再把"查看模块"置灰——
  灰掉等于把一次没跑起来的查询显示成一条结论。
- **进程归因 NotFound 断言了单一成因**（"多半已经退出了"）。开了 KVA Shadow 的机器上
  用户态命中**必然**落进这个分支，那时这句话是确定而错误的。
- **`length=0` 上下游含义相反**。这一层与对话框把 0 读作"整页"，命中路径的纯函数层
  把 0 读作"空区间，匹配不上任何东西"（刻意如此，有单测）。驱动为弥合分歧在长度为 0
  时把请求地址换成**物理**页基址，而命中时拿来比的是客户**线性**地址——于是"落在请求
  范围内"对一个明确请求整页的目标恒为"否"。修法是在服务层就落成页对齐虚拟地址 + 4096，
  根本不走那条分支。按物理地址建的监视则一律显示"无法判断"，因为两个地址空间不可比。
- **常驻没跑时界面显示"监视中 / 命中 0"**。规则表在常驻期间冻结，所以安装**必然**
  发生在常驻停着的时候。照直显示，用户读到的是"这段时间没人动过"，而那段时间根本
  没有人在看。状态改成"已武装（常驻未运行，还没有开始观察）"，详情里另加一行。
- **§10 要求的三个入口只有一个**。"未知可执行区域"时除了反汇编，现在还有
  "查看写入者内存"与"查看 RIP 所在页"（整页而不是 0x200——要判断一段不属于任何模块
  的代码是什么，一小段看不出来）。

### 驱动侧

- **页归属只在装 watch 时查**，普通 EPT 规则可以合法地压到 watch 或分离视图的页上；
  命中路径遇到同页的另一条规则会走全机 fail-closed 退虚拟化，正是本功能存在的意义
  所在的反面。现在普通规则 ADD 也逐页查，但**只拒 view 与 watch**——规则与规则想要
  的是同一件事（更少权限），它们本来就该共存。
- **命中路径 CAS 成功后的三处 fail-closed 不回退状态**：叶项找不到、私有层次翻译不出、
  INVEPT 失败，三处直接 return 而不动 `WatchState`。停机只把 ARMED 转 INVALIDATED，
  于是那条记录**永久**停在"正在处理命中 / 命中 0"。现在转 FAULTED（协议里把 FAULTED
  的语义从"只有安装期失败"扩到覆盖这一种，并写明为什么不能用 TRIGGERED 或 INVALIDATED）。
- **失效 watch 时恢复了叶权限却不 INVEPT**。同一个 recompute 之后，ADD / REARM / CLEAR
  三条路径都失效了，只有这条没有。同时让这个停机扫描也修复 TRIGGERED / FAULTED 的页，
  否则会留下"没有任何规则声称拥有它、但 EPT 仍然拒绝"的叶项。
- **`MajorFunction` 槽位地址**：`KSWORD_ARK_DRIVER_MAJOR_FUNCTION_ENTRY` 尾部追加
  `slotAddress`（`&DriverObject->MajorFunction[i]`）。R3 推不出来——那个偏移是内核
  结构布局，用户态写死一个数就是把内核布局假设搬到了用户态。旧驱动回 0，界面据此
  禁用该入口并说明原因，而不是拿 0 去装一条监视。

### 本轮验证边界

编译机上全绿：主程序 Release 零错误、驱动 `/t:Rebuild` 零错误零警告且
`Driver is 'Universal'`、`KswordARKLightTests` 18 个套件全过（`HVM watch` 157/157）、
HVM 控制算术单测 85/85、i18n 审计 26413 条、`hvm_ctl` 目录 83 条命令与参数回归、
私有 EPT 不变量、IOCTL 注册与访问策略（0 HIGH）、驱动功能矩阵计划、tracked JSON 2313 个。

顺带把套件清单接进了 CI：`scripts/ksword-expected-suites.json` 此前只有手工验收脚本
读它，CI 只判退出码——而退出码恰恰抓不到"整个套件没被链接进来"。新增
`scripts/Test-KswordSuiteManifest.ps1`，两次变异（清单里加一个不存在的套件、
把断言下限抬高一条）都被拦下。

**没有实机验证**：本轮改了驱动，所以第二十二节 1–9 项需要在靶机上重跑一遍
（`hvm_ctl` 的 `watch-selftest*` 系列，退出码即判定）。编译机证明不了 VMX 行为。

**P2 已评估、结论是不做**：见 `docs/next/hvm-continuous-watch-feasibility.md`。
四条候选路径里没有一条同时过 issue 给的四条准入门槛；唯一四关全过的 EPT 执行视图
翻转只覆盖执行、不覆盖读写。因此不增加 `Mode = CONTINUOUS`。
