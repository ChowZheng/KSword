# PDB 特征码回退增量适配

本轮扩展缺少精确 PDB profile 时的只读运行时解析。已有 PDB/System Informer 字段仍由原有合并规则优先使用；回退结果继续标记为 RuntimePattern 或 PrivatePatternScan，不能提升为 PDB 信任或据此放宽注销权限。

## 覆盖范围

| 路径 | 本轮适配 | 证据和拒绝条件 |
| --- | --- | --- |
| 进程、线程导出访问器 | 20 处既有访问器入口支持 RCX disp8/disp32 的指针、ULONG、USHORT、UCHAR 读取与 LEA；识别明确 NOP/CET 前缀、RET/REP RET 和最多两级 EB/E9 入口跳转 | 只接受完整返回语义；逐字节安全读取，跳转仍限于原 PE 的可执行节；字段读取必须与当前真实导出的返回值一致 |
| 进程、线程、映像通知数组 | 从各自公开注册/移除导出沿 PE 函数边界内的调用图定位全局，替换首个短特征匹配 | 整个 64 槽容器处于可写数据节；完整验证 EX_FAST_REF、活动回调块及已加载模块可执行节，活跃自注册时精确匹配本家族回调；二次快照不一致、空容器或多候选均不发布 |
| 注册表回调链 | 从 CmRegisterCallback/CmRegisterCallbackEx/CmUnRegisterCallback 定位链头 | 先通过当前自注册 Cookie/context/function 校准前缀；逐跳安全读、双向互指、预算内完整闭环、两遍身份一致；无活跃校准或有歧义时不可用 |
| 对象类型表、工作队列、特殊回调、Shadow SSDT、CI 缓存 | 原有回退改用 PE 异常目录确认的函数范围，避免固定窗口包含相邻例程 | 保留各模块既有结构、唯一性、来源和能力检查；未知叶函数不凭固定窗口继续扫描，明确 EB/E9 跳板可以进入后续有函数表的实现 |
| CI 缓存候选选择 | 修复同分 A/B 冲突被后续重复 A 清除的问题 | 同一地址的重复引用保留已有歧义；只有严格更强的链/锁关联证据能解除弱证据冲突；资源身份和锁下复核保留 |

新增生产模块为 `KswordARKDriver/src/platform/process_accessor_decode.c/.h` 与 `KswordARKDriver/src/features/callback/callback_global_fallback.c/.h`，已同步驱动工程和 filters。

没有为 EpSession 的二级访问器、未知私有布局或无活动证据的空容器填入推测偏移。OS build 上限仍按既有用户决定停用，逐功能 IRQL、范围、可读性和结构检查继续执行。

## 离线验证

在仓库根目录运行以下脚本，使用 x64 MSVC `/W4 /WX`，产物写入已存在的 `output/`。这些脚本不装载驱动，不打开设备，不使用 PDB/profile 指定待发现的地址。

```powershell
& KswordARKDriver/tests/ProcessAccessorDecodeRegression.ps1
& KswordARKDriver/tests/RuntimeSignatureRegression.ps1
& KswordARKDriver/tests/CiHashSelectionRegression.ps1
& tools/CallbackGlobalFallbackRegression.ps1
```

访问器回归编译真实生产 decoder；共用扫描器与 CI 候选选择回归原样提取生产函数体。回调全局回归编译真实生产回退模块，以有界合成内存和受控模块/注册 oracle 验证候选判据。

离线检查覆盖代码截断、短读、错误寄存器、返回值变换、跳转溢出/循环/预算、函数边界、邻接诱饵、重定位、容器损坏、并发变化以及候选歧义。它们不等于其它 Windows 版本兼容或实际驱动查询验收；实机仍需在正常换版后分别检查有 PDB 与无 PDB 的枚举、来源和 unavailable/partial 状态。

## 本轮验证结果

- 访问器 228 项、共用扫描器 21 项、CI 选择 18 项、回调全局 69 项，共 336 项离线检查通过，均在 `/W4 /WX` 下编译。
- Release/x64 标准 MSVC/WDK Build 退出码为 0，产物为统一 Release 目录的 `KswordARK.sys`；编译与链接通过。
- ApiValidator 报告 `Driver is 'Universal'.`；Inf2Cat 的 Errors/Warnings 均为 None，Catalog generation complete。
- IOCTL 计划门禁通过：211 个注册项，147 个覆盖、64 个排除；本轮没有增删 IOCTL。
- 本轮构建使用 `KswordArkSkipAutoVariantSign=true`，没有执行变体签名、驱动安装、装载或设备 IOCTL 联调。

构建日志保留在 `output/pdb-signature-driver-build-20261005-fixed.log`。首次构建发现用户态标准类型头与内核 CRT 冲突，随后改为 `_KERNEL_MODE` 下的 WDK 自有类型分支；没有通过关闭警告绕过编译检查。
