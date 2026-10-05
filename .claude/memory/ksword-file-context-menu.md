# FileDock 右键菜单与特殊权限启动

- `FileDock.cpp::showPanelContextMenu` 完成动作可用性设置后，使用
  `hideUnavailableFileMenuActions` 递归隐藏禁用动作及空子菜单；Qt 的
  `setSeparatorsCollapsible(true)` 折叠首尾/连续分隔线。插件只有禁用占位项时也隐藏。
- “扫描映射进程(R0)”与“文件解锁器”相邻，属性为最后一个动作。
- “以特殊权限运行”只对单选 `.exe` 显示，提供 System、TrustedInstaller、管理员、
  普通用户；System/TI 需要当前已提升，管理员可通过 Windows UAC 启动。
- 独立 R3 后端为 `ksword/process/process_run_as.*`，不使用驱动令牌写入。
  SYSTEM 源为当前 Session 的 winlogon，并验证 LocalSystem SID；TI 源 PID 来自 SCM，
  验证 LocalSystem 用户和启用的 TrustedInstaller 服务 SID。服务启动等待最多 15 秒，
  整个启动任务在 Qt 线程池中执行。
- 调权只作用于临时线程令牌；两层上下文作用域分别恢复 SYSTEM 模拟及原有线程令牌。
  普通用户优先使用当前未提升令牌、未提升的 linked token 或交互式 Shell 令牌，
  必须验证未提升及 Medium 或更低完整性；不能失败后继承管理员/SYSTEM。
- 令牌启动重建目标用户环境块并保留当前交互式 Session，使用明确的映像路径、
  带引号的命令行及映像所在工作目录；CreateProcess 回退前重新生成可变命令行。
  当前已经是 SYSTEM 时，“管理员”取交互式用户的已提升 linked token，不能继承 SYSTEM。
- `python tools/file_context_menu_test.py` 提取真实菜单构造和递归过滤代码做 Qt offscreen
  检查，并链接真实后端启动无副作用身份探针，覆盖普通用户、嵌套普通用户启动、
  无提升的 SYSTEM/TI 拒绝、原线程模拟恢复。选区、Oplock、插件与权限可用性用替身。
  `--privileged` 在已提升宿主中额外启动管理员/System/TI 探针，TI 可启动系统服务。
- 2026-10-04：普通用户 Medium/未提升身份、交互式 Session、菜单及恢复检查通过；
  主程序 x64 Release 构建及双语审计通过。当前执行宿主未提升，管理员/System/TI
  成功启动的实机探针未执行，不能把它们描述成已验收。
