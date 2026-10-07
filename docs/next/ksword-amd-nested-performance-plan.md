# AMD 嵌套虚拟化下一阶段性能方案（2026-10-06）

状态：源码与历史证据研究完成；本轮未修改驱动/CLI、未加载、未启动 VM、未请求 UAC、未进行实机性能验收。

## 1. 当前基线及需要纠正的历史判断

研究基线为 `2c3d08c93c924ecbff4ab7921d45fb85784b81c3`。与 9 月最后的 MSR 试验回滚提交 `a5d7a2a7` 比较，当前 `hvm_svm_*` 和 metrics 结构没有源码差异。10 月的 AMD 变化主要为 GUI、通用准入与 featureFlags 的分层发布；不能把 GUI 接通当作嵌套执行算法已经升级。

工作区已有 `.claude/memory/ksword-object-callback-remove.md` 的其它任务修改，本轮不纳入提交。

保持 Windows 10、现有 AMD 电脑和 VMware Workstation 16；以后先验证 1 vCPU，再直接验证 8 vCPU。用户离开期间仅进行离线研究。既有截图/口述中有进入桌面的记录，但下表所用窗口只有固件/退出计数证据，不能用它们补出桌面完成或正式开机速度验收。

### 1.1 按原始 JSON 重新计算

原始样本为 `artifacts/build-npt-transfer-v8-live/a.json`、`b.json`，间隔 **10.3365066 秒**。32 个 CPU 的热点记录均有效、代次一致、累计总数单调；按 CPU 与层级做差，不将累计快照相加。复算程序及结果在忽略目录 `artifacts/amd-perf-research-20261006/`。

| 层级/退出 | 增量 | 占本层退出次数 | 含义 |
|---|---:|---:|---|
| L1 全部 | 1,681,507 | 100% | 占全部 L1+L2 退出次数 92.43% |
| L1 MSR `0x7c` | 885,086 | 52.64% | EFER、HSAVE、XSS 状态虚拟化 |
| L1 XSETBV `0x8d` | 161,047 | 9.58% | XCR0 切换/校验 |
| L1 VMLOAD `0x82` | 160,807 | 9.56% | 当前为软件搬运 |
| L1 VMSAVE `0x83` | 80,419 | 4.78% | 当前为软件白名单写回 |
| L1 STGI/CLGI `0x84/0x85` | 262,551 | 15.61% | 当前为软件 GIF/事件协调 |
| L1 VMRUN `0x80` | 131,038 | 7.79% | 截获并创建 L2 运行事务 |
| L1 CPUID `0x72` | 559 | 0.03% | 该窗口不是首要优化对象 |
| L2 全部 | 137,636 | 100% | 包含 L1 要求反射的退出 |
| L2 IOIO `0x7b` | 81,208 | 59.00% | 不得越过 VMware 的设备模拟 |
| L2 MSR `0x7c` | 28,336 | 20.59% | 多数为 EFER 读取 |
| L2 NPF `0x400` | 21,468 | 15.60% | 约 2,077/s，按实际 QPC 间隔计算 |

L1 MSR 的精确分布：EFER 写 321,850、读 160,924；HSAVE 写 160,925、读 80,462；XSS 写 160,925。TSC、APERF/MPERF 和 P-state **未出现在这个 L1 热点集合**。不能再用原生放行这些 MSR 的办法解释或解决当前热点。

同窗口 NPT 缓存 lookups=131,038、hits=131,031、resets=7；跨 CPU owner 转移 54、pool recycle=0。说明这一窗口中反复清空 NPT02/跨 CPU 接管并非主要退出来源；其它启动阶段仍需独立采样。

**这里证明的是退出频率，不是周期占比。** 没有覆盖完整汇编桥和各 C 阶段的计时，尚不能断言硬件 VMEXIT 指令本身占多少时间，也不能把上述比例换算成提速比例。

### 1.2 上次失败试验的边界

`48edd296` 曾从合并 MSRPM 中抹掉 L1 要求的特定读取拦截；`a5d7a2a7` 已回滚。它改变了 L1-owned 退出语义，不能作为后续优化的模板。

其 10.4308628 秒窗口：L2 NPF 增量 8,852,581，pool recycle=15,925，epochChanged=15,222。记录了灾难性影子缓存抖动，但不是与 v8 同启动阶段的受控 A/B，无法仅据这些计数确定因果；Vix socket 10054 也不足以单独证明 VM 崩溃。历史报告误把 `0x80` 标成 MSR，应更正为 VMRUN。

## 2. 现有代码中真正可优化的工作

### 2.1 每次退出的固定成本

`hvm_svm_entry.asm` 每次走完整 XSAVE/XSAVES → host VMLOAD → C 退出分派 → C 进入协调 → XRSTOR/XRSTORS → guest VMLOAD → VMRUN。XCR0/XSS 与 root 不同还会切换控制寄存器/MSR；每次进入将 `VMCB.CLEAN=0`。

`hvm_svm_exit.c::KswordSvmTrace` 每次更新 ring、读取多个 VMCB 字段并执行三个 Interlocked 操作；general entry/exit/hotspot 还有多次序列更新。C 事件路径每次读/写/回读 CR8。每项都是候选成本，先计时，不凭代码长度宣布谁最慢。

Linux 的常规 SVM 汇编桥不在每次 VMRUN 周围直接做全 XSTATE 保存/恢复；KVM 在 KVM_RUN 层管理 FPU 生命周期。这依赖 Linux 的 FPU 使用约束，Windows/MSVC 当前 C 路径不能直接省掉 XSAVE。参考其分层设计，另建可证明只用整数寄存器的短路径。

### 2.2 VMRUN/VMLOAD/VMSAVE 的额外成本

- `nested_fetch.c::KswSvmNestedFetchInstruction` 按指令的每个字节重走 L1 页表，并反复重走 NPT01、复核路径。普通三字节 SVM 指令也重复三遍。每次物理 word 回调都 map/unmap 窗口、两次 INVLPG。应改为一次调用内按最多两个代码页/最多三个对齐 qword 批量取指，最后复核路径。
- `nested_session.c::KswSvmNestedSessionEnter` 仍有接管前、接管后两次整页 VMCB12 读取；transfer 路径已经减少过重复读取，VMRUN 路径尚未同步该优化。
- VMRUN 每次捕获最多 20 KiB 权限图；当前先清空完整图、再覆盖 enabled 图，然后逐 byte OR 到第二套图。可以消除覆盖前清零并按对齐 word 合并，但必须保留 L0/L1 OR 语义和 L1 原图供退出归属判断。
- `nested_commit.c` 每次扫描 512 个 word 并调用 whitelist，实际 VMSAVE 只写 16 个 word。可以生成按 operation 分类的固定写回表；掩码/部分写回/并发保护不变。
- 单个 CPU 的硬件 VMCB 在 L1/L2 之间整页覆盖，阻碍 clean bits 复用。后续拆分稳定 VMCB01/VMCB02，并单独管理 VMLOAD 状态。

KVM 的稀疏 MSRPM 合并建立在它的外层图默认拦截很多 MSR 的前提上；KSword 外层图仅少数状态 MSR 被置位。不能照搬只读少量 L1 bitmap 的算法，也不能只根据地址不变就缓存可变 guest bitmap。

### 2.3 硬件能力的机会

历史实体机 `CPUID.8000000A.EDX=0x1ebfbcff` 含 clean bits(bit 5)、Virtual VMLOAD/VMSAVE(bit 15)、vGIF(bit 16)。历史 VMware 克隆只暴露 `0x200ed`，不包含 bit 15/16。因此需要两套路径：实体机硬件加速，克隆继续软件回归。最终开关以每核重新探测和实际控制/指令测试为准。

当前 CPUID 虚拟 mask 为 `0xc9`，virtual capability 并不暴露 clean/VLS/vGIF；builder 也拒绝相应扩展控制。**L0 利用硬件加速 L1** 与 **向 L1 宣布它可给 L2 使用这些控制** 是两项不同工作，第一步只处理前者。

## 3. 进入性能开发前必须厘清的 TLB 语义

`KswordSvmNestedGeneralEntry` 在 session=L2 时，每次重新读取捕获的 `Vmcb12[TlbControl]`。一次 L1 VMRUN 请求 flush，可能被 L0 后续 NPF/IO/MSR 重入重复执行。应建立每次虚拟 VMRUN 的 pending 请求，在第一次实际执行对应硬件进入时消费；INVALID/未进入不能提前消费，L0 自己发布/改变 NPT02 另有独立 pending flush。

同时：NPT01 固定 **不表示 NPT12 固定**。L1 可在根地址/ASID不变时修改 NPT12 再请求 flush。仅清硬件 TLB 并保留旧 NPT02 页表，不足以保证下一次解析来自新的 NPT12。KVM 的 nested transition 同时请求 MMU sync 和 TLB flush；这里必须补齐源页表同步的证明。

先用“虚拟 flush 时保守失效相关 NPT02”的正确参考路径。再实现有预算的源 PTE 依赖重验：每个已发布 leaf 记录 Inner walk 的 entry GPA/value、NPT01 lifetime、cache/permission 与所覆盖的范围；虚拟 flush 时重验来源，保留未变映射，撤销 frame/permission/PS/PAT/NX/A-D 有关变化对应的叶子。预算不足/根或角色变化则整体失效。跨 CPU/虚拟 ASID要有 owner/generation 管理；不能只比较 nCR3。

另核对 flush 1 的虚拟全部 ASID、3 的指定 ASID和7的非全局语义：不能把 flush 1 全部简单缩成当前 L2 ASID 3。稳定 L1/L2 ASID不免除各自的虚拟失效义务。

当前 `Cpu->TlbRequests++` 位于退出入口，无条件计数，本质是执行/退出数量，不是实际 TLB flush 次数。需要保留旧字段兼容解释，并新增独立的请求/执行/重复抑制计数。

## 4. 实施顺序、代码落点和每步门槛

| 阶段 | 实现 | 成功证据 |
|---|---|---|
| P0 计时与正确基线 | 完整桥计时、计数分析修正、TLB请求消费/源同步对照 | 各阶段周期账本闭合，flush与映射变化用例正确 |
| P1 软件事务减负 | 批量取指、VMRUN前只resolve、权限图/白名单批量处理 | 相同输入输出/故障行为，read/map/字节扫描减少 |
| P2 常见 L1 整数短路径 | EFER/HSAVE读、幂等XSS写等绕过全XSTATE往返 | 硬件SIMD/CET状态保持且周期实测下降 |
| P3 硬件 Virtual VMLOAD/VMSAVE | 仅在满足L1模式/状态/能力时清相应拦截 | 0x82/0x83频率明显下降，guest当前状态完全一致 |
| P4 硬件 vGIF | L1 GIF由硬件V_GIF承载，事件/NMI与L2归属同步 | 0x84/0x85下降且中断/停止行为正确 |
| P5 稳定 VMCB+clean | 分离01/02、dirty分类、稳定字段不重装 | dirty覆盖和实际VMRUN行为验证，周期继续下降 |

每个阶段单独 commit，不推送；失败候选保持证据并回退。P3/P4可分别开关，不能把两项同时打开后无法归因。

### P0：把“退出多”变成“时间花在哪里”

1. 新增 `hvm_svm_perf.h/.c`，上下文附加到 `KSW_SVM_CPU` 尾部；关键汇编前缀 offsets 不变，新增 ASM offset 编译期断言。每 CPU 固定存储，按 L1/L2、退出码及 MSR读写累计样本数、周期总和、最大值和对数桶。
2. 默认关闭计时，通过明确诊断控制在准备时开启；有完整采样模式和按 1/64 退出采样模式。只选一次采样位贯穿同一往返，避免不同阶段计数不匹配。
3. 汇编时间点：VMEXIT后第一可用位置、XSTATE保存后、host VMLOAD后、C分派返回、entry协调返回、XSTATE恢复后、VMRUN前。能计到的“entry-pre → exit-first”包含guest执行和硬件转移，不能命名为纯硬件VMEXIT成本。序列化计时需选验证过的 LFENCE/RDTSC 或 RDTSCP 序列、记录 AUX/CPU和测量本身成本；XSAVE微基准另测。
4. C子阶段计时：fetch、operand capture、owner acquire/release、permission capture/merge、VMEXIT写回、NPF walk/install、event/NMI、CR8、trace。子阶段不要再次加到完整桥总时间导致重复计费。
5. 新增 actualFlush[0/1/3/7]、virtualFlushReceived/Consumed、shadowFlush、fullXstateSave/Restore、switchXcr0/Xss、map/unmap/INVLPG数量、writtenWords/bitmapBytes和 fast/slow 计数。
6. metrics 独立升级 v10，协议只在 `shared/driver/`；同步 `hvm_ctl`、shared命令引擎、主GUI证据解析与导出。旧/新长度或版本不符明确不可用。
7. 修正 `analyze_svm_profile.py`：读取全部JSONL行；用同boot/run/generation/CPU的相邻差值；逐记录检验valid/even sequence/饱和/单调；NPF纳入原因栈，prepared/hardwareExit不混入exit总量；flight环按ordinal去重，跨层、丢项、不完整阶段不算精确耗时。当前脚本累计相加且只读首行，不能用作严谨时间火焰图。
8. TLB消费与源NPT同步用例：相同root/ASID改变frame或RW/NX、large拆分、L1/L2切换、重复L0内部重入、请求7、INVALID以及跨CPU接管。先保证映射与参考全失效路径一致，再优化失效范围。

### P1：先落地低风险的软件减负

`nested_fetch.c`：同一调用内一次翻译每个代码页，只读所需对齐qword，最后复核每条L1页表路径与NPT01。保持真实prefix/NRIP、跨页、页表变化和失败输出语义；不引入跨VMRUN的代码字节缓存。

`nested_session.c`：第一遍改用现成 `KswSvmNestedResolveOperand` 找HPA并获取owner；第二遍保持接管后的完整捕获、HPA一致性和INVALID处理。与transfer已实现策略一致。

`nested_permissions.c`：enabled图直接覆盖，不先clear；disabled图仍清零。合并改为对齐64位OR或经过实际编译验证的安全块拷贝；保持原图给route判定，位图修改仍每次可见。

`nested_commit.c`/`nested_writeback.c`：固定offset+mask表只遍历可写字段；对所有512个word与现有mask函数做等价对照，包括部分写回和保留位。固定表不等于允许全页memcpy。

`hvm_phys_window.c` 先保持原回调生命周期；如果计时证实 map/INVLPG主导，再增加显式CPU私有、同一次root事务的批量读scope，scope退出必须还原窗口。缓存同页映射只存在于该scope，不跨重入/guest执行，也不删除页表/RAM属性校验。

### P2：全XSTATE保存不必成为每个简单退出的固定税

新增独立MASM整数短路径，直接修改CPU私有数据/VMCB，不能调用一般C、memcpy、日志、Windows API或可能用SIMD的库。MSVC关闭自动vectorize本身不足以证明安全，必须检查实际OBJ指令与调用闭包。

第一批候选只限L1、无待处理事件/EXITINTINFO、无stop/power请求、无IRET/NMI窗口/特殊overlay、NRIP有效、CPL正确的 EFER/HSAVE读取和幂等XSS写。处理完成必须等价消费RF/interrupt-shadow、更新RIP/返回寄存器及必要telemetry。EFER改变模式/SVME、XSS/XCR0真正变化、L2-owned退出、异常/中断和所有不匹配状态回旧完整路径。

短路径从第一次触碰XSTATE前分岔，并在重新进入前绕开旧XRSTOR；guest XSTATE仍留在寄存器中。fallback先恢复现有的完整保存链，不能让旧保存区覆盖更新后的guest向量状态。先保留已有guest/host VMLOAD/VMSAVE和寄存器切换，逐步实测后再考虑删减其它固定操作。

若P0证明trace和seqlock占显著成本，将近期ring设成低频采样，终止/fault/lifecycle始终记录；单写者sequence改为架构正确的release/acquire发布，读者仍拒绝不一致。记录频率变化要公开，不能以少日志伪装少退出。

### P3：硬件 Virtual VMLOAD/VMSAVE

落点：`hvm_svm_vmcb.c`、`nested_general.c`、`nested_register.c`、`nested_entry.c`、`nested_session/transfer/state.c`及独立加速策略模块。

L0 VMCB启用Virtual VMLOAD/VMSAVE控制，按虚拟L1 EFER.SVME决定0x82/0x83拦截：虚拟SVME=0仍拦截并按架构拒绝，虚拟SVME=1且硬件/模式/源映射合同满足时才放行。guest operand由硬件NPT01翻译，绝不能裸清拦截使guest PA被当host PA。

审清原生VMSAVE绕过软件owner token后的缓存依赖、别名/跨CPU访问、guest FS/GS/TR/syscall状态持有者；L1/L2之间的VMLOAD状态不能被整页VMCB覆盖丢失。L2首步仍保留当前软件拦截/归属，不顺带公开第三层嵌套。

硬件用例须覆盖SVME0/1、CPL、地址尺寸/跨页和错误operand、current FS/GS/kernelGS/syscall改变、并发vCPU以及常驻stop/native恢复。老CPU/克隆走原软件路径。正确后，v8窗口中两类退出最多有24.12万次可消除，即L1次数的14.35%；这不是预报14.35%提速。

### P4：硬件 vGIF

将L1虚拟GIF绑定到VMCB的V_GIF/V_GIF_ENABLE，基于virtual SVME与pending事件动态控制STGI/CLGI拦截。Entry/Exit/Reflection/INVALID从正确VMCB来源读取或设置GIF；不能让硬件更新后仍使用旧 `Execution->Gif`。

原来依赖STGI退出触发的排队IRQ/NMI递送必须保留通知途径；必要时只在有GIF阻塞事件时重新拦截STGI。区分physical GIF、L1 vGIF、L2 vGIF以及NMI硬件屏蔽，不以开启bit代替状态机。验证GIF0→排队→STGI、IF/shadow/TPR、NMI/IRET、L2反射、重复stop/关机。无VNMI支持不顺带开启VNMI。

历史样本STGI/CLGI约26.26万次，属于另一个15.61%的L1频率机会。与P3合计约30%的L1次数可能减少；真实周期收益以P0计时和正常guest进展为准。

### P5：VMCB01/VMCB02分离及clean bits

两个稳定的4KiB硬件VMCB分别持有L1/L2执行状态，HSAVE/host镜像各有明确身份；自动VMRUN状态与VMLOAD状态分开。参考KVM将VMLOAD/VMSAVE状态集中到稳定镜像的设计，避免切层时来回复制整页。

所有软件写入路径维护dirty group（intercepts、MSRPM/IOPM、ASID、interrupt、NP、CR、DR、DT、segment、CR2、CET等实际支持的组）；不得复制L1给的clean位到物理VMCB。只在真实成功VMRUN后标记可复用，首次/新operand/迁移/恢复/INVALID强制dirty。中断和CR2初期保守保持dirty。因目前各模块直接写VMCB，该项在统一dirty写入入口和01/02切换后实施，不能先把汇编的CLEAN=0改成全1。

## 5. 多核验证和性能判断

用户回来后才做需要UAC的步骤。一次性确认实际LabHostReady、Hypervisor/VBS以及HSAVE准入；源码提交、签后SYS/PDB/CLI身份与运行代次必须绑定。

先1 vCPU验证正确启动/停止，再8 vCPU完整OS桌面。两者都需guest进展证据，不能把固件runtime、Tools单次心跳或NPF下降当桌面通过。基线使用同一关机快照、磁盘/内存/拓扑/启动路径；自动修复与正常boot分开记录。

每个候选至少采集boot早期、Windows logo、桌面稳定三个可比窗口：exit/useful-work、各类周期、hardware flush、NPT重用/回收、guest进展。另用有界微基准分别测EFER/HSAVE/XSS、VMLOAD/VMSAVE与XSETBV；非法/变化操作与合法幂等操作都测，不能仅测快速分支。

XSTATE专项覆盖SSE/AVX/AVX-512已启用部分、x87/MXCSR、guest XCR0/XSS切换与原生停止后的CET用户态；只测映射的组件，扩展状态硬件证明不可用模拟替代。GIF专项用真实timer/IRQ、NMI边界和停止进行验证。8核看每核工作线程持续进展、owner不残留和退出完整。

性能阶段目标是guest正常完成相同工作、root周期成本可重复下降；暂不承诺达到原生百分之几。达到稳定桌面后再固定一项CPU/内存/I/O工作量，以无KSword同配置为baseline；阶段门不依赖长时间压测。

## 6. 开源参考与采用边界

本轮下载固定版本的关键源码到本机临时参考目录，`sources.json` 保存版本和SHA256；不向仓库复制第三方实现。

- [Linux KVM nested.c，固定69f80fef](https://github.com/torvalds/linux/blob/69f80fef3153299d9c72c53d1d71eef6354b6926/arch/x86/kvm/svm/nested.c)：参考L0/L1合并归属、源MMU同步、01/02职责与硬件扩展条件。
- [Linux KVM svm.c](https://github.com/torvalds/linux/blob/69f80fef3153299d9c72c53d1d71eef6354b6926/arch/x86/kvm/svm/svm.c)：参考virtual SVME控制VLS/vGIF拦截、pending event重新评估以及clean发布。
- [Linux KVM vmenter.S](https://github.com/torvalds/linux/blob/69f80fef3153299d9c72c53d1d71eef6354b6926/arch/x86/kvm/svm/vmenter.S)、[x86.c](https://github.com/torvalds/linux/blob/69f80fef3153299d9c72c53d1d71eef6354b6926/arch/x86/kvm/x86.c)：参考VMLOAD状态镜像和FPU生命周期；Windows编译/异常环境需独立证明。
- [NoirVisor svm_exit.c，固定08dd5ec6](https://github.com/Zero-Tang/NoirVisor/blob/08dd5ec6096e68ca255cee4130b8f0dcb4c10ed2/src/svm_core/svm_exit.c)：可参考字段级VMLOAD搬运和dirty分类；其嵌套STGI/CLGI仍有GIF模拟FIXME，不能当完整中断实现依据。
- [devirtz-kernel](https://github.com/meowdiocre/devirtz-kernel)：用作辨识退出减少策略。其修改含native CPUID/PMU/MSR、宽松XSTATE检查；与本项目的guest能力合同和L1-owned退出不同，不再直接照搬。尤其不能撤销L1的设备/MSR拦截来提速。

推荐下一次编码从P0和P1开始；第一项需要周期证据才能决定的软件深度优化是P2，第一项真正减少SVM退出的硬件功能是P3。P4/P5随后独立验证。


## 2026-10-06 实施记录（静态验证）

P1 已提交 `8646bd5c`：单次取指复用两个页的源 walk 和对齐 word，读取后重验所有源路径；三字节样本物理读次数 189→63。VMRUN 首遍只解析身份，租约之后完整捕获；VMSAVE 从扫描 512 个 word 改为枚举 16 个可写 word，权限图只清零禁用侧。23 个宿主测试目标、驱动 Release x64 /WX 与 x64 WDK API/INF/CAT 通过，未执行硬件候选。

P0 已实现：虚拟 TLB_CONTROL 按 VMRUN 捕获一次，真实非 INVALID 返回才消费；合并 shadow publication 与虚拟 1/3/7 的完整性要求，未消费请求不会被进入准备清除。缓存记录至多 512 个源 PTE；虚拟 flush 时通过 NPT01 重新读源，仅容忍 Accessed 变化，frame/权限/PS/缓存/NX/Dirty 改变或账本溢出则失效。新增硬件 flush 计数及每 64 次退出一次的汇编周期采样，metrics v10/配套 CLI 输出五个 root 阶段和七个内部叶操作。软件事件协调的进入阶段独立计时；退出侧剩余成本包含事件、路由、诊断，不能把它全称事件成本。计时排除纯硬件转移和来宾执行。

分析器完整读取 JSON/JSONL/PowerShell UTF16，每个相邻同代次 CPU 窗口做差，检查偶数序列、饱和、身份、总数和阶段闭合；NPF 被纳入退出图，生命周期不再混入，flight ring 时间不再冒充 CPU 周期图。旧 v9 窗口可计次数，但不会生成伪造的周期图。

P0 证据：`tools/hvm_lab/build-tests.cmd` 23 目标通过（session 3366 项，含 8 host threads×100 模拟事务）；生产 JSON/PS5 CP936、85 命令目录和参数门通过；Python profile 5 项通过。`artifacts/amd-perf-20261006-profile/build.txt` 标准 64 位 WDK Build exit0、Universal、INF/CAT 无警告错误；SYS 未签名，尚未加载，不能宣称实测加速。GUI ABI 更新需后续一起构建，禁止旧 v9 GUI 查新 v10 metrics。


P2 的可切换候选已实现（2026-10-07）：`prepare-svm-fast/resident-svm-fast` 单独加入 FAST_MSR；其 L1 entry 必须无租约、队列、注入、NMI/IRQ/IRET 窗口、待 flush，且下一次退出保持 INT_CTL/物理 CR8。固定 GIF=0/1 的遮罩都保留，不省略需要重新协调的事件。EFER/HSAVE/XSS 读和同值写通过专用 scalar C 叶，变值写全部回完整桥；XSS 无 XSAVES 则回完整架构拒绝。叶不使用宿主 GS/TLS/回调、真实 SVM MSR、不访问 guest RAM；EFER 的硬件 LMA 实时取 VMCB，并在同值写时同步虚拟镜像。汇编只在合格 MSR 退出时调用它，跳过 XSTATE/mask/VMLOAD 配对；其间 guest VMLOAD 状态和所有向量组件保持 live。Hotspots/逐 CPU退出/机器计数精确累计，快路 ring 不逐次记录，FastSubset 不额外加到 exit total。

链接门直接反汇编完整 `hvm_svm_fast.obj`，闭集整数指令、仅本函数内直跳、无 call/向量/x87/FS/GS；Release 二进制 229 条指令通过。源码的 volatile scalar + compiler barriers 防止聚合复制和序列外重排，禁用该叶 LTCG；不依赖不受支持的编译器“禁 SIMD”选项。portable leaf/准入测试 208 项、全部24 C目标、分析器/机器码门6项通过；生产 JSON/PS5CP936、87 命令目录通过，WDK零警告/API/INF/CAT通过。尚无真实 XSTATE/事件/加速收益结果，不能据无SIMD代码证明完整硬件正确性。相同 SYS 可用 profile 与 fast 两种模式作阶段 A/B，不混 VLS/vGIF。


## 2026-10-07 首轮硬件数据与优先级更新

fast模式补测：两个真实QPC窗口10.9415527/10.9344453秒内，EFER/HSAVE快路读取分别190814/95359，32核持续ACTIVE；软件root样本VMRUN48.72%、NPF39.25%。用户仍看到Windows标志转圈，无完整桌面。漏采早期约109秒与A/B来宾阶段差异使这些数据不具备整体速度比较条件。结束后全32核原生、资源归零，退出主程序后SCM最终STOPPED，证据artifacts/amd-perf-20261007-fast-live。

实体机32CPU profile准入、自检、ACTIVE与周期计数有效性已真实通过；单核VM启动到Windows标志，有用户进展观察，完整桌面尚未通过。摘要与原始文件哈希见 `evidence/amd-root-profile-20261007.json`。软件root样本中的VMRUN约72.73%，权限图/源同步/取指分别32.31/30.04/14.52%；MSR3.56%。应先在实际profile数据下进一步削减页表重复翻译、权限图捕获/合并与源表同步的内存窗口开销；P2快路保留为独立比较，不宣称凭退出次数就能恢复原生性能。

NPF逐步增多，补采达到73k/95k每秒，没有terminal锁存；raw RIP/CR3/GPA仍变化，无法仅据计数断言固定NPF循环或正常启动成功。Root采样不包含硬件VMEXIT/VMRUN和VMware自身工作；周期性1/64可能相位偏采样。测试B按同签后SYS、同单核VM独立切fast；收尾必须确认32核全部原生且teardown成功。8核、XSTATE专项、VLS/vGIF与clean仍未完成动态验收。
