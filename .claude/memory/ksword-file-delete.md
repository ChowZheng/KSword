---
name: ksword-file-delete
description: R0 文件删除、POSIX 路径移除、共享检查高风险模式和 Win10 兼容边界
metadata:
  type: project
---

# R0 文件删除边界

- `DELETE_PATH` v2 在旧 v1 回执尾部追加打开、Disposition、关闭后复核状态；旧调用方给 v1 大小的输出缓冲仍收到 v1 前缀。
- POSIX 删除优先只请求 `DELETE | SYNCHRONIZE`，避免预先请求属性写入权限增加共享冲突。只读属性确认后才重开处理。
- 独立 IRP 删除预设必须托管打开（`ZwCreateFile`），只请求 `DELETE | SYNCHRONIZE`，再向 `BASE_FS` 发送删除 IRP；释放对象引用后由 `ZwClose` 配对收尾。裸 `FILE_OBJECT` 手工 CREATE 即使返回成功，NTFS 的后续 SET_INFORMATION 仍可能因打开类型/关联不完整返回 `STATUS_INVALID_PARAMETER`，不能在普通删除预设中复用。通用 IRP 构造器仍保留显式手工 CREATE。删除优先 Ex/POSIX + 忽略只读，Ex 不支持时在同一对象上回退传统类；v2 回执保留打开和最终删除阶段状态。此模式只绕过删除阶段过滤，打开和关闭走正常文件系统栈。
- 高风险忽略共享检查模式仅限单文件，保留请求确认标记；`DELETE_PATH` 的普通删除和忽略共享检查模式均不再受 KSword Safety Policy 拦截。旧策略位仅为协议兼容保留，设置它们不会改变删除行为。通用 IRP 提交的删除信息类也不走文件写入策略位，但保留 IRP 引擎原有确认令牌。`IoCreateFileEx` 用 `MmGetSystemRoutineAddress` 运行时解析，避免旧 Win10 因新增静态导入无法加载驱动。该选项只跳过 I/O 管理器检查，文件系统仍可能拒绝。
- 不用原始磁盘/PCIe 写入修改已挂载卷的文件元数据；控制器后端已有离线磁盘约束。
- 删除回执只有关闭句柄后确认路径不存在才计为完成；同名路径被重建仍算可见，复核被拒绝则记未验证。不承诺立即释放仍被映像映射的数据。
- 删除后按路径复核与只读重试查询使用 `ZwQueryFullAttributesFile` + `FILE_NETWORK_OPEN_INFORMATION`。`ZwQueryAttributesFile` 缺少公共 WDK 声明，不能靠本机私有声明或禁用 `/WX` 维持 CI；完整属性查询仍只消费 `FileAttributes` 或查询状态，不改变关闭后复核和文件身份重验证边界。

## 2026-10-03 强制 IRP 删除

- 用户明确要求在强制删除中直接清空 `ImageSectionObject`、`DataSectionObject`、`SharedCacheMap`。实现集中在 `src/features/file/file_delete_irp.c`，只在 IRP 删除预设与已确认的忽略共享后端遇到 ACCESS_DENIED / SHARING_VIOLATION / CANNOT_DELETE / USER_MAPPED_FILE 后执行：先尝试 MM 清理，再直接清空三个共享成员并重试一轮。不释放不透明对象、不替换结构地址、不恢复旧指针。该结构为同一流的多个 FILE_OBJECT 共享，操作仍有实机映射/缓存一致性风险；离线回归和编译不能证明运行中映像删除安全。
- 忽略共享后端的 `open=0, disposition=0xC0000022` 不再只重复栈顶 Zw 请求；它引用原句柄的同一 FILE_OBJECT，借用通用引擎向 `IoGetBaseFileSystemDeviceObject` 直发 SET_INFORMATION，无路径重开、无额外 CLEANUP/CLOSE，原句柄由上层关闭后复核。基础层不可用时直接失败，不静默回退 RELATED。
- 强制预设使用内部 `KswordARKDriverSubmitFileDeleteIrp`；普通通用 IRP 提交继续执行调用方指定信息类，不启用直接 section 清空。未新增 IOCTL 或修改协议布局。
- `KswordARKDriver/tests/FileDeleteIrpRegression.ps1` 使用真实生产删除代码和有界 kernel API 模拟，22 项回归覆盖 Ex/传统类兼容、三指针清空、同对象重试、取消不重复投递、引用配对、错误 IRQL、失败状态保留，以及原样提取的生产打开选择/提交函数：内部预设托管打开、目录/重解析点选项、失败收尾和通用构造器兼容。CREATE 和底层 IRP 仍由模拟替代；不得把离线通过当作驱动加载或活动映射实测通过。
- 2026-10-03 普通文件 IRP 删除报告 `open=0, disposition=0xC000000D, verify=0xC00000BB`：删除失败时未进入关闭后路径复核，`verify` 是初始未执行状态，不能误诊成验证 API 不支持。托管打开修复后的 22 项回归通过，替换为 HEAD 的旧打开/提交函数时新增用例中 4 项失败。x64 Release 在 `/W4 /WX` 下编译/链接、x64 ApiValidator、Inf2Cat 通过；自动 CSignTool 签名不返回，核验具体进程后停止本次构建/签名，用 `/p:KswordArkSkipAutoVariantSign=true` 完整 Build 返回 0。产物未签名，未加载驱动、未实测删除。

## FileDock 失败反馈延迟

- 驱动回执已经返回后，原 `appendDriverDeleteFailureDetail` 仍会同步扫描全系统文件句柄、进程映像与加载模块，再把错误交回 UI；批量失败会重复扫描。此扫描不参与删除，是十几秒额外等待的可疑来源，已从删除失败路径移除，保留现有文件解锁器供用户按需诊断。
- 失败结果先弹窗，关闭弹窗后再刷新文件面板，避免模型重建与导航阻塞报错。模态弹窗返回后重新校验 QPointer，防止嵌套事件循环中 Dock 被销毁。
- 每次顶层 R0 删除调用返回即记录 `ioElapsedMs`，覆盖 DriverClient 与 IOCTL 往返；它不含额外占用扫描或 UI 面板刷新。失败时该日志使用 Warn，成功使用 Debug。不能把这个总时间误称为某个内核 IRP 阶段耗时。
- MainWindow 的 R0 日志消费者成功读到数据后连续排空；120 ms 仅为空闲轮询间隔，不能仅凭该常量解释 10–20 秒延迟。
