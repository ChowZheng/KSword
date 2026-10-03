# 统一内存编辑器与汇编

- `UI/MemoryEditorWidget` 是后端无关的快照编辑器，主程序的普通/R0/R-1/DDMA 内存、影子页、Hook 补丁、内核证据与转储内存入口共用。内部 Hex/反汇编/文本/对比视图使用同一份字节缓存；控件不调用任何内存写入接口。用户修改后须通过所属页面显式应用（影子页/Hook 由安装动作提交）。完整组件核查与保留边界见 `docs/内存编辑器组件清单.md`；独立 ARKLight Win32 客户端仍使用自身组件，不声称全仓已移除旧 Hex 工具。
- `MemoryEditorWidget::hexEditor()` 仅用于保留书签、断点等既有 Hex 集成。程序直接调用 Hex 的 `setByteArray` / `setByteAtAbsoluteAddress` 后，须调用 `refreshFromHexEditor()`；新读取应使用 `setSnapshot` 建立基线。成功写回必须以实际回读数据创建新快照，不能把暂存字节直接当作已成功写入。
- `MemoryEditHistory.Core` 只存 before/after 差异块，限制 32 MiB/256 步；一次 `refreshFromHexEditor` 聚合为一步（汇编、模板、粘贴与字符串修改）。Ctrl+Z 撤销，Ctrl+Y/Ctrl+Shift+Z 重做；新编辑切断 redo 分支。新快照、实际写回后接受及放弃修改均清空历史，不能借历史撤销真实写入。单次操作超出容量保留当前缓存但清除不可跨越的历史；回放前核对相关字节，失配拒绝整步回放。
- `setSnapshot` 的末尾 `sourceIdentity` 必须涵盖目标/后端/会话代次等身份。只有身份、基址和长度均一致才保留上一读取；物理地址、CR3 和 PID 不可跨目标混比较。Hex/反汇编/文本的橙色表示当前缓存相对读取基线的待提交修改，青色表示两次实际读取的变化；选区/搜索高亮优先。`HexEditorWidget::setChangeReferences` 为统一的 palette-aware 高亮入口，保留旧值 tooltip。
- 对比页可选读取基线或上次读取，默认仅列差异，每页最多 256 行；并列基线/当前 Hex 和文本，统计变化字节，双击定位，右键可复制两侧字节。文本视图从当前选中行开始，64 KiB 预算避免大快照生成无限格式对象。KernelDisassemblyDialog 的菜单与两次模态确认绑定捕获证据代次和架构，不在菜单结束时重取新行。
- 反汇编按明确的起点解码，默认对应请求地址，避免从前置读取缓存或指令中间开始；64 KiB/4096 指令预算内展示。可手选 x86/x64，物理地址和 CR3 不能自动判定代码位数。指令行保持地址顺序，关闭全局一次性表头排序；上下文菜单在模态循环前保存地址与文本，快照改变后拒绝编辑旧行。
- `MemoryAssembly.Core` 使用仓库现有 Zydis 4.1.1 编码器；Qt 包装为 `InstructionAssembler`。支持常用 Intel 指令、多行、局部标签、寄存器/索引/位移、显式内存大小、实际地址分支与 RIP 相对寻址。裸数字默认十六进制；`0x`/`h` 为十六进制，`0d` 为十进制。它不实现 CE Auto Assembler 的 alloc/registersymbol 等脚本指令或外部模块符号解析；不支持的语法明确拒绝，失败不返回部分机器码。
- 汇编编辑对话框先编译和预览旧/新字节；变长补丁须由用户明确扩大覆盖长度；必须覆盖完整且可解码的旧指令，剩余空间可用 NOP 填充。关闭填充时新字节长度须与覆盖长度相等。填入缓存前复核快照代次、基址、架构与内容。
- Tab4 写回绑定附加进程代次与读取后端；DDMA 在 Tab4 保持只读。Tab6 保留标准 R0 内核事务，其他后端使用 `MemoryAccessBackend`，写回绑定快照后端及 DDMA 会话代次。最终回读逐个比对授权差异块，任一失败不报告整体成功；未知回读清空编辑快照。
- R-1 页写回绑定成功读取的物理/虚拟模式、地址和 CR3；待应用修改期间锁定请求参数。IOCTL 仍在后台运行，写权限与原有危险动作确认保留。所有新线程用的翻译模板在 UI 线程先解析。关闭窗口取消后续步骤，`QPointer` 和操作序号抑制陈旧回调；接口不提供原子的比较后写入，不能宣称防止所有并发修改。
- KvmViewDialog 加载完整目标页后暂存影子修改，显式安装前重查映射及整页原字节；KvmHookWizard 使用 VA 作为显示/汇编地址，PA 仅用于 IOCTL 和计划，模板与整页导入不重建快照。DDMA 独立页绑定全局会话代次，确认框后复核上下文、写前核对原字节；成功/失败/partial 均以真实回读更新显示，不完整回读清空禁用。
- DumpMemoryView 与 TRIAGE 中带 VA 的 raw 块为只读共享组件，保留已捕获范围、文件身份和预览限额；Secondary Dump Data 等无 VA 的文件辅助数据继续作为文件偏移报告。KvmWatchPanel 采样、DMA 操作备份日志等仍为证据报告；磁盘/文件/网络工具不属于内存编辑器迁移。
- 汇编核心独立回归入口：`tools/Invoke-MemoryAssemblyTests.ps1`（gcc/g++ + 仓库 Zydis，无 Qt）。覆盖实际编码/解码往返、x86 高地址、x64 RIP、标签、错误输入和无部分输出。2026-10-02 本机核心回归通过；指定 Qt/MSBuild 缺失，主程序编译、真实 GUI 和 R-1 驱动写入未验证。
- 历史回归 `tools/Invoke-MemoryEditHistoryTests.ps1` 420 项通过；汇编核心 217 项通过。`tools/hvm_unit_tests/test_ddma_editor.py` 用生产读写方法与 facade 类型、替代后端/Qt shell 检查 13 个提交/冲突/回读/会话/取消分支，不能代替 Qt GUI 或硬件实写。2026-10-02 主程序构建检查仍因指定 Qt 路径不存在停止。
