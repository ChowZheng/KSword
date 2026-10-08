# 内核池分配回放

- UI 入口是 `MemoryConsumerEvidencePage` 内的独立分析子页。采集保存成功后自动调用 `PoolAllocationAnalysisWidget::analyzeFile`；加载已有 ETL 不启动跟踪、驱动或时间线。结果与 PFN 分类账分开。
- 安装的 `MSNT_SystemTrace` MOF/TDH 确认：PoolTrace GUID `{0268a8b6-74fd-4302-9dd0-6e8f1795c0cf}`，v2 opcode 32/33 为普通/会话分配，34/35 为普通/会话释放。字段是 `Type`、`Tag`、`NumberOfBytes`（SizeT）、`Entry`（pointer）、可选 `SessionId`；不能按名称猜成 Kernel-Memory 或 PoolSize/PoolAddress。普通载荷长 `8+2P`，会话再加 4 字节，`P` 从事件头的位宽取值。
- 两遍原始 QPC 回放：先保留有界 StackWalk/映像历史，再配对分配。StackWalk 关联键来自**载荷** QPC 和 TID；头部时间戳/PID 不是替代键，同键冲突不能任选一条。压缩 StackKey 不在当前解码契约里，保持缺栈状态。
- 地址配对键包含 session；组键保留完整原始帧、历史 image IDs、tag 和池类型。重复分配不伪造释放。已知缺口隔离当时活跃实例；未知位置的丢失使用 `Analyzer::NoteUnknownGap`，整段配对归入不确定。计数为零的未知缺口也必须保留；缓冲区数量、丢失标记数量不能混作丢事件数量。
- 映像历史截断、乱序或未知位置丢失时不能继续把旧模块当真实来源，保留原始地址。模块是栈来源索引；单凭 tag 或栈中出现不能确认泄漏责任。文件读取完成、零丢失头计数也不证明循环缓冲覆盖的全区间完整。
- 当前只关联 PID=0 的历史内核映像与内核帧；未跟踪 Process 生命周期时，用户帧保持原地址，避免 PID 复用且缺少 ImageUnload 时误归属。真实 `OpenTrace`/`ProcessTrace` 集成使用自有 UUID 的 PRIVATE_IN_PROC 提供者写模拟事件；这证明离线 ETL 管线，不等于实际系统 Pool 采集验收。
- 本地符号解析验证 PE/RSDS 和本地 PDB，不自动使用嵌入的远程路径。所有 DbgHelp 用户共用 `ksword/dbghelp_serialization.h`：包括既有 ThreadStackWindow、DumpSymbolResolver、ArkRuntimeDynData 与注入栈解析，以及新 Pool reader；独立文件 mutex 无法串行化整个 DLL。
- Qt 6.9.3 与便携 Qt 6.11 的字体测量会不同。两枚长文本按钮可把 HBox 最小宽度撑到 514px；使用已有 FlowLayout 换行，不放宽 420px 几何断言。离屏截图要显式加载 CJK/等宽字体，否则功能断言过了仍可能全是方框。
- 检查入口：`tools/Invoke-PoolAllocationAnalysisTests.ps1`（实际 decoder + Qt-free 账本，CI 用 HostX64 MSVC）和 `tools/Invoke-PoolAllocationAnalysisUiTests.ps1`（真实 Qt model/view 及受控后台任务）。Codex sandbox 可能拒绝 `GetFinalPathNameByHandleW`，需要明确区分隔离环境访问失败与正常本地测试结果，不删除生产文件身份复核来让夹具通过。
