# 磁盘队列、I/O 日志与 MFT 索引

- 文件 → MFT 搜索与空间分布（`file.tab.ntfs_index`）由 `FileDock/NtfsSearchPage` 承载，使用 `ParameterWorkspace`；搜索与空间分布共用 `NtfsFastIndex` 不可变快照。查询/卷探测/汇总/排序在后台，表格使用共享 ResultTableHost，定位走 FileDock 原异步导航并显示隐藏/系统项。
- `NtfsIndexRecord` 是 Qt/Windows 无关的 FILE/USA/属性和 MFT 映射解析器；raw 记录严格验证 USA，FSCTL 返回格式有显式入口。默认 DATA 的逻辑/分配大小与实际卷已用空间不同；硬链接以完整文件引用去重，扩展名称按基础记录身份合并，无法计量的大小显式报告。
- 索引只读，不创建/删除 Journal。扫描前取 USN 游标，扫描后核对 MFT 布局并重放；日志身份/保留范围变化须重建，失败保留旧快照。活动卷不是 VSS 原子快照；不支持的 MFT 映射/属性列表明确失败，不能以部分结果冒充完整成功。
- 硬件 → 硬盘监控复用卷性能 lease，区分瞬时 QueueDepth、平均队列、IOPS 与进程文件 I/O；不可将多卷相加当物理磁盘队列。Kernel-File 日志采用私有会话，显示头 PID 和请求/完成阶段，不把请求字节当落盘字节。待消费 2048/显示 10000 条，覆盖/淘汰/ETW 丢失分开。
- 回归与 Windows 验收清单见 `docs/磁盘监控与MFT索引.md`。Linux 解析/数值/QtCore 检查和语法检查不等于 HostX64 MSVC 构建、原生 NTFS/ETW 或真实 GUI 验收。

- 磁盘编辑 GPT 名称：`PARTITION_INFORMATION_GPT::Name[36]` 是 NUL 填充的固定 UTF-16 字段；`fromWCharArray(Name,36).trimmed()` 会保留尾部 U+0000，导致地图和表格名称末尾出现方框。`gptPartitionName` 用有界 `std::find` 截断首个 NUL，无终止符时保留全部 36 单元；与 DiskStructureParser 原始 GPT 路径保持一致。不要用全局字体或 provider 编码猜测处理此问题。
