---
name: ksword-file-delete
description: R0 文件删除、POSIX 路径移除、共享检查高风险模式和 Win10 兼容边界
metadata:
  type: project
---

# R0 文件删除边界

- `DELETE_PATH` v2 在旧 v1 回执尾部追加打开、Disposition、关闭后复核状态；旧调用方给 v1 大小的输出缓冲仍收到 v1 前缀。
- POSIX 删除优先只请求 `DELETE | SYNCHRONIZE`，避免预先请求属性写入权限增加共享冲突。只读属性确认后才重开处理。
- 独立 IRP 删除后端使用通用引擎的 `BASE_FS` 手工 CREATE，只请求 `DELETE | SYNCHRONIZE`，配对 CLEANUP/CLOSE 返回 CREATE 接收者。删除优先 Ex/POSIX + 忽略只读，Ex 不支持时在同一对象上回退传统 `FileDispositionInformation`，v2 回执保留 CREATE 和最终删除阶段状态。
- 高风险忽略共享检查模式仅限单文件，独立安全策略位默认关闭，确认标记和策略位都必须满足。`IoCreateFileEx` 用 `MmGetSystemRoutineAddress` 运行时解析，避免旧 Win10 因新增静态导入无法加载驱动。该选项只跳过 I/O 管理器检查，文件系统仍可能拒绝。
- 不用原始磁盘/PCIe 写入修改已挂载卷的文件元数据；控制器后端已有离线磁盘约束。
- 删除回执只有关闭句柄后确认路径不存在才计为完成；同名路径被重建仍算可见，复核被拒绝则记未验证。不承诺立即释放仍被映像映射的数据。
- 删除后按路径复核与只读重试查询使用 `ZwQueryFullAttributesFile` + `FILE_NETWORK_OPEN_INFORMATION`。`ZwQueryAttributesFile` 缺少公共 WDK 声明，不能靠本机私有声明或禁用 `/WX` 维持 CI；完整属性查询仍只消费 `FileAttributes` 或查询状态，不改变关闭后复核和文件身份重验证边界。

## 2026-10-03 强制 IRP 删除

- 用户明确要求在强制删除中直接清空 `ImageSectionObject`、`DataSectionObject`、`SharedCacheMap`。实现集中在 `src/features/file/file_delete_irp.c`，只在 IRP 删除预设与已确认的忽略共享后端遇到 ACCESS_DENIED / SHARING_VIOLATION / CANNOT_DELETE / USER_MAPPED_FILE 后执行：先尝试 MM 清理，再直接清空三个共享成员并重试一轮。不释放不透明对象、不替换结构地址、不恢复旧指针。该结构为同一流的多个 FILE_OBJECT 共享，操作仍有实机映射/缓存一致性风险；离线回归和编译不能证明运行中映像删除安全。
- 忽略共享后端的 `open=0, disposition=0xC0000022` 不再只重复栈顶 Zw 请求；它引用原句柄的同一 FILE_OBJECT，借用通用引擎向 `IoGetBaseFileSystemDeviceObject` 直发 SET_INFORMATION，无路径重开、无额外 CLEANUP/CLOSE，原句柄由上层关闭后复核。基础层不可用时直接失败，不静默回退 RELATED。
- 强制预设使用内部 `KswordARKDriverSubmitFileDeleteIrp`；普通通用 IRP 提交继续执行调用方指定信息类，不启用直接 section 清空。未新增 IOCTL 或修改协议布局。
- `tests/FileDeleteIrpRegression.ps1` 使用真实生产删除代码和有界 kernel API 模拟，15 项回归覆盖 Ex/传统类兼容、三指针清空、同对象重试、取消不重复投递、引用配对、错误 IRQL 与失败状态保留。不得把离线通过当作驱动加载或活动映射实测通过。
