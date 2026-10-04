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

## System32 二次修复：Qt 的 UI 回填并非纯缓存操作

- 第一轮验证缺少真实可见视图的完整目录加载。约 5,186 项的 System32 在 `QFileSystemModel` 的 `MetaCall` 中实测阻塞 1,286 ms；关闭代理动态排序仍超过 1 秒，只替换图标也没有解决。
- Qt 6.9.3 的 `QFileSystemModelPrivate::fileSystemChanged` 在模型/UI 线程调用 `QFileInfoGatherer::getInfo`，其中逐项调用 provider 的 `icon` 和 `type`。默认 `type` 会初始化 MIME 数据库并按文件内容识别类型；即便改为 `MatchExtension`，冷初始化及整目录匹配仍实测阻塞约 844 ms。不能在这条回填链路中调用 MIME API，也不能同步访问 Windows Shell。
- `FastFileIconProvider` 在回填时只返回预取的通用图标和缓存 QFileInfo 的后缀；无后缀文件复用已有“文件”翻译。普通模式类型排序使用这一稳定的缓存后缀，详细类型显示在后台补齐，避免排序触发读取。
- `AsyncFilePresentationCache` 仅为视图实际请求的项查询原生小图标和原有 MIME 描述，每批最多 8 项，最多缓存 512 项；查询失败也缓存。工作线程做 COM/Shell/MIME 查询并返回 `QImage`，UI 线程才创建 `QPixmap/QIcon`。Shell 路径必须转成原生分隔符，否则临时目录测试中的图标提取会失败。
- 异步回填核对缓存代次、持久索引和完整路径，目录加载、模型重置、重命名时失效，关闭后丢弃回调。目录项立即可见，慢展示查询只保留占位值，不占用 UI 线程。
- `python tools/file_dock_latency_test.py --system32` 首先测试真实 System32 导航与可见 QTreeView，随后执行原有十万行回归。先完成空视图的首帧绘制，排除窗口初次启动成本；用 1 ms 定时器测量导航和 1.5 秒补全期间的最大 UI 间隔，门槛为 100 ms。修复后本机多次实测约 46–62 ms。另覆盖真实原生图标/MIME 描述、120 ms 慢查询期间的 UI 心跳、去重、失败缓存、重置和关闭后的旧结果。
- Qt 源码依据：[qfilesystemmodel.cpp](https://raw.githubusercontent.com/qt/qtbase/v6.9.3/src/gui/itemmodels/qfilesystemmodel.cpp)、[qfileinfogatherer.cpp](https://raw.githubusercontent.com/qt/qtbase/v6.9.3/src/gui/itemmodels/qfileinfogatherer.cpp)、[qabstractfileiconprovider.cpp](https://raw.githubusercontent.com/qt/qtbase/v6.9.3/src/gui/image/qabstractfileiconprovider.cpp)。
- 最终回归最大 UI 间隔 55 ms；十万行及慢展示查询用例通过。完整 x64 Release 构建 `BUILD_RESULT=SUCCESS / EXIT_CODE=0`，产物 20,662,680 字节（日志 `.codex-build-logs/ksword-build-check-20261004-150149.raw.log`）。首次链接遇到当前程序占用（LNK1104），将旧 exe 改为同目录备份名后重新链接，保留运行实例；新版本需重启使用。测试签名写入成功但本机信任验证仍报 `0x80096019`，不等于签名信任校验通过。

## 同目录刷新保留视口

- 滚动恢复属于数据刷新行为，不能放入全局 QSS/主题样式。`FileRefreshScrollState` 仅由 `FileDock::refreshPanel` 为当前可见的详情/树或列表/图标视图创建；其他页面需要时再抽公共行为组件并显式接入。
- 在重建模型之前保存可见文件的完整路径、行内像素偏移及横向/纵向滚动值；Windows API 等匹配目录的 `directoryLoaded`，手动模式等快照 `modelReset`，排队到过滤/排序完成后恢复。优先保持同一文件的视口位置，文件被删/筛掉时将原滚动值夹到新范围。
- 连续刷新复用尚未恢复的状态，不能把临时空模型的表头位置覆盖进去。目录、视图模式、过滤、排序或读取方式改变，以及用户主动操作视图/滚动条后，丢弃旧恢复任务。
- 路径查找只遍历已枚举模型缓存，不调用 `QFileSystemModel::index(path)` 做同步查盘；只读持久索引在布局变动中追踪文件。`QListView::Batched` 等目标项分批布局可用后再恢复，禁止 `doItemsLayout()` 强制一次布局整个大目录；隐藏面板等 `Show`。
- 回归覆盖前方插入条目后的文件/像素偏移、横向位置、删除锚点、行数缩减、连续刷新、跨目录和用户操作取消、5,000 项列表/图标分批布局；真实 System32 重建 `QFileSystemModel` 后仍保持原位置。加载与刷新测得最大 UI 间隔 44 ms（100 ms 门槛）。
- 模型/视图在恢复回调前销毁也已回归。完整主程序 x64 Release 构建成功（`BUILD_RESULT=SUCCESS`、`EXIT_CODE=0`，日志 `.codex-build-logs/ksword-build-check-20261004-152940.raw.log`），产物 20,669,848 字节；仍保留运行中的旧实例，需重启使用新版。
