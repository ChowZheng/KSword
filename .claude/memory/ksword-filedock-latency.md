# FileDock 大目录响应性

## 2026-10-04 修复边界

- `FileDock.cpp` 的 `ManualDirectoryModel` 直接持有不可变枚举快照，以 `QAbstractTableModel` 按需生成单元格。R3 原始卷、纯 MFT、R0 Zw 与 R0 IRP 共用发布路径；不再在 UI 中为每行创建六个 `QStandardItem` 或查询前 512 项的重解析点。旧快照和疑似隐藏项集合在后台释放。
- `ExplorerFileSortProxyModel` 使用 `QFileSystemModel::isDir/fileName/size/type/lastModified` 的缓存，不能重新创建 `QFileInfo(path)` 查盘。名称自然排序复用 `QCollator`，手动模式的大小/时间按原始数值排序，类型比较不经过异步显示数据。
- 两种模型共用 `AsyncReparseMarkerCache`，请求显示时后台每批至多查询 32 项；正、负结果都缓存。UI 回填校验缓存代次、持久索引和路径，模型重置、重命名或销毁后丢弃旧结果。工作线程通过应用对象排队，只在 UI 线程解引用 `QPointer`。
- 文件详情树使用统一行高；图标/列表使用 `QListView::Batched`，每批 128 项。禁止重新引入依赖全部行的尺寸计算。
- `selectedPaths` 用 `QSet` 去重并保留选区顺序。状态栏大小来自模型缓存；磁盘空间和单项属性在后台查询，每个面板最多一个任务，以选区/路径代次合并更新并拒绝旧结果。
- 枚举结果在初次回调和右键菜单延迟提交处均核对路径、读取方式和请求代次。已离开的目录不能覆盖当前模型。

## 验证

- `python tools/file_dock_latency_test.py` 从生产源码提取实际 Qt 模型、排序器、选区收集和状态更新；只替换主题/i18n 与重解析查询，使用 MSVC x64 和 Qt offscreen，不依赖主程序链接。
- 回归覆盖十万行发布、自然/大小/时间排序、目录优先、疑似隐藏标记、名称过滤、十万项选区收集与大小统计、选区变化后的旧状态抑制、慢查询期间 UI 定时器、正/负缓存、重置/关闭后的旧结果，以及真实临时目录的 QFileSystemModel 缓存路径。
- 生产模型回归不等于 R0 驱动实机枚举验收；超大真实目录、慢网络路径仍需运行观察。
- 本轮完整主程序编译还发现 `MemoryDock.DriverMemoryView.cpp` 的文本导出引用已移除的行宽和 ASCII 辅助函数；就地恢复 16 字节行宽与不可打印字节替换，保留导出格式。
