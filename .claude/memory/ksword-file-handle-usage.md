# 文件占用扫描与解锁

- 入口为 `FileDock.cpp` 的 `FileDetailDialog::buildUsageTab`，后端为 `ksword/file/file_handle_tools.cpp`，Qt 桥接为 `FileHandleUsageScanner.*`。旧 `unlockPathsByDriver` 已直接转向统一属性页，后面的旧选择窗口代码不可达。
- “关闭句柄（R3）”要求非自身、非关键系统进程、有效 PID/进程创建时间与非零 Handle。`R3 ProcessImage` / `R3 ModuleSnapshot` 是映射占用证据，Handle=0；映像 section 可以在原始文件句柄已关闭后继续持有文件。R0 关闭文件句柄也不能自动解除这些映射。2026-10-05 用户截图仅一个真实句柄属于 Ksword 自身，其余为模块行，所有行均无法执行关闭是符合现有保护条件的结果。禁用原因现在通过按钮与 Handle 列提示展示。
- 文件解锁器任务中用户在 2026-10-05 最初要求先不加 R0 关闭；随后另行授权句柄页增加 R3/R0 关闭，又在本聊天授权文件占用页接入已有 IOCTL。现在 `buildUsageTab` 在 R3 按钮旁增加“关闭句柄（R0）”，通过 `DriverClient::closeHandle`，不新增协议或修改驱动。底层实现与限制见 [句柄页 R3/R0 关闭](ksword-handle-close.md)。
- `HandleUsageEntry` 后端与 Qt 桥接保存扫描时的 `objectAddress`；R0 行同时保留枚举提供的 `processCreationTime100ns`，只有缺失时才用 R3 创建时间兜底，不能把 R0-only 身份覆盖成 0。R3 行保留系统快照中的对象地址，未提权/地址缺失时 R0 按钮禁用并提示重扫。模块合成行没有对象/句柄，仍不可关闭。
- R0 按钮要求可操作进程、非零创建时间/对象地址以及小于 `0x80000000` 的非零用户句柄。确认框前复制完整值快照；确认后 `QtConcurrent` 后台执行关闭，不捕获 UI 或行指针。操作期间禁用刷新、列表及所有动作，并用 `m_usageCloseInProgress` 拒绝事件重入。完成后无论成功失败都刷新；关闭/恢复未完整成功时显示 DriverClient 两阶段状态，不回退 R3。关闭属性窗使 watcher 回调失效，已发出的驱动操作仍由其自身收尾。
- 无驱动时回落 R3。旧实现设进度 35% 后串行对全部 File 句柄运行 `GetFileType`、DOS normalized path 查询及 `NtQueryObject` 兜底，整个循环没有进度更新/等待上限，取消也无法打断当前系统调用。
- R3 扫描现用 opened NT path (`GetFinalPathNameByHandleW(FILE_NAME_OPENED | VOLUME_NAME_NT)`)，与目标已有 DOS/NT 两套规则比较；避免每个系统文件/设备句柄执行卷名转换、normalized path 查找及可能阻塞的 `NtQueryObject` 兜底。仍是路径匹配，不能宣称覆盖所有硬链接/重解析别名。
- `file_handle_query_worker.h` 用可复用专用线程，250 ms 等待上限，20 ms 取消检查；线程独占复制的本地句柄直到查询与 CloseHandle 结束。超时只请求 `CancelSynchronousIo`，不强杀线程；驱动可忽略取消，所以最多保留 4 个活跃查询线程（跨扫描计数），每轮最多 4 次超时，然后继续映像/模块检查。迟到线程仅持有自己的共享状态和纯查询函数，不持有扫描/UI 回调。等待上限不等于底层 I/O 一定已经取消，也不覆盖整个快照/进程/模块扫描的所有系统调用。
- 循环最多每 100 ms 更新 35–89% 进度；R3 `fileLikeHandleCount` 改为快照 File 类型数量，原来误用命中数量。任何路径超时或线程容量耗尽都会设置 `fileHandleScanIncomplete`，Qt 桥接保留该位，页面明确显示扫描不完整；不能把部分结果当完整无占用结论。
- `tools/Invoke-FileHandleQueryTests.cmd` 编译真实 worker 回归、既有 R0 fallback harness，并运行独立子进程持有临时文件的真实 R3 扫描。覆盖线程复用、超时、取消、跨扫描容量、迟到资源释放、R0 空结果不回落及 File 类型过滤；真实 R3 测试仅验证能找到测试句柄，不执行远程关闭/终止。不能替代完整 UI 点击或所有文件系统实测。
- 文件占用页 R0 接入后，既有 HandleClose 客户端/内核业务 fixture/独立子进程 R3 关闭回归通过；扫描回归新增精确检查 R0 创建时间及对象地址未被覆盖。2026-10-05 主程序 Release/x64 完整构建返回 `BUILD_RESULT=SUCCESS`、`EXIT_CODE=0`，新 exe 为 20741632 字节，语言审计通过；实际新驱动加载与文件占用页 R0 点击关闭仍未实测。构建中存在无关 `OtherDock/WindowInputClient.cpp` 两项既有宏重定义警告，不能称整仓零告警。
